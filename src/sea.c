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
#include "dfp754_d32.h"
#include "dfp754_d64.h"
#include "tv.h"
#include "nifty.h"

typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t m;
	px_t s;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;

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

/* modes, extend here */
typedef enum {
	SMODE_DFLT,
	SMODE_SPRD = SMODE_DFLT,
	SMODE_ADEV,
	SMODE_VELO,
	SMODE_TDLT,
	NSMODES
} smode_t;

static const char *const smodes[] = {
	[SMODE_SPRD] = "sprd",
	[SMODE_ADEV] = "adev",
	[SMODE_VELO] = "velo",
	[SMODE_TDLT] = "tdlt",
};

/* parameters */
static tv_t modulus = 86400U * NSECS;
static tv_t binwdth = 60U * NSECS;
static size_t nbins;

static smode_t smode;


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
stuf_trig(tv_t t)
{
	tv_t bin = t / binwdth, sub = t % binwdth;
	size_t bin1 = (bin + 0U) % nbins;
	size_t bin2 = (bin + 1U) % nbins;
	double fac1 = (1 - (double)sub / binwdth);
	double fac2 = (0 + (double)sub / binwdth);
	return (sbin_t){2U, (size_t[]){bin1, bin2}, (double[]){fac1, fac2}};
}

static sbin_t
stuf_triv(tv_t t)
{
	tv_t bin = (t / binwdth) % nbins;
	return (sbin_t){1U, (size_t[]){bin}, (double[]){1.}};
}


static tv_t metr;
static tik_t nxquo = {NATV};
static tik_t prquo;
/* contract we're on about */
static char cont[64];
static size_t conz;
/* bins */
static stat_t *bins;
/* just to have a prototype for quantization */
static px_t proto;

static int
push_init(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	const char *ip;
	size_t iz;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = ++on, '\t')) == NULL)) {
		return -1;
	}
	iz = on - ip;

	/* snarf quotes */
	if (!(bid = strtopx(++on, &on)) || *on != '\t' ||
	    !(ask = strtopx(++on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* we're init'ing, so everything changed */
	prquo = nxquo = (tik_t){metr, (ask + bid) / 2.df, (ask - bid) / 2.df};
	proto = bid;

	memcpy(cont, ip, conz = iz);
	return 1;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	tik_t this;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(++on, '\t')) == NULL)) {
		return -1;
	}

	/* snarf quotes */
	if (!(bid = strtopx(++on, &on)) || *on != '\t' ||
	    !(ask = strtopx(++on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* obtain mid+spr representation */
	this = (tik_t){metr, (ask + bid) / 2.df, (ask - bid) / 2.df};

	/* see what changed */
	if (this.m != nxquo.m) {
		/* yep */
		prquo = nxquo;
		nxquo = this;
		return 1;
	}
	return 0;
}

static int
send_tik(tik_t q)
{
	char buf[256U];
	size_t len;
	px_t bid = quantized32(q.m - q.s, proto);
	px_t ask = quantized32(q.m + q.s, proto);

	len = tvtostr(buf, sizeof(buf), q.t);
	buf[len++] = '\t';
	buf[len++] = '\t';
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, bid);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, ask);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return 0;
}


static inline void
bin_gen(sbin_t sch, double x)
{
	for (size_t i = 0U; i < sch.n; i++) {
		STAT_PUSH(bins[sch.bins[i]], sch.fcts[i] * x);
	}
	return;
}

static void
bin_sprd(sbin_t sch)
{
	bin_gen(sch, (double)fabsd32(nxquo.m - prquo.m) / (double)nxquo.s);
	return;
}

static void
bin_adev(sbin_t sch)
{
	bin_gen(sch, (double)fabsd32(nxquo.m - prquo.m));
	return;
}

static void
bin_velo(sbin_t sch)
{
	tv_t tdlt = nxquo.t - prquo.t;
	px_t pdlt = fabsd32(nxquo.m - prquo.m);
	const double xp = (double)pdlt * NSECS / tdlt;

	bin_gen(sch, xp);
	return;
}

static void
bin_tdlt(sbin_t sch)
{
	bin_gen(sch, (double)(nxquo.t - prquo.t) / NSECS);
	return;
}

static inline double
des_gen(sbin_t sch, double x)
{
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * x / bins[sch.bins[i]].m1;
	}
	s *= bins[nbins].m1;
	return s;
}

static px_t
des_sprd(sbin_t sch)
{
	if (LIKELY(nxquo.s > 0.df)) {
		const double xp = (double)(nxquo.m - prquo.m) / (double)nxquo.s;
		return (px_t)des_gen(sch, xp) * nxquo.s;
	}
	/* don't do him */
	return nxquo.m - prquo.m;
}

static px_t
des_adev(sbin_t sch)
{
	return (px_t)des_gen(sch, nxquo.m - prquo.m);
}

static px_t
des_velo(sbin_t sch)
{
	tv_t tdlt = nxquo.t - prquo.t;
	px_t pdlt = nxquo.m - prquo.m;

	return (px_t)(des_gen(sch, (double)pdlt * NSECS / tdlt) * tdlt / NSECS);
}

static tv_t
des_tdlt(sbin_t sch)
{
	return (tv_t)(des_gen(sch, (double)(nxquo.t - prquo.t) / NSECS) * NSECS);
}

static inline double
ens_gen(sbin_t sch, double x)
{
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * x * bins[sch.bins[i]].m1;
	}
	s /= bins[nbins].m1;
	return s;
}

static px_t
ens_sprd(sbin_t sch)
{
	if (LIKELY(nxquo.s > 0.df)) {
		const double xp = (double)(nxquo.m - prquo.m) / (double)nxquo.s;
		return (px_t)ens_gen(sch, xp) * nxquo.s;
	}
	/* return him unfiltered */
	return nxquo.m - prquo.m;
}

static px_t
ens_adev(sbin_t sch)
{
	return (px_t)ens_gen(sch, nxquo.m - prquo.m);
}

static px_t
ens_velo(sbin_t sch)
{
	tv_t tdlt = nxquo.t - prquo.t;
	px_t pdlt = nxquo.m - prquo.m;

	return (px_t)(ens_gen(sch, (double)pdlt * NSECS / tdlt) * tdlt / NSECS);
}

static tv_t
ens_tdlt(sbin_t sch)
{
	return (tv_t)(ens_gen(sch, (double)(nxquo.t - prquo.t) / NSECS) * NSECS);
}


static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	void(*bin)(sbin_t);

	switch (smode) {
	case SMODE_SPRD:
		bin = bin_sprd;
		break;
	case SMODE_ADEV:
		bin = bin_adev;
		break;
	case SMODE_VELO:
		bin = bin_velo;
		break;
	case SMODE_TDLT:
		bin = bin_tdlt;
		break;
	default:
		return -1;
	}
		
	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) < 0);

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		int c = push_beef(line, nrd);
		sbin_t s;

		if (c <= 0) {
			continue;
		}

		s = stuf_trig(metr);
		assert(s.n == 2U);

		bin(s);
	}
	/* finalise our findings */
	free(line);

	/* calc medians */
	for (size_t i = 0U; i < nbins; i++) {
		STAT_PUSH(bins[nbins], bins[i].m1);
	}

	/* print seasonality curve */
	printf("%s\t%lu\t%lu\n", smodes[smode], modulus, binwdth);
	for (size_t i = 0U; i < nbins; i++) {
		stat_t b = stat_eval(bins[i]);
		printf("%f\t%g\t%g\n",
			b.m0, b.m0 > 0 ? b.m1 : 1, b.m0 > 0 ? b.m2 : 1);
	}
	/* print medians */
	with (stat_t b = stat_eval(bins[nbins])) {
		printf("%f\t%g\t%g\n", b.m0, b.m1, b.m2);
	}
	return 0;
}

static int
desea(bool deseap)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	px_t(*des)(sbin_t);
	px_t base;

	switch (smode) {
	case SMODE_SPRD:
		des = deseap ? des_sprd : ens_sprd;
		break;
	case SMODE_ADEV:
		des = deseap ? des_adev : ens_adev;
		break;
	case SMODE_VELO:
		des = deseap ? des_velo : ens_velo;
		break;
	case SMODE_TDLT:
	default:
		return -1;
	}

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) < 0);
	send_tik(nxquo);
	base = nxquo.m;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		int c = push_beef(line, nrd);

		if (c < 0) {
			continue;
		} else if (c > 0) {
			sbin_t s = stuf_triv(metr);
			assert(s.n == 1U);

			base += des(s);
		}

		send_tik((tik_t){metr, base, nxquo.s});
	}
	/* finalise our findings */
	free(line);
	return 0;
}

static int
deseaT(bool deseap)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	tv_t(*des)(sbin_t);
	tv_t amtr;

	switch (smode) {
	case SMODE_TDLT:
		des = deseap ? des_tdlt : ens_tdlt;
		break;
	case SMODE_SPRD:
	case SMODE_ADEV:
	case SMODE_VELO:
	default:
		return -1;
	}

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) < 0);
	send_tik(nxquo);
	amtr = metr;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		int c = push_beef(line, nrd);

		if (c <= 0) {
			continue;
		}

		sbin_t s = stuf_triv(metr);
		assert(s.n == 1U);

		amtr += des(s);

		send_tik((tik_t){amtr, nxquo.m, nxquo.s});
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
	/* first line holds the smode and widths*/
	for (smode_t m = SMODE_DFLT; ++m < NSMODES;) {
		if (!memcmp(line, smodes[m], 4U)) {
			smode = m;
			break;
		}
	}
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
		bins[i].m0 = strtod(on, &on);
		on++;
		bins[i].m1 = strtod(on, &on);
		on++;
		bins[i].m2 = sqrt(strtod(on, &on));
	}
	if (UNLIKELY(getline(&line, &llen, sp) <= 0)) {
		/* great */
		rc = -1;
		goto out;
	}
	/* also read medians */
	with (char *on = line) {
		bins[nbins].m0 = strtod(on, &on);
		on++;
		bins[nbins].m1 = strtod(on, &on);
		on++;
		bins[nbins].m2 = sqrt(strtod(on, &on));
	}
	/* scale by medians */
	for (size_t i = 0U; i < nbins; i++) {
		bins[i].m0 /= bins[nbins].m0;
		bins[i].m1 /= bins[nbins].m1;
		bins[i].m2 /= bins[nbins].m2;
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
		modulus *= NSECS;
	}
	if (argi->width_arg) {
		if (!(binwdth = strtoul(argi->width_arg, NULL, 10))) {
			errno = 0, serror("\
Error: window width parameter must be positive.");
			rc = 1;
			goto out;
		}
		/* turn into msec resolutiona */
		binwdth *= NSECS;
	}

	if (argi->mode_arg) {
		if (0) {
			;
		} else if (!strcmp(argi->mode_arg, "sprd")) {
			smode = SMODE_SPRD;
		} else if (!strcmp(argi->mode_arg, "adev")) {
			smode = SMODE_ADEV;
		} else if (!strcmp(argi->mode_arg, "velo")) {
			smode = SMODE_VELO;
		} else if (!strcmp(argi->mode_arg, "tdlt")) {
			smode = SMODE_TDLT;
		}
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
	bins = calloc(nbins + 1U/*for medians*/, sizeof(*bins));

	if (!argi->nargs) {
		rc = offline() < 0;
	} else if ((rc = rdsea(NULL)) < 0) {
		/* pity */
		serror("\
Error: cannot process seasonality file `%s'", *argi->args);
		rc = 1;
	} else {
		switch (smode) {
		case SMODE_SPRD:
		case SMODE_ADEV:
		case SMODE_VELO:
			rc = desea(!argi->reverse_flag) < 0;
			break;
		case SMODE_TDLT:
			rc = deseaT(!argi->reverse_flag) < 0;
			break;
		default:
			rc = 1;
		}
	}

	free(bins);

out:
	yuck_free(argi);
	return rc;
}
