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
static unsigned char *prev;
static tv_t *pmtr;
static size_t pren, prez;

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
skim(void)
{
	static quo_t old;
	static tv_t omtr;
	static px_t pnl[NRGM];
	rgm_t rgm[NRGM];
	px_t new[NRGM];

	/* calculate returns */
	new[RGM_FLAT] = 0.df;
	new[RGM_LONG] = quo.b - old.a;
	new[RGM_SHORT] = old.b - quo.a;

	/* calculate PREV(m, l) */
	for (rgm_t i = RGM_FLAT; i < NRGM; i++) {
		rgm_t maxr = RGM_FLAT;
		px_t maxp = pnl[RGM_FLAT] + new[i];
		for (rgm_t j = RGM_FLAT; ++j < NRGM;) {
			px_t cand = pnl[j] + new[i] - (i != j ? thresh : 0.df);
			if (cand > maxp) {
				maxr = j;
				maxp = cand;
			}
		}
		pnl[i] = maxp;
		rgm[i] = maxr;
	}

	/* append to prev list */
	if (UNLIKELY(pren >= prez)) {
		prez = (prez * 2U) ?: 4096U;
		prev = realloc(prev, prez * sizeof(*prev));
		pmtr = realloc(pmtr, prez * sizeof(*pmtr));
	}
	with (unsigned char x = 0U) {
		for (rgm_t i = RGM_FLAT; i < NRGM; i++) {
			x ^= (unsigned char)(rgm[i] << (i * RGM2POW / 2U));
		}
		prev[pren] = x;
		pmtr[pren] = omtr;
		pren++;
	}

	/* memorise quo */
	old = quo;
	omtr = metr;
	return;
}

static inline void
rvrt(unsigned char *a, size_t n)
{
	for (size_t i = 0U, m = n-- / 2U; i < m; i++) {
		unsigned char tmp = a[n - i];
		a[n - i] = a[i];
		a[i] = tmp;
	}
	return;
}

static void
prnt(tv_t t, rgm_t r)
{
	static const char *rgms[] = {
		[RGM_FLAT] = "UNK",
		[RGM_LONG] = "LONG",
		[RGM_SHORT] = "SHORT",
		[RGM_CANCEL] = "CANCEL",
	};
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), t);
	buf[len++] = '\t';
	len += (memcpy(buf + len, rgms[r], r + 3U), r + 3U);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return;
}

static void
dgst(void)
{
/* this destroys PREV */
	rgm_t r = RGM_FLAT;

	/* revert PREV string */
	rvrt(prev, pren);

	/* now obtain the optimal trades */
	for (size_t i = 0U; i < pren; i++) {
		r = (rgm_t)((prev[i] >> (r * RGM2POW / 2U)) & (RGM2POW - 1U));
		/* by side-effect store the optimal regime */
		prev[i] = (unsigned char)r;
	}

	/* revert once more */
	rvrt(prev, pren);

	/* now go for printing */
	r = RGM_FLAT;
	for (size_t i = 0U; i < pren; r = (rgm_t)prev[i++]) {
		/* only go for edges */
		if (r ^ prev[i]) {
			prnt(pmtr[i], (rgm_t)(prev[i] ?: RGM_CANCEL));
		}
	}
	/* and finish on a flat */
	if (r) {
		prnt(metr, RGM_CANCEL);
	}

	/* finalise PREV */
	if (LIKELY(prev != NULL)) {
		free(prev);
	}
	if (LIKELY(pmtr != NULL)) {
		free(pmtr);
	}
	pren = prez = 0UL;
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
			skim();
		}
	}
	/* finalise our findings */
	dgst();
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
