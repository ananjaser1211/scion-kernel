# GNSS on the Galaxy S9 / S9+ (BCM4775): bring-up notes

Status (12 Nov 2026, evening): **no fix yet.** The chip's ROM has been read (`shub_peek=1`, build 29). The RPC numbers
of the on-chip position engine's interfaces are known and the chip accepts them. `gnss_start` ran on the chip, which
answered by asking the *host* to start the engine (`$pglirm,...` in RPC 0x38). So the on-chip engine is driven through
`lhd`'s side of the protocol, and nothing here answers that yet. Location today still comes from Wi-Fi only. This file is
the trail towards real GNSS.

Update (12 Nov, night): Android's `lhd` and `gpsd` were found in FloydROM and read (below). The chip runs the measurement
engine, and Broadcom's gpsd is the position engine. They talk MEIF, framed, through TxNav RPCs 0x1E/0x1F. The first MEIF
message sent from here, CONTROL(RESET), made the chip assert and hang the hub until a cold power cycle.

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

## Android's userland: lhd and gpsd (FloydROM, read 12 Nov 2026, night)

FloydROM (an S9 ROM, `FloydROM_T_Exynos9810_Build-22.zip`) carries Samsung's and Broadcom's GNSS userland under
`floyd/device/common/vendor/`: `bin/hw/lhd` (Broadcom "LHD2 ver. 108.20.545200"), `bin/hw/gpsd` (GLL, same build,
"StarFlagtailandroid"), `etc/gnss/gps.xml`, `etc/init/init.gps.rc`, and `vendor/etc/sensor/lhd.conf`. Both daemons are
stripped, but they keep a MiniDebugInfo section (`.gnu_debugdata`, xz) with every function's name: 1.2k symbols in lhd and
17k in gpsd. The binaries are analysed locally for interoperability only, and never committed. What they show:

**lhd is only a pipe.** `lhd.conf`: `LheBbdPacket=/dev/ttyBCM`, `LheBbdControl=/dev/bbd_control`, `LhePatch=/dev/bbd_patch`,
`LhePinMux=P17=M1` ("ExtLNA for GNSS"), `GpioNStdbyPath=/sys/class/sec/gps/GPS_PWR_EN/value`. lhd owns the transport
and the patch download (the same job `hakios.ssp.c` does). Towards gpsd it is a pair of named pipes
(`/data/system/gps/.pipe.gpsd_to_lhd` and its partner, plus `CtrlPipe`). IPC message 3 carries gpsd's bytes, and lhd
passes them on **unchanged**. For GNSS it uses these RPCs (lhd's own names):

| RPC | Direction | lhd's name | Encoding |
|---|---|---|---|
| 0x1C | to chip | `RpcTxNavigationEncoder::Start(bool)` | `[u8 on]`, reliable; sent when gpsd opens (`GnssTurnOn`) |
| 0x1D | to chip | `TxNavigation::BurstMode(bool,bool,u16,u16)` | never used by lhd; asserts in the patch |
| 0x1E | to chip | `TxNavigation::Data(u16, u8*)` | `[u16 n][n bytes]` of gpsd's stream, reliable, at most 1000 per RPC |
| 0x1F | from chip | `RpcRxNavigationDecoder` | `[u16 n][n bytes]` for gpsd |
| 0x38 | from chip | `RpcGnssRequestDecoder::GnssControl(char*)` | `[u16 n][text][u8]`: `$pglirm,...` is forwarded to gpsd, and lhd repeats it on a timer until gpsd acks (`GNSS_CONTROL_ACK`) |
| 0x34 | to chip | `RpcGlobalRequestEncoder::SetPinMuxing(u8 pin, u8 mode)` | sent once the hub is ready: `[17][1]` (LhePinMux P17=M1, the external LNA); `[7][2]` too, behind a flag, on ASIC 0x4775B0 |
| 0x18 | from chip | assert | `[u32 line][u32 arg2][u32 arg3][u32 arg4][u32 ts][u8 n][file]` |

`GPSD:CORE_ON` / `CORE_OFF` (to `/dev/bbd_control`) is only lhd telling Samsung's kernel driver that gpsd turned GNSS on
(`OnGnssTurnedOn`); the chip never sees it. The firmware's "GnssPvtDataStart" and "PVT data" are these TxNav RPCs: 0x1E's
bytes go into a ring buffer for the GNSS task (`Flagtail`), which deframes them as MEIF.

**gpsd is the position engine, and the measurement engine is on the chip.** gpsd's `GlPe_MeIfImpl` talks to the hub
firmware's `GlMe_PeIfImpl` (ME "Chinook", on the chip). gpsd's `SetRfType`, `SetRfMode`, `SetLnaMask` and the other RF
setters are empty stubs here: the chip configures its own RF. gps.xml's `FrqPlan`, `RfType` and LNA go only into a text
string (`lte_band_13:%d;...;lna_mask:%d;rf_mode:%d;gps_carrier_type:...`, `SetOnChipConfigParameters`). The host ME
driver that gpsd also contains (`GlMeSrd*`, Pike/Carp `EswRpc*`, ESW patch loader) is for other chips. The MEIF wire
format is:

- message: `[u16 id][u16 len][u16 serial][payload][u16 sum]`. `len` counts everything before the sum, and the sum adds
  those bytes. The serial counts up from 1 (u8 wrap). gpsd asserts that `len` is even.
- framing: `A5 93` + message, with every `A5` doubled, + `A5 68`. The hub's deframer (0x008112ac) matches.
- ids: 0 ACK, 1 NACK, 2 ERROR, 3 CONTROL, 4 STATE_REPORT, 5 EVENT_REPORT, 6 CONFIG_SET, 7 CONFIG_GET, 8 CONFIG_VALUES,
  9 AIDING, 0xA DATA_AIDING, 0xB MEASUREMENT, 0xC DATA, 0xD DEBUG, 0xE/0xF TEST, 0x10 SW_UPLOAD (also carries config
  strings, type 0x8000), 0x11-0x13 SENSOR, 0x14 ASIC_CFG, 0x15 ASIC, 0x16 PLATF_STATS; 0x8000 LOGGING, 0x8002
  PASS_THRU, 0x8003 DEBUG_CONFIG, 0x8004 DAC; 0x8100 GLPP2, 0x8101 COMPACTMEAS, 0x8102 TIME_TRANSFER, 0x8103
  GPS_TIME_REPORT; PEX (on-chip "MicroNav" PVT) 0x8180 AIDING, 0x8181 UTC_TIME_AIDING, 0x8182 LTO, 0x8183 CONFIG,
  0x8185 HOST_ME_HB, 0x8186/0x8187 TIME_INJECT, 0x818B INITIAL_POS, 0x818C HOST_MICRO_NAV_FIX_STATUS, 0x81A0
  TIME_REPORT, 0x81A1 STATUS_REPORT, 0x81D0 NVRAM.
- CONTROL is `[serial][u16 state]` (10 bytes in all): states 0 RESET, 1 SETUP, 2 OFF, 3 SLEEP, 4 IDLE, 5 ON,
  6 POWER_SAVE, 7 TEST, 8 SHUTDOWN, 9 ERROR, 10 PASSTHROUGH, 11 REQ_CURRENT. STATE_REPORT has the old state at byte 6
  and the new state at byte 8.
- gpsd's sequence: `TurnAsicOn` sends CONTROL(RESET). When the chip reports a state of OFF or below, gpsd sends CONFIG_GET,
  CONFIG_SET (a list of items built from GlSettings), the config string, and CONTROL(IDLE). When the chip reports IDLE and
  gpsd wants measurements, its first aiding (satellites, signal, or oscillator offset) is followed by CONTROL(ON). The chip
  then sends MEASUREMENT / COMPACTMEAS, and gpsd's Kalman filter computes the fix (`GlPeNavGnssKF`). The on-chip MicroNav
  is used for batching and geofences (`$pglirm,req_breadcrumb,pvtOnChip`), configured with PEX messages.

Tried on the phone (build 30, indoors): `gnsstool rpc 1c 01`, then `rpc 1e 0e00a593030008000100000000000c00a568`, which is
CONTROL(RESET), gpsd's first message, framed. Both RPCs were acknowledged. About 0.1 s later the chip sent eleven RPC 0x18
asserts over 22 s, at intervals growing from 0.56 s to 0.88 s, all `rpc_engine.cpp` line 222. That is the ROM's RPC
dispatcher (0x0001cb42), which asserts when a decoder that handled an RPC left bytes unread or read past the end. After
that the hub answered nothing (no SSP, no sensor logs), and a warm reboot did not bring it back: LDO37 stays on. Read from
the ROM, the 0x1E decoder (0x00022f5c) is GetU16 then GetBuffer(n), little-endian, and the hub's MEIF deframer matches
gpsd's framing. So the bytes sent were well formed. The likeliest reading is that RESET made the ME restart something
whose own RPCs then failed. That is not proven. Until it is understood, do not send CONTROL(RESET) again. The next probe
is CONTROL(REQ_CURRENT), `gnsstool meif 3 0b00`, which changes no state, with the chip's log enabled first.

**MEIF works** (build 31, after a cold power cycle of the chip). The sequence was `rpc 34 1101` (LNA pin mux),
`rpc 401` `enable_log_to_host_LOG_DEBUG_Overwrite`, `rpc 1c 01`, then `meif 3 0b00` (REQ_CURRENT). The chip answered at
once in RPC 0x1F with a framed STATE_REPORT (len 20, checksum good): **SETUP -> SETUP**, serial bytes `01 01` (our
serial, then the chip's own). It sent nothing else in 10 s, and no assert. The hub and the peek reader kept working.
So the path from the host to the engine is right, and the engine is already in SETUP without a RESET, which is where
gpsd sends CONFIG_GET / CONFIG_SET / the config string / CONTROL(IDLE). The RESET assert is still unexplained. Skipping
RESET is the working rule.

**CONFIG_GET** is gpsd's fixed request: `meif 7 07000100020003000500080009000a00` (count 7, then parameter ids 1, 2, 3, 5,
8, 9, 10). The chip answered with CONFIG_VALUES (len 152). Its items are `[u16 id][u16 size incl. header][data]`:
1 (64 bytes, mostly zero), 2 `e8030000 00000000` (1000), 3 `9f010000 01000000` + zeros, 5 `e803 ffffffff 0000 ffffffff`,
8 `0002 0100`, 9 `0032 0c00`, 10 `00000000 ffff 0000`.

gpsd's **CONFIG_SET** (id 6) is 92 bytes, or 100 with an optional 0x8004 item. The items, read from its builder:

| Item | Size | Data |
|---|---|---|
| 5 | 16 | `e803 0100 0003 06|07 00 0a00 0a00` |
| 10 | 12 | u32, u16 (fixed 8.8 of a value × 1e6), u16 |
| 8 | 8 | u16 oscillator uncertainty in ppm, 8.8 (`0002` = 2 ppm, the chip's default and gps.xml's 2PPM), `0100` |
| 0x8002 | 16 | `0100` + zeros |
| 0x8006 | 8 | u32 from settings |
| 7 | 8 | zeros |
| 0x8001 | 16 | u16, 0, u32 from settings, 0 |

The config string goes as SW_UPLOAD (id 0x10) type 0x8000.

**CONTROL(IDLE) from SETUP fails**: `meif 3 0400` got MEIF ERROR (id 2) with the text "GPS BB not responding -- ERR"
(`GlMe_PeIfImpl::CommunicationBreakdown`), then STATE_REPORT SETUP -> ERROR. The hub kept running. The baseband (the
GNSS core under the ME, "ChipData" to `GlMeImpl::SetAsicData`) never spoke. The ME is loaded only on RESET
(`handling MEIF_STATE_RESET : m_bIsMeasEngineLoaded`, `LoadMeasEngine`, "ChipData with unloaded ME"). So skipping RESET
cannot work: RESET is what starts the ME and the baseband, and it is RESET that asserted (`rpc_engine.cpp` 222). The
likeliest reading is that the baseband's own RPCs, on the chip's internal link to the ME, do not match the ME's decoder.
That would happen if the baseband runs its bare ROM where Android has loaded a patch. gpsd embeds a Pike baseband patch
("esw3_dev:4775_FF", `GetPatchSegmentPike`, `GlMeSrdAsicInitPatchLoader`); how it reaches the chip is the next thing to
read. Correction: the hub firmware carries its own Pike ESW (`esw3_dev:4775_B0@292538`) and the ME code that loads it
(`bri_wait4patchload`, `esw_wait4version`, `GlMeSrdAsicConfig::SetConfigParameters` with `esw_ctrl:` Load/Retain/
DynPwr options). gpsd's own copy is for its host-ME mode.

**RESET, second try (cold-started chip, build 31): no assert.** The order was `rpc 34 1101`,
`rpc 401 enable_log_to_host_LOG_DEBUG_Overwrite`, `rpc 1c 01`, `meif 3 0000`. The chip sent ACK (id 0), then STATE_REPORT
**RESET -> SETUP**. The hub stayed fine. (With two readers on `/dev/hakios-gnss`, each record goes to one of them only.)
The differences from the first try are the cold start of the chip, the LNA pin mux sent first, and the debug-log request.
Which of them mattered is not known. After that:
- CONTROL(IDLE) got **NACK** (`00 03`: reason 0, serial 3). The state stays SETUP.
- CONFIG_SET was sent as gpsd builds it, with the settings-derived fields set to 0 or to the chip's own values:
  `meif 6 0700 05001000e8030100000306000a000a00 0a000c0000000000ffff0000 0800080000020100 0280100001000000 0000000000000000
  0680080000000000 0700080000000000 018010000000000000000000000000000`, 92 bytes. It also got **NACK** (`00 05`).
- The hub's control handler (0x0081b214) NACKs ("MEIF:Trans in progress(%d->%d),control_msg(%d) rejected!") while a
  state transition is pending (`[ME+8]`, its target, non-zero). So the ME is most likely stuck in a transition out of
  SETUP, probably waiting for the baseband (ESW load). The chip's own log never reached the host. `enable_log_to_host` did
  not route the ME's log, which probably needs MEIF LOGGING / DEBUG_CONFIG (0x8000 / 0x8003, gpsd's
  `ConfigureMeLogging`).
- The config strings: the chip knows only `lte_band_13:` and `esw_ctrl:` (its strings). gpsd's
  `lna_mask/rf_mode/gps_carrier_type` string is the ONCHIP config message (id 0x17:
  `[u16 n][string][zero pad to 4]`), and SW_UPLOAD (0x10) is `[u16 type][u32 arg][data][1-4 zero bytes]`, type 0x8000
  for `lte_band_13:%d;%s`.

Offline, before the next try:
- **The ME's log** is a CONFIG_SET sub-block, 0x8001 (`MEIF_LOGGINGS_CONFIG_SB`). gpsd's `ConfigureMeLogging` sends
  exactly `[6][len 24][serial][count 1][01 80 10 00][u16 priority][0000][u32 facility mask][00000000]`; the chip logs it
  as "MEIF:Logging(%x, %lx)". The text then comes back as MEIF DEBUG (0xD) or LOGGING (0x8000), which gnsstool now prints
  with numbers of four or more decimals masked. The CONFIG_SET sent earlier carried 0x8001 with zeros, i.e. logging off.
- **The CONFIG_SET handler** (around 0x0081cbc0-0x0081d5xx) walks the sub-blocks, checks each length ("has wrong
  length"), and NACKs a bad one. The chip's sub-block names: CHANNEL, DATA, EXT_LNA, EXT_OSC, EXT_OSC_REQ, FAR_START,
  HOST_REQ, IO_PIN, LOGGINGS, LOW_SIG_MODE, MEASUREMENT, ME_OSC_MODEL, ME_OSC_PARAMS, MISC, ONE_PPS,
  PLATFORM_SPECIFIC, POWER_SAVE(_INTEGRATION_TIME), RF_PARAMS, RTC_OSC_PARAMS, SENSOR_MEASUREMENT, SIM_MODE,
  TIME_STAMP(_OSC_CAL), TX_BLANK, USER_DYNAMICS, BLANKING_MISC, COUNT_IN_OFFSET. Their numbers are not mapped yet.
- **`esw_ctrl:R,L,D;`** (0x008592ec): three flags, Retain ESW, Load ESW, ESW dynamic power control, separated by `,` and
  ended by `;`. The chip also knows `dc_ctrl:`, `dbg_ctrl:`, `lte_band_13:`, `forced_dc:`, `sniff_ctrl:`, `mi_ctrl:`,
  `asic_version_ctrl:`. gpsd parses the same keys for its host-ME mode, but its strings do not set `esw_ctrl` here, so on
  Android the chip's defaults apply.
- **Next sequence** (cold-started chip): `rpc 34 1101`; `rpc 1c 01`; logging on
  (`meif 6 0100 01801000 ff000000 ffffffff 00000000`); REQ_CURRENT; RESET; read the log and the state; then IDLE. If the log
  shows the ESW not loading, add `esw_ctrl:` by SW_UPLOAD 0x8000.

Build 32, cold boot. Sent pin mux and `1c 01`, then the logging CONFIG_SET: **ACK**, but no log text came. REQ_CURRENT:
SETUP. RESET: ACK, then RESET -> SETUP, no assert. REQ_CURRENT: SETUP. After that, the logging CONFIG_SET and IDLE were
both **NACKed**. The control handler (0x0081b214) explains at least the IDLE NACK:
- It acts on a CONTROL only when the pending target `[ME+8]` is -1 (none), or when the request is RESET.
- If the target equals the request, nothing is done.
- Otherwise it NACKs with "Trans in progress".
- **REQ_CURRENT itself sets the target to the current state** (0x0081b4b0: `target = current`).

So, once the ME is loaded, a REQ_CURRENT probe leaves target = SETUP behind, and every following CONTROL is refused.
Before RESET the ME is not loaded, and the target is cleared. gpsd never sends REQ_CURRENT in this phase. **Rule: after
RESET, do not probe with REQ_CURRENT. Read the state from the STATE_REPORTs the chip sends by itself.** The chip's log
did not come out by any route: not as MEIF DEBUG/LOGGING, not in the hub's log RPCs, and not as readable text in RAM
0x00900000-0x0091fd00, which was dumped with peek. CORE_ON in Samsung's `ssp_bbd.c` only sets `IsGpsWorking`. The only
supply Samsung's kernel switches is vdd_mcu_1p8 (LDO37); the baseband's power is the chip's own business.

Correction and further reading (restart at 21:05, build 32). The sequence was pin mux, `1c 01`, logging CONFIG_SET
(ACK), then RESET (ACK, RESET -> SETUP), then IDLE straight away **with no REQ_CURRENT: NACK all the same.** CONFIG_GET
is NACKed after RESET too, although it worked before RESET. So the REQ_CURRENT theory above is wrong. What the code says:
- gpsd sends REQ_CURRENT itself after a RESET -> SETUP report (its state-report jump table, 0x00154cb6: SETUP from
  RESET goes to `control(11)`). It sends CONFIG_GET / CONFIG_SET / the strings / IDLE only after the SETUP -> SETUP
  answer.
- The hub's packet gate (0x0081147e-0x008115d8) does four things:
  - It checks the MEIF serials: each one must be the last plus 1. The first message after a reset sets the start.
  - It lets RESET and REQ_CURRENT through at any time.
  - It treats a repeated serial as a retransmission.
  - While `[ME+8]` is non-zero, it refuses everything else ("SetPe2MeData IGNORED as ME is reseting").
- So after RESET the ME stays "resetting". It accepts only RESET and REQ_CURRENT. Most likely it is waiting for the
  baseband, as the pre-RESET IDLE reported in so many words ("GPS BB not responding").
- The GNSS core is opened by `AcquireGnssCore` (0x00809754: create MicroNav, `GnssCoreHal::Open`, register the
  core's data). Its caller is `PvtEngineCb::OnResource` (0x0080f384, resources 8 and 16), i.e. the on-chip PVT engine
  claims the core. `gnss_start` (RPC 0x401) before RESET, which got the usual `$pglirm` answer, did not change the
  NACK. `gnss_stop` was sent afterwards.
- No log from the chip by any route. RPC 0x2B `SetLoggingConfig([u8 on][u8 level][u32 facilities])`, which lhd sends
  after the transport syncs (facilities 0xffffdfff by default), was sent as `01 07 ffdfffff`. It was acknowledged, but
  nothing new came.

Where this leaves route (a): the ME and its MEIF are answered and understood, but the ME never gets its baseband. What
Android does that we do not is not known yet. The likeliest candidates are something in lhd's start that is not an RPC
we have seen, and the order of `gnss_start` / the 0x38 `$pglirm` handshake (lhd forwards `$pglirm` to gpsd and repeats
it until gpsd acks).

**lhd's bring-up of the hub, compared with `hakios.ssp.c`** (read from lhd's FirmwareDownloader and Tick):

| lhd | ours |
|---|---|
| NSTDBY (`GpioNStdbyPath` = GPS_PWR_EN = gpg0-2, the DT `gps` node, `sec_gps_bcm4753.c`) low, then high | same pin, same order; LDO37 cycled first on a cold start |
| autobaud, sync, `GetVersion` (0x00 -> 0x03); a ROM mismatch -> `Reset` 0x1A | sync and version the same |
| patch records (`MemoryWrite` 0x05; masked writes, CRC/checksum verifies, `ServiceStart` 0x37 records: none in this patch) | the same writes; the patch has only 0xF1 records |
| `<Type=Application>` -> `StartScratchApp` 0x2E (`ServiceStart` 0x37 only for other types) | 0x2E |
| `GetVersion` again (FD_WAIT4APP) | (version read at sync) |
| at "MCU Transport Synchronized": `SetLoggingConfig` 0x2B `[1][level][mask]` (mask 0xffffdfff by default) | not sent (sent by hand, no effect seen) |
| at LhReady: `SetPinMuxing` 0x34 `[17][1]` (ExternalLna, LheSettings+25, on by default); `[7][2]` only with `EnablePpsOut` | not sent at boot; sent by hand |
| `/dev/bbd_control`: `ESW:READY`, `GPSD:CORE_ON/OFF`, `BBD:PassThru`, `BBD:DEBUG`, `BBD:RESET_REQ`, `LHD:STOP` | Samsung's `ssp_bbd.c` only records them (`IsGpsWorking`, crash counts); nothing reaches the chip |
| gpsd open -> `Start` 0x1C `[1]`, then gpsd's MEIF through 0x1E | 0x1C by hand |

Samsung's sensor driver has no GNSS step of its own. The star2lte sensor HAL talks to the ssp driver only. No AP clock or
TCXO is switched for GNSS: the DT's only GNSS pin is gpg0-2, which we drive, and `SetGnssTcxoMode` 0x39 is never sent by lhd.
So lhd does nothing at bring-up that we skip, apart from the logging config and the pin mux.

One difference in the tests themselves stands out. The only RESET that got a reply from the baseband side (the
`rpc_engine.cpp` 222 assert, build 30, warm) was sent **without** the pin mux 0x34 `[17][1]`. Every attempt since then
sent the pin mux first, and the baseband stayed silent. Next test, on a cold chip: `1c 01`, **no pin mux**, logging
CONFIG_SET, RESET, then gpsd's order (wait for RESET -> SETUP, REQ_CURRENT, wait for SETUP -> SETUP, CONFIG_GET, ...),
with serials consecutive. If the assert comes back, the pin mux is what silences the baseband (or the warm start is
what made it talk). If the ME then accepts CONFIG_GET, carry on to CONFIG_SET and IDLE.

Done (TWRP restart at 21:15, cold chip), with **no pin mux**: `1c 01`, logging CONFIG_SET (ACK), RESET (ACK,
RESET -> SETUP), REQ_CURRENT (SETUP -> SETUP), CONFIG_GET -> **NACK**. It was the same with the pin mux, so the pin mux
is not the cause. Then the build-30 history was replayed in the same boot (`enable_log_to_host_LOG_INFO_Overwrite`,
`gnss_start` (with its `$pglirm` answer), `gnss_stop`, `1c 00`, `1c 01`) and RESET was sent again: ACK, RESET -> SETUP,
no assert, no baseband. **What is left of the build-30 difference is the warm chip**: that boot had no LDO37 power cycle
(shub_cold came with build 31), and the RESET there was the first MEIF message of the boot. Next: one boot with
`shub_cold=0`, then a bare `1c 01` + RESET as the first MEIF message. An assert means the baseband is alive on a warm
chip and the cold start (LDO37 off for 100 ms) leaves it dead.

**The build-30 assert is explained, and it was our own malformed RPC** (replayed at 21:19, fresh boot, RESET as the
first MEIF message). The hand-typed `rpc 1e 0e00a593030008000100000000000c00a568` is 18 bytes, not 16: after
`0300 0800 0100` come `0000 0000` (four bytes where the state takes two), and the framed bytes are 16 while their length
word says 14 (`0e00`). The ROM's 0x1E decoder takes `[u16 14][14 bytes]`, two bytes are left over, and the dispatcher
asserts (`rpc_engine.cpp` 222). The replay did exactly that again: ASSERT `rpc_engine.cpp` line 222, args 0 0 008f8c70,
**one** event (the same ts 05e27434 in all eleven copies, so the chip resends its one assert record), and then the hub
was dead until a restart. So the assert says nothing about the baseband. `gnsstool meif` builds the frame correctly
(len 8, 14 framed bytes), and its RESETs never asserted. Remaining fact: after a correct RESET the ME waits for good
(only RESET and REQ_CURRENT pass), and an IDLE before RESET reports "GPS BB not responding". The baseband never answers
the ME, and why is still open.

**Two breakthroughs (restart at 21:23).**

1. **Serials restart at 1 after RESET.** The hub's packet gate (0x0081147e-0x008115d8) NACKs every non-control
   message whose serial is not the last one plus 1. NACK `[u8 reason 0][u8 serial]` means a serial mismatch, not a
   pending transition. RESET resets the count: the first message after RESET must carry serial 1 (`meif 3 0000` with
   any serial, then `MEIF_SERIAL=1 meif 7 ...`). With that, CONFIG_GET after RESET is answered, the logging CONFIG_SET is
   ACKed, and **IDLE is accepted**. It still ends in "GPS BB not responding" -> ERROR, now about 10 s later.
2. **The chip's log works.** RPC 0x2B is `SetLoggingConfig([u8 on][u8 facility][u32 level mask])`, one facility per
   RPC. `rpc 2b 01XXffffffff` for every facility 0x00-0xff turns on everything. The lines come as RPC 0x19, and the
   kernel prints them as `shub: hub log: ...` (format `ffNN QtimeL text`). Some lines are base64-ish binary dumps (`&...W
   @^@...`); none showed a position, but filter them all the same. The log of RESET -> IDLE shows:
   - `LoadMeasEngine` -> `AcquireGnssCore` (MicroNav created, core opened, `PvtEngineCb:OnResourceRequest Resource(8)`),
     `SetRefClockParams(26000000, 2000, 59388)`, `SetPikeLnaMask(1)`, `SetPikeRfMode(3)`, `SetRfType(35)`, "ME Loaded".
   - `DEF_4755x:T IsPikeAsicType:T`, `GlMeSrdAsicinit::HW Flow Control Set`, **"SetHWAsicVerson: Received HW Version
     47752 B0 ASIC"**: the baseband answers the boot-ROM interface.
   - **"GlMeSrdAsicinit::Debbugger detected or run without loading is enabled. Exiting Legacy!"**, then "INIT_STAGE A:
     Requesting the Asic Version Number trial 1..4", "Resending Bri_GetOutOfLegacy", and finally **"esw_wait4version no
     version received from ESW, RetainEsw=0, LoadEsw=1... could be Overlay was not retained!"**.

So the ME skips the ESW (baseband firmware) download and leaves legacy mode as though the ESW were already in the
baseband's RAM, and the baseband then has nothing to run. In the patch's `bri_wait4asic_ready` (0x00857154-0x00857214),
the baseband's message 12 (`[0x110] == 0x95`) sets flag 16 ("debugger", bit 2 of the next byte set) or flag 17 (bit 2
clear). Either flag leads to "Exiting Legacy" without `bri_wait4patchload`. Flags are bit sets at `[obj+0xa4+8]`
(ROM 0x0003c200 set, 0x0003cda0 test). Next: read the ROM's own `bri_wait4asic_ready` (around ROM string 0x000b3fa0),
see what flag 17 means and what path loads the ESW (`bri_wait4patchload`), and whether `esw_ctrl:` (Retain, Load,
DynPwr) or the baseband's reported status bit decides it.

The ROM's `bri_wait4asic_ready` (0x000b3e24) and the patch's replacement compared: the ROM tests flag 16 or 17. Then,
**if flag 16 (debugger) is clear and the config allows loading** (`[obj+72]->vfunc(12)` and 0x000d50a0), it goes to the
patch-load state (`bri_wait4patchload`, 0x000b4059). Otherwise it exits legacy (0x000b625a, state 0x000b4401). **The
patch's version drops the load branch**: with flag 16 or 17 set, it always exits legacy (ROM 0x000b625b, state
0x000b4401) and never loads the ESW. So on this firmware the ESW must already be in place ("overlay retained") or come
some other way. The `esw_ctrl:` string goes in as SW_UPLOAD type 0x8000 (`meif 10 0080 00000000 <text> <pad>`, ACKed,
logged "MEIF:SwUp(0x8000 0)"). A first try with `lte_band_13:0;esw_ctrl:1,1,0;` before IDLE was cut short: the phone
dropped off the network during the IDLE that followed. All 256 log facilities were on, so the log flood is a suspect.
Next time, turn on only the facilities that matter.
(Correction: the 21:33 drop was a flash of build 33, not the log.)

Build 33 (the hub now gets AP_STATUS wakeup/resume, thermistor and mag PDC at boot):
- **Getting the ME's own log.** The GlMe/GlMeSrd lines need the MEIF logging sub-block (CONFIG_SET 0x8001) after
  RESET, plus RPC 0x2B for the hub's facilities. 0x2B alone gives only the PVT/core lines. Use **one reader** on
  `/dev/hakios-gnss`: two readers split the byte stream. Each `meif` command's own listen captures the log into a
  file.
- **`esw_ctrl:1,1,0;`** (via SW_UPLOAD 0x8000): the log shows it applied ("SetRetainEswEnabled Enabled",
  "SetLoadEswEnabled Enabled"). The result is the same: "Exiting Legacy", then "no version received from ESW,
  RetainEsw=1, LoadEsw=1".
- **gpsd's full CONFIG_SET** (7 sub-blocks), now with the right serials: ACKed. The sub-block ids, as named by the
  chip's log: 5 = MEASUREMENT_CONF, 10 = EXT_OSC_CONF, 8 = ME_OSC_PARAMS_CONF, 0x8002 = MISC_CONFIG, 0x8006 =
  PLATFORM_SPECIFIC_CONFIG, 7 = RF_PARAMS_CONF, 0x8001 = LOGGINGS_CONFIG. The chip's config dump: `MeOsc(0 512 1)`,
  `ExtOsc(0 255996)`, `Int 1000`, `Channel(24 24)`, `RfLoss 6`, `LnaConf 0x0`. IDLE: the same failure.
- **RPC 0x39 `SetGnssTcxoMode(1)`** before RESET: ACKed, no change.
- The patch's ESW segment table (`GetPatchSegmentPike` in the patch, 0x0084ab40): the count is BSS at 0x0086fd28
  (zero after the download), so the only segment it hands out is a 12-byte stub for baseband address 0x0005fff0. **The
  hub patch carries no ESW image.** The MCU does not load one (the load branch is gone), and the chip's SW_UPLOAD
  handler can take uploads ("MEIF:SwUp ERR(%lu, %lu, %lu)"), but gpsd sends only config strings. So the baseband must
  boot an ESW of its own when it leaves legacy (from its ROM, or an overlay that survives from somewhere else), and it
  does not answer after that.
- MiniMe banner seen: "Broadcom GLL ver. 68.19.21 282318, Jan 23 2017, pike:glmesrd_build.cpp".

**Where the ESW comes from on Android: gpsd's host measurement engine.** In `GlMeIf::Instance` (gpsd 0x002cc6b0), gpsd
asks `GlSettings::ShouldBeUsingOnChipMe()` (vtable +696):
- **true**: `InstancePe_MeIf` (MEIF to the chip's MiniMe), `SetUsingOnChipMe(1)`.
- **false**: the **host ME** (`GlMeIf::Instance(...)`: GlMeSrd/GlDineCtrl running in gpsd, with `meif_4771:1;` added to
  the config string), `SetUsingOnChipMe(0)`.

The choice is made per request set by `GlEngineImplStd::setOnChipMeSetting(GlSet32, bool)` ("ShouldBeUsingOnChipMe
%s", `GlReqSm::idle ShouldBeUsingOnChipMe`, `on_chip_positioning`). The on-chip ME serves the low-power requests
(breadcrumbs, geofences, `$pglirm ... pvtOnChip`). Ordinary navigation runs the ME in gpsd. The host ME reaches the
baseband **through the chip in PASSTHROUGH**:
- `GlMeMeifCodec::SendPassthroughCtrlMsg` sends the fixed frame `a593 0300 0800 0300 0a00 1800 a568` (CONTROL serial
  3, state 10 = PASSTHROUGH).
- `GlMeMeifCodec::EncodeData` then wraps the host ME's baseband bytes as MEIF **0x8002 PASS_THRU**:
  `[0x8002][len = n + 8, made even][serial][u16 n][n bytes][sum]`, at most 2048 bytes each.

That host ME runs `GlMeSrdAsicinit` itself, bri boot ROM interface included. It loads the ESW from gpsd's own copy:
`GetPatchSegmentPike("4775_B0")` -> `a4775_B0_ImageSegments`, `ui4775_B0_NumSegments = 2`:

| Baseband address | Size | Content |
|---|---|---|
| 0x5fdf0 | 528 bytes | segment 0 |
| 0x78000 | 25844 bytes | segment 1 |
| 0x5fff0 | 12 bytes | stub (the same one the hub patch hands out) |

The ESW is `esw3_dev:4775_B0@394878`, about 26 KB in gpsd's `.rodata` (0x1abff0 / 0x1ac200). It stays local, never
committed. With RetainEsw the overlay survives, and the chip's MiniMe can later run without loading it. That is the
"could be Overlay was not retained!" case we hit: nothing ever loaded it.

So a fix has two possible routes:
- **(1) Load the ESW ourselves through PASSTHROUGH.** Replicate `GlMeSrdAsicinit`'s bri sequence (startup, flow
  control, version, asic_ready, `bri_wait4patchload` / `bri_wait4patchblockload` with CRC, GetOutOfLegacy), using
  gpsd's two segments as MEIF 0x8002 packets. Then go back to the on-chip ME with `esw_ctrl:1,...` (retain) and RESET /
  IDLE / ON. The on-chip ME and MicroNav could then measure and fix.
- **(2) Give the hub patch's own segment table (0x0086fd28: count, table pointer, name) the ESW in hub RAM, and restore
  the load branch.** This needs patch surgery and about 26 KB of free hub RAM.

Route (1) needs the bri message formats. The ROM implements the same `GlMeSrdAsicinit` (0x000b3670 onwards), so they
can be read from either side.

**The bri (boot ROM interface) messages, first pass** (gpsd `GlMeSrdAsicinit::Bri_*`; `[obj+152]->[8]` is the ASIC
mode, with 2-3 for one layout and 1 for the other). A message is a short byte string sent with `Bri_SendAsicData`. It
starts `ff`/`fe` `00` `fd` `<cmd>`, carries its arguments, and ends in `fc`:
- GetOutOfLegacy: `ff 00 fd 05 55 aa 00 ff fc` (mode 2-3), or `fe 00 fd 00 00 55 aa 00 ff fc` (mode 1).
- RequestHwVersion: `ff 00 fd 09 00 00 00 c0 fc` (mode 2-3), or `ff 00 fd 00 00 fc fd 00` (mode 1, as stored).
- In gpsd's rodata next to them: `fe 01 fd 43 30 01 00 c0 01 00 80 80 ... fc`, `fe 00 fd 43 6c 00 00 c0 07 00 00 00 fc`,
  `ff 00 fd 09 6c 00 00 c0 fc`, `ff 00 fd 09 68 00 00 c0 fc`. These read like register writes (`fd 43 <addr LE>
  <data>`) and reads (`fd 09 <addr LE>`) at 0xc000xxxx.
- ESW block load (`Bri_EswLoadBlockAndRequestCrc`, at most 1020 bytes per block, data in chunks of up to 2048): a header
  `fe <n> fd 4c <addr bytes>` (mode 2-3; `fd 62 ... c0` in mode 1), the data, `fc`, then a CRC request. The CRC
  answer is checked in `bri_wait4patchblockload`.

Still to read before a phone test: the exact header byte order, the CRC algorithm and its request/answer, the replies
`bri_wait4*` expect (in `Bri_CheckForReceivedPacket_*`), the ASIC mode the 4775 uses, and how the bytes travel inside
PASS_THRU (raw, or framed by `GlMeSrdPacketManager`).

**The bri protocol, decoded** (gpsd `GlMeSrdAsicinit`, ASIC type 3 = Pike, the 4775's; the type must be 2 or 3, and
mode 1 is another layout that is not used here):
- **Transport.** CONTROL(PASSTHROUGH) (state 10) is accepted from SETUP or ERROR. The chip answers ACK, opens the GNSS core
  (`AcquireGnssCore`), and reports `-> PASSTHROUGH`. From then on, each MEIF 0x8002 PASS_THRU
  `[u16 n][n bytes]` (MEIF len = n + 8, plus one pad byte if odd) is handed to the baseband **raw**
  (`GlMeCallBacks::OnAsicData`). The baseband's bytes come back the same way in 0x8002 messages. The bri bytes
  are not wrapped by `GlMeSrdPacketManager`, which is the ESW's protocol. PASS_THRU messages are counted by the serial
  gate. CONTROL messages are not, so after a RESET the first PASS_THRU carries serial 1. One framed MEIF message can be
  split over several RPCs 0x1E (at most 998 bytes each).
- **Byte stuffing** only with software flow control (`E0` -> `E0 02`, XON -> `E0 03`, XOFF -> `E0 04`). The 4775 uses
  hardware flow control, so bytes go as they are.
- **Requests** `ff|fe <n> fd <cmd> <args> fc`. **Answers** start `fe 00 fd` (the parser syncs on `fe`) and are 9 or 13
  bytes:

| Step (gpsd state) | Sent | Answer |
|---|---|---|
| autobaud (`bri_startup_asic`) | 16 x `80` | none |
| HW flow control (`bri_wait4flowcontrol`) | `ff 00 fd 0f 01 33 0e 10 fc`, then `fe 01 fd 43 30 01 00 c0 01 00 80 80 80 80 80 80 fc` (write at 0xC0000130) | `fe 00 fd 0f 01 33 0e 10 fc` (echo) |
| SW flow control instead | `ff 00 fd 0f 02 a b c fc`, XON = (a&15)+b, XOFF = (a>>4)+c | echo |
| HW version (`bri_wait4version`) | read 0xC0000000: `ff 00 fd 09 00 00 00 c0 fc` | `fe 00 fd x9 <addr LE> <data LE> fc`; ours **0x3000000F** |
| debug register (`bri_wait4asic_ready`) | read 0xC0000A54: `ff 00 fd 09 54 0a 00 c0 fc` | bit 2 of the data = debugger. Clear (ours) + LoadEsw -> load |
| clock doubler on/off (optional) | `ff 00 fd 86 ff 00 aa 55 fc` / `ff 00 fd 06 ff 00 aa 55 fc` | 9 bytes, cmd 0x86 / 0x06 (gpsd goes on after a 500 ms timeout) |
| block write (`Bri_EswLoadBlockAndRequestCrc`) | `fe <words-1> fd 4c <addr LE> <data> fc` (data a multiple of 4; a ragged end is rewritten as the last whole word with `fe 00 fd 4c`) | none |
| block check | `fe 00 fd 4a <addr LE> <u16 size LE> fc` | `fe 00 fd xa <addr LE> <u24 byte sum LE> <crc8> fc` |
| GetOutOfLegacy | `ff 00 fd 05 55 aa 00 ff fc` | the ESW starts and speaks its packet protocol (`e0 ...`) |

- **Check**: the byte sum of the block (24 bits) and **CRC-8, polynomial 0x4D, MSB first, initial value 0, no final
  XOR** (`GlUtlCrc`, table at gpsd 0x1562a6). The address must match too.
- **Blocks**: 1020 bytes (the patch loader's size without retention), or 1016 when fewer than 4 bytes would be left.
  Segments in table order, `a4775_B0_ImageSegments` = {0x5fdf0, 528}, {0x78000, 25844}, then index 2 = the 12-byte word
  at 0x5fff0. Two variants of that word exist: gpsd 0x1abfc0 (`...00 00 00 01`, the same bytes seg0 already has there)
  and 0x1abfe4 (`00 00 00 00 00 00 00 03 00 00 00 01`, chosen by the patch loader's retention/jump flag). The local
  `stub_5fff0.bin` is the second one, and the load worked with it.

**Phone test (build 33, 22:00, indoors).** `gnsstool esw -r -x -l 4 seg0_5fdf0.bin@5fdf0 seg1_78000.bin@78000
stub_5fff0.bin@5fff0`:
- RESET -> SETUP, then PASSTHROUGH -> `RESET -> PASSTHROUGH`.
- Flow control answer `01 33 0e 10` (set), HW version 0x3000000F, debug register 0 (no debugger).
- **26384 bytes in 28 blocks, every sum and CRC matching, 1.3 s.**
- GetOutOfLegacy: the baseband sent 17 bytes `e0 00 00 00 07 10 02 01 84 03 30 00 00 0f a9 e0 01`, the ESW's first
  packet.

**Handover to the on-chip engine works.**
1. `meif 3 0000` (RESET).
2. `MEIF_SERIAL=1 meif 10` with `lte_band_13:0;esw_ctrl:1,1,0;` (retain).
3. `MEIF_SERIAL=2 meif 6` logging sub-block.
4. `MEIF_SERIAL=3 meif 3 0400` (IDLE).

The MiniMe went through bri again: the RESET put the baseband back in its boot ROM, but the RAM kept the overlay. Its
log shows "HW Flow Control Set", "Received HW Version 47752 B0", "Exiting Legacy", then **"INIT_STAGE A: DONE.
Version received: Asic 0x3000000F, IFC 132, Radio 0x03"**, then **STATE_REPORT SETUP -> IDLE**, followed by MEIF ASIC
(0x15) messages. The engine stayed in IDLE.

**ON, and what keeps the receiver off** (22:00-22:20, three boots).
- The whole sequence is repeatable on a cold chip: the ESW load (0.6-1.3 s), then RESET, esw_ctrl, IDLE, and **CONTROL(ON) -> STATE_REPORT IDLE -> ON**.
- While IDLE the baseband searched for about 60 s. Indoors and not yet at the window it made 64 GPS and 20 GLONASS
  attempts with 0 detections. The serial link to it had no errors (1135 packets received, 0 sequence or CRC errors).
- At ON, `GlOnChipReqMgr::ProcessPowerCycleLogic` reports "MeIfStateOn, MeEngLoaded, Asic Off". It sends the ASIC to
  standby (`AsicToStandby`), unloads the engine and destroys the MicroNav. The config dump shows `ReqType 0x0`: without a
  position request, the on-chip request manager does not keep the receiver on.
- `gnss_start` (RPC 0x401) with the engine ON only gets the usual `$pglirm,req_breadcrumb,pvtOnChip,periodSec,36000,
  batchOption,128`. On Android, gpsd answers that with its own request. **The request reaches the chip as MEIF 0x818E**
  (gpsd `GlPePexHostEncoder::SendReqInfo` / `SendGeofenceInfo`), len 28:
  `[u16 a][u16 b][u16 c][u16 d][u16 e][u16 h][u16 0][u32 g][u16 geofence count][u16 0]`, then 32 bytes per geofence.
  The hub decodes it (0x00823c14) into `GlOnChipReqMgr::UpdateHostRequest(a, b, c, d, e, now, g, h)` (0x00804a60):
  - a = request id. 0xFFFF, 0xFFFE, 0xFFFD and 0xFFFC are commands (clear, stop, dump, ...).
  - c = flags. Bit 0x800 means geofence, bit 0x80 means ignored here. Otherwise it is a batching request:
    `[+0]=1, id, b, flags, d (0 -> 3), e, g, h`.
  - The fields' meanings (period, count, accuracy) are not settled yet. The next test is a minimal batching request
    (a=1, b=1, the rest 0) after ON.
- Hazards seen:
  - **RPC 0x401 typed by hand without its trailing u8 asserts the chip** (`rpc_engine.cpp` 222) and wedges the hub until
    a restart. `gnsstool pvt TEXT` now builds it.
  - **Turning on the hub log facilities with RPC 0x2B, many in a row**, once about 90 s after boot and once at about
    7 min, was followed both times by the reliable-RPC channel dying ("NOT acknowledged", no peek answers) while the
    sensors kept running. Turn on only a few facilities, and never all 256.
  - Without facilities, `enable_log_to_host_LOG_INFO_Overwrite` alone gives only the PVT task's own lines.

**The minimal host request does nothing visible** (22:25 boot, phone at a window).
- ESW, then RESET, esw_ctrl, IDLE, ON as before (`IDLE -> ON`).
- Then MEIF 0x818E with a=1, b=1 and the rest 0. It was acknowledged at the RPC level; no MEIF answer came, and nothing else came in 10 minutes.
- Turning on facilities 0x0E, 0x0C, 0x4B, 0x16, 0x4F and 0x97 one at a time, 2 s apart, kept the hub healthy, but no
  log line came either. The engine is probably idle with the ASIC off, so there is nothing to log.
- What gpsd really sends for an on-chip request (`GlPeReqBag::UpdateSensorHubConfig`, gpsd 0x3ac5b0):
  - the MicroNav config 0x8183, `[u16 0][u32 flags][u16][u16 0]`. The flags are 17 or 529 plus constellation and
    sensor bits.
  - the request 0x818E: a = request id, b = the breadcrumb period in s, c = the batch options, e = a constellation
    setting, h = request field +160, g = ms to the next batch point.
- **Next**: send 0x8183 before 0x818E with gpsd's flag values, and decode the hub's 0x8183 handler and
  `ProcessPowerCycleLogic` (around 0x00804e00-0x00805330) to find what turns the ASIC on. With logs, use only
  `pvt enable_log_to_host_LOG_INFO_Overwrite` and a few 0x2B facilities, 2 s apart.

**What turns the ASIC on** (hub patch, read offline after the 22:25 test).
- `GlOnChipReqMgr` (object at GlMe_PeIfImpl + 0x1cf0) keeps **3 request slots of 28 bytes at +0x610**: slot 0 batching
  (host), slot 1 geofence, slot 2 **on-chip** (`GetReportReqIds (B, G, O, GE)`). Slot layout: `[+0] active`, `[+2] id`,
  `[+4] period s`, `[+6] flags`, `[+8]`, `[+10]`, `[+12] base time`, `[+16] next deadline`, `[+20]`, `[+24]`.
- `ProcessPowerCycleLogic(this, timer, x)` (0x00804fc2) computes the time to the next update over the active slots
  (`GetTimeToNextUpdateMs`, 0x008051ac):
  - With no slot active, the result is 0x2FFFFFFF, which is the 805306367 seen in the log.
  - If the time is 0x2FFFFFFF, or larger than the warm-up `[+0x688]` (13000 ms, the second number in the log): **Asic
    Off**, and a timer is set.
  - Otherwise: **Asic On**.
  - A slot counts only if its base time `[+12]` is non-zero. The exception is the on-chip slot 2 and timer runs, where it
    counts as base + period × 1000 (or 600 s).
- **Slot 0 (batching, MEIF 0x818E)** takes its base time from the last-location time (`m_ulSystemTimeOfLastLms`,
  [+0x6ac]). If g is non-zero, it is now + g with a base of 0. On a receiver that has never fixed, the base stays 0, so
  **the 0x818E request can never turn the ASIC on**. That fits the silent 10 minutes.
- **Slot 2 (on-chip)** is set by `UpdateOnchipRequest(on, period)` (0x00804c2c): `[+0]=on`, `[+4]=period`,
  `[+16]=0`, then `ProcessPowerCycleLogic(1, 0)`. With an empty base, the deadline has already passed, the time to the
  next update is about 1 s (< 13 s), and the result is **Asic On**. This is the path to use.
- Its only caller is GlMe_PeIfImpl 0x00813c30 (`UpdateOnchipRequest(period != 0, period)`), reached from 0x00818de0
  (vtable 0x0082ec78). That one is called by a decoder at 0x008100b2, which reads **three u32** (period first) from an
  RPC or message stream, with log tag 15. Still to find: which RPC id or MEIF message reaches 0x008100b2. Candidates are
  the patch's own PVT RPC decoders next to it (0x0080fd5e-0x00810200). `BrcmGnssStart` (`gnss_start`) only sends the
  `$pglirm` text and never calls it, so on Android this "start on-chip PVT" call comes from the host.
- Next step, offline: identify the RPC or MEIF behind 0x008100b2. It may be a TxNav or PVT RPC that lhd sends on gpsd's
  behalf (lhd's encoders do not include it), or a MEIF/PEX message. Then send `period = 1` after ON on a cold chip.

- The function at 0x008100b2 is slot 14 of a patch RPC class. Its vtable starts at 0x0082e9e4; the slots are
  0x0086311d, 0x0086310b, 0x0080fc4d, ... 0x0081006f (slot 13), **0x008100b3 (slot 14)**, 0x0080fda5, ...
- Each slot is a transaction with a tag passed in r3: 1, 2, ... 14, 15 for the request, and 0x81, 0x82, ... 0x8e for
  the answer. That is an RPC *client* inside the hub. Slot 14 (tag 15) receives three u32s and passes them to
  0x00818de0 -> `UpdateOnchipRequest(period != 0, period)`.
- **Open question for the next agent**: who serves that RPC, and with which global RPC id (the transport id, not the
  per-class tag)?
  - It may be the host (lhd), the ROM's GNSS service, or the hub talking to itself.
  - The tag-to-id mapping is in the helpers 0x0080fee2 / 0x0080fef4 / 0x00818934. Read them, then look for the id in
    lhd's decoders (`lhd.ann`) and in the ROM's RPC tables (0x00022000-0x00023400).
  - If the hub expects the AP to answer with the period, gnsstool can answer it. Otherwise the call may have to be
    triggered from the PVT side; `GnssPvtDataStart` 0x1C and the PVT data RPC 0x1E's "DC" special case are the
    candidates.

**The open question, answered: `gnss_start` is the trigger** (read offline and checked with peek, 22:45).
- The vtable 0x0082e9e4 class is not an RPC client. It is a **call recorder** wrapped around the hub's ME engine, GulpMeChinookEngine, whose
  object sits at wrapper+20 with vtable 0x0082ec40. Each slot does three things: if tracing is on (0x00810176), it writes a record with
  the tag and the arguments (1..17 for a call, 0x81.. for its return; 0x00818934 / 0x00818ad8 build the record); it calls the real
  method; and it writes the return record. So tag 15 is a trace tag. No transport id is behind it, and nobody has to answer it.
- The wrapper is built by 0x0080fc1a (from 0x00808594, at core+0x1fcb8, returned by the core's vtable 0x0082e5a8 slot 4). Its
  pointer is also kept at `[0x008e78f8 + 0xb874]` (0x008f316c). A peek of that object shows vtable 0x0082e9e4.
- The caller of slot 14 is the GNSS task's **event 0x200 handler** (0x00805f20, entry 9 of the event table that 0x00806058 fills in at
  0x0090dfa4). It calls `[ctx+0xb874]->slot14(a, b, c)` with the three words at 0x0090e728.
- Those words are written by 0x00805ef4 `(period s, flags, constellation mask)`, which posts event 0x200 **only if they changed**. Its
  callers are `StartGnssRequest` (0x00803a30): period = FixIntMs/1000, flags 0x200008 or 0x204208; and the stop path (0x00803ae8):
  zeros. `BrcmGnssStart` (`gnss_start`) calls StartGnssRequest. So **`gnsstool pvt gnss_start` with the engine ON gives
  `UpdateOnchipRequest : Start 1` -> `Asic On`, then `LoadMeasEngine`**. Earlier attempts saw nothing because the engine was not ON
  then, or because the words already held the same values.

**ASIC on, first search** (22:50, the build-33 boot, phone at a window).
- `pvt gnss_start` after ON: Asic On, the ME loads, the baseband runs bri again ("Exiting Legacy", "INIT_STAGE C: Received
  Heartbeat"), and MicroNav starts (`HybridMode BatchMeasAndPvt`, `SIGAID(32)`: a full-sky search over GPS 1-32 with no aiding).
  The engine sends one `ONCHIP_NMEA` GGA/RMC/PGLOR per second, with no fix.
- **Indoor detection turns it off again**: after 16 s with no satellites, it logs "Indoor Detected", the request period becomes 600 s,
  and `Asic Off`. **`pvt disable_indoor`** stops that. The ASIC then stays on, with `ProcessPowerCycleLogic ... Asic On, 1000` every
  second. (`disable_indoor` re-evaluates the slots by itself. Slot 0 from the earlier 0x818E a=1 b=1 request now counts too, because
  loading the ME set the last-location time.)
- Result after about 12 minutes with the ASIC on: **Sats(0)**, "No sats@N", and no `Strong Sats Acquired`. The AGC report every 5 s
  gives GPS_GAL AAGC 8.00 dB (sometimes 11.52), GLO 15.96 to 18.46 dB, and BDS about 24 dB. The pin mux 0x34 `[17][1]` (external
  LNA), sent while the search ran, changed nothing.
- The chip's RF defaults match gps.xml: `SetRfType(35)` = GL_RF_4755_BRCM_EXT_LNA, LNA mask 1 (L1_EXT_ON). The one difference is
  `SetPikeRfMode(3)` = L1_LP where gps.xml has L1 (1). The chip's ONCHIP_CONFIG parser knows only `indoor_logic:` and `lte_filter:`.
- **The RF settings already match gps.xml.** The ME picks the RF mode from the baseband's version word (hub 0x008572b6:
  0x3000000F or 0x3300000F gives mode 1; 0x34/0x35/0x3700000F give 3). After INIT_STAGE A it logs "Update PikeRfMode(1)" (L1, as in
  gps.xml) and "Update PikeLnaMask(0x1)" (L1_EXT_ON). The `SetPikeRfMode(3)` at load is only the default before that. Sending
  ONCHIP_CONFIG (0x17) with `rf_mode` would do nothing, because the chip parses only `indoor_logic:` and `lte_filter:`.
- **The pin mux takes effect.** RPC 0x34 reaches the ROM's global request server (object 0x0091c548, server vtable 0x000fb0c0,
  slot 10 = 0x00021726). That calls 0x000120c6(pin, mode), which writes a 4-bit field taken from the table at 0x00012178. Pin 17 is
  bits 4-7 of 0x40410408 (pin 7 is bits 28-31 of 0x40410400). A peek after `rpc 34 1101` reads 0x40410408 = 0x00000010, so P17
  is in mode 1. Whether mode 1 really routes the baseband's LNA enable to the external LNA cannot be read in software.
- MicroNav also logs "PowerMode EnableLowPower T m_bPowerSaveIsOn T" (gps.xml's EnableLowPowerPmm) and "SetPeMeas 0 PeMeas, 0
  MeMeas" every second: no measurements at all.
- gnsstool now masks every text record (hub log lines included): numbers with four or more decimals, and the latitude and
  longitude fields of GGA/RMC/GLL/GNS. The engine's noise statistics are the one exception (they carry no position).
- Next: the same test outdoors or at an open window, on a cold chip, with the pin mux sent before the ESW load, and with
  `disable_indoor` before `gnss_start`. If there are still no detections, read the ME's detection table and noise statistics. The
  ME prints them when it unloads, so turn indoor detection back on and let the ASIC go off. That shows whether the RF path or the
  external LNA is dead. Aiding (time from the AP's clock, a coarse position) is the step after that.

**Open sky, cold chip** (TWRP restart at 23:09, phone outdoors, the same build 33).
- Sequence (local script): pin mux `rpc 34 1101` first, `pvt enable_log_to_host_LOG_INFO_Overwrite`, a few 0x2B facilities 2 s
  apart, `1c 01`, ESW load (28 blocks checked, 0.6 s), RESET, esw_ctrl, logging, IDLE, ON, `pvt disable_indoor`, `pvt gnss_start`.
  Everything was answered, and the ASIC was on from 23:14.
- **Log facilities:** the GlOnChipReqMgr / MicroNav / ME lines came only after facilities **0x4f and 0x97** (0x0e and 0x16 alone gave
  nothing). 0x0c floods the log with `CkgGetRuntimeInfo` once a second. Use 0x0e, 0x16, 0x4f and 0x97, 2 s apart.
- **External LNA config:** the MEIF CONFIG_SET sub-block **25 = MEIF_EXT_LNA_CONF_SB** (handler 0x0081d1d4: `[u16 25][u16 8][u16 0|1]
  [u16 0]`, which sets LnaConf and calls the engine's slot 0x44 with value == 1). gpsd's CONFIG_SET does not send it.
  `meif 6 01001900080001000000` was ACKed ("MEIF_EXT_LNA_CONF_SB"), followed by gnss_stop/gnss_start to reload the ME
  ("INIT_STAGE: COMPLETE"). It made no visible difference.
- **Result: no satellites.** The engine's own table when it unloads (printed at `gnss_stop` with indoor detection on):
  - First 7 minutes: GPS 0 detections in 204 attempts, 0 valid pseudoranges.
  - 6 more minutes with the external LNA flag set: 0 in 156.
  - GLONASS made 0 attempts: the request's constellation mask is GPS only.
  - The serial link to the baseband had no errors (2034 packets received).
- **The RF front end delivers normal noise:** measured noise GPS_GAL u 3.618 (the GLL table expects 3.825, -5.4%) and GLNS 3.502
  (3.581, -2.2%). Tracking DAGC: GPS 70.8 and GLNS 79.2. AGC report: GPS_GAL AAGC 17.54 dB, GLO 21.98, BDS 32.08 (on the earlier
  warm boot, indoors and with no pin mux before the load: GPS 8.00-11.52). So the ADC sees a noise floor about where the
  engine expects it, but no satellite rises above it.
- Likely causes, in order:
  1. Nothing from the antenna reaches the LNA: the external LNA unpowered or not enabled (P17 mode 1 is set, but whether it
     drives LNA_EN is not proven), or an RF switch or diplexer on the GNSS path that the AP must set.
  2. A reference-clock error larger than the search window (26 MHz, 2000 ppb uncertainty; gps.xml FrqPlan
     `FRQ_PLAN_26MHZ_2PPM_49_152MHZ_300PPB`).
  3. Something gpsd's host side gives the on-chip ME that we do not send (CONFIG_SET items with real values instead of zeros).
  Next: check the star2lte device tree and schematic for a GNSS LNA supply or enable and for an RF switch on the AP side (Samsung's
  `sec_gps_bcm4753.c` and the DT `gps` node only show gpg0-2). Compare with a stock Android trace if one can be had. Try a
  CONFIG_SET with gpsd's real settings values (MEASUREMENT 5, EXT_OSC 10, ME_OSC_PARAMS 8, RF_PARAMS 7 with RfLoss 5).

**Is the GNSS RF path tied to the AP or the CP?** (offline, Apollo DTs `exynos9810-star2lte_{kor,eur_open}_26.dts`, 23:35)
- The only GNSS node is `gps` (`samsung,exynos54xx-bcm4753`, pinctrl `ssp-gps-pwr-en` = gpg0-2, output, pull-down, drive 3). KOR and
  EUR differ only in the pinctrl phandle number.
- No `tcxo`, `clk_buf`, `rf_sw`, `ant_sel`, `vgps` or GNSS regulator node exists in either DT. The one LNA pin, `fm-lna-en`
  (gph1-3), belongs to the FM radio.
- Apollo's `drivers/sensorhub/brcm` (ssp_bbd.c, bbdpl) has nothing about an LNA or TCXO. `drivers/misc/modem_v1` never mentions
  GPS or GNSS. The 9810 PMU tables' GNSS entries (`CMGP2GNSS`, `MAILBOX_GNSS2CP`) are for the SoC's own GNSS block, which the S9
  does not use.
- `init.gps.rc`: `lhd` is `class main`, with no dependency on cbd or ril. So nothing in software ties the BCM4775's RF to the AP or
  the modem. The external LNA's supply and enable, and the 4775's 26 MHz TCXO, are on the board and driven by the 4775 itself (P17
  = LNA enable through the pin mux, as lhd does). Whether the TCXO is shared with the CP's RFIC (and so needs the CP up) cannot be
  seen in the DT. Only the schematic can settle it.
- No kernel GPIO, regulator or clock work follows from this.
- Still open for the next agent:
  - Read gpsd's CONFIG_SET builder for the real values of items 5 (MEASUREMENT), 10 (EXT_OSC: u32 + 8.8 ppm), 8 (ME_OSC_PARAMS)
    and 7 (RF_PARAMS: RfLoss 5 dB from gps.xml). Send them after RESET, before IDLE.
  - Try a wider oscillator uncertainty (item 8, 0x0500 = 5 ppm, or more) to rule out a reference offset beyond 2 ppm.
  - If neither helps, a schematic or hardware check of the GNSS antenna/LNA path is the remaining step.

**gpsd's real CONFIG_SET** (offline, 13 Nov; gpsd `GlPe_MeIfImpl::meif_config_set_msg` 0x47c528, `GlSettingsImpl` at
`[this+1616]`, `GlSettingsImpl::SetFrequencyPlan` 0x3873b8).
- gps.xml's `FRQ_PLAN_26MHZ_2PPM_49_152MHZ_300PPB` is enum 47 (name table at gpsd 0x787b60). Its jump-table case (0x387898)
  sets these fields:
  - +76 = 3 and +80 = 26000000 (the TCXO).
  - +88 = 2e-6 (TCXO uncertainty).
  - +96 = 49152000 (the external oscillator).
  - +104 = 3e-7 (its uncertainty).
  - +112 = 1 (an external oscillator is present).
- Nothing else in gps.xml reaches the CONFIG_SET. `PlatformSpecificFlag` (+32) and `CntinOffsetPpb` (+808) are not in
  gps.xml, so both are 0. With +808 at 0, the optional 0x8004 item is left out. `RfPathLossDb` does not go into any item.
- The builder's constants are at gpsd 0xe2b40 and 0xe2a68 (item 5 and the item 10 header), 0xe38a0 (item 8's tail and
  the 0x8002 header), and 0xe2998 (1e6).

| Item | Bytes gpsd sends (this gps.xml) | Meaning |
|---|---|---|
| 5 MEASUREMENT | `0500 1000 e803 0100 0003 0700 0a00 0a00` | fixed; the byte at +8 is 6 + (ext osc present) = **7** (the chip's default is 6) |
| 10 EXT_OSC | `0a00 0c00 0000ee02 4c00 0100` | u32 49152000 Hz, u16 8.8 ppm = 0.3 (76), u16 1 = present (the chip's default is `00000000 ffff 0000`, i.e. none) |
| 8 ME_OSC_PARAMS | `0800 0800 0002 0100` | u16 8.8 ppm = 2.0, s16 1 (the same as the chip's default) |
| 0x8002 MISC | `0280 1000 0100 0000 00000000 00000000` | fixed |
| 0x8006 PLATFORM_SPECIFIC | `0680 0800 00000000` | PlatformSpecificFlags = 0 |
| 7 RF_PARAMS | `0700 0800 00000000` | **always zero** in gpsd (written with `stp x12, xzr`). The chip stores the first u16 as RfLoss, whose default is 6 |
| 0x8001 LOGGINGS | `0180 1000 <u16 pri> 0000 <u32 mask> 00000000` | from the log settings |

So the earlier zeros were right for items 7 and 8. What differs is that gpsd tells the ME about a 49.152 MHz, 300 ppb
external oscillator, in item 10 and in item 5's flag. The hub stores item 10 (handler 0x0081ceae) as ExtOsc(freq, unc)
(`IsExtOscAvailable`, `GetExtOscFrqHz`, `GetExtOscUncPPB`). Item 8's ppm becomes `GetMeOscFrqUncPPB`, which is the 2000
in `GlDineCtrl::SetRefClockParams(26000000, 2000, ...)`. The 26 MHz TCXO frequency is not a CONFIG_SET value: the hub
patch hard-codes it when it sets up the ME (0x0081022e: literal 0x018cba80 to vfunc +248, then LNA mask 1, RF mode 3,
RF type 35). gps.xml agrees with it.

**Open sky with gpsd's real config, and a wider oscillator window** (TWRP restart at 23:44, phone outdoors and flat on
open sky, the same build).
- Sequence (local script): pin mux, `pvt enable_log_to_host_LOG_INFO_Overwrite`, facilities 0x0e/0x16/0x4f/0x97 2 s apart,
  `1c 01`, ESW load, RESET, then with serials 1-5: esw_ctrl, **gpsd's CONFIG_SET** (92 bytes, the table above, 0x8001 with
  logging on), EXT_LNA (25), IDLE, ON. Each phase is `pvt disable_indoor` + `pvt gnss_start` + listen + `pvt gnss_stop`.
  Before each later phase, a one-item CONFIG_SET (item 8, serial 6, 7, 8) changes the ME oscillator's uncertainty.
- Everything was ACKed. The chip took the values. `SetRefClockParams(26000000, 2000, 296)`: the third number is now the
  external oscillator's 296 ppb, where it was 59388 with the chip's defaults. At ON the ME calls `IsExtOscAvailable` and
  `MeasCNTIN`, then `SetCntinMeas(3, ...)`, i.e. it measures the TCXO against the external oscillator. The ME reloads at
  each `gnss_start`, and every phase logged the new value (`SetRefClockParams(26000000, 5000 / 10000 / 20000, 296)`).

| Phase | ME_OSC uncertainty | Time | GPS detections / attempts | Measured noise GPS_GAL (table 3.825) |
|---|---|---|---|---|
| 1 | 2 ppm (gpsd) | 60 s | 0 / 55 | (not printed) |
| 2 | 5 ppm | 60 s | 0 / 55 | 3.548 |
| 3 | 10 ppm | 120 s | 0 / 64 | 3.631 |
| 4 | 20 ppm | 120 s | 0 / 64 | 3.537 |

- AGC: GPS_GAL AAGC 21.62 dB, GLO 30.5-32.1, BDS 34.6, the same in every phase. `Sats(0)` throughout, and no valid
  pseudorange. With 10 and 20 ppm the attempts per second fall (64 in 120 s against 55 in 60 s), as a wider frequency
  search would make them. Strong open-sky signals would be found in a 20 ppm window in seconds. So **neither gpsd's
  CONFIG_SET nor a reference offset up to 20 ppm explains the zero.**
- Two more phases at 20 ppm, with the ME still ON. A `gnsstool` that leaves the oscillator lines unmasked was used.
  - 25 s: **1 detection in 49 attempts (2.04%)**, 0 cross-correlations, 0 valid pseudoranges. No satellite ID or C/N0 was
    logged for it.
  - 120 s straight after: 0 in 64 (noise 3.740).
  - A lone detection that never becomes a pseudorange, and does not come back, is most likely a false alarm at the
    detection threshold, not a satellite.
- The oscillator offset (`MEIF:OSC GetOscOffset(...)`, `SetCntinMeas(3, ...)`) is logged only at the first IDLE -> ON
  with the ME loaded. That was before the unmasking, so the value is not known. An IDLE -> ON cycle later, with the ME
  unloaded, did not log it again. With gnsstool 94a521a it shows on the next cold run. It is the TCXO's offset against
  the 49.152 MHz reference, in ppm, and it is the one remaining software reading of the reference clock.

**The stock device tree** (owner's backup `backup/2026-11-12-pre-hfs`, offline):
- `DTB.img` (and `DTBO.img`, the same hash) is **all zeros**, so the stock DTB partition was already blank.
- The star2 DTBs that do exist are inside `BOOT.img` (two copies, Scion's own boot image, built from Apollo) and
  `RECOVERY.img` (TWRP).
- All are "Samsung Star2 rev26", `model_info-hw_rev` 0x1a with `hw_rev_end` 0xff. One DTB covers every board revision
  from 26 up, so there is no later-revision variant to miss.
- TWRP's DTB has no `fm-lna-en` and no FM radio node, so it is not the KOR tree (it is EUR/global).
- Compared without phandles, the GNSS-related nodes are identical in TWRP's DTB and Apollo's `star2lte_kor_26`: `gps`
  (gpg0-2), `BCM4773@0` (SPI 25 MHz, mode 3, host-req gpa1-7, mcu-req/resp gph0-5/4, batch-wake gpa0-5, shub-int
  gpa4-0, cs gph0-3, vdd_mcu_1p8), and the `ssp-*` and `ssp-gps-pwr-en` pinctrl. The only difference is
  `ssp-mag-array` (the compass calibration).
- No board revision adds a GNSS LNA, TCXO, antenna switch or regulator.

**Where this leaves the zero** (no software lead left from this list):
- Ruled out in software: gpsd's CONFIG_SET (now sent as gpsd sends it), the external oscillator and the frequency plan
  as gps.xml gives them, a reference offset up to 20 ppm, the RF type, mode and LNA mask (the chip's own, = gps.xml),
  EXT_LNA_CONF, the P17 pin mux (set and read back), the device tree, and any AP or modem dependency (previous section).
- What only the hardware can tell:
  1. **Whether the antenna's signal reaches the 4775's RF input.** The noise floor and AGC say the receive chain makes
     normal *noise*. An LNA with nothing on its input makes the same noise floor, so they do not show that the antenna
     is connected. Check the GNSS antenna feed, its spring contact or coax, and any diplexer or matching parts between
     the antenna and the LNA. A phone that has been opened is the first suspect.
  2. **Whether the external LNA is powered and enabled.** The pin mux puts P17 in mode 1, as lhd does (0x40410408 =
     0x10), but whether P17 is the LNA's enable on this board, and whether the LNA has its supply, can only be measured.
  3. **The TCXO's real frequency and offset.** The engine assumes 26 MHz, hard-coded in the hub patch, and gps.xml
     agrees. A window of 20 ppm found nothing. A probe on the TCXO, or the CNTIN number above, settles it.
- Cheapest decisive test: **does GNSS work on this phone under stock Android?** One cold start outdoors with any GPS app.
  If it does not, the fault is hardware (1-3). If it does, the difference is still in software. Then capture lhd/gpsd's
  traffic there (lhd's log with `LogFacMask` widened) and compare it with ours message by message.

**Integration, when it works.**
- **Firmware**: `devices/star2lte/firmware/gnss/` holds a README and SHA256SUMS only. `tools/fetch-firmware.sh`
  extracts the three 4775_B0 segments from the user's own gpsd: `--from DIR`, or the pinned vendor tree if it has
  gpsd. It takes them by the `a4775_B0_ImageSegments` table (MiniDebugInfo symbols), or by hash-checked offsets.
  build.sh installs them in root/fw.
- **Loader and engine**: a **userland daemon** (a minimal lhd). It loads the ESW through PASSTHROUGH at start, switches
  to the on-chip ME with retain on, runs RESET/CONFIG/IDLE/ON, and feeds locationd. It talks to `/dev/hakios-gnss`
  only. Reasons not to put this in the kernel: the bri state machine and the ESW image are large and need retries and
  timers; the ESW file belongs in the userland firmware path; and a daemon can be restarted without a reboot. The
  kernel keeps the transport, the 0x1E/0x1F records and the 0x19 log routing, as now.

**What gpsd's host engine programs into the radio, compared with the MiniMe** (offline, 13 Nov; GPS worked on this
phone under Android, so the antenna and LNA hardware are taken as good).
- The radio is set up in INIT_STAGE B by `GlMeSrdAsicinit::ProgramRfFrequencyPlanPike` (gpsd 0x48d3f4; the ROM's copy
  is around 0x000b5050). It sends one ESW message: a 60-byte RF init block (`GlMeSrdRfInit4755::GetRfInit`, gpsd
  0x4bb0b4, ROM 0x000d51d2), then a list of analog radio (AR, type 0xe0) register writes. The ESW protocol is big-endian.
- The hub log prints every ME<->ESW packet in base64 (`@^@hs..`/`@^@sh..`). Decoded from the build-33 log, the MiniMe's
  INIT_STAGE B message is the RF init block plus **104 AR writes** (from the hub patch's table at 0x008300b0), and then
  a DR (type 0xc0) message from `patch_ProgramDrRegs`.
- gpsd, run in an emulator (unicorn, the vendor gpsd's own code) with this phone's inputs (radio 0x03 -> RF version 4,
  `GL_MULTI_CARR_RF_MODE_L1` = 1, LNA mask 1, external LNA), sends something different:

| | gpsd host engine (Android navigation) | the chip's MiniMe (ours) |
|---|---|---|
| RF init block, word 7 (+32) | **0x88eac856** (modes 1-4; 0x15dcbf8a only for mode 5 = L1_L2_L5) | **0x15dcbf8a**: the ROM (literal 0x000d561c) uses it for every mode 1-5 |
| RF init, the other 14 words | the same | the same |
| AR register writes | **none** for mode L1 (`ProgramArRegs` 0x4bb200 skips modes 1, 2 and 5; the ESW's own settings stay) | **104 writes** from the hub patch's table (0x008300b0, chosen by the patch's ProgramArRegs at 0x0085b44c for RF version 4, mode 1), plus 0xEF bit 7 for the external LNA |

- The AR tables have changed between builds: the ROM's (0x0000cde4) and the hub patch's (0x008300b0) differ in about
  20 registers (0x16, 0x28, 0x2f, 0x30, 0x32, 0x33, 0x40, 0x41, 0x50, 0x53, 0x58, 0x63, 0x68, 0x71, 0x93, 0x9c, 0xc0,
  0xea, 0xeb, 0xef, 0xf0). gpsd's newer code carries no table at all for RF version 4, so with the ESW that gpsd loads
  (`4775_B0@394878`, the one we load) the radio's analog settings are the ESW's own. The MiniMe (GLL 68.19.21 build
  282318, Jan 2017) was written for ESW `@292538` and overwrites them with its older table. The same kind of change
  shows in word 7 of the RF init block. **These two are the prime suspects**: on Android, ordinary navigation never lets
  the MiniMe program the radio.
- No GPIO, LNA-enable, antenna-select or "Radio 0x03" write was found on the host side beyond these: lhd's pin mux
  (P17 = M1, which we send) is the only pin setting, and gpsd's ESW messages for the radio are the two above.
- The AR table loader (ROM 0x000d5a60) stops at a register number 0xFFFF. The hub patch is in RAM, so **writing 0xFFFF
  over the first entry (0x008300b0) makes the MiniMe send no AR table**, only 0xEF |= 0x80, which is gpsd's behaviour
  apart from that one bit. RPC 0x05 `[u32 address][u16 n][data]` is the ROM's memory write (the patch download uses it);
  whether it is still served after the application starts is untested. `gnsstool peek 8300b0 8` checks it.
- Word 7 comes from a ROM literal, so it cannot be changed the same way. If the AR test alone does not help, the next
  step is to have the MiniMe's outgoing INIT_STAGE B message rewritten in the hub patch at download time (a kernel
  change), or to run INIT_STAGE B ourselves through PASSTHROUGH.

**The CNTIN reference (49.152 MHz) is missing.** At the first IDLE -> ON (build 33) the ESW reported
`gll_sysevt_rpc_frequency_captured: there is no reference cnt in clk detected` (hs_clk_cnt = 1). In gpsd that means
no CNTIN clock: the result is marked invalid (gpsd 0x4c0ae8), and the engine goes on with the TCXO alone. gps.xml's
"49.152 MHz 300 ppb" is the network-locked clock from the modem. gpsd asks for it through RIL
(`RildClientModuleControlPlaneSamsung::cntinRequest`), and the CP firmware (RADIO.img) has `[SIPC_0_GPS] GPS FREQ.AID
SET (clk enable=%d)` and the `LCS_FREQ_AID` machinery. Our CP never boots, so CNTIN is absent. That costs only the
fast frequency aiding. The first open-sky run, with the chip's defaults (no external oscillator, so no CNTIN), found
nothing either. So **sending gpsd's EXT_OSC item is wrong here**: it announces a reference that is not there. Leave it
out until the CP runs.

**The modem and the antenna** (RADIO.img, read for strings only): the CP drives its own antenna tuners and switches
(AIT, X-GND, ANT_SW0-7, MIPI RFFE), and makes a GNSS blanking signal (`DSPTX_Gnss_GeneratePaenSig`, `IRAT_MUX_GPS_SEL`).
None of its strings or tables mentions a GNSS antenna path, LNA or switch. Nothing found says the GNSS antenna needs the
CP. It cannot be ruled out offline, because a tuner on a shared antenna would be configured by RF data, not strings.
Booting the CP minimally (cbd-like loading of RADIO.img through the modem_v1 interface, plus the CP's NV/EFS) is a
large job, and identity data is involved. It is not the next step.

**Next test (phone, cold chip, indoors first and then open sky):** before the GNSS sequence, `gnsstool rpc 05
b0008300 0200 ffff` (address LE, n = 2, the 0xFFFF terminator), then `gnsstool peek 8300b0 8` to see it applied. Then
run the usual sequence with the CONFIG_SET **without EXT_OSC** (item 10) and with item 5's byte at +8 back to 6. Check in
the hub log that INIT_STAGE B is ACKed and that the decoded `hs Q` message has no AR list, then compare AGC/noise
indoors, and the detections outdoors. Record `MEIF:OSC GetOscOffset` / `SetCntinMeas` at the first IDLE -> ON (now
unmasked).

## The tools (kernel and userland)

- `/dev/hakios-gnss` (made at `shub=6`, root only; `drivers/input/ssp/hakios.ssp.c`): every RPC from the hub that is
  not SSP arrives as an `'R'` record; user space can send any RPC (reliable or plain packet) and any SSP transaction.
- `shub_peek=1` (boot-arg): as the firmware is sent, SSP command 0xEC (`SH_MSG2SSP_GLASS_TYPE`, which nothing sends)
  is pointed at 22 bytes of our own code written over that command's log string: a READ of 0xEC with data = an address
  answers with the hub's memory there. Applied only when the replaced bytes are exactly the expected ones; the log says
  `shub: 4: shub_peek: the memory reader (SSP 0xEC) added (26 of 26 bytes as expected)`.
- `gnsstool` (`userland/src/gnsstool.c`, in `/redwood/bin` or built by hand):
  `listen [S]`, `rpc ID [HEX] [S]`, `prpc ...`, `ssp CMD OPTS ARG LEN [HEX]`, `peek ADDR [LEN]`,
  `dump ADDR LEN FILE [CHUNK]`, `meif ID [HEX] [S]`, `esw [-r] [-n] [-2] [-x] [-l S] [FILE@ADDR ...]` (PASSTHROUGH,
  bri handshake, checked ESW load, GetOutOfLegacy), `bri HEX [S]` (raw PASS_THRU bytes).

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
