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
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d32.h"
#include "dfp754_d64.h"
#include "hash.h"
#include "tv.h"
#include "nifty.h"

#define MAX_PREDS	(4096U)

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


static void
send_fra(tv_t fmtr, quo_t quo, quo_t fra, const char *id, size_t iz)
{
	char buf[256U];
	size_t len;

	len = tvtostr(buf, sizeof(buf), fmtr);
	buf[len++] = '\t';
	len += (memcpy(buf + len, id, iz), iz);
	buf[len++] = '\t';
	/* instrument, omitted */
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, quo.b + fra.b);
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, quo.a + fra.a);
	buf[len++] = '\n';

	fwrite(buf, 1, len, stdout);
	return;
}


static FILE *qfp;
static FILE *ffp;
static quo_t quo;
static quo_t fra;
static char *cont;
static size_t conz;

static tv_t
next_quo(void)
{
	static char *line;
	static size_t llen;
	tv_t newm;
	char *on;

	if (UNLIKELY(qfp == NULL)) {
		return NATV;
	} else if (UNLIKELY(getline(&line, &llen, qfp) <= 0)) {
		free(line);
		fclose(qfp);
		qfp = NULL;
		return NATV;
	}

	newm = strtotv(line, &on);
	/* instrument next */
	on = strchr(++on, '\t');
	quo.b = strtopx(++on, &on);
	quo.a = strtopx(++on, &on);
	return newm;
}

static tv_t
next_fra(void)
{
	static char *line;
	static size_t llen;
	tv_t newm;
	char *on;

	if (UNLIKELY(ffp == NULL)) {
		return NATV;
	} else if (UNLIKELY(getline(&line, &llen, ffp) <= 0)) {
		free(line);
		fclose(ffp);
		ffp = NULL;
		return NATV;
	}

	/* snarf metronome */
	newm = strtotv(line, &on);
	/* anything that comes now is a FRA identifier */
	if (UNLIKELY((cont = ++on) == NULL)) {
		return NATV;
	} else if (UNLIKELY((on = strchr(cont, '\t')) == NULL)) {
		return NATV;
	}
	/* stash identifier */
	conz = on++ - cont;
	/* overread underlying */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return NATV;
	}
	/* snarf the base amount */
	fra = (quo_t){strtopx(++on, &on), strtopx(++on, &on)};
	return newm;
}

static int
offline(void)
{
	quo_t base;
	tv_t qmtr;
	tv_t fmtr;

	fmtr = next_fra();

	while (fmtr < NATV) {
		while ((qmtr = next_quo()) <= fmtr) {
			/* stash */
			base = quo;
		}
		if (UNLIKELY(qmtr == NATV)) {
			/* it's better if we have quotes after
			 * the last forward pricing */
			break;
		}
		/* print fra now */
		do {
			send_fra(fmtr, base, fra, cont, conz);
		} while ((fmtr = next_fra()) < qmtr);
	}
	return 0;
}


#include "fra.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	} else if (!argi->nargs) {
		errno = 0, serror("\
Error: QUOTES file is mandatory.");
		rc = 1;
		goto out;
	}

	if (UNLIKELY((qfp = fopen(*argi->args, "r")) == NULL)) {
		serror("\
Error: cannot open QUOTES file `%s'", *argi->args);
		rc = 1;
		goto out;
	}

	/* set FRA file pointer to stdin */
	ffp = stdin;

	/* offline mode */
	rc = offline();

	if (qfp) {
		fclose(qfp);
	}
	if (ffp) {
		fclose(ffp);
	}
out:
	yuck_free(argi);
	return rc;
}

/* fra.c ends here */
