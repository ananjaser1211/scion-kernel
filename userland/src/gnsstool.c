/*
 * gnsstool: the GNSS bring-up's hands on the BCM4775's link, through the kernel's /dev/hakios-gnss
 * (drivers/input/ssp/hakios.ssp.c; docs/gnss.md says what is known and what this is for).
 *
 *   gnsstool listen [SECONDS]            print every RPC the hub sends that is not SSP (logs, GNSS traffic), as it comes
 *   gnsstool rpc ID [HEX] [SECONDS]      send RPC ID (hex) with HEX bytes in a reliable packet, then listen
 *   gnsstool prpc ID [HEX] [SECONDS]     the same in a plain packet
 *   gnsstool ssp CMD OPTS ARG LEN [HEX]  one raw SSP transaction (all hex; OPTS 0 reads LEN bytes, 1 writes HEX)
 *   gnsstool peek ADDR [LEN]             the hub's memory (needs the boot-arg shub_peek=1): hex dump
 *   gnsstool dump ADDR LEN FILE [CHUNK]  the same into FILE, CHUNK bytes per read (default 256)
 *   gnsstool meif ID [HEX] [SECONDS]     send one MEIF message to the chip's measurement engine (RPC 0x1E, "TxNav data"),
 *                                        then listen; ID and HEX in hex, HEX is the payload after the serial number
 *   gnsstool esw [-r] [-n] [-2] [-x] [-l SECONDS] [FILE@ADDR ...]
 *                                        put the engine in PASSTHROUGH and talk to the baseband's boot ROM as gpsd does:
 *                                        handshake, then load each FILE at baseband address ADDR (hex) in checked blocks;
 *                                        -r RESET first, -n no flow control, -2 clock doubler, -x GetOutOfLegacy at the end
 *   gnsstool pvt TEXT [SECONDS]          the on-chip engine's debug request (RPC 0x401): gnss_start, gnss_stop,
 *                                        enable_log_to_host_LOG_INFO_Overwrite, ...
 *   gnsstool bri HEX [SECONDS]           raw bytes for the baseband in a MEIF PASS_THRU (in PASSTHROUGH), then listen
 *
 * MEIF_SERIAL in the environment sets the serial of the first MEIF message sent (meif, esw, bri; default 1).
 *
 * MEIF (gpsd's GlPe_MeIfImpl, the hub's GlMe_PeIfImpl): [u16 id][u16 len][u16 serial][payload][u16 sum], len counting
 * everything before the sum, the sum adding all those bytes; framed A5 93 ... A5 68 with every A5 inside doubled. RPC 0x1E
 * carries [u16 n][n framed bytes] to the chip, RPC 0x1F the same back. Only ids, lengths and states are printed: the
 * measurement and position messages are not, so no position ever reaches a log.
 *
 * Records from the kernel: [u8 kind][u8 flags][u16 id][u16 len][len bytes], kinds 'R' (an RPC from the hub), 'A' (a
 * raw RPC sent; flags 1: acknowledged) and 'S' (a raw SSP transaction's answer; flags 1: answered).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEV "/dev/hakios-gnss"

static int fd = -1;
static uint8_t sbuf[70000];
static size_t slen;

struct rec {
	uint8_t kind, flags;
	uint16_t id, len;
	uint8_t data[65536];
};

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the next record within `timeout` seconds: 1, or 0 on timeout */
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
			perror("read " DEV);
			exit(1);
		}
		if (now() >= end) {
			return 0;
		}
		usleep(2000);
	}
}

static const char *
meif_name(unsigned id)
{
	static const char *low[] = { "ACK", "NACK", "ERROR", "CONTROL", "STATE_REPORT", "EVENT_REPORT", "CONFIG_SET",
		"CONFIG_GET", "CONFIG_VALUES", "AIDING", "DATA_AIDING", "MEASUREMENT", "DATA", "DEBUG", "TEST_REQUEST",
		"TEST_RESULT", "SW_UPLOAD", "SENSOR_CONTROL", "SENSOR_EVENT_REPORT", "SENSOR_MEASUREMENT", "ASIC_CFG", "ASIC",
		"PLATF_STATS" };

	if (id < sizeof(low) / sizeof(low[0])) {
		return low[id];
	}
	switch (id) {
	case 0x8000: return "LOGGING";
	case 0x8002: return "PASS_THRU_MODE";
	case 0x8003: return "DEBUG_CONFIG";
	case 0x8004: return "DAC";
	case 0x8100: return "GLPP2";
	case 0x8101: return "COMPACTMEAS";
	case 0x8102: return "TIME_TRANSFER";
	case 0x8103: return "GPS_TIME_REPORT";
	case 0x8183: return "PEX_MICRO_NAV_CONFIG";
	case 0x8185: return "HOST_ME_HB";
	case 0x81a0: return "PEX_TIME_REPORT";
	case 0x81a1: return "PEX_STATUS_REPORT";
	case 0x81d0: return "PEX_NVRAM";
	default: return "?";
	}
}

static const char *
meif_state(unsigned s)
{
	static const char *n[] = { "RESET", "SETUP", "OFF", "SLEEP", "IDLE", "ON", "POWER_SAVE", "TEST", "SHUTDOWN", "ERROR",
		"PASSTHROUGH", "REQ_CURRENT" };

	return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/*
 * Masks what could be a position in a line of the chip's text, in place: every number with four or more decimals
 * (except in the engine's noise statistics), and the latitude and longitude fields of an NMEA GGA, RMC, GLL or GNS sentence, whatever their precision.
 */
static void
mask_position(char *t)
{
	size_t k = strlen(t);
	/* the measurement engine's noise and gain statistics are not positions, and they say whether the RF path works */
	int stats = (strstr(t, "NoiseStats") != NULL || strstr(t, "DagcStats") != NULL) && strchr(t, '$') == NULL &&
	    strstr(t, "Lat") == NULL && strstr(t, "Lon") == NULL && strstr(t, "LLA") == NULL && strstr(t, "NMEA") == NULL;

	for (size_t i = 0; i < k && !stats; i++) {
		if (t[i] == '.' && i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') {
			size_t j = i + 1;

			while (j < k && t[j] >= '0' && t[j] <= '9') {
				j++;
			}
			if (j - i - 1 >= 4) {
				size_t b = i;

				while (b > 0 && ((t[b - 1] >= '0' && t[b - 1] <= '9') || t[b - 1] == '-')) {
					b--;
				}
				for (size_t x = b; x < j; x++) {
					t[x] = '#';
				}
			}
			i = j;
		}
	}
	for (char *d = strchr(t, '$'); d; d = strchr(d + 1, '$')) {
		int from;

		if (strlen(d) < 7) {
			break;
		}
		/* the fields after the sentence name: GGA, GNS time, lat, N/S, lon, E/W; GLL lat, ...; RMC time, status, lat, ... */
		if (strncmp(d + 3, "GGA", 3) == 0 || strncmp(d + 3, "GNS", 3) == 0) {
			from = 2;
		} else if (strncmp(d + 3, "GLL", 3) == 0) {
			from = 1;
		} else if (strncmp(d + 3, "RMC", 3) == 0) {
			from = 3;
		} else {
			continue;
		}
		int f = 0;

		for (char *c = d + 1; *c && *c != '*' && *c != '$'; c++) {
			if (*c == ',') {
				f++;
			} else if (f >= from && f < from + 4) {
				*c = '#';
			}
		}
	}
}

/* one MEIF message, unframed: ids, lengths and states only */
static void
meif_print(const uint8_t *m, unsigned n)
{
	unsigned id, len, sum = 0;

	if (n < 6) {
		printf("    MEIF short (%u bytes)\n", n);
		return;
	}
	id = m[0] | m[1] << 8;
	len = m[2] | m[3] << 8;
	for (unsigned i = 0; i < len && i < n; i++) {
		sum += m[i];
	}
	printf("    MEIF 0x%x %s len %u serial %u%s", id, meif_name(id), len, m[4] | m[5] << 8,
	    len + 2 != n || (uint16_t)sum != (m[n - 2] | m[n - 1] << 8) ? " (bad length or sum)" : "");
	if (id == 4 && n >= 10) {
		printf(" %s -> %s", meif_state(m[6] | m[7] << 8), meif_state(m[8] | m[9] << 8));
	}
	if (id == 0xd || id == 0x8000) {
		/* the engine's log text. Every number with four or more decimals is masked, so a position never gets out */
		char t[1600];
		unsigned k = 0;

		for (unsigned i = 6; i + 2 < n && k + 1 < sizeof(t); i++) {
			t[k++] = m[i] >= 0x20 && m[i] < 0x7f ? (char)m[i] : m[i] == 0 ? ' ' : '.';
		}
		t[k] = 0;
		mask_position(t);
		printf(" \"%s\"\n", t);
		return;
	}
	if (id <= 2 || id == 4 || id == 8) {
		printf(" :");
		for (unsigned i = 6; i + 2 < n && i < (id == 8 || id == 2 ? 400u : 40u); i++) {
			printf(" %02x", m[i]);
		}
	}
	putchar('\n');
}

static void meif_got(const uint8_t *m, unsigned n);

/* RPC 0x1F, [u16 n][n bytes], the bytes A5 93 ... A5 68 framed MEIF */
static void
meif_rx(const uint8_t *d, unsigned len)
{
	static uint8_t m[4096];
	static unsigned k;
	static int st;  /* 0 outside, 1 inside, 2 inside after A5, 3 outside after A5 */
	unsigned n = len >= 2 ? (unsigned)(d[0] | d[1] << 8) : 0;

	if (n + 2 > len) {
		n = len >= 2 ? len - 2 : 0;
	}
	for (unsigned i = 2; i < n + 2; i++) {
		uint8_t c = d[i];

		switch (st) {
		case 0:
			st = c == 0xa5 ? 3 : 0;
			break;
		case 3:
			st = c == 0x93 ? 1 : c == 0xa5 ? 3 : 0;
			k = 0;
			break;
		case 1:
			if (c == 0xa5) {
				st = 2;
			} else if (k < sizeof(m)) {
				m[k++] = c;
			}
			break;
		case 2:
			if (c == 0xa5) {
				st = 1;
				if (k < sizeof(m)) {
					m[k++] = c;
				}
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

static void
print_rec(const struct rec *r)
{
	int text = r->len > 2;

	for (unsigned i = 2; i < r->len && text; i++) {
		uint8_t c = r->data[i];

		text = (c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t' || c == 0;
	}
	printf("%.3f %c%s id 0x%x len %u:", now(), r->kind, r->flags ? "+" : "", r->id, r->len);
	if (r->kind == 'R' && r->id == 0x1f) {
		printf(" (TxNav data from the chip)\n");
		meif_rx(r->data, r->len);
		return;
	}
	if (r->kind == 'R' && r->id == 0x18 && r->len >= 21 && 21u + r->data[20] <= r->len) {
		/* an assert on the chip: [u32 line][u32 arg2][u32 arg3][u32 arg4][u32 ts][u8 n][file, NUL] */
		printf(" ASSERT %.*s line %u args %08x %08x %08x ts %08x\n", r->data[20], (const char *)r->data + 21,
		    (unsigned)(r->data[0] | r->data[1] << 8 | r->data[2] << 16 | (unsigned)r->data[3] << 24),
		    (unsigned)(r->data[4] | r->data[5] << 8 | r->data[6] << 16 | (unsigned)r->data[7] << 24),
		    (unsigned)(r->data[8] | r->data[9] << 8 | r->data[10] << 16 | (unsigned)r->data[11] << 24),
		    (unsigned)(r->data[12] | r->data[13] << 8 | r->data[14] << 16 | (unsigned)r->data[15] << 24),
		    (unsigned)(r->data[16] | r->data[17] << 8 | r->data[18] << 16 | (unsigned)r->data[19] << 24));
		return;
	}
	if (text) {
		static char t[65536];
		unsigned k = 0;

		for (unsigned i = 2; i < r->len; i++) {
			t[k++] = r->data[i] >= 0x20 ? (char)r->data[i] : r->data[i] == 0 ? ' ' : '.';
		}
		t[k] = 0;
		mask_position(t);
		printf(" \"%s\"\n", t);
		return;
	}
	for (unsigned i = 0; i < r->len && i < 96; i++) {
		printf(" %02x", r->data[i]);
	}
	printf("%s\n", r->len > 96 ? " ..." : "");
}

static void
listen_for(double secs)
{
	struct rec *r = malloc(sizeof(*r));
	double end = now() + secs;

	while (now() < end) {
		if (next_rec(r, end - now())) {
			print_rec(r);
			fflush(stdout);
		}
	}
	free(r);
}

static void
put(const uint8_t *p, size_t n)
{
	for (int t = 0; t < 500; t++) {
		if (write(fd, p, n) == (ssize_t)n) {
			return;
		}
		if (errno != EBUSY) {
			perror("write " DEV);
			exit(1);
		}
		usleep(10000);
	}
	fprintf(stderr, "gnsstool: " DEV " stays busy (is the hub running, shub=6?)\n");
	exit(1);
}

static size_t
unhex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;

	while (*s && n < max) {
		unsigned v;

		if (*s == ' ' || *s == ':' || *s == ',') {
			s++;
			continue;
		}
		if (sscanf(s, "%2x", &v) != 1) {
			break;
		}
		out[n++] = (uint8_t)v;
		s += s[1] ? 2 : 1;
	}
	return n;
}

/* wait for the record of kind `kind` that answers a request; print others on the way. NULL on timeout */
static struct rec *
answer(uint8_t kind, double timeout)
{
	static struct rec r;
	double end = now() + timeout;

	while (now() < end) {
		if (!next_rec(&r, end - now())) {
			break;
		}
		if (r.kind == kind) {
			return &r;
		}
		print_rec(&r);
	}
	return NULL;
}

static struct rec *
ssp(uint8_t cmd, uint16_t opts, uint32_t arg, uint16_t len, const uint8_t *data, size_t n)
{
	uint8_t q[10 + 64];

	q[0] = 'S';
	q[1] = cmd;
	q[2] = (uint8_t)opts;
	q[3] = (uint8_t)(opts >> 8);
	for (int i = 0; i < 4; i++) {
		q[4 + i] = (uint8_t)(arg >> (8 * i));
	}
	q[8] = (uint8_t)len;
	q[9] = (uint8_t)(len >> 8);
	if (n > 64) {
		n = 64;
	}
	memcpy(q + 10, data, n);
	put(q, 10 + n);
	return answer('S', 3);
}

static int
peek(uint32_t addr, uint16_t len, uint8_t *out)
{
	struct rec *r = ssp(0xEC, 0, addr, len, NULL, 0);

	if (r == NULL || !r->flags || r->len != len) {
		return -1;
	}
	memcpy(out, r->data, len);
	return 0;
}

/*
 * The baseband's boot ROM interface ("bri"), reached through the measurement engine's PASSTHROUGH state, as gpsd's host
 * measurement engine does it (GlMeSrdAsicinit; docs/gnss.md has the protocol). CONTROL(PASSTHROUGH) makes the chip
 * forward the bytes of every MEIF 0x8002 PASS_THRU ([u16 n][n bytes]) to the baseband as they are, and send the
 * baseband's bytes back the same way. The boot ROM's messages are `ff|fe <n> fd <cmd> <args> fc`; its answers start
 * `fe 00 fd` and are 9 bytes (flow control 0x0f, clock doubler 0x86/0x06) or 13 (a read, 0x?9: address, data; a block
 * check, 0x?a: address, 24-bit byte sum, CRC-8).
 */
static unsigned meif_serial = 1;        /* the serial of the next MEIF message sent */
static int meif_last_state = -1;        /* the new state of the last STATE_REPORT */
static unsigned meif_refused;           /* NACKs and ERRORs seen */
static uint8_t bri_acc[16];
static unsigned bri_acc_n;
static uint8_t bri_q[8][13];            /* complete answers not yet taken */
static unsigned bri_qn;
static unsigned long bri_rx_bytes;
static int bri_verbose = 1;

static void
bri_rx(const uint8_t *d, unsigned n)
{
	bri_rx_bytes += n;
	if (bri_verbose) {
		printf("    baseband %u bytes:", n);
		for (unsigned i = 0; i < n && i < 48; i++) {
			printf(" %02x", d[i]);
		}
		printf("%s\n", n > 48 ? " ..." : "");
	}
	for (unsigned i = 0; i < n; i++) {
		uint8_t c = d[i];
		unsigned len;

		if (bri_acc_n == 0 && c != 0xfe) {
			continue;
		}
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
			/* garbage: start again at the next fe */
			unsigned j = 1;

			while (j < len && bri_acc[j] != 0xfe) {
				j++;
			}
			memmove(bri_acc, bri_acc + j, len - j);
			bri_acc_n = len - j;
		}
	}
}

/* a whole MEIF message from the chip: the baseband's bytes to the bri parser, the rest printed */
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
		meif_last_state = m[8] | m[9] << 8;
	}
	if (id == 1 || id == 2) {
		meif_refused++;
	}
	meif_print(m, n);
}

/* one MEIF message to the chip, framed, in RPCs 0x1E of at most 990 framed bytes; 0 when every RPC was acknowledged */
static int
meif_send(unsigned id, const uint8_t *p, unsigned plen)
{
	static uint8_t msg[2200], fr[4500], q[3 + 1000];
	unsigned n = 6 + plen, sum = 0, k = 0;

	if (plen > 2100) {
		return -1;
	}
	memcpy(msg + 6, p, plen);
	if (n & 1) {
		msg[n++] = 0;   /* the length is always even (gpsd asserts it); the chip allows the pad byte */
	}
	msg[0] = (uint8_t)id;
	msg[1] = (uint8_t)(id >> 8);
	msg[2] = (uint8_t)n;
	msg[3] = (uint8_t)(n >> 8);
	msg[4] = (uint8_t)meif_serial++;
	msg[5] = 0;
	for (unsigned i = 0; i < n; i++) {
		sum += msg[i];
	}
	msg[n] = (uint8_t)sum;
	msg[n + 1] = (uint8_t)(sum >> 8);
	fr[k++] = 0xa5;
	fr[k++] = 0x93;
	for (unsigned i = 0; i < n + 2; i++) {
		fr[k++] = msg[i];
		if (msg[i] == 0xa5) {
			fr[k++] = 0xa5;
		}
	}
	fr[k++] = 0xa5;
	fr[k++] = 0x68;
	for (unsigned off = 0; off < k;) {
		unsigned c = k - off < 990 ? k - off : 990;
		struct rec *r;

		q[0] = 'R';
		q[1] = 0x1e;
		q[2] = 0;
		q[3] = (uint8_t)c;
		q[4] = (uint8_t)(c >> 8);
		memcpy(q + 5, fr + off, c);
		put(q, 5 + c);
		r = answer('A', 8);
		if (r == NULL || !r->flags) {
			printf("RPC 0x1e (MEIF 0x%x serial %u): %s\n", id, msg[4], r == NULL ? "no word from the kernel" :
			    "NOT acknowledged");
			return -1;
		}
		off += c;
	}
	return 0;
}

/* bytes for the baseband, in PASS_THRU messages of at most 900 */
static int
bri_send(const uint8_t *d, unsigned n)
{
	static uint8_t p[2 + 900];

	while (n > 0) {
		unsigned c = n < 900 ? n : 900;

		p[0] = (uint8_t)c;
		p[1] = (uint8_t)(c >> 8);
		memcpy(p + 2, d, c);
		if (meif_send(0x8002, p, 2 + c) != 0) {
			return -1;
		}
		d += c;
		n -= c;
	}
	return 0;
}

/* handle what the chip sends for up to `t` seconds; stop early at a bri answer with command `cmd` (0x09 and 0x0a match
 * on the low four bits) and copy it to `out`. 1 when it came */
static int
bri_wait(int cmd, double t, uint8_t *out)
{
	static struct rec r;
	double end = now() + t;

	for (;;) {
		for (unsigned i = 0; i < bri_qn; i++) {
			uint8_t c = bri_q[i][3];
			int hit = cmd == 0x09 || cmd == 0x0a ? (c & 0x0f) == cmd : c == cmd;

			if (hit) {
				if (out != NULL) {
					memcpy(out, bri_q[i], 13);
				}
				memmove(bri_q[i], bri_q[i + 1], (bri_qn - i - 1) * 13);
				bri_qn--;
				return 1;
			}
		}
		if (now() >= end || !next_rec(&r, end - now())) {
			return 0;
		}
		if (r.kind != 'A') {
			print_rec(&r);
			fflush(stdout);
		}
	}
}

/* CRC-8, polynomial 0x4d, MSB first, from 0 (GlUtlCrc; the baseband's block check) */
static uint8_t
crc8_4d(const uint8_t *d, unsigned n)
{
	uint8_t c = 0;

	while (n--) {
		c ^= *d++;
		for (int b = 0; b < 8; b++) {
			c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x4d : c << 1);
		}
	}
	return c;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* bri: read a baseband register (`ff 00 fd 09 <addr> fc`); 0 and *val when answered */
static int
bri_read(uint32_t addr, uint32_t *val)
{
	uint8_t m[9] = { 0xff, 0x00, 0xfd, 0x09, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16),
		(uint8_t)(addr >> 24), 0xfc }, a[13];

	for (int t = 0; t < 3; t++) {
		if (bri_send(m, sizeof(m)) != 0) {
			return -1;
		}
		while (bri_wait(0x09, 0.5, a)) {
			if (le32(a + 4) == addr) {
				*val = le32(a + 8);
				return 0;
			}
		}
	}
	return -1;
}

/* bri: write `size` bytes at `addr` (`fe <words-1> fd 4c <addr> <data> fc`, a last partial word rewritten whole) and
 * check them (`fe 00 fd 4a <addr> <u16 size> fc`, answered with the address, the byte sum and the CRC-8) */
static int
bri_block(const uint8_t *d, uint32_t addr, unsigned size)
{
	static uint8_t m[8 + 4096 + 32];
	unsigned body = size & ~3u, tail = size & 3u, sum = 0;
	uint8_t crc = crc8_4d(d, size), a[13];

	if (size < 4 || size > 4096) {
		return -1;
	}
	for (unsigned i = 0; i < size; i++) {
		sum += d[i];
	}
	for (int t = 0; t < 3; t++) {
		unsigned k = 0;
		uint32_t at = addr + size - 4;

		m[k++] = 0xfe;
		m[k++] = (uint8_t)(body / 4 - 1);
		m[k++] = 0xfd;
		m[k++] = 0x4c;
		for (int i = 0; i < 4; i++) {
			m[k++] = (uint8_t)(addr >> (8 * i));
		}
		memcpy(m + k, d, body);
		k += body;
		m[k++] = 0xfc;
		if (tail) {
			m[k++] = 0xfe;
			m[k++] = 0x00;
			m[k++] = 0xfd;
			m[k++] = 0x4c;
			for (int i = 0; i < 4; i++) {
				m[k++] = (uint8_t)(at >> (8 * i));
			}
			memcpy(m + k, d + size - 4, 4);
			k += 4;
			m[k++] = 0xfc;
		}
		m[k++] = 0xfe;
		m[k++] = 0x00;
		m[k++] = 0xfd;
		m[k++] = 0x4a;
		for (int i = 0; i < 4; i++) {
			m[k++] = (uint8_t)(addr >> (8 * i));
		}
		m[k++] = (uint8_t)size;
		m[k++] = (uint8_t)(size >> 8);
		m[k++] = 0xfc;
		if (bri_send(m, k) != 0) {
			return -1;
		}
		while (bri_wait(0x0a, 2.0, a)) {
			uint32_t ra = le32(a + 4), rs = a[8] | a[9] << 8 | a[10] << 16;

			if (ra != addr) {
				printf("  block %05x: a check answer for %05x, ignored\n", addr, ra);
				continue;
			}
			if (rs == (sum & 0xffffff) && a[11] == crc) {
				return 0;
			}
			printf("  block %05x (%u bytes): sum %06x crc %02x, expected %06x %02x\n", addr, size, rs, a[11],
			    sum & 0xffffff, crc);
			break;
		}
		if (t < 2) {
			printf("  block %05x: no good check answer, try %d\n", addr, t + 2);
		}
	}
	return -1;
}

/*
 * gnsstool esw [-r] [-n] [-2] [-x] [-l SECS] [FILE@ADDR ...]: PASSTHROUGH, then gpsd's boot ROM sequence: autobaud,
 * flow control, the hardware version, the debug register, then each FILE written at baseband address ADDR (hex) in
 * blocks of 1020 bytes, each checked. -r: CONTROL(RESET) first. -n: no flow control set. -2: the clock doubler on for
 * the load. -x: GetOutOfLegacy at the end (start the ESW). -l: listen SECS at the end (default 3). With no FILE only the
 * handshake is done.
 */
static int
esw_main(int argc, char **argv)
{
	static const uint8_t autobaud[16] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x80, 0x80 };
	/* gpsd with hardware flow control: `ff 00 fd 0f 01 33 0e 10 fc` and the register write that goes with it; the
	 * answer echoes the setting */
	static const uint8_t hwflow[9] = { 0xff, 0x00, 0xfd, 0x0f, 0x01, 0x33, 0x0e, 0x10, 0xfc };
	static const uint8_t hwflow2[17] = { 0xfe, 0x01, 0xfd, 0x43, 0x30, 0x01, 0x00, 0xc0, 0x01, 0x00, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x80, 0xfc };
	static uint8_t img[256 * 1024];
	int reset = 0, flow = 1, clk2x = 0, out_of_legacy = 0, ok = 0;
	double listen_end = 3, t0;
	const char *seq = getenv("MEIF_SERIAL");
	uint8_t st[2], a[13];
	unsigned total = 0, blocks = 0;
	uint32_t v;
	int i;

	for (i = 2; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'r': reset = 1; break;
		case 'n': flow = 0; break;
		case '2': clk2x = 1; break;
		case 'x': out_of_legacy = 1; break;
		case 'l': listen_end = i + 1 < argc ? atof(argv[++i]) : 3; break;
		default: fprintf(stderr, "gnsstool esw: unknown option %s\n", argv[i]); return 2;
		}
	}
	meif_serial = seq ? (unsigned)atoi(seq) : 1;
	if (reset) {
		st[0] = 0;
		st[1] = 0;
		printf("CONTROL(RESET)\n");
		if (meif_send(3, st, 2) != 0) {
			return 1;
		}
		bri_wait(-1, 1.5, NULL);
	}
	st[0] = 10;
	st[1] = 0;
	printf("CONTROL(PASSTHROUGH), serial %u\n", meif_serial);
	meif_last_state = -1;
	meif_refused = 0;
	if (meif_send(3, st, 2) != 0) {
		return 1;
	}
	for (double end = now() + 3; now() < end && meif_last_state != 10 && meif_refused == 0;) {
		bri_wait(-1, 0.1, NULL);
	}
	if (meif_last_state != 10) {
		printf("no PASSTHROUGH state report (last state %d, %u refusals); going on anyway\n", meif_last_state,
		    meif_refused);
	}
	if (reset) {
		meif_serial = 1;        /* after a RESET the chip wants serial 1 next; CONTROL messages are not counted */
	}

	for (int t = 0; t < 3 && !ok; t++) {
		printf("bri: autobaud%s (try %d)\n", flow ? ", hardware flow control" : "", t + 1);
		if (bri_send(autobaud, sizeof(autobaud)) != 0) {
			return 1;
		}
		if (!flow) {
			ok = 1;
			break;
		}
		if (bri_send(hwflow, sizeof(hwflow)) != 0 || bri_send(hwflow2, sizeof(hwflow2)) != 0) {
			return 1;
		}
		if (bri_wait(0x0f, 0.6, a)) {
			printf("bri: flow control answer %02x %02x %02x %02x%s\n", a[4], a[5], a[6], a[7],
			    a[4] == 1 && a[5] == 0x33 && a[6] == 0x0e && a[7] == 0x10 ? " (set)" : " (unexpected)");
			ok = 1;
		}
	}
	if (!ok) {
		printf("bri: no flow control answer; going on\n");
	}
	if (bri_read(0xc0000000, &v) != 0) {
		printf("bri: no hardware version: the boot ROM does not answer\n");
		bri_wait(-1, listen_end, NULL);
		printf("next MEIF serial %u\n", meif_serial);
		return 1;
	}
	printf("bri: hardware version 0x%08x\n", v);
	if (bri_read(0xc0000a54, &v) == 0) {
		printf("bri: debug register 0x%08x: debugger %s\n", v, v & 4 ? "detected" : "not detected");
	} else {
		printf("bri: no answer for the debug register\n");
	}
	if (clk2x) {
		static const uint8_t on[9] = { 0xff, 0x00, 0xfd, 0x86, 0xff, 0x00, 0xaa, 0x55, 0xfc };

		bri_send(on, sizeof(on));
		printf("bri: clock doubler on: %s\n", bri_wait(0x86, 0.6, a) ? "answered" : "no answer");
	}

	/* the segments, in the order given (gpsd: 0x5fdf0, 0x78000, then the 12 bytes at 0x5fff0) */
	t0 = now();
	bri_verbose = 0;
	for (; i < argc; i++) {
		char path[512];
		const char *at = strrchr(argv[i], '@');
		uint32_t addr;
		size_t len;
		FILE *f;

		if (at == NULL || (size_t)(at - argv[i]) >= sizeof(path)) {
			fprintf(stderr, "gnsstool esw: %s is not FILE@ADDR\n", argv[i]);
			return 2;
		}
		memcpy(path, argv[i], (size_t)(at - argv[i]));
		path[at - argv[i]] = 0;
		addr = (uint32_t)strtoul(at + 1, NULL, 16);
		f = fopen(path, "rb");
		if (f == NULL) {
			perror(path);
			return 1;
		}
		len = fread(img, 1, sizeof(img), f);
		fclose(f);
		if (len < 4 || (len & 3)) {
			fprintf(stderr, "gnsstool esw: %s: %zu bytes, not whole words\n", path, len);
			return 2;
		}
		printf("esw: %s, %zu bytes at %05x\n", path, len, addr);
		fflush(stdout);
		for (size_t off = 0; off < len;) {
			size_t rem = len - off, sz = rem;

			if (rem > 1020) {
				sz = rem - 1020 < 4 ? 1016 : 1020;   /* never leave less than a word (gpsd's patch loader) */
			}
			if (bri_block(img + off, addr + (uint32_t)off, (unsigned)sz) != 0) {
				printf("esw: block at %05x failed; stopping\n", addr + (uint32_t)off);
				bri_verbose = 1;
				bri_wait(-1, listen_end, NULL);
				printf("next MEIF serial %u\n", meif_serial);
				return 1;
			}
			off += sz;
			total += (unsigned)sz;
			blocks++;
		}
	}
	bri_verbose = 1;
	if (blocks > 0) {
		printf("esw: %u bytes in %u blocks written and checked, %.1f s\n", total, blocks, now() - t0);
	}
	if (clk2x) {
		static const uint8_t off[9] = { 0xff, 0x00, 0xfd, 0x06, 0xff, 0x00, 0xaa, 0x55, 0xfc };

		bri_send(off, sizeof(off));
		printf("bri: clock doubler off: %s\n", bri_wait(0x06, 0.6, a) ? "answered" : "no answer");
	}
	if (out_of_legacy) {
		static const uint8_t gool[9] = { 0xff, 0x00, 0xfd, 0x05, 0x55, 0xaa, 0x00, 0xff, 0xfc };

		printf("bri: GetOutOfLegacy\n");
		bri_rx_bytes = 0;
		bri_send(gool, sizeof(gool));
	}
	bri_wait(-1, listen_end, NULL);
	if (out_of_legacy) {
		printf("esw: %lu bytes from the baseband after GetOutOfLegacy\n", bri_rx_bytes);
	}
	printf("next MEIF serial %u\n", meif_serial);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";

	fd = open(DEV, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(DEV);
		return 1;
	}
	if (strcmp(cmd, "listen") == 0) {
		listen_for(argc > 2 ? atof(argv[2]) : 3600 * 24);
	} else if ((strcmp(cmd, "rpc") == 0 || strcmp(cmd, "prpc") == 0) && argc > 2) {
		uint8_t q[3 + 1000];
		unsigned id = (unsigned)strtoul(argv[2], NULL, 16);
		size_t n = argc > 3 ? unhex(argv[3], q + 3, 1000) : 0;
		struct rec *r;

		q[0] = cmd[0] == 'r' ? 'R' : 'P';
		q[1] = (uint8_t)id;
		q[2] = (uint8_t)(id >> 8);
		put(q, 3 + n);
		r = answer('A', 8);
		printf("RPC 0x%x, %zu bytes: %s\n", id, n, r == NULL ? "no word from the kernel" :
		    r->flags ? (cmd[0] == 'r' ? "acknowledged" : "sent") : "NOT acknowledged");
		fflush(stdout);
		listen_for(argc > 4 ? atof(argv[4]) : 3);
	} else if (strcmp(cmd, "meif") == 0 && argc > 2) {
		static uint8_t msg[1100], q[3 + 2300];
		unsigned id = (unsigned)strtoul(argv[2], NULL, 16), n, sum = 0, k = 0;
		const char *seq = getenv("MEIF_SERIAL");
		struct rec *r;

		n = 6 + (argc > 3 ? (unsigned)unhex(argv[3], msg + 6, 1000) : 0);
		msg[0] = (uint8_t)id;
		msg[1] = (uint8_t)(id >> 8);
		msg[2] = (uint8_t)n;
		msg[3] = (uint8_t)(n >> 8);
		msg[4] = (uint8_t)(seq ? atoi(seq) : 1);
		msg[5] = 0;
		for (unsigned i = 0; i < n; i++) {
			sum += msg[i];
		}
		msg[n] = (uint8_t)sum;
		msg[n + 1] = (uint8_t)(sum >> 8);
		meif_print(msg, n + 2);
		q[0] = 'R';
		q[1] = 0x1e;
		q[2] = 0;
		k = 5;
		q[k++] = 0xa5;
		q[k++] = 0x93;
		for (unsigned i = 0; i < n + 2; i++) {
			q[k++] = msg[i];
			if (msg[i] == 0xa5) {
				q[k++] = 0xa5;
			}
		}
		q[k++] = 0xa5;
		q[k++] = 0x68;
		q[3] = (uint8_t)(k - 5);
		q[4] = (uint8_t)((k - 5) >> 8);
		put(q, k);
		r = answer('A', 8);
		printf("RPC 0x1e, %u framed bytes: %s\n", k - 5, r == NULL ? "no word from the kernel" :
		    r->flags ? "acknowledged" : "NOT acknowledged");
		fflush(stdout);
		listen_for(argc > 4 ? atof(argv[4]) : 5);
	} else if (strcmp(cmd, "pvt") == 0 && argc > 2) {
		/* RPC 0x401, GnssPvtDebugRequest: [u16 n][the text and its NUL, n bytes][u8 0]. A wrong length makes the
		 * chip assert (rpc_engine.cpp 222) and wedges the hub until a cold restart, so it is built here, not by hand */
		uint8_t q[3 + 1000];
		size_t t = strlen(argv[2]) + 1;
		struct rec *r;

		if (t > 900) {
			return 2;
		}
		q[0] = 'R';
		q[1] = 0x01;
		q[2] = 0x04;
		q[3] = (uint8_t)t;
		q[4] = (uint8_t)(t >> 8);
		memcpy(q + 5, argv[2], t);
		q[5 + t] = 0;
		put(q, 6 + t);
		r = answer('A', 8);
		printf("RPC 0x401 \"%s\": %s\n", argv[2], r == NULL ? "no word from the kernel" :
		    r->flags ? "acknowledged" : "NOT acknowledged");
		fflush(stdout);
		listen_for(argc > 3 ? atof(argv[3]) : 3);
	} else if (strcmp(cmd, "esw") == 0) {
		return esw_main(argc, argv);
	} else if (strcmp(cmd, "bri") == 0 && argc > 2) {
		static uint8_t b[4096];
		unsigned n = (unsigned)unhex(argv[2], b, sizeof(b));
		const char *seq = getenv("MEIF_SERIAL");

		meif_serial = seq ? (unsigned)atoi(seq) : 1;
		if (bri_send(b, n) != 0) {
			return 1;
		}
		printf("%u bytes for the baseband sent; next MEIF serial %u\n", n, meif_serial);
		fflush(stdout);
		bri_wait(-1, argc > 3 ? atof(argv[3]) : 3, NULL);
	} else if (strcmp(cmd, "ssp") == 0 && argc > 5) {
		uint8_t d[64];
		size_t n = argc > 6 ? unhex(argv[6], d, sizeof(d)) : 0;
		struct rec *r = ssp((uint8_t)strtoul(argv[2], NULL, 16), (uint16_t)strtoul(argv[3], NULL, 16),
		    (uint32_t)strtoul(argv[4], NULL, 16), (uint16_t)strtoul(argv[5], NULL, 16), d, n);

		if (r == NULL) {
			printf("no answer\n");
			return 1;
		}
		print_rec(r);
	} else if (strcmp(cmd, "peek") == 0 && argc > 2) {
		uint32_t a = (uint32_t)strtoul(argv[2], NULL, 16);
		uint16_t len = argc > 3 ? (uint16_t)strtoul(argv[3], NULL, 16) : 64;
		uint8_t b[2000];

		if (len > sizeof(b)) {
			len = sizeof(b);
		}
		if (peek(a, len, b) != 0) {
			printf("no answer (is shub_peek=1 set, and did the log say \"the memory reader (SSP 0xEC) added\"?)\n");
			return 1;
		}
		for (unsigned i = 0; i < len; i += 16) {
			printf("%08x:", a + i);
			for (unsigned j = i; j < i + 16 && j < len; j++) {
				printf(" %02x", b[j]);
			}
			printf("  ");
			for (unsigned j = i; j < i + 16 && j < len; j++) {
				putchar(b[j] >= 0x20 && b[j] < 0x7f ? b[j] : '.');
			}
			putchar('\n');
		}
	} else if (strcmp(cmd, "dump") == 0 && argc > 4) {
		uint32_t a = (uint32_t)strtoul(argv[2], NULL, 16), len = (uint32_t)strtoul(argv[3], NULL, 16);
		uint16_t chunk = argc > 5 ? (uint16_t)strtoul(argv[5], NULL, 0) : 256;
		FILE *f = fopen(argv[4], "wb");
		uint8_t b[2000];
		double t0 = now();

		if (f == NULL || chunk == 0 || chunk > sizeof(b)) {
			perror(argv[4]);
			return 1;
		}
		for (uint32_t at = 0; at < len; at += chunk) {
			uint16_t n = (uint16_t)(len - at < chunk ? len - at : chunk);
			int ok = -1;

			for (int t = 0; t < 3 && ok != 0; t++) {
				ok = peek(a + at, n, b);
			}
			if (ok != 0) {
				fprintf(stderr, "gnsstool: no answer at %08x; stopping (%u bytes written)\n", a + at, at);
				fclose(f);
				return 1;
			}
			fwrite(b, 1, n, f);
			if ((at / chunk) % 64 == 0) {
				fprintf(stderr, "\r%08x  %u%%  ", a + at, (unsigned)((uint64_t)at * 100 / len));
			}
		}
		fclose(f);
		fprintf(stderr, "\rgnsstool: %u bytes from %08x in %.1f s\n", len, a, now() - t0);
	} else {
		fprintf(stderr, "usage: gnsstool listen [SECONDS] | rpc|prpc ID [HEX] [SECONDS] | ssp CMD OPTS ARG LEN [HEX] |\n"
		    "                peek ADDR [LEN] | dump ADDR LEN FILE [CHUNK] | meif ID [HEX] [SECONDS] |\n"
		    "                esw [-r] [-n] [-2] [-x] [-l SECONDS] [FILE@ADDR ...] | bri HEX [SECONDS] |\n"
		    "                pvt TEXT [SECONDS]\n"
		    "                (numbers in hex, CHUNK decimal)\n");
		return 2;
	}
	return 0;
}
