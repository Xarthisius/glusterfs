
xlator_LTLIBRARIES = error-gen.la
xlatordir = $(libdir)/glusterfs/$(PACKAGE_VERSION)/xlator/debug

error_gen_la_LDFLAGS = -module -avoid-version -shared

error_gen_la_SOURCES = error-gen.c
error_gen_la_LIBADD = $(top_builddir)/libglusterfs/src/libglusterfs.la

noinst_HEADERS = error-gen.h error-gen-mem-types.h

AM_CFLAGS = -fPIC -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Wall -D$(GF_HOST_OS)\
	-I$(top_srcdir)/libglusterfs/src -shared -nostartfiles $(GF_CFLAGS)

CLEANFILES = 

