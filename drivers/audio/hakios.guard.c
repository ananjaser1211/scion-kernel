/*
 * The speakers' guard: what the samples go through on their way to the phone's two amplifiers
 * (drivers/audio/hakios.sound.c's feeder), in place of the protection Android has in the audio DSP.
 *
 * Under Android the DSP runs Awinic's algorithm (SKTune, "aw882xx_rx_switch") with each
 * speaker's calibration: it keeps the cone's excursion and the voice coil's temperature in
 * bounds, measuring both through the amplifiers' current and voltage feedback. None of that runs
 * here, and nothing here knows the speakers' parameters (Android's files that can be read have
 * none: the amplifiers' register profile, the battery tables of its "monitor", and in the device
 * tree a range of 4 to 9 ohms for the calibrated resistance). So the guard holds what reaches the
 * speakers to levels that are safe for small phone speakers whatever they are exactly, measured
 * the only way there is without feedback -- from the samples and the amplifiers' level -- and
 * kept well under what the speakers can take at Android's level:
 *
 *   high-pass  a second-order Butterworth filter at 200 Hz (sound_hpf: 150 or 120): a phone's speaker gives out next to
 *              nothing below its resonance (several hundred Hz), and what it is given there only
 *              moves the cone further and heats the coil
 *   power      the power that would reach a speaker, averaged three ways -- over 0.2 s (the
 *              cone and coil at once), 3 s (the voice coil's warming) and 60 s (the magnet and
 *              the frame) -- and the level turned down so that none of the three averages goes
 *              over its limit: 3, 6 and 9 dB under the power of a full-scale sine at Android's
 *              level. Sound of ordinary loudness never comes near them; a sustained full-scale
 *              tone is turned down at once to the first, after seconds to the second and over a
 *              minute to the third, and the level comes back up as the averages fall, as a
 *              speaker cools. The averages go on falling while the amplifiers are off.
 *   peaks      no sample above 3 dB under full scale at Android's level: a limiter that turns
 *              the level down at the sample that would go over (so none does) and lets it come
 *              back with a time constant of 43 ms
 *   monitor    a further reduction that the amplifiers' watch asks for (hakios.speaker.c:
 *              Android's table for a cold amplifier, and a low battery)
 *
 * Before all that, a gain (sound_gain, at most 12 dB): Darwin's own processing for the iPhone X's
 * speaker keeps even a full-scale tone at its top volume 12.6 to 14.3 dB under full scale, so
 * without it Darwin at the top would stay that much under Android at the top. What it lifts over
 * full scale is held to full scale by the peak limiter (or to its lower limit), never clipped.
 *
 * All the limits are in terms of the speakers: a signal at full scale with the amplifiers at
 * Android's level (the loudest they are ever set to) is 0 dB. With the amplifiers turned down
 * (sound_below) the same samples are that much quieter at the speakers, and the guard holds them
 * back only that much later. Everything is integer arithmetic (the kernel's threads do no
 * floating point); fractions are in Q30 unless said otherwise.
 *
 * One caller at a time (the feeder); hakios_guard_attenuation() and hakios_guard_monitor() may
 * be called from another thread.
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.audio.h>

#define ONE                     (1ll << 30)
#define FULL_SCALE              32768

/* The limits, in eighths of a dB under full scale at Android's level (powers: under a full-scale square wave's) */
#define PEAK_LIMIT              (3 * 8)
#define RMS_LIMIT               (6 * 8)         /* a full-scale sine is 3 dB under a square wave: these are 3, 6, 9 dB under it */
#define FAST_LIMIT              (9 * 8)
#define SLOW_LIMIT              (12 * 8)
/* ... and the averages' time constants, in frames (48 kHz) */
#define RMS_FRAMES              9600            /* 0.2 s */
#define FAST_FRAMES             144000          /* 3 s */
#define SLOW_FRAMES             2880000         /* 60 s */
#define RELEASE_SHIFT           11              /* the peak limiter lets go with a time constant of 2048 frames, 43 ms */
#define MAX_FRAMES              1024            /* the most frames processed at once (the feeder's period: 960) */

/* 10^(-n/160), n = 0..47, in Q30: an attenuation of n eighths of a dB, as a factor of amplitude */
static const uint32_t eighths[48] = {
	1073741824, 1058400094, 1043277569, 1028371116, 1013677647, 999194121, 984917536, 970844937, 956973408, 943300077,
	929822112, 916536722, 903441154, 890532697, 877808678, 865266461, 852903448, 840717079, 828704830, 816864213,
	805192776, 793688102, 782347808, 771169545, 760150998, 749289885, 738583957, 728030996, 717628817, 707375265,
	697268218, 687305580, 677485290, 667805313, 658263644, 648858308, 639587356, 630448869, 621440953, 612561743,
	603809400, 595182112, 586678091, 578295576, 570032831, 561888146, 553859832, 545946228,
};
#define SIX_DB                  538145694       /* 10^(-6/20) in Q30 */

/* the high-pass filter at 48 kHz, Q = 1/sqrt(2): b0, b1, b2, a1, a2 in Q30 (RBJ's cookbook); 200 Hz unless sound_hpf says
 * 150 or 120 (hakios_guard_tone): lower lets more of the speakers' lowest notes through, and the power limits below see it */
static const struct { uint32_t hz; int64_t k[5]; } hpfs[] = {
	{ 120, { 1061881538, -2123763076, 1061881538, -2123632067, 1050152262 } },
	{ 150, { 1058936991, -2117873982, 1058936991, -2117669842, 1044336297 } },
	{ 200, { 1054047540, -2108095079, 1054047540, -2107733822, 1034714513 } },
};
/* a low shelf at 250 Hz, slope 1, +0 to +6 dB (sound_bass): what a phone's speakers give out least of but can still give,
 * lifted before the power limits and the peak limiter, so that they hold it as they hold everything else */
static const int64_t shelves[7][5] = {
	{ 1073741824, -2097799412, 1025181377, -2097799412, 1025181377 },  /* +0 dB */
	{ 1075173001, -2099143588, 1025161746, -2099208356, 1026528155 },  /* +1 dB */
	{ 1076607272, -2100447549, 1025102823, -2100577384, 1027838435 },  /* +2 dB */
	{ 1078045831, -2101712210, 1025004564, -2101907621, 1029113160 },  /* +3 dB */
	{ 1079489883, -2102938453, 1024866902, -2103200165, 1030353248 },  /* +4 dB */
	{ 1080940640, -2104127126, 1024689739, -2104456082, 1031559599 },  /* +5 dB */
	{ 1082399326, -2105279044, 1024472949, -2105676406, 1032733089 },  /* +6 dB */
};
static const int64_t *hpf_k = hpfs[2].k;      /* 200 Hz */
static const int64_t *shelf_k = shelves[0];    /* the shelf's coefficients, when shelf_on */
static bool shelf_on;

static struct {
	bool hpf;
	uint64_t gain;                          /* the gain before everything, Q30 (1 to 4) */
	volatile uint32_t attenuation;          /* the amplifiers' level under Android's, eighths of a dB; HAKIOS_GUARD_SILENT: off */
	volatile uint32_t monitor;              /* the monitor's reduction, eighths of a dB */
	int64_t x1[2], x2[2], y1[2], y2[2];     /* the filter's past, in 1/256 of a sample */
	int64_t s1[2], s2[2], t1[2], t2[2];     /* the shelf's past (input, output), likewise */
	uint64_t rms[2], fast[2], slow[2];      /* average powers at each speaker, Q30 of a full-scale square wave at Android's level */
	uint64_t level;                         /* the power guard's and the monitor's gain at the end of the last block */
	uint64_t envelope;                      /* the peak limiter's, 1/65536 of a sample */
	struct hakios_guard_report report;
} guard = { .hpf = true, .attenuation = HAKIOS_GUARD_SILENT, .gain = ONE, .level = ONE, .report = { .least_gain = ONE, .least_peak_gain = 65536 } };

static int32_t work[2 * MAX_FRAMES];

/* 10^(-n/160) in Q30: n eighths of a dB under, as a factor of amplitude */
static uint64_t
amplitude(uint32_t n)
{
	uint64_t value = eighths[n % 48];

	for (uint32_t i = 0; i < n / 48 && value != 0; i++) {
		value = value * SIX_DB >> 30;
	}
	return value;
}

static uint64_t
isqrt(uint64_t value)
{
	uint64_t root = 0, bit = 1ull << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

/* how much of `limit' a power is, as the square of the gain that would bring it down to it (Q30, at most 1) */
static uint64_t
headroom(uint64_t power, uint64_t limit)
{
	if (power <= limit) {
		return ONE;
	}
	return (limit << 30) / power;
}

void
hakios_guard_configure(bool hpf, uint32_t gain)
{
	guard.hpf = hpf;
	/* 10^(gain/160): one over the attenuation by as much */
	guard.gain = (1ull << 60) / amplitude(gain > HAKIOS_GUARD_MAX_GAIN ? HAKIOS_GUARD_MAX_GAIN : gain);
}

/* the high-pass filter's corner (120, 150 or 200 Hz: the nearest at or above `hpf_hz') and the low shelf (0 to 6 dB); before
 * the first block, from the feeder's thread (hakios.sound.c) */
void
hakios_guard_tone(uint32_t hpf_hz, uint32_t bass_db)
{
	unsigned i = 0;

	while (i + 1 < sizeof(hpfs) / sizeof(hpfs[0]) && hpfs[i].hz < hpf_hz) {
		i++;
	}
	hpf_k = hpfs[i].k;
	shelf_k = shelves[bass_db > 6 ? 6 : bass_db];
	shelf_on = bass_db != 0;
	for (unsigned c = 0; c < 2; c++) {
		guard.x1[c] = guard.x2[c] = guard.y1[c] = guard.y2[c] = 0;
		guard.s1[c] = guard.s2[c] = guard.t1[c] = guard.t2[c] = 0;
	}
}

uint32_t
hakios_guard_hpf_hz(void)
{
	for (unsigned i = 0; i < sizeof(hpfs) / sizeof(hpfs[0]); i++) {
		if (hpfs[i].k == hpf_k) {
			return hpfs[i].hz;
		}
	}
	return 0;
}

void
hakios_guard_attenuation(uint32_t below)
{
	guard.attenuation = below;
}

void
hakios_guard_monitor(uint32_t below)
{
	guard.monitor = below;
}

static void
block(int16_t *frames, uint32_t count)
{
	uint32_t attenuation = guard.attenuation;
	struct hakios_guard_report *r = &guard.report;
	/* (2 * attenuation is kept from wrapping in 32 bits: a wrapped value says nothing about the level, and for some it is
	   a huge one, which makes the power limits see no power at all while the amplifiers are not turned down) */
	uint64_t to_speaker = attenuation == HAKIOS_GUARD_SILENT ? 0 :
	    amplitude(attenuation > HAKIOS_GUARD_SILENT / 2 ? HAKIOS_GUARD_SILENT - 1 : 2 * attenuation);
	uint64_t sum[2] = { 0, 0 }, g2 = ONE, g, start = guard.level, ceiling;
	uint32_t binding = 0;

	/* the high-pass filter, and what comes out of it: its power, per channel */
	for (uint32_t i = 0; i < 2 * count; i++) {
		unsigned c = i & 1;
		int64_t x = (int64_t)frames[i] * (int64_t)guard.gain >> 22, y;    /* (the gain, and 1/256 of a sample) */
		uint32_t level = frames[i] < 0 ? (uint32_t)-frames[i] : (uint32_t)frames[i];

		if (level > r->peak_in) {
			r->peak_in = level;
		}
		if (guard.hpf) {
			y = (hpf_k[0] * x + hpf_k[1] * guard.x1[c] + hpf_k[2] * guard.x2[c] - hpf_k[3] * guard.y1[c] - hpf_k[4] * guard.y2[c]) >> 30;
			guard.x2[c] = guard.x1[c];
			guard.x1[c] = x;
			guard.y2[c] = guard.y1[c];
			guard.y1[c] = y;
		} else {
			y = x;
		}
		if (shelf_on) {
			int64_t z = (shelf_k[0] * y + shelf_k[1] * guard.s1[c] + shelf_k[2] * guard.s2[c] - shelf_k[3] * guard.t1[c] -
			    shelf_k[4] * guard.t2[c]) >> 30;

			guard.s2[c] = guard.s1[c];
			guard.s1[c] = y;
			guard.t2[c] = guard.t1[c];
			guard.t1[c] = z;
			y = z;
		}
		work[i] = (int32_t)((y + 128) >> 8);
		sum[c] += (uint64_t)((int64_t)work[i] * work[i]);
	}

	/* the averages at the speakers, and the gain that keeps all three under their limits */
	for (unsigned c = 0; c < 2; c++) {
		/* the block's power, Q30 of full scale (a square wave at 32768), then as it reaches the speaker */
		uint64_t power = (uint64_t)((unsigned __int128)(sum[c] / count) * to_speaker >> 30), h;

		guard.rms[c] = (uint64_t)((int64_t)guard.rms[c] + ((int64_t)power - (int64_t)guard.rms[c]) * count / RMS_FRAMES);
		guard.fast[c] = (uint64_t)((int64_t)guard.fast[c] + ((int64_t)power - (int64_t)guard.fast[c]) * count / FAST_FRAMES);
		guard.slow[c] = (uint64_t)((int64_t)guard.slow[c] + ((int64_t)power - (int64_t)guard.slow[c]) * count / SLOW_FRAMES);
		if ((h = headroom(guard.rms[c], amplitude(2 * RMS_LIMIT))) < g2) {
			g2 = h;
			binding = 1;
		}
		if ((h = headroom(guard.fast[c], amplitude(2 * FAST_LIMIT))) < g2) {
			g2 = h;
			binding = 2;
		}
		if ((h = headroom(guard.slow[c], amplitude(2 * SLOW_LIMIT))) < g2) {
			g2 = h;
			binding = 3;
		}
	}
	g = isqrt(g2 << 30) * amplitude(guard.monitor) >> 30;
	if (g > ONE) {
		g = ONE;
	}
	r->blocks++;
	if (binding == 1) {
		r->rms_blocks++;
	} else if (binding == 2) {
		r->fast_blocks++;
	} else if (binding == 3) {
		r->slow_blocks++;
	}
	if (g < r->least_gain) {
		r->least_gain = g;
	}

	/* the peaks: no sample over the limit at the speaker (and none over full scale) */
	ceiling = FULL_SCALE - 1;
	if (attenuation != HAKIOS_GUARD_SILENT && attenuation < PEAK_LIMIT) {
		ceiling = (uint64_t)FULL_SCALE * amplitude(PEAK_LIMIT - attenuation) >> 30;
	}
	for (uint32_t f = 0; f < count; f++) {
		/* (the power guard's gain moves from the last block's to this one's across the block) */
		uint64_t gain = (uint64_t)((int64_t)start + ((int64_t)g - (int64_t)start) * (int64_t)(f + 1) / (int64_t)count);
		int64_t y[2];
		uint64_t level, peak_gain = 65536;

		for (unsigned c = 0; c < 2; c++) {
			y[c] = (int64_t)work[2 * f + c] * (int64_t)gain >> 30;
		}
		level = (uint64_t)(y[0] < 0 ? -y[0] : y[0]);
		if ((uint64_t)(y[1] < 0 ? -y[1] : y[1]) > level) {
			level = (uint64_t)(y[1] < 0 ? -y[1] : y[1]);
		}
		guard.envelope -= guard.envelope >> RELEASE_SHIFT;
		if (level << 16 > guard.envelope) {
			guard.envelope = level << 16;
		}
		if (guard.envelope > ceiling << 16) {
			peak_gain = (ceiling << 32) / guard.envelope;
			r->limited_frames++;
			if (peak_gain < r->least_peak_gain) {
				r->least_peak_gain = (uint32_t)peak_gain;
			}
		}
		for (unsigned c = 0; c < 2; c++) {
			int64_t out = y[c] * (int64_t)peak_gain >> 16;
			uint32_t magnitude;

			out = out > FULL_SCALE - 1 ? FULL_SCALE - 1 : out < -FULL_SCALE ? -FULL_SCALE : out;
			frames[2 * f + c] = (int16_t)out;
			magnitude = (uint32_t)(out < 0 ? -out : out);
			if (magnitude > r->peak_out) {
				r->peak_out = magnitude;
			}
		}
	}
	guard.level = g;
	r->gain = g;
	r->rms = guard.rms[0] > guard.rms[1] ? guard.rms[0] : guard.rms[1];
	r->fast = guard.fast[0] > guard.fast[1] ? guard.fast[0] : guard.fast[1];
	r->slow = guard.slow[0] > guard.slow[1] ? guard.slow[0] : guard.slow[1];
}

void
hakios_guard_process(int16_t *frames, uint32_t count)
{
	while (count > 0) {
		uint32_t n = count < MAX_FRAMES ? count : MAX_FRAMES;

		block(frames, n);
		frames += 2 * n;
		count -= n;
	}
}

/* What has happened since the last call (and the averages and the gain as they are now). */
void
hakios_guard_take_report(struct hakios_guard_report *report)
{
	*report = guard.report;
	guard.report = (struct hakios_guard_report){ .least_gain = ONE, .least_peak_gain = 65536 };
}

/* 10 log10 of a Q30 ratio, in tenths of a dB (for what is said) */
int
hakios_guard_decibels(uint64_t ratio)
{
	static const uint16_t fraction[17] = {
		0, 5732, 11136, 16248, 21098, 25711, 30109, 34312, 38336, 42196, 45904, 49472, 52911, 56229, 59434, 62534, 65535,
	};
	int64_t log2;                           /* Q16 */
	unsigned top = 63;

	if (ratio == 0) {
		return -999;
	}
	while (!(ratio >> top)) {
		top--;
	}
	{
		/* the 4 bits under the top one choose a segment, the next 12 where in it */
		uint64_t mantissa = top >= 16 ? ratio >> (top - 16) : ratio << (16 - top);
		unsigned index = (unsigned)(mantissa >> 12) & 15, within = (unsigned)mantissa & 4095;

		log2 = ((int64_t)top - 30) * 65536 + fraction[index] + (int64_t)(fraction[index + 1] - fraction[index]) * within / 4096;
	}
	return (int)(log2 * 30103 / 65536 / 1000);
}
