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

typedef struct {
	qx_t b;
	qx_t a;
} qty_t;

static tv_t intv;
static enum {
	UNIT_NONE,
	UNIT_SECS,
	UNIT_DAYS,
	UNIT_MONTHS,
	UNIT_YEARS,
} unit;


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

static inline __attribute__((pure, const)) qx_t
min_qx(qx_t q1, qx_t q2)
{
	return q1 <= q2 ? q1 : q2;
}

static inline __attribute__((pure, const)) qx_t
max_qx(qx_t q1, qx_t q2)
{
	return q1 >= q2 ? q1 : q2;
}


/* next candle time */
static tv_t nxct;

static px_t minask;
static px_t maxbid;
/* only used for draw-up/draw-down */
static px_t maxask;
static px_t minbid;
static px_t minspr;
static px_t maxspr;
static px_t maxdu;
static px_t maxdd;
static qx_t maxasz = 0.dd;
static qx_t maxbsz = 0.dd;
/* buy and sell imbalances */
static qx_t maxbim = 0.dd;
static qx_t maxsim = 0.dd;
static tv_t _1st = NOT_A_TIME;
static tv_t last;
static tv_t mindlt = NOT_A_TIME;
static tv_t maxdlt;

static char cont[64];
static size_t conz;

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
	/* calc initial spread */
	minspr = maxspr = minask - maxbid;

	/* snarf quantities */
	if (*on == '\t') {
		maxbsz = strtoqx(++on, &on);
		maxasz = strtoqx(++on, &on);

		maxsim = maxbim = maxasz - maxbsz;
	}

	/* more resetting */
	maxdd = maxdu = 0.df;
	/* just so we can kick off max-du and max-dd calcs */
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
	qty_t Q;
	char *on;
	int rc = 0;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (UNLIKELY(t < last)) {
		fputs("Warning: non-chronological\n", stderr);
		rc = -1;
		goto out;
	} else if (UNLIKELY(t > nxct)) {
		prnt_cndl();
		nxct = next_cndl(t);
		_1st = last = t;
		return push_init(on, lz - (on - ln));
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

	/* snarf quantities */
	Q.b = strtoqx(++on, &on);
	Q.a = strtoqx(++on, &on);

	maxbid = max_px(maxbid, q.b);
	minask = min_px(minask, q.a);
	with (px_t s = q.a - q.b) {
		minspr = min_px(minspr, s);
		maxspr = max_px(maxspr, s);
	}

	with (px_t du = q.a - minbid, dd = q.b - maxask) {
		maxdu = max_px(maxdu, du);
		maxdd = min_px(maxdd, dd);
		/* for next round */
		minbid = min_px(minbid, q.b);
		maxask = max_px(maxask, q.a);
	}

	with (tv_t dlt = t - last) {
		mindlt = min_tv(mindlt, dlt);
		maxdlt = max_tv(maxdlt, dlt);
	}

	maxbsz = max_qx(maxbsz, Q.b);
	maxasz = max_qx(maxasz, Q.a);
	with (qx_t imb = Q.a - Q.b) {
		maxsim = max_qx(maxsim, imb);
		maxbim = min_qx(maxbim, imb);
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
	char buf[4096U];
	size_t len = 0U;

	if (UNLIKELY(_1st == NOT_A_TIME)) {
		return;
	}

	switch (ncndl++) {
	default:
		break;
	case 0U:
		fputs("cndl\tccy\t_1st\tlast\tmindlt\tmaxdlt\tminask\tmaxbid\tminspr\tmaxspr\tmaxbsz\tmaxasz\tmaxbim\tmaxsim\tmaxdu\tmaxdd\n", stdout);
		break;
	}

	/* candle identifier */
	len = cttostr(buf, sizeof(buf), nxct);

	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);

	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, _1st);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, last);
	buf[len++] = '\t';
	if (mindlt != NOT_A_TIME) {
		len += tvtostr(buf + len, sizeof(buf) - len, mindlt);
	}
	buf[len++] = '\t';
	if (maxdlt != 0) {
		len += tvtostr(buf + len, sizeof(buf) - len, maxdlt);
	}

	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, minask);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxbid);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, minspr);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxspr);

	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, maxbsz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, maxasz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, -maxbim);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, maxsim);

	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxdu);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, maxdd);

	buf[len++] = '\n';
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}


#include "candle.yucc"

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
