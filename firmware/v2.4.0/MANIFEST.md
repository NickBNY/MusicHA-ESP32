# SonosESP firmware images, manifest

## Build identity

| Item | Value |
|---|---|
| Firmware version | `2.4.0` (`FIRMWARE_VERSION` in `include/ui_common.h`, line 24, read from the worktree) |
| Git branch | `feature/rewrite-wip` |
| Head commit | `33fb96bed7518c6aa60454ea99fa01e8cfe9e6b0` |
| Source worktree | `/Users/nbelo/sonos/SonosESP/.worktrees/firmware-build` (detached HEAD at the tip of `feature/rewrite-wip`; `git status` clean) |
| Build date | 2026-11-24 (builds finished 00:24, 00:28 and 00:32, UTC-05:00) |
| Build tool | PlatformIO, `pio run -e esp32_4inch -e esp32_4inch_r3 -e esp32_7inch`, one environment per run |
| Target | ESP32-P4, 16 MB flash, partition table `default_16MB.csv` |
| Status | Not committed, not pushed, not tagged, not flashed |

## Not run on hardware, no camera

None of these images has been run on hardware. No board was flashed or booted. Every check below ran on the build host.

Camera support is not included. The source has no camera feature, so there is nothing to enable.

## Files

Chip revision target: "pre-rev3" means ESP32-P4 revisions 0.x and 1.x (`esp32p4_es` libraries). "rev3" means revision 3.x (`esp32p4` libraries). The image headers do not enforce the revision (see Findings), so match the file to the board by hand.

| File | Role | Environment | Chip revision target | Size (bytes) | SHA-256 |
|---|---|---|---|---|---|
| `firmware-4inch.bin` | OTA | `esp32_4inch` | pre-rev3 | 2629824 | `4b1d954ee04096a5df599f4628132196cda29ce91672628fe8c5d1ccf7b09d62` |
| `firmware-4inch-r3.bin` | OTA | `esp32_4inch_r3` | rev3 | 2631408 | `c5621e03b9fb7d57cb7e6c924fcb4a99ba09c54d8d696cf15c88da26d4a904c4` |
| `firmware-7inch.bin` | OTA | `esp32_7inch` | pre-rev3 | 2683952 | `b807a9ab859d91618ee15a9683194276d67821fde31cee5a6e00dec56560741d` |
| `factory-esp32_4inch-v2.4.0.bin` | factory (flash at `0x0`) | `esp32_4inch` | pre-rev3 | 2695360 | `50d81928d405dc95f3466c0b8a4e68eea614f43ab839b2854fb761971cac9d63` |
| `factory-esp32_4inch_r3-v2.4.0.bin` | factory (flash at `0x0`) | `esp32_4inch_r3` | rev3 | 2696944 | `c9d4396837daa9cc3519445b149c95f6e7d78689774278765ca304752dd5fe7e` |
| `factory-esp32_7inch-v2.4.0.bin` | factory (flash at `0x0`) | `esp32_7inch` | pre-rev3 | 2749488 | `f4d559b710b332f1bded741b2db7ca7ed9236b8bb3d222dd72407b6929ee8991` |
| `debug/firmware-esp32_4inch-v2.4.0.elf` | debug (coredump decoding) | `esp32_4inch` | pre-rev3 | 28464656 | `3fc158ad32f5ac29230d4ba46f237fcecbbed801b51246dd72ba5b918bbcfe23` |
| `debug/firmware-esp32_4inch-v2.4.0.map` | debug (linker map) | `esp32_4inch` | pre-rev3 | 14193683 | `93ce61372d6ff5ea19a14dc9aea317e793efb33ce509b2d9cba408d72bd0e8b3` |
| `debug/firmware-esp32_4inch_r3-v2.4.0.elf` | debug (coredump decoding) | `esp32_4inch_r3` | rev3 | 28595656 | `7cf76e8279e7f2bba9f32529b31b7822ec8f0d4abb71d7287ad1d38fbddb7ab8` |
| `debug/firmware-esp32_4inch_r3-v2.4.0.map` | debug (linker map) | `esp32_4inch_r3` | rev3 | 14151458 | `9a16948d63f0e5eaacb69e727fbb253ba4d283c7707f92d556a39dc23f021019` |
| `debug/firmware-esp32_7inch-v2.4.0.elf` | debug (coredump decoding) | `esp32_7inch` | pre-rev3 | 28179348 | `f2a924fae0fb65b434a729c58a12d5a12494aa2079bbc83e81786e404ba00e67` |
| `debug/firmware-esp32_7inch-v2.4.0.map` | debug (linker map) | `esp32_7inch` | pre-rev3 | 14106906 | `cfbac2cffaf91cf8c6c423b3a94655eeffff99b36cdff902e7f8a18aff7d9d06` |
| `FLASHING.md` | documentation | all | all | 21522 | `5bad5576e9b530dffd944547acb352633893a216e31188e742df66ac4e4a65fc` |
| `MANIFEST.md` | documentation (this file) | all | all | see `SHA256SUMS.txt` | see `SHA256SUMS.txt` |
| `SHA256SUMS.txt` | checksum list for every other file | all | all | not listed | not listed |

A file cannot carry its own hash, so `MANIFEST.md` is covered by `SHA256SUMS.txt` only, and `SHA256SUMS.txt` covers no hash of itself. Check the folder with `shasum -a 256 -c SHA256SUMS.txt` from inside it.

The OTA files are each `.pio/build/<env>/firmware.bin` copied unchanged, under the release asset names the updater looks for. The factory files are each PlatformIO `firmware.factory.bin`, copied unchanged. The ELF and map files are copied from `.pio/build/<env>/`.

## Verification

Partition table `default_16MB.csv`. OTA slot `app0` at `0x10000`, `0x640000` (6,553,600) bytes. `app1` has the same size. Each factory image was checked against its parts.

| | esp32_4inch | esp32_4inch_r3 | esp32_7inch |
|---|---|---|---|
| OTA image | `firmware-4inch.bin` | `firmware-4inch-r3.bin` | `firmware-7inch.bin` |
| Image size (bytes) | 2,629,824 | 2,631,408 | 2,683,952 |
| Partition size (bytes) | 6,553,600 | 6,553,600 | 6,553,600 |
| Headroom (bytes) | 3,923,776 (59.9%) | 3,922,192 (59.8%) | 3,869,648 (59.0%) |
| Chip revision, min / max (image header) | v0.0 / v655.35 | v0.0 / v655.35 | v0.0 / v655.35 |
| Chip ID | 0x12 (ESP32-P4) | 0x12 (ESP32-P4) | 0x12 (ESP32-P4) |
| Bootloader in factory image | at `0x2000` | at `0x2000` | at `0x2000` |
| Partition table in factory image | at `0x8000` | at `0x8000` | at `0x8000` |
| otadata (`boot_app0.bin`) | at `0xe000` | at `0xe000` | at `0xe000` |
| App in factory image | at `0x10000` | at `0x10000` | at `0x10000` |
| **Result** | **PASS** | **PASS** | **PASS** |

Checks run on all three environments:

- The offsets match the three `web-installer/manifest-*.json` files.
- The bootloader, partition table and otadata regions match the bytes in `.pio/build/<env>/` and the framework `boot_app0.bin`. The first `0x2000` bytes are `0xFF`, and the gaps between regions are `0xFF`.
- The app region equals the OTA `.bin` byte for byte. The factory image length is `0x10000` plus the app size.
- Each OTA `.bin` equals its `.pio/build/<env>/firmware.bin`, and each factory image equals its `firmware.factory.bin`.
- The embedded partition table parses to the same six entries as the CSV (nvs, otadata, app0, app1, spiffs, coredump).
- The XOR checksum and the appended SHA-256 digest of each app image are valid (`esptool` v5.2.0).

## Host test result

`pio test -e native`: exit code 0. 196 test cases, 196 succeeded, 0 failed, 16.07 seconds, 10 suites passed (`test_reboot_log`, `test_sync`, `test_esphome_session`, `test_position_estimate`, `test_topology`, `test_esphome_proto`, `test_esphome_mdns`, `test_battery`, `test_auto_update`, `test_sleep_timer`). These tests run on the build host. They do not exercise the device firmware or the display.

## Findings

1. **The header chip revision range does not separate the builds.** All three app images and all three bootloaders report min v0.0 and max v655.35. The pre-rev3 and rev3 split lives in the prebuilt libraries, the linker script and the bootloader contents. The `esp32_4inch_r3` map links 0 references from `esp32p4_es/`, and the other two maps link only `esp32p4_es/`. Flash rev3 files only to rev3 boards and the others only to pre-rev3 boards, because the images do not refuse a wrong-revision chip.
2. **Flash mode in the headers is DIO.** The headers carry DIO, 16 MB and 80 MHz, while `platformio.ini` sets `board_build.flash_mode = qio`. DIO at 80 MHz is a valid, slower setting. Boot with this setting has not been tested.
3. **Build warnings.** None come from project code. All three builds print `-Wdiscarded-qualifiers` in `esp32-hal-spi.c:637` and `-Wliteral-suffix` in `periph_ctrl.h:88`. The `esp32_4inch_r3` link also prints "cannot assign memory region sram_seg to chip memory type", and the link and image succeeded.
4. **Embedded app info.** `Project name` ("arduino-lib-builder"), `App version` ("14a0af9") and `Compile time` ("Mar 31 2026") in `esptool image-info` come from the prebuilt Arduino libraries, not from this firmware.

See `FLASHING.md` for flashing commands, the auto-update default (inert in this build) and coredump decoding.

Advisory note: this manifest records build and host-side check results. It makes no statement about on-device behavior.
