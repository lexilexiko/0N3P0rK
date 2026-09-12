# 0N3P0rK — Full project guide & history

**Current version: 1.3.0**  
Firmware for **M5Cardputer** / **Cardputer ADV** (ESP32-S3).

**Idea in one line:** a living pig on a small farm (Tamagotchi-style), and a Wi‑Fi / radio lab in the same barn.

> Think Tamagotchi first. The radio is in the barn.

This document is the **full** project picture: what the device is, how to flash it, how to use the main menus, and **what changed from early builds through 1.3.0**.  
Secret menu codes are **not** listed here (keep them private).

---

## Table of contents

1. [Hardware](#hardware)
2. [Flash & build](#flash--build)
3. [First minutes](#first-minutes)
4. [Basic controls](#basic-controls)
5. [Farm & pig](#farm--pig)
6. [Seasons & unlock roadmap](#seasons--unlock-roadmap)
7. [Radio & tools](#radio--tools)
8. [PigPass](#pigpass)
9. [SD layout](#sd-layout)
10. [Web site](#web-site)
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
esptool.py --chip esp32s3 --port COMx write_flash 0x0 0N3P0rK_v1.2.8_*_Full.bin
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

### Updating from an older build

1. Back up the SD card, especially `/0N3P0rK/handshakes/`, `Passworld/`, and any
   sync credentials.
2. Flash the new `firmware.bin`.
3. If the partition table changed since the previous installation, erase flash
   once before flashing.
4. Insert the SD card and reboot.
5. Open **SET → STATUS** and confirm that the displayed firmware version is
   `1.3.0`.

Existing SD captures are not removed by a firmware update. NVS settings are
loaded with compatibility defaults when an older configuration does not contain
newer fields.

---

## First minutes

1. Insert a **FAT32** microSD before boot (loot / talk / wordlists).
2. Device boots to the **farm** with the pig.
3. Open **SETTINGS** from the menu.
4. Use **RADIO** for capture, **PIGPASS** for offline crack, **LOOT** / file manager for SD files.
5. Play on the farm: walk, jump, seasons, wolf, XP — features unlock as the level grows.

## Basic controls

The exact key labels are shown in the bottom hint bar and may be changed under
**SET → KEYS**.

| Control | Typical action |
| --- | --- |
| `;` / `.` | Move the selection up / down |
| `ENTER` | Open, confirm, or start the selected item |
| `` ` `` | Go back or close the current page |
| `SPACE` | Farm attack-hop; in Spectrum, perform the current action |
| `Z` | Skip the current radio target for this capture session |
| `G` / `0` | Dim or suspend the farm presentation while supported radio work continues |

### Main menu flow

1. Open **ATTACK** from the main menu.
2. Choose **LIGHT** for passive capture, **AGGRO** for active capture on
   authorized networks, or **STOP** to stop the radio.
3. Use **SET → RADIO** to configure hopping, locking, deauthentication,
   handshake method, capture format, and targeting behavior.
4. Use **LOOT** to inspect files and synchronize captures with WPASec or
   Pwncrack.
5. Use **SET → STATUS** to check board, SD, Wi-Fi, heap, and firmware state.

Only test networks and devices that you own or are explicitly authorized to
assess. Active radio features must not be used against third-party networks.

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
- Methods + packs (including dedicated tuning paths for PCAP vs 22000)  
- Modes such as aggressive / pinned targeting  
- Skip network (e.g. **Z**) — ignore for the current session  
- Bottom bar status while capturing  
- Loot on **SD** under the project tree  

Capture workflow:

1. Insert the SD card before starting the radio.
2. Select **ATTACK → LIGHT** or **ATTACK → AGGRO**.
3. Leave the device running while it scans and records valid handshake
   exchanges.
4. Press **STOP** before opening **LOOT**. The capture buffers are flushed and
   released before file synchronization.
5. Open **LOOT** and select the WPASec or Pwncrack tab.
6. Use the upload action for captured files. Use the result/download action
   only after the remote service is reachable.

The capture path writes classic PCAP files and prepares Hashcat 22000 material
when enough valid handshake data is available. Incomplete, oversized, or
invalid files are rejected instead of being presented as successful captures.

### Radio configuration

The **SET → RADIO** page contains the stable radio controls:

- channel hop set and dwell timing;
- lock timing and lock-on-handshake behavior;
- handshake method and fallback selection;
- deauthentication and EAPOL/PMKID options;
- RSSI filtering and kick burst count;
- PCAP format and maximum capture size;
- handshake depth and target hold behavior.

The **PACK** selector applies a tested group of radio values. Editing
individual values marks the profile as custom. **RADIO → RESET** restores the
stock radio profile without deleting files from the SD card.

### Spectrum

Open **ATTACK → SPECTRUM** to view nearby 2.4 GHz activity, detected access
points, clients, channels, authentication type, and PMF information. Press
`ENTER` on a network to lock its view. Spectrum can suspend the farm scene to
reduce CPU work while it is active.

### Other modes

| Mode | Role |
| --- | --- |
| **Spectrum** | Channel / air view; scene can suspend to save CPU |
| **PigPass** | Offline PSK try from captures + wordlist |
| **EvilPig** | Portal-style lab tool |
| **BLE / IR / USB SD** | Extra toys as implemented |
| **Loot / File manager** | Browse SD; manager is SD-only |

---

## PigPass

- Tabs: **PCAP** and **22000**  
- Larger file lists for wordlists / captures  
- While open: farm **scene suspended** (like Spectrum minimize idea); resume when closed / minimized as designed  
- Results / checkpoints on SD under `pigpass/`

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

Approximate product line from early Methodik / handshake-first builds to current.  
Patch numbers may match tags you used in git; the **story** is what matters.

### Early line (pre–1.2 / Methodik roots)

- Pig farm UI on Cardputer  
- Wi‑Fi sniffer focused on **handshake** catch  
- Loot on storage, basic menus  
- Influence / parallel ideas from the wider Cardputer & handshake scene (including **Oct0sec / M5PORKCHOP**-class projects as reference for “catch HS first”)

### 1.2.x radio focus

- Handshake-first radio kept and tightened  
- Methods / packs for different capture styles  
- Hashcat **22000** path alongside classic PCAP  
- Focus on **not deleting good captures**, less junk files  
- Skip-network behavior, status bar clarity  

### ~1.2.5–1.2.6

- Stability passes on sniffer write path  
- Settings / radio menu polish  
- Pack & method pairs aimed at PCAP vs 22000  

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

### 1.3.0 (current)

#### Capture and stability

- Reworked the continuous capture lifecycle so capture memory is allocated
  when the radio starts and released when it stops.
- Added bounded capture queues and deferred SD writes to reduce callback
  pressure, watchdog resets, and screen corruption during radio operation.
- Preserved the existing Light, Aggressive, and Pinned capture entry points.
- Added safer frame, PMF, RSN, and handshake bounds checking.
- Improved handshake assembly so M1/M2 and optional M3/M4 depth handling do not
  create malformed output.
- Kept capture files small and compatible with the WPASec upload limits.

#### Loot and synchronization

- Repaired WPASec multipart uploads, including the expected `webfile` field.
- Streamed uploads with read validation and short-file checks.
- Reworked the Loot entry path to stop the capture pipeline and reclaim heap
  before synchronization.
- Fixed Pwncrack result downloads to use the HTTPS endpoint directly instead of
  depending on a redirect from HTTP.
- Added clearer failure stages for connection, HTTP, empty-result, and HTML
  responses.
- Kept WPASec and Pwncrack files under the SD project directory so they remain
  available offline.

#### Radio and user interface

- Added clearer RADIO method and pack handling while keeping saved method
  numbering compatible with previous builds.
- Added radio reset behavior that restores the stock profile without touching
  SD captures.
- Improved status bars, target locking, session skip behavior, and capture
  lifecycle feedback.
- Added runtime heap cleanup for offline 22000 conversion and synchronization.

#### Compatibility and build

- Updated the firmware version to `1.3.0`.
- Verified the PlatformIO build for the M5Stack StampS3 target.
- Kept the same firmware target for the original M5Cardputer and Cardputer
  ADV hardware.

---

## Legal & credits

For **education and authorized testing** only. You are responsible for how you use radio features.

License: see `LICENSE`.  
Not affiliated with M5Stack.

**Thanks** to everyone who tested builds, to the Cardputer community, and to **Oct0sec** for handshake-path inspiration.

**0N3P0rK** — oink responsibly.
