/*
 * The Galaxy S9 panel's gamma (drivers/display/panel/hakios.s6e3ha8_gamma.h) on this machine: its tables checked against what
 * they must be, and the gamma worked out for made-up MTPs (the panel's calibration, register C8) and checked for sense.
 *
 *   cc -O2 -Wall -Wextra tools/s6e3ha8_gamma_test.c -lm -o s6e3ha8_gamma_test && ./s6e3ha8_gamma_test
 *   ./s6e3ha8_gamma_test --rows S0|S1 <C8 as 68 hex digits>    the 74 rows, as Android's kernel prints them
 * (tools/s6e3ha8-gamma-test.sh does the first.)
 *
 * What is checked:
 *   - the curves are round((g / 255)^2.15 or ^2.20 * 2^24), worked out again here;
 *   - the tables: luminances rising from 2 to 400, base luminances rising from 105 to 400, the 400-nit row untuned
 *     (curve 2.20, no offset), the levels rising from 0 to 255, every AOR within what the driver sent before
 *     (0x000C-0x0B76), level 255 at 0x000C; the panel by ID; the row by level;
 *   - for every row and point, the gray it takes is within 0-255 before any clamp (so the BSP's unsigned gray, which
 *     would make a negative one 255, never matters);
 *   - C8 decoded as the BSP decodes it;
 *   - for each MTP and panel:
 *     the 400-nit row is the panel's centres (0x100 at 255, 0x80 elsewhere, VT 0), whatever the MTP (the calculation
 *     takes the MTP off again);
 *     every row, put back through the panel's dividers with the MTP added, gives voltages that fall from gray 0
 *     (VREGOUT) to 1 to 7, and from 11 to 23 ... to 255: a curve that rises with the gray (not 7 to 11, which Samsung's
 *     gray offset makes about equal in the dim rows, where a point's MTP can put a few millivolts the wrong way);
 *     its white, found on the calibrated curve, is within 6 % of the row's base luminance, where 255's value was not
 *     clamped (the 105-nit rows ask V255 for about the dimmest white it can make, VREGOUT * (1 - 129 / 860), and are
 *     often just past it, on Android as here);
 *     the light the panel gives, that white times the part of the frame the AOR leaves lit, does not fall by more
 *     than 4 % from one level to the next. (By the crown panel's 2984 lines a frame: the star tables do not state
 *     theirs. Samsung's tables themselves step down by about 2.5 % at a few levels where the next row begins: S0 at
 *     166, 194, 202 and 224. At 165, row 58, base 298 nits, with AOR 0x017C; at 166, row 59, base 307, with 0x020E:
 *     the same light only for a frame of some 5360 lines.)
 * Made-up MTPs: none (zero), a plausible one, 2000 random ones within +-10 (VT 0-3), and two extreme ones (only the
 * ranges are checked for those). Real MTPs are small corrections toward the target curve; much larger random ones
 * make curves no panel has, and the gray Samsung's calculation wants is then beyond a point's range (clamped).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/display/panel/hakios.s6e3ha8_gamma.h"

#define VTOTAL  2984

static int failures;
static struct ha8_work work;
static uint8_t table[HA8_ROWS][HA8_CA];

static void
fail(const char *what, int a, int b)
{
	if (failures++ < 40) {
		printf("FAIL %s (%d, %d)\n", what, a, b);
	}
}

/* a CA row's values per point and colour (the inverse of ha8_row_ca's packing; s6e3ha8.c gamma_ctoi) */
static void
ca_values(const uint8_t *ca, int32_t value[HA8_TP][3])
{
	for (int c = 0; c < 3; c++) {
		value[HA8_V255][c] = ((ca[0] >> c) & 1) << 8 | ca[1 + c];
		for (int p = HA8_V255 - 1; p > HA8_VT; p--) {
			value[p][c] = ca[(HA8_V255 - p) * 3 + 1 + c];
		}
	}
	value[HA8_VT][0] = ca[32] & 0xF;
	value[HA8_VT][1] = ca[33] >> 4;
	value[HA8_VT][2] = ca[33] & 0xF;
}

/* the luminance, on the calibrated curve, of a voltage (green), the gray found between two by the line */
static double
nits_of_volt(int64_t v)
{
	for (int g = 0; g < 255; g++) {
		int64_t hi = work.gray[g][1], lo = work.gray[g + 1][1];

		if (v <= hi && v >= lo) {
			double pos = g + (hi == lo ? 0 : (double)(hi - v) / (double)(hi - lo));

			return HA8_TARGET * pow(pos / 255, 2.2);
		}
	}
	return v > work.gray[0][1] ? 0 : HA8_TARGET;
}

/* the white a row gives (no AOR; -1 when 255's value was clamped), and whether its voltages fall from gray 0
 * (VREGOUT) through the points to 255 */
static double
row_white(const uint8_t *ca, bool *falling)
{
	int32_t value[HA8_TP][3], reg[HA8_TP][3];
	int64_t volt[HA8_TP][3];

	ca_values(ca, value);
	for (int p = 0; p < HA8_TP; p++) {
		for (int c = 0; c < 3; c++) {
			reg[p][c] = value[p][c] + work.offset[p][c];
		}
	}
	ha8_point_volts(reg, volt);
	*falling = true;
	for (int c = 0; c < 3; c++) {
		*falling = *falling && volt[1][c] < HA8_VREGOUT && volt[2][c] < volt[1][c];
		/* (not 7 to 11: in the dim rows Samsung's gray offset lifts gray 7 to about gray 11's voltage, and an MTP
		 * there can take it past by some millivolts) */
		for (int p = 4; p < HA8_TP; p++) {
			*falling = *falling && volt[p][c] < volt[p - 1][c];
		}
	}
	for (int c = 0; c < 3; c++) {
		if (value[HA8_V255][c] == 0 || value[HA8_V255][c] == 511) {
			return -1;
		}
	}
	return nits_of_volt(volt[HA8_V255][1]);
}

static void
print_rows(const struct ha8_panel *panel)
{
	for (int r = 0; r < HA8_ROWS; r++) {
		printf("gamma[%3d] : ", panel->rows[r].nits);
		for (int i = 0; i < HA8_CA; i++) {
			printf("%02X ", table[r][i]);
		}
		printf("\n");
	}
}

static bool
parse_mtp(const char *hex, uint8_t mtp[HA8_CA])
{
	if (strlen(hex) != 2 * HA8_CA) {
		return false;
	}
	for (int i = 0; i < HA8_CA; i++) {
		unsigned v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			return false;
		}
		mtp[i] = (uint8_t)v;
	}
	return true;
}

static void
check_tables(void)
{
	const uint32_t *curves[2] = { ha8_curve_215, ha8_curve_220 };
	const double gammas[2] = { 2.15, 2.20 };

	for (int k = 0; k < 2; k++) {
		for (int g = 0; g < 256; g++) {
			uint32_t want = (uint32_t)(pow(g / 255.0, gammas[k]) * (double)(1 << HA8_SHIFT) + 0.5);

			if (curves[k][g] != want) {
				fail("curve", k, g);
			}
		}
	}
	for (int k = 0; k < 3; k++) {
		const struct ha8_panel *panel = &ha8_panels[k];
		const struct ha8_row *last = &panel->rows[HA8_ROWS - 1];

		if (panel->rows[0].nits != 2 || last->nits != 400 || last->base != 400 || last->curve != HA8_G220) {
			fail("ends of the rows", k, 0);
		}
		for (int p = 0; p < HA8_TP; p++) {
			if (last->gray[p] != 0 || last->color[p][0] != 0 || last->color[p][1] != 0 || last->color[p][2] != 0) {
				fail("400-nit row tuned", k, p);
			}
		}
		for (int r = 0; r < HA8_ROWS; r++) {
			const struct ha8_row *row = &panel->rows[r];

			if ((r > 0 && row->nits <= panel->rows[r - 1].nits) || row->base > 400 || row->base < 102 ||
			    (r > 0 && row->base < panel->rows[r - 1].base)) {
				fail("row's luminances", k, r);
			}
			for (int p = 2; p < HA8_TP; p++) {
				uint64_t base = (uint64_t)row->base << HA8_SHIFT;
				const uint32_t *curve = row->curve == HA8_G220 ? ha8_curve_220 : ha8_curve_215;
				int gray = ha8_nearest_gray(p == HA8_V255 ? base :
				    base * curve[ha8_points[p].gray] >> HA8_SHIFT) + row->gray[p];

				if (gray < 0 || gray > 255) {
					fail("gray out of range", k, r * 100 + p);
				}
			}
		}
		for (int level = 0; level < 256; level++) {
			if (panel->aor[level] < 0x000C || panel->aor[level] > 0x0B76) {
				fail("AOR range", k, level);
			}
		}
		if (panel->aor[255] != 0x000C) {
			fail("AOR at 255", k, panel->aor[255]);
		}
	}
	if (ha8_row_level[0] != 0 || ha8_row_level[HA8_ROWS - 1] != 255) {
		fail("levels' ends", 0, 0);
	}
	for (int r = 1; r < HA8_ROWS; r++) {
		if (ha8_row_level[r] <= ha8_row_level[r - 1]) {
			fail("levels rising", r, 0);
		}
	}
	if (ha8_row_of_level(0) != 0 || ha8_row_of_level(1) != 1 || ha8_row_of_level(7) != 1 ||
	    ha8_row_of_level(8) != 2 || ha8_row_of_level(255) != HA8_ROWS - 1) {
		fail("row of level", ha8_row_of_level(1), ha8_row_of_level(8));
	}
	if (ha8_panel_of(0x910444, false) != &ha8_panels[1] || ha8_panel_of(0x910443, false) != &ha8_panels[0] ||
	    ha8_panel_of(0x910453, false) != &ha8_panels[0] || ha8_panel_of(0, false) != &ha8_panels[0] ||
	    ha8_panel_of(0, true) != &ha8_panels[2]) {
		fail("panel of ID", 0, 0);
	}
}

static void
check_mtp_decode(void)
{
	uint8_t mtp[HA8_CA] = { 0 };
	int32_t off[HA8_TP][3];

	mtp[0] = 0x01;                          /* 255's sign */
	mtp[1] = 0x05; mtp[2] = 0x81; mtp[3] = 0x00;    /* 255: -5, -129, 0 (bit 7 is magnitude there) */
	mtp[4] = 0x83; mtp[5] = 0x02; mtp[6] = 0xFF;    /* 203: -3, 2, -127 */
	mtp[28] = 0x01; mtp[29] = 0x80; mtp[30] = 0x8A; /* 1: 1, 0, -10 */
	mtp[31] = 0xEE;                         /* unused */
	mtp[32] = 0xA7; mtp[33] = 0x3C;         /* VT: 7, 3, 12 */
	ha8_mtp(mtp, off);
	if (off[10][0] != -5 || off[10][1] != -129 || off[10][2] != 0 || off[9][0] != -3 || off[9][1] != 2 ||
	    off[9][2] != -127 || off[1][0] != 1 || off[1][1] != 0 || off[1][2] != -10 || off[0][0] != 7 ||
	    off[0][1] != 3 || off[0][2] != 12 || off[5][1] != 0) {
		fail("MTP decode", off[10][1], off[0][2]);
	}
}

struct result {
	uint32_t clamped;
	double worst_white;     /* the largest |white / base - 1| */
	double worst_dip;       /* the largest fall of the light from one level to the next, as a part of it */
	bool centres, falling;
};

static struct result
check_one(const struct ha8_panel *panel, const uint8_t mtp[HA8_CA])
{
	struct result res = { 0, 0, 0, true, true };
	double white[HA8_ROWS], last = -1;

	res.clamped = ha8_gamma(&work, panel, mtp, table);
	for (int i = 0; i < HA8_CA; i++) {
		uint8_t want = i == 0 ? 0x07 : (i >= 4 && i <= 30) ? 0x80 : 0x00;

		res.centres = res.centres && table[HA8_ROWS - 1][i] == want;
	}
	for (int r = 0; r < HA8_ROWS; r++) {
		bool falling;
		double off;

		white[r] = row_white(table[r], &falling);
		res.falling = res.falling && falling;
		if (white[r] < 0) {
			continue;
		}
		off = fabs(white[r] / panel->rows[r].base - 1);
		res.worst_white = off > res.worst_white ? off : res.worst_white;
	}
	for (int level = 0; level < 256; level++) {
		double light = white[ha8_row_of_level(level)] * (VTOTAL - panel->aor[level]) / VTOTAL;

		if (white[ha8_row_of_level(level)] < 0) {
			last = -1;
			continue;
		}
		if (last > 0 && light < last && (last - light) / last > res.worst_dip) {
			res.worst_dip = (last - light) / last;
		}
		last = light;
	}
	return res;
}

static bool
sane(const struct result *res)
{
	return res->centres && res->falling && res->worst_white <= 0.06 && res->worst_dip <= 0.04;
}

static uint64_t seed = 0x9E3779B97F4A7C15ull;

static unsigned
random_below(unsigned n)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (unsigned)(seed % n);
}

/* an MTP within +-limit at every point (VT 0-3): sign-magnitude, 255's sign the one bit of byte 0 */
static void
random_mtp(uint8_t mtp[HA8_CA], unsigned limit)
{
	memset(mtp, 0, HA8_CA);
	mtp[0] = (uint8_t)random_below(2);
	for (int i = 1; i <= 3; i++) {
		mtp[i] = (uint8_t)random_below(limit + 1);
	}
	for (int i = 4; i <= 30; i++) {
		unsigned size = random_below(limit + 1), negative = random_below(2);

		mtp[i] = (uint8_t)(size | (negative ? 0x80 : 0));
	}
	mtp[32] = (uint8_t)random_below(4);
	mtp[33] = (uint8_t)(random_below(4) << 4);
	mtp[33] |= (uint8_t)random_below(4);
}

static void
report(const struct ha8_panel *panel, const char *what, const struct result *res)
{
	printf("%s, %-10s %3u clamped, 400 nits = centres %s, curves falling %s, white within %.2f %%, "
	    "light's worst dip %.2f %%\n", panel->name, what, res->clamped, res->centres ? "yes" : "NO",
	    res->falling ? "yes" : "NO", 100 * res->worst_white, 100 * res->worst_dip);
}

int
main(int argc, char **argv)
{
	static const uint8_t plausible[HA8_CA] = {
		0x00, 0x03, 0x01, 0x05, 0x02, 0x81, 0x03, 0x84, 0x00, 0x02, 0x05, 0x82, 0x01, 0x03, 0x00, 0x83,
		0x86, 0x02, 0x04, 0x08, 0x85, 0x0A, 0x07, 0x00, 0x8C, 0x09, 0x84, 0x10, 0x0B, 0x88, 0x00, 0x00,
		0x02, 0x31,
	};
	uint8_t mtp[HA8_CA];
	uint32_t random_clamped[3] = { 0, 0, 0 };
	double worst_white[3] = { 0, 0, 0 }, worst_dip[3] = { 0, 0, 0 };

	if (argc == 4 && strcmp(argv[1], "--rows") == 0) {
		int panel_index = strcmp(argv[2], "STAR2") == 0 ? 2 : strcmp(argv[2], "S1") == 0 ? 1 : 0;
		const struct ha8_panel *panel = &ha8_panels[panel_index];

		if (!parse_mtp(argv[3], mtp)) {
			fprintf(stderr, "want 68 hex digits\n");
			return 2;
		}
		ha8_gamma(&work, panel, mtp, table);
		print_rows(panel);
		return 0;
	}
	check_tables();
	check_mtp_decode();
	for (int k = 0; k < 3; k++) {
		const struct ha8_panel *panel = &ha8_panels[k];
		struct result res;

		memset(mtp, 0, sizeof(mtp));
		res = check_one(panel, mtp);
		report(panel, "MTP 0:", &res);
		if (!sane(&res)) {
			fail("MTP 0", k, (int)res.clamped);
		}
		res = check_one(panel, plausible);
		report(panel, "plausible:", &res);
		if (!sane(&res)) {
			fail("MTP plausible", k, (int)res.clamped);
		}
		for (int r = 0; r < HA8_ROWS; r++) {
			if (panel->rows[r].nits == 2 || panel->rows[r].nits == 100 || panel->rows[r].nits == 400) {
				printf("    gamma[%3d] :", panel->rows[r].nits);
				for (int i = 0; i < HA8_CA; i++) {
					printf(" %02X", table[r][i]);
				}
				printf("\n");
			}
		}
		for (int n = 0; n < 2000; n++) {
			random_mtp(mtp, 10);
			res = check_one(panel, mtp);
			random_clamped[k] += res.clamped;
			worst_white[k] = res.worst_white > worst_white[k] ? res.worst_white : worst_white[k];
			worst_dip[k] = res.worst_dip > worst_dip[k] ? res.worst_dip : worst_dip[k];
			if (!sane(&res)) {
				fail("random MTP", k, n);
			}
		}
		printf("%s, 2000 random:  %u clamped in all (%.1f an MTP), white within %.2f %%, light's worst dip %.2f %%\n",
		    panel->name, random_clamped[k], random_clamped[k] / 2000.0, 100 * worst_white[k], 100 * worst_dip[k]);
		memset(mtp, 0xFF, sizeof(mtp));
		res = check_one(panel, mtp);
		printf("%s, MTP all FF:   %u clamped (ranges only)\n", panel->name, res.clamped);
		memset(mtp, 0x7F, sizeof(mtp));
		res = check_one(panel, mtp);
		printf("%s, MTP all 7F:   %u clamped (ranges only)\n", panel->name, res.clamped);
	}
	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
