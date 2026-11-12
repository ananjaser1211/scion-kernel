#!/usr/bin/env python3
"""Make drivers/camera/exynos/hakios.s5k2l3_setfiles.h from Apollo's S5K2L3 setfiles A and B.

  C=APOLLO/drivers/media/platform/exynos/fimc-is2/sensor/module_framework/cis
  tools/extract-2l3-setfile.py $C/fimc-is-cis-2l3-setA.h $C/fimc-is-cis-2l3-setB.h \
      > drivers/camera/exynos/hakios.s5k2l3_setfiles.h

The Galaxy S9+'s primary rear camera (Star2, S5K2L3 at 0x10 on the camera's HSI2C0) is set up by three of a
setfile's tables, in the order sensor_2l3_cis_set_global_setting_internal() and sensor_2l3_cis_mode_change() write
them: Reset_TnP (the sensor's firmware patch, mostly burst writes to the indirect data port 0x6F12), Global, and one
mode's table. The mode taken is SENSOR_2L3_2016X1512_30FPS_MODE2 (index 14 of sensor_2l3_setfiles_A and _B): 2016 x
1512 RAW10, four lanes at 1352 Mb/s, no phase-detection virtual channel (PD_NONE), so the link carries VC0 alone.

Which set a sensor takes is decided by its revision (register 0x0002), as sensor_2l3_cis_select_setfile() decides
it: 0xA2-0xA4 (EVT0.2) setfile A, 0xA5 (EVT0.3) setfile B, any other none.

The tables are the sensor's own register values: data, read here and written out as data. Each entry of Apollo's is
(address, value, size) in u32s; I2C_MODE_BURST_ADDR/_DATA and I2C_MODE_DELAY are its markers. Here an entry is one of
  S2L3_W16 address value      a 16-bit write
  S2L3_W8  address value      an 8-bit write
  S2L3_BURST port value       one word of a burst to `port' (consecutive words of one burst are written in one
                              message, or several: the port increments its own pointer, so a burst split is the same)
  S2L3_DELAY 0 microseconds   a pause
"""
import re
import sys

TABLES = [
    ("reset_tnp", "sensor_2l3_setfile_%s_Reset_TnP"),
    ("global", "sensor_2l3_setfile_%s_Global"),
    ("mode_2016x1512_30", "sensor_2l3_setfile_%s_2016x1512_30fps_mode2"),
]
PLL = "sensor_2l3_pllinfo_%s_2016x1512_30fps_mode2"
OPS = ("S2L3_W16", "S2L3_W8", "S2L3_BURST", "S2L3_DELAY")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def table(text, name):
    m = re.search(r"const\s+u32\s+%s\s*\[\s*\]\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("extract-2l3-setfile: %s not found" % name)
    words = [w.strip() for w in m.group(1).split(",") if w.strip()]
    if len(words) % 3:
        sys.exit("extract-2l3-setfile: %s has %d words, not a multiple of 3" % (name, len(words)))
    out, burst_port = [], None
    for i in range(0, len(words), 3):
        a, v, s = words[i:i + 3]
        if a == "I2C_MODE_BURST_ADDR":
            burst_port = int(v, 0)
            if int(s, 0) != 2:
                sys.exit("extract-2l3-setfile: %s: a burst of %s-byte words" % (name, s))
            continue
        if a == "I2C_MODE_BURST_DATA":
            if burst_port is None:
                sys.exit("extract-2l3-setfile: %s: burst data without a burst address" % name)
            out.append(("S2L3_BURST", burst_port, int(v, 0)))
            continue
        burst_port = None
        if a == "I2C_MODE_DELAY":
            us = int(v, 0)
            if us > 0xFFFF:
                sys.exit("extract-2l3-setfile: %s: a delay of %d us" % (name, us))
            out.append(("S2L3_DELAY", 0, us))
            continue
        addr, val, size = int(a, 0), int(v, 0), int(s, 0)
        if size not in (1, 2) or addr > 0xFFFF or val > (0xFF if size == 1 else 0xFFFF):
            sys.exit("extract-2l3-setfile: %s: entry %s %s %s" % (name, a, v, s))
        out.append(("S2L3_W8" if size == 1 else "S2L3_W16", addr, val))
    return out


def pll(text, name):
    m = re.search(r"%s\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("extract-2l3-setfile: %s not found" % name)
    words = [w.strip() for w in m.group(1).split(",") if w.strip()]
    # ext_clk, mipi_datarate, pclk, frame_length_lines, line_length_pck
    return int(words[1], 0), int(words[2], 0), int(words[3], 0), int(words[4], 0)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sets = [("A", strip_comments(open(sys.argv[1], encoding="latin-1").read())),
            ("B", strip_comments(open(sys.argv[2], encoding="latin-1").read()))]
    print("/*")
    print(" * Made by tools/extract-2l3-setfile.py from Apollo's fimc-is-cis-2l3-setA.h and -setB.h: the S5K2L3's own")
    print(" * register values, as data. Do not edit; run the tool again.")
    print(" */")
    print("#ifndef HAKIOS_S5K2L3_SETFILES_H")
    print("#define HAKIOS_S5K2L3_SETFILES_H")
    print("")
    print("enum { S2L3_W16, S2L3_W8, S2L3_BURST, S2L3_DELAY };")
    print("struct s2l3_reg { uint16_t addr, value; uint8_t op; };")
    timing = None
    for letter, text in sets:
        name = PLL % letter
        t = pll(text, name)
        if timing is not None and t != timing:
            sys.exit("extract-2l3-setfile: %s differs from setfile A's timing" % name)
        timing = t
        for short, tname in TABLES:
            tname = tname % letter
            entries = table(text, tname)
            counts = {k: sum(1 for e in entries if e[0] == k) for k in OPS}
            print("")
            print("/* %s: %d entries (%d 16-bit, %d 8-bit, %d burst words, %d pauses) */" % (
                tname, len(entries), counts["S2L3_W16"], counts["S2L3_W8"], counts["S2L3_BURST"], counts["S2L3_DELAY"]))
            print("static const struct s2l3_reg s2l3_%s_%s[] = {" % (letter.lower(), short))
            line = []
            for op, a, v in entries:
                line.append("{ %#06x, %#06x, %s }," % (a, v, op))
                if len(line) == 4:
                    print("\t" + " ".join(line))
                    line = []
            if line:
                print("\t" + " ".join(line))
            print("};")
    rate, pclk, fll, llp = timing
    print("")
    print("/* the mode's timing, the same in both sets (sensor_2l3_pllinfo_[AB]_2016x1512_30fps_mode2) */")
    print("#define S2L3_MODE_MIPI_MBPS %u" % (rate // 1000000))
    print("#define S2L3_MODE_PCLK %u" % pclk)
    print("#define S2L3_MODE_LINES %#x" % fll)
    print("#define S2L3_MODE_LINE_PCK %#x" % llp)
    print("")
    print("#endif /* HAKIOS_S5K2L3_SETFILES_H */")


main()
