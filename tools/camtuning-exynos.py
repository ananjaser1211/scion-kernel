#!/usr/bin/env python3
"""Make a camera's colour tuning, as libhakios.media's processor reads it (userland/src/camera_isp.c,
isp_load_tuning; the format is tools/camtuning.py's, "RWCISP02"), out of an Exynos (fimc-is v5) phone's
setfile and RTA library -- the Galaxy S9's /vendor/firmware/setfile_<sensor>.bin and fimc_is_rta_2l3_3h1.bin
(the vendor's, kept in fw-camera-s9/ and never in the repository; so is what this makes of them).

usage: camtuning-exynos.py SETFILE RTA OUT.isp [options]     (OUT.isp "-": lists what is there only)
  --sensor 2l3|3h1|3m3   which camera (default: from SETFILE's name); picks the RTA's white balance set
  --awb-set N            the RTA's white balance set to take instead (0-2, in the file's order)
  --record N             the setfile's colour record whose matrix is taken (default 1)
  --keep-fluorescent     keep the fluorescent tube among the reference lights (it is off the daylight
                         locus, greener; left out, the line runs from the filament lamp to daylight)
  --no-lsc               no lens shading: 0 ranges (isp_load_tuning as it is refuses that); without this
                         option one range of gains of 1 is written, which corrects nothing

What is taken, and how sure it is:

The setfile (kernel: drivers/media/platform/exynos/fimc-is2/hardware/fimc-is-hw-control.c,
parse_setfile_header): u32 magic 0x12345679 (V3), designed bits, "ISV5", a revision, the number of
scenarios and of sub-IPs, the offset of the entries; then scenario -> setfile index per IP, the count of
setfiles per IP, and (offset, size) of each. IP 0 is 3AA+ISP (designed bit 3); its setfiles are 0x23dc0
bytes each, the scenarios' variants of one structure (only noise tables differ). In it, packed at odd
offsets, a table of 23 colour records 0xf4 bytes apart: a 3x3 matrix (s16, 256 = 1, each row summing to
256), an identity, two copies of nine target colours, seven reference and nine source colours. The matrices
are the same in the 2L3's, 3H1's and 3M3's setfiles: they are the ISP's colour matrix presets, not a
calibration of a sensor (Samsung's pipeline makes that at run time: the RTA's ACCM and the nine-colour
tables, whose layout is not known). The chosen one is given to every light and temperature; record 1 (the
same as 2, 3 and 14) is the default.

The RTA (an AArch64 blob, its data after 0xf6000): white balance sets of floats, each with the gains
(R, 1, B) of the presets incandescent, fluorescent, daylight and cloudy, then seven R gains and seven B
gains of lights from a candle-like one to a deep shade, the presets among them (a fourth set further on is
all 1s and is not found here). Which set is which camera is written nowhere found: taken to be the sensor
positions in order (rear 2L3, front 3H1, rear tele 3M3). Their colour temperatures are nominal: 2300, 2850
(incandescent), 4000 (fluorescent), 5500 (daylight), 6500 (cloudy), 7500, 10000 K.
"""
import re
import struct
import sys

GRID = 17 * 13
MAGIC = 0x12345679
RECORDS = 23
RECORD = 0xf4
LIST_CCTS = (2300, 2850, 4000, 5500, 6500, 7500, 10000)
PRESET_CCTS = (2850, 4000, 5500, 6500)
FLUORESCENT = 4000
SET_OF_SENSOR = {"2l3": 0, "3h1": 1, "3m3": 2}


def ip0_blob(data):
    """the first 3AA+ISP setfile: (its offset in the file, its bytes)"""
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MAGIC or data[8:12] != b"ISV5":
        sys.exit("camtuning-exynos: not a fimc-is v5 setfile")
    scenarios, ips, entries = struct.unpack_from("<3I", data, 16)
    counts_at = 28 + 4 * scenarios * ips
    counts = struct.unpack_from("<%dI" % ips, data, counts_at)
    off, size = struct.unpack_from("<II", data, counts_at + 4 * ips)
    if counts[0] < 1 or entries + off + size > len(data):
        sys.exit("camtuning-exynos: the setfile's table points out of the file")
    return entries + off, data[entries + off:entries + off + size]


def colour_records(blob):
    """the colour records' matrices, [(offset in the blob, 9 floats)]: each found by its identity (a
    record's second matrix) after a matrix whose rows sum to 256, 0xf4 bytes apart"""
    ident = struct.pack("<9h", 256, 0, 0, 0, 256, 0, 0, 0, 256)
    for m in re.finditer(re.escape(ident), blob):
        at = m.start() - 18
        found = []
        for i in range(RECORDS):
            a = at + i * RECORD
            if a < 0 or blob[a + 18:a + 36] != ident:
                break
            v = struct.unpack_from("<9h", blob, a)
            if any(sum(v[3 * r:3 * r + 3]) != 256 for r in range(3)):
                break
            found.append((a, [x / 256 for x in v]))
        if len(found) == RECORDS:
            return found
    sys.exit("camtuning-exynos: no table of colour records in the setfile")


def awb_sets(rta):
    """the RTA's white balance sets with presets, in the file's order:
    [(offset, presets [(R gain, B gain)] x 4, list [(R gain, B gain)] x 7)]"""
    out, at = [], 0xf6000
    while at + 4 * 26 <= len(rta):
        v = struct.unpack_from("<12f", rta, at)
        trip = [v[3 * k:3 * k + 3] for k in range(4)]
        if all(g == 1.0 and 0.8 < r < 4 and 0.8 < b < 4 for r, g, b in trip) and \
                trip[0][0] < trip[2][0] < trip[3][0] and trip[0][2] > trip[2][2] > trip[3][2] and \
                trip[3][0] > 1.3 * trip[0][0] and trip[0][2] > 1.3 * trip[3][2]:
            lst = struct.unpack_from("<14f", rta, at + 48)
            out.append((at, [(r, b) for r, _, b in trip], list(zip(lst[:7], lst[7:]))))
            at += 4 * 26
            continue
        at += 4
    return out


def references(presets, lst, keep_fluorescent):
    """[(R/G, B/G, CCT)] of grey under each light, from the bluest light to the reddest"""
    if any(abs(r - 1) > 1e-6 or abs(b - 1) > 1e-6 for r, b in lst):
        points = list(zip(lst, LIST_CCTS))
    else:
        points = list(zip(presets, PRESET_CCTS))
    points = [(gains, cct) for gains, cct in points if keep_fluorescent or cct != FLUORESCENT]
    return [(1 / r, 1 / b, float(cct)) for (r, b), cct in sorted(points, key=lambda p: -p[1])]


def main():
    opts = {"--sensor": None, "--awb-set": None, "--record": "1"}
    flags, pos, args = set(), [], sys.argv[1:]
    while args:
        a = args.pop(0)
        if a in opts and args:
            opts[a] = args.pop(0)
        elif a in ("--keep-fluorescent", "--no-lsc"):
            flags.add(a)
        else:
            pos.append(a)
    if len(pos) != 3:
        sys.exit(__doc__)
    setfile, rta_path, out_path = pos
    data = open(setfile, "rb").read()
    rta = open(rta_path, "rb").read()
    sensor = opts["--sensor"] or next((s for s in SET_OF_SENSOR if s in setfile.lower()), None)
    if opts["--awb-set"] is not None:
        set_index = int(opts["--awb-set"])
    elif sensor in SET_OF_SENSOR:
        set_index = SET_OF_SENSOR[sensor]
    else:
        sys.exit("camtuning-exynos: which camera? (--sensor or --awb-set)")

    base, blob = ip0_blob(data)
    recs = colour_records(blob)
    record = int(opts["--record"])
    matrix = recs[record][1]
    sets = awb_sets(rta)
    if set_index >= len(sets):
        sys.exit("camtuning-exynos: the RTA has %d white balance sets with presets" % len(sets))
    _, presets, lst = sets[set_index]
    refs = references(presets, lst, "--keep-fluorescent" in flags)

    for i, (a, m) in enumerate(recs):
        print("%s colour record %2d at 0x%x: %s" % ("*" if i == record else " ", i, base + a,
                                                    " ".join("%6.3f" % x for x in m)))
    for i, (a, pr, ls) in enumerate(sets):
        print("%s white balance set %d at 0x%x: presets (R, B gain) %s; list %s" % (
            "*" if i == set_index else " ", i, a, " ".join("%.3f/%.3f" % p for p in pr),
            " ".join("%.3f/%.3f" % p for p in ls)))
    for rg, bg, cct in refs:
        print("reference light R/G %.3f B/G %.3f, %.0f K" % (rg, bg, cct))
    if out_path == "-":
        return

    out = bytearray(b"RWCISP02")
    # the colour matrix: one light range (every lux index) and one temperature range (every CCT)
    out += struct.pack("<I2fI2f9f", 1, 0, 1000, 1, 0, 100000, *matrix)
    if "--no-lsc" in flags:
        out += struct.pack("<I", 0)
    else:
        out += struct.pack("<I2fI2f", 1, 0, 1000, 1, 0, 100000) + struct.pack("<%df" % (4 * GRID), *[1.0] * (4 * GRID))
    out += struct.pack("<I", len(refs))
    for ref in refs:
        out += struct.pack("<3f", *ref)
    open(out_path, "wb").write(out)


main()
