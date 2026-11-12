#!/usr/bin/env python3
"""nmea-synth.py [-n SECONDS] [--lat L] [--lon L] [--hdop H] [--speed KNOTS] > test.nmea

A made-up NMEA 0183 stream for testing gnssd without the receiver (docs/gnssd.md): one epoch a second of RMC, GGA,
GSA and GSV, by default at the Null Island test point (0.001 N, 0.001 E, in the Gulf of Guinea; locationd treats
exactly 0,0 as "no position"), drifting slowly east. Never use a real place of anyone's here.
"""
import argparse, datetime, math

def ck(s):
    c = 0
    for ch in s: c ^= ord(ch)
    return '$%s*%02X' % (s, c)

def dm(v, pos, neg, w):
    h = pos if v >= 0 else neg
    v = abs(v); d = int(v)
    return '%0*d%07.4f' % (w, d, (v - d) * 60), h

a = argparse.ArgumentParser()
a.add_argument('-n', type=int, default=60)
a.add_argument('--lat', type=float, default=0.001)
a.add_argument('--lon', type=float, default=0.001)
a.add_argument('--hdop', type=float, default=0.9)
a.add_argument('--speed', type=float, default=1.0, help='knots, to the east')
o = a.parse_args()
t0 = datetime.datetime(2026, 1, 1, 12, 0, 0)
sats = [(2, 60, 45, 44), (5, 35, 120, 40), (12, 20, 200, 35), (15, 70, 300, 46), (18, 15, 30, 31),
        (21, 45, 250, 42), (25, 25, 160, 38), (29, 55, 80, 43)]
for i in range(o.n):
    t = t0 + datetime.timedelta(seconds=i)
    lon = o.lon + i * o.speed * 0.514444 / 111320.0
    la, lah = dm(o.lat, 'N', 'S', 2); lo, loh = dm(lon, 'E', 'W', 3)
    hms = t.strftime('%H%M%S') + '.00'
    print(ck('GPRMC,%s,A,%s,%s,%s,%s,%.1f,90.0,%s,,,A' % (hms, la, lah, lo, loh, o.speed, t.strftime('%d%m%y'))))
    print(ck('GPGGA,%s,%s,%s,%s,%s,1,%02d,%.1f,12.0,M,17.0,M,,' % (hms, la, lah, lo, loh, len(sats), o.hdop)))
    print(ck('GPGSA,A,3,%s,%.1f,%.1f,%.1f' % (','.join('%02d' % s[0] for s in sats) + ',' * (12 - len(sats)),
                                               o.hdop * 1.6, o.hdop, o.hdop * 1.3)))
    for m in range(2):
        part = sats[m * 4:m * 4 + 4]
        print(ck('GPGSV,2,%d,%02d,%s' % (m + 1, len(sats), ','.join('%02d,%02d,%03d,%02d' % s for s in part))))
