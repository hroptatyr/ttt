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
#include <sys/time.h>
#include <time.h>
#include <dfp754.h>
#include "dfp754_d32.h"
#include "nifty.h"

#define NSECS	(1000000000)
#define MSECS	(1000)
#define MAX_TICKS	4096U

typedef size_t cnt_t;
typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr
#define NOT_A_TIME	((tv_t)-1ULL)

typedef struct {
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	tv_t t[MAX_TICKS];
	quo_t q[MAX_TICKS];
	size_t head;
	size_t tail;
} qv_t;

/* configuration */
static tv_t wwdth = 60U * MSECS;


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

static inline __attribute__((pure, const)) quo_t
diff_quo(quo_t q1, quo_t q2)
{
/* calc Q1 - Q2 */
	return (quo_t){q1.b - q2.b, q1.a - q2.a};
}

static inline __attribute__((pure, const)) quo_t
sum_quo(quo_t q1, quo_t q2)
{
/* calc Q1 + Q2 */
	return (quo_t){q1.b + q2.b, q1.a + q2.a};
}

static inline __attribute__((pure, const)) quo_t
sq_quo(quo_t q)
{
/* calc Q * Q */
	return (quo_t){q.b * q.b, q.a * q.a};
}

static inline __attribute__((pure, const)) quo_t
sqrt_quo(quo_t q)
{
/* calc Q * Q */
	return (quo_t){
		sqrtd32(q.b > 0.df ? q.b : 0.df),
			sqrtd32(q.a > 0.df ? q.a : 0.df)};
}

static inline __attribute__((pure, const)) quo_t
iscal_quo(quo_t q, px_t x)
{
/* calc Q / X */
	return (quo_t){q.b / x, q.a / x};
}

static inline __attribute__((pure, const)) quo_t
quantize_quo(quo_t q, px_t m)
{
/* calc Q * Q */
	return (quo_t){quantized32(q.b, m), quantized32(q.a, m)};
}


/* actual contract data */
static tv_t metr;
static quo_t quo;
static qv_t win;
/* beef data */
static quo_t sma;
static quo_t smv;

#define MOVE(x)								\
	do {								\
		memcpy((x).t, (x).t + MAX_TICKS / 2U,			\
		       MAX_TICKS / 2U * sizeof(*(x).t));		\
		memcpy((x).q, (x).q + MAX_TICKS / 2U,			\
		       MAX_TICKS / 2U * sizeof(*(x).q));		\
		(x).tail = MAX_TICKS / 2U;				\
		(x).head = (x).head > MAX_TICKS / 2U			\
			? (x).head - MAX_TICKS / 2U			\
			: 0U;						\
	} while (0)

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	char *on;
	quo_t q;
	tv_t t;

	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		/* got metronome cock-up */
		return -1;
	} else if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	} else if (UNLIKELY(*on++ != '\t')) {
		return -1;
	}

	if ((q.b = strtopx(on, &on)), (unsigned char)*on++ >= ' ') {
		return -1;
	}
	if ((q.a = strtopx(on, &on)), (unsigned char)*on++ >= ' ') {
		return -1;
	}

	/* assign */
	metr = t;
	quo = q;
	return 0;
}

static int
calc_sma(void)
{
	static quo_t old = {__DEC32_MAX__, __DEC32_MAX__};
	/* moments */
	static size_t m0;
	static quo_t m1;
	static quo_t m2;

	if (UNLIKELY(win.tail >= MAX_TICKS)) {
		MOVE(win);
	}
	if (UNLIKELY(old.b == __DEC32_MAX__)) {
		old = quo;
		return -1;
	}
		
	/* assign metr and bid/ask*/
	with (const size_t iq = win.tail++) {
		quo_t q = diff_quo(quo, old);

		win.t[iq] = metr;
		win.q[iq] = q;

		m0++;
		m1 = sum_quo(m1, q);
		m2 = sum_quo(m2, sq_quo(q));
	}
	/* memorise quo */
	old = quo;

	/* we want ticks in the window to be at most WWDTH millis old,
	 * use head pointer to fast forward there */
	while (win.head < win.tail && metr - win.t[win.head] > wwdth) {
		m0--;
		m1 = diff_quo(m1, win.q[win.head]);
		m2 = diff_quo(m2, sq_quo(win.q[win.head]));
		win.head++;
	}

#if defined __INTEL_COMPILER
# pragma warning (push)
# pragma warning (disable:981)
#endif	/* __INTEL_COMPILER */

	with (px_t p0 = (px_t)m0) {
		/* m1/p0 */
		sma = quantize_quo(iscal_quo(m1, p0), quo.b);
		/* m2/p0 - m1*m1 */
		smv = quantize_quo(sqrt_quo(iscal_quo(diff_quo(m2, sq_quo(sma)), -0 -1.df)), quo.b);
	}
#if defined __INTEL_COMPILER
# pragma warning (pop)
#endif	/* __INTEL_COMPILER */
	return 0;
}

static void
prnt_sma(void)
{
	char buf[256U];
	size_t len = 0U;

	len += tvtostr(buf + len, sizeof(buf) - len, metr);
	buf[len++] = '\t';
	len += (memcpy(buf + len, "SMA", 3U), 3U);
	buf[len++] = '\t';
	/* contract */
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, sma.b);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, sma.a);
	buf[len++] = '\n';

	len += tvtostr(buf + len, sizeof(buf) - len, metr);
	buf[len++] = '\t';
	len += (memcpy(buf + len, "SMV", 3U), 3U);
	buf[len++] = '\t';
	/* contract */
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, smv.b);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, smv.a);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return;
}


static int
offline(void)
{
	/* offline mode */
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		if (push_beef(line, nrd) < 0) {
			/* parsing failed or packet isn't for us */
			;
		} else if (calc_sma() < 0) {
			;
		} else {
			prnt_sma();
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}


#include "seasons.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->window_arg) {
		if (!(wwdth = strtoul(argi->window_arg, NULL, 10))) {
			errno = 0, serror("\
Error: window size must be positive.");
			rc = 1;
			goto out;
		}
		wwdth *= MSECS;
	}

	/* offline mode */
	rc = offline();

out:
	yuck_free(argi);
	return rc;
}

/* seasons.c ends here */
