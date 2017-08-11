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
#include "tv.h"
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

static tvu_t intv;

static unsigned int highbits = 1U;
static unsigned int elapsp;
static unsigned int allp;
static tvu_t pbase;

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
tztostr(char *restrict buf, size_t bsz, const tv_t *rv, size_t nv)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nv; i++) {
		buf[len++] = '\t';
		len += tvtostr(buf + len, bsz - len, rv[i]);
	}
	return len;
}


static inline __attribute__((pure, const)) tv_t
min_tv(tv_t t1, tv_t t2)
{
	return t1 <= t2 ? t1 : t2;
}

static inline __attribute__((pure, const)) tv_t
max_tv(tv_t t1, tv_t t2)
{
	return t1 >= t2 ? t1 : t2;
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


/* next candle time */
static tv_t nxct;

static tv_t last;
static size_t ip, np;

/* stats */
static union {
	size_t start[0U];

#define MAKE_SLOTS(n)				\
	struct {				\
		size_t dlt[1U << (n)];		\
						\
		tv_t tlo[1U << (n)];		\
		tv_t thi[1U << (n)];		\
	} _##n

	MAKE_SLOTS(0);
	MAKE_SLOTS(5);
	MAKE_SLOTS(9);
	MAKE_SLOTS(13);
	MAKE_SLOTS(17);
	MAKE_SLOTS(21);
} cnt;

static size_t *dlt;
static tv_t *tlo;
static tv_t *thi;
static size_t cntz;
static double agg;

static void(*prnt_cndl)(void);
static char buf[sizeof(cnt)];

static tv_t
next_cndl(tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (intv.u) {
	default:
	case UNIT_NONE:
		return NATV;
	case UNIT_NSECS:
		return (t / intv.t + 1U) * intv.t;
	case UNIT_SECS:
		return (t / NSECS / intv.t + 1U) * intv.t * NSECS;
	case UNIT_DAYS:
		t /= 24ULL * 60ULL * 60ULL * NSECS;
		t += intv.t;
		t *= 24ULL * 60ULL * 60ULL * NSECS;
		return t;
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / NSECS;
	tm = gmtime(&u);
	tm->tm_mday = 1;
	tm->tm_yday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;

	switch (intv.u) {
	case UNIT_MONTHS:
		*tm = (struct tm){
			.tm_year = tm->tm_year,
			.tm_mon = tm->tm_mon + intv.t,
			.tm_mday = 1,
		};
		break;
	case UNIT_YEARS:
		*tm = (struct tm){
			.tm_year = tm->tm_year + intv.t,
			.tm_mon = 0,
			.tm_mday = 1,
		};
		break;
	}
	return mktime(tm) * NSECS;
}

static inline void
rset_cndl(void)
{
	if (!allp) {
		/* just reset all stats pages */
		memset(cnt.start, 0, cntz);
	} else {
		memset(cnt.start, 0, (1 << highbits) * sizeof(*dlt));
	}
	return;
}

static inline void
rset_rngs(void)
{
	if (!allp) {
		memset(tlo, -1, (1U << highbits) * sizeof(*tlo));
		return;
	}
	return;
}

static int
push_erlagg(char *ln, size_t UNUSED(lz))
{
	tv_t t;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, NULL)) == NATV)) {
		return -1;
	} else if (UNLIKELY(t < last)) {
		fputs("Warning: non-chronological\n", stderr);
		return -1;
	} else if (UNLIKELY(t > nxct)) {
		if (LIKELY(nxct)) {
			prnt_cndl();
		}
		agg = 0.;
		nxct = next_cndl(t);
	}

	/* measure time */
	with (tv_t dt = t - last) {
		/* add offset */
		switch (pbase.u) {
		default:
		case UNIT_NONE:
			break;
		case UNIT_NSECS:
			dt += pbase.t;
			break;
		case UNIT_SECS:
			dt += pbase.t * 1000000000ULL;
			break;
		}
		agg += (double)NSECS / (double)dt;
	}
	/* and store state */
	last = t;
	ip = 0U;
	return 0;
}

static int
push_erlng(char *ln, size_t UNUSED(lz))
{
	tv_t t;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, NULL)) == NATV)) {
		return -1;
	} else if (UNLIKELY(t < last)) {
		fputs("Warning: non-chronological\n", stderr);
		return -1;
	} else if (UNLIKELY(t > nxct)) {
		if (LIKELY(nxct)) {
			prnt_cndl();
		}
		rset_cndl();
		rset_rngs();
		nxct = next_cndl(t);
	} else if (LIKELY(ip++ < np)) {
		return 0;
	} else if (t - last < pbase.t) {
		/* hasn't survived long enough */
		;
	} else {
		/* measure time */
		const tv_t dt = t - last;
		size_t acc = !elapsp ? 1ULL : dt;
		unsigned int slot = ilog2(dt / NSECS + 1U);
		/* determine sub slot, if applicable */
		unsigned int width = (1U << slot), base = width - 1U;
		unsigned int subs;

		{
			/* translate in terms of base */
			subs = (dt - base * NSECS) << (highbits - 5U);
			/* divide by width */
			subs /= width * NSECS;

			slot <<= highbits;
			slot >>= 5U;
			slot ^= subs;
		}
		/* poisson fit */
		dlt[slot] += acc;

		tlo[slot] = min_tv(tlo[slot], dt);
		thi[slot] = max_tv(thi[slot], dt);
	}
	/* and store state */
	last = t;
	ip = 0U;
	return 0;
}

static int
push_poiss(char *ln, size_t UNUSED(lz))
{
	static size_t this = 1U;
	int rc = 0;
	tv_t t;

	/* metronome is up first */
	if (UNLIKELY(ln == NULL)) {
		goto final;
	} else if (UNLIKELY((t = strtotv(ln, NULL)) == NATV)) {
		return -1;
	} else if (UNLIKELY(t > nxct)) {
		if (LIKELY(nxct)) {
			prnt_cndl();
		}
		rset_cndl();
		nxct = next_cndl(t);
		/* make sure we use the trimmed t value */
		t /= pbase.t;
	} else if (UNLIKELY((t /= pbase.t) < last)) {
		errno = 0, serror("\
Warning: non-chronological");
		rc = -1;
	} else if (UNLIKELY(t == last)) {
		this++;
	} else {
		dlt[0U] += t - last - 1U;
	final:
		if (LIKELY(this < cntz)) {
			dlt[this]++;
		} else {
			errno = 0, serror("\
Warning: base interval too big, consider setting --base to a smaller value.\n\
Got %zu events but can only track %zu.", this, cntz);
			rc = -1;
		}
		this = 1U;
	}
	/* and store state */
	last = t;
	return rc;
}


/* fitters */
static inline double
dzip(const zip_t m, double x)
{
	double v;

	if (UNLIKELY(!x)) {
		v = log(m.pi + (1 - m.pi) * exp(-m.lambda));
	} else {
		v = log(1 - m.pi) - m.lambda + x * log(m.lambda) - lgamma(x + 1);
	}
	return v;
}

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
		ltd, (double)(np + 1U), (double)(td * (np + 1U) * NSECS) / su,
	};
	/* reconstruct
	 * so that fitted model and empirical model have same support */
	with (size_t tmp = 0U) {
		for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
			if (!dlt[i]) {
				continue;
			}
			tmp += exp(m.logn + dgamma(m, (double)thi[i] / NSECS));
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
	const double lms = log((double)NSECS);

	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		td += dlt[i];
	}
	for (size_t i = 1U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		ls += (double)dlt[i] * (log((double)thi[i]) - lms);
		su += (double)dlt[i] * (double)thi[i];
	}

	const double ltd = log((double)td);
	double s = log(su) - ltd - lms - ls / (double)td;
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
			tmp += exp(m.logn + dgamma(m, (double)thi[i] / NSECS));
		}
		/* get at least the same count in the count */
		m.logn += ltd - log((double)tmp);
	}
	return m;
}

static zip_t
fit_zip(void)
{
	size_t d = 0U, w = 0U;

	for (size_t i = 0U; i < cntz; i++) {
		d += dlt[i];
	}
	for (size_t i = 1U; i < cntz; i++) {
		w += dlt[i] * i;
	}

	/* run 10 iterations of the MLE estimator */
	double lambda = 2;
	const double mu = (double)w / (double)d;
	const double z = (double)dlt[0U] / (double)d;
	for (size_t i = 0U; i < 10U; i++) {
		lambda = mu * (1 - exp(-lambda)) / (1 - z);
	}
	return (zip_t){log((double)d), (double)lambda, (double)(1 - mu / lambda)};
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
prnt_agg(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (!ncndl++) {
		static const char hdr[] = "cndl\tcnt\n";
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
	}

	len = 0U;
	/* candle instance */
	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* count */
	buf[len++] = '\t';
	len += snprintf(buf + len, sizeof(buf) - len, "%.0f", agg);
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_mtrx(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const gamma_t me = fit_erlang();
	const gamma_t mg = fit_gamma();
	const pareto_t ml = fit_lomax();

	if (!ncndl++) {
		static const char hdr[] = "cndl\tmetric";

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

	/* delta t */
	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += tztostr(buf + len, sizeof(buf) - len, tlo, 1U << highbits);
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += tztostr(buf + len, sizeof(buf) - len, thi, 1U << highbits);
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	len += memncpy(buf + len, "\tcnt", strlenof("\tcnt"));
	len += zztostr(buf + len, sizeof(buf) - len, dlt, 1U << highbits);
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	len += memncpy(buf + len, "\ttheo_erlang", strlenof("\ttheo_erlang"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(me.logn, dgamma(me, (double)thi[i] / NSECS)));
	}
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	len += memncpy(buf + len, "\ttheo_gamma", strlenof("\ttheo_gamma"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(mg.logn, dgamma(mg, (double)thi[i] / NSECS)));
	}
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	/* type t */
	len += memncpy(buf + len, "\ttheo_lomax", strlenof("\ttheo_lomax"));
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(ml.logn, dpareto(ml, (double)thi[i])));
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
		static const char hdr[] = "cndl\tlo\thi\tcnt\ttheo_erlang\ttheo_gamma\ttheo_lomax\n";
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
	}

	/* delta t */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		/* otherwise */
		len += tvutostr(buf + len, sizeof(buf) - len,
				(tvu_t){nxct, intv.u});
		/* type t */
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, tlo[i]);
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, thi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(me.logn, dgamma(me, (double)thi[i] / NSECS)));
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(mg.logn, dgamma(mg, (double)thi[i] / NSECS)));
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(ml.logn, dpareto(ml, (double)thi[i])));
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_mtrx_poiss(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const zip_t m = fit_zip();

	if (!ncndl++) {
		static const char hdr[] = "cndl\tmetric";

		len = memncpy(buf, hdr, strlenof(hdr));
		for (size_t i = 0U; i < cntz; i++) {
			if (!dlt[i] && !allp) {
				continue;
			}
			buf[len++] = '\t';
			len += ztostr(buf + len, sizeof(buf) - len, i);
		}
		buf[len++] = '\n';
		fwrite(buf, sizeof(*buf), len, stdout);
		len = 0U;
	}

	/* delta t */
	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	len += memncpy(buf + len, "\tcnt", strlenof("\tcnt"));
	for (size_t i = 0U; i < cntz; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
	}
	buf[len++] = '\n';

	len += tvutostr(buf + len, sizeof(buf) - len, (tvu_t){nxct, intv.u});
	len += memncpy(buf + len, "\ttheo_zip", strlenof("\ttheo_zip"));
	for (size_t i = 0U; i < cntz; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(m.logn, dzip(m, (double)i)));
	}
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_molt_poiss(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const zip_t m = fit_zip();

	if (!ncndl++) {
		static const char hdr[] = "cndl\tk\tcnt\ttheo\n";
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
	}

	/* delta t */
	len = 0U;
	for (size_t i = 0U; i < cntz; i++) {
		if (!dlt[i] && !allp) {
			continue;
		}
		/* otherwise */
		len += tvutostr(buf + len, sizeof(buf) - len,
				(tvu_t){nxct, intv.u});
		/* type t */
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, i);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len,
			      mtozu(m.logn, dzip(m, (double)i)));
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}


#include "evtdist.yucc"

static int
setup_erlang(struct yuck_cmd_erlang_s argi[static 1U])
{
	if (argi->occurrences_arg) {
		if (!(np = strtoul(argi->occurrences_arg, NULL, 10))) {
			errno = 0, serror("\
Error: occurrences must be positive.");
			return -1;
		}
		/* we need it off-by-one */
		np--;
	}
	if (!argi->base_arg) {
		;
	} else if (!(pbase = strtotvu(argi->base_arg, NULL)).t) {
		errno = 0, serror("\
Error: cannot read base argument.");
		return -1;
	}
	switch (pbase.u) {
	case UNIT_MONTHS:
	case UNIT_YEARS:
		errno = 0, serror("\
Error: invalid suffix in base argument.");
		return -1;
	case UNIT_NONE:
	case UNIT_NSECS:
		/* yay */
		break;
	case UNIT_SECS:
		pbase.t *= NSECS;
		break;
	case UNIT_DAYS:
		pbase.t *= 60ULL * 60ULL * 24ULL * NSECS;
		break;
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
						\
		cntz = sizeof(cnt._##n)

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

	/* count events or elapsed times */
	elapsp = argi->time_flag;
	ztostr = !elapsp ? zutostr : tvtostr;

	if (allp) {
		/* set up lo and hi */
		tlo[0U] = 0;
		for (size_t i = 1U, n = (1U << highbits); i < n; i++) {
			const size_t sh = highbits > 5U ? highbits - 5U : 0U;
			const size_t w = (1ULL << sh) - 1ULL;
			const size_t s = ((1ULL << (i >> sh)) - 1U);
			const size_t l = (s + 1U);

			tlo[i] = s * NSECS + (((i & w) * l * NSECS) >> sh);
		}
		for (size_t i = 0U, n = (1U << highbits) - 1U; i < n; i++) {
			thi[i] = tlo[i + 1U];
		}
		thi[(1U << highbits) - 1U] = NATV;
	}
	return 0;
}

static int
setup_poisson(struct yuck_cmd_poisson_s argi[static 1U])
{
	if (!argi->base_arg) {
		errno = 0, serror("\
Error: --base argument is mandatory.");
		return -1;
	} else if (!(pbase = strtotvu(argi->base_arg, NULL)).t) {
		return -1;
	}
	switch (pbase.u) {
	case UNIT_NONE:
		errno = 0, serror("\
Error: base argument in poisson mode is mandatory.");
		return -1;
	case UNIT_MONTHS:
	case UNIT_YEARS:
		errno = 0, serror("\
Error: invalid suffix in base argument.");
		return -1;
	case UNIT_NSECS:
		/* yay */
		break;
	case UNIT_SECS:
		pbase.t *= NSECS;
		break;
	case UNIT_DAYS:
		pbase.t *= 60ULL * 60ULL * 24ULL * NSECS;
		break;
	}

	/* set candle printer */
	prnt_cndl = !argi->table_flag
		? prnt_cndl_molt_poiss : prnt_cndl_mtrx_poiss;

	/* just reuse erlang's memory */
	dlt = cnt.start;
	cntz = sizeof(cnt) / sizeof(dlt);
	ztostr = zutostr;
	return 0;
}

static int
setup_erlagg(struct yuck_cmd_erlagg_s argi[static 1U])
{
	if (argi->offset_arg) {
		pbase = strtotvu(argi->offset_arg, NULL);
		switch (pbase.u) {
		default:
			break;
		case UNIT_NONE:
		case UNIT_DAYS:
		case UNIT_MONTHS:
		case UNIT_YEARS:
			errno = 0, serror("\
Error: invalid suffix to --offset argument.");
			return -1;
		}			
	}

	/* set candle printer */
	prnt_cndl = prnt_agg;
	return 0;
}

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = EXIT_FAILURE;
		goto out;
	}

	if (argi->interval_arg) {
		intv = strtotvu(argi->interval_arg, NULL);
		if (!intv.t) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = EXIT_FAILURE;
			goto out;
		} else if (!intv.u) {
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = EXIT_FAILURE;
			goto out;
		}
	}

	allp = argi->all_flag;

	switch (argi->cmd) {
	case EVTDIST_CMD_ERLANG:
		if (setup_erlang((void*)argi) < 0) {
			rc = EXIT_FAILURE;
			goto out;
		}
		break;
	case EVTDIST_CMD_POISSON:
		if (setup_poisson((void*)argi) < 0) {
			rc = EXIT_FAILURE;
			goto out;
		}
		break;
	default:
	case EVTDIST_CMD_ERLAGG:
		if (setup_erlagg((void*)argi) < 0) {
			rc = EXIT_FAILURE;
			goto out;
		}
		break;
	}

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		switch (argi->cmd) {
		default:
		case EVTDIST_CMD_ERLANG:
			while ((nrd = getline(&line, &llen, stdin)) > 0) {
				(void)push_erlng(line, nrd);
			}
			break;
		case EVTDIST_CMD_POISSON:
			while ((nrd = getline(&line, &llen, stdin)) > 0) {
				(void)push_poiss(line, nrd);
			}
			/* finalise poisson pusher */
			(void)push_poiss(NULL, 0U);
			break;
		case EVTDIST_CMD_ERLAGG:
			while ((nrd = getline(&line, &llen, stdin)) > 0) {
				(void)push_erlagg(line, nrd);
			}
			break;
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
