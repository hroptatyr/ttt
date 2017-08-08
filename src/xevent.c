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


/* line buffer, we keep offsets and stamps per line and the big buffer */
static size_t nlines, zlines;
static tv_t *tvs;
static off_t *ofs;
static char *buf;
static size_t bsz, bix, bof;

static void
bangln(tv_t metr, const char *ln, size_t lz)
{
	if (UNLIKELY(nlines + 1U >= zlines)) {
		zlines = (zlines * 2U) ?: 256U;
		tvs = realloc(tvs, zlines * sizeof(*tvs));
		ofs = realloc(ofs, zlines * sizeof(*ofs));
	}
	/* store current offset */
	ofs[nlines] = bix;
	tvs[nlines] = metr;
	/* convenience */
	ofs[++nlines] = bix + lz;
	if (UNLIKELY(bix + lz > bsz)) {
		bsz = (bsz * 2U) ?: 4096U;
		buf = realloc(buf, bsz * sizeof(*buf));
	}
	memcpy(buf + bix, ln, lz);
	bix += lz;
	return;
}

static void
ffwdln(tv_t from)
{
	size_t i;

	for (i = 0U; i < nlines && tvs[i] < from; i++);
	if (i == nlines) {
		/* reset buffer and stuff */
		bix = bof = 0U;
		nlines = 0U;
	} else {
		/* TVS[I] >= FROM */
		if ((bof = ofs[i]) >= bsz / 2U) {
			memmove(buf, buf + bof, bix - bof);
			memmove(tvs, tvs + i, (nlines - i) * sizeof(*tvs));
			nlines -= i;
			for (size_t j = 0U; j < nlines; j++, i++) {
				ofs[j] = ofs[i] - bof;
			}
		}
		/* otherwise leave things as is */
	}
	return;
}

static int
prntln(long unsigned int n)
{
	char ofn[32U];
	int fd;

	if (UNLIKELY(bof >= bix)) {
		return 0;
	}
	snprintf(ofn, sizeof(ofn), "xx%08lu", n);
	if (UNLIKELY((fd = open(ofn, O_RDWR | O_CREAT | O_TRUNC, 0666)) < 0)) {
		return -1;
	}
	write(fd, buf + bof, bix - bof);
	close(fd);
	/* also print filename */
	puts(ofn);
	return 0;
}

static void
freeln(void)
{
	if (tvs) {
		free(tvs);
	}
	if (ofs) {
		free(ofs);
	}
	if (buf) {
		free(buf);
	}
	bix = bof = bsz = nlines = zlines = 0U;
	return;
}


static int
xevent(FILE *fp)
{
	char *line = NULL;
	size_t llen = 0UL;
	tv_t next, from, till, metr;
	ssize_t nrd;
	char shk[256U];
	size_t zshk;
	int rc = 0;

next:
	{
		char *on;

		if ((nrd = getline(&line, &llen, stdin)) <= 0) {
			goto out;
		}
		/* otherwise get next from/till pair */
		till = from = next = strtotv(line, &on);
		from -= nbef;
		till += naft;

		zshk = 0U;
		if (verbp) {
			/* copy shock string */
			zshk = nrd - (on - line);

			while (zshk > 0 && (unsigned char)on[zshk - 1] < ' ') {
				zshk--;
			}
			if (zshk) {
				memcpy(shk, on, zshk);
			}
		}
	}

	/* fast forward buffer */
	ffwdln(from);
	while ((nrd = getline(&line, &llen, fp)) > 0) {
		char *on;

		if (UNLIKELY((metr = strtotv(line, &on)) == NOT_A_TIME)) {
			continue;
		} else if (LIKELY(metr < from)) {
			continue;
		} else if (UNLIKELY(metr > next && verbp)) {
			char tmp[256U];
			size_t len;

			len = tvtostr(tmp, sizeof(tmp), next);
			if (zshk) {
				tmp[len++] = '\t';
				len += (memcpy(tmp + len, shk, zshk), zshk);
				tmp[len++] = '\n';
			} else {
				len += (memcpy(tmp + len, "\tSHOCK\n", 7U), 7U);
			}
			bangln(next, tmp, len);
			next = NOT_A_TIME;
		} else if (UNLIKELY(metr > till)) {
			prntln(nnfn++);
		}
		/* copy line lest we forget*/
		bangln(metr, line, nrd);

		if (UNLIKELY(metr > till)) {
			/* fetch next stamp */
			goto next;
		}
	}
	/* final buffer */
	prntln(nnfn++);
out:
	if (line) {
		free(line);
	}
	freeln();
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
