#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "nifty.h"

typedef long unsigned int tv_t;

/* context lines */
static tv_t nbef, naft;
static long unsigned int nnfn;
static unsigned int verbp;

#define MAP_MEM		(MAP_PRIVATE | MAP_ANON)
#define PROT_MEM	(PROT_READ | PROT_WRITE)

#define NSECS	(1000000000)
#define MSECS	(1000)
#define NOT_A_TIME	((tv_t)-1ULL)


static __attribute__((format(printf, 1, 2))) void
serror(const char *fmt, ...)
{
#define uerror	errno = 0, serror
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
		if (UNLIKELY(!(s = strtoul(ln, &on, 10)))) {
			return NOT_A_TIME;
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
	if (LIKELY(endptr != NULL)) {
		*endptr = on;
	}
	return r;
}

static ssize_t
tvtostr(char *restrict buf, size_t bsz, tv_t t)
{
	return snprintf(buf, bsz, "%lu.%03lu000000", t / MSECS, t % MSECS);
}


static int
xevent(FILE *fp)
{
	char *line = NULL;
	size_t llen = 0UL;
	tv_t next, from, till, metr;
	ssize_t nrd;
	char ofn[32U];
	FILE *ofp;
	int rc = 0;

next:
	do {
		if ((nrd = getline(&line, &llen, stdin)) <= 0) {
			goto out;
		}
		/* otherwise get next from/till pair */
		till = from = next = strtotv(line, NULL);
		from -= nbef;
		till += naft;
	} while (till < metr);
	/* and open output file in anticipation */
	snprintf(ofn, sizeof(ofn), "xx%08lu", nnfn++);
	if ((ofp = fopen(ofn, "w")) == NULL) {
		rc = -1;
		goto out;
	}

	while ((nrd = getline(&line, &llen, fp)) > 0) {
		char *on;

		if (UNLIKELY((metr = strtotv(line, &on)) == NOT_A_TIME)) {
			continue;
		} else if (LIKELY(metr < from)) {
			continue;
		} else if (UNLIKELY(metr > next && verbp)) {
			char buf[64U];
			size_t len;

			len = tvtostr(buf, sizeof(buf), next);
			memcpy(buf + len, "\tSHOCK\t\t\t\t\t\t\n", 13U);
			len += 13U;
			fwrite(buf, 1, len, ofp);
			next = NOT_A_TIME;
		} else if (UNLIKELY(metr > till)) {
			fclose(ofp);
			puts(ofn);
			goto next;
		}
		/* copy line */
		fwrite(line, 1, nrd, ofp);
	}
out:
	if (line) {
		free(line);
	}
	return rc;
}


#include "xevent.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;
	FILE *fp;

	if (yuck_parse(argi, argc, argv) < 0) {
		return 1;
	} else if (!argi->nargs) {
		uerror("Error: no FILE given");
		rc = 1;
		goto out;
	}

	if (argi->before_arg) {
		nbef = strtoul(argi->before_arg, NULL, 0);
		nbef *= MSECS;
	}

	if (argi->after_arg) {
		naft = strtoul(argi->after_arg, NULL, 0);
		naft *= MSECS;
	}

	if (argi->number_arg) {
		nnfn = strtoul(argi->number_arg, NULL, 0);
	}

	verbp = argi->verbose_flag;

	if ((fp = fopen(*argi->args, "r")) == NULL) {
		serror("Error: cannot open file `%s'", *argi->args);
		rc = 1;
		goto out;
	} else if (xevent(fp) < 0) {
		serror("Error: cannot chunk up file `%s'", *argi->args);
		rc = 1;
	}
	/* close and out */
	fclose(fp);
out:
	yuck_free(argi);
	return rc;
}
