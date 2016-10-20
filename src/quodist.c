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
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "nifty.h"
#include "dfp754_d32.h"
#include "dfp754_d64.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr
#define NOT_A_TIME	((tv_t)-1ULL)

/* relevant tick dimensions */
typedef struct {
	px_t b;
	px_t a;
} quo_t;

static tv_t intv;
static enum {
	UNIT_NONE,
	UNIT_SECS,
	UNIT_DAYS,
	UNIT_MONTHS,
	UNIT_YEARS,
} unit;

static unsigned int highbits = 1U;


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
	return snprintf(buf, bsz, "%lu.%03lu000000", t / MSECS, t % MSECS);
}

static ssize_t
cttostr(char *restrict buf, size_t bsz, tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (unit) {
	default:
	case UNIT_NONE:
		memcpy(buf, "ALL", 3U);
		return 3U;
	case UNIT_SECS:
		return tvtostr(buf, bsz, t);
	case UNIT_DAYS:
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / MSECS;
	u--;
	tm = gmtime(&u);

	switch (unit) {
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
ztostr(char *restrict buf, size_t bsz, size_t n)
{
	return snprintf(buf, bsz, "%zu", n);
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

static inline __attribute__((pure, const)) px_t
min_px(px_t p1, px_t p2)
{
	return p1 <= p2 ? p1 : p2;
}

static inline __attribute__((pure, const)) px_t
max_px(px_t p1, px_t p2)
{
	return p1 >= p2 ? p1 : p2;
}

static inline __attribute__((pure, const)) uint32_t
ilog2(const uint32_t x)
{
	return 31U - __builtin_clz(x);
}


/* next candle time */
static tv_t nxct;

static tv_t _1st = NOT_A_TIME;
static tv_t last;
static px_t minbid;
static px_t maxbid;
static px_t minask;
static px_t maxask;
static tv_t mindlt = NOT_A_TIME;
static tv_t maxdlt;

static char cont[64];
static size_t conz;

/* stats */
static union {
	size_t start[];
	struct {
		size_t dlt[1U];
		size_t bid[1U];
		size_t ask[1U];
	} _0;
	struct {
		size_t dlt[32U];
		size_t bid[32U];
		size_t ask[32U];
	} _5;
	struct {
		size_t dlt[512U];
		size_t bid[512U];
		size_t ask[512U];
	} _9;
	struct {
		size_t dlt[8192U];
		size_t bid[8192U];
		size_t ask[8192U];
	} _13;
} cnt;

static size_t *logdlt;
static size_t *pmantb;
static size_t *pmanta;

static void prnt_cndl(void);

static tv_t
next_cndl(tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (unit) {
	default:
	case UNIT_NONE:
		return NOT_A_TIME;
	case UNIT_SECS:
		return (t / intv + 1U) * intv;
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

	switch (unit) {
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

static int
push_init(char *ln, size_t UNUSED(lz))
{
	size_t iz;
	char *on;

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ln, '\t')) == NULL)) {
		return -1;
	}
	iz = on++ - ln;

	/* snarf quotes */
	if (!(maxbid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(minask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	minbid = maxbid;
	maxask = minask;

	mindlt = NOT_A_TIME;
	maxdlt = 0ULL;

	memcpy(cont, ln, conz = iz);
	return 0;
}

static int
push_beef(char *ln, size_t lz)
{
	tv_t t;
	quo_t q;
	char *on;
	int rc = 0;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (t < last) {
		fputs("Warning: non-chronological\n", stderr);
		rc = -1;
		goto out;
	} else if (UNLIKELY(t >= nxct)) {
		prnt_cndl();
		nxct = next_cndl(t);
		_1st = last = t;
		(void)push_init(on, lz - (on - ln));
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(q.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	with (unsigned int bm, am) {
		minbid = min_px(minbid, q.b);
		maxbid = max_px(maxbid, q.b);
		minask = min_px(minask, q.a);
		maxask = max_px(maxask, q.a);

		bm = decompd32(q.b).mant;
		am = decompd32(q.a).mant;

		bm <<= __builtin_clz(bm);
		am <<= __builtin_clz(am);

		bm >>= 32U - highbits;
		am >>= 32U - highbits;

		bm &= (1U << highbits) - 1U;
		am &= (1U << highbits) - 1U;

		pmantb[bm]++;
		pmanta[am]++;
	}

	with (tv_t dlt = t - last) {
		unsigned int slot;

		mindlt = min_tv(mindlt, dlt);
		maxdlt = max_tv(maxdlt, dlt);

		slot = ilog2(dlt / MSECS + 1U);
		/* determine sub slot, if applicable */
		with (unsigned int width = (1U << slot), base = width - 1U) {
			unsigned int subs;

			/* translate in terms of base */
			subs = (dlt - base * MSECS) << (highbits - 5U);
			/* divide by width */
			subs /= width * MSECS;

			slot <<= highbits;
			slot >>= 5U;
			slot ^= subs;
		}
		/* poisson fit */
		logdlt[slot]++;
	}

out:
	/* and store state */
	last = t;
	return rc;
}

static void
prnt_cndl(void)
{
	static size_t ncndl;
	static char buf[sizeof(cnt)];
	size_t len = 0U;

	if (UNLIKELY(_1st == NOT_A_TIME)) {
		return;
	}

	switch (ncndl++) {
		static const char hdr[] = "cndl\tccy\ttype\tmin\tmax";
	default:
		break;
	case 0U:
		len = (memcpy(buf, hdr, strlenof(hdr)), strlenof(hdr));
		for (size_t i = 0U; i < (1U << highbits); i++) {
			buf[len++] = '\t';
			buf[len++] = 'v';
			len += snprintf(buf + len, sizeof(buf) - len, "%zu", i);
		}
		buf[len++] = '\n';
		fwrite(buf, sizeof(*buf), len, stdout);
		break;
	}

	/* candle identifier */
	len = cttostr(buf, sizeof(buf), nxct);

	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);

	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';

	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, _1st);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, last);

	for (size_t i = 0U; i < (1U << highbits); i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, logdlt[i]);
	}
	buf[len++] = '\n';

	/* candle identifier */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);

	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);

	buf[len++] = '\t';
	buf[len++] = 'b';

	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, minbid);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxbid);

	for (size_t i = 0U; i < (1U << highbits); i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, pmantb[i]);
	}
	buf[len++] = '\n';

	/* candle identifier */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);

	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);

	buf[len++] = '\t';
	buf[len++] = 'a';

	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, minask);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxask);

	for (size_t i = 0U; i < (1U << highbits); i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, pmanta[i]);
	}

	buf[len++] = '\n';
	fwrite(buf, sizeof(*buf), len, stdout);

	/* just reset all stats pages */
	memset(cnt.start, 0, sizeof(cnt));
	return;
}


#include "quodist.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];

	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->interval_arg) {
		char *on;

		if (!(intv = strtoul(argi->interval_arg, &on, 10))) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		}
		switch (*on++) {
		secs:
		case '\0':
		case 'S':
		case 's':
			/* seconds, don't fiddle */
			intv *= MSECS;
			unit = UNIT_SECS;
			break;
		case 'm':
		case 'M':
			if (*on == 'o' || *on == 'O') {
				goto months;
			}
			intv *= 60U;
			goto secs;

		months:
			unit = UNIT_MONTHS;
			break;

		case 'y':
		case 'Y':
			unit = UNIT_YEARS;
			break;

		case 'h':
		case 'H':
			intv *= 60U * 60U;
			goto secs;
		case 'd':
		case 'D':
			unit = UNIT_DAYS;
			break;

		default:
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	/* set resolution */
	highbits = (argi->verbose_flag << 2U) ^ (argi->verbose_flag > 0U);

	switch (highbits) {
	case 0U:
		logdlt = cnt._0.dlt;
		pmantb = cnt._0.bid;
		pmanta = cnt._0.ask;
		break;
	case 5U:
		logdlt = cnt._5.dlt;
		pmantb = cnt._5.bid;
		pmanta = cnt._5.ask;
		break;
	case 9U:
		logdlt = cnt._9.dlt;
		pmantb = cnt._9.bid;
		pmanta = cnt._9.ask;
		break;
	case 13U:
		logdlt = cnt._13.dlt;
		pmantb = cnt._13.bid;
		pmanta = cnt._13.ask;
		break;
	default:
		errno = 0, serror("\
Error: verbose flag can only be used once, twice or three times..");
		rc = 1;
		goto out;
	}

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			(void)push_beef(line, nrd);
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
