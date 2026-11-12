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
	if (id <= 2 || id == 4 || id == 8) {
		printf(" :");
		for (unsigned i = 6; i + 2 < n && i < (id == 8 || id == 2 ? 400u : 40u); i++) {
			printf(" %02x", m[i]);
		}
	}
	putchar('\n');
}

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
				meif_print(m, k);
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
		printf(" \"");
		for (unsigned i = 2; i < r->len; i++) {
			putchar(r->data[i] >= 0x20 ? r->data[i] : r->data[i] == 0 ? ' ' : '.');
		}
		printf("\"\n");
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
		    "                peek ADDR [LEN] | dump ADDR LEN FILE [CHUNK] | meif ID [HEX] [SECONDS]\n"
		    "                (numbers in hex, CHUNK decimal)\n");
		return 2;
	}
	return 0;
}
