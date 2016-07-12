### Makefile.am
include $(top_builddir)/version.mk

AM_CFLAGS = $(EXTRA_CFLAGS)
AM_CPPFLAGS = -D_POSIX_C_SOURCE=201001L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
AM_CPPFLAGS += -DHAVE_VERSION_H
AM_LDFLAGS = $(XCCLDFLAGS)

bin_PROGRAMS =
noinst_PROGRAMS =
lib_LTLIBRARIES =
noinst_LTLIBRARIES =
pkglib_LTLIBRARIES =
noinst_HEADERS =
BUILT_SOURCES =
EXTRA_DIST = $(BUILT_SOURCES)
CLEANFILES = 
SUFFIXES =

CLEANFILES += version.c
EXTRA_DIST += version.c.in

noinst_LIBRARIES = libdfp.a
libdfp_a_SOURCES =
libdfp_a_SOURCES += dfp754_d32.c dfp754_d32.h
libdfp_a_SOURCES += dfp754_d64.c dfp754_d64.h
libdfp_a_CPPFLAGS = $(AM_CPPFLAGS)
libdfp_a_CPPFLAGS += $(dfp754_CFLAGS)

bin_PROGRAMS += candle
candle_SOURCES = candle.c candle.yuck
candle_SOURCES += version.c version.h
candle_CPPFLAGS = $(AM_CPPFLAGS)
candle_CPPFLAGS += -D__STDC_WANT_DEC_FP__
candle_CPPFLAGS += $(ev_CFLAGS)
candle_CPPFLAGS += $(dfp754_CFLAGS)
candle_CPPFLAGS += -DHAVE_VERSION_H
candle_LDFLAGS = $(AM_LDFLAGS)
candle_LDFLAGS += $(ev_LIBS)
candle_LDFLAGS += $(dfp754_LIBS)
candle_LDADD = libdfp.a
candle_LDADD += -ldecimal
BUILT_SOURCES += candle.yucc

bin_PROGRAMS += sex
sex_SOURCES = sex.c sex.yuck
sex_SOURCES += hash.c hash.h
sex_SOURCES += version.c version.h
sex_CPPFLAGS = $(AM_CPPFLAGS)
sex_CPPFLAGS += -D__STDC_WANT_DEC_FP__
sex_CPPFLAGS += $(dfp754_CFLAGS)
sex_CPPFLAGS += -DHAVE_VERSION_H
sex_LDFLAGS = $(AM_LDFLAGS)
sex_LDFLAGS += $(dfp754_LIBS)
sex_LDADD = libdfp.a
sex_LDADD += -ldecimal
BUILT_SOURCES += sex.yucc

bin_PROGRAMS += eva
eva_SOURCES = eva.c eva.yuck
eva_SOURCES += hash.c hash.h
eva_SOURCES += version.c version.h
eva_CPPFLAGS = $(AM_CPPFLAGS)
eva_CPPFLAGS += -D__STDC_WANT_DEC_FP__
eva_CPPFLAGS += $(dfp754_CFLAGS)
eva_CPPFLAGS += -DHAVE_VERSION_H
eva_LDFLAGS = $(AM_LDFLAGS)
eva_LDFLAGS += $(dfp754_LIBS)
eva_LDADD = libdfp.a
eva_LDADD += -ldecimal
BUILT_SOURCES += eva.yucc

bin_PROGRAMS += imp
imp_SOURCES = imp.c imp.yuck
imp_SOURCES += hash.c hash.h
imp_SOURCES += version.c version.h
imp_CPPFLAGS = $(AM_CPPFLAGS)
imp_CPPFLAGS += -D__STDC_WANT_DEC_FP__
imp_CPPFLAGS += $(dfp754_CFLAGS)
imp_CPPFLAGS += -DHAVE_VERSION_H
imp_LDFLAGS = $(AM_LDFLAGS)
imp_LDFLAGS += $(dfp754_LIBS)
imp_LDADD = libdfp.a
imp_LDADD += -ldecimal
BUILT_SOURCES += imp.yucc

bin_PROGRAMS += axp
axp_SOURCES = axp.c axp.yuck
axp_SOURCES += hash.c hash.h
axp_SOURCES += version.c version.h
axp_CPPFLAGS = $(AM_CPPFLAGS)
axp_CPPFLAGS += -D__STDC_WANT_DEC_FP__
axp_CPPFLAGS += $(dfp754_CFLAGS)
axp_CPPFLAGS += -DHAVE_VERSION_H
axp_LDFLAGS = $(AM_LDFLAGS)
axp_LDFLAGS += $(dfp754_LIBS)
axp_LDADD = libdfp.a
axp_LDADD += -ldecimal
BUILT_SOURCES += axp.yucc

bin_PROGRAMS += xevent
xevent_SOURCES = xevent.c xevent.yuck
xevent_SOURCES += version.c version.h
xevent_CPPFLAGS = $(AM_CPPFLAGS)
xevent_CPPFLAGS += -D_GNU_SOURCE
BUILT_SOURCES += xevent.yucc

bin_PROGRAMS += thours
thours_SOURCES = thours.c thours.yuck
thours_SOURCES += version.c version.h
thours_CPPFLAGS = $(AM_CPPFLAGS)
thours_CPPFLAGS += -D_GNU_SOURCE
BUILT_SOURCES += thours.yucc


## version rules
version.c: version.c.in $(top_builddir)/.version
	$(AM_V_GEN) PATH="$(top_builddir)/build-aux:$${PATH}" \
		yuck$(EXEEXT) scmver --ignore-noscm --force -o $@ \
			--use-reference --reference $(top_builddir)/.version $<

## yuck rule
SUFFIXES += .yuck
SUFFIXES += .yucc
.yuck.yucc:
	$(AM_V_GEN) PATH=$(top_builddir)/build-aux:"$${PATH}" \
		yuck$(EXEEXT) gen -o $@ $<

## Create preprocessor output (debugging purposes only)
.c.i:
	$(COMPILE) -E -o $@ $<

## Create assembler output (debugging purposes only)
.c.s:
	$(COMPILE) -S -c $(AM_CFLAGS) $<
