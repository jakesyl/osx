## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = 1.4

INCLUDES = @X_CFLAGS@ \
	$(XAW_INCLUDES)

LIBS =

noinst_HEADERS = SFinternal.h SFDecls.h xstat.h

if WITH_X
noinst_LTLIBRARIES = libfilereq.la
endif

libfilereq_la_SOURCES = Dir.c Draw.c Path.c SelFile.c $(noinst_HEADERS)

libfilereq_la_LIBADD = $(XAW_LIBS)

EXTRA_DIST = README.selfile
