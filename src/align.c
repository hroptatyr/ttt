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


static int
from_cmdln(char *const *fn, size_t nfn)
{
/* files in the parameter array */
	const size_t maxnfp = get_maxfp();
	struct {
		/* current file, line and read count */
		FILE *p;
		char *line;
		size_t llen;
		size_t nrd;
		/* offset past stamp and stamp */
		off_t lix;
		tv_t t;
		/* previous line buffer */
		char *pbuf;
		size_t pbsz;
		size_t plen;
	} f[min_z(nfn, maxnfp)];
	size_t nf = 0U;
	size_t j;

	memset(f, 0, sizeof(f));
	for (j = 0U; j < nfn && nf < countof(f); j++) {
		f[nf].p = fopen(fn[j], "r");
		if (UNLIKELY(f[nf].p == NULL)) {
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
		if (f[i].p == NULL) {
			continue;
		} else if (f[i].t > metr) {
			continue;
		}
		do {
			ssize_t nrd = getline(&f[i].line, &f[i].llen, f[i].p);
			char *on;

			if (nrd <= 0) {
				fclose(f[i].p);
				f[i].p = NULL;
				f[i].t = NATV;
				break;
			}
			/* massage line */
			nrd -= f[i].line[nrd - 1] == '\n';
			f[i].line[nrd] = '\0';
			/* otherwise store line length */
			f[i].nrd = nrd;
			f[i].t = strtotv(f[i].line, &on);
			f[i].lix = on - f[i].line;
		} while (f[i].t == NATV);
	}
metr:
	metr = NATV;
	for (size_t i = 0U; i < nf; i++) {
		if (f[i].t < metr) {
			metr = f[i].t;
		}
	}
	/* up the metronome */
	if (UNLIKELY(metr == NATV || (metr = next(metr)) == NATV)) {
		goto out;
	}
push:
	/* push lines < METR */
	for (size_t i = 0U; i < nf; i++) {
		if (f[i].t <= metr) {
			f[i].plen = f[i].nrd - f[i].lix;
			if (UNLIKELY(f[i].pbsz <= f[i].plen)) {
				f[i].pbsz = _next_2pow(f[i].llen);
				f[i].pbuf = realloc(f[i].pbuf, f[i].pbsz);
			}
			/* and push */
			memcpy(f[i].pbuf, f[i].line + f[i].lix, f[i].plen + 1U);
		}
	}
	/* more lines now */
	for (size_t i = j = 0U; i < nf; i++) {
		if (f[i].p == NULL) {
			continue;
		} else if (f[i].t > metr) {
			continue;
		}
		do {
			ssize_t nrd = getline(&f[i].line, &f[i].llen, f[i].p);
			char *on;

			if (nrd <= 0) {
				fclose(f[i].p);
				f[i].p = NULL;
				f[i].t = NATV;
				break;
			}
			/* massage line */
			nrd -= f[i].line[nrd - 1] == '\n';
			f[i].line[nrd] = '\0';
			/* otherwise store line length */
			f[i].nrd = nrd;
			f[i].t = strtotv(f[i].line, &on);
			f[i].lix = on - f[i].line;
		} while (f[i].t == NATV);
		j++;
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
		if (f[i].plen) {
			fwrite(f[i].pbuf, 1, f[i].plen, stdout);
		} else {
			fputc('\t', stdout);
		}
	}
	fputc('\n', stdout);
	goto metr;

out:
	/* and close */
	for (size_t i = 0U; i < nf; i++) {
		if (f[i].line != NULL) {
			free(f[i].line);
		}
		if (f[i].pbuf != NULL) {
			free(f[i].pbuf);
		}
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
		from_cmdln(argi->args, argi->nargs);
	}

	if (argi->stamps_arg) {
		fclose(sfil);
	}

out:
	yuck_free(argi);
	return rc;
}

/* align.c ends here */
