# Troubleshooting

Common problems and how to fix them. If none of this helps, [open an issue](https://github.com/OpenSurface/SonosESP/issues) — and please include the [serial log](#how-to-get-a-serial-log), it usually identifies the cause immediately.

**Jump to:** [Computer can't see the device](#the-computer-cant-see-the-device) · [Updates fail](#updates-fail-or-stop-partway) · [Blank screen](#blank-screen-after-an-update) · [Wi-Fi](#wi-fi-wont-connect) · [No speakers](#no-sonos-speakers-found) · [Album art](#album-art-doesnt-appear) · [Lyrics](#lyrics-dont-appear) · [Screen flickers](#the-screen-flickers-during-an-update) · [Restarted on its own](#the-panel-restarted-on-its-own) · [Panel battery](#the-panels-own-battery) · [Full reset](#start-completely-fresh) · [Serial log](#how-to-get-a-serial-log)

---

## The computer can't see the device

Nothing appears in Device Manager / no new serial port when you plug it in. **Usually this is not a broken board.** Work through these in order.

### 1. Use the other USB-C port ⭐

**This is the most common cause.** The board has **two USB-C ports** and only one of them talks to a computer:

| Port | What it's for |
|---|---|
| **Full-speed Type-C** | Flashing and serial — **use this one** |
| **High-speed Type-C** | USB host/OTG — looks completely dead to a PC |

They look identical. If you're on the wrong one there is no beep, no Device Manager entry, nothing — exactly like a dead board. **Move the cable to the other port and try again.**

### 2. Try a different USB-C cable

Many USB-C cables are **charge-only** and carry no data. If a cable charges your phone that proves nothing. Use one you know does data transfer.

### 3. Force recovery (bootloader) mode

Works even if the wrong firmware was flashed or the firmware crashes at boot.

There are **two small buttons next to each other** on the board (typically marked `BOOT`/`IO0` and `RST`/`EN`):

1. Hold **one** of them down
2. Keeping it held, plug in the USB-C cable
3. Hold ~2 more seconds, then release

If nothing changes, repeat with **the other button**. One of them is BOOT — that's the one that forces recovery mode.

> On some builds of this board the buttons aren't fitted and only solder pads are present. If you can't find any buttons, that's normal — rely on steps 1 and 2.

### 4. Check the driver

The ESP32-P4 uses its **built-in USB serial** — no driver is normally required on Windows 10/11, macOS or Linux. If Device Manager shows an *unknown device* rather than nothing at all, the board is alive and it's a driver/cable issue, not damage.

### Still nothing?

If the board doesn't appear **in recovery mode, on the correct port, with a known-good data cable, on more than one computer**, then it may genuinely be damaged.

---

## Updates fail or stop partway

Symptoms: the progress bar reaches 30–50 % and stops, or the update needs many attempts.

### Updates now resume instead of restarting (1.11.0 and later)

The download can stall because of a known bug in the Wi-Fi co-processor link — the connection stays open but data stops arriving. It is a platform-level issue we can't prevent, so instead the update now **survives** it.

From 1.11.0 the device keeps everything it has already written and asks the server to continue from that exact byte, rather than starting over. If you see

> `Download stalled - resuming...` then `Resuming from 47%...`

that is the recovery working — **let it run**. It will try up to 6 times, and each attempt picks up where the last one stopped. Before 1.11.0 every retry restarted from 0 %, which is why an update could fail 20 times in a row at roughly the same place.

**What to do now:** retry from **Settings → Firmware Update**. If it still fails after that, install the latest firmware with the [web installer](https://opensurface.github.io/SonosESP/) instead — that path doesn't use the network at all. Your Wi-Fi and settings are preserved unless you tick the erase option.

**What the messages mean** (visible on screen, and in the serial log):

| Message | Meaning |
|---|---|
| `Download stalled - resuming...` | No data for 30 s. The download will continue from where it stopped — not an error. |
| `Download slow - resuming...` | This attempt hit its time limit; the next one resumes. |
| `Resuming from N%...` | Reconnecting and continuing from N %. Normal. |
| `Download failed - try again later` | All 6 attempts used, or the 10-minute overall limit was reached. |
| `Download failed (HTTP …)` | The server rejected the request — check internet access. |
| `Flash write failed: …` | Could not write to flash. Use the web installer. |
| `Not enough space for OTA` | The firmware won't fit the OTA partition. |
| `No firmware URL — check for updates first` | Tap *Check for Updates* before *Install*. |

**Things that genuinely help:**
- Move the device closer to the router — the download is long and a weak signal is the usual culprit
- Avoid updating while music is streaming to the same device
- Retry at a quieter time; the release files are served by GitHub's CDN

---

## 7-inch screen: stripes, or blank after flashing

Guition changed the LCD panel partway through production without changing the
product code, so two different panels ship as the same model. They need
different settings, and nothing in the firmware can tell them apart.

**You do not need to know which one you have.** After flashing, the screen shows
colour bars and asks *"Can you read this?"*

- **Picture looks clean** (bars run edge to edge) -> tap **Looks good**. Done, it
  never asks again.
- **Screen is striped, blank, or the bars are broken** -> **do nothing**. After
  20 seconds it restarts and tries the other panel setting, then asks again.

That is the whole fix. Just wait when it looks wrong.

There is no setting to change afterwards, and that is deliberate: if the
picture is good you have nothing to change, and if it is bad you could not reach
the setting anyway. The check runs itself.

---

## Black screen from the very first install (ESP32-P4 rev 3.x)

If a brand-new 4" panel flashes and verifies without error but the screen and
backlight never come on, the board most likely has **ESP32-P4 revision 3.x**
silicon and needs the separate rev3 build.

GUITION began shipping rev3 chips under the same product code. Nothing on the
outside of the box distinguishes them.

### How to tell

Either of these answers it:

- **The web installer** — pick **4-inch (rev 3.x)** instead of **4-inch**. If
  you guessed wrong, reinstall the other one over the top; nothing is damaged.
- **The serial log**, from v2.3.2 onwards, prints it on the first few lines:

  ```
  [CHIP] ESP32-P4 rev v1.0 - image built for v0.1..v1.99 (pre-rev3 build)
  ```

  A board reporting `rev v3.x` needs the rev3 build. If the log shows nothing
  of ours at all, that is itself the symptom: the firmware is not starting.
- **esptool** reports it directly, e.g. `ESP32-P4 (revision v3.2)`.

### Why there are two builds

The two silicon revisions have **different on-chip memory maps**. Pre-rev3
splits the internal SRAM into an executable low region and a data-only high
region with a reserved gap between them; rev3 presents one contiguous
executable region. The linker therefore places code and data at different
addresses, and a binary only works at the addresses it was linked for. Same
source, same features, same panel — only the layout differs.

The rev3 build also has its own bootloader, so install the complete rev3 set
rather than mixing parts.

### Updating

OTA keeps the two apart automatically: a rev3 unit asks for
`firmware-4inch-r3.bin` and will not accept the pre-rev3 asset.

The first release to carry `firmware-4inch-r3.bin` was the nightly
`v2.3.2-nightly.5a1626c`. The Stable release v2.3.1 has no rev3 asset, so a
rev3 unit on the Stable channel reports *"v… available, no build for this
screen"* until the next Stable release ships one. That message is the safety
check working, not a fault. Auto-update follows the same rule: it never
installs an image without the rev3 asset name.

The 7" has no rev3 build yet. Its panel driver carries the same rev3 clock fix
as the 4", but nobody has a rev3 7" to test it on, so there is no
`esp32_7inch_r3` environment and no 7" rev3 asset.

**Do not hand-install the wrong one.** It is not refused and it does not roll
back by itself — recovery is a cable reinstall of the correct build.

---

## Blank screen after an update

**Most likely cause: the wrong screen variant was installed.** A 4″ build on a 7″ panel (or the reverse) initialises the wrong display driver, so the panel stays dark even though the device is running normally.

This affected 7″ units updating **from v1.9.0 or earlier**, whose updater could only find one firmware file — the 4″ one. Fixed in v1.10.0+, but the fix only helps once it's on the device, so affected units need one manual reflash.

**Fix:** install over USB with the [web installer](https://opensurface.github.io/SonosESP/) and **pick the correct screen size**. From then on updates choose the right build automatically.

---

## Wi-Fi won't connect

| Message | What it means |
|---|---|
| `Authentication failed - check password` | Wrong password. Re-enter it carefully — it's case-sensitive. |
| `Network not found` | The SSID wasn't seen. Check it's in range and broadcasting. |
| `Connection timeout` | Router didn't complete the connection. |

Points to check:
- **2.4 GHz only.** The Wi-Fi radio does not support 5 GHz. If your router uses one name for both bands, the device may be trying the 5 GHz one — temporarily splitting the bands is the reliable test.
- **Hidden networks** aren't selectable from the scan list.
- **Special characters** in the password are supported, but check the on-screen keyboard entered exactly what you intended.
- **Captive portals** (guest networks needing a web login) will not work.

Credentials are saved in NVS and survive reboots and firmware updates.

### Mesh networks

If several access points share one network name, the panel now scans every
channel and joins the **strongest** one. Earlier firmware stopped at the first
access point it heard, which on a mesh is whichever node answered quickest, not
the best one — a panel in a far room could sit on a distant node while a strong
one was in the next room. This costs about a second on connect.

To check the link, open **Settings → WiFi**. The status card shows four signal
bars and the figure in dBm, refreshed every 5 seconds while that screen is
open. Four bars is above -60 dBm, three is -60 to -67, two is -67 to -75, and
one dim bar is below -75. One bar is weak enough to explain dropped artwork
and slow updates.

### The Wi-Fi co-processor, and the version warning

The ESP32-P4 has no radio of its own. Wi-Fi runs on a second chip, an
ESP32-C6, over a link called ESP-Hosted: the P4 is the *host*, the C6 is the
*co-processor*, and each runs its own half of that software.

The two halves can be different versions. Boards ship with whatever C6 firmware
was flashed at the factory, and the host half is built into SonosESP. Measured
on a current board: the C6 reports **2.3.0**, the host is **2.12.3**.

**The gap is present on every SonosESP release so far, and Wi-Fi works on most
panels with it.** It does not explain a weak signal, and it is not a reason to
reflash the C6 when something unrelated is wrong.

It can explain **drops**. Community measurements on the same 4" board
([#196](https://github.com/OpenSurface/SonosESP/issues/196),
[field notes](https://github.com/ultramcu/guition-jc4880p443c-i-w)) found:

- some boards ship the C6 with **2.3.2**, not 2.3.0;
- on a 2.3.x C6, the first association failed with `reason=8 ASSOC_LEAVE` and
  only a retry about 20 seconds later succeeded, and the host logged
  `Dropping packet(s) from stream` on the Wi-Fi link;
- after the C6 was moved to **2.12.x**, the same board associated on the first
  try and showed no disconnects over the test period, at the same signal level.

So if your panel connects, then drops Wi-Fi again within minutes, the C6
version is a plausible cause. The SonosESP maintainers have not reproduced or
tested that fix themselves.

You will not see it in a normal serial log. The host reports it like this —

```
Version mismatch: Host [2.12.0] > Co-proc [2.3.0] ==> Upgrade co-proc ...
```

— but that is a warning-level message, and release builds compile warnings out
to save space. It is only visible in a build made with the log level raised.

**We do not currently ship a way to update the C6.** The ESP-Hosted project has
host/slave OTA examples, and the older API for it is deprecated upstream, but
nothing in SonosESP drives it and we have not tested any procedure. Updating
the co-processor firmware incorrectly can leave a board with no Wi-Fi at all,
so this page will not guess at the steps. If you have a genuine link problem,
a serial log is far more useful than a co-processor reflash.

Owners who accept that risk have documented two routes for the 4" board, both
outside SonosESP and at your own risk: a wired UART flash of the C6 through the
board's expansion header
([field notes](https://github.com/ultramcu/guition-jc4880p443c-i-w), section 1),
and an over-SDIO update pushed by a separate P4 program before any Wi-Fi
association
([#196](https://github.com/OpenSurface/SonosESP/issues/196)). The C6 image that
matches the host in SonosESP is `esp32c6-v2.12.3.bin`, which PlatformIO
installs under `framework-arduinoespressif32-libs/hosted/`. The C6 keeps its
firmware in its own flash, so reflashing SonosESP afterwards does not undo it.

---

## No Sonos speakers found

1. Confirm the device is on Wi-Fi (the settings screen shows the connection state)
2. **Sonos and the display must be on the same network/subnet.** Guest networks, VLANs and some mesh setups block the discovery traffic used to find speakers.
3. Go to **Settings → Speakers → Scan** and allow a few seconds
4. Check **AP isolation / client isolation** is off on your router — this silently blocks device-to-device traffic
5. Confirm the Sonos app on your phone sees the speakers from the same network

The panel also looks by itself: it refreshes the speaker list after boot and
every 4 hours once the screen has been idle for a few seconds, and when it
knows no speakers at all it scans again every 10 minutes. A speaker that was
switched off during the first scan therefore appears without pressing Scan.

---

## Album art doesn't appear

- Some sources simply don't provide artwork — radio stations often have none
- Very large images are rejected on purpose to protect memory
- Occasional misses after long uptime are usually memory pressure; a reboot restores it
- A placeholder music note means "no artwork available", not a fault

---

## Lyrics don't appear

- Enable them in **Settings → General → Show synced lyrics**, or with **Lyrics on/off** under the artwork in the Amber player. Both change the same setting
- Lyrics come from [LRCLIB](https://lrclib.net/) and **only time-synced lyrics are shown**. Many tracks have none — this is the usual reason.
- Matching relies on the artist/title reported by Sonos; live or remastered titles often fail to match
- Radio and TV/line-in sources have no lyrics

---

## The screen flickers during an update

**This is a known hardware limitation, not a bug we can currently fix.**

While firmware is written to flash, the processor's cache must be switched off. The display's image lives in external memory reached through that cache, so for those moments the panel is fed invalid data and flickers.

The feature that would avoid this is flash ["auto-suspend"](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/spi_flash/spi_flash_optional_feature.html), which lets the cache keep running through a write. The ESP32-P4 itself supports it. The flash chip fitted to these panels does not — they ship a Boya BY25Q, and [ESP-IDF's own driver for that part](https://github.com/espressif/esp-idf/blob/master/components/spi_flash/spi_flash_chip_boya.c) records suspend as unavailable in so many words. Espressif lists only three supported families: XM25xxD, GD25QxxE and FM25Q32.

**A board with one of those chips would not necessarily fix it either.** Espressif documents auto-suspend as unsuitable for workloads with a "high requirement of real-time system or triggering interrupt very frequently (e.g. LCD flush, bluetooth, Wi-Fi, etc.)" — and an update is a Wi-Fi download, a screen refresh and a flash write happening at once, which is precisely that case. So this is less "the wrong chip was fitted" and more "no current option makes writing firmware invisible to the display."

Your board reports its own chip in the serial log at boot:

```
[FLASH] Boya BY25Q 16MB (0x684018) - Auto-suspend: NO
```

It's harmless: let the update finish and the device reboots normally.

---

## The panel restarted on its own

Since v2.0.6 the panel keeps a record. **Settings → General → Device → Recent restarts** lists the last eight, newest first, with when each happened and how long the panel had been running. The ones in gold are worth reporting:

| Shown as | What happened |
|---|---|
| **Crashed** | A firmware fault. The next boot prints a coredump summary in the [serial log](#how-to-get-a-serial-log) |
| **Froze (watchdog)** | Something stopped responding and the watchdog restarted the panel |
| **Power dipped** | The supply sagged. Try another USB power adapter or cable |
| **Low memory (self-restart)** · **Low memory after reconnect** | The memory Wi-Fi needs ran low, and the panel restarted to recover |
| **Wi-Fi did not reconnect** | Wi-Fi stayed down, and the panel restarted to try again |
| **Update failed, recovered** | An update did not complete, and the panel restarted to recover |

Everything else — **You restarted it**, **Firmware update**, **Powered on**, **USB connected** and the like — is expected. **USB connected** is what opening a serial monitor looks like.

If gold entries keep appearing, [open an issue](https://github.com/OpenSurface/SonosESP/issues) with a photo of the list and, if you can, a serial log from the next boot.

### Crashed, with task `sdio_read` or `sdio_rx_buf`

If the coredump summary in the serial log looks like this:

```
[COREDUMP]   task   : sdio_rx_buf        (or sdio_read)
[COREDUMP]   PC     : 0x4FF1xxxx
[COREDUMP]   mcause : 0x00000002   mtval : 0x00000000
```

it is a **known crash in the Wi-Fi co-processor driver**, not in SonosESP's own
code ([#200](https://github.com/OpenSurface/SonosESP/issues/200),
[#208](https://github.com/OpenSurface/SonosESP/issues/208)). `mcause 0x2` is an
illegal instruction, which is how a failed assert ends on this chip, and a PC
in `0x4FF1xxxx` is internal RAM, where the precompiled ESP-Hosted driver runs.
The assert fires when the driver's receive buffer pool runs out. Reports so far
came after 3.5 hours and after 1 day 20 hours of uptime, on both the 4" and
the 7".

**There is no fix in current releases.** SonosESP ships ESP-Hosted **2.12.3**
inside the precompiled Arduino libraries. Upstream **2.12.13** replaced that
assert with a log-and-drop, and it can only arrive with a newer Arduino core,
which has not been tested on these panels yet. The firmware already spaces out
its network traffic to keep that buffer pool from running dry (the cooldowns in
`src/ui_network_guard.cpp`), which makes the crash rarer but cannot remove it.

What happens when it fires: the panel restarts by itself, records **Crashed**
in Recent restarts, and reconnects to the remembered speaker, in about
40 seconds in the #200 log. You do not need to do anything.

What helps the report:

- the `[COREDUMP]` lines from the next boot, and the firmware version;
- the `build :` value on that line, which identifies the exact firmware image;
- the Wi-Fi signal from **Settings → WiFi**;
- whether the C6 has ever been updated (see
  [the co-processor section](#the-wi-fi-co-processor-and-the-version-warning)).

---

## The panel's own battery

The battery levels SonosESP shows belong to **Sonos portables** (Move, Roam),
read from the speaker over the network. The panel cannot show the charge of a
LiPo attached to the panel itself
([#213](https://github.com/OpenSurface/SonosESP/issues/213)).

On the 4" board (JC4880P443C) the battery connector feeds an **IP5306**
charger and boost chip. In the vendor schematic, sheet 02, none of its pins
reach the ESP32-P4: the battery line has no voltage divider to an ADC input,
the charge-state pins (`LED1` to `LED3`) and `KEY` are not connected to
any P4 GPIO, and no I²C lines run to the chip. The firmware therefore has
nothing to read. We have not found a published schematic for the 7" board
(JC1060P470C).

Two ways to see the charge today:

- the IP5306's own LEDs, where your board has them fitted;
- a hardware change: a resistor divider from `BAT+` to a free P4 ADC pin on the
  expansion header. If you build one, open an issue with the pin and resistor
  values and the firmware side can be added.

---

## Start completely fresh

To wipe all settings (Wi-Fi, speaker choice, theme, clock and display preferences):

1. Open the [web installer](https://opensurface.github.io/SonosESP/)
2. Pick your screen size
3. Tick the **erase device** option when prompted
4. After flashing, set up Wi-Fi again from scratch

---

## How to get a serial log

The log almost always names the exact failure, and it's the single most useful thing to attach to an issue.

1. Connect the device to your computer (see [the USB section](#the-computer-cant-see-the-device) if it isn't detected)
2. Open any serial monitor at **115200 baud** — Arduino IDE, PuTTY, `screen`, or:
   ```bash
   pio device monitor -b 115200
   ```
3. Reproduce the problem
4. Copy the output into your issue

Useful lines to look for: `[OTA]`, `[WIFI]`, `[SONOS]`, `[ART]`, `[CLOCK]`, `[REBOOT]`, `[BATT]`, `[SLEEP]`.

---

## Still stuck?

[Open an issue](https://github.com/OpenSurface/SonosESP/issues) with:

- What you were doing and what happened
- Your screen size (4″ or 7″) and firmware version (**Settings → Update**)
- A serial log if you can get one
- If it restarted by itself, a photo of **Settings → General → Recent restarts**
- A photo of the screen, if the problem is visual
