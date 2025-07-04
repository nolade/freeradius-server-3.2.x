#
#  Makefile
#
#		NOTE: This top-level Makefile must not
#		use GNU-make extensions. The lower ones can.
#
#  Version:	$Id$
#

#
#  If we didn't call ./configure just define the version.
#
RADIUSD_VERSION_STRING := $(shell cat VERSION)

#
#  The default rule is "all".
#
all:

#
#  Catch people who try to use BSD make
#
ifeq "0" "1"
.error GNU Make is required to build FreeRADIUS
endif

#
#  We require Make.inc, UNLESS the target is "make deb"
#
#  Since "make deb" re-runs configure... there's no point in
#  requiring the developer to run configure *before* making
#  the debian packages.
#
ifneq "$(MAKECMDGOALS)" "deb"
ifneq "$(MAKECMDGOALS)" "rpm"
ifeq "$(findstring docker,$(MAKECMDGOALS))" ""
ifeq "$(findstring crossbuild,$(MAKECMDGOALS))" ""
$(if $(wildcard Make.inc),,$(error Missing 'Make.inc' Run './configure [options]' and retry))

include Make.inc
endif
endif
endif
endif

MFLAGS += --no-print-directory

#
#  The version of GNU Make is too old, don't use it (.FEATURES
#  variable was added in 3.81)
#
ifndef .FEATURES
$(error The build system requires GNU Make 3.81 or later.)
endif

export DESTDIR := $(R)

#
#  And over-ride all of the other magic.
#
ifneq "$(MAKECMDGOALS)" "deb"
ifneq "$(MAKECMDGOALS)" "rpm"
ifeq "$(findstring docker,$(MAKECMDGOALS))" ""
ifeq "$(findstring crossbuild,$(MAKECMDGOALS))" ""
include scripts/boiler.mk
endif
endif
endif
endif

#
#  To work around OpenSSL issues within CI.
#
.PHONY:
raddb/test.conf:
	@echo 'security {' >> $@
	@echo '        allow_vulnerable_openssl = yes' >> $@
	@echo '}' >> $@
	@echo '$$INCLUDE radiusd.conf' >> $@

#
#  Run "radiusd -C", looking for errors.
#
#  Only redirect STDOUT, which should contain details of why the test failed.
#  Don't molest STDERR as this may be used to receive output from a debugger.
#
$(BUILD_DIR)/tests/radiusd-c: raddb/test.conf ${BUILD_DIR}/bin/radiusd | build.raddb
	${Q}$(MAKE) -C raddb/certs
	${Q}printf "radiusd -C... "
	${Q}if ! $(TESTBIN)/radiusd -XCMd ./raddb -D ./share -n test > $(BUILD_DIR)/tests/radiusd.config.log; then \
		rm -f raddb/test.conf; \
		cat $(BUILD_DIR)/tests/radiusd.config.log; \
		echo "fail"; \
		exit 1; \
	fi
	${Q}rm -f raddb/test.conf
	${Q}echo "ok"
	${Q}touch $@

test: ${BUILD_DIR}/bin/radiusd ${BUILD_DIR}/bin/radclient tests.unit tests.xlat tests.keywords tests.modules tests.auth test.sql_nas_table $(BUILD_DIR)/tests/radiusd-c | build.raddb
	${Q}$(MAKE) -C src/tests tests

#
#  Tests specifically for CI. We do a LOT more than just
#  the above tests
#
ci-test: raddb/test.conf test
	${Q}$(TESTBIN)/radiusd -xxxv -n test
	${Q}rm -f raddb/test.conf
	${Q}$(MAKE) install
	${Q}perl -p -i -e 's/allow_vulnerable_openssl = no/allow_vulnerable_openssl = yes/' ${raddbdir}/radiusd.conf
	${Q}sh ${HOME}/freeradius/etc/raddb/certs
	${Q}${sbindir}/radiusd -XC

#
#  The $(R) is a magic variable not defined anywhere in this source.
#  It's purpose is to allow an admin to create an installation 'tar'
#  file *without* actually installing it.  e.g.:
#
#  $ R=/home/root/tmp make install
#  $ cd /home/root/tmp
#  $ tar -cf ~/freeradius-package.tar *
#
#  The 'tar' file can then be un-tar'd on any similar machine.  It's a
#  cheap way of creating packages, without using a package manager.
#  Many of the platform-specific packaging tools use the $(R) variable
#  when creating their packages.
#
#  For compatibility with typical GNU packages (e.g. as seen in libltdl),
#  we make sure DESTDIR is defined.
#
export DESTDIR := $(R)

DICTIONARIES := $(wildcard share/dictionary*)
install.share: $(addprefix $(R)$(dictdir)/,$(notdir $(DICTIONARIES)))

.PHONY: dictionary.format
dictionary.format: $(DICTIONARIES)
	${Q}./share/format.pl $(DICTIONARIES)

$(R)$(dictdir)/%: share/%
	@echo INSTALL $(notdir $<)
	${Q}$(INSTALL) -m 644 $< $@

MANFILES := $(filter-out $(MANSKIP),$(wildcard man/man*/*.?))
MANDIR   := $(wildcard man/man*)
install.man: $(subst man/,$(R)$(mandir)/,$(MANFILES))

$(MANDIR):
	${Q}echo INSTALL $(patsubst $(R)$(mandir)/%,man/%,$@)
	${Q}$(INSTALL) -d -m 755 $@

$(R)$(mandir)/%: man/% | $(dir $@)
	${Q}echo INSTALL $(notdir $<)
	${Q}sed -e "s,/etc/raddb,$(raddbdir),g" \
		-e "s,/usr/local/share,$(datarootdir),g" \
		$< > $<.subst
	${Q}$(INSTALL) -m 644 $<.subst $@
	${Q}rm $<.subst

#
#  Don't install rlm_test
#
ALL_INSTALL := $(patsubst %rlm_test.la,,$(ALL_INSTALL))

install: install.share install.man
	${Q}$(INSTALL) -d -m 700	$(R)$(logdir)
	${Q}$(INSTALL) -d -m 700	$(R)$(radacctdir)

ifneq ($(RADMIN),)
ifneq ($(RGROUP),)
.PHONY: install-chown
install-chown:
	chown -R $(RADMIN)   $(R)$(raddbdir)
	chgrp -R $(RGROUP)   $(R)$(raddbdir)
	chmod u=rwx,g=rx,o=  `find $(R)$(raddbdir) -type d -print`
	chmod u=rw,g=r,o=    `find $(R)$(raddbdir) -type f -print`
	chown -R $(RADMIN)   $(R)$(logdir)
	chgrp -R $(RGROUP)   $(R)$(logdir)
	find $(R)$(logdir) -type d -exec chmod u=rwx,g=rwx,o= {} \;
	find $(R)$(logdir) -type d -exec chmod g+s {} \;
	find $(R)$(logdir) -type f -exec chmod u=rw,g=rw,o= {} \;
	chown -R $(RADMIN)   $(R)$(RUNDIR)
	chgrp -R $(RGROUP)   $(R)$(RUNDIR)
	find $(R)$(RUNDIR) -type d -exec chmod u=rwx,g=rwx,o= {} \;
	find $(R)$(RUNDIR) -type d -exec chmod g+s {} \;
	find $(R)$(RUNDIR) -type f -exec chmod u=rw,g=rw,o= {} \;
endif
endif

distclean: clean
	@-find src/modules -regex .\*/config[.][^.]*\$$ -delete
	@-find src/modules -name autom4te.cache -exec rm -rf '{}' \;
	@rm -rf config.cache config.log config.status libtool \
		src/include/radpaths.h src/include/stamp-h \
		libltdl/config.log libltdl/config.status \
		libltdl/libtool autom4te.cache build
	@-find . ! -name configure.ac -name \*.in -print | \
		sed 's/\.in$$//' | \
		while read file; do rm -f $$file; done

######################################################################
#
#  Automatic remaking rules suggested by info:autoconf#Automatic_Remaking
#
######################################################################
#
#  Do these checks ONLY if we're re-building the "configure"
#  scripts, and ONLY the "configure" scripts.  If we leave
#  these rules enabled by default, then they're run too often.
#
ifeq "$(MAKECMDGOALS)" "reconfig"

CONFIGURE_AC_FILES := $(shell find . -name configure.ac -print)
CONFIGURE_FILES	   := $(patsubst %.ac,%,$(CONFIGURE_AC_FILES))

#
#  The GNU tools make autoconf=="missing autoconf", which then returns
#  0, even when autoconf doesn't exist.  This check is to ensure that
#  we run AUTOCONF only when it exists.
#
AUTOCONF_EXISTS := $(shell autoconf --version 2>/dev/null)

ifeq "$(AUTOCONF_EXISTS)" ""
$(error You need to install autoconf to re-build the "configure" scripts)
endif

#  Configure files depend on ".ac" files, and on the top-level macro files
#  If there are headers, run auto-header, too.
src/%configure: src/%configure.ac acinclude.m4 aclocal.m4 $(wildcard $(dir $@)m4/*m4) | src/freeradius-devel
	@echo AUTOCONF $(dir $@)
	${Q}cd $(dir $@) && \
		$(ACLOCAL) --force -I $(top_builddir) -I $(top_builddir)/m4 && \
		$(AUTOCONF) --force
	${Q}if grep AC_CONFIG_HEADERS $@ >/dev/null; then\
		echo AUTOHEADER $@ \
		cd $(dir $@) && $(AUTOHEADER) --force; \
	 fi

#  "%configure" doesn't match "configure"
configure: configure.ac $(wildcard ac*.m4) $(wildcard m4/*.m4)
	@echo AUTOCONF $@
	@$(ACLOCAL) --force
	@$(AUTOCONF) --force

src/include/autoconf.h.in: configure.ac
	@echo AUTOHEADER $@
	@$(AUTOHEADER) --force

reconfig: $(CONFIGURE_FILES) src/include/autoconf.h.in

config.status: configure
	./config.status --recheck

#  target is "reconfig"
endif

#
#  If we've already run configure, then add rules which cause the
#  module-specific "all.mk" files to depend on the mk.in files, and on
#  the configure script.
#
ifneq "$(wildcard config.log)" ""
CONFIGURE_ARGS	   := $(shell head -10 config.log | grep '^  \$$' | sed 's/^....//;s:.*configure ::')

src/%all.mk: src/%all.mk.in src/%configure
	${Q}echo CONFIGURE $(dir $@)
	${Q}rm -f ./config.cache $(dir $<)/config.cache
	${Q}cd $(dir $<) && ./configure $(CONFIGURE_ARGS) && touch $(notdir $@)
endif

.PHONY: check-includes
check-includes:
	scripts/min-includes.pl `find . -name "*.c" -print`

.PHONY: TAGS
TAGS:
	etags `find src -type f -name '*.[ch]' -print` > $@

#
#  Make test certificates.
#
.PHONY: certs
certs:
	${Q}$(MAKE) -C raddb/certs

######################################################################
#
#  Make a release.
#
#  Note that "Make.inc" has to be updated with the release number
#  BEFORE running this command!
#
######################################################################
BRANCH = $(shell git rev-parse --abbrev-ref HEAD)

freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz: .git/HEAD
	git archive --format=tar --prefix=freeradius-server-$(RADIUSD_VERSION_STRING)/ $(BRANCH) | gzip > $@

freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2: .git/HEAD
	git archive --format=tar --prefix=freeradius-server-$(RADIUSD_VERSION_STRING)/ $(BRANCH) | bzip2 > $@

%.sig: %
	gpg --local-user packages@freeradius.org -b $<

#
#  High-level targets
#
.PHONY: dist-check
dist-check: redhat/freeradius.spec suse/freeradius.spec debian/changelog
	${Q}if [ `grep ^Version: redhat/freeradius.spec | sed 's/.*://;s/ //g'` != "$(RADIUSD_VERSION_STRING)" ]; then \
		cat redhat/freeradius.spec | sed 's/^Version:.*/Version: $(RADIUSD_VERSION_STRING)/' > redhat/.foo; \
		mv redhat/.foo redhat/freeradius.spec; \
		echo redhat/freeradius.spec 'Version' needs to be updated; \
		exit 1; \
	fi
	${Q}if [ `grep ^Version: suse/freeradius.spec | sed 's/.*://;s/ //g'` != "$(RADIUSD_VERSION_STRING)" ]; then \
		cat suse/freeradius.spec | sed 's/^Version: .*/Version:      $(RADIUSD_VERSION_STRING)/' > suse/.foo; \
		mv suse/.foo suse/freeradius.spec; \
		echo suse/freeradius.spec 'Version' needs to be updated; \
		exit 1; \
	fi
	${Q}if [ `head -n 1 doc/ChangeLog | awk '/^FreeRADIUS/{print $$2}'` != "$(RADIUSD_VERSION_STRING)" ]; then \
		echo doc/ChangeLog needs to be updated; \
		exit 1; \
	fi
	${Q}if [ `head -n 1 debian/changelog | sed 's/.*(//;s/-0).*//;s/-1).*//;s/\+.*//'` != "$(RADIUSD_VERSION_STRING)" ]; then \
		echo debian/changelog needs to be updated; \
		exit 1; \
	fi
	${Q}if [ `grep version doc/antora/antora.yml | sed 's/^.*version: //'` != "'$(RADIUSD_VERSION_STRING)'" ]; then \
		echo doc/antora/antora.yml needs to be updated with: version '$(RADIUSD_VERSION_STRING)'; \
		exit 1; \
	fi

dist: dist-check freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2

dist-sign: dist-check freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz.sig freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2.sig

dist-publish: freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz.sig freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz.sig freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2 freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz.sig freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2.sig
	scp $^ freeradius.org@ftp.freeradius.org:public_ftp

#
#  Note that we do NOT do the tagging here!  We just print out what
#  to do!
#
dist-tag: freeradius-server-$(RADIUSD_VERSION_STRING).tar.gz freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2
	${Q}echo "git tag release_`echo $(RADIUSD_VERSION_STRING) | tr .- __`"

#
#  Docker-related targets (main Docker images and crossbuild)
#
ifneq "$(findstring docker,$(MAKECMDGOALS))" ""
include scripts/docker/docker.mk
endif

ifneq "$(findstring crossbuild,$(MAKECMDGOALS))" ""
include scripts/crossbuild/crossbuild.mk
endif

#
#  Build a Debian package
#
DEBBUILDEXTRA = --jobs=auto
.PHONY: deb
deb:
	fakeroot debian/rules debian/control #clean
	fakeroot dpkg-buildpackage $(DEBBUILDEXTRA) -b -uc

#
#  Build an RPM package
#
.PHONY: rpm
rpmbuild/SOURCES/freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2: freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2
	${Q}mkdir -p $(addprefix rpmbuild/,SOURCES SPECS BUILD RPMS SRPMS BUILDROOT)
	${Q}for file in `awk '/^Source...:/ {print $$2}' redhat/freeradius.spec` ; do cp redhat/$$file rpmbuild/SOURCES/$$file ; done
	${Q}cp $< $@

rpm: rpmbuild/SOURCES/freeradius-server-$(RADIUSD_VERSION_STRING).tar.bz2
	${Q}if ! yum-builddep -q -C --assumeno redhat/freeradius.spec 1> /dev/null 2>&1; then \
		echo "ERROR: Required depdendencies not found, install them with: yum-builddep redhat/freeradius.spec"; \
		exit 1; \
	fi
	${Q}QA_RPATHS=0x0003 rpmbuild --define "_topdir `pwd`/rpmbuild" -bb $(RPMBUILD_FLAGS) redhat/freeradius.spec

#
#  Developer checks
#
.PHONY: warnings
warnings:
	${Q}(make clean all 2>&1) | egrep -v '^/|deprecated|^In file included|: In function|   from |^HEADER|^CC|^LN|^LINK' > warnings.txt
	${Q}@wc -l warnings.txt

#
#  Ensure we're using tabs in the configuration files,
#  and remove trailing whitespace in source files.
#
.PHONY: whitespace
whitespace:
	${Q}for x in $$(git ls-files raddb/ src/); do unexpand $$x > $$x.bak; cp $$x.bak $$x; rm -f $$x.bak;done
	${Q}perl -p -i -e 'trim' $$(git ls-files src/)
