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

static px_t comm = 0.df;

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


static ssize_t
send_eva(tv_t top, tv_t now, px_t p, quo_t q)
{
	static const char verb[] = "PNL\t";
	char buf[256U];
	size_t len;
	px_t pnl = p > 0.df ? q.b - p : q.a - p;

	len = snprintf(buf, sizeof(buf), "%lu.%03lu000000",
		       now / MSECS, now % MSECS);
	buf[len++] = '\t';
	len += (memcpy(buf + len, verb, strlenof(verb)), strlenof(verb));
	len += snprintf(buf + len, sizeof(buf) - len, "%lu", now - top);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, pnl);
	buf[len++] = '\n';
	return fwrite(buf, 1, len, stdout);
}


static int
offline(FILE *qfp)
{
	static tv_t ptv[4096U];
	static tv_t pnx[4096U];
	static px_t ppx[4096U];
	size_t npos = 0U;
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	quo_t q = {0.df, 0.df};
	tv_t omtr = 0ULL;

	while ((nrd = getline(&line, &llen, qfp)) > 0) {
		char *on;

		metr = strtotv(line, &on);
		for (size_t i = 0U; i < npos; i++) {
			if (UNLIKELY(pnx[i] <= metr)) {
				send_eva(ptv[i], pnx[i], ppx[i], q);
				pnx[i] += 10000U;
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
				pp = q.a;
				break;
			case 'S'/*HORT*/:
				pp = -q.b;
				break;
			case 'C'/*ANCEL*/:
			case 'E'/*MERG*/:
			default:
				continue;
			}
			/* now we're busy executing */
			ppx[npos] = pp;
			ptv[npos] = pnx[npos] = omtr;
			npos++;
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

	if (UNLIKELY((qfp = fopen(*argi->args, "r")) < 0)) {
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
