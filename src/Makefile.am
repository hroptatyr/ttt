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
candle_CPPFLAGS += $(dfp754_CFLAGS)
candle_CPPFLAGS += -DHAVE_VERSION_H
candle_LDFLAGS = $(AM_LDFLAGS)
candle_LDFLAGS += $(dfp754_LIBS)
candle_LDADD = libdfp.a
candle_LDADD += -ldecimal
BUILT_SOURCES += candle.yucc

bin_PROGRAMS += snap
snap_SOURCES = snap.c snap.yuck
snap_SOURCES += version.c version.h
snap_CPPFLAGS = $(AM_CPPFLAGS)
snap_CPPFLAGS += -D__STDC_WANT_DEC_FP__
snap_CPPFLAGS += $(dfp754_CFLAGS)
snap_CPPFLAGS += -DHAVE_VERSION_H
snap_LDFLAGS = $(AM_LDFLAGS)
snap_LDFLAGS += $(dfp754_LIBS)
snap_LDADD = libdfp.a
snap_LDADD += -ldecimal
BUILT_SOURCES += snap.yucc

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

bin_PROGRAMS += mid
mid_SOURCES = mid.c mid.yuck
mid_SOURCES += version.c version.h
mid_CPPFLAGS = $(AM_CPPFLAGS)
mid_CPPFLAGS += -D__STDC_WANT_DEC_FP__
mid_CPPFLAGS += $(dfp754_CFLAGS)
mid_CPPFLAGS += -DHAVE_VERSION_H
mid_LDFLAGS = $(AM_LDFLAGS)
mid_LDFLAGS += $(dfp754_LIBS)
mid_LDADD = libdfp.a
mid_LDADD += -ldecimal
BUILT_SOURCES += mid.yucc

bin_PROGRAMS += qq
qq_SOURCES = qq.c qq.yuck
qq_SOURCES += version.c version.h
qq_CPPFLAGS = $(AM_CPPFLAGS)
qq_CPPFLAGS += -D__STDC_WANT_DEC_FP__
qq_CPPFLAGS += $(dfp754_CFLAGS)
qq_CPPFLAGS += -DHAVE_VERSION_H
qq_LDFLAGS = $(AM_LDFLAGS)
qq_LDFLAGS += $(dfp754_LIBS)
qq_LDADD = libdfp.a
qq_LDADD += -ldecimal
BUILT_SOURCES += qq.yucc

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

bin_PROGRAMS += sma
sma_SOURCES = sma.c sma.yuck
sma_SOURCES += version.c version.h
sma_CPPFLAGS = $(AM_CPPFLAGS)
sma_CPPFLAGS += -D_GNU_SOURCE
sma_CPPFLAGS += -D__STDC_WANT_DEC_FP__
sma_CPPFLAGS += $(dfp754_CFLAGS)
sma_CPPFLAGS += -DHAVE_VERSION_H
sma_LDFLAGS = $(AM_LDFLAGS)
sma_LDFLAGS += $(dfp754_LIBS)
sma_LDADD = libdfp.a
sma_LDADD += -ldecimal
BUILT_SOURCES += sma.yucc

bin_PROGRAMS += opt
opt_SOURCES = opt.c opt.yuck
opt_SOURCES += hash.c hash.h
opt_SOURCES += version.c version.h
opt_CPPFLAGS = $(AM_CPPFLAGS)
opt_CPPFLAGS += -D__STDC_WANT_DEC_FP__
opt_CPPFLAGS += $(dfp754_CFLAGS)
opt_CPPFLAGS += -DHAVE_VERSION_H
opt_LDFLAGS = $(AM_LDFLAGS)
opt_LDFLAGS += $(dfp754_LIBS)
opt_LDADD = libdfp.a
opt_LDADD += -ldecimal
BUILT_SOURCES += opt.yucc

bin_PROGRAMS += fra
fra_SOURCES = fra.c fra.yuck
fra_SOURCES += hash.c hash.h
fra_SOURCES += version.c version.h
fra_CPPFLAGS = $(AM_CPPFLAGS)
fra_CPPFLAGS += -D__STDC_WANT_DEC_FP__
fra_CPPFLAGS += $(dfp754_CFLAGS)
fra_CPPFLAGS += -DHAVE_VERSION_H
fra_LDFLAGS = $(AM_LDFLAGS)
fra_LDFLAGS += $(dfp754_LIBS)
fra_LDADD = libdfp.a
fra_LDADD += -ldecimal
BUILT_SOURCES += fra.yucc


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
