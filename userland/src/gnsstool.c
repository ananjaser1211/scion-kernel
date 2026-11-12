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

static void
print_rec(const struct rec *r)
{
	int text = r->len > 2;

	for (unsigned i = 2; i < r->len && text; i++) {
		uint8_t c = r->data[i];

		text = (c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t' || c == 0;
	}
	printf("%.3f %c%s id 0x%x len %u:", now(), r->kind, r->flags ? "+" : "", r->id, r->len);
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
		    "                peek ADDR [LEN] | dump ADDR LEN FILE [CHUNK]   (numbers in hex, CHUNK decimal)\n");
		return 2;
	}
	return 0;
}
