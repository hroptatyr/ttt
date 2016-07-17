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

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t p;
} tik_t;

typedef struct {
	px_t b;
	px_t a;
} quo_t;


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


static tv_t intv = 60U;

static tv_t metr;
static quo_t last = {__DEC32_MAX__, __DEC32_MIN__};

static void
crst(void)
{
	last = (quo_t){__DEC32_MAX__, __DEC32_MIN__};
	return;
}

static int
snap(void)
{
	char buf[256U];
	size_t bi;

	bi = snprintf(buf, sizeof(buf), "%lu.000000000", metr * intv);
	/* open */
	buf[bi++] = '\t';
	buf[bi++] = '\t';
	/* series name */
	buf[bi++] = '\t';
	/* close */
	buf[bi++] = '\t';
	if (LIKELY(last.b < __DEC32_MAX__)) {
		bi += pxtostr(buf + bi, sizeof(buf) - bi, last.b);
	}
	buf[bi++] = '\t';
	if (LIKELY(last.a > __DEC32_MIN__)) {
		bi += pxtostr(buf + bi, sizeof(buf) - bi, last.a);
	}

	buf[bi++] = '\n';
	fwrite(buf, 1, bi, stdout);
	return 0;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	tv_t oldm = metr;
	quo_t this;
	char *on;

	/* time value up first */
	with (long unsigned int s, x) {

		if (ln[20U] != '\t') {
			return -1;
		} else if (!(s = strtoul(ln, &on, 10))) {
			return -1;
		} else if (*on++ != '.') {
			return -1;
		} else if ((x = strtoul(on, &on, 10), *on++ != '\t')) {
			return -1;
		}
		/* assign with minute resolution */
		metr = s / intv;
	}

	/* do we need to draw another candle? */
	if (UNLIKELY(metr > oldm)) {
		/* yip */
		snap();
		/* and reset */
		crst();
	}

	/* now comes a descriptor */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;
	/* and an IP/port pair */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(this.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(this.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* keep some state */
	last = this;
	return 0;
}


#include "snap.yucc"

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

	if (argi->interval_arg) {
		if (!(intv = strtoul(argi->interval_arg, NULL, 10))) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		}
	}

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		crst();
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			push_beef(line, nrd);
		}

		/* finalise our findings */
		free(line);
	}

out:
	yuck_free(argi);
	return rc;
}
