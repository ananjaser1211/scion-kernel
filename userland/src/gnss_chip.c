/*
 * gnss_chip: the BCM4775's GNSS bring-up through /dev/hakios-gnss, as a library (gnss_chip.h; docs/gnss.md, docs/gnssd.md).
 * The protocol code is gnsstool's (src/gnsstool.c), here without its printing, for gnssd.
 */
#include "gnss_chip.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEV "/dev/hakios-gnss"

static int fd = -1;
static struct gc_events ev;
static uint8_t sbuf[70000];
static size_t slen;

struct rec {
	uint8_t kind, flags;
	uint16_t id, len;
	uint8_t data[65536];
};
static struct rec rbuf;

static void
say(const char *fmt, ...)
{
	char line[400];
	va_list ap;

	if (ev.log == NULL) return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	ev.log(line, ev.ctx);
}

static void
step(const char *name)
{
	if (ev.step) ev.step(name, ev.ctx);
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the next record within `timeout` seconds: 1, 0 on timeout, -1 when the device failed */
static int
next_rec(struct rec *r, double timeout)
{
	double end = now() + timeout;

	for (;;) {
		if (slen >= 6) {
			uint16_t len = (uint16_t)(sbuf[4] | sbuf[5] << 8);

			if (slen >= 6u + len) {
				r->kind = sbuf[0];
				r->flags = sbuf[1];
				r->id = (uint16_t)(sbuf[2] | sbuf[3] << 8);
				r->len = len;
				memcpy(r->data, sbuf + 6, len);
				memmove(sbuf, sbuf + 6 + len, slen - 6 - len);
				slen -= 6u + len;
				return 1;
			}
		}
		ssize_t n = read(fd, sbuf + slen, sizeof(sbuf) - slen);

		if (n > 0) {
			slen += (size_t)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR) {
			say("gnss_chip: read " DEV ": %s", strerror(errno));
			return -1;
		}
		if (now() >= end) return 0;
		usleep(2000);
	}
}

/* ---- MEIF from the chip -------------------------------------------------------------------------------------------- */

static int last_state = -1;
static unsigned refused;

static uint8_t bri_acc[16];
static unsigned bri_acc_n;
static uint8_t bri_q[8][13];
static unsigned bri_qn;
static unsigned long bri_rx_bytes;

static void
bri_rx(const uint8_t *d, unsigned n)
{
	bri_rx_bytes += n;
	for (unsigned i = 0; i < n; i++) {
		uint8_t c = d[i];
		unsigned len;

		if (bri_acc_n == 0 && c != 0xfe) continue;
		bri_acc[bri_acc_n++] = c;
		len = bri_acc_n;
		if ((len == 9 && bri_acc[8] == 0xfc && bri_acc[1] == 0 && bri_acc[2] == 0xfd &&
		    (bri_acc[3] == 0x0f || (bri_acc[3] | 0x80) == 0x86)) ||
		    (len == 13 && bri_acc[12] == 0xfc && bri_acc[1] == 0 && bri_acc[2] == 0xfd)) {
			if (bri_qn < 8) {
				memset(bri_q[bri_qn], 0, 13);
				memcpy(bri_q[bri_qn++], bri_acc, len);
			}
			bri_acc_n = 0;
		} else if (len >= 13) {
			unsigned j = 1;

			while (j < len && bri_acc[j] != 0xfe) j++;
			memmove(bri_acc, bri_acc + j, len - j);
			bri_acc_n = len - j;
		}
	}
}

static void
meif_got(const uint8_t *m, unsigned n)
{
	unsigned id = n >= 2 ? (unsigned)(m[0] | m[1] << 8) : 0xffff;

	if (id == 0x8002 && n >= 10) {
		unsigned c = m[6] | m[7] << 8;

		bri_rx(m + 8, c <= n - 10 ? c : n - 10);
		return;
	}
	if (id == 4 && n >= 10) {
		unsigned from = m[6] | m[7] << 8, to = m[8] | m[9] << 8;

		last_state = (int)to;
		if (ev.state) ev.state(from, to, ev.ctx);
		return;
	}
	if (id == 1 || id == 2) refused++;
	if (ev.meif) ev.meif(m, n, ev.ctx);
}

/* RPC 0x1F, [u16 n][n bytes], the bytes A5 93 ... A5 68 framed MEIF */
static void
meif_rx(const uint8_t *d, unsigned len)
{
	static uint8_t m[4096];
	static unsigned k;
	static int st;  /* 0 outside, 1 inside, 2 inside after A5, 3 outside after A5 */
	unsigned n = len >= 2 ? (unsigned)(d[0] | d[1] << 8) : 0;

	if (n + 2 > len) n = len >= 2 ? len - 2 : 0;
	for (unsigned i = 2; i < n + 2; i++) {
		uint8_t c = d[i];

		switch (st) {
		case 0: st = c == 0xa5 ? 3 : 0; break;
		case 3: st = c == 0x93 ? 1 : c == 0xa5 ? 3 : 0; k = 0; break;
		case 1:
			if (c == 0xa5) st = 2;
			else if (k < sizeof(m)) m[k++] = c;
			break;
		case 2:
			if (c == 0xa5) {
				st = 1;
				if (k < sizeof(m)) m[k++] = c;
			} else if (c == 0x68) {
				meif_got(m, k);
				st = 0;
			} else {
				st = c == 0x93 ? 1 : 0;
				k = 0;
			}
			break;
		}
	}
}

/* hand a record from the hub to whoever wants it */
static void
dispatch(const struct rec *r)
{
	if (r->kind != 'R') return;
	if (r->id == 0x1f) {
		meif_rx(r->data, r->len);
	} else if (r->id == 0x18 && r->len >= 21 && 21u + r->data[20] <= r->len) {
		char file[256];
		unsigned n = r->data[20] < sizeof(file) - 1 ? r->data[20] : sizeof(file) - 1;

		memcpy(file, r->data + 21, n);
		file[n] = 0;
		if (ev.chip_assert)
			ev.chip_assert(file, r->data[0] | r->data[1] << 8 | r->data[2] << 16 | (unsigned)r->data[3] << 24, ev.ctx);
	} else if (r->id == 0x19 && r->len > 2 && ev.text) {
		static char t[65536];
		unsigned k = 0;

		for (unsigned i = 2; i < r->len; i++) t[k++] = r->data[i] >= 0x20 && r->data[i] < 0x7f ? (char)r->data[i] : ' ';
		t[k] = 0;
		ev.text(t, ev.ctx);
	}
}

int
gc_poll(double seconds)
{
	double end = now() + seconds;
	int r;

	while (now() < end) {
		r = next_rec(&rbuf, end - now());
		if (r < 0) return -1;
		if (r == 0) break;
		dispatch(&rbuf);
	}
	return 0;
}

/* ---- to the chip ----------------------------------------------------------------------------------------------- */

static int
put(const uint8_t *p, size_t n)
{
	for (int t = 0; t < 500; t++) {
		if (write(fd, p, n) == (ssize_t)n) return 0;
		if (errno != EBUSY) {
			say("gnss_chip: write " DEV ": %s", strerror(errno));
			return -1;
		}
		usleep(10000);
	}
	say("gnss_chip: " DEV " stays busy (is the hub running, shub=6?)");
	return -1;
}

/* wait for the kernel's word on what was just sent; the hub's records on the way are handed on */
static struct rec *
answer(uint8_t kind, double timeout)
{
	double end = now() + timeout;

	while (now() < end) {
		if (next_rec(&rbuf, end - now()) <= 0) break;
		if (rbuf.kind == kind) return &rbuf;
		dispatch(&rbuf);
	}
	return NULL;
}

int
gc_rpc(unsigned id, const uint8_t *data, size_t n)
{
	uint8_t q[3 + 1000];
	struct rec *r;

	if (n > 1000) return -1;
	q[0] = 'R';
	q[1] = (uint8_t)id;
	q[2] = (uint8_t)(id >> 8);
	memcpy(q + 3, data, n);
	if (put(q, 3 + n) != 0) return -1;
	r = answer('A', 8);
	if (r == NULL || !r->flags) {
		say("gnss_chip: RPC 0x%x: %s", id, r == NULL ? "no word from the kernel" : "NOT acknowledged");
		return -1;
	}
	return 0;
}

int
gc_pvt(const char *text)
{
	uint8_t q[1000];
	size_t t = strlen(text) + 1;

	if (t > 900) return -1;
	q[0] = (uint8_t)t;
	q[1] = (uint8_t)(t >> 8);
	memcpy(q + 2, text, t);
	q[2 + t] = 0;
	return gc_rpc(0x401, q, 3 + t);
}

int
gc_log_facility(unsigned facility)
{
	uint8_t q[6] = { 0x01, (uint8_t)facility, 0xff, 0xff, 0xff, 0xff };

	return gc_rpc(0x2b, q, sizeof(q));
}

static unsigned meif_serial = 1;
static int esw_blocks = -1;

int
gc_esw_blocks(void)
{
	return esw_blocks;
}

void
gc_meif_serial(unsigned next)
{
	meif_serial = next;
}

int
gc_meif(unsigned id, const uint8_t *p, unsigned plen)
{
	static uint8_t msg[2200], fr[4500], q[2 + 1000];
	unsigned n = 6 + plen, sum = 0, k = 0;

	if (plen > 2100) return -1;
	memcpy(msg + 6, p, plen);
	if (n & 1) msg[n++] = 0;        /* the length is always even (gpsd asserts it) */
	msg[0] = (uint8_t)id;
	msg[1] = (uint8_t)(id >> 8);
	msg[2] = (uint8_t)n;
	msg[3] = (uint8_t)(n >> 8);
	msg[4] = (uint8_t)meif_serial++;
	msg[5] = 0;
	for (unsigned i = 0; i < n; i++) sum += msg[i];
	msg[n] = (uint8_t)sum;
	msg[n + 1] = (uint8_t)(sum >> 8);
	fr[k++] = 0xa5;
	fr[k++] = 0x93;
	for (unsigned i = 0; i < n + 2; i++) {
		fr[k++] = msg[i];
		if (msg[i] == 0xa5) fr[k++] = 0xa5;
	}
	fr[k++] = 0xa5;
	fr[k++] = 0x68;
	for (unsigned off = 0; off < k;) {
		unsigned c = k - off < 990 ? k - off : 990;

		q[0] = (uint8_t)c;
		q[1] = (uint8_t)(c >> 8);
		memcpy(q + 2, fr + off, c);
		if (gc_rpc(0x1e, q, 2 + c) != 0) {
			say("gnss_chip: MEIF 0x%x serial %u not delivered", id, msg[4]);
			return -1;
		}
		off += c;
	}
	return 0;
}

int
gc_wait_state(unsigned state, double seconds)
{
	double end = now() + seconds;

	while (now() < end) {
		if (last_state == (int)state) return 0;
		if (gc_poll(0.1) != 0) return -1;
	}
	return last_state == (int)state ? 0 : -1;
}

int
gc_last_state(void)
{
	return last_state;
}

static int
control(unsigned state)
{
	uint8_t st[2] = { (uint8_t)state, 0 };

	return gc_meif(3, st, 2);
}

/* ---- the baseband's boot ROM (bri), through PASSTHROUGH -------------------------------------------------------- */

static int
bri_send(const uint8_t *d, unsigned n)
{
	static uint8_t p[2 + 900];

	while (n > 0) {
		unsigned c = n < 900 ? n : 900;

		p[0] = (uint8_t)c;
		p[1] = (uint8_t)(c >> 8);
		memcpy(p + 2, d, c);
		if (gc_meif(0x8002, p, 2 + c) != 0) return -1;
		d += c;
		n -= c;
	}
	return 0;
}

/* records for up to `t` seconds, stopping at a bri answer with command `cmd` (0x09, 0x0a: the low four bits) */
static int
bri_wait(int cmd, double t, uint8_t *out)
{
	double end = now() + t;

	for (;;) {
		for (unsigned i = 0; i < bri_qn; i++) {
			uint8_t c = bri_q[i][3];
			int hit = cmd == 0x09 || cmd == 0x0a ? (c & 0x0f) == cmd : c == cmd;

			if (hit) {
				if (out != NULL) memcpy(out, bri_q[i], 13);
				memmove(bri_q[i], bri_q[i + 1], (bri_qn - i - 1) * 13);
				bri_qn--;
				return 1;
			}
		}
		if (now() >= end || next_rec(&rbuf, end - now()) <= 0) return 0;
		dispatch(&rbuf);
	}
}

/* CRC-8, polynomial 0x4d, MSB first, from 0 (GlUtlCrc; the baseband's block check) */
static uint8_t
crc8_4d(const uint8_t *d, unsigned n)
{
	uint8_t c = 0;

	while (n--) {
		c ^= *d++;
		for (int b = 0; b < 8; b++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x4d : c << 1);
	}
	return c;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
bri_read(uint32_t addr, uint32_t *val)
{
	uint8_t m[9] = { 0xff, 0x00, 0xfd, 0x09, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16),
		(uint8_t)(addr >> 24), 0xfc }, a[13];

	for (int t = 0; t < 3; t++) {
		if (bri_send(m, sizeof(m)) != 0) return -1;
		while (bri_wait(0x09, 0.5, a)) {
			if (le32(a + 4) == addr) {
				*val = le32(a + 8);
				return 0;
			}
		}
	}
	return -1;
}

/* write `size` bytes at `addr` (a last partial word rewritten whole) and check them: the byte sum and the CRC-8 */
static int
bri_block(const uint8_t *d, uint32_t addr, unsigned size)
{
	static uint8_t m[8 + 4096 + 32];
	unsigned body = size & ~3u, tail = size & 3u, sum = 0;
	uint8_t crc = crc8_4d(d, size), a[13];

	if (size < 4 || size > 4096) return -1;
	for (unsigned i = 0; i < size; i++) sum += d[i];
	for (int t = 0; t < 3; t++) {
		unsigned k = 0;
		uint32_t at = addr + size - 4;

		m[k++] = 0xfe;
		m[k++] = (uint8_t)(body / 4 - 1);
		m[k++] = 0xfd;
		m[k++] = 0x4c;
		for (int i = 0; i < 4; i++) m[k++] = (uint8_t)(addr >> (8 * i));
		memcpy(m + k, d, body);
		k += body;
		m[k++] = 0xfc;
		if (tail) {
			m[k++] = 0xfe;
			m[k++] = 0x00;
			m[k++] = 0xfd;
			m[k++] = 0x4c;
			for (int i = 0; i < 4; i++) m[k++] = (uint8_t)(at >> (8 * i));
			memcpy(m + k, d + size - 4, 4);
			k += 4;
			m[k++] = 0xfc;
		}
		m[k++] = 0xfe;
		m[k++] = 0x00;
		m[k++] = 0xfd;
		m[k++] = 0x4a;
		for (int i = 0; i < 4; i++) m[k++] = (uint8_t)(addr >> (8 * i));
		m[k++] = (uint8_t)size;
		m[k++] = (uint8_t)(size >> 8);
		m[k++] = 0xfc;
		if (bri_send(m, k) != 0) return -1;
		while (bri_wait(0x0a, 2.0, a)) {
			uint32_t ra = le32(a + 4), rs = a[8] | a[9] << 8 | a[10] << 16;

			if (ra != addr) continue;
			if (rs == (sum & 0xffffff) && a[11] == crc) return 0;
			say("gnss_chip: block %05x (%u bytes): sum %06x crc %02x, expected %06x %02x", addr, size, rs, a[11],
			    sum & 0xffffff, crc);
			break;
		}
	}
	return -1;
}

int
gc_esw_load(const struct gc_segment *seg, int nseg)
{
	static const uint8_t autobaud[16] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x80, 0x80 };
	static const uint8_t hwflow[9] = { 0xff, 0x00, 0xfd, 0x0f, 0x01, 0x33, 0x0e, 0x10, 0xfc };
	static const uint8_t hwflow2[17] = { 0xfe, 0x01, 0xfd, 0x43, 0x30, 0x01, 0x00, 0xc0, 0x01, 0x00, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x80, 0xfc };
	static const uint8_t gool[9] = { 0xff, 0x00, 0xfd, 0x05, 0x55, 0xaa, 0x00, 0xff, 0xfc };
	static uint8_t img[256 * 1024];
	unsigned total = 0, blocks = 0;
	uint8_t a[13];
	uint32_t v;
	double t0;
	int ok = 0;

	esw_blocks = -1;
	/* RESET, then PASSTHROUGH: the chip forwards MEIF 0x8002 PASS_THRU bytes to the baseband */
	meif_serial = 1;
	if (control(GC_RESET) != 0) return -1;
	bri_wait(-1, 1.5, NULL);
	last_state = -1;
	refused = 0;
	if (control(GC_PASSTHROUGH) != 0) return -1;
	for (double end = now() + 3; now() < end && last_state != GC_PASSTHROUGH && refused == 0;) bri_wait(-1, 0.1, NULL);
	if (last_state != GC_PASSTHROUGH) say("gnss_chip: no PASSTHROUGH state report (last %d); going on", last_state);
	meif_serial = 1;        /* after a RESET the chip wants serial 1 next; CONTROL messages are not counted */

	for (int t = 0; t < 3 && !ok; t++) {
		if (bri_send(autobaud, sizeof(autobaud)) != 0 || bri_send(hwflow, sizeof(hwflow)) != 0 ||
		    bri_send(hwflow2, sizeof(hwflow2)) != 0) return -1;
		ok = bri_wait(0x0f, 0.6, a);
	}
	if (!ok) say("gnss_chip: bri: no flow control answer; going on");
	if (bri_read(0xc0000000, &v) != 0) {
		say("gnss_chip: bri: the baseband's boot ROM does not answer");
		return -1;
	}
	say("gnss_chip: bri: baseband version 0x%08x", v);

	t0 = now();
	for (int s = 0; s < nseg; s++) {
		FILE *f = fopen(seg[s].path, "rb");
		size_t len;

		if (f == NULL) {
			say("gnss_chip: %s: %s", seg[s].path, strerror(errno));
			return -1;
		}
		len = fread(img, 1, sizeof(img), f);
		fclose(f);
		if (len < 4 || (len & 3)) {
			say("gnss_chip: %s: %zu bytes, not whole words", seg[s].path, len);
			return -1;
		}
		for (size_t off = 0; off < len;) {
			size_t rem = len - off, sz = rem;

			if (rem > 1020) sz = rem - 1020 < 4 ? 1016 : 1020;      /* never leave less than a word */
			if (bri_block(img + off, seg[s].addr + (uint32_t)off, (unsigned)sz) != 0) {
				say("gnss_chip: ESW block at %05x failed", seg[s].addr + (uint32_t)off);
				return -1;
			}
			off += sz;
			total += (unsigned)sz;
			blocks++;
		}
	}
	say("gnss_chip: ESW: %u bytes in %u blocks written and checked, %.1f s", total, blocks, now() - t0);
	esw_blocks = (int)blocks;
	bri_rx_bytes = 0;
	if (bri_send(gool, sizeof(gool)) != 0) return -1;      /* GetOutOfLegacy: the ESW starts */
	bri_wait(-1, 2, NULL);
	say("gnss_chip: ESW running (%lu bytes from the baseband)", bri_rx_bytes);
	return 0;
}

int
gc_peek(uint32_t addr, uint16_t len, uint8_t *out)
{
	uint8_t q[10];
	struct rec *r;

	q[0] = 'S';
	q[1] = 0xec;            /* the memory reader the kernel adds with shub_peek=1 (docs/gnss.md, "The tools") */
	q[2] = q[3] = 0;
	for (int i = 0; i < 4; i++) q[4 + i] = (uint8_t)(addr >> (8 * i));
	q[8] = (uint8_t)len;
	q[9] = (uint8_t)(len >> 8);
	if (put(q, sizeof(q)) != 0) return -1;
	r = answer('S', 3);
	if (r == NULL || !r->flags || r->len != len) return -1;
	memcpy(out, r->data, len);
	return 0;
}

/*
 * The MiniMe's AR table cut (docs/gnss.md, "What gpsd's host engine programs into the radio" and the indoor run after
 * it): 0xFFFF over the first register number of the hub patch's 104-entry table at 0x008300b0 makes INIT_STAGE B send
 * no AR writes, as gpsd's host engine does. RPC 0x05 is the ROM's memory write. Written only over the table this hub
 * patch has (its first entry reads 04 00 00 00 2a 00 00 00), and read back; it lasts until the hub restarts.
 */
#define AR_TABLE 0x008300b0u

int
gc_ar_table_cut(void)
{
	static const uint8_t first[8] = { 0x04, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00 };
	static const uint8_t wr[8] = { 0xb0, 0x00, 0x83, 0x00, 0x02, 0x00, 0xff, 0xff };
	uint8_t b[8];

	if (gc_peek(AR_TABLE, sizeof(b), b) != 0) {
		say("gnss_chip: AR table cut skipped: no answer from the peek (boot-arg shub_peek=1?)");
		return 0;
	}
	if (b[0] == 0xff && b[1] == 0xff && memcmp(b + 2, first + 2, 6) == 0) {
		say("gnss_chip: AR table already cut (since the hub started)");
		return 1;
	}
	if (memcmp(b, first, sizeof(first)) != 0) {
		say("gnss_chip: AR table cut skipped: 0x%08x holds %02x %02x %02x %02x %02x %02x %02x %02x, not the table",
		    AR_TABLE, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
		return 0;
	}
	if (gc_rpc(0x05, wr, sizeof(wr)) != 0) return -1;
	if (gc_peek(AR_TABLE, sizeof(b), b) != 0 || b[0] != 0xff || b[1] != 0xff) {
		say("gnss_chip: AR table cut skipped: the peek does not show it; going on with the table");
		return 0;
	}
	say("gnss_chip: AR table cut (0xffff at 0x%08x, read back)", AR_TABLE);
	return 1;
}

int
gc_engine_on(void)
{
	/* "lte_band_13:0;esw_ctrl:1,1,0;": keep (retain) the ESW the baseband has, for the on-chip engine */
	static const uint8_t esw_ctrl[] = { 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 'l', 't', 'e', '_', 'b', 'a', 'n', 'd', '_',
		'1', '3', ':', '0', ';', 'e', 's', 'w', '_', 'c', 't', 'r', 'l', ':', '1', ',', '1', ',', '0', ';', 0, 0, 0 };
	/*
	 * CONFIG_SET as gpsd builds it for gps.xml (docs/gnss.md, "gpsd's real CONFIG_SET"), WITHOUT the external
	 * oscillator: no item 10, and item 5's byte at +8 back to 6, since the 49.152 MHz CNTIN reference comes from the
	 * modem, which does not run here. Six items: 5 MEASUREMENT, 8 ME_OSC_PARAMS (2 ppm), 0x8002 MISC, 0x8006
	 * PLATFORM_SPECIFIC, 7 RF_PARAMS (zero), 0x8001 LOGGINGS (on). The bytes of the d8957eb run.
	 */
	static const uint8_t config[] = { 0x06, 0x00,
		0x05, 0x00, 0x10, 0x00, 0xe8, 0x03, 0x01, 0x00, 0x00, 0x03, 0x06, 0x00, 0x0a, 0x00, 0x0a, 0x00,
		0x08, 0x00, 0x08, 0x00, 0x00, 0x02, 0x01, 0x00,
		0x02, 0x80, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x06, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x80, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 };
	/* MEIF_EXT_LNA_CONF_SB (25): an external LNA (gps.xml); sent in the open-sky runs and the d8957eb run */
	static const uint8_t ext_lna[] = { 0x01, 0x00, 0x19, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00 };

	/* serials: RESET outside the count, then 1 esw_ctrl, 2 CONFIG_SET, 3 EXT_LNA, 4 IDLE, 5 ON */
	step("RESET");
	meif_serial = 99;
	if (control(GC_RESET) != 0) return -1;
	gc_poll(2);
	meif_serial = 1;
	step("esw_ctrl");
	if (gc_meif(0x10, esw_ctrl, sizeof(esw_ctrl)) != 0) return -1;
	gc_poll(1);
	step("CONFIG_SET");
	if (gc_meif(6, config, sizeof(config)) != 0) return -1;
	gc_poll(1);
	step("EXT_LNA");
	if (gc_meif(6, ext_lna, sizeof(ext_lna)) != 0) return -1;
	gc_poll(1);
	step("IDLE");
	last_state = -1;
	if (control(GC_IDLE) != 0 || gc_wait_state(GC_IDLE, 15) != 0) {
		say("gnss_chip: the engine did not reach IDLE (last state %d)", last_state);
		return -1;
	}
	step("ON");
	if (control(GC_ON) != 0 || gc_wait_state(GC_ON, 5) != 0) {
		say("gnss_chip: the engine did not reach ON (last state %d)", last_state);
		return -1;
	}
	return 0;
}

int
gc_open(const struct gc_events *e)
{
	memset(&ev, 0, sizeof(ev));
	if (e) ev = *e;
	slen = 0;
	last_state = -1;
	bri_qn = bri_acc_n = 0;
	fd = open(DEV, O_RDWR | O_NONBLOCK);
	return fd < 0 ? -1 : 0;
}

void
gc_close(void)
{
	if (fd >= 0) close(fd);
	fd = -1;
}

/*
 * Masks what could be a position in a line of the chip's text, in place: every number with four or more decimals
 * (except in the engine's noise statistics), and the latitude and longitude fields of NMEA GGA, RMC, GLL and GNS.
 */
void
gc_mask_position(char *t)
{
	size_t k = strlen(t);
	int stats = (strstr(t, "NoiseStats") != NULL || strstr(t, "DagcStats") != NULL) && strstr(t, "$G") == NULL &&
	    strstr(t, "Lat") == NULL && strstr(t, "Lon") == NULL && strstr(t, "LLA") == NULL && strstr(t, "NMEA") == NULL;

	for (size_t i = 0; i < k && !stats; i++) {
		if (t[i] == '.' && i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') {
			size_t j = i + 1;

			while (j < k && t[j] >= '0' && t[j] <= '9') j++;
			if (j - i - 1 >= 4) {
				size_t b = i;

				while (b > 0 && ((t[b - 1] >= '0' && t[b - 1] <= '9') || t[b - 1] == '-')) b--;
				for (size_t x = b; x < j; x++) t[x] = '#';
			}
			i = j;
		}
	}
	for (char *d = strchr(t, '$'); d; d = strchr(d + 1, '$')) {
		int from, f = 0;

		if (strlen(d) < 7) break;
		if (strncmp(d + 3, "GGA", 3) == 0 || strncmp(d + 3, "GNS", 3) == 0) from = 2;
		else if (strncmp(d + 3, "GLL", 3) == 0) from = 1;
		else if (strncmp(d + 3, "RMC", 3) == 0) from = 3;
		else continue;
		for (char *c = d + 1; *c && *c != '*' && *c != '$'; c++) {
			if (*c == ',') f++;
			else if (f >= from && f < from + 4) *c = '#';
		}
	}
}
