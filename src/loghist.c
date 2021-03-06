#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <values.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <ieee754.h>
#include <math.h>
#include <tgmath.h>
#include "nifty.h"

typedef struct {
	double logn;
	double lambda;
	double pi;
} zip_t;

typedef struct {
	double logn;
	double shape;
	double rate;
} gamma_t;

typedef struct {
	double logn;
	double shape;
	/* log(1/scale) == -log(scale) */
	double rate;
} pareto_t;

static unsigned int highbits = 1U;

#if defined __INTEL_COMPILER
# pragma warning (disable:981)
#endif	/* __INTEL_COMPILER */


static __attribute__((format(printf, 1, 2))) void
serror(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}

static inline size_t
memncpy(void *restrict tgt, const void *src, size_t zrc)
{
	(void)memcpy(tgt, src, zrc);
	return zrc;
}

static ssize_t
dtostr(char *restrict buf, size_t bsz, double v)
{
	return snprintf(buf, bsz, "%.16g", v);
}

static ssize_t
zutostr(char *restrict buf, size_t bsz, size_t n)
{
	if (UNLIKELY(n == -1ULL)) {
		return memncpy(buf, "inf", 3U);
	}
	return snprintf(buf, bsz, "%zu", n);
}

static ssize_t(*ztostr)(char *restrict buf, size_t bsz, size_t n) = zutostr;

static size_t
zztostr(char *restrict buf, size_t bsz, const size_t *zv, size_t nz)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nz; i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, bsz - len, zv[i]);
	}
	return len;
}

static size_t
dvtostr(char *restrict buf, size_t bsz, const double *rv, size_t nv)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nv; i++) {
		buf[len++] = '\t';
		len += dtostr(buf + len, bsz - len, rv[i]);
	}
	return len;
}


static inline __attribute__((pure, const)) double
min_d(double v1, double v2)
{
	return v1 <= v2 ? v1 : v2;
}

static inline __attribute__((pure, const)) double
max_d(double v1, double v2)
{
	return v1 >= v2 ? v1 : v2;
}

static inline __attribute__((pure, const)) uint32_t
ilog2(const uint32_t x)
{
	return 31U - __builtin_clz(x);
}


static size_t ip, np;

/* stats */
static union {
	size_t start[0U];

#define MAKE_SLOTS(n)				\
	struct {				\
		size_t dlt[1U << (n)];		\
						\
		double tlo[1U << (n)];		\
		double thi[1U << (n)];		\
	} _##n

	MAKE_SLOTS(0);
	MAKE_SLOTS(5);
	MAKE_SLOTS(9);
	MAKE_SLOTS(13);
	MAKE_SLOTS(17);
	MAKE_SLOTS(21);
} cnt;

static size_t *dlt;
static double *tlo;
static double *thi;

static void(*prnt_cndl)(void);
static char buf[sizeof(cnt)];

static int
push_data(char *ln, size_t UNUSED(lz))
{
	static double v;
	char *on;
	double x;

	/* metronome is up first */
	if (UNLIKELY((x = strtod(ln, &on)) <= 0 || on == ln)) {
		return -1;
	} else if (v += x, LIKELY(ip++ < np)) {
		return 0;
	} else {
		/* dissect value*/
		unsigned int slot = ilog2(lrint(v) + 1U);
		/* determine sub slot, if applicable */
		unsigned int width = (1U << slot), base = width - 1U;
		unsigned int subs;

		{
			/* translate in terms of base */
			subs = lrint((v - base) * 1000) << (highbits - 5U);
			/* divide by width */
			subs /= width;
			subs /= 1000U;

			slot <<= highbits;
			slot >>= 5U;
			slot ^= subs;
		}
		/* poisson fit */
		dlt[slot]++;

		tlo[slot] = min_d(tlo[slot], v);
		thi[slot] = max_d(thi[slot], v);
	}
	/* and store state */
	v = 0;
	ip = 0U;
	return 0;
}


/* printers */
static void
prnt_cndl_mtrx(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (!ncndl++) {
		static const char hdr[] = "metric";

		len = memncpy(buf, hdr, strlenof(hdr));
		for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
			buf[len++] = '\t';
			buf[len++] = 'v';
			len += ztostr(buf + len, sizeof(buf) - len, i);
		}
		buf[len++] = '\n';
		fwrite(buf, sizeof(*buf), len, stdout);
		len = 0U;
	}

	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += dvtostr(buf + len, sizeof(buf) - len, tlo, 1U << highbits);
	buf[len++] = '\n';

	/* type t */
	buf[len++] = 'H';
	len += dvtostr(buf + len, sizeof(buf) - len, thi, 1U << highbits);
	buf[len++] = '\n';

	/* count data */
	len += memncpy(buf + len, "cnt", strlenof("cnt"));
	len += zztostr(buf + len, sizeof(buf) - len, dlt, 1U << highbits);
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_molt(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (!ncndl++) {
		static const char hdr[] = "lo\thi\tcnt\n";
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
	}

	/* delta t */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		len += dtostr(buf + len, sizeof(buf) - len, tlo[i]);
		buf[len++] = '\t';
		len += dtostr(buf + len, sizeof(buf) - len, thi[i]);
		buf[len++] = '\t';
		len += zutostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}


#include "loghist.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = EXIT_FAILURE;
		goto out;
	}

	if (argi->occurrences_arg) {
		if (!(np = strtoul(argi->occurrences_arg, NULL, 10))) {
			errno = 0, serror("\
Error: occurrences must be positive.");
			return -1;
		}
		/* we need it off-by-one */
		np--;
	}
	/* set candle printer */
	prnt_cndl = !argi->table_flag ? prnt_cndl_molt : prnt_cndl_mtrx;

	/* set resolution */
	highbits = (argi->verbose_flag << 2U) ^ (argi->verbose_flag > 0U);

	switch (highbits) {
#define ASS_PTRS(n)				\
		dlt = cnt._##n.dlt;		\
						\
		tlo = cnt._##n.tlo;		\
		thi = cnt._##n.thi;		\

	case 0U:
		ASS_PTRS(0);
		break;

	case 5U:
		ASS_PTRS(5);
		break;

	case 9U:
		ASS_PTRS(9);
		break;

	case 13U:
		ASS_PTRS(13);
		break;

	case 17U:
		ASS_PTRS(17);
		break;

	case 21U:
		ASS_PTRS(21);
		break;

	default:
		errno = 0, serror("\
Error: verbose flag can only be used one to five times.");
		return -1;
	}
	/* massage tlo/thi */
	memset(tlo, -1, (1U << highbits) * sizeof(*tlo));
	memset(thi, -1, (1U << highbits) * sizeof(*thi));

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			(void)push_data(line, nrd);
		}
		/* finalise our findings */
		free(line);

		/* print the final candle */
		prnt_cndl();
	}

out:
	yuck_free(argi);
	return rc;
}
