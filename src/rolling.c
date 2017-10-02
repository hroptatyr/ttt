/*** rolling.c -- generate rolling windo
 *
 * Copyright (C) 2016-2017 Sebastian Freundt
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
#include "nifty.h"

#define MAX_COLS	(256U)

static tvu_t intv;


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


static tv_t *metrs;
static size_t *loffs;
static char *lines;
static size_t head, tail;
static size_t zoffs;

static inline size_t
llens(size_t i)
{
	return loffs[(i + 1U) & (zoffs - 1U)] - loffs[i];
}

static void
init(void)
{
	zoffs = 1024U;
	metrs = malloc(zoffs * sizeof(*metrs));
	loffs = malloc(zoffs * sizeof(*loffs));
	return;
}

static void
fini(void)
{
	if (metrs) {
		free(metrs);
	}
	if (loffs) {
		free(loffs);
	}
	if (lines) {
		free(lines);
	}
	return;
}

static void
push(tv_t m, const char *ln, size_t lz)
{
	static size_t linez;
	static size_t li;

	if (UNLIKELY(tail + 1U == head)) {
		/* resize */
		size_t nu_zoffs = zoffs * 2U;
		metrs = realloc(metrs, nu_zoffs * sizeof(*metrs));
		loffs = realloc(loffs, nu_zoffs * sizeof(*loffs));
		memmove(metrs + zoffs, metrs, tail * sizeof(*metrs));
		memmove(loffs + zoffs, loffs, tail * sizeof(*loffs));
		tail += zoffs;
		zoffs = nu_zoffs;
	}

	/* copy line to buffer */
	if (UNLIKELY(li + lz > linez)) {
		if (loffs[head] > linez / 2U) {
			/* just use the unused bobs at the beginning */
			const off_t lh = loffs[head];
			const size_t n = head > tail ? zoffs : tail;
			const size_t o = head < tail ? 0U : tail;

			memmove(lines, lines + lh, li -= lh);
			for (size_t i = head; i < n; i++) {
				loffs[i] -= lh;
			}
			for (size_t i = 0U; i < o; i++) {
				loffs[i] -= lh;
			}
		} else {
			/* enlarge the buffer */
			linez = (linez * 2U) ?: 8192U;
			lines = realloc(lines, linez * sizeof(*lines));
		}
	}
	memcpy(lines + li, ln, lz);
	/* keep track of metronomes and offsets */
	metrs[tail] = m;
	loffs[tail] = li;
	loffs[++tail] = li += lz;
	tail &= zoffs - 1U;
	return;
}

static tv_t
pop(tv_t keep)
{
	const size_t n = head > tail ? zoffs : tail;
	const size_t o = head < tail ? 0U : tail;
	char buf[32U];

	for (size_t i = head; i < n; i++) {
		size_t len = tvtostr(buf, sizeof(buf), metrs[i]);
		fwrite(buf, 1, len, stdout);
		fwrite(lines + loffs[i], 1, llens(i), stdout);
	}
	for (size_t i = 0U; i < o; i++) {
		size_t len = tvtostr(buf, sizeof(buf), metrs[i]);
		buf[len++] = '\t';
		fwrite(buf, 1, len, stdout);
		fwrite(lines + loffs[i], 1, llens(i), stdout);
	}
	/* advance head pointer */
	for (; head < n && metrs[head] <= keep; head++);
	for (size_t i = 0U; i < o && metrs[i] <= keep; head = ++i);
	return head != tail ? metrs[head] : keep + intv.t;
}


static int
from_stdin(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	tv_t next;

	init();
	/* first line is special */
	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		const char *const eol = line + nrd;
		char *on;
		tv_t newm = strtotv(line, &on);

		if (newm == NATV) {
			continue;
		}
		/* and push this guy */
		push(newm, on, eol - on);
		next = newm + intv.t;
		break;
	}
	/* first window is special */
	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		const char *const eol = line + nrd;
		char *on;
		tv_t newm = strtotv(line, &on);

		if (newm == NATV) {
			continue;
		} else if (newm >= next) {
			break;
		}
		/* and push this guy */
		push(newm, on, eol - on);
	}

	/* normal mode of operation now */
	while (nrd > 0) {
		const char *const eol = line + nrd;
		char *on;
		tv_t newm = strtotv(line, &on);

		if (newm == NATV) {
			goto nxln;
		}
		if (newm >= next) {
			/* and pop old lines */
			next = pop(newm - intv.t) + intv.t;
			puts("\f");
		}
		/* and push this guy */
		push(newm, on, eol - on);
	nxln:
		nrd = getline(&line, &llen, stdin);
	}

	/* dump last window */
	pop(NATV);
	fini();

	/* otherwise we're finished here */
	if (line) {
		free(line);
	}
	return 0;
}


#include "rolling.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		return 1;
	} else if (!argi->interval_arg) {
		errno = 0, serror("\
Error: --interval|-i is mandatory");
		rc = 1;
		goto out;
	}

	intv = strtotvu(argi->interval_arg, NULL);
	if (!intv.t) {
		errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
		rc = 1;
		goto out;
	}
	/* canonicalise */
	switch (intv.u) {
	case UNIT_DAYS:
		intv.t *= 86400LLU;
	case UNIT_SECS:
		intv.t *= NSECS;
	case UNIT_NSECS:
		/* good */
		break;
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	default:
		errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, yr.");
		rc = 1;
		goto out;
	}

	rc = from_stdin() < 0;

out:
	yuck_free(argi);
	return rc;
}

/* rolling.c ends here */
