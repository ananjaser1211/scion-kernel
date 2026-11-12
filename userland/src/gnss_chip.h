/*
 * gnss_chip: the BCM4775's GNSS side through the kernel's /dev/hakios-gnss (drivers/input/ssp/hakios.ssp.c), the
 * sequence docs/gnss.md found: the AR table cut, the external LNA's pin mux, GnssPvtDataStart, the ESW through the measurement engine's
 * PASSTHROUGH and the baseband's boot ROM (bri, every block checked), RESET, esw_ctrl (retain), IDLE, ON, then the
 * on-chip engine's requests (disable_indoor, gnss_start; gnss_stop). The same steps gnsstool takes one at a time.
 *
 * Records from the kernel: [u8 kind][u8 flags][u16 id][u16 len][len bytes], kinds 'R' (an RPC from the hub), 'A' (a
 * raw RPC sent; flags 1: acknowledged), 'S' (an SSP answer). MEIF: [u16 id][u16 len][u16 serial][payload][u16 sum],
 * framed A5 93 ... A5 68 (A5 doubled) in RPC 0x1E to the chip and 0x1F back.
 *
 * Not thread-safe: one thread owns the device. Nothing here prints; what it has to say goes to the log callback, and
 * the hub's text goes to the text callback as it is (a caller that logs it must mask positions: gc_mask_position()).
 */
#ifndef GNSS_CHIP_H
#define GNSS_CHIP_H
#include <stddef.h>
#include <stdint.h>

struct gc_segment {
	const char *path;       /* the piece of the ESW */
	uint32_t addr;          /* its baseband address */
};

struct gc_events {
	void (*text)(const char *line, void *ctx);                      /* a hub log line (RPC 0x19), unmasked */
	void (*state)(unsigned from, unsigned to, void *ctx);           /* MEIF STATE_REPORT */
	void (*meif)(const uint8_t *msg, unsigned len, void *ctx);      /* any other MEIF message from the chip */
	void (*chip_assert)(const char *file, unsigned line, void *ctx);/* RPC 0x18: the chip asserted */
	void (*log)(const char *line, void *ctx);                       /* what the library has to say */
	void (*step)(const char *name, void *ctx);                      /* gc_engine_on()'s steps, as each starts */
	void *ctx;
};

/* MEIF states (STATE_REPORT, CONTROL) */
enum { GC_RESET = 0, GC_SETUP = 1, GC_OFF = 2, GC_IDLE = 4, GC_ON = 5, GC_ERROR = 9, GC_PASSTHROUGH = 10 };

int gc_open(const struct gc_events *ev);        /* 0, or -1 (errno) */
void gc_close(void);
int gc_poll(double seconds);                    /* take and hand on records for up to `seconds`; -1 when the device failed */
int gc_rpc(unsigned id, const uint8_t *data, size_t n);         /* a reliable RPC; 0 when acknowledged */
int gc_pvt(const char *text);                   /* RPC 0x401 GnssPvtDebugRequest, built right ([u16 n][text NUL][u8 0]) */
int gc_log_facility(unsigned facility);         /* RPC 0x2B: one hub log facility on (few, 2 s apart: docs/gnss.md) */
int gc_meif(unsigned id, const uint8_t *payload, unsigned n);   /* one MEIF message; 0 when every RPC was acknowledged */
void gc_meif_serial(unsigned next);             /* the serial of the next MEIF message */
int gc_wait_state(unsigned state, double seconds);              /* 0 when a STATE_REPORT to `state` came */
int gc_esw_load(const struct gc_segment *seg, int n);           /* RESET, PASSTHROUGH, bri, checked blocks, GetOutOfLegacy */
int gc_esw_blocks(void);                        /* the blocks the last gc_esw_load() wrote and checked; -1 none */
int gc_peek(uint32_t addr, uint16_t len, uint8_t *out);         /* the hub's memory (SSP 0xEC, boot-arg shub_peek=1) */
int gc_ar_table_cut(void);                      /* RPC 0x05: 0xffff over the MiniMe's AR table; 1 cut, 0 skipped, -1 */
int gc_engine_on(void);                         /* RESET, esw_ctrl (retain), CONFIG_SET (no EXT_OSC), EXT_LNA, IDLE, ON */
int gc_last_state(void);                        /* the last STATE_REPORT's new state, -1 none */
void gc_mask_position(char *text);              /* masks what could be a position in a line of the chip's text */

#endif
