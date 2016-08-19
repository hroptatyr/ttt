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
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "nifty.h"
#include "dfp754_d32.h"
#include "dfp754_d64.h"

#define NSECS	(1000000000)
#define MSECS	(1000)

typedef _Decimal32 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr
#define NOT_A_TIME	((tv_t)-1ULL)

/* relevant tick dimensions */
typedef struct {
	tv_t t;
	px_t p;
} tik_t;

/* what-changed enum */
typedef enum {
	WHAT_UNK = 0U,
	WHAT_BID = 1U,
	WHAT_ASK = 2U,
	WHAT_BOTH = WHAT_BID | WHAT_ASK,
} what_t;

#define BID	(0U)
#define ASK	(1U)


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
	return snprintf(buf, bsz, "%lu.%03lu000000", t / MSECS, t % MSECS);
}


static tv_t metr;
static tik_t nxquo[2U] = {{NOT_A_TIME}, {NOT_A_TIME}};
static tik_t prquo[2U];
/* contract we're on about */
static char cont[64];
static size_t conz;

static what_t
push_init(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	unsigned int rc = WHAT_UNK;
	const char *ip;
	size_t iz;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		goto out;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ip = on, '\t')) == NULL)) {
		goto out;
	}
	iz = on++ - ip;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		goto out;
	}
	/* we're init'ing, so everything changed */
	prquo[BID] = nxquo[BID] = (tik_t){metr, bid};
	rc ^= WHAT_BID;

	prquo[ASK] = nxquo[ASK] = (tik_t){metr, ask};
	rc ^= WHAT_ASK;

	memcpy(cont, ip, conz = iz);
out:
	return (what_t)rc;
}

static what_t
push_beef(char *ln, size_t UNUSED(lz))
{
	px_t bid, ask;
	unsigned int rc = WHAT_UNK;
	char *on;

	/* metronome is up first */
	if (UNLIKELY((metr = strtotv(ln, &on)) == NOT_A_TIME)) {
		goto out;
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		goto out;
	}
	on++;

	/* snarf quotes */
	if (!(bid = strtopx(on, &on)) || *on++ != '\t' ||
	    !(ask = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		goto out;
	}
	/* see what changed */
	if (bid != nxquo[BID].p) {
		prquo[BID] = nxquo[BID];
		nxquo[BID] = (tik_t){metr, bid};
		rc ^= WHAT_BID;
	}
	if (ask != nxquo[ASK].p) {
		prquo[ASK] = nxquo[ASK];
		nxquo[ASK] = (tik_t){metr, ask};
		rc ^= WHAT_ASK;
	}
out:
	return (what_t)rc;
}

static int
offline(void)
{
	char *line = NULL;
	size_t llen = 0UL;
	ssize_t nrd;

	while ((nrd = getline(&line, &llen, stdin)) > 0 &&
	       push_init(line, nrd) == WHAT_UNK);

	while ((nrd = getline(&line, &llen, stdin)) > 0) {
		what_t c = push_beef(line, nrd);
		char buf[256U];
		size_t len = 0U;

		if (!c) {
			continue;
		}
		
		len = tvtostr(buf, sizeof(buf), metr);
		buf[len++] = '\t';
		len += (memcpy(buf + len, "DELT", 4U), 4U);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		if (c & WHAT_BID) {
			px_t bdlt = nxquo[BID].p - prquo[BID].p;
			len += pxtostr(buf + len, sizeof(buf) - len, bdlt);
		}
		buf[len++] = '\t';
		if (c & WHAT_ASK) {
			px_t adlt = nxquo[ASK].p - prquo[ASK].p;
			len += pxtostr(buf + len, sizeof(buf) - len, adlt);
		}
		buf[len++] = '\t';
		if (c & WHAT_BID) {
			tv_t bdlt = nxquo[BID].t - prquo[BID].t;
			len += tvtostr(buf + len, sizeof(buf) - len, bdlt);
		}
		buf[len++] = '\t';
		if (c & WHAT_ASK) {
			tv_t adlt = nxquo[ASK].t - prquo[ASK].t;
			len += tvtostr(buf + len, sizeof(buf) - len, adlt);
		}
		buf[len++] = '\n';

		fwrite(buf, 1, len, stdout);
	}

	/* finalise our findings */
	free(line);
	return 0;
}


#include "sea.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];

	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (!argi->nargs) {
		rc = offline() < 0;
	}

out:
	yuck_free(argi);
	return rc;
}
