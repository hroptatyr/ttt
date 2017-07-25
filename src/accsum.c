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
#define fabsqx		fabsd64
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

static unsigned int edgp;
static unsigned int grossp;
static unsigned int verbp;

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


static acc_t a;
static acc_t l;

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
		return NOT_A_TIME;
	}
	const char *const eol = line + nrd;

	/* snarf metronome */
	newm = strtotv(line, &on);
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

static int
offline(void)
{
	static const char sstr[3U] = "FLS";
	tv_t alst;
	tv_t amtr;
	/* stats per side */
	tv_t tagg[countof(sstr)] = {};
	qx_t rpnl[countof(sstr)] = {};
	qx_t rp[countof(sstr)] = {};
	qx_t best[countof(sstr)];
	qx_t wrst[countof(sstr)];
	/* higher valence metrics */
	size_t wins[countof(sstr) * countof(sstr)] = {};
	size_t cnts[countof(sstr) * countof(sstr)] = {};
	size_t olsd = 0U;
#define _M(a, i, j)	(a[i + countof(sstr) * j])
#define CNTS(i, j)	(_M(cnts, i, j))
#define WINS(i, j)	(_M(wins, i, j))

	alst = amtr = next_acc();

	memset(best, -1, sizeof(best));
	memset(wrst, -1, sizeof(wrst));

	do {
		const tv_t tdif = amtr - alst;
		const qx_t x = !edgp ? a.base : a.base - l.base;
		const size_t side = (x != 0.dd) + (x < 0.dd);

		tagg[olsd] += tdif;

		CNTS(olsd, side)++;
		/* check for winners */
		with (qx_t r = calc_rpnl()) {
			r += calc_rcom();
			r += calc_rspr();
			rpnl[olsd] += r;
			best[olsd] = best[olsd] >= r ? best[olsd] : r;
			wrst[olsd] = wrst[olsd] <= r ? wrst[olsd] : r;
			/* hits only metrics */
			rp[olsd] += r > 0.dd ? r : 0.dd;
			WINS(olsd, side) += r > 0.dd;
		}

		olsd = side;
	} while ((l = a, alst = amtr, amtr = next_acc()) < NOT_A_TIME);

	char buf[256U];
	size_t hits[countof(sstr)];
	size_t cagg[countof(sstr)];
	size_t len;

	/* overview */
	fputs("\thits\tcount\ttime\n", stdout);
	for (size_t i = 0U; i < countof(sstr); i++) {
		hits[i] = 0U;
		cagg[i] = 0U;

		/* count wins and states */
		for (size_t j = 0U; j < countof(sstr); j++) {
			hits[i] += WINS(i, j);
		}
		for (size_t j = 0U; j < countof(sstr); j++) {
			cagg[i] += CNTS(j, i);
		}
		len = 0U;
		buf[len++] = sstr[i];
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%zu", hits[i]);
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%zu", cagg[i]);
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, tagg[i]);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* single trades */
	fputs("\n\tavg\tbest\tworst\n", stdout);
	for (size_t i = 1U; i < countof(sstr); i++) {
		qx_t avg = quantized64(rpnl[i] / (qx_t)cagg[i], l.term);
		qx_t B = quantized64(best[i], l.term);
		qx_t W = quantized64(wrst[i], l.term);

		len = 0U;
		buf[len++] = sstr[i];
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, avg);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, B);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, W);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}
	len = (memcpy(buf, "L+S\t", 4U), 4U);
	with (qx_t avg = 0.dd, B, W) {
		size_t cnt = 0U;
		for (size_t i = 1U; i < countof(sstr); i++) {
			avg += rpnl[i];
			cnt += cagg[i];
		}
		avg = quantized64(avg / (qx_t)cnt, l.term);

		B = best[1U];
		W = wrst[1U];
		for (size_t i = 2U; i < countof(sstr); i++) {
			B = best[i] > B ? best[i] : B;
			W = wrst[i] < W ? wrst[i] : W;
		}
		B = quantized64(B, l.term);
		W = quantized64(W, l.term);

		len += qxtostr(buf + len, sizeof(buf) - len, avg);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, B);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, W);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* single trades skewed averages */
	fputs("\n\thit-r\thit-sk\tloss-sk\n", stdout);
	for (size_t i = 1U; i < countof(sstr); i++) {
		double r = (double)hits[i] / (double)cagg[i];
		qx_t P = quantized64(rp[i] / (qx_t)hits[i], l.term);
		qx_t L = quantized64(
			(rpnl[i] - rp[i]) / (qx_t)(cagg[i] - hits[i]), l.term);

		len = 0U;
		buf[len++] = sstr[i];
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%.4f", fabs(r));
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, P);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, L);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}
	len = (memcpy(buf, "L+S\t", 4U), 4U);
	with (qx_t s = 0.dd, P = 0.dd, L = 0.dd) {
		double r;
		size_t hnt = 0U, cnt = 0U;
		for (size_t i = 1U; i < countof(sstr); i++) {
			hnt += hits[i];
			cnt += cagg[i];
			P += rp[i];
			s += rpnl[i];
		}
		r = (double)hnt / cnt;
		L = quantized64((s - P) / (qx_t)(cnt - hnt), l.term);
		P = quantized64(P / (qx_t)hnt, l.term);

		len += snprintf(buf + len, sizeof(buf) - len, "%.4f", fabs(r));
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, P);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, L);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* rpnl */
	fputs("\n\trpnl\trp\trl\n", stdout);
	for (size_t i = 1U; i < countof(sstr); i++) {
		qx_t r = quantized64(rpnl[i], l.term);
		qx_t P = quantized64(rp[i], l.term);
		qx_t L = r - P;

		len = 0U;
		buf[len++] = sstr[i];
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, r);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, P);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, L);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}
	len = (memcpy(buf, "L+S\t", 4U), 4U);
	with (qx_t s = 0.dd, P = 0.dd, L = 0.dd) {
		for (size_t i = 1U; i < countof(sstr); i++) {
			s += rpnl[i];
		}
		s = quantized64(s, l.term);

		for (size_t i = 1U; i < countof(sstr); i++) {
			P += rp[i];
		}
		P = quantized64(P, l.term);
		L = s - P;

		len += qxtostr(buf + len, sizeof(buf) - len, s);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, P);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, L);
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* transiitions */
	len = 0U;
	buf[len++] = '\n';
	len += (memcpy(buf + len, "count", 5U), 5U);
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
	len += (memcpy(buf + len, "hits", 4U), 4U);
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

	if (LIKELY(!verbp)) {
		return 0;
	}

	/* print an explanation */
	static const char x[] = "\
\f\n\
Legend:\n\
F\tflat\n\
L\tlong\n\
S\tshort\n\
L+S\tlong and short together\n\
Xold\tX was the old position\n\
Xnew\tX is the new position\n\
\n\
hits\ta position that, when left, turned out to be profitable\n\
count\tthe number of positions entered, profitable or not\n\
time\ttotal time spent in the designated position\n\
\n\
avg\taverage profit/loss per position\n\
best\tbest position\n\
worst\tworst position\n\
\n\
hit-r\thit rate, ratio of hits versus count\n\
hit-sk\taverage profit on a hit\n\
loss-sk\taverage loss on a non-hit\n\
\n\
rpnl\trealised profit or loss\n\
rp\trealised profit\n\
rl\trealised loss\n\
";

	fwrite(x, sizeof(*x), countof(x), stdout);
	return 0;
}


#include "accsum.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	edgp = argi->edge_flag;
	grossp = argi->gross_flag;
	verbp = argi->verbose_flag;

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
