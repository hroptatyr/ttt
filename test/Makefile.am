# Help the Developers and yourself. Just use the C locale and settings
# for the compilation. They can still be overriden by make LANG=<whatever>
# but that is general a not very good idea
include $(top_builddir)/version.mk

LANG = C
LC_ALL = C

AM_CFLAGS = $(EXTRA_CFLAGS)
AM_CPPFLAGS = -D_POSIX_C_SOURCE=200112L -D_XOPEN_SOURCE=600 -D_BSD_SOURCE
AM_CPPFLAGS += -DTEST

EXTRA_DIST = $(BUILT_SOURCES) $(TESTS)
TESTS =
TEST_EXTENSIONS =
BUILT_SOURCES =
check_PROGRAMS =
CLEANFILES = $(check_PROGRAMS)

## summon our helper
include clitoris.am
AM_CLIT_LOG_FLAGS = -v --builddir $(top_builddir)/src
TESTS_ENVIRONMENT = root=$(top_srcdir)

TESTS += sex_01.clit
TESTS += sex_02.clit
TESTS += sex_03.clit
TESTS += sex_04.clit
TESTS += sex_05.clit
TESTS += sex_06.clit
EXTRA_DIST += EURUSD

TESTS += opt_01.clit
TESTS += opt_02.clit
EXTRA_DIST += EURUSD.s

TESTS += accsum_01.clit
TESTS += accsum_02.clit
TESTS += accsum_03.clit
TESTS += accsum_04.clit
EXTRA_DIST += test1.acc
EXTRA_DIST += test2.acc

TESTS += snap_01.clit
TESTS += snap_02.clit
TESTS += snap_03.clit
TESTS += snap_04.clit
EXTRA_DIST += EURUSD

TESTS += mid_01.clit
TESTS += mid_02.clit
TESTS += mid_03.clit
TESTS += mid_04.clit
EXTRA_DIST += EURUSD

TESTS += candle_01.clit
TESTS += candle_02.clit
TESTS += candle_03.clit
EXTRA_DIST += EURUSD

TESTS += quodist_01.clit
TESTS += quodist_02.clit
TESTS += quodist_03.clit
TESTS += quodist_04.clit
TESTS += quodist_05.clit
TESTS += quodist_06.clit
EXTRA_DIST += EURUSD

## Makefile.am ends here
