bin_PROGRAMS = memc3 
pkginclude_HEADERS = protocol_binary.h
noinst_PROGRAMS = sizes testapp timedrun

BUILT_SOURCES=

testapp_SOURCES = testapp.c util.c util.h

timedrun_SOURCES = timedrun.c

memc3_SOURCES = memcached.c memcached.h \
                hash.c hash.h \
                slabs.c slabs.h \
                items.c items.h \
                assoc_chain.c assoc_chain.h \
                assoc_cuckoo.c assoc_cuckoo.h \
                thread.c daemon.c \
                stats.c stats.h \
                util.c util.h \
                trace.h cache.h sasl_defs.h \
                memc3_util.c memc3_util.h

if BUILD_CACHE
memc3_SOURCES += cache.c
testapp_SOURCES += cache.c
endif

if BUILD_SOLARIS_PRIVS
memc3_SOURCES += solaris_priv.c
endif

if ENABLE_SASL
memc3_SOURCES += sasl_defs.c
endif

memc3_debug_SOURCES = $(memc3_SOURCES)
memc3_CPPFLAGS = 
memc3_debug_LDADD = @PROFILER_LDFLAGS@
memc3_debug_CFLAGS = @PROFILER_FLAGS@

memc3_LDADD =
memc3_DEPENDENCIES =
memc3_debug_DEPENDENCIES =
CLEANFILES=

SUBDIRS = doc
DIST_DIRS = scripts
EXTRA_DIST = doc scripts t memcached.spec memcached_dtrace.d version.m4

MOSTLYCLEANFILES = *.gcov *.gcno *.gcda *.tcov

test:	memc3-debug sizes testapp
	$(srcdir)/sizes
	$(srcdir)/testapp
	prove $(srcdir)/t
	@if test `basename $(PROFILER)` = "gcov"; then \
	  for file in memc3_debug-*.gc??; do \
	    mv -f $$file `echo $$file | sed 's/memc3_debug-//'`; \
	  done && \
	  for file in *.gcda; do \
	    srcfile=`echo $$file | sed 's/.gcda/.c/'`; \
	    if test -n "`echo $(memc3_debug_SOURCES) | grep $$srcfile`"; then \
	      echo `$(PROFILER) $$srcfile` | sed 's/'$$srcfile':.*//'; \
	    fi \
	  done \
	elif test `basename $(PROFILER)` = "tcov"; then \
	  files=`grep SRCFILE memc3-debug.profile/tcovd | sed 's/SRCFILE://' | sort | uniq` && \
	  $(PROFILER) -x memc3-debug.profile $$files 2>&1; \
	  for file in *.tcov; do \
	    srcfile=`echo $$file | sed 's/.tcov//'`; \
	    if test -n "`echo $(memc3_debug_SOURCES) | grep $$srcfile`"; then \
	      echo $$srcfile : `grep 'Percent of the file executed' $$file`; \
	    fi \
	  done \
	else :; fi

docs:
	(cat Doxyfile ; echo "PROJECT_NUMBER=`cat version.num`") | doxygen -

dist-hook:
	rm -f $(distdir)/*/*~ $(distdir)/t/lib/*~ $(distdir)/*~
