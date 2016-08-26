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
	MODE_DFLT,
	MODE_SPRD = MODE_DFLT,
	MODE_ADEV,
	MODE_VELO,
	MODE_TDLT,
	NMODES
} mode_t;

static const char *const modes[] = {
	[MODE_SPRD] = "sprd",
	[MODE_ADEV] = "adev",
	[MODE_VELO] = "velo",
	[MODE_TDLT] = "tdlt",
};

/* parameters */
static tv_t modulus = 86400U * MSECS;
static tv_t binwdth = 60U * MSECS;
static size_t nbins;

static mode_t mode;


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
static tik_t nxquo = {NOT_A_TIME};
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
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = on, '\t')) == NULL)) {
		return -1;
	}
	iz = on++ - ip;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
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
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
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
	const double xp = (double)pdlt * MSECS / tdlt;

	bin_gen(sch, xp);
	return;
}

static inline double
des_gen(sbin_t sch, double x)
{
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * x / bins[sch.bins[i]].m1;
	}
	return s;
}

static px_t
des_sprd(sbin_t sch)
{
	const double xp = (double)(nxquo.m - prquo.m) / (double)nxquo.s;
	return (px_t)des_gen(sch, xp) * nxquo.s;
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

	return (px_t)(des_gen(sch, (double)pdlt * MSECS / tdlt) * tdlt / MSECS);
}

static inline double
ens_gen(sbin_t sch, double x)
{
	double s = 0.;

	for (size_t i = 0U; i < sch.n; i++) {
		s += sch.fcts[i] * x * bins[sch.bins[i]].m1;
	}
	return s;
}

static px_t
ens_sprd(sbin_t sch)
{
	const double xp = (double)(nxquo.m - prquo.m) / (double)nxquo.s;
	return (px_t)ens_gen(sch, xp) * nxquo.s;
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

	return (px_t)(ens_gen(sch, (double)pdlt * MSECS / tdlt) * tdlt / MSECS);
}


static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	void(*bin)(sbin_t);

	switch (mode) {
	case MODE_SPRD:
		bin = bin_sprd;
		break;
	case MODE_ADEV:
		bin = bin_adev;
		break;
	case MODE_VELO:
		bin = bin_velo;
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

	/* print seasonality curve */
	printf("%s\t%lu\t%lu\n", modes[mode], modulus, binwdth);
	for (size_t i = 0U; i < nbins; i++) {
		stat_t b = stat_eval(bins[i]);
		printf("%f\t%g\t%g\n", b.m0, b.m1, b.m2);
	}
	/* print medians */
	with (stat_t b = stat_eval(bins[nbins - 1U])) {
		printf("%f\t%g\t%g\n", b.m0, b.m1, b.m2);
	}
	return 0;
}

static int
desea(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	px_t(*des)(sbin_t);
	px_t base;

	switch (mode) {
	case MODE_SPRD:
		des = des_sprd;
		break;
	case MODE_ADEV:
		des = des_adev;
		break;
	case MODE_VELO:
		des = des_velo;
		break;
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
			sbin_t s = stuf_trig(metr);
			assert(s.n == 2U);

			base += des(s);
		}

		send_tik((tik_t){metr, base, nxquo.s});
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
	px_t(*ens)(sbin_t);
	px_t base;

	switch (mode) {
	case MODE_SPRD:
		ens = ens_sprd;
		break;
	case MODE_ADEV:
		ens = ens_adev;
		break;
	case MODE_VELO:
		ens = ens_velo;
		break;
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
			sbin_t s = stuf_trig(metr);
			assert(s.n == 2U);

			base += ens(s);
		}

		send_tik((tik_t){metr, base, nxquo.s});
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
	for (mode_t m = MODE_DFLT; ++m < NMODES;) {
		if (!memcmp(line, modes[m], 4U)) {
			mode = m;
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
		bins[i].m2 = strtod(on, &on);
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
		bins[nbins].m2 = strtod(on, &on);
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

	if (UNLIKELY(argi->absdev_flag && argi->velocity_flag)) {
		errno = 0, serror("\
Error: only one of --absdev and --velocity can be specified.");
		rc = 1;
		goto out;
	} else if (argi->absdev_flag) {
		mode = MODE_ADEV;
	} else if (argi->velocity_flag) {
		mode = MODE_VELO;
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
		if (!argi->reverse_flag) {
			rc = desea() < 0;
		} else {
			rc = ensea() < 0;
		}
	}

	free(bins);

out:
	yuck_free(argi);
	return rc;
}
