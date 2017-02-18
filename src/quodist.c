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
#include <limits.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <ieee754.h>
#include <math.h>
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
typedef size_t cnt_t;
#define strtopx		strtod32
#define pxtostr		d32tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr
#define NOT_A_TIME	((tv_t)-1ULL)

/* relevant tick dimensions */
typedef struct {
	px_t b;
	px_t a;
} quo_t;

typedef struct {
	qx_t b;
	qx_t a;
} qty_t;

typedef struct {
	px_t lo;
	px_t hi;
} pxrng_t;

typedef struct {
	qx_t lo;
	qx_t hi;
} qxrng_t;

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
zutostr(char *restrict buf, size_t bsz, cnt_t n)
{
	return snprintf(buf, bsz, "%zu", n);
}

static ssize_t(*ztostr)(char *restrict buf, size_t bsz, cnt_t n) = zutostr;

static size_t
zztostr(char *restrict buf, size_t bsz, const cnt_t *zv, size_t nz)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nz; i++) {
		buf[len++] = '\t';
		len += ztostr(buf + len, bsz - len, zv[i]);
	}
	return len;
}

static size_t
pztostr(char *restrict buf, size_t bsz, const px_t *rv, size_t nv)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nv; i++) {
		buf[len++] = '\t';
		len += pxtostr(buf + len, bsz - len, rv[i]);
	}
	return len;
}

static size_t
qztostr(char *restrict buf, size_t bsz, const qx_t *rv, size_t nv)
{
	size_t len = 0U;

	for (size_t i = 0U; i < nv; i++) {
		buf[len++] = '\t';
		len += qxtostr(buf + len, bsz - len, rv[i]);
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


static inline __attribute__((pure, const)) int
min_d(int i1, int i2)
{
	return i1 <= i2 ? i1 : i2;
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

static inline __attribute__((pure, const)) px_t
min_px(px_t p1, px_t p2)
{
	return p1 <= p2 ? p1 : p2;
}

static inline __attribute__((pure, const)) px_t
max_px(px_t p1, px_t p2)
{
	return p1 >= p2 ? p1 : p2;
}

static inline __attribute__((pure, const)) qx_t
min_qx(qx_t q1, qx_t q2)
{
	return q1 <= q2 ? q1 : q2;
}

static inline __attribute__((pure, const)) qx_t
max_qx(qx_t q1, qx_t q2)
{
	return q1 >= q2 ? q1 : q2;
}

static inline __attribute__((pure, const)) uint32_t
ilog2(const uint32_t x)
{
	return 31U - __builtin_clz(x);
}

static inline __attribute__((const, pure)) size_t
pxtoslot(const px_t x)
{
	uint32_t xm = decompd32(x).mant;
	xm <<= __builtin_clz(xm);
	xm >>= 32U - highbits;
	xm &= (1U << highbits) - 1U;
	return xm;
}

static inline __attribute__((const, pure)) size_t
qxtoslot(const qx_t x)
{
	uint64_t xm = decompd64(x).mant;
	/* we're only interested in highbits, so shift to fit */
	xm = xm >> 32U ?: xm;
	xm <<= __builtin_clz(xm);
	xm >>= 32U - highbits;
	xm &= (1U << highbits) - 1U;
	return xm;
}

static inline __attribute__((const, pure)) size_t
tvtoslot(const tv_t t)
{
	unsigned int slot = ilog2(t / MSECS + 1U);
	/* determine sub slot, if applicable */
	unsigned int width = (1U << slot);
	unsigned int base = width - 1U;
	unsigned int subs;

	/* translate in terms of base */
	subs = (t - base * MSECS) << (highbits - 5U);
	/* divide by width */
	subs /= width * MSECS;

	slot <<= highbits;
	slot >>= 5U;
	slot ^= subs;
	return slot;
}

static inline size_t
dpxtoslot(const px_t x)
{
/* specifically for price differences */
	static unsigned int _pivs[] = {-1U, 10U, 100U, 1000U, 10000U, 100000U};
	const unsigned int piv = _pivs[highbits >> 2U];
	static int minx = INT_MAX;
	bcd32_t dx = decompd32(x);
	unsigned int slot;

	minx = min_d(minx, dx.expo);
	/* map negatives to 0 */
	slot = (dx.mant & ~dx.sign);
	with (size_t magn = (piv - 1U) * (dx.expo - minx)) {
		for (; slot / piv; slot /= piv, magn += piv - 1U);
		slot += magn;
	}
	/* clamp at 32U */
	slot |= (slot < (1U << highbits)) - 1U;
	slot &= (1U << highbits) - 1U;
	return slot;
}

static inline size_t
dprtoslot(const px_t x)
{
/* specifically for price differences */
	static unsigned int _pivs[] = {-1U, 10U, 100U, 1000U, 10000U, 100000U};
	const unsigned int piv = _pivs[highbits >> 2U];
	static int minx = INT_MAX;
	bcd32_t dx = decompd32(x);
	unsigned int slot;

	minx = min_d(minx, dx.expo);
	/* map negatives to 0 */
	slot = (dx.mant & ~dx.sign);
	with (size_t magn = (piv - 1U) * (dx.expo - minx)) {
		for (; slot / piv; slot /= piv, magn += piv - 1U);
		slot += magn;
	}
	/* clamp at 32U */
	slot |= (slot < (1U << highbits)) - 1U;
	slot &= (1U << highbits) - 1U;
	return slot;
}


/* next candle time */
static tv_t nxct;

static tv_t _1st = NOT_A_TIME;
static tv_t last;

static char cont[64];
static size_t conz;

/* stats */
static union {
	cnt_t start[0U];

#define MAKE_SLOTS(n)				\
	struct {				\
		cnt_t dlt[1U << (n)];		\
		cnt_t bid[1U << (n)];		\
		cnt_t ask[1U << (n)];		\
		cnt_t bsz[1U << (n)];		\
		cnt_t asz[1U << (n)];		\
		cnt_t spr[1U << (n)];		\
		cnt_t rsp[1U << (n)];		\
						\
		tv_t tlo[1U << (n)];		\
		tv_t thi[1U << (n)];		\
						\
		px_t bhi[1U << (n)];		\
		px_t ahi[1U << (n)];		\
		px_t blo[1U << (n)];		\
		px_t alo[1U << (n)];		\
		px_t shi[1U << (n)];		\
		px_t slo[1U << (n)];		\
		px_t rhi[1U << (n)];		\
		px_t rlo[1U << (n)];		\
						\
		qx_t Bhi[1U << (n)];		\
		qx_t Ahi[1U << (n)];		\
		qx_t Blo[1U << (n)];		\
		qx_t Alo[1U << (n)];		\
	} _##n

	MAKE_SLOTS(0);
	MAKE_SLOTS(5);
	MAKE_SLOTS(9);
	MAKE_SLOTS(13);
	MAKE_SLOTS(17);
	MAKE_SLOTS(21);
} cnt;

static cnt_t *dlt;
static cnt_t *bid;
static cnt_t *ask;
static cnt_t *bsz;
static cnt_t *asz;
static cnt_t *spr;
static cnt_t *rsp;
static tv_t *tlo;
static tv_t *thi;
static px_t *blo;
static px_t *bhi;
static px_t *alo;
static px_t *ahi;
static qx_t *Blo;
static qx_t *Bhi;
static qx_t *Alo;
static qx_t *Ahi;
static px_t *slo;
static px_t *shi;
static px_t *rlo;
static px_t *rhi;
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
push_init(char *ln, size_t UNUSED(lz))
{
	size_t iz;
	char *on;

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(ln, '\t')) == NULL)) {
		return -1;
	}
	iz = on++ - ln;

	/* snarf quotes */
	if (!strtopx(on, &on) || *on++ != '\t' ||
	    !strtopx(on, &on) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	memcpy(cont, ln, conz = iz);
	return 0;
}

static int
push_beef(char *ln, size_t lz)
{
	size_t acc;
	tv_t t;
	quo_t q;
	qty_t Q;
	char *on;
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
		if (UNLIKELY(push_init(on, lz - (on - ln)) < 0)) {
			return -1;
		}
	}

	/* instrument name, don't hash him */
	if (UNLIKELY((on = strchr(on, '\t')) == NULL)) {
		return -1;
	}
	on++;

	/* snarf quotes */
	if (!(q.b = strtopx(on, &on)) || *on++ != '\t' ||
	    !(q.a = strtopx(on, &on)) || (*on != '\t' && *on != '\n')) {
		return -1;
	}

	/* measure time */
	acc = !elapsp ? 1ULL : (t - last);

	/* snarf quantities */
	if (*on == '\t' &&
	    ((Q.b = strtoqx(++on, &on)) || *on == '\t') &&
	    ((Q.a = strtoqx(++on, &on)) || *on == '\n')) {
		size_t bm = qxtoslot(Q.b);
		size_t am = qxtoslot(Q.a);

		bsz[bm] += acc;
		asz[am] += acc;

		Blo[bm] = min_qx(Blo[bm] ?: __DEC64_MOST_POSITIVE__, Q.b);
		Bhi[bm] = max_qx(Bhi[bm], Q.b);
		Alo[am] = min_qx(Alo[am] ?: __DEC64_MOST_POSITIVE__, Q.a);
		Ahi[am] = max_qx(Ahi[am], Q.a);
	}

	{
		size_t bm = pxtoslot(q.b);
		size_t am = pxtoslot(q.a);

		bid[bm] += acc;
		ask[am] += acc;

		blo[bm] = min_px(blo[bm] ?: __DEC32_MOST_POSITIVE__, q.b);
		bhi[bm] = max_px(bhi[bm], q.b);
		alo[am] = min_px(alo[am] ?: __DEC32_MOST_POSITIVE__, q.a);
		ahi[am] = max_px(ahi[am], q.a);
	}

	with (px_t s = q.a - q.b, r = quantized32(s / q.b, q.b)) {
		size_t sm = dpxtoslot(s);
		size_t rm = dprtoslot(r);

		spr[sm] += acc;
		rsp[rm] += acc;

		slo[sm] = min_px(slo[sm] ?: __DEC32_MOST_POSITIVE__, s);
		shi[sm] = max_px(shi[sm], s);
		rlo[rm] = min_px(rlo[rm] ?: __DEC32_MOST_POSITIVE__, r);
		rhi[rm] = max_px(rhi[rm], r);
	}

	with (tv_t dt = t - last) {
		size_t slot = tvtoslot(dt);

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
		static const char hdr[] = "cndl\tccy\tdimen\tmetric";
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
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, dlt, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += tztostr(buf + len, sizeof(buf) - len, tlo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	/* type t */
	buf[len++] = '\t';
	buf[len++] = 't';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += tztostr(buf + len, sizeof(buf) - len, thi, 1U << highbits);
	buf[len++] = '\n';


	/* bid */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'b';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, bid, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'b';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += pztostr(buf + len, sizeof(buf) - len, blo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'b';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += pztostr(buf + len, sizeof(buf) - len, bhi, 1U << highbits);
	buf[len++] = '\n';


	/* ask */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'a';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, ask, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'a';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += pztostr(buf + len, sizeof(buf) - len, alo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'a';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += pztostr(buf + len, sizeof(buf) - len, ahi, 1U << highbits);
	buf[len++] = '\n';


	/* spr */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 's';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, spr, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 's';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += pztostr(buf + len, sizeof(buf) - len, slo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 's';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += pztostr(buf + len, sizeof(buf) - len, shi, 1U << highbits);
	buf[len++] = '\n';

	/* relative spreads */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'r';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, rsp, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'r';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += pztostr(buf + len, sizeof(buf) - len, rlo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'r';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += pztostr(buf + len, sizeof(buf) - len, rhi, 1U << highbits);
	buf[len++] = '\n';


	/* check if there's quantities */
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (bsz[i]) {
			goto Bsz;
		}
	}
	goto prnt;

Bsz:
	/* bid quantities */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'B';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, bsz, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'B';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += qztostr(buf + len, sizeof(buf) - len, Blo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'B';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += qztostr(buf + len, sizeof(buf) - len, Bhi, 1U << highbits);
	buf[len++] = '\n';

	/* check for ask quantities */
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (asz[i]) {
			goto Asz;
		}
	}
	goto prnt;

Asz:
	/* ask quantities */
	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'A';
	buf[len++] = '\t';
	buf[len++] = 'n';
	len += zztostr(buf + len, sizeof(buf) - len, asz, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'A';
	buf[len++] = '\t';
	buf[len++] = 'L';
	len += qztostr(buf + len, sizeof(buf) - len, Alo, 1U << highbits);
	buf[len++] = '\n';

	len += cttostr(buf + len, sizeof(buf) - len, nxct);
	buf[len++] = '\t';
	len += (memcpy(buf + len, cont, conz), conz);
	buf[len++] = '\t';
	buf[len++] = 'A';
	buf[len++] = '\t';
	buf[len++] = 'H';
	len += qztostr(buf + len, sizeof(buf) - len, Ahi, 1U << highbits);
	buf[len++] = '\n';

prnt:
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
		static const char hdr[] = "cndl\tccy\tdimen\tlo\thi\tcnt\n";
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
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
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

	/* bid */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!bid[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 'b';
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, blo[i]);
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, bhi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, bid[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);

	/* ask */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!ask[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 'a';
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, alo[i]);
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, ahi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, ask[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);

	/* spr */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!spr[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 's';
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, slo[i]);
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, shi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, spr[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);

	/* relative spread */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!rsp[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 'r';
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, rlo[i]);
		buf[len++] = '\t';
		len += pxtostr(buf + len, sizeof(buf) - len, rhi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, rsp[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);

	/* bid quantities */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!bsz[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 'B';
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, Blo[i]);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, Bhi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, bsz[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);

	/* ask quantities */
	len = 0U;
	for (size_t i = 0U, n = 1U << highbits; i < n; i++) {
		if (!asz[i]) {
			continue;
		}
		/* otherwise */
		len += cttostr(buf + len, sizeof(buf) - len, nxct);
		buf[len++] = '\t';
		len += (memcpy(buf + len, cont, conz), conz);
		buf[len++] = '\t';
		buf[len++] = 'A';
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, Alo[i]);
		buf[len++] = '\t';
		len += qxtostr(buf + len, sizeof(buf) - len, Ahi[i]);
		buf[len++] = '\t';
		len += ztostr(buf + len, sizeof(buf) - len, asz[i]);
		buf[len++] = '\n';
	}
	fwrite(buf, sizeof(*buf), len, stdout);
	return;
}


#include "quodist.yucc"

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
		bid = cnt._##n.bid;		\
		ask = cnt._##n.ask;		\
		bsz = cnt._##n.bsz;		\
		asz = cnt._##n.asz;		\
		spr = cnt._##n.spr;		\
		rsp = cnt._##n.rsp;		\
						\
		tlo = cnt._##n.tlo;		\
		thi = cnt._##n.thi;		\
						\
		blo = cnt._##n.blo;		\
		bhi = cnt._##n.bhi;		\
		alo = cnt._##n.alo;		\
		ahi = cnt._##n.ahi;		\
		shi = cnt._##n.shi;		\
		slo = cnt._##n.slo;		\
		rhi = cnt._##n.rhi;		\
		rlo = cnt._##n.rlo;		\
						\
		Blo = cnt._##n.Blo;		\
		Bhi = cnt._##n.Bhi;		\
		Alo = cnt._##n.Alo;		\
		Ahi = cnt._##n.Ahi;		\
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
