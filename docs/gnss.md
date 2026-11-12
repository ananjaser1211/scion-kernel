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
