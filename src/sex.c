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
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#else  /* !HAVE_DFP754_H && !HAVE_DFP_STDLIB_H */
static inline __attribute__((pure, const)) _Decimal32
fabsd32(_Decimal32 x)
{
	return x >= 0 ? x : -x;
}
static inline __attribute__((pure, const)) _Decimal64
fabsd64(_Decimal64 x)
{
	return x >= 0 ? x : -x;
}
#endif	/* HAVE_DFP754_H || HAVE_DFP_STDLIB_H */
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
#define NANPX		NAND32
#define isnanpx		isnand32

#define NOT_A_TIME	((tv_t)-1ULL)

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	tv_t t;
	px_t p;
	qx_t q;
	/* spread at the time */
	px_t s;
	/* quote age */
	tv_t g;
} exe_t;

typedef struct {
	qx_t base;
	qx_t term;
	qx_t comb;
	qx_t comt;
} acc_t;

/* regimes
 * these are chosen so that transitions work
 * LONG ^ CLOSE -> SHORT  SHORT ^ CLOSE -> LONG */
typedef enum {
	RGM_UNK = 0b0000U,
	RGM_LONG = 0b0001U,
	RGM_SHORT = 0b0010U,
	RGM_CANCEL = 0b0011U,
	RGM_TIMEOUT = 0b0100U,
	RGM_LONGRVRS = 0b0101U,
	RGM_SHORTRVRS = 0b0110U,
	/* make this one coincide with RGM_CANCEL in the LSBs */
	RGM_EMERGCLOSE = 0b111U,
} rgm_t;


/* orders */
typedef struct {
	tv_t t;
	rgm_t r;
	tv_t gtd;
	qx_t q;
	px_t lp;
	px_t tp;
	px_t sl;
	/* number of rejected executions */
	unsigned int nr;
} ord_t;

static tv_t exe_age = 60U;
static qx_t qty = 1.dd;
static px_t comb = 0.df;
static px_t comt = 0.df;
static unsigned int absq;
static unsigned int maxq;
static tv_t rtry;


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


static const char *cont;
static size_t conz;
static hx_t conx;

static inline char*
strcws(const char *x)
{
	const unsigned char *y;
	for (y = (const unsigned char*)x; *y >= ' '; y++);
	return deconst(y);
}

static hx_t
strtohx(const char *x, char **on)
{
	char *ep;
	hx_t res;

	ep = strcws(x);
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
		if (UNLIKELY((s = strtoul(ln, &on, 10), on == ln))) {
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

static inline __attribute__((const, pure)) tv_t
max_tv(tv_t t1, tv_t t2)
{
	return t1 > t2 ? t1 : t2;
}


static ssize_t
send_exe(exe_t x)
{
/* exe encodes delta to metronome and delay */
	static const char vexe[] = "EXE\t";
	static const char vrej[] = "REJ\t";
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), x.t);
	buf[len++] = '\t';
	len += (memcpy(buf + len, (x.q ? vexe : vrej), strlenof(vexe)),
		strlenof(vexe));
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.q);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, x.p);
	/* spread at the time */
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, x.s);
	/* how long was quote standing */
	buf[len++] = '\t';
	len += tvtostr(buf + len, sizeof(buf) - len, x.g);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static ssize_t
send_acc(tv_t t, acc_t a)
{
	static const char verb[] = "ACC\t";
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), t);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.base);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.term);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.comb);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.comt);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static exe_t
try_exec(ord_t o, quo_t q)
{
/* this takes an order + quotes and executes it at market price */
	const tv_t t = max_tv(o.t, q.t);
	px_t p;
	px_t s = q.a - q.b;
	tv_t age = t - q.t;

	switch (o.r) {
	case RGM_LONG:
	case RGM_SHORT:
		if (o.q > 0.dd && (isnanpx(p = q.a) || p > o.lp)) {
			/* no can do exec */
			break;
		} else if (o.q < 0.dd && (isnanpx(p = q.b) || p < o.lp)) {
			/* no can do exec */
			break;
		}
		return (exe_t){t, p, o.q, s, age};

	case RGM_CANCEL:
	case RGM_EMERGCLOSE:
		if (o.q > 0.dd) {
			p = q.b;
		} else if (o.q < 0.dd) {
			p = q.a;
		} else {
			p = 0.df;
		}
		if (UNLIKELY(isnanpx(p))) {
			/* reject */
			break;
		}
		return (exe_t){t, p, -o.q, s, age};

	default:
		/* otherwise do nothing */
		break;
	}
	return (exe_t){t, 0.df, 0.dd, s, age};
}

static acc_t
alloc(acc_t a, exe_t x, px_t cb/*base comm*/, px_t ct/*terms coommission*/)
{
/* allocate execution X to account A. */
	/* calc accounts */
	a.base += x.q;
	a.term -= x.q * x.p;
	a.comb -= fabsd64(x.q) * cb;
	a.comt -= fabsd64(x.q * x.p) * ct;

	/* quantize to make them look nicer */
	a.base = quantized64(a.base, 0.00dd);
	a.term = quantized64(a.term, 0.00dd);
	a.comb = quantized64(a.comb, 0.00dd);
	a.comt = quantized64(a.comt, 0.00dd);
	return a;
}


static ord_t
yield_ord(FILE *ofp)
{
	static char *line;
	static size_t llen;
	char *on;
	ord_t o;
	tv_t t;

retry:
	if (UNLIKELY(getline(&line, &llen, ofp) <= 0)) {
		free(line);
		line = NULL;
		llen = 0UL;
		return (ord_t){NOT_A_TIME};
	}
	/* otherwise snarf the order line */
	if (UNLIKELY((t = strtotv(line, &on)) == NOT_A_TIME || on == line)) {
		goto retry;
	}
	/* read the order */
	switch (*on) {
		px_t p;
		hx_t hx;

	case 'L'/*ONG*/:
		o = (ord_t){t, RGM_LONG, .q = qty, .lp = INFD32};
		on += 4U;
		goto ord;
	case 'S'/*HORT*/:
		o = (ord_t){t, RGM_SHORT, .q = -qty, .lp = -INFD32};
		on += 5U;
		goto ord;
	case 'C'/*ANCEL*/:
	case 'E'/*MERG*/:
		on = strcws(on);
		if (LIKELY(*on++ == '\t' && (hx = strtohx(on, &on)))) {
			/* got a tab and a currency indicator */
			if (UNLIKELY(hx != conx && conx)) {
				/* but it's not for us */
				goto retry;
			}
		}
		o = (ord_t){t, RGM_CANCEL, .q = 0.dd};
		break;
	default:
		goto retry;

	ord:
		if (UNLIKELY(*on++ != '\t')) {
			break;
		}
		if (UNLIKELY(!(hx = strtohx(on, &on)))) {
			/* no currency indicator */
			break;
		} else if (UNLIKELY(hx != conx && conx)) {
			/* not for us this one isn't */
			goto retry;
		}
		/* otherwise snarf the limit price */
		if ((p = strtopx(++on, &on))) {
			o.gtd = NOT_A_TIME;
			o.lp = p;
		}
		if (*on != '\t' && (on = strchr(on, '\t')) == NULL) {
			break;
		}
		/* oh and a target price */
		o.tp = strtopx(++on, &on);
		if (*on != '\t' && (on = strchr(on, '\t')) == NULL) {
			break;
		}
		/* and finally a stop/loss */
		o.sl = strtopx(++on, &on);
		if (*on != '\t' && (on = strchr(on, '\t')) == NULL) {
			break;
		}
	}
	/* tune to exe delay */
	o.t += exe_age;
	o.gtd = o.gtd ?: rtry < NOT_A_TIME ? o.t + rtry : rtry;
	return o;
}

static quo_t
yield_quo(FILE *qfp)
{
	static char *line;
	static size_t llen;
	char *on;
	quo_t q;
	hx_t h;

retry:
	if (UNLIKELY(getline(&line, &llen, qfp) <= 0)) {
		free(line);
		line = NULL;
		llen = 0UL;
		return (quo_t){NOT_A_TIME};
	}
	/* otherwise snarf the quote line */
	if (UNLIKELY((q.t = strtotv(line, &on)) == NOT_A_TIME || on == line)) {
		goto retry;
	}
	/* instrument next */
	if (UNLIKELY(!(h = strtohx(on, &on)) || *on != '\t')) {
		goto retry;
	} else if (UNLIKELY(h != conx && conx)) {
		goto retry;
	}
	with (const char *str = ++on) {
		q.b = strtopx(str, &on);
		q.b = on > str ? q.b : NANPX;
	}
	with (const char *str = ++on) {
		q.a = strtopx(str, &on);
		q.a = on > str ? q.a : NANPX;
	}
	return q;
}

static int
offline(FILE *qfp)
{
	acc_t acc = {
		.base = 0.dd, .term = 0.dd, .comb = 0.dd, .comt = 0.dd,
	};
	ord_t oq[256U];
	size_t ioq = 0U, noq = 0U;
	quo_t q = {NOT_A_TIME};

	/* we can't do nothing before the first quote, so read that one
	 * as a reference and fast forward orders beyond that point */
	for (quo_t newq; (newq = yield_quo(qfp)).t < NOT_A_TIME; q = newq) {
	ord:
		if (UNLIKELY(stdin == NULL)) {
			/* order file is eof'd, skip fetching more */
			goto exe;
		}
		for (ord_t newo;
		     noq < countof(oq) / 2U &&
			     (newo = yield_ord(stdin)).t < NOT_A_TIME;
		     oq[noq++] = newo);
		if (UNLIKELY(noq < countof(oq) / 2U)) {
			/* out of orders we are */
			fclose(stdin);
			stdin = NULL;
		}

	exe:
		/* go through order queue and try exec'ing @q */
		for (size_t i = ioq; i < noq && oq[i].t < newq.t; i++) {
			exe_t x;

			switch (oq[i].r) {
			case RGM_UNK:
				/* don't go for dead orders */
				continue;
			case RGM_CANCEL:
			case RGM_EMERGCLOSE:
				/* adjust for current account base */
				oq[i].q = acc.base;
				/* cancel all pending limit orders */
				for (size_t j = ioq; j < noq; j++) {
					if (i == j) {
						continue;
					} else if (oq[j].t > oq[i].t) {
						continue;
					}
					/* otherwise shred him */
					oq[j].r = RGM_UNK;
				}
			default:
				break;
			}
			/* adapt cancellations to current accounts */
			oq[i].q = (oq[i].r & RGM_CANCEL) == RGM_CANCEL
				? acc.base
				: oq[i].q;
			/* try executing him */
			x = try_exec(oq[i], q);
			if (!x.q && oq[i].gtd > x.t) {
				continue;
			}
			/* massage execution */
			x.q -= !absq ||
				(oq[i].r & RGM_CANCEL) == RGM_CANCEL ||
				x.q > 0.dd && acc.base > 0.dd ||
				x.q < 0.dd && acc.base < 0.dd
				? 0.dd
				: acc.base;
			x.q = !maxq || acc.base != x.q ? x.q : 0.dd;
			/* otherwise send post-trade details */
			send_exe(x);
			acc = alloc(acc, x, comb, comt);
			send_acc(x.t, acc);

			/* check for brackets */
			if (oq[i].tp) {
				oq[noq++] = (ord_t){
					x.t,
					.r = (rgm_t)(oq[i].r ^ RGM_CANCEL),
					.gtd = NOT_A_TIME,
					.q = -x.q,
					.lp = oq[i].tp,
					.sl = oq[i].sl,
				};
			}
			/* instead of dequeuing we're just setting
			 * an order's regime */
			oq[i].r = RGM_UNK;
		}
		/* fast forward dead orders */
		for (; ioq < noq && !oq[ioq].r; ioq++);
		/* gc'ing again */
		if (UNLIKELY(ioq >= countof(oq) / 2U)) {
			memmove(oq, oq + ioq, (noq - ioq) * sizeof(*oq));
			noq -= ioq;
			ioq = 0U;
		}
		if (UNLIKELY(stdin == NULL)) {
			/* order file is eof'd, skip fetching more */
			;
		} else if (UNLIKELY(!noq || oq[noq - 1U].t < newq.t)) {
			/* fill up the queue some more and do more exec'ing */
			goto ord;
		}
	}

	/* finalise with the last known quote */
	if (acc.base) {
		ord_t o = {q.t, RGM_CANCEL, .q = acc.base};
		exe_t x = try_exec(o, q);
		send_exe(x);
		acc = alloc(acc, x, comb, comt);
		send_acc(x.t, acc);
	}
	return 0;
}


#include "sex.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;
	FILE *qfp;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	} else if (!argi->nargs) {
		errno = 0, serror("\
Error: QUOTES file is mandatory.");
		rc = 1;
		goto out;
	}

	if (argi->pair_arg) {
		cont = argi->pair_arg;
		conz = strlen(cont);
		conx = hash(cont, conz);
	}

	if (argi->exe_delay_arg) {
		exe_age = strtoul(argi->exe_delay_arg, NULL, 10);
	}

	if (argi->commission_arg) {
		char *on = argi->commission_arg;

		switch (*on) {
		default:
			comb = strtopx(on, &on);

			if (*on == '/') {
		case '/':
			comt = strtopx(++on, &on);
			if (*on == '/') {
				errno = 0, serror("\
Error: commission must be given as PXb[/PXt]");
				rc = 1;
				goto out;
			}
			}
			break;
		}
	}

	if (argi->quantity_arg) {
		qty = strtoqx(argi->quantity_arg, NULL);
	}

	absq = argi->absqty_flag;
	maxq = argi->maxqty_flag;

	if (argi->retry_arg) {
		if (argi->retry_arg != YUCK_OPTARG_NONE) {
			rtry = strtoul(argi->retry_arg, NULL, 10);
		} else {
			rtry = NOT_A_TIME;
		}
	}

	if (UNLIKELY((qfp = fopen(*argi->args, "r")) == NULL)) {
		serror("\
Error: cannot open QUOTES file `%s'", *argi->args);
		rc = 1;
		goto out;
	}

	/* offline mode */
	rc = offline(qfp);

	fclose(qfp);
out:
	yuck_free(argi);
	return rc;
}
