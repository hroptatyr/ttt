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

typedef enum {
	RGM_FLAT,
	RGM_LONG,
	RGM_SHORT,
	RGM_CANCEL,
	NRGM = RGM_CANCEL,
	RGM2POW,
} rgm_t;

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
static tv_t metr;
static tik_t bid, ask;
static const char *cont = "";
static size_t conz;
/* state */
static tik_t bstb = {.p = __DEC32_MOST_NEGATIVE__};
static tik_t bsta = {.p = __DEC32_MOST_POSITIVE__};

static int
push_beef(const char *ln, size_t UNUSED(lz))
{
	char *on;
	int rc = -1;

	with (hx_t hx) {
		if (UNLIKELY(!(hx = strtohx(ln, &on)) || *on++ != '\t')) {
			return -1;
		} else if (hxs != hx) {
			return -1;
		}
	}
	/* snarf quotes */
	if (*on >= ' ') {
		bid = (tik_t){metr, strtopx(on, &on)};
		rc = 0;
	}
	if (*on++ != '\t') {
		;
	} else if (*on >= ' ') {
		ask = (tik_t){metr, strtopx(on, &on)};
		rc = 0;
	}
	return rc;
}

static void
dgst(rgm_t r, tik_t q)
{
	static const char *rgms[] = {
		[RGM_FLAT] = "UNK",
		[RGM_LONG] = "LONG",
		[RGM_SHORT] = "SHORT",
		[RGM_CANCEL] = "CANCEL",
	};
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), q.t);
	buf[len++] = '\t';
	len += (memcpy(buf + len, rgms[r], r + 3U), r + 3U);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	return;
}

static void
skim(bool lastp)
{
	static rgm_t r;

	if (bid.p >= bstb.p) {
		/* track keeping */
		bstb = bid;
	} else if (r != RGM_SHORT && bstb.p - ask.p > thresh) {
		/* remember current regime */
		r = RGM_SHORT;
		/* look for a good long */
		bsta = ask;
		/* trade */
		dgst(r, bstb);
	}

	if (ask.p <= bsta.p) {
		/* track keeping */
		bsta = ask;
	} else if (r != RGM_LONG && bid.p - bsta.p > thresh) {
		/* remember current regime */
		r = RGM_LONG;
		/* look for a good short */
		bstb = bid;
		/* trade */
		dgst(r, bsta);
	}

	if (UNLIKELY(lastp)) {
		switch (r) {
		default:
		case RGM_FLAT:
			break;
		case RGM_LONG:
			dgst(RGM_CANCEL, bstb);
			break;
		case RGM_SHORT:
			dgst(RGM_CANCEL, bsta);
			break;
		}
		r = RGM_FLAT;
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

		if (UNLIKELY((metr = strtotv(line, &on)) == NOT_A_TIME)) {
			/* got metronome cock-up */
			;
		} else if (UNLIKELY(push_beef(on, nrd) < 0)) {
			/* data is fucked or not for us */
			;
		} else {
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
	}

	if (argi->pair_arg) {
		/* assign front and hind */
		cont = argi->pair_arg;
		conz = strlen(argi->pair_arg);
	}
	/* hash contract designator */
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
