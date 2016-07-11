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
#include <math.h>
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

static tv_t intx[] = {
	5000U, 10000U, 15000U, 30000U,
	60000U, 120000U, 300000U, 600000U,
	900000U, 1800000U, 3600000U, 5400000U,
	7200000U, 10800000U, 1440000U, 18000000U,
	21600000U, 28800000U, 43200000U, 64800000U,
	86400000U, 129600000U, 172800000U, 216000000U,
	259200000U, 302400000U,	345600000U, 388800000U,
	432000000U, 475200000U, 518400000U, 604800000U,
};
static tv_t intv = 10000U;
static tv_t maxt;
static bool abs_tod_p;

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


static hx_t hxs;
static tv_t metr;
static const char *cont;
static size_t conz;
static unsigned int intv_scal_exp_p;

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

static inline __attribute__((const, pure)) qx_t
min_qx(qx_t q1, qx_t q2)
{
	return q1 < q2 ? q1 : q2;
}

static inline __attribute__((const, pure)) qx_t
max_qx(qx_t q1, qx_t q2)
{
	return q1 > q2 ? q1 : q2;
}


static size_t zeva;
static size_t *eva0;
static double *eva1;
static double *eva2;
static double *eva3;
static double *eva4;

static ssize_t
send_eva(tv_t top, tv_t now, px_t pnl)
{
	static const char verb[] = "PNL\t";
	char buf[256U];
	size_t len;

	len = snprintf(buf, sizeof(buf), "%lu.%03lu000000",
		       now / MSECS, now % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += snprintf(buf + len, sizeof(buf) - len, "%lu", now - top);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, pnl);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static ssize_t
send_abs(tv_t UNUSED(top), tv_t now, px_t pnl)
{
	static const char verb[] = "PNL\t";
	char buf[256U];
	size_t len;

	len = snprintf(buf, sizeof(buf), "%lu.%03lu000000",
		       now / MSECS, now % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += snprintf(buf + len, sizeof(buf) - len, "%lu", now % maxt);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, pnl);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static ssize_t
send_sum(tv_t lag, double pnl, double dev, double skew, double kurt)
{
	static const char verb[] = "PNL\t";
	char buf[256U];
	size_t len;

	len = snprintf(buf, sizeof(buf), "%lu.%03lu000000",
		       metr / MSECS, metr % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += snprintf(buf + len, sizeof(buf) - len, "%lu", lag);
	buf[len++] = '\t';
	len += snprintf(buf + len, sizeof(buf) - len, "%f", pnl);
	buf[len++] = '\t';
	len += snprintf(buf + len, sizeof(buf) - len, "%f", dev);
	buf[len++] = '\t';
	len += snprintf(buf + len, sizeof(buf) - len, "%f", skew);
	buf[len++] = '\t';
	len += snprintf(buf + len, sizeof(buf) - len, "%f", kurt);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}

static ssize_t
push_eva(tv_t top, tv_t now, px_t pnl)
{
	const size_t i = (now - top) / intv;

	if (UNLIKELY(i >= zeva)) {
		const size_t nuze = 2U * zeva;

		eva0 = realloc(eva0, nuze * sizeof(*eva0));
		eva1 = realloc(eva1, nuze * sizeof(*eva1));
		eva2 = realloc(eva2, nuze * sizeof(*eva2));
		eva3 = realloc(eva3, nuze * sizeof(*eva3));
		eva4 = realloc(eva4, nuze * sizeof(*eva4));
		/* clear memory */
		memset(eva0 + zeva, 0, zeva * sizeof(*eva0));
		memset(eva1 + zeva, 0, zeva * sizeof(*eva1));
		memset(eva2 + zeva, 0, zeva * sizeof(*eva2));
		memset(eva3 + zeva, 0, zeva * sizeof(*eva3));
		memset(eva4 + zeva, 0, zeva * sizeof(*eva4));

		zeva = nuze;
	}

	double nn = (double)eva0[i];
	double delta = (double)pnl - eva1[i];
	double delta1 = delta / (nn + 1.);
	double delta1S = delta1 * delta1;
	double delta2 = delta * delta1 * nn;

	eva4[i] += delta2 * delta1S * (nn * nn - nn + 1.) +
		6. * delta1S * eva2[i] - 4. * delta1 * eva3[i];
	eva3[i] += delta2 * delta1 * (nn - 1.) - 3. * delta1 * eva2[i];
	eva2[i] += delta2;
	eva1[i] += delta1;
	eva0[i]++;
	return 0;
}

static ssize_t
push_abs(tv_t UNUSED(top), tv_t now, px_t pnl)
{
	const size_t i = (now % maxt) / intv;

	if (UNLIKELY(i >= zeva)) {
		const size_t nuze = 2U * zeva;

		eva0 = realloc(eva0, nuze * sizeof(*eva0));
		eva1 = realloc(eva1, nuze * sizeof(*eva1));
		eva2 = realloc(eva2, nuze * sizeof(*eva2));
		eva3 = realloc(eva3, nuze * sizeof(*eva3));
		eva4 = realloc(eva4, nuze * sizeof(*eva4));
		/* clear memory */
		memset(eva0 + zeva, 0, zeva * sizeof(*eva0));
		memset(eva1 + zeva, 0, zeva * sizeof(*eva1));
		memset(eva2 + zeva, 0, zeva * sizeof(*eva2));
		memset(eva3 + zeva, 0, zeva * sizeof(*eva3));
		memset(eva4 + zeva, 0, zeva * sizeof(*eva4));

		zeva = nuze;
	}

	double nn = (double)eva0[i];
	double delta = (double)pnl - eva1[i];
	double delta1 = delta / (nn + 1.);
	double delta1S = delta1 * delta1;
	double delta2 = delta * delta1 * nn;

	eva4[i] += delta2 * delta1S * (nn * nn - nn + 1.) +
		6. * delta1S * eva2[i] - 4. * delta1 * eva3[i];
	eva3[i] += delta2 * delta1 * (nn - 1.) - 3. * delta1 * eva2[i];
	eva2[i] += delta2;
	eva1[i] += delta1;
	eva0[i]++;
	return 0;
}

static int
send_sums(void)
{
	for (size_t i = 0U; i < zeva; i++) {
		double mu, sigma, skew, kurt;

		if (UNLIKELY(!eva0[i])) {
			continue;
		}

		/* calc mean and  */
		mu = eva1[i];
		sigma = sqrt(eva2[i] / ((double)eva0[i] - 1.));
		skew = sqrt((double)eva0[i]) *
			eva3[i] / sqrt(eva2[i] * eva2[i] * eva2[i]);
		kurt = ((double)eva0[i] * eva4[i]) / (eva2[i] * eva2[i]) - 3.;
		send_sum((tv_t)(i * intv), mu, sigma, skew, kurt);
	}
	return 0;
}


static int
offline(FILE *qfp, bool sump)
{
	static tv_t _ptv[4096U];
	static tv_t _pnx[4096U];
	static px_t _ppx[4096U];
	static unsigned int _ini[4096U];
	tv_t *ptv = _ptv;
	tv_t *pnx = _pnx;
	px_t *ppx = _ppx;
	unsigned int *ini = _ini;
	size_t npos = 0U;
	size_t mpos = 0U;
	size_t zpos = countof(_ptv);
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	quo_t q = {0.df, 0.df};
	tv_t omtr = 0ULL;
	/* eva routine */
	ssize_t(*eva)(tv_t, tv_t, px_t);

	if (!abs_tod_p) {
		eva = !sump ? send_eva : push_eva;
	} else {
		eva = !sump ? send_abs : push_abs;
	}

	while ((nrd = getline(&line, &llen, qfp)) > 0) {
		char *on;

		metr = strtotv(line, &on);
		switch (intv_scal_exp_p) {
		case 0U:
			for (size_t i = mpos; i < npos && pnx[i] <= metr; i++) {
				if (UNLIKELY(isinfd32(ppx[i]))) {
					if (ppx[i] > 0) {
						ppx[i] = q.a;
					} else {
						ppx[i] = -q.b;
					}
				}
				with (px_t p = ppx[i],
				      pnl = p > 0.df ? q.b - p : -q.a - p) {
					eva(ptv[i], pnx[i], pnl);
				}
				pnx[i] += intv;

				if (UNLIKELY(maxt && pnx[i] > ptv[i] + maxt)) {
					/* phase him out */
					mpos = i + 1U;
				}
			}
			break;
		default:
			for (size_t i = mpos; i < npos; i++) {
				if (pnx[i] > metr) {
					continue;
				} else if (UNLIKELY(isinfd32(ppx[i]))) {
					if (ppx[i] > 0) {
						ppx[i] = q.a;
					} else {
						ppx[i] = -q.b;
					}
				}
				with (px_t p = ppx[i],
				      pnl = p > 0.df ? q.b - p : -q.a - p) {
					eva(ptv[i], pnx[i], pnl);
				}
				if (UNLIKELY(ini[i] >= countof(intx))) {
					/* phase him out */
					mpos = i + 1U;
				} else {
					pnx[i] = ptv[i] + intx[ini[i]++];
				}
			}
			break;
		}
		/* more house keeping */
		if (mpos >= zpos / 2U) {
			/* yay, we've got some spares */
			if (LIKELY(mpos < npos)) {
				/* move, move, move */
				const size_t nleft = npos - mpos;

				memmove(ptv, ptv + mpos, nleft * sizeof(*ptv));
				memmove(pnx, pnx + mpos, nleft * sizeof(*pnx));
				memmove(ppx, ppx + mpos, nleft * sizeof(*ppx));
				mpos = 0U;
				npos = nleft;
			} else {
				/* just reset him */
				mpos = npos = 0U;
			}
		}
		/* instrument next */
		on = strchr(on, '\t');
		q.b = strtopx(++on, &on);
		q.a = strtopx(++on, &on);

		if (LIKELY(omtr > metr)) {
			continue;
		}
		/* otherwise get next opportunity */
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			px_t pp;

			if (UNLIKELY((omtr = strtotv(line, &on)) < metr)) {
				continue;
			}
			/* read the order */
			switch (*on) {
			case 'L'/*ONG*/:
				pp = INFD32;
				break;
			case 'S'/*HORT*/:
				pp = MINFD32;
				break;
			case 'C'/*ANCEL*/:
			case 'E'/*MERG*/:
			default:
				continue;
			}
			/* now we're busy executing */
			ppx[npos] = pp;
			ptv[npos] = omtr - (!abs_tod_p ? 0U : (omtr % intv));
			pnx[npos] = ptv[npos] + (!abs_tod_p ? 0 : intv);
			if (UNLIKELY(++npos >= zpos)) {
				const size_t nuzp = 2U * zpos;
				tv_t *nuptv = malloc(nuzp * sizeof(*ptv));
				tv_t *nupnx = malloc(nuzp * sizeof(*pnx));
				px_t *nuppx = malloc(nuzp * sizeof(*ppx));

				memcpy(nuptv, ptv, zpos * sizeof(*ptv));
				memcpy(nupnx, pnx, zpos * sizeof(*pnx));
				memcpy(nuppx, ppx, zpos * sizeof(*ppx));

				if (ptv != _ptv) {
					free(ptv);
					free(pnx);
					free(ppx);
				}

				ptv = nuptv;
				pnx = nupnx;
				ppx = nuppx;
				zpos = nuzp;
			}
			break;
		}
	}
	/* finalise with the last known quote */
	while ((nrd = getline(&line, &llen, qfp)) > 0) {
		char *on;

		metr = strtotv(line, &on);
		/* instrument next */
		on = strchr(on, '\t');
		q.b = strtopx(++on, &on);
		q.a = -strtopx(++on, &on);
	}

	free(line);
	if (ptv != _ptv) {
		free(ptv);
		free(pnx);
		free(ppx);
	}
	return 0;
}


#include "imp.yucc"

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

	if (argi->exp_intv_scale_flag) {
		intv_scal_exp_p = 1U;
	} else if (argi->interval_arg) {
		if (UNLIKELY(!(intv = strtoul(argi->interval_arg, NULL, 10)))) {
			errno = 0, serror("\
Error: interval parameter must be positive.");
			rc = 1;
			goto out;
		}
		/* we operate in MSECS */
		intv *= MSECS;
	}

	if (argi->max_lag_arg) {
		maxt = strtoul(argi->max_lag_arg, NULL, 10);
		maxt *= MSECS;
	}

	if ((abs_tod_p = !!argi->abs_tod_flag)) {
		/* we need a maxt in this case */
		if (!maxt) {
			maxt = 86400 * MSECS;
		}
	}

	if (UNLIKELY((qfp = fopen(*argi->args, "r")) < 0)) {
		serror("\
Error: cannot open QUOTES file `%s'", *argi->args);
		rc = 1;
		goto out;
	}

	if (argi->summary_flag) {
		/* set up moment vectors */
		zeva = (maxt / intv ?: 4095U) + 1U;
		eva0 = calloc(zeva, sizeof(*eva0));
		eva1 = calloc(zeva, sizeof(*eva1));
		eva2 = calloc(zeva, sizeof(*eva2));
		eva3 = calloc(zeva, sizeof(*eva3));
		eva4 = calloc(zeva, sizeof(*eva4));
	}

	/* offline mode */
	rc = offline(qfp, !!argi->summary_flag);

	if (argi->summary_flag) {
		/* print summary */
		send_sums();
		/* unset moment vectors */
		free(eva0);
		free(eva1);
		free(eva2);
		free(eva3);
		free(eva4);
	}

	fclose(qfp);
out:
	yuck_free(argi);
	return rc;
}
