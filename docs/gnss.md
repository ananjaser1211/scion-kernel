# GNSS on the Galaxy S9 / S9+ (BCM4775): bring-up notes

Status (12 Nov 2026): **no fix yet.** Static analysis done; the kernel has a raw link for user space
(`/dev/hakios-gnss`) and an opt-in memory reader in the hub firmware (`shub_peek=1`), not yet run on the phone.
Location today comes from Wi-Fi only, which is not accurate enough; this file is the trail towards real GNSS.

## What the chip is

The S9's GNSS receiver is the Broadcom BCM4775 B0, the same chip that carries the sensor hub (`devices/starlte/SENSORS.md`):
one SPI bus (`spi@13AC0000`, `BCM4773@0`), MCU_REQ / MCU_RESP / HOST_REQ / AP_SLEEP and NSTDBY (gpg0-2, the `gps`
node). There is no UART. `drivers/input/ssp/hakios.ssp.c` already powers it, loads the firmware through the ROM and talks
Samsung's sensor protocol over Broadcom's packet/RPC transport.

On Android, Samsung's kernel (`drivers/sensorhub/brcm/bbdpl/`) only moves bytes: `/dev/ttyBCM` is the raw SPI stream,
`/dev/bbd_sensor` / `bbd_shmd` the sensor RPCs (0x20/0x21), `/dev/bbd_control` text commands (`GPSD:CORE_ON`, `ESW:READY`
...), `/dev/bbd_patch` the hub firmware. The closed daemon `lhd` owns everything else: the transport, the firmware
download, and the GNSS side. Samsung's `bbd_rpc_lh.c` names only the first 34 RPCs, obfuscated (IRpcA..IRpcL, then
IRpcSensorRequest = 0x20, IRpcSensorResponse = 0x21); the GNSS RPCs are not named anywhere in GPL code.

## Where the GNSS engine runs: inside the hub firmware

The one firmware image (`bcm_sensorhub_a1`, 410700 bytes at 0x00800000, started at 0x00800161; Samsung's GPL
`bbd_patch_file_star.h`) is not only the sensor hub. Its strings and code (Thumb-2, Cortex-M; the rest of the code is
the chip's 1 MiB ROM at 0x00000000-0x000FFFFF, reached through 1325 `ldr pc` veneers) show:

- **the measurement engine and its host interface**: `GulpMeChinookEngine`, `GlMe_PeIfImpl` (`LoadMeasEngine`,
  `SendBatchedMeasurement`, `MEIF_PROPRIETARY_COMPACTMEAS`, `MEIF_MEASUREMENT_CONF_SB`, `MEIF_HOST_REQ_CONF_SB`,
  `MEIF_PLATFORM_SPECIFIC_CONFIG_SB`, `MEIF_ONCHIP_CONFIG_MSG`, `Config: TSCal(...) ExtOscReq(...) LnaConf ...`).
  With `lhd` as host, the chip sends **raw measurements** and `lhd`'s engine (Broadcom's GLL/glgps) computes the fix:
  the "hosted" mode.
- **an on-chip position engine**: `GulpMicroNav`, `GlOnChipReqMgr`, `patch_glpe_onchip_pex_decoder.cpp`,
  `OnChipNvMem` (it keeps ephemeris, position and oscillator offset), `Set GLONASS/GALILEO/BDS constellation`. It
  decodes the navigation data itself (`bNeedNewEphs`, `bNeedDecodeGlonass`, `RequestDataBits`), so it can fix with no
  assistance from the AP (cold start: minutes outdoors). Samsung uses it for batching and geofencing
  (`$pglirm,req_breadcrumb,pvtOnChip,periodSec,%d,batchOption,%d`, `$pglirm,stop,pvtOnChip`).
- **NMEA from the on-chip engine**: `$GPGGA`, `$GPRMC`, `$PGLOR,%d,FIX,%.1f`, `$PGLOR,%d,SPD,...`, written through the
  chip's log as `ONCHIP_NMEA[0][%s` (log category 22; the function that formats a fix is at 0x008088f6).

So a fix need not come from a position engine written for the AP: **the chip can compute it**. What is missing is how
to switch it on and get the result out.

### The switches, in the firmware (addresses are this build's)

| What | Where | Notes |
|---|---|---|
| `GnssPvtDebugRequest(const char *)` | 0x00807ef8 | string commands: `gnss_start`, `gnss_start_w_adr`, `gnss_stop`, `register_indoor`, `clear_indoor`, `enable_indoor`, `disable_indoor`, `delete_aiding`, `disable_log_to_host`, `enable_log_to_host_LOG_{DEBUG,INFO}_{Overwrite,Dont_Overwrite}`, two `BrcmGnssSetContextStatus_...`; anything else logs `COMMAND NOT RECOGNIZED` |
| `gnss_start` | 0x00807f38 | calls `BrcmGnssStart` (0x008028c4) with FixIntMs 1000, RptIntMs 1000, MaxFixCnt 10, AccMaskM 50, PwrMode 1, no position callback |
| `GnssPvtDataStart(bool)` | 0x00807e38 | true raises event 0x20, false 0x40; the GNSS task (`Flagtail`, 0x00803730) waits for 0x20: "Startup - Waiting for Host to connect" |
| PVT data from the host | 0x00807e94 | `(data, len)` queued to the PVT task; `"DC"` is special-cased |
| the two interfaces | objects at RAM 0x0090e488 / 0x0090e494, vtables 0x0082e690 / 0x0082e6d0 | built by 0x00807b88. Interface A: slot 3 `GnssPvtDataStart`, slot 5 PVT data; interface B: slot 6 `GnssPvtDebugRequest`. Slot 2 of each is ROM code (0x00022ee5, 0x00022d97), the RPC decoder; their base-class vtables are in ROM (0x000fb700, 0x000fb6cc) |

The two interfaces are RPC servers whose **decoding is in the ROM**: the RPC numbers and the argument encoding for
these calls cannot be read from the firmware file. Nor can any of `lhd`'s own RPCs; `lhd` was not found (below).

### The SSP command table

For reference (it is how the memory reader below is added): the hub's SSP dispatcher at 0x008263f2 walks a table of
85 `{u32 cmd, u32 handler}` at 0x008266d4 and calls `handler(msg, buffer, ...)`, where `msg` is
`[cmd][u16 length][u16 options][u32 data][buffer *]` (the AP's 9-byte header) and, for a READ, `buffer` was
allocated for `length` bytes and goes back to the AP. None of the 85 commands is about GNSS.

## What is on this machine (looked for, 12 Nov 2026)

- No `lhd`, `gpsd`, `gps.xml`, `lhd.conf` or GNSS ROM patch in any Samsung kernel tree, backup or firmware copy here.
- On the phone, `VENDOR`, `ODM`, `HIDDEN` and `CACHE` are empty ext4 file systems (only `lost+found`); `SYSTEM` is iOS.
  The stock vendor partition is gone, so `lhd` and its configuration cannot be had from the phone either.
- What exists is the hub firmware above (it is all the chip needs to run code) and Samsung's GPL kernels (Apollo,
  Note 10 Lite, S7 trees), which add nothing on the GNSS RPCs.

`lhd`'s configuration (`gps.xml`: TCXO, LNA, frequency plan, which constellations) matters to the hosted mode
(`MEIF_PLATFORM_SPECIFIC_CONFIG_SB`), and perhaps to the on-chip engine; whether the chip's defaults are enough is
one of the open questions.

## The tools (kernel and userland)

- `/dev/hakios-gnss` (made at `shub=6`, root only; `drivers/input/ssp/hakios.ssp.c`): every RPC from the hub that is
  not SSP arrives as an `'R'` record; user space can send any RPC (reliable or plain packet) and any SSP transaction.
- `shub_peek=1` (boot-arg): as the firmware is sent, SSP command 0xEC (`SH_MSG2SSP_GLASS_TYPE`, which nothing sends)
  is pointed at 22 bytes of our own code written over that command's log string: a READ of 0xEC with data = an address
  answers with the hub's memory there. Applied only when the replaced bytes are exactly the expected ones; the log says
  `shub: 4: shub_peek: the memory reader (SSP 0xEC) added (26 of 26 bytes as expected)`.
- `gnsstool` (`userland/src/gnsstool.c`, in `/redwood/bin` or built by hand):
  `listen [S]`, `rpc ID [HEX] [S]`, `prpc ...`, `ssp CMD OPTS ARG LEN [HEX]`, `peek ADDR [LEN]`,
  `dump ADDR LEN FILE [CHUNK]`.

## Next steps

1. **Boot a kernel with these commits and `shub_peek=1`** (with the usual `shub=6`). Check the log line above and
   that the sensors still run. Then on the phone:
   `gnsstool peek 0 40` (the ROM's vector table: initial SP, then Thumb addresses near 0x10c03),
   `gnsstool dump 0 100000 /var/root/rom.bin 256` (1 MiB). Keep `rom.bin` local: it is Broadcom's, never committed.
   If a read stops the hub (a fault), the sensors stop too until the next boot: dump in pieces.
2. **Read the ROM**: the RPC decoders at 0x00022d97 / 0x00022ee5 and the vtables at 0x000fb6cc / 0x000fb700 give the RPC
   numbers of `GnssPvtDataStart`, the PVT data call and `GnssPvtDebugRequest`, and how a bool and a string are encoded.
   Also find the log-to-host path (0x00012bcd, the firmware's log call) to know how `ONCHIP_NMEA` reaches the AP.
3. **Switch on the on-chip engine**: `gnsstool rpc <PvtDataStart> 01` (host connected), then the debug request
   `enable_log_to_host_LOG_INFO_Overwrite` and `gnss_start`, with `gnsstool listen` running; outdoors, wait for
   `ONCHIP_NMEA` / `$GPGGA` lines (several minutes for a cold start). Without the ROM, the fallback is a careful scan of
   RPC numbers above 0x21 (skipping 0x2E, start-application) with `gnsstool rpc`, watching for "Connected to Host" and new
   RPCs.
4. **Feed iOS**: a small daemon that reads the fixes (NMEA) from `/dev/hakios-gnss` and gives them to locationd. The
   quickest route is `CLSimulationManager` (as `loctool sim` does: `startLocationSimulation` with each new fix; the fix
   is flagged as simulated, which apps rarely check). The proper route is to look for locationd's own NMEA input for
   accessories (iAP2 location) later. Only the accuracy is ever logged, never the position.
5. If the on-chip engine turns out to need `lhd`'s configuration or is limited (batching only), the hosted mode is the
   long road: the measurement messages (`MEIF_PROPRIETARY_COMPACTMEAS`) decoded and a position engine on the AP.
