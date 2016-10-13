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
typedef union {
	struct {
		px_t b;
		px_t a;
	};
	struct {
		px_t m;
		px_t s;
	};
} quo_t;


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


static px_t minask;
static px_t maxbid;
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

static int push_quo(char*, size_t);
static int push_mid(char*, size_t);
static int(*push_beef)(char*, size_t) = push_quo;

static int
push_init(char *ln, size_t UNUSED(lz))
{
	const char *ip;
	size_t iz;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((_1st = last = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = on, '\t')) == NULL)) {
		return -1;
	}
	iz = on++ - ip;

	/* snarf quotes */
	if (!(maxbid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(minask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* check that we're dealing with bids and asks */
	if (maxbid <= minask) {
		/* all is fine */
		minspr = maxspr = minask - maxbid;
	} else {
		/* nope, it's probably mid-point spread */
		minspr = maxspr = minask;
		minask = maxbid;
		push_beef = push_mid;
	}

	/* snarf quantities */
	if (*on == '\t') {
		maxbsz = strtoqx(++on, &on);
		maxasz = strtoqx(++on, &on);
	}

	memcpy(cont, ip, conz = iz);
	return 0;
}

static int
push_quo(char *ln, size_t UNUSED(lz))
{
	tv_t t;
	quo_t q;
	char *on;
	int rc = 0;
	qx_t bsz, asz;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (t < last) {
		fputs("Warning: non-chronological\n", stderr);
		rc = -1;
		goto out;
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
	bsz = strtoqx(++on, &on);
	asz = strtoqx(++on, &on);

	maxbid = max_px(maxbid, q.b);
	minask = min_px(minask, q.a);
	with (px_t s = q.a - q.b) {
		minspr = min_px(minspr, s);
		maxspr = max_px(maxspr, s);
	}

	with (px_t du = q.a - minask, dd = q.b - maxbid) {
		maxdu = max_px(maxdu, du);
		maxdd = min_px(maxdd, dd);
	}

	with (tv_t dlt = t - last) {
		mindlt = min_tv(mindlt, dlt);
		maxdlt = max_tv(maxdlt, dlt);
	}

	maxbsz = max_qx(maxbsz, bsz);
	maxasz = max_qx(maxasz, asz);
	with (qx_t imb = asz - bsz) {
		maxsim = max_qx(maxsim, imb);
		maxbim = min_qx(maxbim, imb);
	}

out:
	/* and store state */
	last = t;
	return rc;
}

static int
push_mid(char *ln, size_t UNUSED(lz))
{
	tv_t t;
	quo_t q;
	char *on;
	int rc = 0;
	qx_t bsz, asz;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (t < last) {
		fputs("Warning: non-chronological\n", stderr);
		rc = -1;
		goto out;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(q.m = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.s = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* snarf quantities */
	bsz = strtoqx(++on, &on);
	asz = strtoqx(++on, &on);

	minask = min_px(minask, q.m);
	maxbid = max_px(maxbid, q.m);
	minspr = min_px(minspr, q.s);
	maxspr = max_px(maxspr, q.s);

	with (tv_t dlt = t - last) {
		mindlt = min_tv(mindlt, dlt);
		maxdlt = max_tv(maxdlt, dlt);
	}

	with (px_t du = q.m - minask, dd = q.m - maxbid) {
		maxdu = max_px(maxdu, du);
		maxdd = min_px(maxdd, dd);
	}

	maxbsz = max_qx(maxbsz, bsz);
	maxasz = max_qx(maxasz, asz);
	with (qx_t imb = asz - bsz) {
		maxsim = max_qx(maxsim, imb);
		maxbim = min_qx(maxbim, imb);
	}

out:
	/* and store state */
	last = t;
	return rc;
}

static void
prnt_sum(void)
{
	char buf[4096U];
	size_t len = 0U;

	fputs("ccy\t_1st\tlast\tmindlt\tmaxdlt\tminask\tmaxbid\tminspr\tmaxspr\tmaxbsz\tmaxasz\tmaxbim\tmaxsim\tmaxdu\tmaxdd\n", stdout);

	len = (memcpy(buf, cont, conz), conz);

	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, _1st);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, last);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, mindlt);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, maxdlt);

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


#include "quosum.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];

	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0 &&
		       push_init(line, nrd) < 0);
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			(void)push_beef(line, nrd);
		}

		/* finalise our findings */
		free(line);
	}

	if (_1st != NOT_A_TIME) {
		prnt_sum();
	}

out:
	yuck_free(argi);
	return rc;
}
