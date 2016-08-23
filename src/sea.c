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

typedef struct {
	px_t b;
	px_t a;
} quo_t;

/* what-changed enum */
typedef enum {
	WHAT_UNK = 0U,
	WHAT_BID = 1U,
	WHAT_MID = 1U,
	WHAT_ASK = 2U,
	WHAT_SPR = 2U,
	WHAT_BOTH = WHAT_BID ^ WHAT_ASK,
} what_t;

typedef enum {
	BID = 0U,
	MID = 0U,
	ASK = 1U,
	SPR = 1U,
	NSIDES = 2U,
} side_t;

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

static unsigned int adevp;
static unsigned int velop;


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
static stat_t *bins[2U];

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

static int
send_quo(tv_t t, quo_t q)
{
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), t);
	buf[len++] = '\t';
	buf[len++] = '\t';
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, q.b);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, q.a);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return 0;
}


static void
bin_tdlt(side_t side, sbin_t sch)
{
	tv_t tdlt = nxquo[side].t - prquo[side].t;
	const double xt = (double)tdlt / MSECS;

	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(bins[side][sch.bins[i]], sch.fcts[i] * xt);
	}
	return;
}

static void
bin_adev(side_t side, sbin_t sch)
{
	px_t pdlt = fabsd32(nxquo[side].p - prquo[side].p);
	const double xp = (double)pdlt;

	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(bins[side][sch.bins[i]], sch.fcts[i] * xp);
	}
	return;
}

static void
bin_velo(side_t side, sbin_t sch)
{
	tv_t tdlt = nxquo[side].t - prquo[side].t;
	px_t pdlt = fabsd32(nxquo[side].p - prquo[side].p);
	const double xp = (double)pdlt * MSECS / tdlt;

	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(bins[side][sch.bins[i]], sch.fcts[i] * xp);
	}
	return;
}

static px_t
desea_adev(side_t side, sbin_t sch)
{
	px_t pdlt = nxquo[side].p - prquo[side].p;
	const double xp = (double)pdlt;
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * xp / bins[side][sch.bins[i]].m1;
	}
	return quantized32((px_t)s, pdlt);
}

static px_t
desea_velo(side_t side, sbin_t sch)
{
	tv_t tdlt = nxquo[side].t - prquo[side].t;
	px_t pdlt = nxquo[side].p - prquo[side].p;
	const double xp = (double)pdlt * MSECS / tdlt;
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * xp / bins[side][sch.bins[i]].m1;
	}
	return quantized32((px_t)s, pdlt);
}

static px_t
ensea_adev(side_t side, sbin_t sch)
{
	px_t pdlt = nxquo[side].p - prquo[side].p;
	const double xp = (double)pdlt;
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * xp * bins[side][sch.bins[i]].m1;
	}
	return quantized32((px_t)s, pdlt);
}

static px_t
ensea_velo(side_t side, sbin_t sch)
{
	tv_t tdlt = nxquo[side].t - prquo[side].t;
	px_t pdlt = nxquo[side].p - prquo[side].p;
	const double xp = (double)pdlt * MSECS / tdlt;
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * xp * bins[side][sch.bins[i]].m1;
	}
	return quantized32((px_t)s, pdlt);
}


static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	void(*bin)(side_t, sbin_t) = bin_tdlt;

	if (0) {
		;
	} else if (adevp) {
		bin = bin_adev;
	} else if (velop) {
		bin = bin_velo;
	}

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

	static const char *modes[] = {
		"tdlt", "adev", "velo",
	};
	const char *mode = modes[(adevp << 0) ^ (velop << 1U)];
	printf("%s\t%lu\t%lu\t\n", mode, modulus, binwdth);

	for (size_t i = 0U; i < nbins; i++) {
		stat_t b = stat_eval(bins[BID][i]);
		stat_t a = stat_eval(bins[ASK][i]);
		printf("%f\t%g\t%f\t%g\n", b.m0, b.m1, a.m0, a.m1);
	}
	return 0;
}

static int
desea(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	quo_t base;
	px_t(*desea)(side_t, sbin_t);

	if (0) {
		;
	} else if (adevp) {
		desea = desea_adev;
	} else if (velop) {
		desea = desea_velo;
	} else {
		/* we can't do tdelta deseasoning */
		return -1;
	}

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) == WHAT_UNK);
	base = (quo_t){prquo[BID].p, prquo[ASK].p};
	send_quo(metr, base);

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		what_t c = push_beef(line, nrd);
		sbin_t s;

		if (!c) {
			continue;
		}

		s = stuf_triag(metr);
		assert(s.n == 2U);

		if (c & WHAT_BID) {
			base.b += desea(BID, s);
		}
		if (c & WHAT_ASK) {
			base.a += desea(ASK, s);
		}

		send_quo(metr, base);
	}
	/* finalise our findings */
	free(line);
	return 0;
}

static int
ensea(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	quo_t base;
	px_t(*ensea)(side_t, sbin_t);

	if (0) {
		;
	} else if (adevp) {
		ensea = ensea_adev;
	} else if (velop) {
		ensea = ensea_velo;
	} else {
		/* we can't season with tdeltas */
		return -1;
	}

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) == WHAT_UNK);
	base = (quo_t){prquo[BID].p, prquo[ASK].p};
	send_quo(metr, base);

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		what_t c = push_beef(line, nrd);
		sbin_t s;

		if (!c) {
			continue;
		}

		s = stuf_triag(metr);
		assert(s.n == 2U);

		if (c & WHAT_BID) {
			base.b += ensea(BID, s);
		}
		if (c & WHAT_ASK) {
			base.a += ensea(ASK, s);
		}

		send_quo(metr, base);
	}
	/* finalise our findings */
	free(line);
	return 0;
}

static int
rdsea(const char *fn)
{
	static FILE *sp;
	static char *line;
	static size_t llen;
	int rc = 0;

	if (fn == NULL) {
		/* go straight to reading  */
		goto rd;
	} else if (UNLIKELY((sp = fopen(fn, "r")) == NULL)) {
		return -1;
	} else if (UNLIKELY(getline(&line, &llen, sp) <= 4U)) {
		rc = -1;
		goto out;
	}
	/* first line holds the mode and widths*/
	adevp = !memcmp(line, "adev\t", 5U);
	velop = !memcmp(line, "velo\t", 5U);
	with (char *on = line + 4U) {
		modulus = strtoul(++on, &on, 10U);
		binwdth = strtoul(++on, &on, 10U);
		if (UNLIKELY((unsigned char)*on >= ' ')) {
			/* hm, thanks for that then */
			rc = -1;
			goto out;
		}
	}
	/* return so the caller can set up arrays and stuff */
	return 0;
rd:
	/* now read all them lines */
	for (size_t i = 0U; i < nbins; i++) {
		char *on;

		if (UNLIKELY(getline(&line, &llen, sp) <= 0)) {
			/* great */
			rc = -1;
			goto out;
		}
		/* should be at least 4 values */
		on = line;
		bins[BID][i].m0 = strtod(on, &on);
		on++;
		bins[BID][i].m1 = strtod(on, &on);
		on++;
		bins[ASK][i].m0 = strtod(on, &on);
		on++;
		bins[ASK][i].m1 = strtod(on, &on);
	}
out:
	free(line);
	fclose(sp);
	return rc;
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

	adevp = argi->absdev_flag;
	velop = argi->velocity_flag;

	if (UNLIKELY(adevp && velop)) {
		errno = 0, serror("\
Error: only one of --absdev and --velocity can be specified.");
		rc = 1;
		goto out;
	}

	if (argi->nargs) {
		/* read modulus and binwidth from file */
		if (rdsea(*argi->args) < 0) {
			serror("\
Error: cannot process seasonality file `%s'", *argi->args);
			rc = 1;
			goto out;
		}

	}

	/* calc nbins */
	if (!(nbins = modulus / binwdth)) {
		errno = 0, serror("\
Error: modulus and window parameters result in 0 bins.");
		rc = 1;
		goto out;
	}

	/* get the tbins and bins on the way */
	bins[BID] = calloc(nbins, sizeof(**bins));
	bins[ASK] = calloc(nbins, sizeof(**bins));

	if (!argi->nargs) {
		rc = offline() < 0;
	} else if ((rc = rdsea(NULL)) < 0) {
		/* pity */
		serror("\
Error: cannot process seasonality file `%s'", *argi->args);
		rc = 1;
	} else {
		if (!argi->reverse_flag) {
			rc = desea() < 0;
		} else {
			rc = ensea() < 0;
		}
	}

	free(bins[BID]);
	free(bins[ASK]);

out:
	yuck_free(argi);
	return rc;
}
