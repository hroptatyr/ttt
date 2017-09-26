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


static tv_t metr;
static tv_t(*next)(tv_t);

static tv_t
_next_intv(tv_t newm)
{
	return newm;
}

static tv_t
_next_stmp(tv_t newm)
{
	static char *line;
	static size_t llen;

	if (getline(&line, &llen, sfil) > 0 &&
	    (newm = strtotv(line, NULL)) != NATV) {
		return newm - 1ULL;
	}
	/* otherwise it's the end of the road */
	free(line);
	line = NULL;
	llen = 0UL;
	return NATV;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	static char *lbuf;
	static size_t lbsz;
	static size_t llen;
	tv_t nmtr;
	char *on;

	auto inline void snap(void)
	{
		char sbuf[24U];
		size_t slen;

		if (UNLIKELY(!metr)) {
			return;
		} else if (UNLIKELY(lbuf == NULL)) {
			return;
		}

		slen = tvtostr(sbuf, sizeof(sbuf), (metr + 1ULL) * intv.t + offs.t);
		/* copy over (lbuf has space for the metronome) */
		memcpy(lbuf + (24UL - slen), sbuf, slen);
		/* and out */
		fwrite(lbuf + (24UL - slen), 1, llen + slen, stdout);
		return;
	}

	/* metronome is up first */
	if (UNLIKELY(ln == NULL)) {
		/* last snapshot of the day */
		for (; metr != NATV; metr = next(NATV)) {
			/* materialise snapshots */
			snap();
		}

		/* free our buffers */
		if (LIKELY(lbuf != NULL)) {
			free(lbuf);
			lbsz = 0ULL;
		}
		return 0;
	} else if (UNLIKELY((nmtr = strtotv(ln, &on)) == NATV)) {
		return -1;
	}
	/* align metronome to interval */
	nmtr--;
	nmtr -= offs.t;
	nmtr /= intv.t;

	/* do we need to draw another candle? */
	for (; UNLIKELY(nmtr > metr); metr = next(nmtr)) {
		/* materialise snapshot */
		snap();
	}

	/* and keep copy of line */
	llen = lz - (on - ln);
	if (UNLIKELY(llen + 24U > lbsz)) {
		size_t newz = _next_2pow(llen + 24U);
		lbuf = realloc(lbuf, newz);
		lbsz = newz;
	}
	/* leave space for metronome */
	memcpy(lbuf + 24U, on, llen);
	return 0;
}


#include "snap1.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];

	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
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

	{
		ssize_t nrd;
		char *lbuf = NULL;
		size_t lbsz = 0ULL;

		while ((nrd = getline(&lbuf, &lbsz, stdin)) > 0) {
			push_beef(lbuf, nrd);
		}
		/* produce the final candle */
		push_beef(NULL, 0ULL);

		/* finalise our findings */
		free(lbuf);
	}

	if (argi->stamps_arg) {
		fclose(sfil);
	}

out:
	yuck_free(argi);
	return rc;
}
