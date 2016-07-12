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
typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr
#define NOT_A_TIME	((tv_t)-1ULL)


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


static int
offline(void)
{
	/* offline mode */
	static cnt_t m[8U * 32U];
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		char *on;
		tv_t t;
		tv_t d, h;

		if (UNLIKELY((t = strtotv(line, &on)) == NOT_A_TIME)) {
			/* got metronome cock-up */
			continue;
		}
		/* convert to hour */
		t /= 3600U * MSECS;
		/* align to monday */
		t -= 4U * 24U;
		/* obtain day */
		d = (t / 24U) % 7U + 1U;
		/* obtain hour */
		h = t % 24U;
		/* count */
		m[d * 32U + h]++;
	}

	for (size_t d = 1U; d <= 7U; d++) {
		for (size_t h = 0U; h < 24U; h++) {
			printf("%zu\t%02zu\t%zu\n", d, h, m[d * 32U + h]);
		}
	}

	/* finalise our findings */
	free(line);
	return 0;
}


#include "thours.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	/* offline mode */
	rc = offline();

out:
	yuck_free(argi);
	return rc;
}

/* thours.c ends here */
