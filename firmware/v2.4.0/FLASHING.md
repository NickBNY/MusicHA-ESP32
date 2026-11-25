# Flashing the SonosESP v2.4.0 images (ESP32-P4)

Firmware for the GUITION ESP32-P4 Sonos touchscreen controller, built from commit `33fb96b` (`FIRMWARE_VERSION` 2.4.0) of the worktree `/Users/nbelo/sonos/SonosESP/.worktrees/firmware-build`. Three builds exist: `esp32_4inch`, `esp32_4inch_r3` and `esp32_7inch`.

## 1. Status: nothing here has run on hardware

- None of these images has been flashed to a board or booted. No board was connected while this file was written.
- What was checked on the host: image sizes against the 6,553,600-byte (`0x640000`) OTA slot, the `0x2000`, `0x8000`, `0xe000` and `0x10000` part offsets, the app checksum and SHA-256 digest, and `pio test -e native` (196 of 196 cases passed). Host tests do not exercise the display or the device.
- The 7 inch build is beta upstream. `web-installer/manifest-7inch.json` says it is not validated on physical 7 inch hardware.
- The rev3 build (`esp32_4inch_r3`) has never run on a rev3 board. `platformio.ini` records the same for the upstream project.
- Every `esptool` and `esp-coredump` command below was checked against `--help` output of esptool v5.2.0 and esp-coredump (espcoredump.py v1.18.1). None was run against a board. Commands that need a board are marked **unverified**.

## 2. WARNING: rev3 chips and pre-rev3 chips need different images

**Never flash the pre-rev3 image to an ESP32-P4 rev 3.x chip, and never flash the rev3 image to a pre-rev3 chip.**

- Pre-rev3 images (`firmware-4inch.bin`, `firmware-7inch.bin`, `factory-esp32_4inch-v2.4.0.bin`, `factory-esp32_7inch-v2.4.0.bin`) are linked against the `esp32p4_es` libraries and memory map. They are for chip revisions 0.x and 1.x.
- Rev3 images (`firmware-4inch-r3.bin`, `factory-esp32_4inch_r3-v2.4.0.bin`) are linked against the `esp32p4` libraries, the rev3 linker script and a different bootloader (21,568 bytes against 21,392 bytes).
- **The images do not refuse a wrong-revision chip.** `platformio.ini` and the update code comments say the bootloader refuses it. `esptool image-info` on the built files shows "Minimal chip revision v0.0" and "Maximal chip revision v655.35" for all three app images, so the revision range in the header is open. Treat a mismatch as a hard failure with no safety net: `docs/TROUBLESHOOTING.md` states that a wrong image "is not refused and it does not roll back by itself", and the recovery is a cable reinstall of the correct build.
- A pre-rev3 image on a rev3 chip is reported upstream as a board that flashes and verifies cleanly and then shows a dark screen and backlight.
- The 7 inch has no rev3 build. A 7 inch board with rev3 silicon has no matching image in this folder.
- Use the bootloader and app from the same set. The `factory-*` files do this for you.

Find your chip revision first (section 4), then pick the file.

## 3. Files in this folder

SHA-256 values were recomputed with `shasum -a 256` when this file was written.

| File | Bytes | Chip | Use |
|---|---|---|---|
| `factory-esp32_4inch-v2.4.0.bin` | 2,695,360 | pre-rev3 | First install, 4 inch. Flash at `0x0` |
| `factory-esp32_4inch_r3-v2.4.0.bin` | 2,696,944 | rev3 | First install, 4 inch. Flash at `0x0` |
| `factory-esp32_7inch-v2.4.0.bin` | 2,749,488 | pre-rev3 | First install, 7 inch (beta). Flash at `0x0` |
| `firmware-4inch.bin` | 2,629,824 | pre-rev3 | OTA app image, 4 inch (release asset name) |
| `firmware-4inch-r3.bin` | 2,631,408 | rev3 | OTA app image, 4 inch rev3 (release asset name) |
| `firmware-7inch.bin` | 2,683,952 | pre-rev3 | OTA app image, 7 inch (release asset name) |
| `debug/firmware-esp32_4inch-v2.4.0.elf` and `.map` | 28,464,656 and 14,193,683 | pre-rev3 | Coredump decoding |
| `debug/firmware-esp32_4inch_r3-v2.4.0.elf` and `.map` | 28,595,656 and 14,151,458 | rev3 | Coredump decoding |
| `debug/firmware-esp32_7inch-v2.4.0.elf` and `.map` | 28,179,348 and 14,106,906 | pre-rev3 | Coredump decoding |

```
4b1d954ee04096a5df599f4628132196cda29ce91672628fe8c5d1ccf7b09d62  firmware-4inch.bin
c5621e03b9fb7d57cb7e6c924fcb4a99ba09c54d8d696cf15c88da26d4a904c4  firmware-4inch-r3.bin
b807a9ab859d91618ee15a9683194276d67821fde31cee5a6e00dec56560741d  firmware-7inch.bin
50d81928d405dc95f3466c0b8a4e68eea614f43ab839b2854fb761971cac9d63  factory-esp32_4inch-v2.4.0.bin
c9d4396837daa9cc3519445b149c95f6e7d78689774278765ca304752dd5fe7e  factory-esp32_4inch_r3-v2.4.0.bin
f4d559b710b332f1bded741b2db7ca7ed9236b8bb3d222dd72407b6929ee8991  factory-esp32_7inch-v2.4.0.bin
```

Flash layout (`default_16MB.csv`, 16 MB flash): `nvs` at `0x9000`, `otadata` at `0xe000`, `app0` at `0x10000` (`0x640000` bytes), `app1` at `0x650000` (`0x640000` bytes), `spiffs` at `0xc90000`, `coredump` at `0xFF0000` (64 KB). A factory image holds the bootloader at `0x2000`, the partition table at `0x8000`, `boot_app0.bin` at `0xe000` and the app at `0x10000`. File offset 0 is flash address `0x0`. Gaps are `0xFF`.

## 4. Find the board revision

`esptool` v5.2.0 is installed with PlatformIO at `/Users/nbelo/.platformio/penv/bin/esptool`. The project venv `/Users/nbelo/sonos/.venv` has `pio` but no `esptool`, and `/Users/nbelo/.platformio/packages/tool-esptoolpy/esptool.py` fails to start because `rich_click` is missing. Use the penv binary, or run `/Users/nbelo/sonos/.venv/bin/pip install esptool` and use `/Users/nbelo/sonos/.venv/bin/esptool`.

Connect the board over USB-C (see section 5, "Two USB-C ports"), then list ports:

```bash
ls /dev/cu.usbmodem*
```

Read the chip (unverified, needs a board):

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX chip-id
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX flash-id
```

Replace `XXXX` with the suffix from the `ls` output. `chip-id` prints the chip with its revision, for example `ESP32-P4 (revision v3.2)` (wording from `platformio.ini`). `flash-id` prints the flash manufacturer and device ID. The serial log from v2.3.2 onward also prints a line such as `[CHIP] ESP32-P4 rev v1.0 - image built for v0.1..v1.99 (pre-rev3 build)`.

| Reported revision | Image to use |
|---|---|
| v0.x or v1.x | Pre-rev3: `factory-esp32_4inch-v2.4.0.bin` (4 inch) or `factory-esp32_7inch-v2.4.0.bin` (7 inch) |
| v3.x | Rev3: `factory-esp32_4inch_r3-v2.4.0.bin` (4 inch only) |
| Any other revision (for example v2.x) | No image in this folder is built for it. Do not flash. |

If `chip-id` cannot connect, see "Two USB-C ports" and "Recovery mode" below.

## 5. First install over USB-C

### Two USB-C ports

The board has two USB-C ports and only one talks to a computer (README, `docs/TROUBLESHOOTING.md`):

| Port | Use |
|---|---|
| Full-speed Type-C | Flashing and serial. Use this one |
| High-speed Type-C | USB host and OTG. Looks dead to a computer |

The ports look identical. If nothing appears under `/dev/cu.usbmodem*`, move the cable to the other port, then try a cable known to carry data (charge-only USB-C cables carry no data).

### Recovery mode

Hold the BOOT button (the board has two small buttons side by side, marked `BOOT`/`IO0` and `RST`/`EN`), plug in the cable while holding it, and release after about 2 seconds. If nothing changes, repeat with the other button. On boards where the buttons are not fitted, only solder pads exist, and recovery mode is not available by button.

### Command: merged factory image at 0x0

This is the equivalent of the web installer, in one file. It rewrites the bootloader, partition table, `otadata` and `app0` in a single write. Baud 460800 is the `upload_speed` in `platformio.ini`.

4 inch, pre-rev3 chip (unverified, needs a board):

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write-flash 0x0 /Users/nbelo/sonos/firmware-images/factory-esp32_4inch-v2.4.0.bin
```

4 inch, rev3 chip (unverified, needs a board):

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write-flash 0x0 /Users/nbelo/sonos/firmware-images/factory-esp32_4inch_r3-v2.4.0.bin
```

7 inch, pre-rev3 chip (unverified, needs a board):

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write-flash 0x0 /Users/nbelo/sonos/firmware-images/factory-esp32_7inch-v2.4.0.bin
```

After the write, unplug and replug the board (README), then set up Wi-Fi on screen.

**This erases saved settings.** The factory image spans `0x9000` to `0xDFFF`, which is the `nvs` partition, so the write replaces NVS with `0xFF` bytes. Wi-Fi credentials, the remembered speaker and all settings are lost. This follows from the image layout and has not been observed on a board. To keep NVS, use the four-part command below.

Flash header note: `esptool image-info` reports flash mode DIO, 16 MB and 80 MHz in the headers of these files, while `platformio.ini` sets `board_build.flash_mode = qio`. DIO at 80 MHz is a valid, slower setting. Whether the board boots with it has not been tested.

### Alternative: four parts, keeps NVS (unverified, needs a board)

This is the layout in `web-installer/manifest-*.json` (offsets 8192, 32768, 57344 and 65536 decimal). It writes only those four regions and leaves NVS alone. The part files are build output in the worktree `.pio` folder (git-ignored) and in the PlatformIO framework package, so they are not in this folder.

Pre-rev3 4 inch:

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX --baud 460800 write-flash \
  0x2000 /Users/nbelo/sonos/SonosESP/.worktrees/firmware-build/.pio/build/esp32_4inch/bootloader.bin \
  0x8000 /Users/nbelo/sonos/SonosESP/.worktrees/firmware-build/.pio/build/esp32_4inch/partitions.bin \
  0xe000 /Users/nbelo/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 /Users/nbelo/sonos/firmware-images/firmware-4inch.bin
```

For rev3 4 inch, take the bootloader from `.pio/build/esp32_4inch_r3/bootloader.bin` and the app from `firmware-4inch-r3.bin`. For 7 inch, use `.pio/build/esp32_7inch/bootloader.bin` and `firmware-7inch.bin`. `partitions.bin` is byte-identical across the three environments. Writing `boot_app0.bin` at `0xe000` resets `otadata` so the board boots `app0`.

### Wipe everything first (unverified, needs a board)

`-e` erases the whole flash before writing, including NVS and the `coredump` partition. Save any coredump first (section 9).

```bash
/Users/nbelo/.platformio/penv/bin/esptool --chip esp32p4 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write-flash -e 0x0 /Users/nbelo/sonos/firmware-images/factory-esp32_4inch-v2.4.0.bin
```

## 6. Updating over the air

The project has one OTA mechanism: the panel downloads a release from GitHub and writes it to the inactive OTA slot. There is no ArduinoOTA, no web upload endpoint and no way to push a local `.bin` to the panel (searched `src/`, `include/` and `lib/`).

How it works (`src/ui_handlers.cpp`, `docs/guide/updates.md`):

1. Open **Settings, then Firmware Update**, pick **Stable** or **Nightly**, tap **Check for Updates**, then install.
2. The panel requests `https://api.github.com/repos/<GITHUB_REPO>/releases/latest` (Stable) or `.../releases?per_page=5` (Nightly, tags containing `-nightly`). The request carries no token, so the repository must be public.
3. It picks the release asset by file name: `firmware-7inch.bin` on a 7 inch build, `firmware-4inch-r3.bin` on a rev3 4 inch build, `firmware-4inch.bin` on a pre-rev3 4 inch build. The three `firmware-*.bin` files in this folder carry exactly these names.
4. Stable installs only a strictly newer semantic version than `FIRMWARE_VERSION`. Nightly offers any version that differs from the running one.
5. The panel stores the URL, restarts, and installs at the start of the next boot. Interrupted downloads resume from the byte reached, up to 6 attempts.
6. After the install the new image runs in a trial. It is confirmed after 60 seconds of healthy runtime. A crash or hang before then rolls back to the previous slot (`src/main.cpp`, `verifyRollbackLater`).

To put these `firmware-*.bin` files on a panel by OTA:

1. Build with your own repository (section 7), flash that build once over USB.
2. Create a public GitHub release in that repository, with a tag that is a higher version than the running one, and upload `firmware-4inch.bin`, `firmware-4inch-r3.bin` and `firmware-7inch.bin` as assets. Upload only the assets your panels need. `RELEASE.md` also lists `bootloader.bin`, `partitions.bin` and per-screen zips as release assets; the panel does not read them.
3. Run Check for Updates on the panel.

The images in this folder were built with the stock `GITHUB_REPO` (upstream). A panel running them looks at upstream releases, not yours. Section 7 explains the consequence.

## 7. Facts to know before first boot

### Auto-update is inert in this build, and a manual install replaces your firmware

- `include/ui_common.h` defines `GITHUB_REPO` as `OpenSurface/SonosESP` unless overridden at build time. `AUTOUPDATE_ALLOW_UPSTREAM` defaults to `0`.
- With both defaults, `kAutoInstallSourceOk` is false. The daily background check still runs (first check 10 minutes after boot, then every 24 hours, only while nobody is touching the panel) and can show an update notice. The panel never installs on its own. The log prints `[OTA] Auto-install is off: updates come from OpenSurface/SonosESP (upstream)` and the Update screen says so under the switch.
- **A manual install from the update screen pulls upstream's release.** Stable requires a strictly newer version than 2.4.0. If upstream publishes one, installing it replaces this build with upstream's firmware. Do not tap Install on a panel running these images unless you want that.
- Auto-update also follows only the Stable channel. It installs after 5 minutes without a touch. Each version is tried automatically once.

To point updates at your own fork, set `GITHUB_REPO` in the `build_flags` of the environments you build in `platformio.ini`, then rebuild:

```ini
[env:esp32_4inch]
extends     = esp32_base
build_flags = ${esp32_base.build_flags} -DSCREEN_SIZE=4 -DGITHUB_REPO=\"owner/repo\"
lib_ignore  = jd9165_lcd
```

Apply the same flag to `esp32_4inch_r3` and `esp32_7inch`. Publish releases in `owner/repo` with the asset names in section 6. Bump the version before each release with `python scripts/bump_version.py patch` (or `minor`, `major`, or an exact version), which updates `version.json`, the three manifests and `include/ui_common.h`. To accept automatic installs from upstream instead, build with `-DAUTOUPDATE_ALLOW_UPSTREAM=1`. The auto-install switch is on the Firmware Update screen under the version card, and it is on by default.

Downloads are not certificate checked (`setInsecure`). Anyone who can intercept traffic to github.com on your network can serve a firmware image. Turn the switch off on a network you do not trust.

### Defaults

- **Home Assistant (ESPHome API) is off.** Enable it in **Settings, then General, then the Home Assistant card**, switch **Enable**. The panel then appears as an ESPHome device on TCP port 6053 with now playing, volume, Wi-Fi signal and uptime. The connection is unencrypted and read-only, so use it only on a trusted home network. If Home Assistant does not discover it, add the ESPHome integration and enter the address shown on the card.
- **Debug is off.** The **Debug information** switch on the same card is greyed out until Enable is on. It adds memory, task stack, restart history and speaker sync entities to the Home Assistant device.
- **Camera support is not included.** `camera` has zero matches in `src/`, `include/`, `lib/`, `docs/`, `README.md` and `platformio.ini`. There is no camera feature to enable.

### First boot and speakers

- The last room or group you picked is stored in NVS and restored at boot. The speaker list refreshes after boot and every 4 hours while the panel is idle, and a panel that knows no speakers scans again every 10 minutes (README, `src/sonos_sync.cpp`).
- After a first install with a factory image, or after a full erase, NVS is empty, so there is nothing to restore. Set up Wi-Fi (2.4 GHz only), then use **Settings, then Speakers, then Scan** and select a room. From then on the selection survives reboots and OTA updates.
- A 7 inch panel runs a first-boot display check. It shows colour bars and asks "Can you read this?". Tap **Looks good** if the picture is clean. If the screen is striped or blank, wait 20 seconds and it restarts with the other panel setting (`docs/TROUBLESHOOTING.md`).

## 8. Going back

There is no manual rollback control in the firmware. The code never calls `esp_ota_set_boot_partition`, so you cannot select the previous slot from the panel.

| Situation | What happens or what to do |
|---|---|
| New OTA image crashes or hangs in its first 60 seconds | The bootloader boots the previous OTA slot automatically |
| New OTA image confirmed (after 60 seconds) and you want the old one back | Install the older version by OTA only if your release channel offers it. Stable never installs an older version. Nightly installs any version that differs. Otherwise reflash over USB |
| Reflash a previous version over USB | Write that version's factory image (or its four parts) with the commands in section 5. It must match the chip revision. The previous version's files are not in this folder |
| Settings or Wi-Fi are corrupt | Run `write-flash -e` (section 5, "Wipe everything first") or `esptool erase-flash`, then write the factory image |
| Board does not boot after a flash | Enter recovery mode (section 5), then reflash the correct-revision factory image |
| Wrong-revision image was flashed | Not refused and no rollback. Reflash the correct-revision factory image over the cable |

An interrupted OTA write leaves the running slot untouched, and the board boots the previous firmware (`docs/guide/updates.md`). Erase and reflash commands are unverified on hardware.

## 9. Decode coredumps with the debug/ ELF files

The firmware writes an ELF core dump to the 64 KB `coredump` partition at `0xFF0000` on every panic and reports a summary on the next boot (`docs/COREDUMP.md`). Decoding needs the ELF of the exact build that crashed.

| Board | ELF in `debug/` | SHA-256 of the file |
|---|---|---|
| 4 inch pre-rev3 | `firmware-esp32_4inch-v2.4.0.elf` | `3fc158ad32f5ac29230d4ba46f237fcecbbed801b51246dd72ba5b918bbcfe23` |
| 4 inch rev3 | `firmware-esp32_4inch_r3-v2.4.0.elf` | `7cf76e8279e7f2bba9f32529b31b7822ec8f0d4abb71d7287ad1d38fbddb7ab8` |
| 7 inch pre-rev3 | `firmware-esp32_7inch-v2.4.0.elf` | `f2a924fae0fb65b434a729c58a12d5a12494aa2079bbc83e81786e404ba00e67` |

`docs/COREDUMP.md` says the boot summary line `build :` is the start of the crashing build's `app_elf_sha256`, and that `esp_coredump` warns on a mismatch. Compare `build :` with the start of the hash above. Matching the file hash to `app_elf_sha256` has not been checked on a booted device. Do not ignore an ELF mismatch warning: a different build gives a plausible, wrong backtrace.

Read the dump off the board and symbolise it (unverified, needs a board that has crashed). `esp-coredump` is installed with PlatformIO at `/Users/nbelo/.platformio/penv/bin/esp-coredump`; `docs/COREDUMP.md` shows the same command as `python -m esp_coredump`. The `--gdb` and `--rom-elf` paths point at PlatformIO packages that exist on this machine:

```bash
/Users/nbelo/.platformio/penv/bin/esp-coredump --chip esp32p4 --port /dev/cu.usbmodemXXXX \
  --gdb /Users/nbelo/.platformio/packages/tool-riscv32-esp-elf-gdb/bin/riscv32-esp-elf-gdb \
  --rom-elf /Users/nbelo/.platformio/packages/tool-esp-rom-elfs/esp32p4_rev0_rom.elf \
  info_corefile /Users/nbelo/sonos/firmware-images/debug/firmware-esp32_4inch-v2.4.0.elf
```

Substitute the ELF for your board from the table. Save the raw dump before it is overwritten or erased, and attach it with the matching ELF to an issue (`-s` writes the core file):

```bash
/Users/nbelo/.platformio/penv/bin/esp-coredump --chip esp32p4 --port /dev/cu.usbmodemXXXX \
  info_corefile -s /Users/nbelo/sonos/firmware-images/crash.elf \
  /Users/nbelo/sonos/firmware-images/debug/firmware-esp32_4inch-v2.4.0.elf
```

To decode a saved core without the board, add `--core <file>` to `info_corefile` (option confirmed in `esp-coredump info_corefile --help`).

Notes:

- The dump is not erased after the boot report. A later panic overwrites it. `erase-flash` and `write-flash -e` wipe it, so decode or save it first. A factory-image write stops at the end of `app0` (below `0xFF0000`) and leaves the partition alone.
- With no `[COREDUMP]` lines on boot, no dump is stored. Watchdog resets (`TASK_WDT`, `INT_WDT`) and brownouts do not write dumps.
- The `.map` files in `debug/` are linker maps for size and placement questions. They are not read by `esp-coredump`.
- Serial log for issues: `/Users/nbelo/sonos/.venv/bin/pio device monitor -b 115200` (the project `monitor_speed` is 115200).

## 10. Commands not verified against hardware

All commands in sections 4, 5, 8 and 9 that use a serial port: `chip-id`, `flash-id`, `write-flash` (single file, four parts, `-e`), `erase-flash`, and `esp-coredump info_corefile` from the device. Syntax was checked against `--help` output only. Verified on this machine: `esptool image-info` on the app images, `shasum -a 256` on all files, the presence of the part files, `esp-coredump`, the gdb binary and the ROM ELF at the paths used above.
