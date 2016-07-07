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
	qx_t base;
	qx_t term;
	qx_t comm;
} acc_t;

typedef struct {
	/* timestamp of valuation */
	tv_t t;
	/* valuation in terms */
	qx_t nlv;
	/* commission route-through */
	qx_t comm;
} eva_t;

static tv_t intv = 10 * MSECS;
static tv_t nexv;


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
static tv_t amtr;
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


static ssize_t
send_eva(eva_t x)
{
/* exe encodes delta to metronome and delay */
	static const char verb[] = "EVA";
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), x.t);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.nlv);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.comm);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static eva_t
eva(tv_t t, acc_t a, quo_t q)
{
	eva_t r = {t, a.term, a.comm};

	if (UNLIKELY(!a.base)) {
		/* um, right */
		;
	} else if (a.base > 0.dd) {
		/* use bids */
		r.nlv += a.base * q.b;
	} else if (a.base < 0.dd) {
		/* use asks */
		r.nlv += a.base * q.a;
	}
	return r;
}


static int
offline(FILE *qfp)
{
	acc_t acc = {
		.base = 0.dd, .term = 0.dd, .comm = 0.dd,
	};
	char *line = NULL;
	size_t llen = 0UL;
	quo_t q = {};

yield_acc:
	for (; getline(&line, &llen, stdin) > 0;) {
		char *on;
		hx_t UNUSED(hx);

		if (UNLIKELY((amtr = strtotv(line, &on)) < metr || !on)) {
			continue;
		}
		/* make sure we're talking accounts */
		if (UNLIKELY(memcmp(on, "ACC\t", 4U))) {
			continue;
		}
		on += 4U;

		/* get currency indicator */
		hx = strtohx(on, &on);

		/* snarf the base amount */
		acc.base = strtoqx(++on, &on);
		acc.term = strtoqx(++on, &on);
		acc.comm = strtoqx(++on, &on);
		goto yield_quo;
	}

yield_quo:
	while (getline(&line, &llen, qfp) > 0) {
		char *on;
		tv_t newm;
		quo_t newq;

		newm = strtotv(line, &on);
		/* instrument next */
		on = strchr(on, '\t');
		newq.b = strtopx(++on, &on);
		newq.a = strtopx(++on, &on);

		if (UNLIKELY(acc.base)) {
			/* squeeze valuation in between */
			for (; nexv < metr; nexv += intv) {
				eva_t v = eva(nexv, acc, q);
				send_eva(v);
			}
		}

		/* shift quotes */
		q = newq;
		metr = newm;

		if (metr >= amtr) {
			goto yield_acc;
		}
	}

	free(line);
	return 0;
}


#include "eva.yucc"

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
