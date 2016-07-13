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
#include "hash.h"

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
	tv_t t;
	px_t p;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;

static px_t thresh;


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

static hx_t
strtohx(const char *x, char **on)
{
	char *ep;
	hx_t res;

	if (UNLIKELY((ep = strchr(x, '\t')) == NULL)) {
		return 0;
	}
	res = hash(x, ep - x);
	if (LIKELY(on != NULL)) {
		*on = ep;
	}
	return res;
}

static tv_t
strtotv(const char *ln, char **endptr)
{
	char *on;
	tv_t r;

	/* time value up first */
	with (long unsigned int s, x) {
		if (UNLIKELY(!(s = strtoul(ln, &on, 10)))) {
			return NOT_A_TIME;
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


static hx_t hxs;
static tik_t bid;
static tik_t ask;
static tv_t metr;
static const char *cont;
static size_t conz;

static int
push_beef(const char *ln, size_t UNUSED(lz))
{
	char *on;
	px_t b, a;

	with (hx_t hx) {
		if (UNLIKELY(!(hx = strtohx(ln, &on)) || *on != '\t')) {
			return -1;
		} else if (hxs != hx) {
			return -1;
		}
	}
	/* snarf quotes */
	if (*++on != '\t' && *on != '\n' &&
	    (b = strtopx(on, &on)) && *on++ == '\t' &&
	    (a = strtopx(on, &on)) && (*on == '\n' || *on == '\t')) {
		bid = (tik_t){metr, b};
		ask = (tik_t){metr, a};
	}
	return 0;
}

static void
skim(bool lastp)
{
	static tik_t hi_bid = {.p = -__DEC32_MAX__};
	static tik_t lo_ask = {.p = __DEC32_MAX__};
	static tik_t h2_bid;
	static tik_t l2_ask;
	static enum {
		RGM_FLAT,
		RGM_LONG,
		RGM_SHORT,
	} rgm;

	static void buy(tik_t x)
	{
		static tik_t bot;
		char buf[256U];
		size_t len;

		len = tvtostr(buf, sizeof(buf), x.t);
		buf[len++] = '\t';
		len += (memcpy(buf + len, "LONG\t", 5U), 5U);
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\n';

		/* no limit price */
		//len += pxtostr(buf + len, sizeof(buf) - len, x.p);
		fwrite(buf, 1, len, stdout);

		lo_ask = l2_ask;

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

		bot = x;
	}

	static void sell(tik_t x)
	{
		static tik_t sld;
		char buf[256U];
		size_t len;

		len = tvtostr(buf, sizeof(buf), x.t);
		buf[len++] = '\t';
		len += (memcpy(buf + len, "SHORT\t", 6U), 6U);
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\n';

		/* no limit price */
		//len += pxtostr(buf + len, sizeof(buf) - len, x.p);
		fwrite(buf, 1, len, stdout);

		hi_bid = h2_bid;

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

		sld = x;
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
		return;
	}

	if (bid.p > hi_bid.p) {
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
		hi_bid = bid;
		l2_ask = ask;
	} else if (bid.p > h2_bid.p) {
		h2_bid = bid;
	}
	if (ask.p < lo_ask.p) {
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
		lo_ask = ask;
		h2_bid = bid;
	} else if (ask.p < l2_ask.p) {
		l2_ask = ask;
	}
	return;
}


static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		char *on;
		tv_t t;

		if (UNLIKELY((t = strtotv(line, &on)) == NOT_A_TIME)) {
			/* got metronome cock-up */
			;
		} else if (UNLIKELY(push_beef(on, nrd) < 0)) {
			/* data is fucked or not for us */
			;
		} else {
			/* make sure we're talking current time */
			metr = t;
			/* see what we can trade */
			skim(false);
		}
	}
	/* finalise our findings */
	skim(true);
	free(line);
	return 0;
}


#include "opt.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	} else if (!argi->pair_arg) {
		errno = 0, serror("\
Error: --pair argument is mandatory, see --help.");
		rc = 1;
		goto out;
	}

	/* assign front and hind */
	cont = argi->pair_arg;
	conz = strlen(argi->pair_arg);
	hxs = hash(cont, conz);

	if (argi->thresh_arg) {
		thresh = strtopx(argi->thresh_arg, NULL);
	}

	/* offline mode */
	rc = offline();

out:
	yuck_free(argi);
	return rc;
}
