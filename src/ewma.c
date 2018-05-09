/*** ewma.c -- smoothen timeseries data
 *
 * Copyright (C) 2016-2018 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of ttt.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials pnrovided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include "tv.h"
#include "hash.h"
#include "nifty.h"

#define MAX_COLS	(256U)

static double dcay;
static tv_t metr;


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


static double cols[2U][MAX_COLS];
static const char *istr;
static size_t ilen;

static void
init(size_t UNUSED(ncol))
{
	memset(cols, 0, sizeof(cols));
	return;
}

static void
bang(const double d[], size_t n)
{
	const double l = pow(dcay, *d);

	/* naughth moment */
	**cols *= l, **cols += 1;
	/* first moment */
	for (size_t i = 1U; i < n; i++) {
		cols[0U][i] *= l;
		cols[0U][i] += d[i];
	}
	/* second moment */
	for (size_t i = 1U; i < n; i++) {
		const double x = d[i] - cols[0U][i] / **cols;
		cols[1U][i] *= l;
		cols[1U][i] += x * x;
	}
	return;
}

static void
unbang(void)
{
	return;
}

static size_t
snarf(double *restrict tgt, char *line, size_t llen)
{
	const char *const eol = line + llen;
	size_t i = 0U;
	tv_t newm;
	char *ln;

	/* snarf timestamp first of all*/
	if (UNLIKELY((newm = strtotv(line, &ln)) == NATV)) {
		return 0U;
	}
	/* record lapse and make newm the new metronome */
	tgt[i++] = (double)(newm - metr) / (double)NSECS, metr = newm;

	/* keep track of optional interim */
	istr = ln, ilen = 0U;
	for (char *on; ln < eol && *ln != '\n'; ln = on) {
		strtod(ln, &on);
		if (on > ln ||
		    (on = memchr(ln + 1U, *istr, eol - (ln + 1U))) == NULL) {
			break;
		}
	}
	/*  */
	ilen = ln - istr;

	/* go for the rest */
	for (char *on; ln < eol && *ln != '\n'; i++, ln = on) {
		tgt[i] = strtod(ln, &on);
		if (UNLIKELY(on <= ln)) {
			break;
		}
	}
	return i;
}

static void
prnt(size_t ncol)
{
	char buf[4096U];
	size_t len = 0U;

	len += tvtostr(buf + len, sizeof(buf) - len, metr);
	/* print interim */
	len += (memcpy(buf + len, istr, ilen), ilen);
	for (size_t i = 1U; i < ncol; i++) {
		const double m = cols[0U][i] / **cols;
		const double v = sqrt(cols[1U][i] / **cols);

		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%g", m);
		buf[len++] = '\t';
		len += snprintf(buf + len, sizeof(buf) - len, "%g", v);
	}
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	return;
}


static int
from_stdin(void)
{
	double ldbl[MAX_COLS];
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	size_t ncol;

	do {
		/* first line after reset is special */
		ncol = 0U;
		while ((nrd = getline(&line, &llen, stdin)) > 0 &&
		       (*line == '\f' || !(ncol = snarf(ldbl, line, nrd))));
		if (UNLIKELY(nrd <= 0)) {
			break;
		}
		init(ncol);

		/* bang all them lines */
		do {
			bang(ldbl, ncol);
			/* print EWMAs */
			prnt(ncol);
		} while ((nrd = getline(&line, &llen, stdin)) > 0 &&
			 *line != '\f' &&
			 snarf(ldbl, line, nrd) == ncol);
	} while (nrd > 0 && puts("\f") >= 0);

	/* otherwise we're finished here */
	if (line) {
		free(line);
	}
	unbang();
	return 0;
}


#include "ewma.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		return 1;
	}

	if (argi->halflife_arg) {
		tvu_t half = strtotvu(argi->halflife_arg, NULL);

		if (!half.t) {
			errno = 0, serror("\
Error: cannot read halflife argument, must be positive.");
			rc = 1;
			goto out;
		} else if (!half.u) {
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w.");
			rc = 1;
			goto out;
		}
		/* calculate decay */
		switch (half.u) {
		case UNIT_DAYS:
			half.t *= 86400LLU;
		case UNIT_SECS:
			half.t *= NSECS;
		case UNIT_NSECS:
			/* good */
			break;
		default:
			errno = 0, serror("\
Error: halflife timescale must be coercible to nanoseconds.");
			rc = 1;
			goto out;
		}
		dcay = 1. - log(2.) * (double)NSECS / (double)half.t;
	}

	rc = from_stdin() < 0;

out:
	yuck_free(argi);
	return rc;
}

/* ewma.c ends here */
