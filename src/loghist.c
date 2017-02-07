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

static inline double
log1pexpf(double x)
{
	if (x <= 18) {
		return log(1 + exp(x));
	} else if (x > 33.3) {
		return x;
	}
	return x + exp(-x);
}

static double
stirlerr(double n)
{
	static const double S0 = 0.083333333333333333333/* 1/12 */;
	static const double S1 = 0.00277777777777777777778/* 1/360 */;
	static const double S2 = 0.00079365079365079365079365/* 1/1260 */;
	static const double S3 = 0.000595238095238095238095238/* 1/1680 */;
	static const double S4 = 0.0008417508417508417508417508/* 1/1188 */;
	/* error for 0, 0.5, 1.0, 1.5, ..., 14.5, 15.0. */
	static const double sferr_halves[31] = {
		0.0, /* n=0 - wrong, place holder only */
		0.1534264097200273452913848, /* 0.5 */
		0.0810614667953272582196702, /* 1.0 */
		0.0548141210519176538961390, /* 1.5 */
		0.0413406959554092940938221, /* 2.0 */
		0.03316287351993628748511048, /* 2.5 */
		0.02767792568499833914878929, /* 3.0 */
		0.02374616365629749597132920, /* 3.5 */
		0.02079067210376509311152277, /* 4.0 */
		0.01848845053267318523077934, /* 4.5 */
		0.01664469118982119216319487, /* 5.0 */
		0.01513497322191737887351255, /* 5.5 */
		0.01387612882307074799874573, /* 6.0 */
		0.01281046524292022692424986, /* 6.5 */
		0.01189670994589177009505572, /* 7.0 */
		0.01110455975820691732662991, /* 7.5 */
		0.010411265261972096497478567, /* 8.0 */
		0.009799416126158803298389475, /* 8.5 */
		0.009255462182712732917728637, /* 9.0 */
		0.008768700134139385462952823, /* 9.5 */
		0.008330563433362871256469318, /* 10.0 */
		0.007934114564314020547248100, /* 10.5 */
		0.007573675487951840794972024, /* 11.0 */
		0.007244554301320383179543912, /* 11.5 */
		0.006942840107209529865664152, /* 12.0 */
		0.006665247032707682442354394, /* 12.5 */
		0.006408994188004207068439631, /* 13.0 */
		0.006171712263039457647532867, /* 13.5 */
		0.005951370112758847735624416, /* 14.0 */
		0.005746216513010115682023589, /* 14.5 */
		0.005554733551962801371038690 /* 15.0 */
	};
	/* log(sqrt(2*pi)) */
	static const double LN_SQRT_2PI = 0.918938533204672741780329736406;
	double nn;

	if (n <= 15) {
		nn = n + n;
		if (fabs(nn - rint(nn)) < __DBL_EPSILON__) {
			return sferr_halves[lrint(nn)];
		}
		return lgamma(n + 1) - (n + 0.5) * log(n) + n - LN_SQRT_2PI;
	}
	nn = n * n;
	if (n > 500) {
		return (S0 - S1 / nn) / n;
	} else if (n > 80) {
		return (S0 - (S1 - S2 / nn) / nn) / n;
	} else if (n > 35) {
		return (S0 - (S1 - (S2 - S3 / nn) / nn) / nn) / n;
	}
	/* otherwise 15 < n <= 35 : */
	return (S0 - (S1 - (S2 - (S3 - S4 / nn) / nn) / nn) / nn) / n;
}

static double
bd0(double x, double q)
{
	if (fabs(x - q) < 0.1*(x + q)) {
		double ej, s, s1, v;

		v = (x - q) / (x + q);
		s = (x - q) * v;

		if (fabs(s) < __DBL_EPSILON__) {
			return s;
		}
		ej = 2 * x * v;
		v = v * v;

		/* Taylor series; 1000: no infinite loop
		 * as |v| < .1, v^2000 is "zero" */
		for (size_t j = 1; j < 1000U; j++) {
			ej *= v;
			s1 = s + ej / ((j << 1U) + 1U);
			if (fabs(s1 - s) < __DBL_EPSILON__) {
				return s1;
			}
			s = s1;
		}
	}
	/* else: | x - np | is not too small */
	return x * log(x / q) + q - x;
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
			subs /= width * 1000;

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


/* fitters */
static inline double
dpois(const zip_t m, double x)
{
	double v;

	if (UNLIKELY(x < __DBL_EPSILON__)) {
		v = -m.lambda;
	} else if (UNLIKELY(m.lambda < __DBL_EPSILON__)) {
		v = -m.lambda + x * log(m.lambda) - lgamma(x + 1);
	} else {
		/* fexp(M_2PI*x, -stirlerr(x)-bd0(x,lambda))
		 * with fexp(f,x) <- -0.5*log(f)+(x) */
		static const double l2pi = 1.837877066409345;

		v = -0.5 * (l2pi + log(x)) - stirlerr(x) - bd0(x, m.lambda);
	}
	return v;
}

static inline double
dgamma(const gamma_t m, double x)
{
	double p;

	if (UNLIKELY(x < __DBL_EPSILON__)) {
		return m.shape < 1 ? INFINITY : m.shape > 1 ? -INFINITY : log(m.rate);
	}
	p = dpois((zip_t){0, x * m.rate}, m.shape - (m.shape >= 1));
	if (m.shape < 1) {
		return p + log(m.shape / x);
	}
	return p + log(m.rate);
}

static inline double
dpareto(const pareto_t m, double x)
{
	double lx, r;

	if (UNLIKELY(x < __DBL_EPSILON__)) {
		return log(m.shape) + m.rate;
	}

	lx = log(x);
	with (double tmp = lx + m.rate) {
		r = log(m.shape) - m.shape * log1pexpf(tmp)
			- log1pexpf(-tmp) - lx;
	}
	return r;
}

static gamma_t
fit_erlang(void)
{
	size_t td = 0U;
	double su = 0;

	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		td += dlt[i];
	}
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (UNLIKELY(!dlt[i])) {
			continue;
		}
		su += (double)dlt[i] * (double)thi[i];
	}

	const double ltd = log((double)td);
	gamma_t m = {
		ltd, (double)(np + 1U), (double)(td * (np + 1U)) / su,
	};
	/* reconstruct
	 * so that fitted model and empirical model have same support */
	with (size_t tmp = 0U) {
		for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
			if (!dlt[i]) {
				continue;
			}
			tmp += exp(m.logn + dgamma(m, (double)thi[i]));
		}
		/* get at least the same count in the count */
		m.logn += ltd - log((double)tmp);
	}
	return m;
}

static gamma_t
fit_gamma(void)
{
	size_t td = 0U;
	double ls = 0, su = 0;

	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		td += dlt[i];
	}
	for (size_t i = 1U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		ls += (double)dlt[i] * log((double)thi[i]);
		su += (double)dlt[i] * (double)thi[i];
	}

	const double ltd = log((double)td);
	double s = log(su) - ltd - ls / (double)td;
	double k = (3 - s + sqrt((s - 3)*(s - 3) + 24*s)) / (12 * s);
	double r = (double)td * k / su;

	/* assimilate k and r */
	with (double tmp = sqrt(k / r)) {
		k /= tmp;
		r *= tmp;
	}

	gamma_t m = {ltd, k, r};
	/* reconstruct
	 * so that fitted model and empirical model have same support */
	with (size_t tmp = 0U) {
		for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
			if (!dlt[i]) {
				continue;
			}
			tmp += exp(m.logn + dgamma(m, (double)thi[i]));
		}
		/* get at least the same count in the count */
		m.logn += ltd - log((double)tmp);
	}
	return m;
}

static pareto_t
fit_lomax(void)
{
	size_t d = 0U;

	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		d += dlt[i];
	}

	const double ld = log((double)d);
	double sh = 0;
	for (size_t i = 1U, n = 1U << highbits; i < n; i++) {
		if (UNLIKELY(!dlt[i])) {
			continue;
		}
		sh += dlt[i] * log((double)thi[i]);
	}

	pareto_t r = {ld, (double)((double)d / sh), 0};
	/* correction for zero inflation, reproduce */
	with (size_t tmp = 0U) {
		for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
			if (!dlt[i]) {
				continue;
			}
			tmp += exp(r.logn + dpareto(r, (double)thi[i]));
		}
		/* get at least the same count in the count */
		r.logn += ld - log((double)tmp);
	}
	return r;
}

static size_t
mtozu(double logn, double x)
{
	x = exp(logn + x);
	return isfinite(x) ? lrint(x) : -1ULL;
}


/* printers */
static void
prnt_cndl_mtrx(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const gamma_t me = fit_erlang();
	const gamma_t mg = fit_gamma();
	const pareto_t ml = fit_lomax();

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

	/* erlang fit */
	len += memncpy(buf + len, "theo_erlang", strlenof("theo_erlang"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(me.logn, dgamma(me, thi[i])));
	}
	buf[len++] = '\n';

	/* gamma fit */
	len += memncpy(buf + len, "theo_gamma", strlenof("theo_gamma"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(mg.logn, dgamma(mg, thi[i])));
	}
	buf[len++] = '\n';

	/* lomax fit */
	len += memncpy(buf + len, "theo_lomax", strlenof("theo_lomax"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(ml.logn, dpareto(ml, thi[i])));
	}
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_molt(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const gamma_t me = fit_erlang();
	const gamma_t mg = fit_gamma();
	const pareto_t ml = fit_lomax();

	if (!ncndl++) {
		static const char hdr[] = "lo\thi\tcnt\ttheo_erlang\ttheo_gamma\ttheo_lomax\n";
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
		buf[len++] = '\t';
		len += zutostr(buf + len, sizeof(buf) - len,
			       mtozu(me.logn, dgamma(me, thi[i])));
		buf[len++] = '\t';
		len += zutostr(buf + len, sizeof(buf) - len,
			       mtozu(mg.logn, dgamma(mg, thi[i])));
		buf[len++] = '\t';
		len += zutostr(buf + len, sizeof(buf) - len,
			       mtozu(ml.logn, dpareto(ml, thi[i])));
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
