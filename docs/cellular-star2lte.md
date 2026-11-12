# Cellular on the Galaxy S9+ (star2lte): feasibility, plan, step 1

Phase 1, 13 November 2026. iOS 16.5 (20F66, the iPhone X's D22 userland) on an SM-G965N (Exynos 9810). The modem is
Samsung's Shannon, built into the SoC as the "CP". Today the CP never boots. CommCenter is in `userland/mkjobs.py`'s DENY
list, and Settings needs a patch (`userland/src/inject_tweaks.c`) because CommCenter is not running.

**Verdict.** Booting the CP looks feasible with no Android code: the kernel can read the phone's own RADIO partition, and
Exynos SiP SMCs already work from this kernel (MFC, Mali, camera and TEE all make them). Talking to the CP is a known
Samsung protocol (SIPC5 framing, IPC 4.x messages), but nobody has implemented it outside Samsung's binaries for this
generation. Running Apple's CommCenter on top of it is not realistic. The way to go is redwood's: the kernel owns the
modem and publishes `kern.hakios.cellular_*`, and the status bar reads them. A small CommCenter stand-in can come later,
to answer CoreTelephony. Step 1 (`shannon=1..3`, `drivers/modem/hakios.shannon.c`) is written and waits for a build.

## 1. How redwood does it (and what carries over)

Redwood (POCO X5 Pro, SM7325) has cellular, and **CommCenter is not involved**. Sources: `docs/cellular.md`,
`drivers/modem/hakios.mpss.c`, `hakios.nas.c`, `hakios.qmi.c`, `hakios.ipa.c`, `hakios.cell.c`,
`userland/src/inject_sb.c`, `userland/mkjobs.py`.

- **In the kernel.** `hakios.mpss.c` boots the Hexagon modem through TrustZone and serves RMTFS from UFS (writes are
  kept in RAM). It brings up SMP2P, GLINK and QRTR, then speaks QMI itself: DMS/UIM for the SIM, NAS for registration
  and signal, and WDS/WDA/DPM for data.
- **Exported state.** The modem's state goes to `sysctl kern.hakios.cellular_up`, `_registered`, `_bars`, `_dbm`,
  `_radio`, `_operator`, `_data`, `_ip`, `_gateway`, `_dns` and `_mtu` (`hakios.mpss.c` around line 5128).
- **The status bar.** `inject_sb.c` hooks SpringBoard's `SBStatusBarStateAggregator`
  (`_updateSignalStrengthItem:…`, the service string, and the signal view's mode). It shows bars and the operator from
  those sysctls and forces the item on, because without CommCenter it would stay hidden.
- **Data.** Data goes over `en1`, an Ethernet-looking interface. The kernel answers ARP and DHCP for it
  (`hakios.cell.c`), so configd treats it like a USB Ethernet adapter. `pdp_ip0` does not exist.
- **What happened when CommCenter was tried** (`mkjobs.py`, the DENY note). With no `baseband` node, launchd picks
  `com.apple.CommCenter-noncellular`. That variant answers `com.apple.commcenter.xpc` and `.coretelephony.xpc`, then
  crash-loops about every 15 s:
  - **SIGSEGV** after "Could not find AppleIPAppender service" (user client `ApplePDPHelperInterface`);
  - **SIGABRT** after "Unable to communicate with AppleBaseband driver" (`com.apple.driver.AppleBasebandPCI`).

  So even on the Qualcomm phone, the one that already speaks QMI, CommCenter was never brought up.

**What carries over to star2lte unchanged.** The sysctl interface and the SpringBoard hook do not care which chip
supplies the numbers. So does the `en1` ARP/DHCP shim in `hakios.cell.c`, once it is fed packets from somewhere other
than the IPA. Everything below `kern.hakios.cellular_*` is new: Shannon has no QMI, QRTR, GLINK or IPA.

## 2. What CommCenter expects on D22

From `sysimage/` (20F66, iPhone10,3) and the D22 device tree (`sysimage/DeviceTree.d22ap.im4p`, LZFSE-decoded):

- **The device tree.** `/device-tree/baseband` has `compatible = "baseband,n41"` and GPIO functions (`bb_on`, `bb_rst`,
  `radio_on`, `pmu_exton`, `coredump`, `reset_det`, `bb_ap_time_sync`). `function-pcie_port_control` points to
  `/arm-io/apcie/pci-bridge2/baseband-pcie`, so the modem is a PCIe device. There is also a `baseband-vol` fstab entry
  (`/private/var/wireless/baseband_data`). A `device-imei` property exists (its value not recorded here).
- **launchd.** `com.apple.CommCenter.plist` sets `_LimitLoadToDeviceTree = baseband` and `UserName = _wireless`, with 12
  Mach services. The main ones are `com.apple.commcenter.coretelephony.xpc`, `com.apple.commcenter.xpc`,
  `com.apple.basebandd.xpc` and `com.apple.commcenter.ari.rt.xpc`.
- **The modem family.** iPhone10,3 is the Qualcomm model. The dyld cache carries firmware paths for `Mav10/13/16/17`
  (Qualcomm) and `ICE17/18/19` (Intel), plus `Mav16`-specific settings (`setMav16MpssSleepVeto:`, `rfEnhLTEMav16`).
  `Mav16` is the iPhone X's MDM9655. The control path is QMI (`libBasebandCommandDriversQMI.dylib`,
  `libQMIParserDynamic.dylib`, `QmuxV/QmuxEv` in CommCenter) over `AppleBasebandPCI`. Data goes through `AppleIPAppender`
  / `ApplePDPHelperInterface`. ARI (`libARI*`, `BasebandTransportICE`) is the Intel path and does not apply to D22.
  Neither family's `.bbfw` firmware is in this image (`/usr/local/standalone/firmware/Baseband` is absent).

**Options:**

| Route | What it takes | Effort (honest) |
|---|---|---|
| (a) Real CommCenter with an emulated baseband | IOKit `AppleBasebandPCI` (user client, power, reset, coredump, firmware download: CommCenter/BBUpdater expect to flash and boot a Mav16 over PCIe), `AppleIPAppender` and `ApplePDPHelper` for data, a fake `baseband` ADT node, and a **QMI-to-Samsung-IPC translator** good enough for Apple's QMI clients (DMS, NAS, UIM, WDS, WMS, VOICE, plus Apple's private services). Then activation: `mobileactivationd`, carrier bundles, ABM. | 6-12+ months, with a real chance of a dead end (activation, firmware-version checks, BBUpdater). Not recommended. |
| (b) No CommCenter; our own `telephonyd` serves `com.apple.commcenter.coretelephony.xpc` | A launchd job with that Mach service and an NSXPC listener for CoreTelephony's `CTXPCServiceInterface`, answering from the kernel's sysctls: SIM state, registration, operator, signal, data-connection status, "not supported" for the rest. The status bar keeps using `inject_sb.c`. | Stub that stops the lookup storms and answers "no SIM" / "SIM ready": 1-2 weeks. Real values: +2-4 weeks. SMS/calls through it: months, each its own protocol surface. |
| (c) Partial: bars and operator only | What redwood has: kernel sysctls plus `inject_sb.c`. Nothing for CoreTelephony clients. | Free on the iOS side once the kernel publishes `kern.hakios.cellular_*` |

**Recommendation:** (c) first, since it costs nothing beyond the kernel work, then (b). The hard, long part on this phone
is the kernel's Shannon driver, not iOS.

## 3. Booting the CP on the Exynos 9810: the evidence

Sources: Apollo (`/home/aj1211/Kernels/Apollo`). Samsung's closed `cbd` daemon drives the kernel through
`/dev/umts_boot0`, so the userland half of the sequence is inferred from the ioctls' kernel side.

### Memory

- **Device tree.** The KOR device tree (`exynos9810-star2lte_kor_26.dtb.reverse.dts`) reserves `modem_if` at
  `0xF0000000`, size `0x9200000`, and `cp_ram_logging` at `0xF9200000`, size 32 MB. Our loader already keeps
  `0xF0000000-0xFB1FFFFF` out (`socs/exynos9810/include/hakios.exynos9810_mem.h`).
- **`shmem@F6E00000`** (`samsung,exynos-shm_ipc`) gives `cp_size 0x7900000`, `vss_size 0x700000`,
  `ipc_offset 0x8000000`, `ipc_size 0x500000`, `zmb_offset 0x8500000` and `zmb_size 0xb00000`.
- **The CP's own memory map**, read from RADIO's BOOT image at `+0xa0` ("MEM0"), agrees: DRAM `0x7900000`, VSS at
  `+0x7900000` (`0x700000`), IPC at `+0x8000000` (`0x500000`), ZMC at `+0x8500000` (`0xb00000`), VPA at `+0x9000000`
  (`0x200000`). That ends exactly at `0x9200000`. Apollo's `shm_ipc.c` reads this table at **base + 0x200 + 0xa0**, with
  the comment "0x200: TOC". That is a warm-reboot layout check. cbd itself loads BOOT at +0 and never sends the TOC
  (see "What cbd does" below), so on a cold boot that probe finds nothing and the device tree's sizes apply.

### RADIO (`backup/2026-11-12-pre-hfs/RADIO.img`, headers only)

| Entry | File offset | CP address (+ offset into the region) | Size |
|---|---|---|---|
| TOC | 0x0 | — | 0x200 |
| BOOT | 0x200 | 0x40000000 (+0) | 0x1e40 |
| MAIN | 0x2040 | 0x40010000 (+0x10000) | 0x24075c0 (about 36 MB) |
| VSS | 0x2409600 | 0x47900000 (+0x7900000) | 0x57cbf0 |
| NV | — (from EFS) | 0x44900000 (+0x4900000) | 0x100000 |
| OFFSET | 0x7aa00 | — | 0x85600 |

The CP sees `0x40000000` where the AP sees `0xF0000000`. **NV is not in RADIO.** `cbd` loads it from
`/efs/nv_data.bin`, which holds identity data. Its contents must never be logged or committed (see §6).

### The sequence

From `link_device_shmem.c`, `modem_ctrl_ss310ap.c`, `pmu-cp.c`, `cal-if/pmucal_cp.c` and
`exynos9810/pmucal_cp_exynos9810.h`. The defconfig has `CONFIG_CP_SECURE_BOOT=y`, `CONFIG_CP_PMUCAL=y`,
`CONFIG_UMTS_MODEM_SS310AP=y`, `CONFIG_LINK_DEVICE_SHMEM=y` and `CONFIG_MCU_IPC=y`.

1. **`IOCTL_MODEM_XMIT_BOOT` (`shmem_xmit_boot`).** Copies an image to `boot_base + m_offset`. The boot region is
   `0xF0000000..+0x8000000`.
2. **`IOCTL_SECURITY_REQ` (`shmem_security_request`).** On the 9810, if `cal_cp_status()` is not 5, it first runs
   `cal_cp_init`:
   - set CP_CTRL_NS bit 1 and CP_CTRL_S bit 3;
   - wait 1 ms;
   - wait for `RESET_SEQUENCER_STATUS` (PMU `0x14060504`) bits [2:0] to read 5;
   - wait for the GIC pending bit `0x10101238` bit 28.

   Then it calls `SMC 0x82001011(0)` (security-engine clock on), `SMC 0x82000700(mode | 0xF0000000, bl_size,
   main_size)` and `SMC 0x82001011(1)`. Mode is 0 for a normal boot, 1 for a dump and 2 for a re-init. EL3 checks
   Samsung's signatures and closes the region to the normal world. The return codes are named in `smc_err_string`
   (`CP_WRONG_BL_SIZE`, `CP_NOT_VALID_MAGIC`, `CP_MEM_IN_SECURE_DRAM` and so on).
3. **CP_CTRL_NS / CP_CTRL_S are not plain registers.** They can only be reached through
   `SMC 0x82000700(READ_CTRL=3, half, reg)` and `(WRITE_CTRL=4, value, reg)`, with `reg` 0 = S and 1 = NS. A read
   returns its 16 bits in the answer's top half.
4. **`IOCTL_MODEM_ON` (`ss310ap_on`).** Clears the 64 mailbox words, then writes the AP status word (mailbox 2:
   `sys_rev`<<6, `ds_det`<<14, `pda_active` bit 5). Then **AP2CP_CFG (`0x14820000`) = 1** ("CP_INIT, AP_BAAW setting is
   executed in el3_mon when coldboot … CP_CPU will work right now"). It then sleeps 300 ms.
5. **`IOCTL_MODEM_BOOT_ON`.** Polls the CP status (mailbox 3, bits [4:1]) until it is non-zero (100 tries).
6. **The CP signals the AP** through the mailbox (`mcu_ipc@14170000`, SPI 0x45): message word 1, valid bit 0x80,
   command bit 0x40, command in bits [5:0]. It sends `CMD_PHONE_START` (8) and `CMD_INIT_START` (1). The AP builds the
   SBD rings in the IPC region (`init_sbd_link`) and answers `PIF_INIT_DONE` (0x0D). Once rild has opened `umts_ipc0`
   and `umts_rfs0`, the AP answers `INIT_END` (2).

Everything the kernel needs is MMIO (PMU, mailbox, AP2CP_CFG), memory it already keeps out, and SiP SMCs. None of it
depends on Linux. **Unknowns that only the phone can settle:**

- the boot-loader size is settled by cbd: BOOT's TOC size (0x1e40), with BOOT at +0 (see below);
- whether S-Boot leaves anything in the region (`shannon_peek=1`);
- the `sys_rev` value Samsung reads from eight GPIOs (`shannon_sysrev=`);
- whether the CP boots with an empty NV (`CMD_NV_REBUILDING` exists for that case).

### What cbd does, instruction by instruction (FloydROM T, `vendor/bin/cbd`)

The kernel side above is half of the story; the other half is Samsung's closed boot daemon. These notes come from an
annotated disassembly of `vendor/bin/cbd` (FloydROM_T_Exynos9810_Build-22, BuildID 23d6c677…). The binary and the
disassembly stay outside the repository, in `~/scion/cell-work`. The function names are the `__func__` strings each
function logs with.

**The service** (`vendor/etc/init/init.baseband.rc`):
`cbd -d -tss310 -bm -mm -P platform/11120000.ufs/by-name/RADIO -n /mnt/vendor/efs`, as root, `seclabel u:r:cbd:s0`.
- `-tss310` selects the modem type.
- `-bm -mm` select SHMEM as both the boot link and the main link.
- `-P` is the RADIO partition, also symlinked to `/dev/mbin0`.
- `-n` is the NV directory.
- The same rc writes `ro.multisim.simslotcount` to `modem_ctrl_ss310ap/parameters/ds_detect`. A single SIM gives
  `ds_det` 0 in the AP status word.

**`start_shannon310_boot`, in order:**

1. **Waits for NV to be checked.** It polls `vendor.cbd.rfs_check_done` with `usleep`, then logs "NV validation done"
   or "TIMEOUT". The RFS side of the RIL (libsec-ril) sets that property after it has checked
   `nv_data.bin` against its `.bak` and md5 copies. cbd has no RFS server of its own: the strings for `umts_rfs0`
   are in the RIL.
2. **`prepare_boot_args` ("Prepare arguments, read TOC, check NV").**
   - Opens RADIO, reads 0x200 bytes and checks for "TOC".
   - Takes the number of stages from the TOC entry's own id field (5): stages 1..4 when an NV directory is given
     (`-n`), which means BOOT, MAIN, VSS and NV. OFFSET and TOC are never sent.
   - For each stage it keeps the file offset, `m_offset = address & 0x3fffffff`, the size, the crc, and whether the
     stage is VSS. The NV stage reads from the NV file instead of RADIO.
   - Opens `/mnt/vendor/efs/nv_data.bin`. If it is missing, `create_empty_nv` writes it **0xFF-filled, the NV entry's
     size (0x100000)**, with fsync.
3. **`IOCTL_MODEM_STATUS` (0x6f27), then `std_boot_modem_reset` = `IOCTL_MODEM_RESET` (0x6f21).** The reset is a no-op
   while the CP is offline (`ss310ap_reset`).
4. **"Request Security : non-secure mode"** = `std_security_req(2, 0, 0)` → `IOCTL_SECURITY_REQ` (0x6f53).
   - The struct is `{mode, size_boot, size_main, 0}`.
   - Around the ioctl, cbd **clears CPU 0 from its affinity** and restores it afterwards. The SMC is never made from
     CPU 0.
   - For mode 2, a return of 0xb (`CP_NOT_WORKING`) counts as success.
   - In the kernel this is the first call, so `cal_cp_init` runs here, before any image is loaded.
5. **"Send BIN"** (`shannon310_boot_xmit`). For stages 1..4: `lseek(b_off)`, then reads of **0xf800 bytes** at a time.
   Each chunk goes through `IOCTL_MODEM_XMIT_BOOT` (0x6f40) with
   `struct modem_firmware {u64 binary; u32 size, m_offset, b_offset, mode = 0, len}`, `m_offset` advancing chunk by
   chunk. VSS is sent only once per Android boot (`vendor.cbd.first_xmit_done`).
6. **"Request Security : secure mode"** = `std_security_req(0, BOOT.size, MAIN.size)`, i.e. (0, 0x1e40, 0x24075c0),
   again off CPU 0. The kernel ORs the region's base into the mode only if `shm_probe` found a CP memory map at boot.
   On a cold boot BOOT sits at +0 and the probe looks at +0x2a0, so it finds none: **x1 = 0**.
7. **`get_factory_prop`** reads `/efs/factory.prop` (SIM count) and echoes it to `…/mif_pdata/sim/ds_detect`.
8. **"MODEM ON : pda_active 1, cp power on"** = `IOCTL_MODEM_ON` (0x6f19, `ss310ap_on`).
9. **"BOOT ON : change modem state, irq, wait cp state"** = `IOCTL_MODEM_BOOT_ON` (0x6f22). It waits for the CP status
   word.
10. **"BOOT START : reset ipcmap, set magic code"** = `IOCTL_MODEM_DL_START` (0x6f28, `shmem_start_download`).
    - Resets the legacy IPC map at the IPC region (`0xF8000000`), the 4 MB `shmem_4mb_phys_map`:
      - header words: `magic`, `access`, FMT tx/rx head/tail, RAW tx/rx head/tail;
      - 4056 reserved bytes;
      - buffers: FMT tx/rx 4 KB each, RAW tx 2084864 bytes, RAW rx 2 MB.
    - Writes `magic = 0x424F4F54` ("BOOT"), because link attrs 0x7c9 include DPRAM_MAGIC, MEM_BOOT and BOOT_ALIGNED.
    - Then `modem_boot_confirm` sets the AP status to 1.
11. **"HANDSHAKE COMMAND : send & recv boot_done, fin message"** (`std_boot_finish_handshake`).
    - `std_udl_stage_done(0)`: writes the 4-byte word **0x900d** to `umts_boot0`, polls up to 30 s, reads 4 bytes and
      expects **0xa00d**.
    - Then `std_udl_stage_start(0xf)`: writes **0x9f00**, expects **0xaf00**.
    - `umts_boot0` has iod attrs 0x2 (SIPC5), so each word goes out as a SIPC5 frame on channel 215. It goes through
      `xmit_udl` into the map's RAW tx queue, followed by a mailbox interrupt. The answer comes back through the RAW rx
      queue.
12. **"BOOT OFF : wait completion, irq"** = `IOCTL_MODEM_BOOT_OFF` (0x6f23). It waits for `init_cmpl`, which completes
    once the CP has sent PHONE_START and INIT_START and the SBD link is up. Then `IOCTL_MODEM_GET_CP_BOOTLOG` (0x6f47)
    and "Boot up process done!!!" (`vendor.cbd.boot_done`).

**What the driver does with this:**
- `shannon=2` (fbd8bcb) is cbd's steps 4-6 as the kernel carries them out: `cal_cp_init`, SMC(2, 0, 0), the four
  stages at `address & 0x3fffffff` with NV 0xFF unless `shannon_nv=1`, then SMC(0, 0x1e40, 0x24075c0). Both SMCs are
  made from a core whose MPIDR is not 0.
- `shannon=3` adds steps 8-9 and listens. Steps 10-12 (the IPC map, the magic, the UDL words over SIPC5, then the SBD
  link) are the next piece of work. They need the SIPC5 framing written for the boot channel and the RAW queues
  served, which a polled kernel loop can do without the RIL.

**Step 1 on the phone (build 36, kernel 540c944):**
- **CP_CTRL_NS = 0x2016a0.** That is CP_ACTIVE_REQ_EN, CP_RESET_REQ_EN, MASK_CP_PWRDN_DONE, RTC_OUT_EN,
  SET_SW_MIF_REQ and WDT_FLAG_CLR_EN. PWRON (bit 1) and RESET_SET (bit 2) are clear.
- **CP_CTRL_S = 0**, so CP_START is clear.
- **CP_STAT = 1** (CP_PWRDN_DONE). CENTRAL_SEQ_CP_STATUS is 0 and RESET_SEQUENCER_STATUS is 0, so the CP has never
  been initialised. The mailbox is quiet.

This is the cold state Android's first `IOCTL_SECURITY_REQ` finds, so `shannon=2` takes the `cal_cp_init` path. The
RADIO TOC and the CP memory map read on the phone match the backup and the device tree:
- DRAM 0x7900000;
- VSS/IPC/ZMC/VPA at +0x7900000/+0x8000000/+0x8500000/+0x9000000;
- LOG at +0x9200000 (0x2000000) = `cp_ram_logging`.

The "more" size, 0x1900000, is the sum of VSS, IPC, ZMC and VPA, and DRAM plus that is 0x9200000, the `modem_if`
size. Everything sits inside the loader's keep-out (0xF0000000-0xFB1FFFFF).

**The boot-loader size:** cbd passes BOOT's TOC size (0x1e40), with BOOT itself at +0, not after the TOC. That is the
driver's default now. `shannon_toc=1` and `shannon_blsize=` remain only for experiments.

## 4. Talking to the CP

`mif_pdata` in the device tree lists the channels (`iod,name` / `iod,id` / `format`). Format 0 is FMT, 1 is RAW, 2 is
RFS, 3 is multiplexed data and 4 is boot:

| Device | Channel | Use |
|---|---|---|
| `umts_ipc0`, `umts_ipc1` | 235, 236 (FMT) | Samsung IPC 4.x control: what libsec-ril speaks |
| `umts_rfs0` | 245 (RFS) | The CP's remote file system: `/efs` reads and writes, NV backups |
| `umts_boot0` | 215 | Boot (cbd) |
| `rmnet0..7` | 10..17 (RAW) | IP data, one per PDP context |
| `multipdp`, `multipdp_hiprio` | 0 (format 3) | Multiplexed data |
| `umts_csd`, `umts_router`, `umts_dm0`, `umts_cass`, `umts_ciq0`, `smd4` | 1, 25, 28, 35, 26, 33 | CSD, router, diagnostics |

- **Link.** SBD (shared buffer descriptors) rings in the IPC region (`0xF8000000`, 5 MB), with mailbox doorbells
  (`link_device_memory_sbd.c`). Each packet carries a SIPC5 header: start byte 0x7F, a config byte, the channel and a
  length.
- **IPC 4.x.** Each FMT packet is `length(2) mseq aseq group index type` followed by data. The groups, from
  libsamsung-ipc (Replicant), are PWR 0x01, CALL 0x02, SMS 0x04, SEC 0x05, DISP 0x07, NET 0x08, MISC 0x0A, GPRS 0x0D
  and GEN 0x80. libsamsung-ipc covers older Exynos modems (XMM626x, the CMC221 family). The 9810's Shannon adds
  messages and versions of its own, so every message has to be checked on the wire.
- **A minimal client.** RFS has to be served first: the CP stalls without its `/efs` files, much as redwood's modem
  needs RMTFS, and writes should stay in RAM, the same policy as redwood. Then:
  - PWR phone-on / boot-complete;
  - SEC SIM status (PIN state, card present);
  - NET registration and serving network (PLMN, operator name, RAT);
  - DISP/MISC signal (RSSI, bars).

  Those four fill `kern.hakios.cellular_registered`, `_operator`, `_bars` and `_dbm`, and the status bar works as on
  redwood. Data comes next: a GPRS PDP context, then `rmnet0` into `hakios.cell.c`'s `en1`.

## 5. What is missing in iOS without a baseband

| Symptom | Where seen | Fixed by a CommCenter stand-in (route b)? |
|---|---|---|
| 380-520 failed lookups a session for `com.apple.commcenter.coretelephony.xpc` (services audit), plus `com.apple.commcenter.xpc` and `basebandd`; with the noncellular CommCenter shipped, 85 + 18 of them disappeared but the process died every ~15 s | `mkjobs.py` | Yes: the lookup succeeds and returns quickly |
| Settings: Location Services row (asks every SIM's carrier bundle through CommCenter) | patched in `inject_tweaks.c` | Yes (the patch could go) |
| App Store storefront: `appstored` asks `coretelephony.xpc` for the SIM's country | `mkjobs.py` (store notes) | Partly: it gives a country, but AMS's storefront also needs an account or bag path |
| Status bar: no signal item, "No SIM" | `inject_sb.c` forces it | Already handled from sysctls (route c) |
| Phone, Messages (SMS), Cellular Data settings, Personal Hotspot, carrier settings updates, emergency calls, cell-based location assistance | not used | Display only; real function needs the IPC client's calls/SMS/data |
| `mobileactivationd` region and activation ("Device is probably unactivated") | `mkjobs.py` (apsd notes) | No: activation needs Apple's servers and SEP identity; unrelated to the modem |

The single biggest system-wide win is the lookup storm. Even a "no SIM, no service" `telephonyd` answering
`coretelephony.xpc` removes it, with no modem at all. That piece does not depend on the CP and could be done in parallel.

## 6. Plan and effort

| Step | Content | Effort | Risk |
|---|---|---|---|
| 1 (written) | `shannon=1`: registers, SMC reads, RADIO TOC/MEM map. `shannon=2`: load + `cal_cp_init` + verify SMC. `shannon=3`: start, wait for status, log the CP's mailbox for 60 s | done; 1-3 runs on the phone | read-only at 1; at 2-3 a wrong value means a failed SMC or a CP watchdog, not damage (nothing is written to flash) |
| 2 | SBD rings in the IPC region, `PIF_INIT_DONE`, an RFS server (EFS read through `hakios_partition_file`, writes kept in RAM), `INIT_END` | 2-4 weeks | the SBD layout has to match `link_device_memory_sbd.c` exactly |
| 3 | IPC 4.x client for PWR/SEC/NET/DISP, publishing `kern.hakios.cellular_*` (bars and operator in the status bar via `inject_sb.c`) | 2-4 weeks | message formats on the 9810 differ from libsamsung-ipc |
| 4 | Data: GPRS PDP context, `rmnet0` through SBD into `hakios.cell.c` (`en1`) | 2-3 weeks | |
| 5 | `telephonyd` for `com.apple.commcenter.coretelephony.xpc` (route b), and CommCenter out of DENY for good | 1-2 weeks stub, +2-4 weeks real values | the private NSXPC protocol has to be read from CoreTelephony |
| 6 | SMS (IPC SMS group, then Messages through the stand-in) | 1-2 months | |
| 7 | Calls: IPC CALL, plus voice routed through ABOX/VSS to the CP | 2-4+ months | needs ABOX's CPU (calliope), which is not running |

**Signal bars and operator from the real network:** steps 1-3, about 1.5-2.5 months of focused work if the verify SMC
and the boot go through without surprises.

## 7. Step 1: `drivers/modem/hakios.shannon.c`

`CONFIG_HAKIOS_SHANNON` is `default n` in `configs/common_defconfig` and `y` in `devices/star2lte/configs/star2lte.config`.
It does nothing without a boot argument. It is called from `hakios_input_init()` after `hakios_mpss_init()`, with a stub
in `hakios.stubs.c`. It needs UFS (`ufs=2`).

| Argument | Effect |
|---|---|
| `shannon=1` | Read only, each MMIO read logged before it is made: CP_CTRL_NS/S via SMC READ_CTRL, PMU CP_STAT, CENTRAL_SEQ conf/status, RESET_SEQUENCER_STATUS (what Samsung reads at any time), mailbox INTMR0 (read by Linux at boot), then RADIO TOC and MEM map. **Not** AP2CP_CFG: build 35 showed that reading it with the CP never initialised is a synchronous external abort (panic in `log_cp_registers`). It is touched only in step 3, once RESET_SEQUENCER_STATUS is 5 and the verify call passed, as `ss310ap_on` does |
| `shannon_regs=1` | Also the CP SYS_PWR_REGs and the other mailbox words (INTSR0/INTMR1/INTSR1, words 0-3) |
| `shannon_peek=1` | Also logs a few words of the first page at `0xF0000000` (what the boot loader left; Linux reads the same page at probe time) |
| `shannon=2` | cbd's order: `cal_cp_init` if the CP was never initialised, SMC(2, 0, 0) (0 or CP_NOT_WORKING accepted), then BOOT/MAIN/VSS/NV at `address & 0x3fffffff` (BOOT +0, MAIN +0x10000, VSS +0x7900000, NV +0x4900000; crc32s logged), then SMC(0, BOOT size, MAIN size). Both SMCs are made off CPU 0, as cbd makes them. Does not start the CP |
| `shannon_nv=1` | With 2-3: NV from `EFS:/nv_data.bin` if it is exactly the NV entry's size (size logged, never contents). Without it NV is 0xFF, what cbd's `create_empty_nv` writes |
| `shannon_toc=1` | Experiment only: TOC at +0 and BOOT after it (the layout of `shm_ipc.c`'s memory-map probe), not cbd's |
| `shannon_base=1` | Experiment only: the region's base ORed into the secure request's mode (Linux does so only after a warm boot) |
| `shannon_blsize=N` | The boot-loader size for the secure request (default BOOT's TOC size, as cbd) |
| `shannon=3` | Then cbd steps 8-11: the AP status word, AP2CP_CFG = 1 (only with RESET_SEQUENCER_STATUS 5), up to 2 s for the CP status; DL_START (the legacy IPC map at 0xF8000000 reset, magic 0x424F4F54, AP status 1); the UDL handshake (0x900d to 0xa00d, then 0x9f00 to 0xaf00, as SIPC5 frames FC D7 08 00 + word in the RAW queues, 30 s each); then 60 s of the CP mailbox (PHONE_START, INIT_START, ...). PIF_INIT_DONE / INIT_END (the SBD rings) are next |
| `shannon_udl_irq=1` | Also rings the mailbox (MASK_SEND_RAW) after each UDL word; Linux xmit_udl does not |
| `shannon_sysrev=N` | The board revision in the AP status word (default 0) |

`sysctl kern.hakios.shannon_state` is 0 (nothing), 1 (read), 2 (verified), 3 (CP status up) or 4 (CP spoke). The
driver does not create `kern.hakios.cellular_*`: the redwood driver owns those names, and they come with step 3 of the
plan. **Order of runs:** `shannon=1` → `shannon=1 shannon_peek=1` → `shannon=2` → `shannon=3`. Each run's log is in
the loader log or USB console (`shannon:` lines). Firmware is read from the phone's own RADIO at run time, so
nothing of Samsung's is in this repository or in any image.

## 8. Rules kept

The RADIO backup was read for its TOC and BOOT's memory-map words only. EFS.img, CPEFS.img and the other identity
images were not opened. No IMEI, IMSI, ICCID, number or cell ID is printed by this document or by the driver. The D22
device tree's `device-imei` property was seen to exist, and its value was not recorded.
