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

static inline __attribute__((const, pure)) quo_t
max_quo(quo_t q1, quo_t q2)
{
	return q1.b > q2.b ? q1 : q2;
}

static inline __attribute__((const, pure)) quo_t
min_quo(quo_t q1, quo_t q2)
{
	return q1.a < q2.a ? q1 : q2;
}


static tv_t intv = 60U;
static px_t thresh = 0.000036df;

static tv_t metr;
static quo_t _1st;
static quo_t last;
static quo_t maxb;
static quo_t mina;
static px_t maxdd, maxdu;
static px_t mindb, maxdb;
static px_t minda, maxda;
static px_t lpnl, spnl;

static void
crst(void)
{
	_1st = (quo_t){__DEC32_MAX__, 0.df};
	lpnl = spnl = 0.df;
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
	bi += pxtostr(buf + bi, sizeof(buf) - bi, _1st.b);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, _1st.a);
	/* high */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxb.b);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, maxb.a);
	/* low */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, mina.b);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, mina.a);
	/* close */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, last.b);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, last.a);
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
	/* optimal profits l/s */
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, lpnl);
	buf[bi++] = '\t';
	bi += pxtostr(buf + bi, sizeof(buf) - bi, spnl);

	buf[bi++] = '\n';
	fwrite(buf, 1, bi, stdout);

	crst();
	return 0;
}

static void
optim(bool lastp)
{
	static tik_t hi_bid = {.p = -__DEC32_MAX__};
	static tik_t lo_ask = {.p = __DEC32_MAX__};
	static tik_t h2_bid;
	static tik_t l2_ask;
	static tv_t omtr;
	static enum {
		RGM_FLAT,
		RGM_LONG,
		RGM_SHORT,
	} rgm;
	/* last trades */
	static tik_t bot;
	static tik_t sld;

	static void buy(tik_t x)
	{
		/* calc profits */
		spnl += sld.p ? sld.p - x.p : 0.df;
		lo_ask = l2_ask;
		bot = x;

		switch (rgm) {
		case RGM_FLAT:
			rgm = RGM_LONG;
			break;
		case RGM_LONG:
			abort();
			break;
		case RGM_SHORT:
			rgm = RGM_FLAT;
			break;
		}
	}

	static void sell(tik_t x)
	{
		/* calc profits */
		lpnl += bot.p ? x.p - bot.p : 0.df;
		hi_bid = h2_bid;
		sld = x;

		switch (rgm) {
		case RGM_FLAT:
			rgm = RGM_SHORT;
			break;
		case RGM_LONG:
			rgm = RGM_FLAT;
			break;
		case RGM_SHORT:
			abort();
			break;
		}
	}

	if (UNLIKELY(lastp)) {
		assert(rgm == RGM_FLAT);
		if (hi_bid.p - lo_ask.p > thresh &&
		    h2_bid.p - lo_ask.p > thresh) {
			if (hi_bid.t < lo_ask.t) {
				sell(hi_bid);
				buy(lo_ask);
			} else if (hi_bid.t > lo_ask.t) {
				buy(lo_ask);
				sell(hi_bid);
			}
		} else if (hi_bid.p - lo_ask.p > thresh &&
			   hi_bid.p - l2_ask.p > thresh) {
			if (lo_ask.t < hi_bid.t) {
				buy(lo_ask);
				sell(hi_bid);
			} else if (lo_ask.t > hi_bid.t) {
				sell(hi_bid);
				buy(lo_ask);
			}
		}

		hi_bid = (tik_t){.p = -__DEC32_MAX__};
		lo_ask = (tik_t){.p = __DEC32_MAX__};
		h2_bid = (tik_t){};
		l2_ask = (tik_t){};
		rgm = RGM_FLAT;
		omtr = 0U;
		bot.p = 0.df;
		sld.p = 0.df;
		return;
	}

	if (last.b > hi_bid.p) {
		if (hi_bid.p - lo_ask.p > thresh &&
		    hi_bid.p - l2_ask.p > thresh) {
			if (lo_ask.t < hi_bid.t) {
				buy(lo_ask);
				sell(hi_bid);
			} else if (lo_ask.t > hi_bid.t) {
				sell(hi_bid);
				buy(lo_ask);
			}
		}
		/* track-keeping */
		hi_bid = (tik_t){omtr, last.b};
		l2_ask = (tik_t){omtr, last.a};
	} else if (last.b > h2_bid.p) {
		h2_bid = (tik_t){omtr, last.b};
	}
	if (last.a < lo_ask.p) {
		if (hi_bid.p - lo_ask.p > thresh &&
		    h2_bid.p - lo_ask.p > thresh) {
			if (hi_bid.t < lo_ask.t) {
				sell(hi_bid);
				buy(lo_ask);
			} else if (hi_bid.t > lo_ask.t) {
				buy(lo_ask);
				sell(hi_bid);
			}
		}
		/* track-keeping */
		lo_ask = (tik_t){omtr, last.a};
		h2_bid = (tik_t){omtr, last.b};
	} else if (last.a < l2_ask.p) {
		l2_ask = (tik_t){omtr, last.a};
	}
	omtr++;
	return;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	tv_t oldm = metr;
	quo_t this;
	char *on;

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
		metr = s / intv;
	}

	/* do we need to draw another candle? */
	if (UNLIKELY(metr > oldm && _1st.b != __DEC32_MAX__)) {
		/* yield */
		optim(true);
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
	if (!(this.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(this.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* ok do the calc'ing */
	if (UNLIKELY(_1st.b == __DEC32_MAX__)) {
		_1st = last = maxb = mina = this;
		mindb = maxdb = maxdd = 0.df;
		minda = maxda = maxdu = 0.df;
		return 0;
	}
	/* min+max */
	maxb = max_quo(maxb, this);
	mina = min_quo(mina, this);

	/* lucky we've got lastb/lasta */
	with (px_t db = this.b - last.b, da = this.a - last.a) {
		maxdb = max_px(maxdb, db);
		mindb = min_px(mindb, db);
		maxda = max_px(maxda, da);
		minda = min_px(minda, da);
	}

	with (px_t du = this.a - mina.b, dd = this.b - maxb.a) {
		maxdu = max_px(maxdu, du);
		maxdd = min_px(maxdd, dd);
	}

	/* keep some state */
	last = this;

	/* skim for optim */
	optim(false);
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

	if (argi->interval_arg) {
		if (!(intv = strtoul(argi->interval_arg, NULL, 10))) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		}
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
