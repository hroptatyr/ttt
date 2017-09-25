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
#include "tv.h"
#include "nifty.h"

/* command line params */
static tvu_t intv = {1U, UNIT_SECS};
static tvu_t offs = {0U, UNIT_SECS};
static FILE *sfil;


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

out:
	yuck_free(argi);
	return rc;
}

/* align.c ends here */
