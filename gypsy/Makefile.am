lib_LTLIBRARIES = libgypsy.la

BUILT_SOURCES = 		\
	gypsy-marshal.c		\
	gypsy-marshal.h		\
	gypsy-server-bindings.h	\
	gypsy-client-bindings.h \
	gypsy-discovery-bindings.h

libgypsy_la_SOURCES = 		\
	$(BUILT_SOURCES)	\
	$(gypsy_headers)	\
	gypsy-accuracy.c	\
	gypsy-control.c		\
	gypsy-course.c		\
	gypsy-device.c		\
	gypsy-discovery.c	\
	gypsy-position.c	\
	gypsy-satellite.c	\
	gypsy-time.c

libgypsy_la_CFLAGS =		\
	-I$(top_srcdir)		\
	-I$(top_builddir)	\
	$(GYPSY_CFLAGS)

libgypsy_la_LIBADD = \
	$(GYPSY_LIBS)

gypsy_headers =			\
	gypsy-accuracy.h	\
	gypsy-control.h		\
	gypsy-course.h		\
	gypsy-device.h		\
	gypsy-discovery.h	\
	gypsy-position.h	\
	gypsy-satellite.h	\
	gypsy-time.h

libgypsy_includedir = $(includedir)/gypsy
libgypsy_include_HEADERS =	\
	$(gypsy_headers)

EXTRA_DIST = gypsy-marshal.list
CLEANFILES = $(BUILT_SOURCES)

%-bindings.h: ../interfaces/%.xml
	$(AM_V_GEN)$(DBUS_BINDING_TOOL) --mode=glib-client --prefix=gypsy $< > $@

gypsy-marshal.h: $(srcdir)/gypsy-marshal.list $(GLIB_GENMARSHAL)
	$(AM_V_GEN)$(GLIB_GENMARSHAL) $< --header --prefix=gypsy_marshal > $@
gypsy-marshal.c: $(srcdir)/gypsy-marshal.list gypsy-marshal.h $(GLIB_GENMARSHAL)
	$(AM_V_GEN)echo "#include \"gypsy-marshal.h\"" > $@ \
	&& $(GLIB_GENMARSHAL) --prefix=gypsy_marshal $(srcdir)/gypsy-marshal.list --body >> $@

