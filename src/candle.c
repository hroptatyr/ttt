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
#include "dfp754_d32.h"
#include "dfp754_d64.h"
#include "tv.h"
#include "nifty.h"

typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr

/* relevant tick dimensions */
typedef struct {
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	qx_t b;
	qx_t a;
} qty_t;

static tvu_t intv;


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
static tv_t _1st = NATV;
static tv_t last;
static tv_t mindlt = NATV;
static tv_t maxdlt;

static char cont[64];
static size_t conz;

static void prnt_cndl(void);

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
		t++;
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
	return mktime(tm) * NSECS;
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

	mindlt = NATV;
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
	if (UNLIKELY((t = strtotv(ln, &on)) == NATV)) {
		return -1;
	} else if (UNLIKELY(*on++ != '\t')) {
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

	if (UNLIKELY(_1st == NATV)) {
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
	len = tvutostr(buf, sizeof(buf), (tvu_t){nxct, intv.u});

	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);

	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, _1st);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, last);
	buf[len++] = '\t';
	if (mindlt != NATV) {
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
		intv = strtotvu(argi->interval_arg, NULL);
		if (!intv.t) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		} else if (!intv.u) {
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
