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
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
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
#define NANPX		NAND32
#define isnanpx		isnand32

/* relevant tick dimensions */
typedef union {
	struct {
		px_t b;
		px_t a;
	};
	struct {
		px_t m;
		px_t s;
	};
} quo_t;

static px_t spr = 0.df;


static inline size_t
npxtostr(char *restrict buf, size_t bsz, px_t p)
{
	return !isnanpx(p) ? pxtostr(buf, bsz, p) : 0;
}


static quo_t q;
static size_t beg;
static size_t end;

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	char *on;

	/* metronome is up first */
	if (UNLIKELY(strtotv(ln, &on) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(++on, '\t')) == NULL)) {
		return -1;
	}
	beg = ++on - ln;

	/* snarf quotes */
	with (const char *str = on) {
		q.b = strtopx(str, &on);
		q.b = on > str ? q.b : NANPX;
	}
	with (const char *str = ++on) {
		q.a = strtopx(str, &on);
		q.a = on > str ? q.a : NANPX;
	}

	end = on - ln;
	return 0;
}

static int
bidask(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		if (UNLIKELY(push_beef(line, nrd) < 0)) {
			continue;
		}

		/* otherwise calc new bid/ask pair */
		with (px_t mid = (q.b + q.a) / 2.df) {
			char buf[64U];
			size_t len = 0U;
			const quo_t newq = {
				quantized32(mid - spr, q.b),
				quantized32(mid + spr, q.a)
			};

			fwrite(line, 1, beg, stdout);
			len += npxtostr(buf + len, sizeof(buf) - len, newq.b);
			buf[len++] = '\t';
			len += npxtostr(buf + len, sizeof(buf) - len, newq.a);
			fwrite(buf, 1, len, stdout);
			fwrite(line + end, 1, nrd - end, stdout);
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}

static int
bidask_widen(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		if (UNLIKELY(push_beef(line, nrd) < 0)) {
			continue;
		}

		/* otherwise calc new bid/ask pair */
		with (px_t mid = (q.b + q.a) / 2.df, sp = (q.a - q.b) / 2.df) {
			char buf[64U];
			size_t len = 0U;
			const quo_t newq = {
				quantized32(mid - (sp + spr), q.b),
				quantized32(mid + (sp + spr), q.a)
			};

			fwrite(line, 1, beg, stdout);
			len += npxtostr(buf + len, sizeof(buf) - len, newq.b);
			buf[len++] = '\t';
			len += npxtostr(buf + len, sizeof(buf) - len, newq.a);
			fwrite(buf, 1, len, stdout);
			fwrite(line + end, 1, nrd - end, stdout);
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}

static int
midspr(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		qx_t bsz, asz;

		if (UNLIKELY(push_beef(line, nrd) < 0)) {
			continue;
		}

		/* get quantities too */
		if (line[end] == '\t') {
			char *on = line + end;
			bsz = strtoqx(++on, &on);
			asz = strtoqx(++on, &on);
		}

		/* otherwise calc new bid/ask pair */
		with (px_t mp = (q.b + q.a) / 2.df, sp = q.a - q.b) {
			char buf[64U];
			size_t len = 0U;

			fwrite(line, 1, beg, stdout);
			len += npxtostr(buf + len, sizeof(buf) - len, mp);
			buf[len++] = '\t';
			len += npxtostr(buf + len, sizeof(buf) - len, sp);

			if (line[end] == '\t') {
				qx_t mq = (bsz + asz) / 2.dd;
				qx_t im = (asz - bsz);

				buf[len++] = '\t';
				len += qxtostr(buf + len, sizeof(buf) - len, mq);
				buf[len++] = '\t';
				len += qxtostr(buf + len, sizeof(buf) - len, im);
			}
			buf[len++] = '\n';
			fwrite(buf, 1, len, stdout);
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}

static int
desprd(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		qx_t mq, im;

		if (UNLIKELY(push_beef(line, nrd) < 0)) {
			continue;
		}

		/* get quantities too */
		if (line[end] == '\t') {
			char *on = line + end;
			mq = strtoqx(++on, &on);
			im = strtoqx(++on, &on);
		}

		/* otherwise calc new bid/ask pair */
		with (px_t sp = q.s / 2.df) {
			char buf[64U];
			size_t len = 0U;
			const quo_t newq = {
				quantized32(q.m - sp, q.s),
				quantized32(q.m + sp, q.s)
			};

			fwrite(line, 1, beg, stdout);
			len += npxtostr(buf + len, sizeof(buf) - len, newq.b);
			buf[len++] = '\t';
			len += npxtostr(buf + len, sizeof(buf) - len, newq.a);

			if (line[end] == '\t') {
				qx_t bq = mq - im / 2.dd;
				qx_t aq = mq + im / 2.dd;

				buf[len++] = '\t';
				len += qxtostr(buf + len, sizeof(buf) - len, bq);
				buf[len++] = '\t';
				len += qxtostr(buf + len, sizeof(buf) - len, aq);
			}
			buf[len++] = '\n';
			fwrite(buf, 1, len, stdout);
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}


#include "mid.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	bool widenp = false;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->spread_arg && argi->spread_arg != YUCK_OPTARG_NONE) {
		const char *s = argi->spread_arg;

		spr = strtopx(s, NULL);
		spr /= 2.df;

		switch (*s) {
		case '+':
		case '-':
			/* widening mode */
			widenp = true;
			break;
		default:
			break;
		}
	}

	if (0) {
		;
	} else if (argi->despread_flag) {
		rc = desprd() < 0;
	} else if (!argi->spread_arg || argi->spread_arg != YUCK_OPTARG_NONE) {
		if (!widenp) {
			rc = bidask() < 0;
		} else {
			rc = bidask_widen() < 0;
		}
	} else {
		rc = midspr() < 0;
	}

out:
	yuck_free(argi);
	return rc;
}
