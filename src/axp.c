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


static tv_t nexv = NOT_A_TIME;
static quo_t q;
static acc_t a;
static acc_t l;

static tv_t
next_quo(void)
{
	static quo_t newq = {0.df, 0.df};
	static char *line;
	static size_t llen;
	tv_t newm;
	char *on;

	/* assign previous next_quo as current quo */
	q = newq;

	if (UNLIKELY(qfp == NULL)) {
		return NOT_A_TIME;
	} else if (UNLIKELY(getline(&line, &llen, qfp) <= 0)) {
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
		line = NULL;
		llen = 0UL;
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
	a.base = strtoqx(++on, &on);
	a.term = strtoqx(++on, &on);
	a.comm = strtoqx(++on, &on);
	return newm;
}

static inline qx_t
calc_rpnl(void)
{
	static qx_t accpnl = 0.dd;
	qx_t this = (a.term * l.base - a.base * l.term)  / (l.base - a.base);
	qx_t pnl = this - accpnl;

	/* keep state */
	accpnl = this;
	return pnl;
}

static int
offline(void)
{
	static const char sstr[3U] = "FLS";
	tv_t alst;
	tv_t amtr;
	/* 3 time based moments */
	tv_t tagg[countof(sstr)] = {};
	/* higher valence metrics */
	size_t wins[countof(sstr) * countof(sstr)] = {};
	size_t cnts[countof(sstr) * countof(sstr)] = {};
	size_t olsd = 0U;
#define _M(a, i, j)	(a[i + countof(sstr) * j])
#define CNTS(i, j)	(_M(cnts, i, j))
#define WINS(i, j)	(_M(wins, i, j))

	(void)next_quo();
	alst = amtr = next_acc();

	do {
		const tv_t tdif = amtr - alst;
		const qx_t x = a.base;
		const size_t side = (x != 0.dd) + (x < 0.dd);

		tagg[olsd] += tdif;

		CNTS(olsd, side)++;
		/* check for winners */
		WINS(olsd, side) += calc_rpnl() > 0.dd;

		olsd = side;
	} while ((l = a, alst = amtr, amtr = next_acc()) < NOT_A_TIME);

	char buf[256U];
	size_t len;

	/* overview */
	fputs("\thits\tcount\ttime\n", stdout);
	for (size_t i = 0U; i < countof(sstr); i++) {
		size_t hits = 0U;
		size_t cagg = 0U;

		/* count wins and states */
		for (size_t j = 0U; j < countof(sstr); j++) {
			hits += WINS(i, j);
		}
		for (size_t j = 0U; j < countof(sstr); j++) {
			cagg += CNTS(j, i);
		}
		len = 0U;
		buf[len++] = sstr[i];
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%zu", hits);
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%zu", cagg);
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, tagg[i]);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* transiitions */
	len = 0U;
	buf[len++] = '\n';
	for (size_t i = 0U; i < countof(sstr); i++) {
		buf[len++] = '\t';
		buf[len++] = sstr[i];
		len += (memcpy(buf + len, "new", 3U), 3U);
	}
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	for (size_t i = 0U; i < countof(sstr); i++) {
		len = 0U;
		buf[len++] = sstr[i];
		len += (memcpy(buf + len, "old", 3U), 3U);
		for (size_t j = 0U; j < countof(sstr); j++) {
			const size_t v = CNTS(i, j);
			buf[len++] = '\t';
			len += snprintf(buf + len, sizeof(buf) - len, "%zu", v);
		}
		buf[len++] = '\n';
		fwrite(buf, 1, len, stdout);
	}

	/* hits */
	len = 0U;
	buf[len++] = '\n';
	for (size_t i = 0U; i < countof(sstr); i++) {
		buf[len++] = '\t';
		buf[len++] = sstr[i];
		len += (memcpy(buf + len, "new", 3U), 3U);
	}
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	for (size_t i = 0U; i < countof(sstr); i++) {
		len = 0U;
		buf[len++] = sstr[i];
		len += (memcpy(buf + len, "old", 3U), 3U);
		for (size_t j = 0U; j < countof(sstr); j++) {
			const size_t v = WINS(i, j);
			buf[len++] = '\t';
			len += snprintf(buf + len, sizeof(buf) - len, "%zu", v);
		}
		buf[len++] = '\n';
		fwrite(buf, 1, len, stdout);
	}
	return 0;
}


#include "axp.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (UNLIKELY((afp = stdin) == NULL)) {
		errno = 0, serror("\
Error: cannot open ACCOUNTS file");
		rc = 1;
		goto out;
	}		

	if (!argi->nargs) {
		;
	} else if (UNLIKELY((qfp = fopen(*argi->args, "r")) == NULL)) {
		serror("\
Error: cannot open QUOTES file `%s'", *argi->args);
		rc = 1;
		goto out;
	}

	/* offline mode */
	rc = offline();

	if (qfp) {
		fclose(qfp);
	}
	fclose(afp);
out:
	yuck_free(argi);
	return rc;
}
