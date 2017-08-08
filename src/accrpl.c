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
#include <time.h>
#include <math.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
#else
static inline __attribute__((pure, const)) _Decimal64
fabsd64(_Decimal64 x)
{
	return x >= 0 ? x : -x;
}
#endif
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
#define fabsqx		fabsd64
#define quantizeqx	quantized64

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t p;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	qx_t base;
	/* terms account gross */
	qx_t term;
	/* terms account commissions */
	qx_t comm;
	/* terms account spread *gains* */
	qx_t sprd;
} acc_t;

typedef struct {
	/* timestamp of valuation */
	tv_t t;
	/* valuation in terms */
	qx_t nlv;
	/* commission route-through */
	qx_t comm;
} eva_t;

static unsigned int grossp;
static const char *cont;
static size_t conz;
/* rewind metronome by this */
static tv_t msub;

static FILE *afp;


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

static inline size_t
memncpy(void *restrict buf, const void *src, size_t n)
{
	return memcpy(buf, src, n), n;
}


static acc_t a;
static acc_t l = {0.dd, 0.dd, 0.dd, 0.dd};

static tv_t
next_acc(void)
{
	static char *line;
	static size_t llen;
	static tv_t newm;
	ssize_t nrd;
	char *on;

	/* assign previous next_quo as current quo */
	a.base = 0.dd;
	a.term = 0.dd;
	a.comm = 0.dd;
	/* leave out a.sprd as we need to accumulate that ourself */

again:
	if (UNLIKELY((nrd = getline(&line, &llen, afp)) <= 0)) {
		free(line);
		line = NULL;
		llen = 0UL;
		return NATV;
	}
	const char *const eol = line + nrd;

	/* snarf metronome */
	newm = strtotv(line, &on), on++;
	if (grossp > 1U && UNLIKELY(!memcmp(on, "EXE\t", 4U))) {
		qx_t b, sprd;

		on += 4U;
		/* instrument name */
		on = memchr(on, '\t', eol - on);
		if (on++ == NULL) {
			goto again;
		}
		/* base qty */
		b = strtoqx(on, &on);
		if (on++ >= eol) {
			goto again;
		}
		/* terms qty */
		on = memchr(on, '\t', eol - on);
		if (on++ == NULL) {
			goto again;
		}
		/* spread */
		sprd = strtoqx(on, NULL) / 2.dd;
		a.sprd += fabsqx(b) * sprd;
		goto again;
	}
	/* make sure we're talking accounts */
	if (UNLIKELY(memcmp(on, "ACC\t", 4U))) {
		goto again;
	}
	on += 4U;

	/* get currency indicator */
	on = memchr(on, '\t', eol - on);
	if (UNLIKELY(on == NULL)) {
		goto again;
	}
	/* snarf the base amount */
	a.base = strtoqx(++on, &on);
	if (UNLIKELY(on >= eol)) {
		goto again;
	} else if (l.base == a.base) {
		/* nothing changed */
		goto again;
	}
	/* terms */
	a.term = strtoqx(++on, &on);
	if (UNLIKELY(on >= eol)) {
		goto again;
	}
	/* base commissions */
	with (qx_t c) {
		c = strtoqx(++on, &on);
		if (UNLIKELY(on >= eol)) {
			goto again;
		}
		/* terms commissions */
		c += strtoqx(++on, &on);
		a.comm = !grossp ? c : 0.dd;
	}
	return newm;
}

static inline qx_t
calc_rpnl(void)
{
	static qx_t accpnl = 0.dd;
	qx_t this = (a.term * l.base - a.base * l.term) / (l.base - a.base);
	qx_t pnl = this - accpnl;

	/* keep state */
	accpnl = this;
	return pnl;
}

static inline qx_t
calc_rcom(void)
{
	static qx_t acccom = 0.dd;
	qx_t this = (a.comm * l.base - a.base * l.comm) / (l.base - a.base);
	qx_t com = this - acccom;

	/* keep state */
	acccom = this;
	return com;
}

static inline qx_t
calc_rspr(void)
{
	static qx_t accspr = 0.dd;
	qx_t this = (a.sprd * l.base - a.base * l.sprd) / (l.base - a.base);
	qx_t spr = this - accspr;

	/* keep state */
	accspr = this;
	return spr;
}

static void
send_rpl(tv_t m, tv_t rt, qx_t r)
{
	char buf[256U];
	size_t len = 0U, lem;

	len += tvtostr(buf + len, sizeof(buf) - len, m - msub);
	buf[len++] = '\t';
	lem = len;
	len += memncpy(buf + len, "RPL\t", 4U);
	len += memncpy(buf + len, cont, conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, r);
	buf[len++] = '\n';

	/* exposure time */
	len += memncpy(buf + len, buf + 0, lem);
	len += memncpy(buf + len, "XPT\t", 4U);
	len += memncpy(buf + len, cont, conz);
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, rt - m);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return;
}


static int
offline(void)
{
	tv_t alst = NATV;
	tv_t amtr;

	amtr = next_acc();

	do {
		qx_t r = calc_rpnl();
		r += calc_rcom();
		r += calc_rspr();

		if (LIKELY(alst < NATV) && l.base) {
			send_rpl(alst, amtr, quantizeqx(r, l.term));
		}
	} while ((l = a, alst = amtr, amtr = next_acc()) < NATV);
	return 0;
}


#include "accrpl.yucc"

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
		cont = argi->pair_arg;
		conz = strlen(cont);
	}

	if (argi->rewind_arg) {
		msub = strtotv(argi->rewind_arg, NULL);
	}

	grossp = argi->gross_flag;

	if (UNLIKELY((afp = stdin) == NULL)) {
		errno = 0, serror("\
Error: cannot open ACCOUNTS file");
		rc = 1;
		goto out;
	}		

	/* offline mode */
	rc = offline();

	fclose(afp);
out:
	yuck_free(argi);
	return rc;
}
