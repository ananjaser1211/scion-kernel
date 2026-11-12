# gnssd: the GNSS chain into iOS

`gnssd` (`userland/src/gnssd.m` with `src/gnss_chip.c`, job `com.hakios.gnssd`) does three things:
- it brings the BCM4775's GNSS receiver up and reads the on-chip engine's fixes;
- it hands them to locationd **as the phone's own GPS**, so Maps, Safari's geolocation, Weather and every CoreLocation
  client see GPS positions;
- it writes a status file for the GPS app (`com.hakios.gps`).

The chip side, which brings the receiver up and gets the first fix, is in [gnss.md](gnss.md). This page covers the
iOS side.

Status (12 Nov 2026, night): locationd talks to gnssd as its gpsd, and START/STOP follow its clients. The chip
sequence is in gnssd. The chip does not produce satellites yet, so no real fix has gone through.

## How a position can get into locationd (iOS 16.5, 20F66, read from the system image)

| Route | What locationd makes of it | Here |
|---|---|---|
| **Built-in GNSS**: `CLGnssProvider` -> `CLGnssController` -> `CLGnssDaemonDevice` -> XPC `com.apple.gpsd`, protobuf `proto.gpsd.Request` / `Response` / `Indication` (schema embedded in locationd: `GpsdProtocol.proto`, `GnssTypes.proto`) | the phone's GPS: not simulated, not accessory | **used**, with one change to locationd (below) |
| Built-in GNSS, baseband: `CLEurekaController` (Qualcomm QMI PDS through CommCenter), `CLIceController` (Intel ARI) | GPS | no: no such baseband stack here |
| External accessory: `CLAccessoryLocationProvider` <- ExternalAccessory (`EAAccessoryDidReceiveNMEASentenceNotification`) <- accessoryd iAP2 LocationInformation / iapd iAP1 | `isProducedByAccessory` | no: the accessory's features depend on MFi authentication (`-[ACCTransportIOAccessoryManager setFeaturesFromAuthStatus:authCert:certType:]`) |
| Simulation: `CLSimulationManager` (entitlement `com.apple.locationd.simulation`) | `isSimulatedBySoftware` | the fallback, `gnssd -L sim` |

**The one change.**
- locationd picks the device behind its GNSS controller in `CLGnssDeviceFacade::create` (0x1011eb084), from the
  hardware model only:
  - `CLGnssDaemonDevice` (gpsd) for the models in a fixed set (0x1003b0104);
  - otherwise `CLEurekaController` or `CLGnssUnknownDevice`.
- On iPhone10,3 that means a Qualcomm baseband this phone has not got. No preference overrides it.
- `src/inject_locationd.c` (`libhakios.locationd.dylib`, inserted into locationd through `etc/inject.conf`) replaces
  the one call of the predicate inside `create` (0x1011eb0e0, `bl` -> `mov w0, #1`), and nothing else. It does so
  only on iPhone10,3, only if the instruction there is build 20F66's, and it logs one console line.
- locationd's new GNSS stack itself is its own preference `UseNewGnssArchitecture`, in **mobile's**
  `com.apple.locationd` domain. It is seeded in `userland/var/`, and gnssd sets it too when a kept data volume lacks
  it (locationd reads it at start).
- Apple's gpsd job is left out (`mkjobs.py`): `com.hakios.gnssd` owns `com.apple.gpsd`.

**The protocol** (tested on the S9+ with gnssd serving and no fixes):
- locationd connects and sends a connection message. gnssd answers with a STATUS indication at once and then every
  5 s. Without it, locationd's "gpsd alive" timer moves its device to Recovery and it sends EXIT.
- Then BUILD_DEVICE (answered with STATUS) and SET_CONFIG_ENABLE_GNSS_CONSTELLATIONS.
- When a client wants GNSS: START (answered with Indication START_COMPLETED / kSuccess), then the assistance it has.
  That is INJECT_ASSISTANCE_TIME, INJECT_ASSISTANCE_POSITION (its Wi-Fi position, about every 5 s),
  INJECT_ASSISTANCE_FILE (the ephemeris file), the RTI file, the emergency config, and others.
- About 13 s after the last client stops: STOP (STOP_COMPLETED). The receiver follows START/STOP.
- Every request gets a Response (kSuccess). A fix goes to locationd as Indication PVTM with a `proto.gnss.Fix`:
  - Time: GPS ns and leap seconds, plus the receipt time in mach continuous time;
  - Position: WGS84 height and undulation, 1-sigma semi-major/minor and vertical uncertainty, reliability, 2D/3D;
  - Velocity: speed and course with their uncertainties.
- XPC dictionaries carry the keys `gpsdRequest` (in) and `gpsdResponse` / `gpsdIndication` (out), each a data
  object holding the protobuf.
- The assistance locationd injects (its time, its Wi-Fi position, the ephemeris) is taken and not used yet. It is
  what the chip needs for a fast first fix (gnss.md's "aiding").

## The daemon

```
gnssd [-L gpsd|sim] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS] [-c S]
gnssd parse [FILE]        # NMEA -> per epoch: fix type, satellites, DOP, accuracy (never the position)
gnssd segments [FWDIR]    # the ESW pieces the chip is given
```
`-c S`: the engine's search statistics every S seconds while no satellite is in view (default 120; 0 never), below.

**(a) The chip** (`chip_bringup()` in gnssd, the protocol in `gnss_chip.c`). A thread of its own owns
`/dev/hakios-gnss` while the receiver is wanted. It runs gnss.md's best-known sequence (the d8957eb run):
1. Wait until 3 minutes after boot: hub log facilities set earlier upset the RPC channel. A Start pressed earlier
   waits; the status says `step` = "waiting for the hub (N s more)" and the app "Waiting for the hub...".
2. The AR table cut (`gc_ar_table_cut()`): peek 0x008300b0; if it holds the hub patch's table (`04 00 00 00 2a 00 00
   00`), RPC 0x05 `b0008300 0200 ffff` and a peek to check it. Skipped, with a log line, when the peek does not answer
   (no `shub_peek=1`), shows something else, or does not show the write. Already cut since the hub started counts as
   cut (`arTableCut`).
3. Pin mux RPC 0x34 `[17][1]` (the external LNA).
4. `enable_log_to_host_LOG_INFO_Overwrite`, then log facilities 0x0e, 0x16, 0x4f, 0x97, 2 s apart, once per gnssd run,
   never in a loop.
5. RPC 0x1C `01`.
6. The ESW: RESET, PASSTHROUGH, bri handshake, every block's sum and CRC-8 checked, GetOutOfLegacy.
7. `gc_engine_on()`: RESET (serial outside the count), then serials 1-5: `esw_ctrl:1,1,0` (retain), gpsd's
   CONFIG_SET **without EXT_OSC** (six items: 5 with its byte back to 6, 8, 0x8002, 0x8006, 7, 0x8001 logging on),
   EXT_LNA (sub-block 25 = 1), IDLE and ON, each confirmed by a STATE_REPORT.
8. `disable_indoor`, then `gnss_start`.

While no satellite is in view, every 120 s (`-c`): `gnss_stop`, 5 s for the engine's statistics table (it prints
detections, attempts and noise only when its measurement engine unloads), then `disable_indoor` and `gnss_start`
again. With a satellite in view it is left alone. When the receiver is no longer wanted it sends `gnss_stop`, waits
5 s for the last table, and closes the device. After a failed step it stays in error (no retry loop) until Pause.

The ESW comes from `/redwood/fw/gnss-esw-<N>-<ADDR>.bin`. `devices/star2lte/firmware/gnss`: `fetch-firmware.sh` cuts
the pieces from Broadcom's gpsd (your own copy, or the pinned public vendor tree) and checks them against SHA256SUMS.
`build.sh DEVICE=star2lte` installs them.

Steps the bring-up finds later (for example gpsd's CONFIG_SET with the 49.152 MHz oscillator, gnss.md) belong in
`gc_engine_on()` in `gnss_chip.c`.

**(b) The fixes.**
- The on-chip engine's NMEA arrives as hub log lines (RPC 0x19, `ONCHIP_NMEA[0][$GPGGA,...`). gnssd takes the
  sentence after the last prefix.
- GGA, RMC, GSA and GSV are parsed. The satellites (system, PRN, C/N0, elevation, azimuth, used) come from GSV and GSA.
- Accuracy is HDOP x 4 m and VDOP x 6 m until the engine's own estimate is decoded: `meif_pvt_decode()` for MEIF PVT,
  or `$PGLOR,..,FIX`.
- `-n FILE|FIFO|-` replaces the chip with a recorded stream (`tools/nmea-synth.py` makes one). With `-n` the chip is
  never touched.

**(c) locationd**: the gpsd service above (default), or `-L sim` (CLSimulationManager; fixes are flagged
`isSimulatedBySoftware`, and speed and course are dropped by the simulation).

**(d) Power.** The receiver runs while locationd has it STARTed, or from the GPS app's Start
(`com.hakios.gnssd.start`) to its Pause (`com.hakios.gnssd.pause`), or always with `-a`.

**(e) Status, for the GPS app.**
- File: `/var/mobile/Library/Caches/com.hakios.gnssd/status.plist`, binary plist, mode 0644, in a 0755 directory
  owned by mobile.
- Written once a second in every state, off included. Each write is a file of gnssd's own renamed over the old one,
  followed by the notification `com.hakios.gnssd.status`.
- Keys:
  - `state`: off, starting, loading_esw, engine_on, searching, fix, error;
  - `error`: only while in error;
  - `fixType`, `satsInView`, `satsUsed`;
  - `sats`: system, prn, cn0, used, and elevation/azimuth once known;
  - `hdop`/`vdop`/`pdop`, `hAccuracy`/`vAccuracy`;
  - `latitude`/`longitude`/`altitude`/`speed`/`course`, only with a 2D or 3D fix;
  - `utc`, `ttffSeconds`, `receiverOnSeconds`;
  - `updated`, the wall-clock date.
  - Diagnostics (all optional):
    - `step` (the bring-up step: "waiting for the hub (N s more)", "AR table cut", "ESW load", "CONFIG_SET", "IDLE",
      "searching", "statistics (gnss_stop)" ...) and `stepError` (why it failed: gnss_chip's last word);
    - `engineState`: the last MEIF STATE_REPORT (IDLE, ON, ...);
    - `eswLoaded`, `eswBlocks`, `arTableCut`;
    - `detections` / `attempts`: GPS, GLONASS, Galileo -> sums over the engine's statistics tables this session
      (`statsTables` of them);
    - `agcGps`, `agcGlonass` (AAGC dB from the AGC report), `noiseGps` / `noiseExpected` (measured GPS_GAL noise and
      the GLL table's);
    - `oscOffsetPpb`, `oscUncertaintyPpb` (`MEIF:OSC GetOscOffset`), `cntin` (`SetCntinMeas`, or "none (no reference
      clock)");
    - `searchSeconds` (since the first gnss_start);
    - `hubLog`: the last 30 relevant hub lines (bring-up, search tables, noise, oscillator, indoor logic, asserts,
      warnings), masked;
    - `sessionLog`: the path of the session log.
- Start and Pause are answered with a status write at once.

Positions go to locationd and to that file (the phone's own screen). They never go to the console or a log. Hub lines
reach the status and the session log only from a fixed list of kinds, never one that mentions Lat, Lon, LLA, Pos, NMEA
or `$G`, and after `mask_strict()`: every number with four or more decimals, then NMEA's latitude and longitude
fields. The statistics gnssd writes itself are rounded to three decimals.

**(f) The session log, for a field test with no computer.**
- Each receiver start writes `/var/mobile/Library/Caches/com.hakios.gnssd/session-YYYYMMDD-HHMMSS.log` (0644). The
  last 10 are kept. Each line is `HH:MM:SS +MM:SS text` (wall clock, time since the start):
  - a `#` header: date, who started it (the app, locationd), uptime, system build, gnssd's build, the sequence;
  - `step: NAME` / `step NAME FAILED: why`, `state: ...`, `engine: IDLE -> ON`;
  - `gnss_chip: ...` (what the library says: the AR cut, the bri version, the ESW blocks);
  - `hub: ...` (the relevant hub lines, as in `hubLog`);
  - every 10 s, `stats: state, engine, step, search s, N in view / M used, fix ND[, accuracy, hdop], AGC, noise,
    detections/attempts`, and when any satellite is seen, `sats: G12 34dB el45 az120 used; ...`;
  - `fix: 3D, ... accuracy ...`, `TTFF`, `fix lost`, `receiver off`, `session end: ...`.
- No position is ever written to it.
- The GPS app's Diagnostics shows the same numbers and the hub lines; **Copy log** puts the current (or newest)
  session log on the pasteboard, **Share log...** hands a copy to the share sheet (Messages, Mail, Save to Files). Over
  SSH: `ls -t /var/mobile/Library/Caches/com.hakios.gnssd/session-*.log`.

## Testing without the chip: do not poison locationd

Feeding a made-up position far from the phone (the Null Island run of 12 Nov, through CLSimulationManager) made
locationd take the local Wi-Fi access points for moved:
- every one became `notindb`, and `AlsWifi query ... out 0` stopped the queries to Apple;
- Wi-Fi positioning stayed dead across locationd restarts until its AP cache (`/var/root/Library/Caches/locationd/
  cache_encryptedB.db*`) was moved aside.

So:
- never feed a synthetic position to the live locationd;
- test the protocol with no fixes (`gnssd -n FIFO` with nothing written: locationd's requests show the path);
- check positions on the client side, or with `gnssd parse`;
- keep a copy of that cache before any experiment that could feed one.

## Installing (the image)

- `build.sh` builds `gnssd` (with `gnss_chip.c`, signed with `src/gnssd.entitlements`) and `libhakios.locationd.dylib`.
- It lists `/redwood/lib/libhakios.locationd.dylib locationd` in INJECT, installs the job `com.hakios.gnssd`, and
  takes the ESW from `devices/star2lte/firmware/gnss` with `DEVICE=star2lte`.
- `mkjobs.py` leaves Apple's gpsd out. The data-volume seed sets `UseNewGnssArchitecture`.
- `kern.exec_inject` holds at most 511 characters. With this line the default INJECT, Bluetooth included, uses about
  435.
