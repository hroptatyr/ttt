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
#endif	/* HAVE_DFP754_H */
#include "nifty.h"
#include "dfp754_d32.h"
#include "dfp754_d64.h"
#include "hash.h"

#define NSECS	(1000000000)
#define MSECS	(1000)
#define UDP_MULTICAST_TTL	64
#define MCAST_ADDR	"ff05::134"
#define MCAST_PORT	7878

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

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

typedef struct {
	px_t p;
	qx_t q;
	qx_t totq;
} exe_t;

typedef struct {
	qx_t base;
	qx_t term;
	qx_t comm;
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
	rgm_t r;
	tv_t t;
	tv_t gtd;
	qx_t q;
	px_t lp;
	px_t tp;
	px_t sl;
} ord_t;

static tv_t exe_age = 60U;
static qx_t qty = 1.dd;
static px_t comm = 0.df;
static unsigned int absq;
static unsigned int maxq;

#define FRONT	(0U)
#define HIND	(1U)


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


static tv_t metr;
static tv_t omtr;
static const char *cont;
static size_t conz;

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

static inline __attribute__((const, pure)) tv_t
min_tv(tv_t t1, tv_t t2)
{
	return t1 < t2 ? t1 : t2;
}


static ssize_t
send_exe(tv_t t, exe_t x)
{
/* exe encodes delta to metronome and delay */
	static const char vexe[] = "EXE\t";
	static const char vrej[] = "REJ\t";
	char buf[256U];
	size_t len;

	len = snprintf(
		buf, sizeof(buf), "%lu.%03lu000000",
		t / MSECS, t % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, (x.q ? vexe : vrej), strlenof(vexe)),
		strlenof(vexe));
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.q);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, x.p);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static ssize_t
send_acc(tv_t t, acc_t a)
{
	static const char verb[] = "ACC\t";
	char buf[256U];
	size_t len;

	len = snprintf(
		buf, sizeof(buf), "%lu.%03lu000000",
		t / MSECS, t % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.base);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.term);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, a.comm);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static exe_t
try_exec(ord_t o, quo_t q)
{
/* this takes an order + quotes and executes it at market price */
	switch (o.r) {
	case RGM_LONG:
		if (q.a > o.lp) {
			/* no exec */
			break;
		}
	buy:
		return (exe_t){q.a, o.q, o.q};

	case RGM_SHORT:
		if (q.b < o.lp) {
			/* no can do exec */
			break;
		}
	sell:
		return (exe_t){q.b, -o.q, -o.q};

	case RGM_CANCEL:
	case RGM_EMERGCLOSE:
		if (o.q > 0.dd) {
			/* have to sell */
			goto sell;
		} else if (o.q < 0.dd) {
			/* have to buy */
			o.q = -o.q;
			goto buy;
		}
		/* otherwise do nothing */
	default:
		break;
	}
	return (exe_t){0.df};
}

static acc_t
alloc(acc_t a, exe_t x, px_t c/*ommission*/)
{
/* allocate execution X to account A. */
	/* calc accounts */
	a.base += x.q;
	a.term -= x.q * x.p;
	a.comm -= fabsd64(x.q) * c;

	/* quantize to make them look nicer */
	a.base = quantized64(a.base, 0.00dd);
	a.term = quantized64(a.term, 0.00dd);
	return a;
}


static int
offline(FILE *qfp)
{
	acc_t acc = {
		.base = 0.dd, .term = 0.dd, .comm = 0.dd,
	};
	ord_t oq[256U];
	size_t noq = 0U;
	char *line = NULL;
	size_t llen = 0UL;
	quo_t q, newq;

yield_ord:
	for (; getline(&line, &llen, stdin) > 0;) {
		ord_t o;
		char *on;

		if (UNLIKELY((omtr = strtotv(line, &on)) < metr)) {
			continue;
		}
		/* read the order */
		switch (*on) {
			qx_t adq;
			hx_t hx;

		case 'L'/*ONG*/:
			adq = !maxq ? qty : acc.base <= 0.dd ? qty : 0.dd;
			adq += absq && acc.base < 0.dd ? qty : 0.dd;
			o = (ord_t){RGM_LONG, .q = adq, .lp = __DEC32_MAX__};
			break;
		case 'S'/*HORT*/:
			adq = !maxq ? qty : acc.base >= 0.dd ? qty : 0.dd;
			adq += absq && acc.base > 0.dd ? qty : 0.dd;
			o = (ord_t){RGM_SHORT, .q = adq, .lp = __DEC32_MIN__};
			goto ord;
		case 'C'/*ANCEL*/:
		case 'E'/*MERG*/:
			o = (ord_t){RGM_CANCEL, .q = acc.base};
			break;
		default:
			continue;

		ord:
			if (UNLIKELY(!(hx = strtohx(on, &on)))) {
				/* no currency indicator */
				break;
			}
			/* otherwise snarf the limit price */
			if ((unsigned char)(*++on ^ '0') >= 10U) {
				;
			} else if (o.lp = strtopx(on, &on), *on != '\t') {
				break;
			}
			/* no room for gtd, imply eternity */
			o.gtd = NOT_A_TIME;
			/* oh and a target price */
			if ((unsigned char)(*++on ^ '0') >= 10U) {
				;
			} else if (o.tp = strtopx(on, &on), *on != '\t') {
				break;
			}
			/* and finally a stop/loss */
			if ((unsigned char)(*++on ^ '0') >= 10U) {
				;
			} else if (o.sl = strtopx(on, &on), *on != '\t') {
				break;
			}
		}
		/* throw away non-sense orders */
		if (LIKELY(o.q)) {
			omtr += exe_age;
			o.t = omtr;
			o.gtd = o.gtd ?: omtr;
			/* enqueue the order */
			oq[noq++] = o;
		}
		goto yield_quo;
	}
	/* no more orders coming, set omtr to NOT_A_TIME so we loop
	 * through quote reception eternally */
	omtr = NOT_A_TIME;

yield_quo:
	for (; getline(&line, &llen, qfp) > 0; q = newq) {
		char *on;

		metr = strtotv(line, &on);
		/* instrument next */
		on = strchr(on, '\t');
		newq.b = strtopx(++on, &on);
		newq.a = strtopx(++on, &on);

		if (UNLIKELY(metr > omtr)) {
			/* we need more orders! */
			goto yield_ord;
		}
		/* process limit order queue */
		for (size_t i = 0U; i < noq && oq[i].t <= metr; i++) {
			exe_t x;

			if (!oq[i].r) {
				/* don't go for dead orders */
				continue;
			}
			/* try executing him */
			x = try_exec(oq[i], q);
			if (!x.q && oq[i].gtd > metr) {
				continue;
			}
			/* otherwise send post-trade details */
			send_exe(min_tv(oq[i].gtd, metr), x);
			acc = alloc(acc, x, comm);
			send_acc(min_tv(oq[i].gtd, metr), acc);

			/* check for brackets */
			if (oq[i].tp) {
				oq[noq++] = (ord_t){
					(rgm_t)(oq[i].r ^ RGM_CANCEL),
					.t = metr,
					.q = -x.q,
					.lp = oq[i].tp,
					.sl = oq[i].sl,
				};
			}
			/* instead of dequeuing we're just setting
			 * an order's regime */
			oq[i].r = RGM_UNK;
		}
		/* gc'ing again */
		if (UNLIKELY(noq >= countof(oq) / 2U)) {
			size_t j = 0U;
			for (size_t i = 0U; i < noq; i++) {
				if (oq[i].r) {
					oq[j++] = oq[i];
				}
			}
			noq = j;
		}
	}

	/* finalise with the last known quote */
	with (exe_t x = try_exec((ord_t){RGM_CANCEL, .q = acc.base}, q)) {
		send_exe(metr, x);
		acc = alloc(acc, x, comm);
		send_acc(metr, acc);
	}

	free(line);
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

	if (argi->exe_delay_arg) {
		exe_age = strtoul(argi->exe_delay_arg, NULL, 10);
	}

	if (argi->commission_arg) {
		comm = strtopx(argi->commission_arg, NULL);
	}

	if (argi->quantity_arg) {
		qty = strtoqx(argi->quantity_arg, NULL);
	}

	absq = argi->absqty_flag;
	maxq = argi->maxqty_flag;

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
