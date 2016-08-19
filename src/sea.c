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
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
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
#define NOT_A_TIME	((tv_t)-1ULL)

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t p;
} tik_t;

/* what-changed enum */
typedef enum {
	WHAT_UNK = 0U,
	WHAT_BID = 1U,
	WHAT_ASK = 2U,
	WHAT_BOTH = WHAT_BID | WHAT_ASK,
} what_t;

#define BID	(0U)
#define ASK	(1U)

typedef struct {
	double m0;
	double m1;
	double m2;
} stat_t;

/* helper for binning scheme */
typedef struct {
	size_t n;
	size_t *bins;
	double *fcts;
} sbin_t;


static tv_t modulus = 86400U * MSECS;
static tv_t binwdth = 60U * MSECS;
static size_t nbins;


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


/* statisticians here */
static stat_t
stat_push(stat_t s, double x)
{
#define STAT_PUSH(s, x)	(s = stat_push(s, x))
	double dlt = x - s.m1;
	s.m1 += dlt / ++s.m0;
	s.m2 += dlt * (x - s.m1);
	return s;
}

static stat_t
stat_eval(stat_t s)
{
	/* only the variance needs fiddling with */
	s.m2 /= s.m0 - 1;
	return s;
}


/* 1-decompos */
static sbin_t
stuf_triag(tv_t t)
{
	tv_t bin = t / binwdth, sub = t % binwdth;
	size_t bin1 = (bin + 0U) % nbins;
	size_t bin2 = (bin + 1U) % nbins;
	double fac1 = (1 - (double)sub / binwdth);
	double fac2 = (0 + (double)sub / binwdth);
	return (sbin_t){2U, (size_t[]){bin1, bin2}, (double[]){fac1, fac2}};
}


static tv_t metr;
static tik_t nxquo[2U] = {{NOT_A_TIME}, {NOT_A_TIME}};
static tik_t prquo[2U];
/* contract we're on about */
static char cont[64];
static size_t conz;
/* bins */
static stat_t *tbins[2U];
static stat_t *pbins[2U];

static what_t
push_init(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	unsigned int rc = WHAT_UNK;
	const char *ip;
	size_t iz;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		goto out;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = on, '\t')) == NULL)) {
		goto out;
	}
	iz = on++ - ip;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		goto out;
	}
	/* we're init'ing, so everything changed */
	prquo[BID] = nxquo[BID] = (tik_t){metr, bid};
	rc ^= WHAT_BID;

	prquo[ASK] = nxquo[ASK] = (tik_t){metr, ask};
	rc ^= WHAT_ASK;

	memcpy(cont, ip, conz = iz);
out:
	return (what_t)rc;
}

static what_t
push_beef(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	unsigned int rc = WHAT_UNK;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		goto out;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		goto out;
	}
	on++;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		goto out;
	}
	/* see what changed */
	if (bid != nxquo[BID].p) {
		prquo[BID] = nxquo[BID];
		nxquo[BID] = (tik_t){metr, bid};
		rc ^= WHAT_BID;
	}
	if (ask != nxquo[ASK].p) {
		prquo[ASK] = nxquo[ASK];
		nxquo[ASK] = (tik_t){metr, ask};
		rc ^= WHAT_ASK;
	}
out:
	return (what_t)rc;
}

static void
bin(size_t side, sbin_t sch)
{
	tv_t tdlt = nxquo[side].t - prquo[side].t;
	px_t pdlt = nxquo[side].p - prquo[side].p;
	const double xt = (double)tdlt / MSECS;
	const double xp = (double)pdlt;

	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(tbins[side][sch.bins[i]], sch.fcts[i] * xt);
	}
	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(pbins[side][sch.bins[i]], sch.fcts[i] * xp);
	}
	return;
}

static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) == WHAT_UNK);

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		what_t c = push_beef(line, nrd);
		sbin_t s;

		if (!c) {
			continue;
		}

		s = stuf_triag(metr);
		assert(s.n == 2U);

		if (c & WHAT_BID) {
			bin(BID, s);
		}
		if (c & WHAT_ASK) {
			bin(ASK, s);
		}
	}
	/* finalise our findings */
	free(line);

	for (size_t i = 0U; i < nbins; i++) {
		stat_t b = stat_eval(pbins[BID][i]);
		stat_t a = stat_eval(pbins[ASK][i]);

		b.m2 = sqrt(b.m2);
		a.m2 = sqrt(a.m2);
		printf("%f\t%g\t%g\t%f\t%g\t%g\n",
		       b.m0, b.m1, b.m2,
		       a.m0, a.m1, a.m2);
	}
	return 0;
}


#include "sea.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];

	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->modulus_arg) {
		if (!(modulus = strtoul(argi->modulus_arg, NULL, 10))) {
			errno = 0, serror("\
Error: modulus parameter must be positive.");
			rc = 1;
			goto out;
		}
		/* turn into msec resolutiona */
		modulus *= MSECS;
	}
	if (argi->width_arg) {
		if (!(binwdth = strtoul(argi->width_arg, NULL, 10))) {
			errno = 0, serror("\
Error: window width parameter must be positive.");
			rc = 1;
			goto out;
		}
		/* turn into msec resolutiona */
		binwdth *= MSECS;
	}

	/* calc nbins */
	if (!(nbins = modulus / binwdth)) {
		errno = 0, serror("\
Error: modulus and window parameters result in 0 bins.");
		rc = 1;
		goto out;
	}
	/* get the tbins and pbins on the way */
	tbins[BID] = calloc(nbins, sizeof(**tbins));
	tbins[ASK] = calloc(nbins, sizeof(**tbins));
	pbins[BID] = calloc(nbins, sizeof(**pbins));
	pbins[ASK] = calloc(nbins, sizeof(**pbins));
	
	if (!argi->nargs) {
		rc = offline() < 0;
	}

	free(tbins[BID]);
	free(tbins[ASK]);
	free(pbins[BID]);
	free(pbins[ASK]);

out:
	yuck_free(argi);
	return rc;
}
