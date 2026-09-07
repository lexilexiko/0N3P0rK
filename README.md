# 0N3P0rK - Full project guide

**Current version: 1.3.0**
Firmware for **M5Cardputer** / **Cardputer ADV** (ESP32-S3).

**Idea in one line:** a living pig on a small farm (Tamagotchi-style), and a Wi‑Fi / radio lab in the same barn.

> Think Tamagotchi first. The radio is in the barn.

This document is the **full** project picture: what the device is, how to flash it, what every major area does, and what changed from the early builds to the current 1.3.0 release.
Secret menu codes are **not** listed here (keep them private).

---

## Table of contents

1. [Hardware](#hardware)
2. [Flash & build](#flash--build)
3. [First minutes](#first-minutes)
4. [Farm & pig](#farm--pig)
5. [Seasons & unlock roadmap](#seasons--unlock-roadmap)
6. [Radio & tools](#radio--tools)
7. [PigPass](#pigpass)
8. [SD layout](#sd-layout)
9. [Web site](#web-site)
10. [LED status scene](#led-status-scene)
11. [Version history](#version-history)
12. [Legal & credits](#legal--credits)

---

## Hardware

| | |
| --- | --- |
| Boards | **M5Cardputer** (original) and **M5Cardputer ADV** |
| MCU | ESP32-S3 (StampS3), 240 MHz |
| Flash | 8 MB — large app partition; internal LittleFS **512 KB** (not used for user loot) |
| Display | 240 × 135 ST7789 — top bar + farm field + bottom bar |
| Keyboard | Original: 74HC138 matrix · ADV: TCA8418 |
| USB | CDC serial — pick COM yourself (VID `303A`) |
| SD | SPI: CS **12**, MOSI **14**, MISO **39**, SCK **40** |

Same firmware `.bin` for original and ADV.  
After changing partition tables from older builds: **erase flash once**, then flash.

All handshakes, wordlists, talk files, and the file manager live on **SD** (not internal flash).

---

## Flash & build

### Ready binary

```text
esptool.py --chip esp32s3 --port COMx write_flash 0x0 0N3P0rK_v1.3.0_*_Full.bin
```

Or **M5Launcher** with a `*Launcher*.bin`.

### Web installer

[lexilexiko.github.io/0N3P0rK](https://lexilexiko.github.io/0N3P0rK/) — Chrome / Edge / Opera (Web Serial).  
Lists `.bin` under `docs/firmware/` (Direct/Full vs Launcher labels).

### From source (PlatformIO)

```text
pio run
pio run -t upload --upload-port COMx
```

- Platform: `espressif32@6.12.0`  
- Artifact: `.pio/build/m5cardputer/firmware.bin`  
- Version injected from `platformio.ini` → `custom_version`

---

## First minutes

1. Insert a **FAT32** microSD before boot (loot / talk / wordlists).
2. Device boots to the **farm** with the pig.
3. Open **SETTINGS** from the menu.
4. Use **RADIO** for capture, **PIGPASS** for offline crack, **LOOT** / file manager for SD files.
5. Play on the farm: walk, jump, seasons, wolf, XP — features unlock as the level grows.

---

## Farm & pig

### Scene (how the picture is built)

Z-order (back → front), modular files under `src/piglet/`:

| Module | Role |
| --- | --- |
| **sky** | Day/night gradient, moon, stars |
| **weather** | Clouds, rain, snow precip |
| **seasonal_fx** | Leaves, snow/sand banks, sandstorm, lightning FX |
| **ground** | Grass / pavement / sand dunes + treadmill scroll |
| **trees** | Trees, bushes, CITY stall/trash/lamp, DESERT palms/cactus |
| **props** | Seasonal daily objects (hive, snowman, fox, fire, …) |
| **avatar** | Player pig + movement |
| **friend_pig** | Companion pig (lv 40+) |
| **cards_table** | Farm table for future card game (lv 45+) |
| **wolf** | Visitor; can target player or friend |
| **mood** | Stats, speech bubbles, monologues |
| **credits** | Level-50 thank-you roll |

### Controls (farm, typical)

- Walk / edge scroll, jump, attack-hop, sit, play-dead  
- **ANIM TEST** (SCENE): cycle demos with `-` / `=` on the farm  
- **G0**: screen/sound off while some radio work can continue (as designed for capture)

### Personality / SCENE menu (highlights)

- Name, skin, season, sky mode, scroll speed  
- **LIFE** — pig keeps living while tools run  
- Layer toggles (grass, trees, weather, mood, wolf…)  
- **TALK SEC** — monologue interval  
- **PROPS** / **FRIEND** on/off (when unlocked)  
- **CODE** — private unlock strings (not documented publicly)

### SYSTEM and TASKS

The **SYSTEM** page contains display, sound, dimming, and LED controls. Its **TASKS** panel is a runtime dispatcher for background work:

| Task | Effect when disabled |
| --- | --- |
| **CAPTURE** | Stops the active sniffer and prevents capture processing |
| **NETWORK** | Turns off Wi-Fi when disabled; can be restored from the same panel |
| **LED LOOP** | Stops LED animation updates |
| **SOUND** | Stops background sound updates |
| **XP** | Stops background XP ticking |
| **SCENE** | Fully parks the pig scene: the pig, wolf, weather, ambient layers, movement, and scene polling stop |

All tasks are enabled by default. Settings are saved in NVS and the panel can also be opened with a user-assigned **TASKS** hotkey from **KEYS**. The display remains available so the device can always be controlled.

### XP

- Soft early ramp; from mid-levels a **flat** cost so late levels stay reachable  
- **Max level 50**

---

## Seasons & unlock roadmap

### Seasons

| Season | Feel |
| --- | --- |
| Spring / Summer / Autumn / Winter | Classic farm + FX |
| **RETRO** | Old-film mono look |
| **NOIR** | Night alley mood |
| **CITY** (lv 25) | Urban ground, stall, trash, lamp, dirty skin option |
| **DESERT** (lv 30) | Sand dunes, palms, cactus, sandstorm, coyote palette, no rain, bright sky |

### Level roadmap

| Level | Unlocks |
| ---: | --- |
| Early | Skins, gold apples, core farm |
| 15 / 18 | RETRO / NOIR |
| **25** | **CITY** |
| **30** | **DESERT** |
| **35** | Seasonal **props** system |
| **40** | **Friend** pig |
| **45** | **Cards table** (jump → stub: cards not ready) |
| **50** | **Credits** (~10 s, cannot skip) |

### Seasonal props (once per **game-day** ≈ 360 s)

Spawn **off-screen** ahead of walk; toast only when visible.

| Season | Object |
| --- | --- |
| Summer | Hive + bees (chase when near) |
| Winter | Snowman (jump to break) |
| Autumn | Sleeping fox + Zzz (leaves when you leave) |
| Spring | Campfire after storm lightning (burns ~½ game-day) |
| City | Box + stray cat |
| Desert | Sand skull |

**ANIM TEST** can force prop demos without spending the daily slot.

---

## Radio & tools

### Handshake capture (Cap)

- Main goal: catch handshakes (PCAP / hashcat **22000** material)  
- **LIGHT (L)**: passive and low-noise capture. It hops slowly between channels, using a default dwell of about 30 seconds per channel.
- **AGGRESSIVE (A)**: active handshake hunting with faster channel work and configured deauthentication behavior.
- PCAP files are appended safely instead of being blindly overwritten.
- Bottom bar status while capturing  
- Loot on **SD** under the project tree  

The current radio design uses the two capture modes above plus shared RADIO and RADIO PRO settings. The older standalone capture-method registry was removed; old method names and method-selection screens are no longer part of the firmware.

### RADIO and RADIO PRO

**RADIO** contains everyday controls:

- `HOP MS` - channel dwell time, from 50 ms to 60 seconds
- `LOCK MS` and `LOCK HS` - handshake lock behavior
- `HOP SET` - all channels, priority channels, or the core 1/6/11 set
- RSSI threshold, random MAC, kick count, bidirectional kick, PMKID, and related controls

**RADIO PRO** contains advanced capture and storage controls:

- `AUTO REPAIR` repairs an incomplete PCAP tail before appending
- `ROLLBACK` removes a partially written packet after a short SD write
- `FRAME LIM` selects a 256- or 512-byte stored frame limit
- `RING N`, `FLUSH N`, and `W RETRY` tune the capture ring and SD write behavior
- `MAGIC CHK`, `SIZE VER`, `PROTECT`, `LEARN REN`, and `MIGRATE` protect and normalize capture files
- `LOG SD` and `SHOW DROP` expose optional diagnostics
- `FLUSH NOW` and `CAP TEST` provide maintenance and PCAP self-test actions

`AUTO REPAIR` and `ROLLBACK` are enabled by default. The `SAFE` pack is a selectable ready-made profile for conservative PCAP protection and frequent SD flushing. It is selected from the normal `PACK` control and applies the complete profile at once. `CAP PERF` is a selectable ON/OFF lower-memory capture profile: a 256-byte frame limit, a smaller ring, reduced radiotap/data-activity work, and fewer optional SD/UI diagnostics. Editing any pack knob changes the pack indicator to `CUSTOM`. `RESET PRO` restores the normal radio profile.

### Capture file safety

Captures are stored under `/0N3P0rK/handshakes/`. On open, the firmware validates the classic PCAP header, repairs a truncated final packet when enabled, and appends only after the file is known to be usable. A recovery copy is kept during repair so a failed replacement does not silently destroy the original. The `LOOT` diagnostics view can inspect PCAP headers and packet data.

### Other modes

| Mode | Role |
| --- | --- |
| **Spectrum** | Channel / air view; scene can suspend to save CPU |
| **PigPass** | Offline PSK try from captures + wordlist |
| **EvilPig** | Portal-style lab tool |
| **BadUSB / BadBLE** | Authorized HID automation over native USB or BLE |
| **BLE / IR / USB SD** | Extra tools as implemented |
| **Loot / File manager** | Browse SD; manager is SD-only |

### BadUSB / BadBLE

BadUSB is a HID automation tool for **authorized lab work and your own devices**. It can emulate a keyboard through the Cardputer's native USB HID interface. BadBLE provides the same style of keyboard actions over Bluetooth Low Energy.

The mode has three tabs:

- **SCRIPTS** - select and run DuckyScript-style `.txt` files from `/0N3P0rK/badusb/`
- **LIVE** - arm the keyboard bridge and send Cardputer key input to the connected target
- **PANEL** - use built-in PC or phone-oriented key presets

The transport and profile can be changed inside the mode:

- `U` selects USB BadUSB
- `B` selects BLE BadBLE
- `P` switches between PC and phone profiles
- `C` starts or reconnects the HID link
- `;` / `.` selects an item
- `ENT` runs a script, fires a preset, or arms LIVE mode
- `` ` `` exits the mode; `FN+`` ` `` disarms LIVE mode

Scripts are limited to `.txt` files and the firmware lists up to 16 files at once. USB waits for a host connection without freezing the menu. BLE waits for pairing/connection and turns Wi-Fi off while the BLE HID transport is active. Starting BadUSB/BadBLE stops an active capture before switching to HID operation.

The separate **USB SD** mode is not BadUSB: it exposes the SD card as storage to a computer and must be selected independently.

---

## PigPass

- Tabs: **PCAP** and **22000**  
- Larger file lists for wordlists / captures  
- While open: farm **scene suspended** (like Spectrum minimize idea); resume when closed / minimized as designed  
- Results / checkpoints on SD under `pigpass/`

---

## LED status scene

The built-in RGB LED is both a status indicator and a small part of the farm scene:

- Handshake files trigger short green confirmation blinks.
- An active wolf produces a slow red breathing alert in the farm.
- Season colors provide the ambient palette.
- Night mode is about 10% brighter than daytime.
- Day and morning remain softer to reduce glare and battery use.
- Normal farm mode uses a slow breathing effect instead of a simple static blink.

LED brightness and the LED loop can be controlled from **SYSTEM** and **TASKS**. Turning off the LED loop does not affect capture or display.

---

## SD layout (typical)

```text
/0N3P0rK/
  handshakes/     captures
  pigpass/        crack state / results
  Passworld/      wordlists
  talk/           optional monologue lines
  evilpig/        portal-related files
  wolf/           wolf loot stash (if used)
  …
```

Exact folder names follow `src/core/sd_layout.h`.

---

## Web site (`docs/`)

Public pages (English):

- **Information** — what it is, requirements, links  
- **Installation** — web flasher (no internal “how the repo is laid out” noise for visitors)  
- **Gallery** — images from `docs/gallery/` (png/jpg/gif/webp), discovered via folder listing or GitHub API  
- **Donate** — placeholder  

Put screenshots in **`docs/gallery/`** on the **default branch**, then refresh Pages.

---

## Version history

Approximate product line from early handshake-first builds to current.
Patch numbers may match tags you used in git; the **story** is what matters.

### Early line (pre–1.2 / Methodik roots)

- Pig farm UI on Cardputer  
- Wi‑Fi sniffer focused on **handshake** catch  
- Loot on storage, basic menus  
- Influence / parallel ideas from the wider Cardputer & handshake scene (including **Oct0sec / M5PORKCHOP**-class projects as reference for “catch HS first”)

### 1.2.x radio focus

- Handshake-first radio kept and tightened  
- Hashcat **22000** path alongside classic PCAP
- Focus on **not deleting good captures**, less junk files
- Clear Light/Aggressive capture status

### ~1.2.5–1.2.6

- Stability passes on sniffer write path  
- Settings / radio menu polish  
- PCAP and 22000 workflow improvements

### 1.2.7

- **SD-only** user storage (internal flash not a loot FS)  
- LittleFS partition shrunk (~512 KB)  
- Pig monologues refresh  
- Web installer site: discover `.bin` by extension, tabs Information / Installation / Gallery / Donate  
- Mood / scene quality-of-life  

### 1.2.8

- **Scene modularization:** `sky`, `ground`, trees, FX  
- **CITY** & **DESERT** seasons  
- **Seasonal props** + game-day / off-screen rules  
- **Friend pig**, **cards table** stub, **lv50 credits**  
- PigPass tabs + scene suspend  
- Cleaner public site + automatic gallery loading   

### 1.3.0 (current release)

- Light mode channel hopping with a slow default dwell
- Configurable `HOP MS` range up to 60 seconds
- New capture pipeline with separate LIGHT and AGGRESSIVE behavior
- PCAP tail repair before append
- Rollback after short or partial SD writes
- PCAP safety controls and self-test in RADIO PRO
- Configurable 256/512-byte frame limit
- `CAP PERF` lower-memory capture profile
- SYSTEM task dispatcher with persistent runtime toggles and a TASKS hotkey
- Full SCENE suspension for stopping the pig, movement, wolf, weather, and farm animation work
- BadUSB and BadBLE HID automation with scripts, LIVE mode, and PC/phone presets
- Expanded LED scene: wolf red alert, day/night brightness, season colors, and breathing ambient effects
- Many new sniffer controls for hopping, handshake locking, ring buffering, SD flushing, retries, diagnostics, and file protection

---

## Legal & credits

For **education and authorized testing** only. You are responsible for how you use radio features.

License: see `LICENSE`.  
Not affiliated with M5Stack.

**Thanks** to everyone who tested builds, to the Cardputer community, and to **Oct0sec** for handshake-path inspiration.

**0N3P0rK** — oink responsibly.
