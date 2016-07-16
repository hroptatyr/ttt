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
static quo_t quo;
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
		quo = (quo_t){b, a};
	}
	return 0;
}

static void
skim(bool lastp)
{
	typedef enum {
		RGM_FLAT,
		RGM_LONG,
		RGM_SHORT,
		NRGM,
	} rgm_t;
	static quo_t old;
	static rgm_t rgm[NRGM];
	static px_t pnl[NRGM];
	px_t new[NRGM];

	/* calculate returns */
	new[RGM_FLAT] = 0.df;
	new[RGM_LONG] = quo.b - old.a;
	new[RGM_SHORT] = old.b - quo.a;

	/* calculate PREV(m, l) */
	printf("%lu.%03lu000000", metr / MSECS, metr % MSECS);
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
		printf("\tPREV(m, %u) = %u (%f)", i, rgm[i], (double)new[i]);
	}
	putchar('\n');

	/* memorise quo */
	old = quo;
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
