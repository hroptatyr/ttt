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
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
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
#define NANPX		NAND32
#define sgnpx		sgnd32

/* relevant tick dimensions */
typedef struct {
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	int b;
	int a;
} sgn_t;

static unsigned int absp;


static __attribute__((const, pure)) int
sgnd32(_Decimal32 d)
{
	return (d >= 0.df) - (d <= 0.df);
}

static __attribute__((const, pure)) char
sgntoch(int s)
{
	return (char)(s + '=');
}


static int
push_beef(const char *ln, size_t lz)
{
	static sgn_t prev;
	static quo_t last = {NANPX, NANPX};
	const char *pre;
	char *on;
	sgn_t this;
	quo_t q;
	int s;

	/* metronome is up first */
	if (UNLIKELY(strtotv(ln, &on) == NATV)) {
		return -1;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(++on, '\t')) == NULL)) {
		return -1;
	}
	pre = ++on;

	/* snarf quotes */
	if (!(q.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* calc step */
	this.b = sgnpx(q.b - last.b);
	this.a = sgnpx(q.a - last.a);

	if (!prev.b && !prev.a) {
		/* just store */
		goto sto;
	}
	/* determine multiplier */
	s = absp ? prev.b ?: 1 : 1;
	/* print */
	fwrite(ln, 1, pre - ln, stdout);
	fputc(sgntoch(prev.b * s), stdout);
	fputc(sgntoch(this.b * s), stdout);
	fputc('\t', stdout);
	/* determine multiplier */
	s = absp ? prev.a ?: 1 : 1;
	/* print */
	fputc(sgntoch(prev.a * s), stdout);
	fputc(sgntoch(this.a * s), stdout);
	fwrite(on, 1, lz - (on - ln), stdout);
sto:
	/* and store state */
	prev = this;
	last = q;
	return 0;
}


#include "pat.yucc"

int
main(int argc, char *argv[])
{
/* grep -F 'EURUSD FAST Curncy' | cut -f1,5,6 */
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	absp = argi->abs_flag;

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
