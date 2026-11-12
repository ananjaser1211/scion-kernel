# gnssd: GNSS fixes for iOS's location stack

`gnssd` (`userland/src/gnssd.m`, the job `com.hakios.gnssd`) takes the BCM4775's fixes and hands them to locationd,
which passes them to every app: Maps, Safari's geolocation, Weather and anything else that uses CoreLocation. The chip
side, which brings the receiver up and gets the first fix, is in [gnss.md](gnss.md). This page covers the iOS side:
how a position gets into locationd, what apps see, and what the daemon expects from the chip.

Status (12 Nov 2026): the feed works end to end with a synthetic NMEA stream. The chip does not produce fixes yet, so
`chip_up()` / `chip_fd()` are the hand-off points for the GNSS bring-up (below).

## How a position can get into locationd (iOS 16.5, 20F66, read from the system image)

Four routes were checked in locationd, ExternalAccessory, CoreAccessories (accessoryd) and Apple's gpsd.

| Route | What locationd makes of it | Usable here? |
|---|---|---|
| **Built-in GNSS, new architecture**: Apple's `gpsd`, Mach service `com.apple.gpsd`, protobuf `proto.gpsd.Request` / `Response` / `Indication` (schema embedded in locationd: `GpsdProtocol.proto`, `GnssTypes.proto`) | the phone's own GPS: `CLLocationType` GPS, neither simulated nor accessory | **No**, see below |
| **Built-in GNSS, baseband**: `CLEurekaController` (Qualcomm QMI PDS through CommCenter) / `CLIceController` (Intel ARI) | the phone's own GPS | No: needs a Qualcomm/Intel baseband stack (CommCenter is not running here) and Apple's private PDS messages |
| **External accessory**: `CLAccessoryLocationProvider` <- `CLAccessoryObserver` <- ExternalAccessory (`EAAccessoryDidReceiveNMEASentenceNotification`, `-[EAAccessory getNMEASentence:]`) <- accessoryd's iAP2 LocationInformation (or iapd's iAP1 `IAPLocationCopyNMEAData`) | `isProducedByAccessory` | No: the accessory has to identify over iAP2, and its features (location among them) depend on MFi authentication (`-[ACCTransportIOAccessoryManager setFeaturesFromAuthStatus:authCert:certType:]`, AuthCP certificate). It can't be done without a real MFi coprocessor or a patched accessoryd |
| **Simulation**: `CLSimulationManager` (entitlement `com.apple.locationd.simulation`; Xcode's "simulate location", `loctool sim`) | `isSimulatedBySoftware` | **Yes**: gnssd's default |

Why the gpsd route is closed on this phone, even though it is the cleanest protocol:

- locationd only uses its new GNSS stack (`CLGnssProvider` -> `CLGnssController`) when `CLPlatformInfo` says so. The
  preference `UseNewGnssArchitecture` (in **mobile's** `com.apple.locationd` domain) turns it on for any device, and
  that was tried on the S9+: `CLGnssProvider,startLocation` and `GnssEvent clientStart/clientStop` follow the clients.
- The device behind the controller is chosen by `CLGnssDeviceFacade::create` (locationd 0x1011eb084), from the
  hardware model alone, with no preference:
  - `CLGnssDaemonDevice` (XPC to `com.apple.gpsd`) only for the hardware ids in a fixed set (0x1003b0104 / 0x1003affe0);
  - otherwise `CLEurekaController` (QMI) or `CLGnssUnknownDevice`.
- The phone runs as iPhone10,3 (D22), which is not in that set. A server on `com.apple.gpsd` (gnssd `-L gpsd`, tried
  with Apple's gpsd job swapped out) gets no connection at all, and Apple's own gpsd job has `runs = 0`.
- Using the protocol would take code injected into locationd to change that one choice. That is the kind of change
  this project avoids, so it is not done.

`gnssd -L gpsd` keeps the complete server anyway, for a later identity that has gpsd or a sanctioned override:

- what it handles: START / STOP (receiver on and off), BUILD_DEVICE (Status), every other request answered with
  kSuccess;
- what it sends: each fix as Indication(PVTM) with `proto.gnss.Fix` (GPS time, Position with the WGS84 height and
  undulation, 1-sigma horizontal and vertical uncertainty, Velocity);
- the XPC dictionary keys: `gpsdRequest` in, `gpsdResponse` / `gpsdIndication` out.

So **the feed is CLSimulationManager**: no Apple code is changed and no system file is patched. The cost is the flag.

## What apps see

Measured on the S9+ with `nullisland.nmea` (made by `tools/nmea-synth.py`: 0.001 N, 0.001 E, HDOP 0.9, 8 satellites,
1 kn east) and a CoreLocation client running as `com.apple.weather`:

- `horizontalAccuracy` 3.6 m (HDOP 0.9 x 4 m), `verticalAccuracy` 7.2 m, altitude 12 m (MSL), timestamp age 0 s, the
  position moving east as the stream does;
- `sourceInformation.isSimulatedBySoftware` = **1**, `isProducedByAccessory` = 0;
- speed and course arrive as -1: CLSimulationManager drops them, although gnssd sets them on the CLLocation;
- Maps, Safari and Weather get the same positions, because the simulation replaces locationd's own fix for every
  client.

An app that checks `isSimulatedBySoftware` (a few games and anti-fraud checks do) will refuse these fixes. Nothing in
the stock apps does.

**Hazard when testing.** A synthetic fix far from where the phone really is affects more than the test:

- After the Null Island run, locationd's Wi-Fi positioning no longer recognised the access points around the phone:
  `@WifiAps lookiter ... notindb`, then `AlsWifi query ... out 0`, with no new query to Apple's service.
- So once the simulation stopped, there was no Wi-Fi fix, even across a locationd restart.
- locationd treats a precise position as the truth and takes the local APs to have moved.
- Real fixes, at the phone's real position, do not do this.

To test the feed, use a synthetic stream briefly, and know that Wi-Fi location may need its cache rebuilt afterwards
(locationd's `cache_encryptedB.db`).

## The daemon

```
gnssd [-L sim|gpsd] [-f FWDIR] [-n NMEA] [-t] [-a] [-i MS]
gnssd parse [FILE]        # NMEA -> one line per epoch: fix quality, satellites, DOP, accuracy (never the position)
gnssd segments [FWDIR]    # the ESW segments chip_up() would load
```

- **(a) chip**: `chip_up()`, `chip_down()`, `chip_fd()`.
  - The ESW segments come from the firmware directory (`-f`, default `/redwood/fw`). `build.sh DEVICE=star2lte`
    installs `devices/star2lte/firmware/` there, flat, and that is where the plan in gnss.md puts the GNSS
    segments.
  - Names: `gnss-esw-<N>-<ADDR>.bin`, where N is the load order and ADDR the baseband address in hex. In gpsd's
    `a4775_B0_ImageSegments` they are `gnss-esw-0-5fdf0.bin`, `gnss-esw-1-78000.bin` and `gnss-esw-2-5fff0.bin` (the
    12-byte word). `tools/fetch-firmware.sh` should write them under those names.
  - Until the bring-up is final, `chip_up()` runs `/redwood/bin/gnsstool esw -r -x FILE@ADDR...` instead of
    repeating its code.
  - Without the segments it says so and leaves the receiver off: locationd keeps to Wi-Fi.
- **(b) fixes**:
  - NMEA 0183, with optional checksums. GGA gives the fix quality, satellites used, HDOP, the MSL altitude and the
    undulation. RMC gives the date, speed and course. GSA gives 2D/3D and the PDOP/HDOP/VDOP. GSV gives the
    satellites in view, over all talkers.
  - An epoch closes when the UTC time changes.
  - Accuracy is the engine's estimate when it gives one (`hacc` / `vacc` in `struct gnss_fix`), otherwise
    HDOP x 4 m (UERE) and VDOP x 6 m.
  - `meif_pvt_decode()` is the stub for MEIF PVT messages.
- **(c) feed**: one CLLocation per epoch to CLSimulationManager (`-i` thins them).
  - When the fix is lost for 10 s, or the receiver goes off, the simulation is stopped and cleared, so locationd
    goes back to Wi-Fi by itself.
  - `-L gpsd` uses the protocol server described above instead.
- **(d) power**:
  - With `-L gpsd`, locationd's START and STOP decide.
  - CLSimulationManager says nothing about who wants a position, and the location status bar state is not readable
    from outside: `CLGetStatusBarIconState()` stayed 0 with clients active.
  - So with `-L sim` the receiver is on while the screen is on (`com.apple.springboard.hasBlankedScreen`) and
    Location Services are on, and for 60 s after the screen goes dark. `-a` keeps it on for tests.

The job runs `gnssd` at load and restarts it if it fails. On SIGTERM it stops the simulation before it exits. It
logs fix quality, accuracy, satellites and speed only, never a position.

## For the GNSS bring-up: what to fill in

1. **`chip_up()`**: after ON, whatever makes the engine report positions (the on-chip PVT request, docs/gnss.md).
   Return true once the receiver is running. It may call gnsstool or, once the sequence is final, do it directly.
2. **`chip_fd()`**: return a descriptor that yields the engine's NMEA as lines (`$GPGGA`/`$GNGGA`, RMC, GSA, GSV,
   `\n`-terminated). For example, the `ONCHIP_NMEA` log lines from `/dev/hakios-gnss`, with the hub's prefix stripped.
   gnssd reads it non-blocking from a dispatch source. If the positions come as MEIF PVT instead, decode them in
   `meif_pvt_decode()` and call `fix_ready()`, setting `hacc` / `vacc` from the engine's own estimate.
3. **`chip_down()`**: `gnss_stop` / engine OFF / the ASIC's low-power state.
4. Name the segments `gnss-esw-<N>-<ADDR>.bin` in `devices/star2lte/firmware/gnss/` (fetch-firmware.sh).

To try a recorded stream without the chip: `gnssd -a -t -n FILE` (a file is replayed one epoch a second, `-` reads
stdin). To check a stream: `gnssd parse FILE`.
