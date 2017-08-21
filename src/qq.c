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
	px_t b;
	px_t a;
} quo_t;

static px_t qunt = 0.df;


static int
push_qunt(const char *ln, size_t lz)
{
	static quo_t last;
	quo_t q;
	char *on;

	/* metronome is up first */
	if (UNLIKELY(strtotv(ln, &on) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(++on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(q.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* otherwise calc new bid/ask pair */
	if (fabsd32(q.b - last.b) <= qunt && fabsd32(q.a - last.a) <= qunt) {
		return 0;
	}
	/* otherwise print */
	fwrite(ln, 1, lz, stdout);
	/* and store state */
	last = q;
	return 0;
}

static int
push_latm(const char *ln, size_t lz)
{
	static px_t lasm;
	quo_t q;
	char *on;

	/* metronome is up first */
	if (UNLIKELY(strtotv(ln, &on) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(++on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(q.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}
	/* check against old midpoint */
	if (lasm >= q.b && lasm <= q.a) {
		return 0;
	}
	/* otherwise print */
	fwrite(ln, 1, lz, stdout);
	/* and store state */
	lasm = (q.a + q.b) / 2.df;
	return 0;
}


#include "qq.yucc"

int
main(int argc, char *argv[])
{
/* grep -F 'EURUSD FAST Curncy' | cut -f1,5,6 */
	static yuck_t argi[1U];
	int(*push_beef)(const char*, size_t) = push_latm;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->quantum_arg) {
		qunt = strtopx(argi->quantum_arg, NULL);
		push_beef = push_qunt;
	}

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			(void)push_beef(line, nrd);
		}

		/* finalise our findings */
		free(line);
	}

out:
	yuck_free(argi);
	return rc;
}
