# 0N3P0rK — Full project guide & history

**Current version: 1.2.8**  
Firmware for **M5Cardputer** / **Cardputer ADV** (ESP32-S3).

**Idea in one line:** a living pig on a small farm (Tamagotchi-style), and a Wi‑Fi / radio lab in the same barn.

> Think Tamagotchi first. The radio is in the barn.

This document is the **full** project picture: what the device is, how to flash it, what every major area does, and **what changed from early builds through 1.2.8**.  
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
10. [Version history](#version-history)
11. [Legal & credits](#legal--credits)

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

### 1.2.8 (current)

- **Scene modularization:** `sky`, `ground`, trees, FX  
- **CITY** & **DESERT** seasons  
- **Seasonal props** + game-day / off-screen rules  
- **Friend pig**, **cards table** stub, **lv50 credits**  
- PigPass tabs + scene suspend  
- Cleaner public site + automatic gallery loading  
- README without public secret codes  

---

## Legal & credits

For **education and authorized testing** only. You are responsible for how you use radio features.

License: see `LICENSE`.  
Not affiliated with M5Stack.

**Thanks** to everyone who tested builds, to the Cardputer community, and to **Oct0sec** for handshake-path inspiration.

**0N3P0rK** — oink responsibly.

