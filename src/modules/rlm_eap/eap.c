/*
 * eap.c    rfc2284 & rfc2869 implementation
 *
 * Version:     $Id$
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
 * Copyright 2000-2003,2006  The FreeRADIUS server project
 * Copyright 2001  hereUare Communications, Inc. <raghud@hereuare.com>
 * Copyright 2003  Alan DeKok <aland@freeradius.org>
 */
/*
 *  EAP PACKET FORMAT
 *  --- ------ ------
 *  0		   1		   2		   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Code      |  Identifier   |	    Length	     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |    Data ...
 * +-+-+-+-+
 *
 *
 * EAP Request and Response Packet Format
 * --- ------- --- -------- ------ ------
 *  0		   1		   2		   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Code      |  Identifier   |	    Length	     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Type      |  Type-Data ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
 *
 *
 * EAP Success and Failure Packet Format
 * --- ------- --- ------- ------ ------
 *  0		   1		   2		   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Code      |  Identifier   |	    Length	     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */

#include <freeradius-devel/modpriv.h>

RCSID("$Id$")

#include "rlm_eap.h"
#include <ctype.h>

static char const *eap_codes[] = {
	 "",				/* 0 is invalid */
	"Request",
	"Response",
	"Success",
	"Failure"
};

static int _eap_module_free(eap_module_t *inst)
{
	/*
	 *	We have to check inst->type as it's only allocated
	 *	if we loaded the eap method.
	 */
	if (inst->type && inst->type->detach) (inst->type->detach)(inst->instance);

#ifndef NDEBUG
	/*
	 *	Don't dlclose() modules if we're doing memory
	 *	debugging.  This removes the symbols needed by
	 *	valgrind.
	 */
	if (!main_config.debug_memory)
#endif
	if (inst->handle) dlclose(inst->handle);

	return 0;
}

/** Load required EAP sub-modules (methods)
 *
 */
int eap_module_instantiate(rlm_eap_t *inst, eap_module_t **m_inst, eap_type_t num, CONF_SECTION *cs)
{
	eap_module_t *method;
	char *mod_name, *p;

	/* Make room for the EAP-Type */
	*m_inst = method = talloc_zero(cs, eap_module_t);
	if (!inst) return -1;

	talloc_set_destructor(method, _eap_module_free);

	/* fill in the structure */
	method->cs = cs;
	method->name = eap_type2name(num);

	/*
	 *	The name of the module were trying to load
	 */
	mod_name = talloc_typed_asprintf(method, "rlm_eap_%s", method->name);

	/*
	 *	dlopen is case sensitive
	 */
	p = mod_name;
	while (*p) {
		*p = tolower((uint8_t) *p);
		p++;
	}

	/*
	 *	Link the loaded EAP-Type
	 */
	method->handle = fr_dlopenext(mod_name);
	if (!method->handle) {
		ERROR("rlm_eap (%s): Failed to link %s: %s", inst->xlat_name, mod_name, fr_strerror());

		return -1;
	}

	method->type = dlsym(method->handle, mod_name);
	if (!method->type) {
		ERROR("rlm_eap (%s): Failed linking to structure in %s: %s", inst->xlat_name,
		       method->name, dlerror());

		return -1;
	}

	cf_log_module(cs, "Linked to sub-module %s", mod_name);

	/*
	 *	Call the attach num in the EAP num module
	 */
	if ((method->type->instantiate) && ((method->type->instantiate)(method->cs, &(method->instance)) < 0)) {
		ERROR("rlm_eap (%s): Failed to initialise %s", inst->xlat_name, mod_name);

		if (method->instance) {
			(void) talloc_steal(method, method->instance);
		}

		return -1;
	}

	if (method->instance) {
		(void) talloc_steal(method, method->instance);
	}

	return 0;
}

/*
 * Call the appropriate handle with the right eap_method.
 */
static int eap_module_call(eap_module_t *module, eap_handler_t *handler)
{
	int rcode = 1;
	REQUEST *request = handler->request;

	char const *caller = request->module;

	rad_assert(module != NULL);

	RDEBUG2("Calling submodule %s to process data", module->type->name);

	request->module = module->type->name;

	switch (handler->stage) {
	case INITIATE:
		if (!module->type->session_init(module->instance, handler)) {
			rcode = 0;
		}

		break;

	case PROCESS:
		/*
		 *   The called function updates the EAP reply packet.
		 */
		if (!module->type->process ||
		    !module->type->process(module->instance, handler)) {
			rcode = 0;
		}

		break;

	default:
		/* Should never enter here */
		RDEBUG("Internal sanity check failed on EAP");
		rcode = 0;
		break;
	}

	request->module = caller;
	return rcode;
}

/** Process NAK data from EAP peer
 *
 */
static eap_type_t eap_process_nak(rlm_eap_t *inst, REQUEST *request,
				    eap_type_t type,
				    eap_type_data_t *nak)
{
	unsigned int i;
	VALUE_PAIR *vp;
	eap_type_t method = PW_EAP_INVALID;

	/*
	 *	The NAK data is the preferred EAP type(s) of
	 *	the client.
	 *
	 *	RFC 3748 says to list one or more proposed
	 *	alternative types, one per octet, or to use
	 *	0 for no alternative.
	 */
	if (!nak->data) {
		REDEBUG("Peer sent empty (invalid) NAK. "
			"Can't select method to continue with");

		return PW_EAP_INVALID;
	}

	/*
	 *	Pick one type out of the one they asked for,
	 *	as they may have asked for many.
	 */
	vp = fr_pair_find_by_num(request->config, PW_EAP_TYPE, 0, TAG_ANY);
	for (i = 0; i < nak->length; i++) {
		/*
		 *	Type 0 is valid, and means there are no
		 *	common choices.
		 */
		if (nak->data[i] == 0) {
			RDEBUG("Peer NAK'd indicating it is not willing to "
			       "continue ");

			return PW_EAP_INVALID;
		}

		/*
		 *	It is invalid to request identity,
		 *	notification & nak in nak.
		 */
		if (nak->data[i] < PW_EAP_MD5) {
			REDEBUG("Peer NAK'd asking for bad "
				"type %s (%d)",
				eap_type2name(nak->data[i]),
				nak->data[i]);

			return PW_EAP_INVALID;
		}

		if ((nak->data[i] >= PW_EAP_MAX_TYPES) ||
		    !inst->methods[nak->data[i]]) {
			RDEBUG2("Peer NAK'd asking for "
				"unsupported EAP type %s (%d), skipping...",
				eap_type2name(nak->data[i]),
				nak->data[i]);

			continue;
		}

		/*
		 *	Prevent a firestorm if the client is confused.
		 */
		if (type == nak->data[i]) {
			RDEBUG2("Peer NAK'd our request for "
				"%s (%d) with a request for "
				"%s (%d), skipping...",
				eap_type2name(nak->data[i]),
				nak->data[i],
				eap_type2name(nak->data[i]),
				nak->data[i]);

			RWARN("!!! We requested to use an EAP type as normal.");
			RWARN("!!! The supplicant rejected that, and requested to use the same EAP type.");
			RWARN("!!!     i.e. the supplicant said 'I don't like X, please use X instead.");
			RWARN("!!! The supplicant software is broken and does not work properly.");
			RWARN("!!! Please upgrade it to software that works.");

			continue;
		}

		/*
		 *	Enforce per-user configuration of EAP
		 *	types.
		 */
		if (vp && (vp->vp_integer != nak->data[i])) {
			RDEBUG2("Peer wants %s (%d), while we "
				"require %s (%d), skipping",
				eap_type2name(nak->data[i]),
				nak->data[i],
				eap_type2name(vp->vp_integer),
				vp->vp_integer);

			continue;
		}

		RDEBUG("Found mutually acceptable type %s (%d)",
		       eap_type2name(nak->data[i]), nak->data[i]);

		method = nak->data[i];

		break;
	}

	if (method == PW_EAP_INVALID) {
		REDEBUG("No mutually acceptable types found");
	}

	return method;
}

/** Select the correct callback based on a response
 *
 * Based on the EAP response from the supplicant, call the appropriate
 * method callback.
 *
 * Default to the configured EAP-Type for all Unsupported EAP-Types.
 *
 * @param inst Configuration data for this instance of rlm_eap.
 * @param handler State data that persists over multiple rounds of EAP.
 * @return a status code.
 */
eap_rcode_t eap_method_select(rlm_eap_t *inst, eap_handler_t *handler)
{
	eap_type_data_t		*type = &handler->eap_ds->response->type;
	REQUEST			*request = handler->request;

	eap_type_t		next = inst->default_method;
	VALUE_PAIR		*vp;

	/*
	 *	Don't trust anyone.
	 */
	if ((type->num == 0) || (type->num >= PW_EAP_MAX_TYPES)) {
		REDEBUG("Peer sent EAP method number %d, which is outside known range", type->num);

		return EAP_INVALID;
	}

	/*
	 *	Multiple levels of TLS nesting are invalid.  But if
	 *	the parent has a home_server defined, then this
	 *	request is being processed through a virtual
	 *	server... so that's OK.
	 *
	 *	i.e. we're inside an EAP tunnel, which means we have a
	 *	parent.  If the outer session exists, and doesn't have
	 *	a home server, then it's multiple layers of tunneling.
	 */
	if (handler->request->parent &&
	    handler->request->parent->parent &&
	    !handler->request->parent->parent->home_server) {
		RERROR("Multiple levels of TLS nesting are invalid");

		return EAP_INVALID;
	}

	RDEBUG2("Peer sent packet with method EAP %s (%d)", eap_type2name(type->num), type->num);
	/*
	 *	Figure out what to do.
	 */
	switch (type->num) {
	case PW_EAP_IDENTITY:
		/*
		 *	Allow per-user configuration of EAP types.
		 */
		vp = fr_pair_find_by_num(handler->request->config, PW_EAP_TYPE, 0,
			      TAG_ANY);
		if (vp) {
			next = vp->vp_integer;

			/*
			 *	Ensure it's valid.
			 */
			if ((next < PW_EAP_MD5) ||
			    (next >= PW_EAP_MAX_TYPES) ||
			    (!inst->methods[next])) {
				REDEBUG2("Tried to start unsupported EAP type %s (%d)",
					 eap_type2name(next), next);

				return EAP_INVALID;
			}

			RDEBUG("Found &control:EAP-Type = %s", eap_type2name(next));
		} else {
			RDEBUG("Using default_eap_type = %s", eap_type2name(next));
		}

	do_initiate:
		/*
		 *	If any of these fail, we messed badly somewhere
		 */
		rad_assert(next >= PW_EAP_MD5);
		rad_assert(next < PW_EAP_MAX_TYPES);
		rad_assert(inst->methods[next]);

		handler->stage = INITIATE;
		handler->type = next;

		if (eap_module_call(inst->methods[next], handler) == 0) {
			REDEBUG2("Failed starting EAP %s (%d) session.  EAP sub-module failed",
				 eap_type2name(next), next);

			return EAP_INVALID;
		}
		break;

	case PW_EAP_NAK:
		/*
		 *	Delete old data, if necessary.
		 */
		if (handler->opaque && handler->free_opaque) {
			handler->free_opaque(handler->opaque);
			handler->free_opaque = NULL;
			handler->opaque = NULL;
		}

		/*
		 *	We got a NAK after the peer started doing a
		 *	particular EAP type.  That's rude, tell the
		 *	peer to go away.
		 */
		if (handler->started) return EAP_INVALID;

		next = eap_process_nak(inst, handler->request,
				       handler->type, type);

		/*
		 *	We probably want to return 'fail' here...
		 */
		if (!next) {
			return EAP_INVALID;
		}

		RDEBUG("Found compatible type in NAK - EAP-Type = %s", eap_type2name(next));
		goto do_initiate;

		/*
		 *	Key off of the configured sub-modules.
		 */
	default:
		/*
		 *	We haven't configured it, it doesn't exist.
		 */
		if (!inst->methods[type->num]) {
			REDEBUG2("Client asked for unsupported EAP type %s (%d)",
				 eap_type2name(type->num),
				 type->num);

			return EAP_INVALID;
		}

		rad_assert(handler->stage == PROCESS);
		handler->type = type->num;
		if (eap_module_call(inst->methods[type->num],
				    handler) == 0) {
			REDEBUG2("Failed continuing EAP %s (%d) session.  EAP sub-module failed",
				 eap_type2name(type->num),
				 type->num);

			return EAP_INVALID;
		}
		handler->started = true;
		break;
	}

	return EAP_OK;
}


/*
 *	compose EAP reply packet in EAP-Message attr of RADIUS.
 *
 *	Set the RADIUS reply codes based on EAP request codes.  Append
 *	any additonal VPs to RADIUS reply
 */
rlm_rcode_t eap_compose(eap_handler_t *handler)
{
	VALUE_PAIR *vp;
	eap_packet_raw_t *eap_packet;
	REQUEST *request;
	EAP_DS *eap_ds;
	eap_packet_t *reply;
	int rcode;

#ifndef NDEBUG
	handler = talloc_get_type_abort(handler, eap_handler_t);
	request = talloc_get_type_abort(handler->request, REQUEST);
	eap_ds = talloc_get_type_abort(handler->eap_ds, EAP_DS);
	reply = talloc_get_type_abort(eap_ds->request, eap_packet_t);
#else
	request = handler->request;
	eap_ds = handler->eap_ds;
	reply = eap_ds->request;
#endif

	/*
	 *	The Id for the EAP packet to the NAS wasn't set.
	 *	Do so now.
	 */
	if (!eap_ds->set_request_id) {
		/*
		 *	Id serves to suppport request/response
		 *	retransmission in the EAP layer and as such
		 *	must be different for 'adjacent' packets
		 *	except in case of success/failure-replies.
		 *
		 *	RFC2716 (EAP-TLS) requires this to be
		 *	incremented, RFC2284 only makes the above-
		 *	mentioned restriction.
		 */
		reply->id = handler->eap_ds->response->id;

		switch (reply->code) {
			/*
			 *	The Id is a simple "ack" for success
			 *	and failure.
			 *
			 *	RFC 3748 section 4.2 says
			 *
			 *	... The Identifier field MUST match
			 *	the Identifier field of the Response
			 *	packet that it is sent in response
			 *	to.
			 */
		case PW_EAP_SUCCESS:
		case PW_EAP_FAILURE:
			break;

			/*
			 *	We've sent a response to their
			 *	request, the Id is incremented.
			 */
		default:
			++reply->id;
		}
	}

	/*
	 *	For Request & Response packets, set the EAP sub-type,
	 *	if the EAP sub-module didn't already set it.
	 *
	 *	This allows the TLS module to be "morphic", and means
	 *	that the TTLS and PEAP modules can call it to do most
	 *	of their dirty work.
	 */
	if (((eap_ds->request->code == PW_EAP_REQUEST) ||
	     (eap_ds->request->code == PW_EAP_RESPONSE)) &&
	    (eap_ds->request->type.num == 0)) {
		rad_assert(handler->type >= PW_EAP_MD5);
		rad_assert(handler->type < PW_EAP_MAX_TYPES);

		eap_ds->request->type.num = handler->type;
	}

	if (eap_wireformat(reply) == EAP_INVALID) {
		return RLM_MODULE_INVALID;
	}
	eap_packet = (eap_packet_raw_t *)reply->packet;

	vp = radius_pair_create(request->reply, &request->reply->vps, PW_EAP_MESSAGE, 0);
	if (!vp) return RLM_MODULE_INVALID;

	vp->vp_length = eap_packet->length[0] * 256 + eap_packet->length[1];
	vp->vp_octets = talloc_steal(vp, reply->packet);
	reply->packet = NULL;

	/*
	 *	EAP-Message is always associated with
	 *	Message-Authenticator but not vice-versa.
	 *
	 *	Don't add a Message-Authenticator if it's already
	 *	there.
	 */
	vp = fr_pair_find_by_num(request->reply->vps, PW_MESSAGE_AUTHENTICATOR, 0, TAG_ANY);
	if (!vp) {
		vp = fr_pair_afrom_num(request->reply, PW_MESSAGE_AUTHENTICATOR, 0);
		vp->vp_length = AUTH_VECTOR_LEN;
		vp->vp_octets = talloc_zero_array(vp, uint8_t, vp->vp_length);
		fr_pair_add(&(request->reply->vps), vp);
	}

	/* Set request reply code, but only if it's not already set. */
	rcode = RLM_MODULE_OK;
	if (!request->reply->code) switch (reply->code) {
	case PW_EAP_RESPONSE:
		request->reply->code = PW_CODE_ACCESS_REJECT;
		rcode = RLM_MODULE_REJECT;
		break;
	case PW_EAP_SUCCESS:
		request->reply->code = PW_CODE_ACCESS_ACCEPT;
		rcode = RLM_MODULE_OK;
		break;
	case PW_EAP_FAILURE:
		request->reply->code = PW_CODE_ACCESS_REJECT;
		rcode = RLM_MODULE_REJECT;
		break;
	case PW_EAP_REQUEST:
		request->reply->code = PW_CODE_ACCESS_CHALLENGE;
		rcode = RLM_MODULE_HANDLED;
		break;
	default:
		/*
		 *	When we're pulling MS-CHAPv2 out of EAP-MS-CHAPv2,
		 *	we do so WITHOUT setting a reply code, as the
		 *	request is being proxied.
		 */
		if (request->options & RAD_REQUEST_OPTION_PROXY_EAP) {
			return RLM_MODULE_HANDLED;
		}

		/* Should never enter here */
		REDEBUG("Reply code %d is unknown, rejecting the request", reply->code);
		request->reply->code = PW_CODE_ACCESS_REJECT;
		reply->code = PW_EAP_FAILURE;
		rcode = RLM_MODULE_REJECT;
		break;
	}

	RDEBUG2("Sending EAP %s (code %i) ID %d length %i",
		eap_codes[eap_packet->code], eap_packet->code, reply->id,
		eap_packet->length[0] * 256 + eap_packet->length[1]);

	return rcode;
}

/*
 * Radius criteria, EAP-Message is invalid without Message-Authenticator
 * For EAP_START, send Access-Challenge with EAP Identity request.
 */
int eap_start(rlm_eap_t *inst, REQUEST *request)
{
	VALUE_PAIR *vp, *proxy;
	VALUE_PAIR *eap_msg;

	eap_msg = fr_pair_find_by_num(request->packet->vps, PW_EAP_MESSAGE, 0, TAG_ANY);
	if (!eap_msg) {
		RDEBUG2("No EAP-Message, not doing EAP");
		return EAP_NOOP;
	}

	/*
	 *	Look for EAP-Type = None (FreeRADIUS specific attribute)
	 *	this allows you to NOT do EAP for some users.
	 */
	vp = fr_pair_find_by_num(request->packet->vps, PW_EAP_TYPE, 0, TAG_ANY);
	if (vp && vp->vp_integer == 0) {
		RDEBUG2("Found EAP-Message, but EAP-Type = None, so we're not doing EAP");
		return EAP_NOOP;
	}

	/*
	 *	http://www.freeradius.org/rfc/rfc2869.html#EAP-Message
	 *
	 *	Checks for Message-Authenticator are handled by rad_recv().
	 */

	/*
	 *	Check for a Proxy-To-Realm.  Don't get excited over LOCAL
	 *	realms (sigh).
	 */
	proxy = fr_pair_find_by_num(request->config, PW_PROXY_TO_REALM, 0, TAG_ANY);
	if (proxy) {
		REALM *realm;

		/*
		 *	If it's a LOCAL realm, then we're not proxying
		 *	to it.
		 */
		realm = realm_find(proxy->vp_strvalue);
		if (!realm || (realm && (!realm->auth_pool))) {
			proxy = NULL;
		}
	}

	/*
	 *	Check the length before de-referencing the contents.
	 *
	 *	Lengths of zero are required by the RFC for EAP-Start,
	 *	but we've never seen them in practice.
	 *
	 *	Lengths of two are what we see in practice as
	 *	EAP-Starts.
	 */
	if ((eap_msg->vp_length == 0) || (eap_msg->vp_length == 2)) {
		uint8_t *p;

		/*
		 *	It's a valid EAP-Start, but the request
		 *	was marked as being proxied.  So we don't
		 *	do EAP, as the home server will do it.
		 */
		if (proxy) {
		do_proxy:
			RDEBUG2("Request is supposed to be proxied to "
				"Realm %s. Not doing EAP.", proxy->vp_strvalue);
			return EAP_NOOP;
		}

		RDEBUG2("Got EAP_START message");
		vp = fr_pair_afrom_num(request->reply, PW_EAP_MESSAGE, 0);
		if (!vp) return EAP_FAIL;
		fr_pair_add(&request->reply->vps, vp);

		/*
		 *	Manually create an EAP Identity request
		 */
		vp->vp_length = 5;
		vp->vp_octets = p = talloc_array(vp, uint8_t, vp->vp_length);

		p[0] = PW_EAP_REQUEST;
		p[1] = 0; /* ID */
		p[2] = 0;
		p[3] = 5; /* length */
		p[4] = PW_EAP_IDENTITY;

		request->reply->code = PW_CODE_ACCESS_CHALLENGE;
		return EAP_FOUND;
	} /* end of handling EAP-Start */

	/*
	 *	The EAP packet header is 4 bytes, plus one byte of
	 *	EAP sub-type.  Short packets are discarded, unless
	 *	we're proxying.
	 */
	if (eap_msg->vp_length < (EAP_HEADER_LEN + 1)) {
		if (proxy) goto do_proxy;

		RDEBUG2("Ignoring EAP-Message which is too short to be meaningful");
		return EAP_FAIL;
	}

	/*
	 *	Create an EAP-Type containing the EAP-type
	 *	from the packet.
	 */
	vp = fr_pair_afrom_num(request->packet, PW_EAP_TYPE, 0);
	if (vp) {
		vp->vp_integer = eap_msg->vp_octets[4];
		fr_pair_add(&(request->packet->vps), vp);
	}

	/*
	 *	If the request was marked to be proxied, do it now.
	 *	This is done after checking for a valid length
	 *	(which may not be good), and after adding the EAP-Type
	 *	attribute.  This lets other modules selectively cancel
	 *	proxying based on EAP-Type.
	 */
	if (proxy) goto do_proxy;

	/*
	 *	From now on, we're supposed to be handling the
	 *	EAP packet.  We better understand it...
	 */

	/*
	 *	We're allowed only a few codes.  Request, Response,
	 *	Success, or Failure.
	 */
	if ((eap_msg->vp_octets[0] == 0) ||
	    (eap_msg->vp_octets[0] >= PW_EAP_MAX_CODES)) {
		RDEBUG2("Peer sent EAP packet with unknown code %i", eap_msg->vp_octets[0]);
	} else {
		RDEBUG2("Peer sent EAP %s (code %i) ID %d length %zu",
		        eap_codes[eap_msg->vp_octets[0]],
		        eap_msg->vp_octets[0],
		        eap_msg->vp_octets[1],
		        eap_msg->vp_length);
	}

	/*
	 *	We handle request and responses.  The only other defined
	 *	codes are success and fail.  The client SHOULD NOT be
	 *	sending success/fail packets to us, as it doesn't make
	 *	sense.
	 */
	if ((eap_msg->vp_octets[0] != PW_EAP_REQUEST) &&
	    (eap_msg->vp_octets[0] != PW_EAP_RESPONSE)) {
		RDEBUG2("Ignoring EAP packet which we don't know how to handle");
		return EAP_FAIL;
	}

	/*
	 *	We've been told to ignore unknown EAP types, AND it's
	 *	an unknown type.  Return "NOOP", which will cause the
	 *	mod_authorize() to return NOOP.
	 *
	 *	EAP-Identity, Notification, and NAK are all handled
	 *	internally, so they never have handlers.
	 */
	if ((eap_msg->vp_octets[4] >= PW_EAP_MD5) &&
	    inst->ignore_unknown_types &&
	    ((eap_msg->vp_octets[4] == 0) ||
	     (eap_msg->vp_octets[4] >= PW_EAP_MAX_TYPES) ||
	     (!inst->methods[eap_msg->vp_octets[4]]))) {
		RDEBUG2("Ignoring Unknown EAP type %02x", eap_msg->vp_octets[4]);
		return EAP_NOOP;
	}

	/*
	 *	They're NAKing the EAP type we wanted to use, and
	 *	asking for one which we don't support.
	 *
	 *	NAK is code + id + length1 + length + NAK
	 *	     + requested EAP type(s).
	 *
	 *	We know at this point that we can't handle the
	 *	request.  We could either return an EAP-Fail here, but
	 *	it's not too critical.
	 *
	 *	By returning "noop", we can ensure that authorize()
	 *	returns NOOP, and another module may choose to proxy
	 *	the request.
	 */
	if ((eap_msg->vp_octets[4] == PW_EAP_NAK) &&
	    (eap_msg->vp_length >= (EAP_HEADER_LEN + 2)) &&
	    inst->ignore_unknown_types &&
	    ((eap_msg->vp_octets[5] == 0) ||
	     (eap_msg->vp_octets[5] >= PW_EAP_MAX_TYPES) ||
	     (!inst->methods[eap_msg->vp_octets[5]]))) {
		RDEBUG2("Ignoring NAK with request for unknown EAP type");
		return EAP_NOOP;
	}

	if ((eap_msg->vp_octets[4] == PW_EAP_TTLS) ||
	    (eap_msg->vp_octets[4] == PW_EAP_FAST) ||
	    (eap_msg->vp_octets[4] == PW_EAP_TEAP) ||
	    (eap_msg->vp_octets[4] == PW_EAP_PEAP)) {
		RDEBUG2("Continuing tunnel setup");
		return EAP_OK;
	}
	/*
	 * We return ok in response to EAP identity
	 * This means we can write:
	 *
	 * eap {
	 *   ok = return
	 * }
	 * ldap
	 * sql
	 *
	 * ...in the inner-tunnel, to avoid expensive and unnecessary SQL/LDAP lookups
	 */
	if (eap_msg->vp_octets[4] == PW_EAP_IDENTITY) {
		RDEBUG2("EAP-Identity reply, returning 'ok' so we can short-circuit the rest of authorize");
		return EAP_OK;
	}

	/*
	 *	Later EAP messages are longer than the 'start'
	 *	message, so if everything is OK, this function returns
	 *	'no start found', so that the rest of the EAP code can
	 *	use the State attribute to match this EAP-Message to
	 *	an ongoing conversation.
	 */
	RDEBUG2("No EAP Start, assuming it's an on-going EAP conversation");

	return EAP_NOTFOUND;
}

/*
 *	compose EAP FAILURE packet in EAP-Message
 */
void eap_fail(eap_handler_t *handler)
{
	/*
	 *	Delete any previous replies.
	 */
	fr_pair_delete_by_num(&handler->request->reply->vps, PW_EAP_MESSAGE, 0, TAG_ANY);
	fr_pair_delete_by_num(&handler->request->reply->vps, PW_STATE, 0, TAG_ANY);

	talloc_free(handler->eap_ds->request);
	handler->eap_ds->request = talloc_zero(handler->eap_ds, eap_packet_t);
	handler->eap_ds->request->code = PW_EAP_FAILURE;
	handler->finished = true;
	eap_compose(handler);
}

/*
 *	compose EAP SUCCESS packet in EAP-Message
 */
void eap_success(eap_handler_t *handler)
{
	handler->eap_ds->request->code = PW_EAP_SUCCESS;
	handler->finished = true;
	eap_compose(handler);
}

/*
 * Basic EAP packet verfications & validations
 */
static int eap_validation(REQUEST *request, eap_packet_raw_t **eap_packet_p)
{
	uint16_t len;
	eap_packet_raw_t *eap_packet = *eap_packet_p;

	memcpy(&len, eap_packet->length, sizeof(uint16_t));
	len = ntohs(len);

	/*
	 *	High level EAP packet checks
	 */
	if (len <= EAP_HEADER_LEN) {
		RAUTH("EAP packet is too small: Ignoring it.");
		return EAP_INVALID;
	}

	if (eap_packet->code == PW_EAP_REQUEST) {
		VALUE_PAIR *vp;
		RAUTH("Unexpected EAP-Request.  NAKing it.");

		vp = pair_make_reply("EAP-Message", "123456", T_OP_SET);
		if (vp) {
			uint8_t buffer[6];

			buffer[0] = PW_EAP_RESPONSE;
			buffer[1] = eap_packet->id;
			buffer[2] = 0;
			buffer[3] = 6;
			buffer[4] = PW_EAP_NAK;
			buffer[5] = 0; /* no overlapping EAP types */

			fr_pair_value_memcpy(vp, buffer, 6);
		}

		/*
		 *	Ensure that the Access-Reject has a Message-Authenticator
		 */
		vp = fr_pair_find_by_num(request->reply->vps, PW_MESSAGE_AUTHENTICATOR, 0, TAG_ANY);
		if (!vp) {
			vp = fr_pair_afrom_num(request->reply, PW_MESSAGE_AUTHENTICATOR, 0);
			vp->vp_length = AUTH_VECTOR_LEN;
			vp->vp_octets = talloc_zero_array(vp, uint8_t, vp->vp_length);
			fr_pair_add(&(request->reply->vps), vp);
		}
		request->reply->code = PW_CODE_ACCESS_REJECT;

		return EAP_INVALID;
	}

	/*
	 *	We only allow responses from the peer.  The peer
	 *	CANNOT ask us to authenticate outselves.
	 */
	if (eap_packet->code != PW_EAP_RESPONSE) {
		RAUTH("Unexpected packet code %02x: Ignoring it.", eap_packet->code);
		return EAP_INVALID;
	}

	if ((eap_packet->data[0] <= 0) ||
	    (eap_packet->data[0] >= PW_EAP_MAX_TYPES)) {
		/*
		 *	Handle expanded types by smashing them to
		 *	normal types.
		 */
		if (eap_packet->data[0] == PW_EAP_EXPANDED_TYPE) {
			uint8_t *p, *q;

			if (len <= (EAP_HEADER_LEN + 1 + 3 + 4)) {
				RAUTH("Expanded EAP type is too short: ignoring the packet");
				return EAP_INVALID;
			}

			if ((eap_packet->data[1] != 0) ||
			    (eap_packet->data[2] != 0) ||
			    (eap_packet->data[3] != 0)) {
				RAUTH("Expanded EAP type has unknown Vendor-ID: ignoring the packet");
				return EAP_INVALID;
			}

			if ((eap_packet->data[4] != 0) ||
			    (eap_packet->data[5] != 0) ||
			    (eap_packet->data[6] != 0)) {
				RAUTH("Expanded EAP type has unknown Vendor-Type: ignoring the packet");
				return EAP_INVALID;
			}

			if ((eap_packet->data[7] == 0) ||
			    (eap_packet->data[7] >= PW_EAP_MAX_TYPES)) {
				RAUTH("Unsupported Expanded EAP type %s (%u): ignoring the packet",
				      eap_type2name(eap_packet->data[7]), eap_packet->data[7]);
				return EAP_INVALID;
			}

			if (eap_packet->data[7] == PW_EAP_NAK) {
				RAUTH("Unsupported Expanded EAP-NAK: ignoring the packet");
				return EAP_INVALID;
			}

			/*
			 *	Re-write the EAP packet to NOT have the expanded type.
			 */
			q = (uint8_t *) eap_packet;
			memmove(q + EAP_HEADER_LEN, q + EAP_HEADER_LEN + 7, len - 7 - EAP_HEADER_LEN);

			p = talloc_realloc(talloc_parent(eap_packet), eap_packet, uint8_t, len - 7);
			if (!p) {
				RAUTH("Unsupported EAP type %s (%u): ignoring the packet",
				      eap_type2name(eap_packet->data[0]), eap_packet->data[0]);
				return EAP_INVALID;
			}

			len -= 7;
			p[2] = (len >> 8) & 0xff;
			p[3] = len & 0xff;

			*eap_packet_p = (eap_packet_raw_t *) p;
			RWARN("Converting Expanded EAP to normal EAP.");
			RWARN("Unnecessary use of Expanded EAP types is not recommended.");

			return EAP_VALID;
		}

		RAUTH("Unsupported EAP type %s (%u): ignoring the packet",
		      eap_type2name(eap_packet->data[0]), eap_packet->data[0]);
		return EAP_INVALID;
	}

	/* we don't expect notification, but we send it */
	if (eap_packet->data[0] == PW_EAP_NOTIFICATION) {
		RAUTH("Got NOTIFICATION, "
			       "Ignoring the packet");
		return EAP_INVALID;
	}

	return EAP_VALID;
}


/*
 *  Get the user Identity only from EAP-Identity packets
 */
static char *eap_identity(REQUEST *request, eap_handler_t *handler, eap_packet_raw_t *eap_packet)
{
	int size;
	uint16_t len;
	char *identity;

	if ((!eap_packet) ||
	    (eap_packet->code != PW_EAP_RESPONSE) ||
	    (eap_packet->data[0] != PW_EAP_IDENTITY)) {
		return NULL;
	}

	memcpy(&len, eap_packet->length, sizeof(uint16_t));
	len = ntohs(len);

	if ((len <= 5) || (eap_packet->data[1] == 0x00)) {
		REDEBUG("EAP-Identity Unknown");
		return NULL;
	}

	if (len > 1024) {
		REDEBUG("EAP-Identity too long");
		return NULL;
	}

	size = len - 5;
	identity = talloc_array(handler, char, size + 1);
	memcpy(identity, &eap_packet->data[1], size);
	identity[size] = '\0';

	return identity;
}


/*
 *	Create our Request-Response data structure with the eap packet
 */
static EAP_DS *eap_buildds(eap_handler_t *handler,
			   eap_packet_raw_t **eap_packet_p)
{
	EAP_DS		*eap_ds = NULL;
	eap_packet_raw_t	*eap_packet = *eap_packet_p;
	int		typelen;
	uint16_t	len;

	if ((eap_ds = eap_ds_alloc(handler)) == NULL) {
		return NULL;
	}

	eap_ds->response->packet = (uint8_t *) eap_packet;
	(void) talloc_steal(eap_ds, eap_packet);
	eap_ds->response->code = eap_packet->code;
	eap_ds->response->id = eap_packet->id;
	eap_ds->response->type.num = eap_packet->data[0];

	memcpy(&len, eap_packet->length, sizeof(uint16_t));
	len = ntohs(len);
	eap_ds->response->length = len;

	/*
	 *	We've eaten the eap packet into the eap_ds.
	 */
	*eap_packet_p = NULL;

	/*
	 *	First 5 bytes in eap, are code + id + length(2) + type.
	 *
	 *	The rest is type-specific data.  We skip type while
	 *	getting typedata from data.
	 */
	typelen = len - 5/*code + id + length + type */;
	if (typelen > 0) {
		/*
		 *	Since the packet contains the complete
		 *	eap_packet, typedata will be a ptr in packet
		 *	to its typedata
		 */
		eap_ds->response->type.data = eap_ds->response->packet + 5/*code+id+length+type*/;
		eap_ds->response->type.length = typelen;
	} else {
		eap_ds->response->type.length = 0;
		eap_ds->response->type.data = NULL;
	}

	return eap_ds;
}


/*
 * If identity response then create a fresh handler & fill the identity
 * else handler MUST be in our list, get that.
 * This handler creation cannot fail
 *
 * username contains REQUEST->username which might have been stripped.
 * identity contains the one sent in EAP-Identity response
 */
eap_handler_t *eap_handler(rlm_eap_t *inst, eap_packet_raw_t **eap_packet_p,
			   REQUEST *request)
{
	eap_handler_t	*handler = NULL;
	eap_packet_raw_t *eap_packet;
	VALUE_PAIR	*vp;

	/*
	 *	Ensure it's a valid EAP-Request, or EAP-Response.
	 */
	if (eap_validation(request, eap_packet_p) == EAP_INVALID) {
	error:
		talloc_free(*eap_packet_p);
		*eap_packet_p = NULL;
		return NULL;
	}

	eap_packet = *eap_packet_p;

	/*
	 *	eap_handler_t MUST be found in the list if it is not
	 *	EAP-Identity response
	 */
	if (eap_packet->data[0] != PW_EAP_IDENTITY) {
		handler = eaplist_find(inst, request, eap_packet);
		if (!handler) {
			/* Either send EAP_Identity or EAP-Fail */
			RDEBUG("Either EAP-request timed out OR EAP-response to an unknown EAP-request");
			goto error;
		}

		/*
		 *	Even more paranoia.  Without this, some weird
		 *	clients could do crazy things.
		 *
		 *	It's ok to send EAP sub-type NAK in response
		 *	to a request for a particular type, but it's NOT
		 *	OK to blindly return data for another type.
		 */
		if ((eap_packet->data[0] != PW_EAP_NAK) &&
		    (eap_packet->data[0] != handler->type)) {
			RERROR("Response appears to match a previous request, but the EAP type is wrong");
			RERROR("We expected EAP type %s, but received type %s",
			       eap_type2name(handler->type),
			       eap_type2name(eap_packet->data[0]));
			RERROR("Your Supplicant or NAS is probably broken");
			goto error;
		}

	       vp = fr_pair_find_by_num(request->packet->vps, PW_USER_NAME, 0, TAG_ANY);
	       if (!vp) {
		       /*
			*	NAS did not set the User-Name
			*	attribute, so we set it here and
			*	prepend it to the beginning of the
			*	request vps so that autz's work
			*	correctly
			*/
		       RDEBUG2("Broken NAS did not set User-Name, setting from EAP Identity");
		       vp = fr_pair_make(request->packet, &request->packet->vps, "User-Name", handler->identity, T_OP_EQ);
		       if (!vp) {
			       goto error;
		       }
	       } else {
		       /*
			*      A little more paranoia.  If the NAS
			*      *did* set the User-Name, and it doesn't
			*      match the identity, (i.e. If they
			*      change their User-Name part way through
			*      the EAP transaction), then reject the
			*      request as the NAS is doing something
			*      funny.
			*/
		       if (strncmp(handler->identity, vp->vp_strvalue,
				   MAX_STRING_LEN) != 0) {
			       RDEBUG("Identity does not match User-Name.  Authentication failed");
			       goto error;
		       }
	       }
	} else {		/* packet was EAP identity */
		handler = eap_handler_alloc(inst, request);
		if (!handler) {
			goto error;
		}

		/*
		 *	All fields in the handler are set to zero.
		 */
		handler->identity = eap_identity(request, handler, eap_packet);
		if (!handler->identity) {
			if (!inst->allow_empty_identities) {
				RDEBUG("Identity Unknown, authentication failed");
			error2:
				talloc_free(handler);
				goto error;
			}

			handler->identity = "";
		}

	       vp = fr_pair_find_by_num(request->packet->vps, PW_USER_NAME, 0, TAG_ANY);
	       if (!vp) {
		       /*
			*	NAS did not set the User-Name
			*	attribute, so we set it here and
			*	prepend it to the beginning of the
			*	request vps so that autz's work
			*	correctly
			*/
		       RWDEBUG2("NAS did not set User-Name.  Setting it locally from EAP Identity");
		       vp = fr_pair_make(request->packet, &request->packet->vps, "User-Name", handler->identity, T_OP_EQ);
		       if (!vp) {
			       goto error2;
		       }
	       } else {
		       /*
			*      Paranoia.  If the NAS *did* set the
			*      User-Name, and it doesn't match the
			*      identity, the NAS is doing something
			*      funny, so reject the request.
			*/
		       if (strncmp(handler->identity, vp->vp_strvalue,
				   MAX_STRING_LEN) != 0) {
			       RDEBUG("Identity does not match User-Name, setting from EAP Identity");
			       goto error2;
		       }
	       }
	}

	handler->eap_ds = eap_buildds(handler, eap_packet_p);
	if (!handler->eap_ds) {
		goto error2;
	}

	handler->timestamp = request->timestamp;
	handler->request = request;
	return handler;
}
