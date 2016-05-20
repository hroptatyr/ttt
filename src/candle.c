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

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t p;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;


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

static inline __attribute__((const, pure)) px_t
min_px(px_t p1, px_t p2)
{
	return p1 < p2 ? p1 : p2;
}

static inline __attribute__((const, pure)) px_t
max_px(px_t p1, px_t p2)
{
	return p1 > p2 ? p1 : p2;
}


static tv_t metr;
static px_t _1stb, _1sta;
static px_t lastb, lasta;
static px_t maxb, mina;
static px_t minb, maxa;
static px_t maxdd, maxdu;
static px_t mindb, maxdb;
static px_t minda, maxda;

static void
crst(void)
{
	_1stb = _1sta = __DEC32_MAX__;
	return;
}

static int
cndl(void)
{
	char buf[256U];
	size_t bi;

	bi = snprintf(buf, sizeof(buf), "%lu", metr);
	/* open */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, _1stb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, _1sta);
	/* high */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxa);
	/* low */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, minb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, mina);
	/* close */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, lastb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, lasta);
	/* max-inc */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxdb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, minda);
	/* max-dec */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, mindb);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxda);
	/* maxdn/maxdu */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxdd);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxdu);

	buf[bi++] = '\n';
	fwrite(buf, 1, bi, stdout);

	crst();
	return 0;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	char *on;
	px_t b, a;
	tv_t oldm = metr;

	/* time value up first */
	with (long unsigned int s, x) {

		if (ln[20U] != '\t') {
			return -1;
		} else if (!(s = strtoul(ln, &on, 10))) {
			return -1;
		} else if (*on++ != '.') {
			return -1;
		} else if ((x = strtoul(on, &on, 10), *on++ != '\t')) {
			return -1;
		}
		/* assign with minute resolution */
		metr = s / 60U;
	}

	/* do we need to draw another candle? */
	if (UNLIKELY(metr > oldm && _1stb != __DEC32_MAX__)) {
		/* yip */
		cndl();
	}

	/* now comes a descriptor */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;
	/* and an IP/port pair */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* ok do the calc'ing */
	if (UNLIKELY(_1stb == __DEC32_MAX__)) {
		_1stb = minb = maxb = lastb = b;
		_1sta = mina = maxa = lasta = a;
		mindb = maxdb = maxdd = 0.df;
		minda = maxda = maxdu = 0.df;
		return 0;
	}
	/* min+max */
	maxb = max_px(maxb, b);
	maxa = max_px(maxa, a);
	minb = min_px(minb, b);
	mina = min_px(mina, a);

	/* lucky we've got lastb/lasta */
	with (px_t db = b - lastb, da = a - lasta) {
		maxdb = max_px(maxdb, db);
		mindb = min_px(mindb, db);
		maxda = max_px(maxda, da);
		minda = min_px(minda, da);
	}

	with (px_t du = b - minb, dd = a - maxa) {
		maxdu = max_px(maxdu, du);
		maxdd = min_px(maxdd, dd);
	}

	/* always set these */
	lastb = b;
	lasta = a;
	return 0;
}


#include "candle.yucc"

int
main(int argc, char *argv[])
{
/* grep -F 'EURUSD FAST Curncy' | cut -f1,5,6 */
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

		crst();
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			push_beef(line, nrd);
		}

		/* finalise our findings */
		free(line);
	}

out:
	yuck_free(argi);
	return rc;
}
