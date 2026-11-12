/*
 * The Galaxy S9's sensor hub (a Broadcom BCM4775 B0, GNSS and sensor-hub MCU in one, on SPI): what travels between the
 * AP and the hub, in plain C with no kernel in it, so that tools/bcm_shub_test.c can check it on a computer. The driver
 * is drivers/input/ssp/hakios.ssp.c; devices/starlte/SENSORS.md has the whole picture.
 *
 * Four layers, the lowest first (facts read in Samsung's GPL driver for the S9, drivers/sensorhub/brcm/ and its bbdpl/,
 * Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e, and in the Galaxy Tab Active3's sensor-hub notes,
 * exynos-linux/samsung-gtactive3wifi docs/re/sensorhub.md, which describe the same chip family on the same SoC; facts
 * only, no code of either):
 *
 *   SSI      the SPI frames: [control][length lo hi][bytes] (shub_ssi_*); the driver does the transfers
 *   packets  Broadcom's transport layer inside the SSI byte stream: ESC SOP, [seq][size][flags][flag details...]
 *            [payload][CRC-8], ESC EOP, with 0xB0/0x11/0x13 escaped (shub_tl_*)
 *   RPCs     the payload is TLVs [id][len][data], id and len one byte, or two (big-endian, bit 15 set) when >= 0x80
 *            (shub_rpc_*)
 *   SSP      Samsung's sensor protocol, carried in RPC 0x20 (to the hub) and 0x21 (from it) as [u16 length][bytes]:
 *            9-byte instructions to the hub, 4-byte headers and data frames back (shub_ssp_*)
 *
 * And the hub's firmware patch (the RAM image its ROM loads, shub_patch_*): records [type][u16 length][body], type 0xF1
 * a write of [u32 address][data].
 */
#ifndef HAKIOS_BCM_SHUB_H
#define HAKIOS_BCM_SHUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- SSI: the SPI link ---------------------------------------------------------------------------------------- */

/*
 * The control byte, the first MOSI byte of every transfer (bcm_gps_spi.h): bit 6 full duplex, bit 4 two-byte lengths,
 * bit 3 flow control, bit 5 read (unused in full duplex: both directions say "write"). Samsung's driver runs full duplex,
 * two-byte lengths, flow control: 0x58 to write, 0x50 to read.
 */
#define SHUB_SSI_TX             0x58
#define SHUB_SSI_RX             0x50
#define SHUB_SSI_FRAME          2048    /* the most one transfer carries, control and lengths included (the driver's) */
#define SHUB_SSI_PAYLOAD        (SHUB_SSI_FRAME - 5)
/* the status byte (the first MISO byte): [2:0] a count, [4:3] its unit (32, 256, 2048, 16384 bytes), bit 6 an error */
static inline uint32_t
shub_ssi_room(uint8_t status)
{
	static const uint32_t unit[4] = { 32, 256, 2048, 16384 };

	return (status & 7u) * unit[(status >> 3) & 3u];
}

/* ---- the transport layer: packets ------------------------------------------------------------------------------ */

#define SHUB_ESC                0xB0
#define SHUB_ESC_SOP            0x00
#define SHUB_ESC_EOP            0x01
#define SHUB_ESC_ESC            0x03    /* a literal 0xB0 */
#define SHUB_ESC_XON            0x04    /* a literal 0x11 */
#define SHUB_ESC_XOFF           0x05    /* a literal 0x13 */

/* flag bits; each one set carries one detail byte after the flags, in the order of the bits */
#define SHUB_F_ACK              (1u << 0)       /* detail: the sequence number acknowledged */
#define SHUB_F_RELIABLE         (1u << 1)       /* detail: this packet's reliable sequence number */
#define SHUB_F_RELIABLE_ACK     (1u << 2)       /* detail: the reliable sequence number acknowledged */
#define SHUB_F_RELIABLE_NACK    (1u << 3)       /* detail: the last good reliable sequence number */
#define SHUB_F_MSG_LOST         (1u << 4)       /* detail: packets the peer lost */
#define SHUB_F_GARBAGE          (1u << 5)       /* detail: garbage bytes the peer saw */
#define SHUB_F_SIZE_EXT         (1u << 6)       /* detail: the size's bits 15:8 */
#define SHUB_F_EXTENDED         (1u << 7)       /* detail: the flags' bits 15:8 */
#define SHUB_F_INTERNAL         (1u << 8)       /* for the transport layer itself (sync); detail 0 */
#define SHUB_F_IGNORE_SEQID     (1u << 9)       /* do not count sequence gaps; detail 0 */
#define SHUB_TL_MAX             1100            /* the largest packet payload either side makes (1024 + headroom) */

/*
 * CRC-8, polynomial 0x4D, MSB first, from 0, over the unescaped bytes from `size` to the last payload byte; sent with
 * its nibbles swapped. Checked against the three sample frames in Samsung's driver (tools/bcm_shub_test.c).
 */
static inline uint8_t
shub_crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n-- > 0) {
		c ^= *p++;
		for (int b = 0; b < 8; b++) {
			c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x4D) : (uint8_t)(c << 1);
		}
	}
	return (uint8_t)((c << 4) | (c >> 4));
}

struct shub_pkt {
	uint8_t seq;
	uint16_t flags;
	uint8_t detail[16];             /* by flag bit; SIZE_EXT's and EXTENDED's are filled in by shub_tl_build */
	const uint8_t *payload;
	uint16_t size;
};

/* append one byte, escaped; false when out of room */
static inline bool
shub_tl_put(uint8_t *out, size_t room, size_t *at, uint8_t b)
{
	uint8_t e = b == SHUB_ESC ? SHUB_ESC_ESC : b == 0x11 ? SHUB_ESC_XON : b == 0x13 ? SHUB_ESC_XOFF : 0xFF;

	if (*at + (e != 0xFF ? 2 : 1) > room) {
		return false;
	}
	if (e != 0xFF) {
		out[(*at)++] = SHUB_ESC;
		out[(*at)++] = e;
	} else {
		out[(*at)++] = b;
	}
	return true;
}

/* a packet, escaped and framed, into out; its length, or 0 when it does not fit */
static inline size_t
shub_tl_build(uint8_t *out, size_t room, const struct shub_pkt *p)
{
	uint8_t raw[SHUB_TL_MAX + 24];
	uint16_t flags = p->flags;
	uint8_t detail[16];
	size_t n = 0, at = 0;

	if (p->size > SHUB_TL_MAX) {
		return 0;
	}
	memcpy(detail, p->detail, sizeof(detail));
	if (p->size > 0xFF) {
		flags |= SHUB_F_SIZE_EXT;
		detail[6] = (uint8_t)(p->size >> 8);
	}
	if (flags > 0xFF) {
		flags |= SHUB_F_EXTENDED;
		detail[7] = (uint8_t)(flags >> 8);
	}
	raw[n++] = p->seq;
	raw[n++] = (uint8_t)p->size;
	raw[n++] = (uint8_t)flags;
	for (int b = 0; b < 16; b++) {
		if (flags & (1u << b)) {
			raw[n++] = detail[b];
		}
	}
	if (p->size > 0) {
		memcpy(raw + n, p->payload, p->size);
		n += p->size;
	}
	raw[n] = shub_crc8(raw + 1, n - 1);
	n++;
	if (room < 4) {
		return 0;
	}
	out[at++] = SHUB_ESC;
	out[at++] = SHUB_ESC_SOP;
	for (size_t i = 0; i < n; i++) {
		if (!shub_tl_put(out, room, &at, raw[i])) {
			return 0;
		}
	}
	if (at + 2 > room) {
		return 0;
	}
	out[at++] = SHUB_ESC;
	out[at++] = SHUB_ESC_EOP;
	return at;
}

/* the receiver: bytes in, whole unescaped packets out */
struct shub_tl_rx {
	uint8_t buf[SHUB_TL_MAX + 32];
	size_t len;
	bool in_packet, esc;
	uint32_t garbage, overflow, bad_escape;
};

#define SHUB_TL_NONE            0       /* nothing yet */
#define SHUB_TL_PACKET          1       /* buf[0..len) is a packet, unescaped (shub_tl_parse it) */

/* one byte of the stream; SHUB_TL_PACKET when it ended a packet */
static inline int
shub_tl_byte(struct shub_tl_rx *r, uint8_t b)
{
	if (r->esc) {
		r->esc = false;
		switch (b) {
		case SHUB_ESC_SOP:
			if (r->in_packet && r->len > 0) {
				r->garbage += (uint32_t)r->len;
			}
			r->in_packet = true;
			r->len = 0;
			return SHUB_TL_NONE;
		case SHUB_ESC_EOP:
			if (!r->in_packet) {
				return SHUB_TL_NONE;
			}
			r->in_packet = false;
			return SHUB_TL_PACKET;
		case SHUB_ESC_ESC: b = SHUB_ESC; break;
		case SHUB_ESC_XON: b = 0x11; break;
		case SHUB_ESC_XOFF: b = 0x13; break;
		default:
			r->bad_escape++;
			r->in_packet = false;
			return SHUB_TL_NONE;
		}
	} else if (b == SHUB_ESC) {
		r->esc = true;
		return SHUB_TL_NONE;
	} else if (b == 0x11 || b == 0x13) {
		return SHUB_TL_NONE;    /* the UART's soft flow control, dropped by the receiver */
	}
	if (!r->in_packet) {
		r->garbage++;
		return SHUB_TL_NONE;
	}
	if (r->len >= sizeof(r->buf)) {
		r->overflow++;
		r->in_packet = false;
		return SHUB_TL_NONE;
	}
	r->buf[r->len++] = b;
	return SHUB_TL_NONE;
}

#define SHUB_TL_OK              0
#define SHUB_TL_SHORT           1       /* fewer bytes than the header and its details */
#define SHUB_TL_LENGTH          2       /* what is left is not size + 1 */
#define SHUB_TL_CRC             3

/* an unescaped packet into its parts (p->payload points into raw) */
static inline int
shub_tl_parse(const uint8_t *raw, size_t len, struct shub_pkt *p)
{
	size_t at = 3;

	memset(p, 0, sizeof(*p));
	if (len < 4) {
		return SHUB_TL_SHORT;
	}
	p->seq = raw[0];
	p->size = raw[1];
	p->flags = raw[2];
	for (int b = 0; b < 16; b++) {
		if (!(p->flags & (1u << b))) {
			continue;
		}
		if (at >= len) {
			return SHUB_TL_SHORT;
		}
		p->detail[b] = raw[at++];
		if (b == 6) {
			p->size |= (uint16_t)(p->detail[b] << 8);
		} else if (b == 7) {
			p->flags |= (uint16_t)(p->detail[b] << 8);
		}
	}
	if (len - at != (size_t)p->size + 1) {
		return SHUB_TL_LENGTH;
	}
	if (shub_crc8(raw + 1, len - 2) != raw[len - 1]) {
		return SHUB_TL_CRC;
	}
	p->payload = raw + at;
	return SHUB_TL_OK;
}

/* the transport layer's own packets (flag INTERNAL), by their first payload byte */
#define SHUB_TLI_SEQ_REQUEST    0       /* [0][n]: n counts the attempts from 1 */
#define SHUB_TLI_SEQ_RESPONSE   1       /* [1][n][last seq received][last ack][last reliable received][next seq]
                                         *  [next reliable seq][last reliable acked] */

/* ---- RPCs --------------------------------------------------------------------------------------------------------- */

#define SHUB_RPC_VERSION_REQ    0x00    /* (no data) */
#define SHUB_RPC_VERSION        0x03    /* [u32 asic][u32 rom][u32 patch level] */
#define SHUB_RPC_WRITE_MEM      0x05    /* [u32 address][u16 n][n bytes] */
#define SHUB_RPC_RESET          0x1A    /* [u8 1] */
#define SHUB_RPC_EXCEPTION      0x17
#define SHUB_RPC_EXCEPTION_TEXT 0x18
#define SHUB_RPC_LOG            0x19    /* [u16 n][text] */
#define SHUB_RPC_SENSOR_REQ     0x20    /* [u16 n][SSP bytes], to the hub */
#define SHUB_RPC_SENSOR_RSP     0x21    /* [u16 n][SSP bytes], from the hub */
#define SHUB_RPC_START_APP      0x2E    /* [u32 entry]: "start scratch app" */
#define SHUB_RPC_LOG2           0x402   /* [u16 n][text] */

/* one id or length: one byte below 0x80, else two, big-endian, bit 15 set */
static inline size_t
shub_rpc_put_num(uint8_t *out, uint16_t v)
{
	if (v < 0x80) {
		out[0] = (uint8_t)v;
		return 1;
	}
	out[0] = (uint8_t)(0x80 | (v >> 8));
	out[1] = (uint8_t)v;
	return 2;
}

static inline bool
shub_rpc_get_num(const uint8_t *p, size_t len, size_t *at, uint16_t *v)
{
	if (*at >= len) {
		return false;
	}
	if (p[*at] & 0x80) {
		if (*at + 1 >= len) {
			return false;
		}
		*v = (uint16_t)(((p[*at] & 0x7F) << 8) | p[*at + 1]);
		*at += 2;
	} else {
		*v = p[(*at)++];
	}
	return true;
}

/* one RPC [id][len][data] into out; its length */
static inline size_t
shub_rpc_put(uint8_t *out, uint16_t id, const uint8_t *data, uint16_t len)
{
	size_t n = shub_rpc_put_num(out, id);

	n += shub_rpc_put_num(out + n, len);
	if (len > 0) {
		memcpy(out + n, data, len);
	}
	return n + len;
}

/* the next RPC of a payload: 1 with id, data and length, 0 at the end, -1 when it is cut short */
static inline int
shub_rpc_next(const uint8_t *p, size_t len, size_t *at, uint16_t *id, const uint8_t **data, uint16_t *n)
{
	if (*at >= len) {
		return 0;
	}
	if (!shub_rpc_get_num(p, len, at, id) || !shub_rpc_get_num(p, len, at, n) || *at + *n > len) {
		return -1;
	}
	*data = p + *at;
	*at += *n;
	return 1;
}

static inline uint32_t
shub_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t
shub_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline void
shub_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- the firmware patch -------------------------------------------------------------------------------------------- */

/*
 * Records [type u8][len u16 LE][body]; the S9's patch (bcm_sensorhub_a1, q_os/bbd_patch_file_star.h of Samsung's kernel,
 * as the hex of its <Blob=>) is 452 writes (0xF1, [u32 address][data]), 413864 bytes in all, 0x00800000-0x0086444c,
 * started at PatchAddress 0x00800161 (a Thumb entry). The other types (0xF0 masked write, 0xF2-0xF5 checks, a service
 * start) are not in it and are refused.
 */
#define SHUB_PATCH_WRITE        0xF1
#define SHUB_PATCH_MAX_RECORD   1000

struct shub_patch_info {
	uint32_t records, bytes, lo, hi;        /* writes, their data bytes, the lowest and the end of what they cover */
};

/* the next write: 1 with address and data, 0 at the end, -1 for a record that is not a write or is cut short */
static inline int
shub_patch_next(const uint8_t *p, size_t len, size_t *at, uint32_t *addr, const uint8_t **data, uint16_t *n)
{
	uint16_t rl;

	if (*at >= len) {
		return 0;
	}
	if (*at + 3 > len) {
		return -1;
	}
	rl = shub_le16(p + *at + 1);
	if (p[*at] != SHUB_PATCH_WRITE || rl < 5 || rl > SHUB_PATCH_MAX_RECORD || *at + 3 + rl > len) {
		return -1;
	}
	*addr = shub_le32(p + *at + 3);
	*data = p + *at + 7;
	*n = (uint16_t)(rl - 4);
	*at += 3 + (size_t)rl;
	return 1;
}

/* the whole patch read through: true when every record is a write */
static inline bool
shub_patch_check(const uint8_t *p, size_t len, struct shub_patch_info *info)
{
	size_t at = 0;
	uint32_t addr;
	const uint8_t *data;
	uint16_t n;
	int r;

	memset(info, 0, sizeof(*info));
	info->lo = 0xFFFFFFFFu;
	while ((r = shub_patch_next(p, len, &at, &addr, &data, &n)) == 1) {
		info->records++;
		info->bytes += n;
		if (addr < info->lo) {
			info->lo = addr;
		}
		if (addr + n > info->hi) {
			info->hi = addr + n;
		}
	}
	return r == 0 && info->records > 0;
}

/* ---- SSP: Samsung's sensor protocol -------------------------------------------------------------------------------- */

/* the options' type, bits 1:0 (ssp.h) */
#define SSP_AP2HUB_READ         0
#define SSP_AP2HUB_WRITE        1
#define SSP_HUB2AP_WRITE        2
#define SSP_AP2HUB_RETURN       (1u << 2)       /* a write that wants a one-byte answer after it */
#define SSP_TYPE(o)             ((o) & 3u)

/* instructions (ssp.h) */
#define SSP_WHOAMI              0x0F    /* read 1: 0x55 */
#define SSP_WHOAMI_ANSWER       0x55
#define SSP_FIRMWARE_REV        0xF0    /* read 4 */
#define SSP_SENSOR_FORMATION    0xF1    /* write 3: accel position, gyro position, mag position */
#define SSP_SENSOR_SCANNING     0xF4    /* read 8: a bitmap of the sensors found, bit n = sensor n */
#define SSP_GET_ACCEL_RANGE     0xE1    /* read 1 */
#define SSP_SET_GYRO_CAL        0xCD    /* write 12: s32 x, y, z (MSG2SSP_AP_MCU_SET_GYRO_CAL) */
#define SSP_SET_ACCEL_CAL       0xCE    /* write 6: s16 x, y, z (MSG2SSP_AP_MCU_SET_ACCEL_CAL) */
#define SSP_SENSOR_ADD          0xA1    /* write 10: [id][s32 period ms][s32 max latency ms][s8 flags] */
#define SSP_SENSOR_REMOVE       0xA2    /* write 5: [id][s32 period ms] */
#define SSP_CHANGE_DELAY        0xA4    /* write 10, as ADD */
#define SSP_CURRENT_TIMESTAMP   0xA7    /* write 8: the AP's time in ns */
#define SSP_AP_WAKEUP           0xD1    /* no data */
#define SSP_AP_RESUME           0xD3

/* records of a hub -> AP data frame, by their first byte (ssp.h, ssp_data.c) */
#define SSP_REC_DEBUG           0x03    /* [u16 n][text] */
#define SSP_REC_BIG_DATA        0x04
#define SSP_REC_META            0x05    /* [what][sensor] */
#define SSP_REC_TIME_SYNC       0x06    /* none */
#define SSP_REC_GYRO_CAL        0x08    /* 12 bytes */
#define SSP_REC_SCONTEXT        0x0C    /* 72 bytes */
#define SSP_REC_SENSOR_INIT     0x0A    /* none: the hub's sensors are set up */
#define SSP_REC_SAMPLE          0x37    /* [sensor][sample][u64 time] */
#define SSP_REC_TIMESTAMP_OFF   0xA8    /* [u64] */

/*
 * the sensors (sensor_list.h) and the size of each one's sample on the S9's firmware (ssp_data.c's get_*_sensordata,
 * with CONFIG_SENSORS_SSP_LIGHT_REPORT_LUX and without _LIGHT_MAX_GAIN_2BYTE, as starlte has them; the magnetometer's
 * with the overflow byte of CONFIG_SSP_SUPPORT_MAGNETIC_OVERFLOW, which starlte's defconfig does not name but its
 * firmware sends: 8 bytes, seen in the hub's frames on the phone, 3 November)
 */
#define SSP_ACCEL               0
#define SSP_GYRO                1
#define SSP_MAG_UNCAL           2
#define SSP_MAG_RAW             3
#define SSP_MAG                 4
#define SSP_PRESSURE            5
#define SSP_PROXIMITY           7
#define SSP_LIGHT               9
#define SSP_PROXIMITY_RAW       10
#define SSP_GAME_ROTATION       16
#define SSP_ROTATION            17
#define SSP_LIGHT_CCT           31
#define SSP_ACCEL_UNCAL         33
#define SSP_SENSORS             37

static const uint8_t ssp_sample_size[SSP_SENSORS] = {
	[0] = 6,        /* accelerometer: s16 x, y, z */
	[1] = 12,       /* gyroscope: s32 x, y, z */
	[2] = 13,       /* magnetometer, uncalibrated: s16 x, y, z, offsets x, y, z, u8 overflow */
	[3] = 6,        /* magnetometer, raw */
	[4] = 8,        /* magnetometer: s16 x, y, z, u8 accuracy, u8 overflow (seen on the phone: 8 bytes) */
	[5] = 6,        /* pressure: s32 (hundredths of hPa... see SENSORS.md), s16 temperature */
	[6] = 20,       /* gesture */
	[7] = 3,        /* proximity: u8 near, u16 adc */
	[8] = 5,        /* temperature and humidity */
	[9] = 18,       /* light: u32 lux, s32 cct, u16 r, g, b, w, u8 a_time, a_gain */
	[10] = 2,       /* proximity raw */
	[13] = 1,       /* step detector */
	[14] = 1,       /* significant motion */
	[15] = 24,      /* gyroscope uncalibrated */
	[16] = 17,      /* game rotation vector */
	[17] = 17,      /* rotation vector */
	[18] = 4,       /* step counter */
	[24] = 12,      /* light IR */
	[25] = 12,      /* interrupt gyro */
	[26] = 1,       /* tilt */
	[27] = 1,       /* pick-up */
	[29] = 3,       /* proximity alert */
	[30] = 2,       /* light flicker */
	[31] = 18,      /* light CCT */
	[33] = 12,      /* accelerometer uncalibrated */
	[35] = 3,       /* proximity pocket */
	[36] = 1,       /* wake-up motion */
};

/* a 9-byte instruction header (ssp.h, struct ssp_msg's first 9 bytes, packed, little-endian) */
static inline void
shub_ssp_header(uint8_t out[9], uint8_t cmd, uint16_t length, uint16_t options, uint32_t data)
{
	out[0] = cmd;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	out[3] = (uint8_t)options;
	out[4] = (uint8_t)(options >> 8);
	shub_put32(out + 5, data);
}

struct shub_sample {
	uint8_t sensor;
	const uint8_t *data;
	uint8_t size;
	uint64_t time;
};

#define SSP_FRAME_OK            0
#define SSP_FRAME_UNKNOWN       1       /* a record the parser does not know: the rest of the frame is lost */
#define SSP_FRAME_SHORT         2

struct shub_frame_counts {
	uint32_t samples, init_done, meta, debug, other;
	size_t stopped_at;              /* where the record that stopped the frame starts */
};

/*
 * A HUB2AP data frame, record by record; each sample to `sample(ctx, s)`, the hub's debug text to `text(ctx, p, n)`.
 * SSP_FRAME_OK, or why it stopped (Samsung's parser stops at an unknown record too).
 */
static inline int
shub_ssp_frame(const uint8_t *p, size_t len, struct shub_frame_counts *c, void *ctx,
    void (*sample)(void *, const struct shub_sample *), void (*text)(void *, const uint8_t *, size_t))
{
	size_t at = 0;

	while (at < len) {
		uint8_t rec;

		c->stopped_at = at;
		rec = p[at++];

		switch (rec) {
		case SSP_REC_SENSOR_INIT:
			c->init_done++;
			break;
		case SSP_REC_TIME_SYNC:
			c->other++;
			break;
		case SSP_REC_TIMESTAMP_OFF:
			if (at + 8 > len) {
				return SSP_FRAME_SHORT;
			}
			at += 8;
			c->other++;
			break;
		case SSP_REC_META:
			if (at + 2 > len) {
				return SSP_FRAME_SHORT;
			}
			at += 2;
			c->meta++;
			break;
		case SSP_REC_SCONTEXT:
			if (at + 72 > len) {
				return SSP_FRAME_SHORT;
			}
			at += 72;
			c->other++;
			break;
		case SSP_REC_GYRO_CAL:
			if (at + 12 > len) {
				return SSP_FRAME_SHORT;
			}
			at += 12;
			c->other++;
			break;
		case SSP_REC_DEBUG: {
			/* print_mcu_debug(): [u16 length][text] */
			size_t n;

			if (at + 2 > len) {
				return SSP_FRAME_SHORT;
			}
			n = shub_le16(p + at);
			at += 2;
			if (at + n > len) {
				return SSP_FRAME_SHORT;
			}
			if (text != NULL) {
				text(ctx, p + at, n);
			}
			at += n;
			c->debug++;
			break;
		}
		case SSP_REC_SAMPLE: {
			struct shub_sample s;
			uint64_t t = 0;

			if (at >= len) {
				return SSP_FRAME_SHORT;
			}
			s.sensor = p[at++];
			if (s.sensor >= SSP_SENSORS || ssp_sample_size[s.sensor] == 0) {
				return SSP_FRAME_UNKNOWN;
			}
			s.size = ssp_sample_size[s.sensor];
			if (at + s.size + 8 > len) {
				return SSP_FRAME_SHORT;
			}
			s.data = p + at;
			at += s.size;
			for (int i = 7; i >= 0; i--) {
				t = t << 8 | p[at + (size_t)i];
			}
			at += 8;
			s.time = t;
			c->samples++;
			if (sample != NULL) {
				sample(ctx, &s);
			}
			break;
		}
		default:
			return SSP_FRAME_UNKNOWN;
		}
	}
	return SSP_FRAME_OK;
}

/*
 * The hub's side of the SSP byte stream, put back together from RPC 0x21's: [u16 options][u16 length] then, for a READ
 * answer, the length the AP asked for, and for a HUB2AP_WRITE the header's length; a WRITE answer is the header alone.
 */
struct shub_ssp_rx {
	uint8_t buf[2048];
	size_t len;
	uint16_t want_read;     /* the length of the READ the AP has outstanding (0: none) */
	uint32_t dropped;
};

#define SSP_RX_NONE             0
#define SSP_RX_READ             1       /* an answer to a READ: [opts][len] then want_read bytes */
#define SSP_RX_WRITE            2       /* a WRITE's go-ahead: the header alone */
#define SSP_RX_DATA             3       /* a HUB2AP data frame */

/*
 * What the buffer starts with: SSP_RX_* with its options, its data and their length, and how many bytes it takes up
 * (to shub_ssp_rx_consume), or SSP_RX_NONE until it is all there.
 */
static inline int
shub_ssp_rx_peek(const struct shub_ssp_rx *r, uint16_t *options, const uint8_t **data, size_t *n, size_t *used)
{
	uint16_t len;

	if (r->len < 4) {
		return SSP_RX_NONE;
	}
	*options = shub_le16(r->buf);
	len = shub_le16(r->buf + 2);
	switch (SSP_TYPE(*options)) {
	case SSP_AP2HUB_READ:
		*n = r->want_read;
		break;
	case SSP_HUB2AP_WRITE:
		*n = len;
		break;
	default:
		*n = 0;
		break;
	}
	if (r->len < 4 + *n) {
		return SSP_RX_NONE;
	}
	*data = r->buf + 4;
	*used = 4 + *n;
	return SSP_TYPE(*options) == SSP_AP2HUB_READ ? SSP_RX_READ :
	       SSP_TYPE(*options) == SSP_HUB2AP_WRITE ? SSP_RX_DATA : SSP_RX_WRITE;
}

static inline void
shub_ssp_rx_consume(struct shub_ssp_rx *r, size_t used)
{
	if (used >= r->len) {
		r->len = 0;
		return;
	}
	memmove(r->buf, r->buf + used, r->len - used);
	r->len -= used;
}

static inline void
shub_ssp_rx_add(struct shub_ssp_rx *r, const uint8_t *p, size_t n)
{
	if (n > sizeof(r->buf) - r->len) {
		r->dropped += (uint32_t)(r->len + n);
		r->len = 0;     /* lost the thread: start again at the next header */
		if (n > sizeof(r->buf)) {
			return;
		}
	}
	memcpy(r->buf + r->len, p, n);
	r->len += n;
}

#endif /* HAKIOS_BCM_SHUB_H */
