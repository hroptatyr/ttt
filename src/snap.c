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
} qua_t;


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


static tv_t intv = 60U * MSECS;

static tv_t metr;
static quo_t last;
static qua_t that;

static char cont[64];
static size_t conz;

static void
crst(void)
{
	last = (quo_t){nand32(""), nand32("")};
	return;
}

static int
snap(void)
{
	char buf[256U];
	size_t bi;

	bi = tvtostr(buf, sizeof(buf), (metr + 1ULL) * intv);
	buf[bi++] = '\t';
	buf[bi++] = '\t';
	buf[bi++] = '\t';
	bi += (memcpy(buf + bi, cont, conz), conz);
	buf[bi++] = '\t';
	if (LIKELY(!isnand32(last.b))) {
		bi += pxtostr(buf + bi, sizeof(buf) - bi, last.b);
	}
	buf[bi++] = '\t';
	if (LIKELY(!isnand32(last.a))) {
		bi += pxtostr(buf + bi, sizeof(buf) - bi, last.a);
	}
	if (that.b > 0.dd && that.a > 0.dd) {
		buf[bi++] = '\t';
		bi += qxtostr(buf + bi, sizeof(buf) - bi, that.b);
		buf[bi++] = '\t';
		bi += qxtostr(buf + bi, sizeof(buf) - bi, that.a);
	}

	buf[bi++] = '\n';
	fwrite(buf, 1, bi, stdout);
	return 0;
}

static int
push_init(char *ln, size_t UNUSED(lz))
{
	quo_t this;
	const char *ip;
	size_t iz;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	}
	/* align metronome to interval */
	metr--;
	metr /= intv;

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = on, '\t')) == NULL)) {
		return -1;
	}
	iz = on++ - ip;

	/* snarf quotes */
	if (!(this.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(this.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* snarf quantities */
	if (*on == '\t') {
		that.b = strtoqx(++on, &on);
		that.a = strtoqx(++on, &on);
	}

	/* we're init'ing, so everything is the last value */
	last = this;

	memcpy(cont, ip, conz = iz);
	return 0;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	tv_t nmtr;
	quo_t this;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((nmtr = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	}
	/* align metronome to interval */
	nmtr--;
	nmtr /= intv;

	/* do we need to draw another candle? */
	if (UNLIKELY(nmtr > metr)) {
		/* yip */
		snap();
		/* and reset */
		crst();
		/* assign new metr */
		metr = nmtr;
	}

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

	/* keep some state */
	last = this;
	return 0;
}


#include "snap.yucc"

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
		intv *= MSECS;
	}

	/* reset candles */
	crst();
	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0 &&
		       push_init(line, nrd) < 0);
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			push_beef(line, nrd);
		}
		/* produce the final candle */
		snap();

		/* finalise our findings */
		free(line);
	}

out:
	yuck_free(argi);
	return rc;
}
