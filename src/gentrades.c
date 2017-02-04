#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <tgmath.h>
#include <time.h>
#include <sys/time.h>
#include "nifty.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef long unsigned int tv_t;
#define NOT_A_TIME	((tv_t)-1ULL)

typedef double rate_t;

typedef enum {
	RGM_FLAT,
	RGM_LONG,
	RGM_SHORT,
	NRGM
} rgm_t;


static rate_t
strtorate(const char *str, char **endptr)
{
	char *on;
	double b, q;

	if (!(b = strtod(str, &on))) {
		return -1;
	} else if (!*on) {
		goto out;
	} else if (*on++ != '/') {
		return -1;
	}
	if (!(q = strtod(on, &on))) {
		/* quotient is 1 then */
		q = 1;
	}
	switch (*on++) {
	case '\0':
	case 'S':
	case 's':
		/* seconds, don't fiddle */
		break;

	case 'm':
	case 'M':
		switch (*on) {
		case '\0':
			/* they want minutes, oh oh */
			b /= 60;
			break;
		case 's':
		case 'S':
			/* milliseconds it is then */
			b /= MSECS;
			break;
		default:
			goto invalid;
		}
		break;

	case 'h':
	case 'H':
		b /= 60 * 60;
		break;
	case 'd':
	case 'D':
		b /= 24 * 60 * 60;
		break;

	default:
	invalid:
		fputs("\
Error: unknown suffix in interval argument, must be s, m, h, d.\n", stderr);
		return -1;
	}
	/* use scale really */
	b = q / b;
out:
	if (endptr != NULL) {
		*endptr = on;
	}
	return b;
}

static inline int
rdseed64(uint64_t *seed)
{
	unsigned char ok;

	asm volatile ("rdseed %0; setc %1"
		      : "=r" (*seed), "=qm" (ok));

	return (int) ok;
}

static inline int
rdrand64(uint64_t *rand)
{
	unsigned char ok;

	asm volatile ("rdrand %0; setc %1"
		      : "=r" (*rand), "=qm" (ok));

	return (int) ok;
}


static double
runif(void)
{
	union {
		uint64_t u64;
		double d;
	} tmp;

	rdrand64(&tmp.u64);
	/* leave only mantissa bits */
	tmp.u64 &= 0x000fffffffffffffULL;
	/* make it a -1.xxx number */
	tmp.u64 ^= 0xbff0000000000000ULL;
	/* just so we're in (0,1] */
	return tmp.d + 2;
}

static double
rexp(const double rate)
{
	return -rate * log(runif());
}


static rate_t rr[NRGM];
static tv_t st = NOT_A_TIME;
static double tr[NRGM];

static int
gen(void)
{
	static const char *rgms[] = {"CANCEL", "LONG", "SHORT"};
	double t = 0;
	rgm_t s = RGM_FLAT;

	/* since k == 1 in our case, we use the rexp for rgamma(1, .) */
	while (1) {
		/* calc next transition */
		t += rexp(rr[s]);
		s += (rgm_t)(1U + (runif() >= tr[s]));
		s = (rgm_t)(s % NRGM);
		printf("%.3f\t%s\n", t, rgms[s]);
	}
}


#include "gentrades.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = EXIT_FAILURE;
		goto out;
	}

	with (rate_t r) {
		if (!argi->rate_arg) {
			;
		} else if ((r = strtorate(argi->rate_arg, NULL)) <= 0) {
			fputs("\
Error: cannot read interval argument, must be positive.\n", stderr);
			rc = EXIT_FAILURE;
			goto out;
		} else {
			/* preset them all to the same rate */
			rr[RGM_FLAT] = rr[RGM_LONG] = rr[RGM_SHORT] = r;
		}

		if (argi->rf_arg &&
		    (r = strtorate(argi->rf_arg, NULL)) <= 0) {
			fputs("\
Error: cannot read rate of flats argument, must be positive.\n", stderr);
			rc = EXIT_FAILURE;
			goto out;
		} else if (argi->rf_arg) {
			rr[RGM_FLAT] = r;
		}

		if (argi->rl_arg &&
		    (r = strtorate(argi->rl_arg, NULL)) <= 0) {
			fputs("\
Error: cannot read rate of longs argument, must be positive.\n", stderr);
			rc = EXIT_FAILURE;
			goto out;
		} else if (argi->rl_arg) {
			rr[RGM_LONG] = r;
		}

		if (argi->rs_arg &&
		    (r = strtorate(argi->rs_arg, NULL)) <= 0) {
			fputs("\
Error: cannot read rate of shorts argument, must be positive.\n", stderr);
			rc = EXIT_FAILURE;
			goto out;
		} else if (argi->rs_arg) {
			rr[RGM_SHORT] = r;
		}
	}

	if (st == NOT_A_TIME) {
		st = time(NULL) * MSECS;
	}

	with (double fl, fs, lf, ls, sf, sl) {
		fl = argi->fl_arg ? strtod(argi->fl_arg, NULL) : 0;
		fs = argi->fs_arg ? strtod(argi->fs_arg, NULL) : 0;
		lf = argi->lf_arg ? strtod(argi->lf_arg, NULL) : 0;
		ls = argi->ls_arg ? strtod(argi->ls_arg, NULL) : 0;
		sf = argi->sf_arg ? strtod(argi->sf_arg, NULL) : 0;
		sl = argi->sl_arg ? strtod(argi->sl_arg, NULL) : 0;

		/* fill in transition matrix, store transition to next state */
		tr[RGM_FLAT] = fl / (fl + fs);
		tr[RGM_LONG] = ls / (lf + ls);
		tr[RGM_SHORT] = sf / (sf + sl);
	}

	/* finally do the simulation */
	rc = gen() < 0;

out:
	yuck_free(argi);
	return rc;
}
