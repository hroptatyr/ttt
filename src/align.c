/*** align.c -- align timestamped data
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
#include <errno.h>
#include <sys/resource.h>
#include "tv.h"
#include "nifty.h"

/* command line params */
static tvu_t intv = {1U, UNIT_SECS};
static tvu_t offs = {0U, UNIT_SECS};
static FILE *sfil;
static tv_t(*next)(tv_t);

static tv_t metr;

struct buf_s {
	char *b;
	size_t z;
	off_t i;
	size_t n;
};


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
_next_intv(tv_t newm)
{
	return ((((newm - 1ULL) - offs.t) / intv.t) + 1ULL) * intv.t;
}

static tv_t
_next_stmp(tv_t newm)
{
	static char *line;
	static size_t llen;

	if (getline(&line, &llen, sfil) > 0 &&
	    (newm = strtotv(line, NULL)) != NATV) {
		return newm + offs.t;
	}
	/* otherwise it's the end of the road */
	free(line);
	line = NULL;
	llen = 0UL;
	return NATV;
}

static size_t
get_maxfp(void)
{
	struct rlimit r;

	if (getrlimit(RLIMIT_NOFILE, &r) < 0) {
		return 0UL;
	} else if (r.rlim_cur <= 3U) {
		return 0UL;
	}
	return r.rlim_cur - 3UL;
}

static inline __attribute__((const, pure)) size_t
min_z(size_t z1, size_t z2)
{
	return z1 <= z2 ? z1 : z2;
}

static size_t
_next_2pow(size_t z)
{
	z--;
	z |= z >> 1U;
	z |= z >> 2U;
	z |= z >> 4U;
	z |= z >> 8U;
	z |= z >> 16U;
	z |= z >> 32U;
	z++;
	return z;
}

static tv_t
_fill(struct buf_s *restrict tgt, FILE *fp)
{
	tv_t t;

	do {
		ssize_t nrd = getline(&tgt->b, &tgt->z, fp);
		char *on;

		if (nrd <= 0) {
			return NATV;
		}
		/* massage line */
		nrd -= tgt->b[nrd - 1] == '\n';
		/* otherwise store line length */
		tgt->n = nrd;
		t = strtotv(tgt->b, &on);
		tgt->i = on - tgt->b;
	} while (t == NATV);
	return t;
}


static int
from_cmdln(char *const *fn, size_t nfn)
{
/* files in the parameter array */
	const size_t maxnfp = get_maxfp();
	const size_t n = min_z(nfn, maxnfp);
	FILE *f[n];
	tv_t t[min_z(nfn, maxnfp)];
	struct buf_s this[n];
	struct buf_s prev[n];
	size_t nf = 0U;
	size_t j;

	memset(this, 0, sizeof(this));
	memset(prev, 0, sizeof(prev));
	memset(t, 0, sizeof(t));
	for (j = 0U; j < nfn && nf < countof(f); j++) {
		f[nf] = fopen(fn[j], "r");
		if (UNLIKELY(f[nf] == NULL)) {
			serror("\
Error: cannot open file `%s'", fn[j]);
			continue;
		}
		nf++;
	}
	if (j < nfn) {
		errno = 0, serror("\
Warning: only %zu files are used due to resource limits", nfn);
	}

	/* read first line */
	for (size_t i = 0U; i < nf; i++) {
		if (f[i] == NULL) {
			;
		} else if (t[i] > metr) {
			;
		} else if (UNLIKELY((t[i] = _fill(this + i, f[i])) == NATV)) {
			fclose(f[i]);
			f[i] = NULL;
		}
	}
metr:
	metr = NATV;
	for (size_t i = 0U; i < nf; i++) {
		if (t[i] < metr) {
			metr = t[i];
		}
	}
	/* up the metronome */
	if (UNLIKELY(metr == NATV || (metr = next(metr)) == NATV)) {
		goto out;
	}
push:
	/* push lines < METR */
	for (size_t i = 0U; i < nf; i++) {
		if (t[i] > metr) {
			/* push later */
			continue;
		}
		prev[i].n = this[i].n - this[i].i;
		if (UNLIKELY(prev[i].z < prev[i].n)) {
			prev[i].z = _next_2pow(prev[i].n);
			prev[i].b = realloc(prev[i].b, prev[i].z);
		}
		/* and push */
		memcpy(prev[i].b, this[i].b + this[i].i, prev[i].n);
	}
	/* more lines now */
	for (size_t i = j = 0U; i < nf; i++) {
		if (f[i] == NULL) {
			;
		} else if (t[i] > metr) {
			;
		} else if (UNLIKELY((t[i] = _fill(this + i, f[i])) == NATV)) {
			fclose(f[i]);
			f[i] = NULL;
		} else {
			j++;
		}
	}
	/* check if lines need pushing */
	if (j) {
		goto push;
	}
	/* align */
	with (char buf[32U]) {
		fwrite(buf, 1, tvtostr(buf, sizeof(buf), metr), stdout);
	}
	for (size_t i = 0U; i < nf; i++) {
		if (prev[i].n) {
			fwrite(prev[i].b, 1, prev[i].n, stdout);
		} else {
			fputc('\t', stdout);
		}
	}
	fputc('\n', stdout);
	goto metr;

out:
	/* and close */
	for (size_t i = 0U; i < nf; i++) {
		if (this[i].b != NULL) {
			free(this[i].b);
		}
		if (prev[i].b != NULL) {
			free(prev[i].b);
		}
	}
	return (nf >= nfn) - 1;
}

static int
from_stdin(void)
{
/* stdin without distinction */
	struct buf_s this;
	struct buf_s prev;
	tv_t t = 0U;

	memset(&this, 0, sizeof(this));
	memset(&prev, 0, sizeof(prev));

	/* read first line */
	if (UNLIKELY((t = _fill(&this, stdin)) == NATV ||
		     (metr = next(t)) == NATV)) {
		goto out;
	}

	do {
		/* push lines < METR */
		while (t <= metr) {
			prev.n = this.n - this.i;
			if (UNLIKELY(prev.z < prev.n)) {
				prev.z = _next_2pow(prev.n);
				prev.b = realloc(prev.b, prev.z);
			}
			/* and push */
			memcpy(prev.b, this.b + this.i, prev.n);
			/* next line */
			t = _fill(&this, stdin);
		}
		/* align */
		with (char buf[32U]) {
			fwrite(buf, 1, tvtostr(buf, sizeof(buf), metr), stdout);
		}
		if (prev.n) {
			fwrite(prev.b, 1, prev.n, stdout);
		} else {
			fputc('\t', stdout);
		}
		fputc('\n', stdout);
	} while (t < NATV && (metr = next(t)) < NATV);

out:
	/* and close */
	if (this.b != NULL) {
		free(this.b);
	}
	if (prev.b != NULL) {
		free(prev.b);
	}
	return 0;
}


#include "align.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		return 1;
	}

	if (argi->interval_arg) {
		intv = strtotvu(argi->interval_arg, NULL);
		if (!intv.t) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		} else if (!intv.u) {
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	if (argi->offset_arg) {
		offs = strtotvu(argi->offset_arg, NULL);
		if (!intv.u) {
			errno = 0, serror("\
Error: unknown suffix in offset argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	if (argi->stamps_arg) {
		if (UNLIKELY((sfil = fopen(argi->stamps_arg, "r")) == NULL)) {
			serror("\
Error: cannot open stamps file");
			rc = 1;
			goto out;
		}
		/* reset intv to unit interval */
		intv = (tvu_t){1, UNIT_NSECS};
	}

	/* assimilate offs and intv */
	switch (intv.u) {
	case UNIT_DAYS:
		intv.t *= 86400LLU;
	case UNIT_SECS:
		intv.t *= NSECS;
	case UNIT_NSECS:
		/* good */
		break;
	default:
		errno = 0, serror("\
Error: snapshots timescale must be coercible to nanoseconds.");
		rc = 1;
		goto out;
	}
	switch (offs.u) {
	case UNIT_DAYS:
		offs.t *= 86400LLU;
	case UNIT_SECS:
		offs.t *= NSECS;
	case UNIT_NSECS:
		/* good */
		break;
	default:
		errno = 0, serror("\
Error: offset timescale must be coercible to nanoseconds.");
		rc = 1;
		goto out;
	}

	/* use a next routine du jour */
	next = !argi->stamps_arg ? _next_intv : _next_stmp;

	if (argi->nargs > 0U) {
		rc = from_cmdln(argi->args, argi->nargs) < 0;
	} else {
		rc = from_stdin() < 0;
	}

	if (argi->stamps_arg) {
		fclose(sfil);
	}

out:
	yuck_free(argi);
	return rc;
}

/* align.c ends here */
