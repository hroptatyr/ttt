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
#include <sys/time.h>
#include <time.h>
#include "tv.h"
#include "nifty.h"

typedef size_t cnt_t;

static tvu_t intv = {60U * 60U * 24U * 7U, UNIT_SECS};
static tvu_t trnd = {60U * 60U, UNIT_SECS};


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


static int
offline(void)
{
/* offline mode */
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;
	cnt_t *m;
	size_t nm;

	/* estimate */
	if (trnd.u != UNIT_SECS) {
	nimpl:
		errno = 0, serror("not implemented");
		return -1;
	}

	switch (intv.u) {
	case UNIT_DAYS:
		intv.t *= 24U * 60U * 60U;
		intv.u = UNIT_SECS;
	case UNIT_SECS:
		break;
	default:
		goto nimpl;
	}

	nm = intv.t / trnd.t;
	if (UNLIKELY((m = calloc(nm, sizeof(*m))) == NULL)) {
		return -1;
	}
	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		tv_t t;

		if (UNLIKELY((t = strtotv(line, NULL)) == NATV)) {
			/* got metronome cock-up */
			continue;
		}
		/* round to seconds */
		t /= NSECS;
		/* move to monday */
		t -= 4U * 24U * 60U * 60U;
		/* align ... */
		t %= intv.t;
		/* ... and round */
		t /= trnd.t;
		/* count */
		m[t]++;
	}

	for (size_t i = 0U; i < nm; i++) {
		printf("%zu\t%zu\n", i, m[i]);
	}

	/* finalise our findings */
	free(line);
	/* and finalise the counter array */
	free(m);
	return 0;
}


#include "thours.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = EXIT_FAILURE;
		goto out;
	}

	if (argi->interval_arg) {
		intv = strtotvu(argi->interval_arg, NULL);
		if (!intv.t) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = EXIT_FAILURE;
			goto out;
		} else if (!intv.u) {
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	if (argi->round_arg) {
		trnd = strtotvu(argi->round_arg, NULL);
		if (!trnd.t) {
			errno = 0, serror("\
Error: cannot read rounding argument, must be positive.");
			rc = EXIT_FAILURE;
			goto out;
		} else if (!intv.u) {
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	/* offline mode */
	rc = offline();

out:
	yuck_free(argi);
	return rc;
}

/* thours.c ends here */
