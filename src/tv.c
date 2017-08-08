/*** tv.c -- timestamp routines
 *
 * Copyright (C) 2014-2017 Sebastian Freundt
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
 *    documentation and/or other materials provided with the distribution.
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
#include <time.h>
#include "tv.h"
#include "nifty.h"


tv_t
strtotv(const char *ln, char **endptr)
{
	char *on;
	tv_t r;

	/* time value up first */
	with (long unsigned int s, x) {
		if (UNLIKELY(!(s = strtoul(ln, &on, 10)) || on == NULL)) {
			r = NATV;
			goto out;
		} else if (*on == '.') {
			char *moron;

			x = strtoul(++on, &moron, 10);
			if (UNLIKELY(moron - on > 9U)) {
				return NATV;
			} else if ((moron - on) % 3U) {
				/* huh? */
				return NATV;
			}
			switch (moron - on) {
			default:
			case 0U:
				x *= MSECS;
			case 3U:
				x *= MSECS;
			case 6U:
				x *= MSECS;
			case 9U:
				/* all is good */
				break;
			}
			on = moron;
		} else {
			x = 0U;
		}
		r = s * NSECS + x;
	}
out:
	if (LIKELY(endptr != NULL)) {
		*endptr = on;
	}
	return r;
}

tvu_t
strtotvu(const char *str, char **endptr)
{
	char *on;
	tvu_t r;

	r.t = strtoul(str, &on, 10);
	switch (*on++) {
	secs:
	case '\0':
	case 'S':
	case 's':
		/* seconds, don't fiddle */
		r.u = UNIT_SECS;
		break;

	case 'n':
	case 'N':
		switch (*on) {
		case 's':
		case 'S':
		nsecs:
			r.u = UNIT_NSECS;
			break;
		default:
			goto invalid;
		}

	case 'm':
	case 'M':
		switch (*on) {
		case '\0':
			/* they want minutes, oh oh */
			r.t *= 60UL;
			goto secs;
		case 's':
		case 'S':
			/* milliseconds it is then */
			r.t *= USECS;
			r.u = UNIT_NSECS;
			goto nsecs;
		case 'o':
		case 'O':
			r.u = UNIT_MONTHS;
			break;
		default:
			goto invalid;
		}
		break;

	case 'y':
	case 'Y':
		r.u = UNIT_YEARS;
		break;

	case 'h':
	case 'H':
		r.t *= 60U * 60U;
		goto secs;
	case 'd':
	case 'D':
		r.u = UNIT_DAYS;
		break;

	default:
	invalid:
		r.u = UNIT_NONE;
		break;
	}
	if (endptr != NULL) {
		*endptr = on;
	}
	return r;
}

ssize_t
tvtostr(char *restrict buf, size_t bsz, tv_t t)
{
	long unsigned int ts = t / NSECS;
	long unsigned int tn = t % NSECS;
	size_t i;

	if (UNLIKELY(bsz < 19U)) {
		return 0U;
	}

	buf[0U] = '0';
	for (i = !ts; ts > 0U; ts /= 10U, i++) {
		buf[i] = (ts % 10U) ^ '0';
	}
	/* revert buffer */
	for (char *bp = buf + i - 1, *ap = buf, tmp; bp > ap;
	     tmp = *bp, *bp-- = *ap, *ap++ = tmp);
	/* nanoseconds, fixed size */
	buf[i] = '.';
	for (size_t j = 9U; j > 0U; tn /= 10U, j--) {
		buf[i + j] = (tn % 10U) ^ '0';
	}
	return i + 10U;
}

ssize_t
tvutostr(char *restrict buf, size_t bsz, tvu_t t)
{
	struct tm *tm;
	time_t u;

	switch (t.u) {
	default:
	case UNIT_NONE:
		memcpy(buf, "ALL", 3U);
		return 3U;
	case UNIT_SECS:
		return tvtostr(buf, bsz, t.t);
	case UNIT_DAYS:
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t.t / NSECS;
	u--;
	tm = gmtime(&u);

	switch (t.u) {
	case UNIT_DAYS:
		return strftime(buf, bsz, "%F", tm);
	case UNIT_MONTHS:
		return strftime(buf, bsz, "%Y-%m", tm);
	case UNIT_YEARS:
		return strftime(buf, bsz, "%Y", tm);
	default:
		break;
	}
	return 0;
}

/* tv.c ends here */
