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
#include "nifty.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef size_t cnt_t;
typedef long unsigned int tv_t;
#define NOT_A_TIME	((tv_t)-1ULL)

typedef struct {
	tv_t t;
	enum {
		UNIT_NONE,
		UNIT_MSECS,
		UNIT_DAYS,
		UNIT_MONTHS,
		UNIT_YEARS,
	} u;
} tvu_t;

static tvu_t intv = {60U * 60U * 24U * 7U * MSECS, UNIT_MSECS};
static tvu_t trnd = {60U * 60U * MSECS, UNIT_MSECS};


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
strtotv(const char *ln, char **endptr)
{
	char *on;
	tv_t r;

	/* time value up first */
	with (long unsigned int s, x) {
		if (UNLIKELY(!(s = strtoul(ln, &on, 10)) || on == NULL)) {
			r = NOT_A_TIME;
			goto out;
		} else if (*on == '.') {
			char *moron;

			x = strtoul(++on, &moron, 10);
			if (UNLIKELY(moron - on > 9U)) {
				return NOT_A_TIME;
			} else if ((moron - on) % 3U) {
				/* huh? */
				return NOT_A_TIME;
			}
			switch (moron - on) {
			case 9U:
				x /= MSECS;
			case 6U:
				x /= MSECS;
			case 3U:
				break;
			case 0U:
			default:
				break;
			}
			on = moron;
		} else {
			x = 0U;
		}
		r = s * MSECS + x;
	}
	/* overread up to 3 tabs */
	for (size_t i = 0U; *on == '\t' && i < 3U; on++, i++);
out:
	if (LIKELY(endptr != NULL)) {
		*endptr = on;
	}
	return r;
}

static tvu_t
strtotvu(const char *str, char **endptr)
{
	char *on;
	tvu_t r;

	if (!(r.t = strtoul(str, &on, 10))) {
		return (tvu_t){};
	}
	switch (*on++) {
	secs:
	case '\0':
	case 'S':
	case 's':
		/* seconds, don't fiddle */
		r.t *= MSECS;
	msecs:
		r.u = UNIT_MSECS;
		break;

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
			goto msecs;
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
		errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
		return (tvu_t){};
	}
	if (endptr != NULL) {
		*endptr = on;
	}
	return r;
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
	if (intv.u != UNIT_MSECS || trnd.u != UNIT_MSECS) {
		errno = 0, serror("not implemented");
		return -1;
	}
	nm = intv.t / trnd.t;
	if (UNLIKELY((m = calloc(nm, sizeof(*m))) == NULL)) {
		return -1;
	}
	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		char *on;
		tv_t t;

		if (UNLIKELY((t = strtotv(line, &on)) == NOT_A_TIME)) {
			/* got metronome cock-up */
			continue;
		}
		/* move to monday */
		t -= 4U * 24U * 60U * 60U * MSECS;
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

	if (argi->interval_arg &&
	    !(intv = strtotvu(argi->interval_arg, NULL)).t) {
		errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
		rc = EXIT_FAILURE;
		goto out;
	}

	if (argi->round_arg &&
	    !(trnd = strtotvu(argi->round_arg, NULL)).t) {
		errno = 0, serror("\
Error: cannot read rounding argument, must be positive.");
		rc = EXIT_FAILURE;
		goto out;
	}

	/* offline mode */
	rc = offline();

out:
	yuck_free(argi);
	return rc;
}

/* thours.c ends here */
