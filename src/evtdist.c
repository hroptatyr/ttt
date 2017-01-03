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
#include <ieee754.h>
#include <math.h>
#include "nifty.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef long unsigned int tv_t;
#define NOT_A_TIME	((tv_t)-1ULL)

static tv_t intv;
static enum {
	UNIT_NONE,
	UNIT_SECS,
	UNIT_DAYS,
	UNIT_MONTHS,
	UNIT_YEARS,
} unit;

static unsigned int highbits = 1U;
static unsigned int elapsp;


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

static ssize_t
tvtostr(char *restrict buf, size_t bsz, tv_t t)
{
	return t < NOT_A_TIME
		? snprintf(buf, bsz, "%lu.%03lu000000", t / MSECS, t % MSECS)
		: 0U;
}

static ssize_t
cttostr(char *restrict buf, size_t bsz, tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (unit) {
	default:
	case UNIT_NONE:
		memcpy(buf, "ALL", 3U);
		return 3U;
	case UNIT_SECS:
		return tvtostr(buf, bsz, t);
	case UNIT_DAYS:
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / MSECS;
	u--;
	tm = gmtime(&u);

	switch (unit) {
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

static ssize_t
zutostr(char *restrict buf, size_t bsz, size_t n)
{
	return snprintf(buf, bsz, "%zu", n);
}

static ssize_t(*ztostr)(char *restrict buf, size_t bsz, size_t n) = zutostr;

static size_t
zztostr(char *restrict buf, size_t bsz, const size_t *zv, size_t nz)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nz; i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, bsz - len, zv[i]);
	}
	return len;
}

static size_t
tztostr(char *restrict buf, size_t bsz, const tv_t *rv, size_t nv)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nv; i++) {
		buf[len++] = '\t';
		len += tvtostr(buf + len, bsz - len, rv[i]);
	}
	return len;
}


static inline __attribute__((pure, const)) tv_t
min_tv(tv_t t1, tv_t t2)
{
	return t1 <= t2 ? t1 : t2;
}

static inline __attribute__((pure, const)) tv_t
max_tv(tv_t t1, tv_t t2)
{
	return t1 >= t2 ? t1 : t2;
}

static inline __attribute__((pure, const)) uint32_t
ilog2(const uint32_t x)
{
	return 31U - __builtin_clz(x);
}


/* next candle time */
static tv_t nxct;

static tv_t _1st = NOT_A_TIME;
static tv_t last;

/* stats */
static union {
	size_t start[0U];

#define MAKE_SLOTS(n)				\
	struct {				\
		size_t dlt[1U << (n)];		\
						\
		tv_t tlo[1U << (n)];		\
		tv_t thi[1U << (n)];		\
	} _##n

	MAKE_SLOTS(0);
	MAKE_SLOTS(5);
	MAKE_SLOTS(9);
	MAKE_SLOTS(13);
	MAKE_SLOTS(17);
	MAKE_SLOTS(21);
} cnt;

static size_t *dlt;
static tv_t *tlo;
static tv_t *thi;
static size_t cntz;

static void(*prnt_cndl)(void);
static char buf[sizeof(cnt)];

static tv_t
next_cndl(tv_t t)
{
	struct tm *tm;
	time_t u;

	switch (unit) {
	default:
	case UNIT_NONE:
		return NOT_A_TIME;
	case UNIT_SECS:
		return (t / intv + 1U) * intv;
	case UNIT_DAYS:
		t /= 24U * 60U * 60U * MSECS;
		t++;
		t *= 24U * 60U * 60U * MSECS;
		return t;
	case UNIT_MONTHS:
	case UNIT_YEARS:
		break;
	}

	u = t / MSECS;
	tm = gmtime(&u);
	tm->tm_mday = 1;
	tm->tm_yday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;

	switch (unit) {
	case UNIT_MONTHS:
		*tm = (struct tm){
			.tm_year = tm->tm_year,
			.tm_mon = tm->tm_mon + 1,
			.tm_mday = 1,
		};
		break;
	case UNIT_YEARS:
		*tm = (struct tm){
			.tm_year = tm->tm_year + 1,
			.tm_mon = 0,
			.tm_mday = 1,
		};
		break;
	}
	return mktime(tm) * MSECS;
}

static void
rset_cndl(void)
{
	/* just reset all stats pages */
	memset(cnt.start, 0, cntz);
	memset(tlo, -1, (1U << highbits) * sizeof(*tlo));
	return;
}

static int
push_beef(char *ln, size_t UNUSED(lz))
{
	size_t acc;
	char *on;
	tv_t t;
	int rc = 0;

	/* metronome is up first */
	if (UNLIKELY((t = strtotv(ln, &on)) == NOT_A_TIME)) {
		return -1;
	} else if (t < last) {
		fputs("Warning: non-chronological\n", stderr);
		rc = -1;
		goto out;
	} else if (UNLIKELY(t > nxct)) {
		prnt_cndl();
		rset_cndl();
		nxct = next_cndl(t);
		_1st = last = t;
	}

	/* measure time */
	acc = !elapsp ? 1ULL : (t - last);

	with (tv_t dt = t - last) {
		unsigned int slot = ilog2(dt / MSECS + 1U);

		/* determine sub slot, if applicable */
		with (unsigned int width = (1U << slot), base = width - 1U) {
			unsigned int subs;

			/* translate in terms of base */
			subs = (dt - base * MSECS) << (highbits - 5U);
			/* divide by width */
			subs /= width * MSECS;

			slot <<= highbits;
			slot >>= 5U;
			slot ^= subs;
		}
		/* poisson fit */
		dlt[slot] += acc;

		tlo[slot] = min_tv(tlo[slot], dt);
		thi[slot] = max_tv(thi[slot], dt);
	}

out:
	/* and store state */
	last = t;
	return rc;
}

static void
prnt_cndl_mtrx(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (UNLIKELY(_1st == NOT_A_TIME)) {
		return;
	}

	switch (ncndl++) {
		static const char hdr[] = "cndl\tdimen\tmetric";
	default:
		break;
	case 0U:
		len = (memcpy(buf, hdr, strlenof(hdr)), strlenof(hdr));
		for (size_t i = 0U; i < (1U << highbits); i++) {
			buf[len++] = '\t';
			buf[len++] = 'v';
			len += snprintf(buf + len, sizeof(buf) - len, "%zu", i);
		}
		buf[len++] = '\n';
		fwrite(buf, sizeof(*buf), len, stdout);
		len = 0U;
		break;
	}

	/* delta t */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, dlt, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += tztostr(buf + len, sizeof(buf) - len, tlo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += tztostr(buf + len, sizeof(buf) - len, thi, 1U << highbits);
	buf[len++] = '\n';

	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}

static void
prnt_cndl_molt(void)
{
	static size_t ncndl;
	size_t len = 0U;

	if (UNLIKELY(_1st == NOT_A_TIME)) {
		return;
	}

	switch (ncndl++) {
		static const char hdr[] = "cndl\tdimen\tlo\thi\tcnt\n";
	default:
		break;
	case 0U:
		fwrite(hdr, sizeof(*hdr), strlenof(hdr), stdout);
		break;
	}

	/* delta t */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!dlt[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		/* type t */
		buf[len++] = '\t';
		buf[len++] = 't';
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, tlo[i]);
		buf[len++] = '\t';
		len += tvtostr(buf + len, sizeof(buf) - len, thi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, dlt[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}


#include "evtdist.yucc"

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
		char *on;

		if (!(intv = strtoul(argi->interval_arg, &on, 10))) {
			errno = 0, serror("\
Error: cannot read interval argument, must be positive.");
			rc = 1;
			goto out;
		}
		switch (*on++) {
		secs:
		case '\0':
		case 'S':
		case 's':
			/* seconds, don't fiddle */
			intv *= MSECS;
			unit = UNIT_SECS;
			break;
		case 'm':
		case 'M':
			if (*on == 'o' || *on == 'O') {
				goto months;
			}
			intv *= 60U;
			goto secs;

		months:
			unit = UNIT_MONTHS;
			break;

		case 'y':
		case 'Y':
			unit = UNIT_YEARS;
			break;

		case 'h':
		case 'H':
			intv *= 60U * 60U;
			goto secs;
		case 'd':
		case 'D':
			unit = UNIT_DAYS;
			break;

		default:
			errno = 0, serror("\
Error: unknown suffix in interval argument, must be s, m, h, d, w, mo, y.");
			rc = 1;
			goto out;
		}
	}

	/* set candle printer */
	prnt_cndl = !argi->table_flag ? prnt_cndl_molt : prnt_cndl_mtrx;

	/* set resolution */
	highbits = (argi->verbose_flag << 2U) ^ (argi->verbose_flag > 0U);

	switch (highbits) {
#define ASS_PTRS(n)				\
		dlt = cnt._##n.dlt;		\
						\
		tlo = cnt._##n.tlo;		\
		thi = cnt._##n.thi;		\
						\
		cntz = sizeof(cnt._##n)

	case 0U:
		ASS_PTRS(0);
		break;

	case 5U:
		ASS_PTRS(5);
		break;

	case 9U:
		ASS_PTRS(9);
		break;

	case 13U:
		ASS_PTRS(13);
		break;

	case 17U:
		ASS_PTRS(17);
		break;

	case 21U:
		ASS_PTRS(21);
		break;

	default:
		errno = 0, serror("\
Error: verbose flag can only be used one to five times..");
		rc = 1;
		goto out;
	}

	/* count events or elapsed times */
	elapsp = argi->time_flag;
	ztostr = !elapsp ? zutostr : tvtostr;

	{
		char *line = NULL;
		size_t llen = 0UL;
		ssize_t nrd;

		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			(void)push_beef(line, nrd);
		}

		/* finalise our findings */
		free(line);

		/* print the final candle */
		prnt_cndl();
	}

out:
	yuck_free(argi);
	return rc;
}
