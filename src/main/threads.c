/*
 * threads.c	request threading support
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2000  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")
USES_APPLE_DEPRECATED_API	/* OpenSSL API has been deprecated by Apple */

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/process.h>

#ifdef HAVE_STDATOMIC_H
#include <freeradius-devel/atomic_queue.h>
#endif

#include <freeradius-devel/rad_assert.h>

/*
 *	Other OS's have sem_init, OS X doesn't.
 */
#ifdef HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif

#ifdef __APPLE__
#ifdef WITH_GCD
#include <dispatch/dispatch.h>
#endif
#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/semaphore.h>

#ifndef WITH_GCD
#undef sem_t
#define sem_t semaphore_t
#undef sem_init
#define sem_init(s,p,c) semaphore_create(mach_task_self(),s,SYNC_POLICY_FIFO,c)
#undef sem_wait
#define sem_wait(s) semaphore_wait(*s)
#undef sem_post
#define sem_post(s) semaphore_signal(*s)
#endif	/* WITH_GCD */
#endif	/* __APPLE__ */

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_PTHREAD_H

#ifdef HAVE_OPENSSL_CRYPTO_H
#include <openssl/crypto.h>
#endif
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#ifdef HAVE_OPENSSL_EVP_H
#include <openssl/evp.h>
#endif

#ifndef WITH_GCD
#define SEMAPHORE_LOCKED	(0)

#define THREAD_RUNNING		(1)
#define THREAD_CANCELLED	(2)
#define THREAD_EXITED		(3)

#define NUM_FIFOS	       RAD_LISTEN_MAX

#ifndef HAVE_STDALIGN_H
#undef HAVE_STDATOMIC_H
#endif

#ifdef HAVE_STDATOMIC_H
#define CAS_INCR(_x) do { uint32_t num; \
			  num = load(_x); \
			  if (cas_incr(_x, num)) break; \
                     } while (true)

#define CAS_DECR(_x) do { uint32_t num; \
			  num = load(_x); \
			  if (cas_decr(_x, num)) break; \
                     } while (true)
#endif

/*
 *  A data structure which contains the information about
 *  the current thread.
 */
typedef struct THREAD_HANDLE {
	struct THREAD_HANDLE	*prev;		//!< Previous thread handle (in the linked list).
	struct THREAD_HANDLE	*next;		//!< Next thread handle (int the linked list).
	pthread_t		pthread_id;	//!< pthread_id.
	int			thread_num;	//!< Server thread number, 1...number of threads.
	int			status;		//!< Is the thread running or exited?
	unsigned int		request_count;	//!< The number of requests that this thread has handled.
	time_t			timestamp;	//!< When the thread started executing.
	REQUEST			*request;
} THREAD_HANDLE;

#endif	/* WITH_GCD */

#ifdef WNOHANG
typedef struct thread_fork_t {
	pid_t		pid;
	int		status;
	int		exited;
} thread_fork_t;
#endif


#ifdef WITH_STATS
typedef struct fr_pps_t {
	uint32_t	pps_old;
	uint32_t	pps_now;
	uint32_t	pps;
	time_t		time_old;
} fr_pps_t;
#endif

/*
 *	In process.c, but no one else should be calling these
 *	functions.
 */
extern void request_free(REQUEST *request);
extern void request_done(REQUEST *request, int original);

#ifndef HAVE_STDATOMIC_H
static int request_fifo_discard(int priority, bool lower, time_t now);
#endif

/*
 *	A data structure to manage the thread pool.  There's no real
 *	need for a data structure, but it makes things conceptually
 *	easier.
 */
typedef struct THREAD_POOL {
#ifndef WITH_GCD
	THREAD_HANDLE	*head;
	THREAD_HANDLE	*tail;

	uint32_t	total_threads;

	uint32_t	max_thread_num;
	uint32_t	start_threads;
	uint32_t	max_threads;
	uint32_t	min_spare_threads;
	uint32_t	max_spare_threads;
	uint32_t	max_requests_per_thread;
	uint32_t	request_count;
	time_t		time_last_spawned;
	uint32_t	cleanup_delay;
	bool		stop_flag;
#endif	/* WITH_GCD */
	bool		spawn_flag;

#ifdef WNOHANG
	pthread_mutex_t	wait_mutex;
	fr_hash_table_t *waiters;
#endif

#ifdef WITH_GCD
	dispatch_queue_t	queue;
#else

#ifdef WITH_STATS
	fr_pps_t	pps_in, pps_out;
#ifdef WITH_ACCOUNTING
	bool		auto_limit_acct;
#endif
#endif

	/*
	 *	All threads wait on this semaphore, for requests
	 *	to enter the queue.
	 */
	sem_t		semaphore;

	uint32_t	max_queue_size;

#ifndef HAVE_STDATOMIC_H
	/*
	 *	To ensure only one thread at a time touches the queue.
	 */
	pthread_mutex_t	queue_mutex;

	uint32_t	active_threads;	/* protected by queue_mutex */
	uint32_t	exited_threads;
	uint32_t	num_queued;
	fr_fifo_t	*fifo[NUM_FIFOS];
#else
	atomic_uint32_t	  active_threads;
	atomic_uint32_t	  exited_threads;
	fr_atomic_queue_t *queue[NUM_FIFOS];
#endif	/* STDATOMIC */
#endif	/* WITH_GCD */
} THREAD_POOL;

static THREAD_POOL thread_pool;
static bool pool_initialized = false;

#ifndef WITH_GCD
static time_t last_cleaned = 0;

static void thread_pool_manage(time_t now);
#endif

#ifndef WITH_GCD
/*
 *	A mapping of configuration file names to internal integers
 */
static const CONF_PARSER thread_config[] = {
	{ "start_servers", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.start_threads), "5" },
	{ "max_servers", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.max_threads), "32" },
	{ "min_spare_servers", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.min_spare_threads), "3" },
	{ "max_spare_servers", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.max_spare_threads), "10" },
	{ "max_requests_per_server", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.max_requests_per_thread), "0" },
	{ "cleanup_delay", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.cleanup_delay), "5" },
	{ "max_queue_size", FR_CONF_POINTER(PW_TYPE_INTEGER, &thread_pool.max_queue_size), "65536" },
#ifdef WITH_STATS
#ifdef WITH_ACCOUNTING
	{ "auto_limit_acct", FR_CONF_POINTER(PW_TYPE_BOOLEAN, &thread_pool.auto_limit_acct), NULL },
#endif
#endif
	CONF_PARSER_TERMINATOR
};
#endif

#if defined(HAVE_OPENSSL_CRYPTO_H) && defined(HAVE_CRYPTO_SET_LOCKING_CALLBACK)

/*
 *	If we're linking against OpenSSL, then it is the
 *	duty of the application, if it is multithreaded,
 *	to provide OpenSSL with appropriate thread id
 *	and mutex locking functions
 *
 *	Note: this only implements static callbacks.
 *	OpenSSL does not use dynamic locking callbacks
 *	right now, but may in the future, so we will have
 *	to add them at some point.
 */
static pthread_mutex_t *ssl_mutexes = NULL;

static void ssl_locking_function(int mode, int n, UNUSED char const *file, UNUSED int line)
{
	rad_assert(&ssl_mutexes[n] != NULL);

	if (mode & CRYPTO_LOCK) {
		pthread_mutex_lock(&ssl_mutexes[n]);
	} else {
		pthread_mutex_unlock(&ssl_mutexes[n]);
	}
}

/*
 *	Create the TLS mutexes.
 */
int tls_mutexes_init(void)
{
	int i, num;

	rad_assert(ssl_mutexes == NULL);

	num = CRYPTO_num_locks();

	ssl_mutexes = rad_malloc(num * sizeof(pthread_mutex_t));
	if (!ssl_mutexes) {
		ERROR("Error allocating memory for SSL mutexes!");
		return -1;
	}

	for (i = 0; i < num; i++) {
		pthread_mutex_init(&ssl_mutexes[i], NULL);
	}

	CRYPTO_set_locking_callback(ssl_locking_function);

	return 0;
}

static void tls_mutexes_destroy(void)
{
#ifdef HAVE_CRYPTO_SET_LOCKING_CALLBACK
	int i, num;

	rad_assert(ssl_mutexes != NULL);

	num = CRYPTO_num_locks();

	for (i = 0; i < num; i++) {
		pthread_mutex_destroy(&ssl_mutexes[i]);
	}
	free(ssl_mutexes);

	CRYPTO_set_locking_callback(NULL);
#endif
}
#else
#define tls_mutexes_destroy()
#endif

#ifdef WNOHANG
/*
 *	We don't want to catch SIGCHLD for a host of reasons.
 *
 *	- exec_wait means that someone, somewhere, somewhen, will
 *	call waitpid(), and catch the child.
 *
 *	- SIGCHLD is delivered to a random thread, not the one that
 *	forked.
 *
 *	- if another thread catches the child, we have to coordinate
 *	with the thread doing the waiting.
 *
 *	- if we don't waitpid() for non-wait children, they'll be zombies,
 *	and will hang around forever.
 *
 */
static void reap_children(void)
{
	pid_t pid;
	int status;
	thread_fork_t mytf, *tf;


	pthread_mutex_lock(&thread_pool.wait_mutex);

	do {
	retry:
		pid = waitpid(0, &status, WNOHANG);
		if (pid <= 0) break;

		mytf.pid = pid;
		tf = fr_hash_table_finddata(thread_pool.waiters, &mytf);
		if (!tf) goto retry;

		tf->status = status;
		tf->exited = 1;
	} while (fr_hash_table_num_elements(thread_pool.waiters) > 0);

	pthread_mutex_unlock(&thread_pool.wait_mutex);
}
#else
#define reap_children()
#endif /* WNOHANG */

#ifndef WITH_GCD
/*
 *	Add a request to the list of waiting requests.
 *	This function gets called ONLY from the main handler thread...
 *
 *	This function should never fail.
 */
int request_enqueue(REQUEST *request)
{
	bool managed = false;
	time_t now;

	rad_assert(pool_initialized == true);

	/*
	 *	Proxied packets get requeued when they time out, or
	 *	when they get a reply.  We want to use "now" as the
	 *	time, and not the time 5-10 seconds in the past when
	 *	we originally received the request.
	 */
	if (request->priority == RAD_LISTEN_PROXY) {
		now = time(NULL);
	} else {
		now = request->timestamp;
	}

	/*
	 *	If we haven't checked the number of child threads
	 *	in a while, OR if the thread pool appears to be full,
	 *	go manage it.
	 */
	if (last_cleaned < now) {
		thread_pool_manage(now);
		managed = true;
	}

	/*
	 *	Update the request state.
	 */
	request->component = "<core>";
	request->module = "<queue>";
	request->child_state = REQUEST_QUEUED;

#ifdef HAVE_STDATOMIC_H
	if (!managed) {
		uint32_t num;

		num = load(thread_pool.active_threads);
		if (num == thread_pool.total_threads) {
			thread_pool_manage(now);
			managed = true;
		}

		if (!managed) {
			num = load(thread_pool.exited_threads);
			if (num > 0) {
				thread_pool_manage(now);
			}
		}
	}

	/*
	 *	Push the request onto the appropriate fifo for that
	 */
	if (!fr_atomic_queue_push(thread_pool.queue[request->priority], request)) {
		RATE_LIMIT(ERROR("Something is blocking the server.  There are too many packets in the queue, "
				 "waiting to be processed.  Ignoring the new request."));
		return 0;
	}

#else  /* no atomic queues */

	if (!managed && 
	    ((thread_pool.active_threads == thread_pool.total_threads) ||
	     (thread_pool.exited_threads > 0))) {
		thread_pool_manage(now);
	}

	pthread_mutex_lock(&thread_pool.queue_mutex);

#ifdef WITH_STATS
#ifdef WITH_ACCOUNTING
	if (thread_pool.auto_limit_acct) {
		struct timeval when;

		/*
		 *	Throw away accounting requests if we're too
		 *	busy.  The NAS should retransmit these, and no
		 *	one should notice.
		 *
		 *	In contrast, we always try to process
		 *	authentication requests.  Those are more time
		 *	critical, and it's harder to determine which
		 *	we can throw away, and which we can keep.
		 *
		 *	We allow the queue to get half full before we
		 *	start worrying.  Even then, we still require
		 *	that the rate of input packets is higher than
		 *	the rate of outgoing packets.  i.e. the queue
		 *	is growing.
		 *
		 *	Once that happens, we roll a dice to see where
		 *	the barrier is for "keep" versus "toss".  If
		 *	the queue is smaller than the barrier, we
		 *	allow it.  If the queue is larger than the
		 *	barrier, we throw the packet away.  Otherwise,
		 *	we keep it.
		 *
		 *	i.e. the probability of throwing the packet
		 *	away increases from 0 (queue is half full), to
		 *	100 percent (queue is completely full).
		 *
		 *	A probabilistic approach allows us to process
		 *	SOME of the new accounting packets.
		 */
		if ((request->packet->code == PW_CODE_ACCOUNTING_REQUEST) &&
		    (thread_pool.num_queued > (thread_pool.max_queue_size / 2)) &&
		    (thread_pool.pps_in.pps_now > thread_pool.pps_out.pps_now)) {
			uint32_t prob;
			uint32_t keep;

			/*
			 *	Take a random value of how full we
			 *	want the queue to be.  It's OK to be
			 *	half full, but we get excited over
			 *	anything more than that.
			 */
			keep = (thread_pool.max_queue_size / 2);
			prob = fr_rand() & ((1 << 10) - 1);
			keep *= prob;
			keep >>= 10;
			keep += (thread_pool.max_queue_size / 2);

			/*
			 *	If the queue is larger than our dice
			 *	roll, we throw the packet away.
			 */
			if (thread_pool.num_queued > keep) {
				pthread_mutex_unlock(&thread_pool.queue_mutex);
				return 0;
			}
		}

		gettimeofday(&when, NULL);

		/*
		 *	Calculate the instantaneous arrival rate into
		 *	the queue.
		 */
		thread_pool.pps_in.pps = rad_pps(&thread_pool.pps_in.pps_old,
						 &thread_pool.pps_in.pps_now,
						 &thread_pool.pps_in.time_old,
						 &when);

		thread_pool.pps_in.pps_now++;
	}
#endif	/* WITH_ACCOUNTING */
#endif

	thread_pool.request_count++;

	/*
	 *	If there are too many packets _overall_, OR the
	 *	destinatio fifo is full, then try to delete a lower
	 *	priority one.
	 */
	if (((thread_pool.num_queued >= thread_pool.max_queue_size) &&
	     (request_fifo_discard(request->priority, true, now) == 0)) ||
	    (fr_fifo_full(thread_pool.fifo[request->priority]) &&
	     (request_fifo_discard(request->priority, false, now) == 0))) {
		pthread_mutex_unlock(&thread_pool.queue_mutex);
		RATE_LIMIT(ERROR("Something is blocking the server.  There are %d packets in the queue, "
				 "waiting to be processed.  Ignoring the new request.", thread_pool.num_queued));
		return 0;
	}

	/*
	 *	Push the request onto the appropriate fifo for that priority.
	 */
	if (!fr_fifo_push(thread_pool.fifo[request->priority], request)) {
		pthread_mutex_unlock(&thread_pool.queue_mutex);
		RATE_LIMIT(ERROR("Something is blocking the server.  There are too many packets in the queue, "
				 "waiting to be processed.  Ignoring the new request."));
		return 0;
	}

	thread_pool.num_queued++;

	pthread_mutex_unlock(&thread_pool.queue_mutex);
#endif

	/*
	 *	There's one more request in the queue.
	 *
	 *	Note that we're not touching the queue any more, so
	 *	the semaphore post is outside of the mutex.  This also
	 *	means that when the thread wakes up and tries to lock
	 *	the mutex, it will be unlocked, and there won't be
	 *	contention.
	 */
	sem_post(&thread_pool.semaphore);

	return 1;
}

#ifndef HAVE_STDATOMIC_H
/*
 *	Try to free up requests by discarding requests of lower priority.
 */
static int request_fifo_discard(int priority, bool lower, time_t now)
{
	int i, rcode;
	REQUEST *request;

	if (lower) {
		for (i = NUM_FIFOS - 1; i < priority; i--) {
			request = fr_fifo_pop(thread_pool.fifo[i]);
			if (!request) continue;

			fr_assert(request->child_state == REQUEST_QUEUED);
			request->child_state = REQUEST_DONE;
			request_done(request, FR_ACTION_DONE);
			return 1;
		}

		/*
		 *	We didn't discard a lower priority entry.
		 *	Maybe we need to discard one of the current
		 *	priority, which has been in the queue for a
		 *	while.
		 */
	}

	/*
	 *	The time stamp for proxied requests may be 5-10
	 *	seconds in the past, because the home server hasn't
	 *	responded.  We therefore can't discard "old" requests,
	 *	as they have just received a reply, or they have just
	 *	timed out.
	 */
	if (priority <= RAD_LISTEN_PROXY) return 0;

	/*
	 *	Peek at the first entry in the fifo.  Note that there
	 *	is not always a first entry.  This is because we're
	 *	called if there are too many _total_ requests.
	 */
	rcode = 0;

retry:
	request = fr_fifo_peek(thread_pool.fifo[priority]);
	if (!request) return rcode;

	/*
	 *	This request expires in the future.  We can't do anything.
	 */
	if ((request->timestamp + main_config.max_request_time) > now) return rcode;

	request = fr_fifo_pop(thread_pool.fifo[priority]);
	rad_assert(request != NULL);
	VERIFY_REQUEST(request);

	request->child_state = REQUEST_DONE;
	if (request->master_state == REQUEST_TO_FREE) {
		request_free(request);
	} else {
		request_done(request, REQUEST_DONE);
	}
	thread_pool.num_queued--;

	/*
	 *	We might as well delete as many old requests as possible.
	 */
	rcode = 1;
	goto retry;
}
#endif

/*
 *	Remove a request from the queue.
 */
static int request_dequeue(REQUEST **prequest)
{
	time_t blocked;
	static time_t last_complained = 0;
	static time_t total_blocked = 0;
	int num_blocked = 0;
#ifndef HAVE_STDATOMIC_H
	RAD_LISTEN_TYPE start;
#endif
	RAD_LISTEN_TYPE i;
	REQUEST *request = NULL;
	reap_children();

	rad_assert(pool_initialized == true);

#ifdef HAVE_STDATOMIC_H
retry:
	for (i = 0; i < NUM_FIFOS; i++) {
		if (!fr_atomic_queue_pop(thread_pool.queue[i], (void **) &request)) continue;

		rad_assert(request != NULL);

		VERIFY_REQUEST(request);

		if (request->master_state != REQUEST_STOP_PROCESSING) {
			break;
		}

		/*
		 *	This entry was marked to be stopped.  Acknowledge it.
		 *
		 *	If we own the request, we delete it. Otherwise
		 *	we run the "done" callback now, which will
		 *	stop timers, remove it from the request hash,
		 *	update listener counts, etc.
		 *
		 *	Running request_done() here means that old
		 *	requests are cleaned up immediately, which
		 *	frees up more resources for new requests.  It
		 *	also means that we don't need to rely on
		 *	timers to free up the old requests, as those
		 *	timers will run much much later.
		 *
		 *	Catching this corner case doesn't change the
		 *	normal operation of the server.  Most requests
		 *	should NOT be marked "stop processing" when
		 *	they're in the queue.  This situation
		 *	generally happens when the server is blocked,
		 *	due to a slow back-end database.
		 */
		request->child_state = REQUEST_DONE;
		if (request->master_state == REQUEST_TO_FREE) {
			request_free(request);
		} else {
			request_done(request, REQUEST_DONE);
		}
	}

	/*
	 *	Popping might fail.  If so, return.
	 */
	if (!request) return 0;

#else
	pthread_mutex_lock(&thread_pool.queue_mutex);

#ifdef WITH_STATS
#ifdef WITH_ACCOUNTING
	if (thread_pool.auto_limit_acct) {
		struct timeval now;

		gettimeofday(&now, NULL);

		/*
		 *	Calculate the instantaneous departure rate
		 *	from the queue.
		 */
		thread_pool.pps_out.pps  = rad_pps(&thread_pool.pps_out.pps_old,
						   &thread_pool.pps_out.pps_now,
						   &thread_pool.pps_out.time_old,
						   &now);
		thread_pool.pps_out.pps_now++;
	}
#endif
#endif

	/*
	 *	Clear old requests from all queues.
	 *
	 *	We only do one pass over the queue, in order to
	 *	amortize the work across the child threads.  Since we
	 *	do N checks for one request de-queued, the old
	 *	requests will be quickly cleared.
	 */
	for (i = 0; i < NUM_FIFOS; i++) {
		request = fr_fifo_peek(thread_pool.fifo[i]);
		if (!request) continue;

		VERIFY_REQUEST(request);

		if (request->master_state != REQUEST_STOP_PROCESSING) {
			continue;
		}

		/*
		 *	This entry was marked to be stopped.  Acknowledge it.
		 */
		request = fr_fifo_pop(thread_pool.fifo[i]);
		rad_assert(request != NULL);
		VERIFY_REQUEST(request);

		request->child_state = REQUEST_DONE;
		if (request->master_state == REQUEST_TO_FREE) {
			request_free(request);
		} else {
			request_done(request, REQUEST_DONE);
		}
		thread_pool.num_queued--;
	}

	start = 0;
 retry:
	/*
	 *	Pop results from the top of the queue
	 */
	for (i = start; i < NUM_FIFOS; i++) {
		request = fr_fifo_pop(thread_pool.fifo[i]);
		if (request) {
			VERIFY_REQUEST(request);
			start = i;
			break;
		}
	}

	if (!request) {
		pthread_mutex_unlock(&thread_pool.queue_mutex);
		*prequest = NULL;
		return 0;
	}

	rad_assert(thread_pool.num_queued > 0);
	thread_pool.num_queued--;
#endif	/* HAVE_STD_ATOMIC_H */

	*prequest = request;

	rad_assert(*prequest != NULL);
	rad_assert(request->magic == REQUEST_MAGIC);

	/*
	 *	If the request has sat in the queue for too long,
	 *	kill it.
	 *
	 *	The main clean-up code can't delete the request from
	 *	the queue, and therefore won't clean it up until we
	 *	have acknowledged it as "done".
	 */
	if (request->master_state == REQUEST_STOP_PROCESSING) {
		request->module = "<done>";
		request->child_state = REQUEST_DONE;
		goto retry;
	}

	rad_assert(request->child_state == REQUEST_QUEUED);

	request->component = "<core>";
	request->module = "";
	request->child_state = REQUEST_RUNNING;

	/*
	 *	The thread is currently processing a request.
	 */
#ifdef HAVE_STDATOMIC_H
	CAS_INCR(thread_pool.active_threads);
#else
	thread_pool.active_threads++;
#endif

	blocked = time(NULL);
	if (!request->proxy && (blocked - request->timestamp) > 5) {
		total_blocked++;
		if (last_complained < blocked) {
			last_complained = blocked;
			blocked -= request->timestamp;
			num_blocked = total_blocked;
		} else {
			blocked = 0;
		}
	} else {
		total_blocked = 0;
		blocked = 0;
	}

#ifndef HAVE_STDATOMIC_H
	pthread_mutex_unlock(&thread_pool.queue_mutex);
#endif

	if (blocked) {
		ERROR("%d requests have been waiting in the processing queue for %d seconds.  Check that all databases are running properly!",
		      num_blocked, (int) blocked);
	}

	return 1;
}


/*
 *	The main thread handler for requests.
 *
 *	Wait on the semaphore until we have it, and process the request.
 */
static void *request_handler_thread(void *arg)
{
	THREAD_HANDLE *self = (THREAD_HANDLE *) arg;

	/*
	 *	Loop forever, until told to exit.
	 */
	do {
		/*
		 *	Wait to be signalled.
		 */
		DEBUG2("Thread %d waiting to be assigned a request",
		       self->thread_num);
	re_wait:
		if (sem_wait(&thread_pool.semaphore) != 0) {
			/*
			 *	Interrupted system call.  Go back to
			 *	waiting, but DON'T print out any more
			 *	text.
			 */
			if ((errno == EINTR) || (errno == EAGAIN)) {
				DEBUG2("Re-wait %d", self->thread_num);
				goto re_wait;
			}
			ERROR("Thread %d failed waiting for semaphore: %s: Exiting\n",
			       self->thread_num, fr_syserror(errno));
			break;
		}

		DEBUG2("Thread %d got semaphore", self->thread_num);

#ifdef HAVE_OPENSSL_ERR_H
		/*
		 *	Clear the error queue for the current thread.
		 */
		ERR_clear_error();
#endif

		/*
		 *	The server is exiting.  Don't dequeue any
		 *	requests.
		 */
		if (thread_pool.stop_flag) break;

		/*
		 *	Try to grab a request from the queue.
		 *
		 *	It may be empty, in which case we fail
		 *	gracefully.
		 */
		if (!request_dequeue(&self->request)) continue;

		self->request->child_pid = self->pthread_id;
		self->request_count++;

		DEBUG2("Thread %d handling request %d, (%d handled so far)",
		       self->thread_num, self->request->number,
		       self->request_count);

#ifndef HAVE_STDATOMIC_H
#ifdef WITH_ACCOUNTING
		if ((self->request->packet->code == PW_CODE_ACCOUNTING_REQUEST) &&
		    thread_pool.auto_limit_acct) {
			VALUE_PAIR *vp;
			REQUEST *request = self->request;

			vp = radius_pair_create(request, &request->config,
					       181, VENDORPEC_FREERADIUS);
			if (vp) vp->vp_integer = thread_pool.pps_in.pps;

			vp = radius_pair_create(request, &request->config,
					       182, VENDORPEC_FREERADIUS);
			if (vp) vp->vp_integer = thread_pool.pps_in.pps;

			vp = radius_pair_create(request, &request->config,
					       183, VENDORPEC_FREERADIUS);
			if (vp) {
				vp->vp_integer = thread_pool.max_queue_size - thread_pool.num_queued;
				vp->vp_integer *= 100;
				vp->vp_integer /= thread_pool.max_queue_size;
			}
		}
#endif
#endif

		self->request->process(self->request, FR_ACTION_RUN);
		self->request = NULL;

#ifdef HAVE_STDATOMIC_H
		CAS_DECR(thread_pool.active_threads);
#else
		/*
		 *	Update the active threads.
		 */
		pthread_mutex_lock(&thread_pool.queue_mutex);
		rad_assert(thread_pool.active_threads > 0);
		thread_pool.active_threads--;
		pthread_mutex_unlock(&thread_pool.queue_mutex);
#endif

		/*
		 *	If the thread has handled too many requests, then make it
		 *	exit.
		 */
		if ((thread_pool.max_requests_per_thread > 0) &&
		    (self->request_count >= thread_pool.max_requests_per_thread)) {
			DEBUG2("Thread %d handled too many requests",
			       self->thread_num);
			break;
		}
	} while (self->status != THREAD_CANCELLED);

	DEBUG2("Thread %d exiting...", self->thread_num);

#ifdef HAVE_OPENSSL_ERR_H
	/*
	 *	If we linked with OpenSSL, the application
	 *	must remove the thread's error queue before
	 *	exiting to prevent memory leaks.
	 */
#if OPENSSL_VERSION_NUMBER < 0x10000000L
	ERR_remove_state(0);
#elif OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	ERR_remove_thread_state(NULL);
#endif
#endif

#ifdef HAVE_STDATOMIC_H
	CAS_INCR(thread_pool.exited_threads);
#else
	pthread_mutex_lock(&thread_pool.queue_mutex);
	thread_pool.exited_threads++;
	pthread_mutex_unlock(&thread_pool.queue_mutex);
#endif

	/*
	 *  Do this as the LAST thing before exiting.
	 */
	self->request = NULL;
	self->status = THREAD_EXITED;
	exec_trigger(NULL, NULL, "server.thread.stop", true);

	return NULL;
}

/*
 *	Take a THREAD_HANDLE, delete it from the thread pool and
 *	free its resources.
 *
 *	This function is called ONLY from the main server thread,
 *	ONLY after the thread has exited.
 */
static void delete_thread(THREAD_HANDLE *handle)
{
	THREAD_HANDLE *prev;
	THREAD_HANDLE *next;

	rad_assert(handle->request == NULL);

	DEBUG2("Deleting thread %d", handle->thread_num);

	prev = handle->prev;
	next = handle->next;
	rad_assert(thread_pool.total_threads > 0);
	thread_pool.total_threads--;

	/*
	 *	Remove the handle from the list.
	 */
	if (prev == NULL) {
		rad_assert(thread_pool.head == handle);
		thread_pool.head = next;
	} else {
		prev->next = next;
	}

	if (next == NULL) {
		rad_assert(thread_pool.tail == handle);
		thread_pool.tail = prev;
	} else {
		next->prev = prev;
	}

	/*
	 *	Free the handle, now that it's no longer referencable.
	 */
	free(handle);
}


/*
 *	Spawn a new thread, and place it in the thread pool.
 *
 *	The thread is started initially in the blocked state, waiting
 *	for the semaphore.
 */
static THREAD_HANDLE *spawn_thread(time_t now, int do_trigger)
{
	int rcode;
	THREAD_HANDLE *handle;

	/*
	 *	Ensure that we don't spawn too many threads.
	 */
	if (thread_pool.total_threads >= thread_pool.max_threads) {
		DEBUG2("Thread spawn failed.  Maximum number of threads (%d) already running.", thread_pool.max_threads);
		return NULL;
	}

	/*
	 *	Allocate a new thread handle.
	 */
	handle = (THREAD_HANDLE *) rad_malloc(sizeof(THREAD_HANDLE));
	memset(handle, 0, sizeof(THREAD_HANDLE));
	handle->prev = NULL;
	handle->next = NULL;
	handle->thread_num = thread_pool.max_thread_num++;
	handle->request_count = 0;
	handle->status = THREAD_RUNNING;
	handle->timestamp = time(NULL);

	/*
	 *	Create the thread joinable, so that it can be cleaned up
	 *	using pthread_join().
	 *
	 *	Note that the function returns non-zero on error, NOT
	 *	-1.  The return code is the error, and errno isn't set.
	 */
	rcode = pthread_create(&handle->pthread_id, 0, request_handler_thread, handle);
	if (rcode != 0) {
		free(handle);
		ERROR("Thread create failed: %s",
		       fr_syserror(rcode));
		return NULL;
	}

	/*
	 *	One more thread to go into the list.
	 */
	thread_pool.total_threads++;
	DEBUG2("Thread spawned new child %d. Total threads in pool: %d",
			handle->thread_num, thread_pool.total_threads);
	if (do_trigger) exec_trigger(NULL, NULL, "server.thread.start", true);

	/*
	 *	Add the thread handle to the tail of the thread pool list.
	 */
	if (thread_pool.tail) {
		thread_pool.tail->next = handle;
		handle->prev = thread_pool.tail;
		thread_pool.tail = handle;
	} else {
		rad_assert(thread_pool.head == NULL);
		thread_pool.head = thread_pool.tail = handle;
	}

	/*
	 *	Update the time we last spawned a thread.
	 */
	thread_pool.time_last_spawned = now;

	/*
	 * Fire trigger if maximum number of threads reached
	 */
	if (thread_pool.total_threads >= thread_pool.max_threads)
		exec_trigger(NULL, NULL, "server.thread.max_threads", true);

	/*
	 *	And return the new handle to the caller.
	 */
	return handle;
}
#endif	/* WITH_GCD */


#ifdef WNOHANG
static uint32_t pid_hash(void const *data)
{
	thread_fork_t const *tf = data;

	return fr_hash(&tf->pid, sizeof(tf->pid));
}

static int pid_cmp(void const *one, void const *two)
{
	thread_fork_t const *a = one;
	thread_fork_t const *b = two;

	return (a->pid - b->pid);
}
#endif

/*
 *	Allocate the thread pool, and seed it with an initial number
 *	of threads.
 *
 *	FIXME: What to do on a SIGHUP???
 */
DIAG_OFF(deprecated-declarations)
int thread_pool_init(CONF_SECTION *cs, bool *spawn_flag)
{
#ifndef WITH_GCD
	uint32_t	i;
	int		rcode;
#endif
	CONF_SECTION	*pool_cf;
	time_t		now;
#ifdef HAVE_STDATOMIC_H
	int num;
	TALLOC_CTX *autofree;

	autofree = talloc_autofree_context();
#endif

	now = time(NULL);

	rad_assert(spawn_flag != NULL);
	rad_assert(*spawn_flag == true);
	rad_assert(pool_initialized == false); /* not called on HUP */

	pool_cf = cf_subsection_find_next(cs, NULL, "thread");
#ifdef WITH_GCD
	if (pool_cf) WARN("Built with Grand Central Dispatch.  Ignoring 'thread' subsection");
#else
	if (!pool_cf) *spawn_flag = false;
#endif

	/*
	 *	Initialize the thread pool to some reasonable values.
	 */
	memset(&thread_pool, 0, sizeof(THREAD_POOL));
#ifndef WITH_GCD
	thread_pool.head = NULL;
	thread_pool.tail = NULL;
	thread_pool.total_threads = 0;
	thread_pool.max_thread_num = 1;
	thread_pool.cleanup_delay = 5;
	thread_pool.stop_flag = false;
#endif
	thread_pool.spawn_flag = *spawn_flag;

	/*
	 *	Don't bother initializing the mutexes or
	 *	creating the hash tables.  They won't be used.
	 */
	if (!*spawn_flag) return 0;

#ifdef WNOHANG
	if ((pthread_mutex_init(&thread_pool.wait_mutex,NULL) != 0)) {
		ERROR("FATAL: Failed to initialize wait mutex: %s",
		       fr_syserror(errno));
		return -1;
	}

	/*
	 *	Create the hash table of child PID's
	 */
	thread_pool.waiters = fr_hash_table_create(pid_hash,
						   pid_cmp,
						   free);
	if (!thread_pool.waiters) {
		ERROR("FATAL: Failed to set up wait hash");
		return -1;
	}
#endif

#ifndef WITH_GCD
	if (cf_section_parse(pool_cf, NULL, thread_config) < 0) {
		return -1;
	}

	/*
	 *	Catch corner cases.
	 */
	if (thread_pool.min_spare_threads < 1)
		thread_pool.min_spare_threads = 1;
	if (thread_pool.max_spare_threads < 1)
		thread_pool.max_spare_threads = 1;
	if (thread_pool.max_spare_threads < thread_pool.min_spare_threads)
		thread_pool.max_spare_threads = thread_pool.min_spare_threads;
	if (thread_pool.max_threads == 0)
		thread_pool.max_threads = 256;
	if ((thread_pool.max_queue_size < 2) || (thread_pool.max_queue_size > 1024*1024)) {
		ERROR("FATAL: max_queue_size value must be in range 2-1048576");
		return -1;
	}

	if (thread_pool.start_threads > thread_pool.max_threads) {
		ERROR("FATAL: start_servers (%i) must be <= max_servers (%i)",
		      thread_pool.start_threads, thread_pool.max_threads);
		return -1;
	}
#endif	/* WITH_GCD */

	/*
	 *	The pool has already been initialized.  Don't spawn
	 *	new threads, and don't forget about forked children.
	 */
	if (pool_initialized) {
		return 0;
	}

#ifndef WITH_GCD
	/*
	 *	Initialize the queue of requests.
	 */
	memset(&thread_pool.semaphore, 0, sizeof(thread_pool.semaphore));
	rcode = sem_init(&thread_pool.semaphore, 0, SEMAPHORE_LOCKED);
	if (rcode != 0) {
		ERROR("FATAL: Failed to initialize semaphore: %s",
		       fr_syserror(errno));
		return -1;
	}

#ifndef HAVE_STDATOMIC_H
	rcode = pthread_mutex_init(&thread_pool.queue_mutex,NULL);
	if (rcode != 0) {
		ERROR("FATAL: Failed to initialize queue mutex: %s",
		       fr_syserror(errno));
		return -1;
	}
#else
	num = 0;
	store(thread_pool.active_threads, num);
	store(thread_pool.exited_threads, num);
#endif

	/*
	 *	Allocate multiple fifos.
	 */
	for (i = 0; i < NUM_FIFOS; i++) {
#ifdef HAVE_STDATOMIC_H
		thread_pool.queue[i] = fr_atomic_queue_alloc(autofree, thread_pool.max_queue_size);
		if (!thread_pool.queue[i]) {
			ERROR("FATAL: Failed to set up request fifo");
			return -1;
		}
#else
		thread_pool.fifo[i] = fr_fifo_create(NULL, thread_pool.max_queue_size, NULL);
		if (!thread_pool.fifo[i]) {
			ERROR("FATAL: Failed to set up request fifo");
			return -1;
		}
#endif
	}
#endif

#ifndef WITH_GCD
	/*
	 *	Create a number of waiting threads.
	 *
	 *	If we fail while creating them, do something intelligent.
	 */
	for (i = 0; i < thread_pool.start_threads; i++) {
		if (spawn_thread(now, 0) == NULL) {
			return -1;
		}
	}
#else
	thread_pool.queue = dispatch_queue_create("org.freeradius.threads", NULL);
	if (!thread_pool.queue) {
		ERROR("Failed creating dispatch queue: %s", fr_syserror(errno));
		fr_exit(1);
	}
#endif

	DEBUG2("Thread pool initialized");
	pool_initialized = true;
	return 0;
}
DIAG_ON(deprecated-declarations)

void thread_pool_stop(void)
{
	int i, total_threads;

	if (!pool_initialized) return;

	/*
	 *	Set pool stop flag.
	 */
	thread_pool.stop_flag = true;

	/*
	 *	Wakeup all threads to make them see stop flag.
	 */
	total_threads = thread_pool.total_threads;
	for (i = 0; i != total_threads; i++) {
		sem_post(&thread_pool.semaphore);
	}
}

/*
 *	Free all thread-related information
 */
void thread_pool_free(void)
{
#ifndef WITH_GCD
	int i;
	int total_threads;
	THREAD_HANDLE *handle;
	THREAD_HANDLE *next;

	if (!pool_initialized) return;

	/*
	 *	Set pool stop flag.
	 */
	thread_pool.stop_flag = true;

	/*
	 *	Wakeup all threads to make them see stop flag.
	 */
	total_threads = thread_pool.total_threads;
	for (i = 0; i != total_threads; i++) {
		sem_post(&thread_pool.semaphore);
	}

	/*
	 *	Join and free all threads.
	 */
	for (handle = thread_pool.head; handle; handle = next) {
		next = handle->next;
		pthread_join(handle->pthread_id, NULL);
		delete_thread(handle);
	}

	/*
	 *	Free any requests which were blocked in the queue, but
	 *	only if we're checking that no memory leaked.
	 */
	if (main_config.memory_report) {
		REQUEST *request;

		while (request_dequeue(&request) == 1) {
			request->child_state = REQUEST_DONE;
			request_free(request);
		}
	}

	for (i = 0; i < NUM_FIFOS; i++) {
#ifdef HAVE_STDATOMIC_H
		fr_atomic_queue_free(&thread_pool.queue[i]);
#else
		fr_fifo_free(thread_pool.fifo[i]);
#endif
	}

#ifdef WNOHANG
	fr_hash_table_free(thread_pool.waiters);
#endif

	/*
	 *	We're no longer threaded.  Remove the mutexes and free
	 *	the memory.
	 */
	tls_mutexes_destroy();
#endif
}


#ifdef WITH_GCD
int request_enqueue(REQUEST *request)
{
	dispatch_block_t block;

	block = ^{
		request->process(request, FR_ACTION_RUN);
	};

	dispatch_async(thread_pool.queue, block);

	return 1;
}
#endif

#ifndef WITH_GCD
/*
 *	Check the min_spare_threads and max_spare_threads.
 *
 *	If there are too many or too few threads waiting, then we
 *	either create some more, or delete some.
 */
static void thread_pool_manage(time_t now)
{
	uint32_t spare;
	int i, total;
	THREAD_HANDLE *handle, *next;
	uint32_t active_threads;

	/*
	 *	Loop over the thread pool, deleting exited threads.
	 */
	for (handle = thread_pool.head; handle; handle = next) {
		next = handle->next;

		/*
		 *	Maybe we've asked the thread to exit, and it
		 *	has agreed.
		 */
		if (handle->status == THREAD_EXITED) {
			pthread_join(handle->pthread_id, NULL);
			delete_thread(handle);

#ifdef HAVE_STDATOMIC_H
			CAS_DECR(thread_pool.exited_threads);
#else
			pthread_mutex_lock(&thread_pool.queue_mutex);
			thread_pool.exited_threads--;
			pthread_mutex_unlock(&thread_pool.queue_mutex);
#endif
		}
	}

	/*
	 *	We don't need a mutex lock here, as we're reading
	 *	active_threads, and not modifying it.  We want a close
	 *	approximation of the number of active threads, and this
	 *	is good enough.
	 */
#ifdef HAVE_STDATOMIC_H
	active_threads = load(thread_pool.active_threads);
#else
	active_threads = thread_pool.active_threads;
#endif
	spare = thread_pool.total_threads - active_threads;
	if (rad_debug_lvl) {
		static uint32_t old_total = 0;
		static uint32_t old_active = 0;

		if ((old_total != thread_pool.total_threads) || (old_active != active_threads)) {
			DEBUG2("Threads: total/active/spare threads = %d/%d/%d",
			       thread_pool.total_threads, active_threads, spare);
			old_total = thread_pool.total_threads;
			old_active = active_threads;
		}
	}

	/*
	 *	If there are too few spare threads.  Go create some more.
	 */
	if ((thread_pool.total_threads < thread_pool.max_threads) &&
	    (spare < thread_pool.min_spare_threads)) {
		total = thread_pool.min_spare_threads - spare;

		if ((total + thread_pool.total_threads) > thread_pool.max_threads) {
			total = thread_pool.max_threads - thread_pool.total_threads;
		}

		DEBUG2("Threads: Spawning %d spares", total);

		/*
		 *	Create a number of spare threads.
		 */
		for (i = 0; i < total; i++) {
			handle = spawn_thread(now, 1);
			if (handle == NULL) {
				return;
			}
		}

		return;		/* there aren't too many spare threads */
	}

	/*
	 *	Only delete spare threads if we haven't already done
	 *	so this second.
	 */
	if (now == last_cleaned) {
		return;
	}
	last_cleaned = now;

	/*
	 *	Only delete the spare threads if sufficient time has
	 *	passed since we last created one.  This helps to minimize
	 *	the amount of create/delete cycles.
	 */
	if ((now - thread_pool.time_last_spawned) < (int)thread_pool.cleanup_delay) {
		return;
	}

	/*
	 *	If there are too many spare threads, delete one.
	 *
	 *	Note that we only delete ONE at a time, instead of
	 *	wiping out many.  This allows the excess servers to
	 *	be slowly reaped, just in case the load spike comes again.
	 */
	if (spare > thread_pool.max_spare_threads) {

		spare -= thread_pool.max_spare_threads;

		DEBUG2("Threads: deleting 1 spare out of %d spares", spare);

		/*
		 *	Walk through the thread pool, deleting the
		 *	first idle thread we come across.
		 */
		for (handle = thread_pool.head; (handle != NULL) && (spare > 0) ; handle = next) {
			next = handle->next;

			/*
			 *	If the thread is not handling a
			 *	request, but still live, then tell it
			 *	to exit.
			 *
			 *	It will eventually wake up, and realize
			 *	it's been told to commit suicide.
			 */
			if ((handle->request == NULL) &&
			    (handle->status == THREAD_RUNNING)) {
				handle->status = THREAD_CANCELLED;
				/*
				 *	Post an extra semaphore, as a
				 *	signal to wake up, and exit.
				 */
				sem_post(&thread_pool.semaphore);
				spare--;
				break;
			}
		}
	}

	/*
	 *	Otherwise everything's kosher.  There are not too few,
	 *	or too many spare threads.  Exit happily.
	 */
	return;
}
#endif	/* WITH_GCD */

#ifdef WNOHANG
/*
 *	Thread wrapper for fork().
 */
pid_t rad_fork(void)
{
	pid_t child_pid;

	if (!pool_initialized) return fork();

	reap_children();	/* be nice to non-wait thingies */

	if (fr_hash_table_num_elements(thread_pool.waiters) >= 1024) {
		return -1;
	}

	/*
	 *	Fork & save the PID for later reaping.
	 */
	child_pid = fork();
	if (child_pid > 0) {
		int rcode;
		thread_fork_t *tf;

		tf = rad_malloc(sizeof(*tf));
		memset(tf, 0, sizeof(*tf));

		tf->pid = child_pid;

		pthread_mutex_lock(&thread_pool.wait_mutex);
		rcode = fr_hash_table_insert(thread_pool.waiters, tf);
		pthread_mutex_unlock(&thread_pool.wait_mutex);

		if (!rcode) {
			ERROR("Failed to store PID, creating what will be a zombie process %d",
			       (int) child_pid);
			free(tf);
		}
	}

	/*
	 *	Return whatever we were told.
	 */
	return child_pid;
}


/*
 *	Wait 10 seconds at most for a child to exit, then give up.
 */
pid_t rad_waitpid(pid_t pid, int *status)
{
	int i;
	thread_fork_t mytf, *tf;

	if (!pool_initialized) return waitpid(pid, status, 0);

	if (pid <= 0) return -1;

	mytf.pid = pid;

	pthread_mutex_lock(&thread_pool.wait_mutex);
	tf = fr_hash_table_finddata(thread_pool.waiters, &mytf);
	pthread_mutex_unlock(&thread_pool.wait_mutex);

	if (!tf) return -1;

	for (i = 0; i < 100; i++) {
		reap_children();

		if (tf->exited) {
			*status = tf->status;

			pthread_mutex_lock(&thread_pool.wait_mutex);
			fr_hash_table_delete(thread_pool.waiters, &mytf);
			pthread_mutex_unlock(&thread_pool.wait_mutex);
			return pid;
		}
		usleep(100000);	/* sleep for 1/10 of a second */
	}

	/*
	 *	10 seconds have passed, give up on the child.
	 */
	pthread_mutex_lock(&thread_pool.wait_mutex);
	fr_hash_table_delete(thread_pool.waiters, &mytf);
	pthread_mutex_unlock(&thread_pool.wait_mutex);

	return 0;
}
#else
/*
 *	No rad_fork or rad_waitpid
 */
#endif

void thread_pool_queue_stats(int array[RAD_LISTEN_MAX], int pps[2])
{
	int i;

#ifndef WITH_GCD
	if (pool_initialized) {
		struct timeval now;

		for (i = 0; i < RAD_LISTEN_MAX; i++) {
#ifndef HAVE_STDATOMIC_H
			array[i] = fr_fifo_num_elements(thread_pool.fifo[i]);
#else
			array[i] = 0;
#endif
		}

		gettimeofday(&now, NULL);

		pps[0] = rad_pps(&thread_pool.pps_in.pps_old,
				 &thread_pool.pps_in.pps_now,
				 &thread_pool.pps_in.time_old,
				 &now);
		pps[1] = rad_pps(&thread_pool.pps_out.pps_old,
				 &thread_pool.pps_out.pps_now,
				 &thread_pool.pps_out.time_old,
				 &now);

	} else
#endif	/* WITH_GCD */
	{
		for (i = 0; i < RAD_LISTEN_MAX; i++) {
			array[i] = 0;
		}

		pps[0] = pps[1] = 0;
	}
}

void thread_pool_thread_stats(int stats[3])
{
#ifndef WITH_GCD
	if (pool_initialized) {
		/*
		 *	We don't need a mutex lock here as we only want to
		 *	read a close approximation of the number of active
		 *	threads, and not modify it.
		 */
#ifdef HAVE_STDATOMIC_H
		stats[0] = load(thread_pool.active_threads);
#else
		stats[0] = thread_pool.active_threads;
#endif
		stats[1] = thread_pool.total_threads;
		stats[2] = thread_pool.max_threads;
	} else
#endif	/* WITH_GCD */
	{
		stats[0] = stats[1] = stats[2] = 0;
	}
}
#endif /* HAVE_PTHREAD_H */

static void time_free(void *data)
{
	free(data);
}

void exec_trigger(REQUEST *request, CONF_SECTION *cs, char const *name, int quench)
{
	CONF_SECTION *subcs;
	CONF_ITEM *ci;
	CONF_PAIR *cp;
	char const *attr;
	char const *value;
	VALUE_PAIR *vp;
	bool alloc = false;

	/*
	 *	Use global "trigger" section if no local config is given.
	 */
	if (!cs) {
		cs = main_config.config;
		attr = name;
	} else {
		/*
		 *	Try to use pair name, rather than reference.
		 */
		attr = strrchr(name, '.');
		if (attr) {
			attr++;
		} else {
			attr = name;
		}
	}

	/*
	 *	Find local "trigger" subsection.  If it isn't found,
	 *	try using the global "trigger" section, and reset the
	 *	reference to the full path, rather than the sub-path.
	 */
	subcs = cf_section_sub_find(cs, "trigger");
	if (!subcs && (cs != main_config.config)) {
		subcs = cf_section_sub_find(main_config.config, "trigger");
		attr = name;
	}

	if (!subcs) return;

	ci = cf_reference_item(subcs, main_config.config, attr);
	if (!ci) {
		ERROR("No such item in trigger section: %s", attr);
		return;
	}

	if (!cf_item_is_pair(ci)) {
		ERROR("Trigger is not a configuration variable: %s", attr);
		return;
	}

	cp = cf_item_to_pair(ci);
	if (!cp) return;

	value = cf_pair_value(cp);
	if (!value) {
		ERROR("Trigger has no value: %s", name);
		return;
	}

	/*
	 *	May be called for Status-Server packets.
	 */
	vp = NULL;
	if (request && request->packet) vp = request->packet->vps;

	/*
	 *	Perform periodic quenching.
	 */
	if (quench) {
		time_t *last_time;

		last_time = cf_data_find(cs, value);
		if (!last_time) {
			last_time = rad_malloc(sizeof(*last_time));
			*last_time = 0;

			if (cf_data_add(cs, value, last_time, time_free) < 0) {
				free(last_time);
				last_time = NULL;
			}
		}

		/*
		 *	Send the quenched traps at most once per second.
		 */
		if (last_time) {
			time_t now = time(NULL);
			if (*last_time == now) return;

			*last_time = now;
		}
	}

	/*
	 *	radius_exec_program always needs a request.
	 */
	if (!request) {
		request = request_alloc(NULL);
		alloc = true;
	}

	DEBUG("Trigger %s -> %s", name, value);

	radius_exec_program(request, NULL, 0, NULL, request, value, vp, false, true, 0);

	if (alloc) talloc_free(request);
}
