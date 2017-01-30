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
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <ieee754.h>
#include <math.h>
#include "nifty.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef long unsigned int tv_t;
#define NOT_A_TIME	((tv_t)-1ULL)

typedef struct {
	size_t n;
	float lambda;
	float pi;
} zip_t;

typedef struct {
	size_t n;
	float shape;
	float rate;
} zie_t;

typedef struct {
	size_t n;
	float shape;
	float logmin;
} pareto_t;

typedef struct {
	tv_t t;
	enum {
		UNIT_NONE,
		UNIT_MSECS,
		UNIT_DAYS,
		UNIT_MONTHS,
		UNIT_YEARS,
	} u;
} tvu_t;

static tvu_t intv;

static unsigned int highbits = 1U;
static unsigned int elapsp;
static tv_t pbase = MSECS;

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

static tv_t
strtotv(const char *ln, char **endptr)
{
	char *on;
	tv_t r;

	/* time value up first */
	with (long unsigned int s, x) {
		if (UNLIKELY(!(s = strtoul(ln, &on, 10)) || on == NULL)) {
			r = NOT_A_TIME;
			goto out;
		} else if (*on == '.') {
			char *moron;

			x = strtoul(++on, &moron, 10);
			if (UNLIKELY(moron - on > 9U)) {
				return NOT_A_TIME;
			} else if ((moron - on) % 3U) {
				/* huh? */
				return NOT_A_TIME;
			}
			switch (moron - on) {
			case 9U:
				x /= MSECS;
			case 6U:
				x /= MSECS;
			case 3U:
				break;
			case 0U:
			default:
				break;
			}
			on = moron;
		} else {
			x = 0U;
		}
		r = s * MSECS + x;
	}
	/* overread up to 3 tabs */
	for (size_t i = 0U; *on == '\t' && i < 3U; on++, i++);
out:
	if (LIKELY(endptr != NULL)) {
		*endptr = on;
	}
	return r;
}

static ssize_t
tvtostr(char *restrict buf, size_t bsz, tv_t t)
{
	return t < NOT_A_TIME
		? snprintf(buf, bsz, "%lu.%03lu000000", t / MSECS, t % MSECS)
		: 0U;
}

static tvu_t
strtotvu(const char *str, char **endptr)
{
	char *on;
	tvu_t r;

	if (!(r.t = strtoul(str, &on, 10))) {
		return (tvu_t){};
	}
	switch (*on++) {
	secs:
	case '\0':
	case 'S':
	case 's':
		/* seconds, don't fiddle */
		r.t *= MSECS;
	msecs:
		r.u = UNIT_MSECS;
		break;

	case 'm':
	case 'M':
		switch (*on) {
		case '\0':
			/* they want minutes, oh oh */
			r.t *= 60UL;
			goto secs;
		case 's':
		case 'S':
			/* milliseconds it is then */
			goto msecs;
		case 'o':
		case 'O':
			r.u = UNIT_MONTHS;
			break;
		default:
			goto invalid;
		}
		break;

	case 'y':
	case 'Y':
		r.u = UNIT_YEARS;
		break;

	case 'h':
	case 'H':
		r.t *= 60U * 60U;
		goto secs;
	case 'd':
	case 'D':
		r.u = UNIT_DAYS;
		break;

	default:
	invalid:
		errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
		return (tvu_t){};
	}
	if (endptr != NULL) {
		*endptr = on;
	}
	return r;
}

static ssize_t
cttostr(char *restrict buf, size_t bsz, tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (intv.u) {
	default:
	case UNIT_NONE:
		memcpy(buf, "ALL", 3U);
		return 3U;
	case UNIT_MSECS:
		return tvtostr(buf, bsz, t);
	case UNIT_DAYS:
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / MSECS;
	u--;
	tm = gmtime(&u);

	switch (intv.u) {
	case UNIT_DAYS:
		return strftime(buf, bsz, "%F", tm);
	case UNIT_MONTHS:
		return strftime(buf, bsz, "%Y-%m", tm);
	case UNIT_YEARS:
		return strftime(buf, bsz, "%Y", tm);
	default:
		break;
	}
	return 0;
}

static ssize_t
zutostr(char *restrict buf, size_t bsz, size_t n)
{
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
		return NOT_A_TIME;
	case UNIT_MSECS:
		return (t / intv.t + 1U) * intv.t;
	case UNIT_DAYS:
		t /= 24U * 60U * 60U * MSECS;
		t++;
		t *= 24U * 60U * 60U * MSECS;
		return t;
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / MSECS;
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
			.tm_mon = tm->tm_mon + 1,
			.tm_mday = 1,
		};
		break;
	case UNIT_YEARS:
		*tm = (struct tm){
			.tm_year = tm->tm_year + 1,
			.tm_mon = 0,
			.tm_mday = 1,
		};
		break;
	}
	return mktime(tm) * MSECS;
}

static inline void
rset_cndl(void)
{
	/* just reset all stats pages */
	memset(cnt.start, 0, cntz);
	return;
}

static inline void
rset_rngs(void)
{
	memset(tlo, -1, (1U << highbits) * sizeof(*tlo));
	return;
}

static int
push_erlng(char *ln, size_t UNUSED(lz))
{
	char *on;
	tv_t t;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
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
	} else {
		/* measure time */
		const tv_t dt = t - last;
		size_t acc = !elapsp ? 1ULL : dt;
		unsigned int slot = ilog2(dt / MSECS + 1U);
		/* determine sub slot, if applicable */
		unsigned int width = (1U << slot), base = width - 1U;
		unsigned int subs;

		{
			/* translate in terms of base */
			subs = (dt - base * MSECS) << (highbits - 5U);
			/* divide by width */
			subs /= width * MSECS;

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
	char *on;
	tv_t t;
	int rc = 0;

	/* metronome is up first */
	if (UNLIKELY(ln == NULL)) {
		goto final;
	} else if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (UNLIKELY(t > nxct)) {
		if (LIKELY(nxct)) {
			prnt_cndl();
		}
		rset_cndl();
		nxct = next_cndl(t);
		/* make sure we use the trimmed t value */
		t /= pbase;
	} else if (UNLIKELY((t /= pbase) < last)) {
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
static zie_t
fit_zie(void)
{
	size_t td = 0U;
	double ls = 0, ld = 0;

	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		td += dlt[i];
	}
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (UNLIKELY(!dlt[i])) {
			continue;
		}
		ld += log((double)dlt[i]);
		ls += log((double)dlt[i]) * (double)((tlo[i] + thi[i]) / 2);
	}
	return (zie_t){td, (float)(np + 1U), (float)(ld / ls * MSECS)};
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
	return (zip_t){d, (float)lambda, (float)(1 - mu / lambda)};
}

static pareto_t
fit_pareto(void)
{
	size_t d = 0U;
	size_t m = 1U;
	double lm;

	for (size_t i = 1U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			m = i;
			break;
		} else if (dlt[i] > dlt[i - 1U]) {
			m = i - 1U;
			break;
		}
	}
	lm = log((double)tlo[m]);
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		d += dlt[i];
	}

	double sh = 0;
	for (size_t i = m, n = 1U << highbits; i < n; i++) {
		if (UNLIKELY(!dlt[i])) {
			continue;
		}
		sh += dlt[i] * (log((double)tlo[i]) - lm);
	}
	return (pareto_t){d, (float)((double)d / sh), (float)lm};
}

static inline size_t
dzip(const zip_t m, size_t k)
{
	float v;

	if (UNLIKELY(!k)) {
		v = m.pi + (1 - m.pi) * expf(-m.lambda);
	} else {
		const float dk = (float)k;
		v = logf(1 - m.pi) - m.lambda +
			dk * logf(m.lambda) - lgammaf(dk + 1);
		v = expf(v);
	}
	return lrintf(m.n * v);
}

static inline size_t
dpois(const zip_t m, float k)
{
	float v;

	if (k < __FLT_EPSILON__) {
		v = -m.lambda;
	} else if (m.lambda < __FLT_EPSILON__) {
		v = -m.lambda + k * logf(m.lambda) - lgammaf(k + 1);
	}
	return lrintf(m.n * expf(v));
}

static inline size_t
dzie(const zie_t m, size_t k)
{
	const float v = (float)((tlo[k] + thi[k]) / 2U) / MSECS;

	if (UNLIKELY(v < __FLT_EPSILON__)) {
		return m.shape < 1 ? -1ULL : m.shape > 1 ? 0ULL
			: lrintf(m.n * m.rate);
	}
	if (m.shape < 1) {
		return dpois((zip_t){m.n, v * m.rate}, m.shape) * m.shape / v;
	}
	return dpois((zip_t){m.n, v * m.rate}, m.shape - 1) * m.rate;
}

static inline size_t
dpareto(const pareto_t m, size_t k)
{
	const float lv = logf((float)tlo[k]);
	float r;

	if (UNLIKELY(lv < m.logmin)) {
		return 0;
	}
	r = logf(m.shape) + m.shape * m.logmin - (m.shape + 1) * lv;
	return lrintf(m.n * expf(r));
}


/* printers */
static void
prnt_cndl_mtrx(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (!ncndl++) {
		static const char hdr[] = "cndl\tmetric";

		len = memncpy(buf, hdr, strlenof(hdr));
		for (size_t i = 0U; i < (1U << highbits); i++) {
			buf[len++] = '\t';
			buf[len++] = 'v';
			len += ztostr(buf + len, sizeof(buf) - len, i);
		}
		buf[len++] = '\n';
		fwrite(buf, sizeof(*buf), len, stdout);
		len = 0U;
	}

	/* delta t */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, dlt, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += tztostr(buf + len, sizeof(buf) - len, tlo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += tztostr(buf + len, sizeof(buf) - len, thi, 1U << highbits);
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_molt(void)
{
	static size_t ncndl;
	size_t len = 0U;
	const zie_t mg = fit_zie();
	const pareto_t mp = fit_pareto();

	if (!ncndl++) {
		static const char hdr[] = "cndl\tlo\thi\tcnt\ttheo_gamma\ttheo_pareto\n";
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
	}

	/* delta t */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		/* type t */
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, tlo[i]);
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, thi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dzie(mg, i));
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dpareto(mp, i));
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
			if (!dlt[i]) {
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
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	len += memncpy(buf + len, "\tcnt", strlenof("\tcnt"));
	for (size_t i = 0U; i < cntz; i++) {
		if (!dlt[i]) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
	}
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	len += memncpy(buf + len, "\ttheo", strlenof("\ttheo"));
	for (size_t i = 0U; i < cntz; i++) {
		if (!dlt[i]) {
			continue;
		}
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dzip(m, i));
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
		if (!dlt[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		/* type t */
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, i);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dzip(m, i));
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
	return 0;
}

static int
setup_poisson(struct yuck_cmd_poisson_s argi[static 1U])
{
	if (!argi->base_arg) {
		errno = 0, serror("\
Error: --base argument is mandatory.");
		return -1;
	} else if (!(pbase = strtotvu(argi->base_arg, NULL).t)) {
		return -1;
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

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = EXIT_FAILURE;
		goto out;
	}

	if (argi->interval_arg &&
	    !(intv = strtotvu(argi->interval_arg, NULL)).t) {
		errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
		rc = EXIT_FAILURE;
		goto out;
	}

	switch (argi->cmd) {
	default:
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
