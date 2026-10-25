# HackiOS on the OnePlus 9 Pro (lemonadep)

The `lemonadep` branches of this repository and of
[hackios-xnu-userland](https://github.com/furangelz/hackios-xnu-userland/tree/lemonadep) run HackiOS on the
**OnePlus 9 Pro** (Snapdragon 888, SM8350, codename `lemonadep`). The project's notes, its history and a setup
script are in [furangelz/ios-sweet](https://github.com/furangelz/ios-sweet); this page is about the code here.

Nothing is flashed: the phone `fastboot boot`s a Mu-Silicium UEFI, the UEFI starts our loader, the loader
receives the kernel and the iOS 16.5 system image over USB and runs them from RAM. A reboot brings back Android.

## State (2026-10-25)

| Works | Not yet |
|---|---|
| Boot to the lock screen and home screen; apps, Control Center, Settings | Wi-Fi, Bluetooth, cellular |
| 7 CPU cores for the kernel (the X1 at 2.84 GHz), the loader watching from core 0 | Sound: the audio DSP runs and its audio services answer, but the speaker path is the redwood's |
| Touch (Synaptics S3908), power and volume keys | Camera, sensors, alert slider |
| Adreno 660 through Mesa/freedreno: CoreAnimation draws with OpenGL ES | Fast charging: without OnePlus's VOOC handshake a charger gives 500 mA |
| Metal for applications, and for CoreAnimation with `camtl=1` (below) | Metal: some layers wrong (boxes behind labels), multisample resolves new and unproven on the phone |
| No tearing from the framebuffer: frames reach the panel by page flips in the display engine | Charging while the phone waits in the loader between runs |
| Panel brightness from iOS (slider, dimming, screen off) | Data that survives a reboot (no var store on this phone) |
| Battery level from the phone's fuel gauge; charging by the phone's own audio DSP firmware | |
| Processors and memory slowed while the screen is off | |

Open problems seen on the phone: horizontal tearing inside iOS's picture while scrolling (every frame is flipped
whole, so it is not the old cause), heat under use (processors and memory stay at their top speed while the
screen is on), a magenta tint seen once.

## Metal

iOS's own Metal.framework finds HackiOS's Metal driver (`userland/src/redwoodmetal.m` in hackios-xnu-userland, the
plug-in `RedwoodMetal.bundle`). The driver compiles Apple's AIR bitcode with the frontend in `tools/mesa/air` (AIR ->
NIR -> ir3) and draws through Mesa's freedreno, in the same process. Three boot-args:

| Boot-arg | What |
|---|---|
| `metal=1` | lets `metal_props on` (userland) name the plug-in on the GPU's `IOAcceleratorES` service, so `MTLCreateSystemDefaultDevice()` returns it |
| `metal_profile=1` | the GPU families claimed: Apple1 and Apple2 (SpringBoard's Siri orb aborts without Apple2) |
| `camtl=1` | the window server (CoreAnimation in backboardd) composites with Metal; without it, it keeps OpenGL ES |

On 2026-10-25 (`camtl=1`) the lock screen, home screen, widgets (chronod's RenderBox) and the Today view were
composited with Metal, and an application's `CAMetalLayer` reached the panel. The test job list is
`userland/rc.jobs.lemonmetal` (`SKIP="kernel loader boot" common/build.sh lemonadep QUIET=1
RC_JOBS=rc.jobs.lemonmetal`, with `SYSTEM=1` once for RedwoodTriangle.app).

## Boot arguments

`common/devices/lemonadep.conf` (`BOOTARGS`, used by `xl run` and `common/run.sh`):

| Boot-arg | What |
|---|---|
| `cpus=8 bootcpu=7` | the kernel on the X1 (core 7) and cores 6..1; core 0 stays the loader's |
| `cpufreq=1` | EPSS: every core group at the top of its table, the memory's bandwidth vote raised; slowed again while the screen is off (`bsd/dev/redwood_cpufreq.c`) |
| `buttons=1` | power (PON), volume up (RESIN), volume down (PM8350 GPIO6) (`redwood_buttons.c`) |
| `touchscreen=1 touch_power=1` | Synaptics S3908 TouchComm on bit-banged I2C (gpio20/21), its supplies (RPMh ldoc3, gpio31, eGPIO 197) (`redwood_synaptics.c`) |
| `gpu=1` | Adreno 660: GMU, zap shader through TrustZone, ringbuffer; `fw-gpu/` (`redwood_adreno.c`, `lemonadep_values`) |
| `battery=1` | BQ28Z610 fuel gauge on bit-banged I2C (gpio12/13), read only, every 5 s (`redwood_gauge.c`) |
| `backlight=1` | DCS 0x51 to the AMB670YF01 panel through DSI0's command FIFO; an `AppleARMBacklight` service for CoreBrightness (`redwood_dpu.c`, `MiniIOMobileFramebuffer.cpp`) |
| `adsp=1` | the audio DSP started through TrustZone (its own signed image from `modem_a`), GLINK, QRTR, APR; its battery manager is asked about the USB port; the DSP charges the battery (`redwood_adsp.c`) |

Others that help: `dpu_probe=1` (how the display engine is set up), `dpu_flip=0` (no page flips),
`backlight_test=1` (dim, bright, normal at boot), `battery_log=1` (a gauge line every 5 s), `battmgr=2` (every
battery-manager property once), `regtrace_class=CLASS` (registry reads of one class into the loader log),
`adreno_speed=1` (GPU at 315 MHz: draws less, too slow in use), `charge_ma=N` (raise the USB input limit to N
mA, at most 2000: **not with OnePlus's own Warp charger**, which without its handshake does not hold the current;
the battery then discharges).

## Firmware from the phone

Never in git: these are the phone's own files, taken off it with the loader (`xl enter`, read-only):

- **GPU** (`fw-gpu/`): `tools/super_read.py vendor_a OUT.img` reads the vendor partition out of `super`;
  `fsck.erofs --extract=DIR OUT.img` (erofs-utils 1.8+); copy `firmware/a660_gmu.bin`, `a660_sqe.fw`,
  `a660_zap.mdt` and `a660_zap.b00..b02`.
- **Audio DSP** (`fw-audio/`): `tools/part_read.py modem_a OUT.img` (FAT, 220 MB), then
  `7z e OUT.img 'image/adsp.*'`. The image is linked at 0x87300000 and relocatable; the kernel puts it at the
  DSP's memory, 0x86100000 + 33 MB.

`common/build.sh lemonadep` then builds the images with `GL=1 ADSP=1`: the GPU and DSP firmware go into the
small image's `fw/` (27.5 MB; with the 15 MB kernel within the loader's 64 MB).

## A session

```sh
XL_DEVICE=lemonadep ./xl enter      # from Android (adb) or the bootloader: fastboot boot of the UEFI
XL_DEVICE=lemonadep ./xl ramdisk    # the system image, 2.4 GB, once per loader session (~90 s)
XL_DEVICE=lemonadep XL_SECS=600 ./xl run   # kernel + small image; the log comes back to logs/
```

`XL_CMDLINE` overrides the boot arguments. From TWRP or Android, `adb reboot bootloader` first.

Things to know:

- **Watch the battery.** Under load the phone uses more than a computer's USB port gives, and **nothing charges in
  the loader** between runs: a phone left there overnight empties. The gauge's line (`battery: gauge ok: ...`)
  is in every log; charge it in Android when it gets low.
- **Do not pull the USB cable during a run.** The loader's USB link does not come back afterwards; the run's log
  is then lost and only Power + Volume Up (held 10-15 s) gets the phone back. For runs off the cable (a wall
  charger), the kernel draws a summary of the battery and the charger on the panel when the run ends
  (`redwood_dpu_note()`): read it off the screen.
- **A run's log keeps its first 256 KB and its last 1 MB**; what is in between is dropped.
- The charger is driven by the DSP's own firmware; the kernel writes nothing to a charger chip. It shuts the DSP
  down (ending the charging) if the battery charges outside 0-45 C or above 4.40 V a cell, since the DSP does not
  read this battery's temperature.

## Where the lemonadep code is

| File | What |
|---|---|
| `xnuloader/loader.c` | `platform_lemonadep` (GIC, cores, framebuffer); `oem readpart NAME OFFSET LENGTH` |
| `uefi/files/Platforms/OnePlus/lemonadepPkg/` | the UEFI port (from Mu-Silicium's lemonadePkg; the OnePlus 9's binaries, ClockDxe patched not to take over the display's rail) |
| `bsd/dev/redwood_synaptics.c` | the S3908 (TouchComm, OnePlus's power order) |
| `bsd/dev/redwood_adreno.c` | the Adreno 660 values (`lemonadep_values`) beside the redwood's 642L |
| `bsd/dev/redwood_dpu.c` | the display engine (page flips), DSI0 commands (brightness), the end-of-run notes |
| `bsd/dev/redwood_gauge.c` | the BQ28Z610 gauge, the charging guard |
| `bsd/dev/redwood_adsp.c` | the audio DSP on lemonadep (memory, relocation), its battery manager's USB view and limits |
| `bsd/dev/redwood_cpufreq.c` | screen-off slowdown |
| `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | flips, `AppleARMBacklight` for CoreBrightness |
| `tools/super_read.py`, `tools/part_read.py` | partitions read through the loader |

(Under `xnu/files/` and in `xnu/redwood.patch`; `xnu/apply.sh` puts them into an XNU tree, `xnu/export.sh` takes
them back out.)
