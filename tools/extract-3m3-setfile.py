#!/usr/bin/env python3
"""Make drivers/camera/exynos/hakios.s5k3m3_setfiles.h from Apollo's S5K3M3 setfile A.

  C=APOLLO/drivers/media/platform/exynos/fimc-is2/sensor/module_framework/cis
  tools/extract-3m3-setfile.py $C/fimc-is-cis-3m3-setA.h > drivers/camera/exynos/hakios.s5k3m3_setfiles.h

The Galaxy S9+'s telephoto camera (Star2, "rear2", S5K3M3 at 0x2D on the camera's HSI2C2, I2C_CAM2 0x10880000) is
set up by two of a setfile's tables, in the order sensor_3m3_cis_set_global_setting() and sensor_3m3_cis_mode_change()
write them: Global (a soft reset, the sensor's firmware patch as burst writes to the indirect data port 0x6F12, and
its analogue settings), then one mode's table. The mode taken is SENSOR_3M3_2016X1512_30FPS (index 4 of
sensor_3m3_setfiles_A; config_module_3m3's mode 4): 2016 x 1512 RAW10 (2x2 binned), four lanes at 1196 Mb/s,
CSI_MODE_VC_ONLY, PD_NONE (the phase-detection tail, 128 x 736, comes on VC1 and is not taken here).

Which set: Apollo's star2 device tree gives the 3M3 `setfile = "setA"` (fimc-is-3m3@2D), and sensor_3m3_cis_probe()
takes setfile A for "setA" (and for "default"); fimc-is-cis-3m3-setB.h is wrapped in #if 0. Unlike the 2L3's, the
3M3's driver does not choose by revision: setfile A is the only one.

The tables are the sensor's own register values: data, read here and written out as data. Each entry of Apollo's is
(address, value, size) in u32s; I2C_MODE_BURST_ADDR/_DATA and I2C_MODE_DELAY are its markers. An entry here has the
S5K2L3 header's layout (struct s2l3_reg and its S2L3_W16/W8/BURST/DELAY, hakios.s5k2l3_setfiles.h, which must be
included first), so that the one table writer of hakios.exynos_camera.c (rear_table) writes both sensors' tables.
"""
import re
import sys

TABLES = [
    ("global", "sensor_3m3_setfile_A_Global"),
    ("mode_2016x1512_30", "sensor_3m3_setfile_A_2016x1512_30fps"),
]
PLL = "sensor_3m3_pllinfo_A_2016x1512_30fps"
OPS = ("S2L3_W16", "S2L3_W8", "S2L3_BURST", "S2L3_DELAY")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def table(text, name):
    m = re.search(r"const\s+u32\s+%s\s*\[\s*\]\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("extract-3m3-setfile: %s not found" % name)
    words = [w.strip() for w in m.group(1).split(",") if w.strip()]
    if len(words) % 3:
        sys.exit("extract-3m3-setfile: %s has %d words, not a multiple of 3" % (name, len(words)))
    out, burst_port = [], None
    for i in range(0, len(words), 3):
        a, v, s = words[i:i + 3]
        if a == "I2C_MODE_BURST_ADDR":
            burst_port = int(v, 0)
            if int(s, 0) != 2:
                sys.exit("extract-3m3-setfile: %s: a burst of %s-byte words" % (name, s))
            continue
        if a == "I2C_MODE_BURST_DATA":
            if burst_port is None:
                sys.exit("extract-3m3-setfile: %s: burst data without a burst address" % name)
            out.append(("S2L3_BURST", burst_port, int(v, 0)))
            continue
        burst_port = None
        if a == "I2C_MODE_DELAY":
            us = int(v, 0)
            if us > 0xFFFF:
                sys.exit("extract-3m3-setfile: %s: a delay of %d us" % (name, us))
            out.append(("S2L3_DELAY", 0, us))
            continue
        addr, val, size = int(a, 0), int(v, 0), int(s, 0)
        if size not in (1, 2) or addr > 0xFFFF or val > (0xFF if size == 1 else 0xFFFF):
            sys.exit("extract-3m3-setfile: %s: entry %s %s %s" % (name, a, v, s))
        out.append(("S2L3_W8" if size == 1 else "S2L3_W16", addr, val))
    return out


def pll(text, name):
    m = re.search(r"%s\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("extract-3m3-setfile: %s not found" % name)
    words = [w.strip() for w in m.group(1).split(",") if w.strip()]
    # ext_clk (EXT_CLK_Mhz * 1000 * 1000), mipi_datarate, pclk, frame_length_lines, line_length_pck
    return int(words[1], 0), int(words[2], 0), int(words[3], 0), int(words[4], 0)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    text = strip_comments(open(sys.argv[1], encoding="latin-1").read())
    print("/*")
    print(" * Made by tools/extract-3m3-setfile.py from Apollo's fimc-is-cis-3m3-setA.h: the S5K3M3's own register")
    print(" * values, as data. Do not edit; run the tool again. Uses hakios.s5k2l3_setfiles.h's struct s2l3_reg.")
    print(" */")
    print("#ifndef HAKIOS_S5K3M3_SETFILES_H")
    print("#define HAKIOS_S5K3M3_SETFILES_H")
    print("")
    print("#ifndef HAKIOS_S5K2L3_SETFILES_H")
    print("#error \"include hakios.s5k2l3_setfiles.h first (struct s2l3_reg)\"")
    print("#endif")
    for short, tname in TABLES:
        entries = table(text, tname)
        counts = {k: sum(1 for e in entries if e[0] == k) for k in OPS}
        print("")
        print("/* %s: %d entries (%d 16-bit, %d 8-bit, %d burst words, %d pauses) */" % (
            tname, len(entries), counts["S2L3_W16"], counts["S2L3_W8"], counts["S2L3_BURST"], counts["S2L3_DELAY"]))
        print("static const struct s2l3_reg s3m3_%s[] = {" % short)
        line = []
        for op, a, v in entries:
            line.append("{ %#06x, %#06x, %s }," % (a, v, op))
            if len(line) == 4:
                print("\t" + " ".join(line))
                line = []
        if line:
            print("\t" + " ".join(line))
        print("};")
    rate, pclk, fll, llp = pll(text, PLL)
    print("")
    print("/* the mode's timing (%s) */" % PLL)
    print("#define S3M3_MODE_MIPI_MBPS %u" % (rate // 1000000))
    print("#define S3M3_MODE_PCLK %u" % pclk)
    print("#define S3M3_MODE_LINES %#x" % fll)
    print("#define S3M3_MODE_LINE_PCK %#x" % llp)
    print("#define S3M3_MODE_LINE_NS %u" % ((llp * 1000000000 + pclk // 2) // pclk))
    print("")
    print("#endif /* HAKIOS_S5K3M3_SETFILES_H */")


main()
