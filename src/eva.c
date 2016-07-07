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
static const char *cont;
static size_t conz;

static FILE *qfp;
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

static inline __attribute__((pure, const)) tv_t
min_tv(tv_t t1, tv_t t2)
{
	return t1 < t2 ? t1 : t2;
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


static tv_t nexv = NOT_A_TIME;
static quo_t q;
static acc_t a;

static tv_t
next_quo(void)
{
	static quo_t newq;
	static char *line;
	static size_t llen;
	tv_t newm;
	char *on;

	/* assign previous next_quo as current quo */
	q = newq;

	if (UNLIKELY(getline(&line, &llen, qfp) <= 0)) {
		free(line);
		return NOT_A_TIME;
	}

	newm = strtotv(line, &on);
	/* instrument next */
	on = strchr(on, '\t');
	newq.b = strtopx(++on, &on);
	newq.a = strtopx(++on, &on);
	return newm;
}

static tv_t
next_acc(void)
{
	static acc_t newa = {
		.base = 0.dd, .term = 0.dd, .comm = 0.dd,
	};
	static char *line;
	static size_t llen;
	static tv_t newm;
	char *on;
	hx_t UNUSED(hx);

	/* assign previous next_quo as current quo */
	a = newa;

	/* set next valuation timer */
	if (a.base) {
		nexv = (((newm - 1UL) / intv) + 1UL) * intv;
	} else {
		nexv = NOT_A_TIME;
	}

again:
	if (UNLIKELY(getline(&line, &llen, afp) <= 0)) {
		free(line);
		return NOT_A_TIME;
	}

	/* snarf metronome */
	newm = strtotv(line, &on);
	/* make sure we're talking accounts */
	if (UNLIKELY(memcmp(on, "ACC\t", 4U))) {
		goto again;
	}
	on += 4U;

	/* get currency indicator */
	hx = strtohx(on, &on);
	/* snarf the base amount */
	newa.base = strtoqx(++on, &on);
	newa.term = strtoqx(++on, &on);
	newa.comm = strtoqx(++on, &on);
	return newm;
}

static int
offline(void)
{
	tv_t qmtr;
	tv_t amtr;

	qmtr = next_quo();
	amtr = next_acc();

	do {
		while (qmtr < amtr && qmtr <= nexv) {
			qmtr = next_quo();
		}
		if (nexv < amtr && nexv < qmtr && a.base) {
			const tv_t metr = min_tv(qmtr, amtr);
			eva_t v = eva(nexv, a, q);

			for (; nexv < metr; v.t = nexv += intv) {
				send_eva(v);
			}
		}
		if (amtr <= qmtr) {
			amtr = next_acc();
		}
	} while (amtr < NOT_A_TIME || qmtr < NOT_A_TIME);
	return 0;
}


#include "eva.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	} else if (!argi->nargs) {
		errno = 0, serror("\
Error: QUOTES file is mandatory.");
		rc = 1;
		goto out;
	}

	if (argi->interval_arg) {
		if (!(intv = strtoul(argi->interval_arg, NULL, 10))) {
			errno = 0, serror("\
Error: interval argument cannot be naught");
			rc = 1;
			goto out;
		}
		/* turn into milliseconds */
		intv *= MSECS;
	}

	if (UNLIKELY((afp = stdin) == NULL)) {
		errno = 0, serror("\
Error: cannot open ACCOUNTS file");
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
	rc = offline();

	fclose(qfp);
	fclose(afp);
out:
	yuck_free(argi);
	return rc;
}
