# GNSS on the Galaxy S9 / S9+ (BCM4775): bring-up notes

Status (12 Nov 2026, evening): **no fix yet.** The chip's ROM has been read (`shub_peek=1`, build 29). The RPC numbers
of the on-chip position engine's interfaces are known and the chip accepts them. `gnss_start` ran on the chip, which
answered by asking the *host* to start the engine (`$pglirm,...` in RPC 0x38). So the on-chip engine is driven through
`lhd`'s side of the protocol, and nothing here answers that yet. Location today still comes from Wi-Fi only. This file is
the trail towards real GNSS.

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

## What the ROM says (read on the phone, 12 Nov 2026)

`shub_peek=1` works. `gnsstool peek 0 40` gives the ROM's vector table (initial SP 0x0091fff8, reset 0x0001a7d1). The
whole ROM (0x0-0xFFFFF, 1 MiB) reads in a few minutes, in 4 KiB pieces of 256-byte reads. About one read in 200 goes
unanswered once and succeeds when tried again. The hub and the sensors kept running throughout. The dump is Broadcom's
code: it is kept with the analysis, outside the repository. What it gives:

- **RPC argument encoding** (the ROM's decode stream, vtable at 0x000fb9e0): a u8 is 1 byte, u16 and u32 are
  little-endian, and a buffer is its bytes as they are. A string is `[u16 n][n bytes]` followed by one u8. A read past the
  end sets an error flag and does not fault. The RPC id and the payload length are the transport's (`shub_rpc_put()`), as
  for RPC 0x03 (the ROM's version, three u32; its encoder is at 0x00022536).
- **Interface A** (decoder 0x00022ee5, object 0x0090e488):
  - **0x1C `GnssPvtDataStart(u8 on)`**.
  - 0x1D (u8, u8, u16, u16) goes to a stub that **asserts** in the patch (`patch_transport_mgr.cpp`, line 250). Never
    send it.
  - **0x1E: PVT data, `[u16 n][n bytes]`**.
- **Interface B** (decoder 0x00022d97, object 0x0090e494):
  - 0x0B, 0x0C and 0x0D take one u32 each.
  - **0x401 is `GnssPvtDebugRequest([u16 n][the string and its NUL][u8 0])`** (`gnss_start`, `gnss_stop`,
    `enable_log_to_host_...`).
- The context hub's own request interface (decoder 0x00023187, vtable 0x000fb7a8) is RPC 0x3D `[u16 n][bytes]`. The SSP
  traffic the kernel sends (0x20, answered in 0x21) is decoded somewhere else.
- **RPC 0x38 comes from the chip: a text command for the host** (`[u16 n][text][u8 0]`). Its encoder is in the ROM at
  0x0002352c (vtable 0x000fb540), called by the patch's `gnss_service.cpp` (near 0x00803b6c).

Tried on the phone (build 29, indoors) with `gnsstool`: `rpc 1c 01`, then `rpc 401` with
`enable_log_to_host_LOG_INFO_Overwrite`, then `rpc 401` with `gnss_start`. The chip acknowledged every RPC, and the hub and
the sensors were unaffected. Nothing came back for the first two. `gnss_start` was answered at once by one RPC 0x38,
`$pglirm,req_breadcrumb,pvtOnChip,periodSec,36000,batchOption,128`, and then nothing for two minutes.

So `BrcmGnssStart` on the chip does not start the receiver itself. It asks the host (`lhd` on Android) to start the
measurement engine and the on-chip PVT, in Broadcom's `$PGLIRM` command language, and `lhd` drives the engine.
`gnss_stop` and `GnssPvtDataStart(0)` were sent afterwards, and the sensors kept running.

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

1. **Find what the host is expected to do with `$pglirm,...`.** In `lhd` it goes to GLL's command parser, which then
   talks to the measurement engine (ME) through the MEIF messages: the `glmesrd` transactions, whose patch halves are
   `patch_glmesrd_transaction_rpc_{encoder,decoder}_{pike,carp}.cpp` and whose other halves are in the ROM. Now that the
   ROM is readable, the next step is to read the ROM's other generated RPC stubs (0x00022000-0x00023400, beside the ones
   above) for the ME's start and configuration RPCs. The ME's log strings (`LoadMeasEngine`, `MEIF_*`) give the order
   `lhd` sends them in.
2. **If the ME can be started from here**, the on-chip PVT's results come back as `ONCHIP_NMEA` log lines (log category
   22) or through the PVT interface, and `gnsstool listen` sees every RPC. This test needs the phone outdoors or at an open
   window: a cold start without assistance takes minutes.
3. **Feed iOS**: a small daemon that reads the fixes from `/dev/hakios-gnss` and hands them to locationd. The quickest
   route is `CLSimulationManager` (as `loctool sim` does). The proper route is locationd's NMEA input for accessories
   (iAP2 location). Only accuracy and satellite counts are ever logged, never the position.
4. If the ME needs `lhd`'s configuration (`gps.xml`: TCXO, LNA, frequency plan), that configuration has to be found or
   guessed. At that point the hosted mode (raw measurements and a position engine on the AP) becomes the alternative.
