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
#include "hash.h"
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
	tv_t t;
	px_t p;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;

/* the name of the new contract */
static char *cont;
static size_t conz;


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

static size_t
xstrlncpy(char *restrict dst, size_t dsz, const char *src, size_t ssz)
{
	if (ssz > dsz) {
		ssz = dsz - 1U;
	}
	memcpy(dst, src, ssz);
	dst[ssz] = '\0';
	return ssz;
}

static size_t
xstrlcpy(char *restrict dst, const char *src, size_t dsz)
{
	size_t ssz = strlen(src);
	if (ssz > dsz) {
		ssz = dsz - 1U;
	}
	memcpy(dst, src, ssz);
	dst[ssz] = '\0';
	return ssz;
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


static void
send_sprd(tv_t metr, quo_t quo)
{
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), metr);
	buf[len++] = '\t';
	buf[len++] = '\t';
	buf[len++] = '\t';
	len += xstrlncpy(buf + len, sizeof(buf) - len, cont, conz);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, quo.b);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, quo.a);
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	return;
}


static size_t nlegs;
static hx_t hxs[8U];
static px_t lev[8U];

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	static px_t legquo[2U * countof(hxs)];
	static uint64_t seen;
	char *on;
	unsigned int which;
	px_t b, a;
	tv_t t;

#define LEGBID(x)	(legquo[(x) * countof(hxs) + 0U])
#define LEGASK(x)	(legquo[(x) * countof(hxs) + 1U])
#define LEGQUO(x, y)	(legquo[(x) * countof(hxs) + (y)])

	if (UNLIKELY((t = strtotv(ln, &on)) == NATV)) {
		/* got metronome cock-up */
		return -1;
	}

	with (hx_t hx) {
		if (UNLIKELY(!(hx = strtohx(++on, &on)) || *on != '\t')) {
			return -1;
		}
		for (which = 0U; which < nlegs; which++) {
			if (hx == hxs[which]) {
				goto snarf;
			}
		}
		/* not for us this isn't, is it? */
		return -1;
	}
snarf:
	/* snarf quotes */
	if ((b = strtopx(++on, &on)) && *on == '\t' &&
	    (a = strtopx(++on, &on)) && (*on == '\n' || *on == '\t')) {
		LEGQUO(which, lev[which] < 0.df) = b * lev[which];
		LEGQUO(which, lev[which] > 0.df) = a * lev[which];
		seen |= 1ULL << which;
	}

	if (LIKELY((seen + 1ULL) >> nlegs)) {
		/* generate spread */
		b = 0.df;
		a = 0.df;
		for (size_t i = 0U; i < nlegs; i++) {
			b += LEGBID(i);
			a += LEGASK(i);
		}

		/* and send him off */
		send_sprd(t, (quo_t){b, a});
	}
	return 0;
}


#include "spread.yucc"

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

	if (!argi->nargs || argi->nargs > countof(hxs)) {
		errno = 0, serror("\
Error: spread instrument needs legs, %zu max", countof(hxs));
		rc = 1;
		goto out;
	}

	for (size_t i = 0U; i < argi->nargs; i++) {
		const char *s = argi->args[i];
		char *on;

		if (!(lev[i] = strtopx(s, &on))) {
			switch (*s) {
			case '+':
				lev[i] = 1.df;
				break;
			case '-':
				lev[i] = -1.df;
				break;
			default:
				errno = 0, serror("\
Error: instrument must be prefixed with +-[LEVER]");
				rc = 1;
				goto out;
			}
		}
		hxs[i] = hash(on, strlen(on));
	}

	/* generate the new name */
	if (argi->name_arg) {
		cont = argi->name_arg;
		conz = strlen(cont);
	} else {
		static char buf[128U];
		size_t bi = 0U;

		for (size_t i = 0U; i < argi->nargs && bi < sizeof(buf); i++) {
			const char *arg = argi->args[i];

			/* overread numbers and stuff in arg */
			for (; (unsigned)(*arg ^ '0') < 10U ||
				     *arg == '+' || *arg == '-' ||
				     *arg == '.'; arg++);
			if (lev[i] > 0.df) {
				buf[bi++] = '+';
			}
			bi += pxtostr(buf + bi, sizeof(buf) - bi, lev[i]);
			bi += xstrlcpy(buf + bi, arg, sizeof(buf) - bi);
		}
		/* set contract */
		cont = buf;
		conz = bi;
	}

	/* set up globals */
	nlegs = argi->nargs;
	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

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
