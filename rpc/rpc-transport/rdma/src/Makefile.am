# TODO : need to change transportdir

transport_LTLIBRARIES = rdma.la
transportdir = $(libdir)/glusterfs/$(PACKAGE_VERSION)/rpc-transport

rdma_la_LDFLAGS = -module -avoid-version -shared

rdma_la_SOURCES = rdma.c name.c
rdma_la_LIBADD = $(top_builddir)/libglusterfs/src/libglusterfs.la     \
	-libverbs

noinst_HEADERS = rdma.h name.h
	-I$(top_srcdir)/libglusterfs/src -I$(top_srcdir)/rpc/rpc-lib/src/ \
	-I$(top_srcdir)/xlators/protocol/lib/src/ -shared -nostartfiles $(GF_CFLAGS)

AM_CFLAGS = -fPIC -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Wall -D$(GF_HOST_OS)  \
	-I$(top_srcdir)/libglusterfs/src -I$(top_srcdir)/rpc/rpc-lib/src/ \
	-I$(top_srcdir)/rpc/xdr/src -shared -nostartfiles $(GF_CFLAGS)

CLEANFILES = *~
