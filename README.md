# 0N3P0rK — Project guide

**Current version: 1.3.6f**
Firmware for **M5Cardputer** / **Cardputer ADV** (ESP32-S3).

**Idea in one line:** a living pig on a small farm (Tamagotchi-style), and a Wi‑Fi / radio lab in the same barn.

> Think Tamagotchi first. The radio is in the barn.

This guide covers the current device features, setup, and everyday use.
For the chronological list of changes, see the
[version history](README_HISTORY.md).
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
8. [BadUSB and BadBLE](#badusb-and-badble)
9. [LED indicator](#led-indicator)
10. [XFER file transfer](#xfer-file-transfer)
11. [FILES — SD File Manager](#files--sd-file-manager)
12. [Music (MP3 player)](#music-mp3-player)
13. [InspectorPig](#inspectorpig-inspect)
14. [PigPass](#pigpass)
15. [SD layout](#sd-layout)
16. [Web site](#web-site)
17. [Version history](#version-history)
18. [Legal & credits](#legal--credits)

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
esptool.py --chip esp32s3 --port COMx write_flash 0x0 0N3P0rK_v1.3.6f_*_Full.bin
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
   `1.3.6f`.

Existing SD captures are not removed by a firmware update. NVS settings are
loaded with compatibility defaults when an older configuration does not contain
newer fields.

---

## First minutes

1. Insert a **FAT32** microSD before boot (loot / talk / wordlists).
2. Device boots to the **farm** with the pig.
3. Open **SETTINGS** from the menu.
4. Use **RADIO** for capture, **LOOT** for captures and service sync, and
   **FILES** to browse and manage SD files.
5. Play on the farm: walk, jump, seasons, wolf, XP — features unlock as the level grows.

## Basic controls

The exact key labels are shown in the bottom hint bar and may be changed under
**SET → KEYS**.

| Control | Typical action |
| --- | --- |
| `^` / `v` | Move the selection up / down |
| `ENT` | Open, confirm, or start the selected item |
| `ESC` | Go back or close the current page |
| `SPACE` | Farm attack-hop; in Spectrum, perform the current action |
| `Z` | Skip the current radio target for this capture session |
| `G` / `0` | Dim or suspend the farm presentation while supported radio work continues |

### Main menu flow

1. Open **ATTACK** from the main menu.
2. Choose **LIGHT** for passive capture, **AGGRO** for active capture on
   authorized networks, or **STOP** to stop the radio.
3. Use **SET → RADIO** to configure hopping, locking, deauthentication,
   handshake method, capture format, and targeting behavior.
4. Use **LOOT** to inspect files and synchronize captures with WPASec,
   Pwncrack, or OnlineHashCrack.
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
| **cards_table** | Playable card-duel table (lv 45+) |
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
- **UNDERGROUND** on/off for the animated ant-farm footer
- **CODE** — private unlock strings (not documented publicly)

### Living underground

The bottom bar can show a scrolling underground scene with ants that wander
through branching tunnels. Tunnels appear, age, and are dug again. Thick,
forked tree and bush roots follow the plants as they grow, collapse, and scroll;
small grass roots remain in every season. Seasonal soil colors match the scene.
City props such as lamps, stalls, and trash cans have no roots. Toggle the whole
scene under **PIG → UNDERGROUND**.

### Card duel

At level **45**, jump onto the farm card table to start a playable duel. Choose
two cards from the five-card hand; the rival chooses its own pair. Attack,
defense, healing, and occasional combo cards resolve in two exchanges. Win two
rounds to win the match.

The duel shows current/max HP, round and match score, card strength, and a
mirrored combat summary for both sides. The previous turn's damage and healing
remain visible while choosing the next hand. Cards animate with fire, shield,
or healing-drop effects; attacks, defenses, and heals have distinct impact
animations and sound cues.

| Key shown in the duel hint | Action |
| --- | --- |
| `1`–`5` | Select or unselect a card; choose two |
| `ENT` | Play the selected cards, continue after a round, or confirm the result |
| `ESC` | Leave the duel |

The on-screen hint changes with the duel phase, for example
`1-5 PICK 2 CARDS`, `1-5 CARD ENT PLAY`, and `ENT NEXT`; press `ESC` to leave.

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
| **45** | **Playable card duel** |
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
5. Open **LOOT** and select the WPASec, Pwncrack, or OHC tab.
6. Use the upload action for captured files. Use the result/download action
   only after the remote service is reachable (WPASec / Pwncrack only — see
   the OnlineHashCrack note below).

The capture path writes classic PCAP files and prepares Hashcat 22000 material
when enough valid handshake data is available. Incomplete, oversized, or
invalid files are rejected instead of being presented as successful captures.

### Checking a capture before upload (LOOT → `i` / `I`)

The handshake inspector is wired straight into **LOOT**, so the capture already
in front of you can be vetted without leaving the list:

| Key | Action |
| --- | --- |
| `i` | Check the highlighted capture → `/0N3P0rK/inspector/<capture>.txt` |
| `I` | Check **every** capture in `/0N3P0rK/handshakes/` → `/0N3P0rK/inspector/report.txt` |

Both keys work from the list and from the open card (`ENT`), and both answer
with a toast: `GOOD 92/100 SAVED`, or `12 FILES  OK 9` for the whole folder.

The run is **headless**: no mode switch, no list, no report screen — the
inspector's view buffers are never allocated for it. Press `i` before `U` / `S`
and the upload only ever carries a capture that really holds a handshake.

### OnlineHashCrack (OHC tab)

The third **LOOT** tab uploads captures to the public OnlineHashCrack WPA API.

| | |
| --- | --- |
| Endpoint | `POST https://api.onlinehashcrack.com` (multipart) |
| Credential | the email of an existing OnlineHashCrack account — **no API key** |
| Fields | `email`, then `file` (`.cap` / `.pcap` / `.pcapng`, up to 200 MB) |
| Setup | drop the address in `/0N3P0rK/ohc/email.txt` |

`GET` / `POST` notes:

- The tab lists the same `.pcap` captures as **WPASEC**, but points at a
  different service, so a capture can be sent to both.
- Switch tabs with `<` (back) and `>` (forward); the order is
  WPASEC → PWNCRACK → OHC.
- `S` uploads every capture that was not sent yet, `U` uploads the selected
  one. `D`, `R`, `T` and the paging keys work exactly as in the other tabs.
- Rows show `[--]` for local and `[..]` for already sent. This tab never shows
  `[OK]`: see the limitation below.
- WPA captures (hashcat mode 22000) do **not** count against the account's
  monthly task quota.

**Results cannot be downloaded.** OnlineHashCrack deletes the uploaded capture
immediately after extracting the PMKID/EAPOL hash and publishes the outcome in
the web dashboard (`https://onlinehashcrack.com/tasks`) and by email. There is
no potfile endpoint, unlike WPASec and Pwncrack — so `Q` / *pull results* only
raises a notice on this tab. Check the dashboard for cracked passwords.

Uploads are acknowledged from the JSON batch summary, which distinguishes three
outcomes: accepted, `already_sent` (kept from re-sending), and `no_hash_found`
(the capture held no usable handshake — it stays listed as local).

Set the address once. An empty file disables the tab.

#### If only some captures arrive

- `S` sends **every** unsent capture. `U` sends **only the highlighted row** — a
  single upload is the normal outcome of pressing `U`.
- When a batch stops early or part of it is rejected, the sync screen shows
  `up<n>/<total> !<reason>` instead of a plain `OK`. The serial console prints
  one line per capture with the server's own `acc` / `skip` / `rej` counters and
  the rejection message.
- Captures answered with `no_hash_found` contain no usable PMKID/EAPOL. They
  stay listed as local and are retried on the next run, because a re-capture of
  the same BSSID overwrites the file with a better handshake.
- The upload loop compacts the heap between captures. TLS needs one large
  contiguous block, and the heap left behind by the previous session is what
  used to cut a batch short after the first file.
- Only `.cap` / `.pcap` / `.pcapng` are accepted. `.22000` files are deliberately
  **not** sent: the public endpoint answers HTTP 400 for them, and the capture
  already carries the same handshake for the server to extract. Use a capture,
  not a hash line.
- Before contacting the service, each file's container is verified locally. A
  file that is not PCAP/PCAPNG is refused as `not a capture (xxxxxxxx)`, and one
  that holds only the 24-byte global header with no packet as
  `no packets (Nub)`. OnlineHashCrack reports both as *unsupported file type*,
  so this check turns the remote error into a readable local reason and saves the
  TLS session.
- A failed or partial transport is retried once immediately, and a file that
  still cannot be sent is skipped **without** ending the batch. Only three
  consecutive heap failures stop a run, and the remaining captures stay unmarked
  so the next run picks them up.
- Every run writes a report to **`/0N3P0rK/ohc/last.log`** — open it from
  **FILEMGR** on the device or pull it over **XFER**. One line per capture:

  ```text
  # 0N3P0rK OHC build=1.3.5
  # queued=12 email=you@example.com
  # name|bytes|magic|status|acc|skip|rej|detail
  488F5A623215.pcap|1240|D4C3B2A1|ok|1|0|0|-
  80E3704C21A7.pcap|24|D4C3B2A1|bad|0|0|0|no packets (24b)
  4C1F3D0A9B77.pcap|?|?|heap|0|0|0|low heap 11/19K
  # done up=1 already=0 no=1 fail=1 reason=-
  ```

  `status` is `ok`, `already`, `nohash`, `bad`, `heap` or `fail`; `bytes` and
  `magic` prove whether the file on the card is a real capture. This file is the
  fastest way to diagnose a run without a serial console.

Sending raw `.22000` hash lines is only possible through the authenticated
OnlineHashCrack API v2 (`POST https://api.onlinehashcrack.com/v2` with an
`sk_`-prefixed key, `action: add_tasks`, `algo_mode: 22000`), which needs a
verified account. Note that v2 caps each hash string at 512 characters, so full
EAPOL pair lines do not fit — only PMKID lines do. This firmware therefore keeps
to the keyless capture endpoint.

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
`ENT` on a network to lock its view. The signal display uses seasonal colors,
a more visible noise trace, and a dotted selected-network profile; the signal
history uses horizontal density steps. Spectrum can suspend the farm scene to
reduce CPU work while it is active.

## BadUSB and BadBLE

The **CONNECT → BADUSB** tool provides authorized HID automation over either
USB HID or BLE HID. Use it only with computers and phones that you own or are
explicitly authorized to test.

### Tabs and controls

| Key | Action |
| --- | --- |
| `1` | Scripts tab |
| `2` | Live typing tab |
| `3` | Preset panel |
| `U` | Select USB HID |
| `B` | Select BLE HID |
| `P` | Toggle PC / phone preset profile |
| `C` | Connect or advertise the selected HID transport |
| `R` | Rescan `/0N3P0rK/badusb/` for scripts |
| `^` / `v` | Move selection up / down |
| `ENT` | Run a script, arm Live typing, or execute a preset |
| `FN` + `ESC` | Disarm Live typing |
| `ESC` | Exit BadUSB |

The screen shows the selected transport, profile, connection status, and a
small status indicator. USB mode waits for a mounted USB HID host. BLE mode
advertises as `0N3P0rK` and may display a pairing PIN.

### Script files

Scripts are plain UTF-8 text files stored in:

```text
/0N3P0rK/badusb/
```

Only `.txt` files are listed. The parser supports comments, text, delays,
default delays, repeated actions, modifier combinations, and common special
keys:

```text
REM Open Notepad on a Windows test machine
DEFAULT_DELAY 80
GUI R
DELAY 400
STRING notepad
ENTER
STRING Hello from 0N3P0rK
```

Supported timing commands include `DELAY`, `DEFAULT_DELAY` (also
`DEFAULTDELAY`), and `REPEAT`. Keep scripts short and test them on a
non-production device first. Leaving the BadUSB screen or pressing its exit
key stops the HID session and releases pressed keys.

## LED indicator

The Cardputer status LED is a built-in WS2812 RGB LED on GPIO 21. LED behavior
is controlled under **SET → SYSTEM**:

- **LED** enables or disables the indicator.
- **LED BRIGHT** sets brightness from 0 to 100 percent.

The LED is intentionally quiet to save battery:

| Device state | LED behavior |
| --- | --- |
| Normal farm / menu | Soft ambient color based on the selected season |
| Light capture | Off except for a short green blink when a handshake file is written |
| Aggressive or pinned capture | Off except for three green blinks when a handshake file is written |
| Loot, Wi-Fi, and other utility screens | Off |
| Disabled in `SET → SYSTEM` | Always off |

Brightness changes are applied immediately and are saved in the device
configuration. The LED is a status hint only; the display and serial log
remain the authoritative source for errors and connection details.

## XFER file transfer

**XFER** is the device's local Wi-Fi file manager. It creates a temporary
access point and serves a browser-based "0N3P0rK Commander" for managing the
SD card without removing it from the Cardputer.

### Starting XFER

1. Open **CONNECT → XFER**.
2. Connect a phone or computer to the Wi-Fi network shown on the Cardputer.
3. Open `http://192.168.4.1` in a browser.
4. Browse directories, download files, upload files, or delete files.
5. Press `ESC` on the Cardputer to stop XFER and turn off its access point.

The default network credentials are:

```text
SSID: 0N3P0rK
Password: 0N3-P0rK
Address: http://192.168.4.1
```

If the credentials were changed in the device configuration, always use the
SSID and password displayed on the XFER screen. The screen also shows the
number of connected stations and browser requests.

### File operations and safety

The web interface provides:

- directory browsing from the SD root;
- file downloads;
- multiple-file uploads into the current directory;
- file and empty-directory deletion;
- refresh and parent-directory navigation.

XFER is local to its temporary access point. It does not connect the device to
the home network or provide Internet access. The path handler rejects
`..` traversal and refuses deletion of the SD root, but the interface still
has write and delete access to SD files. Use a private test network and keep
important captures backed up before deleting or replacing them.

Starting XFER stops an active capture session and suspends the farm scene to
free radio and memory resources. Stop XFER before starting another Wi-Fi mode.

## FILES — SD File Manager

Open **FILES** from the main menu to browse and manage the microSD card. This
manager works with SD storage; it does not expose internal flash as a user
file system. Directories are navigable, the listing is sorted and scrollable,
and low memory is reported if only a partial listing can be built.

### Browse and manage

| Key | Action |
| --- | --- |
| `^` / `v` | Select the previous / next entry |
| `ENT` | Open a directory or file |
| `BKSP` / `ESC` | Go to the parent directory; exit at the SD root |
| `N` | Create a text file (opens it in the editor) |
| `M` | Create a folder |
| `R` | Rename the selected entry |
| `C` | Copy the selected file or directory |
| `V` | Mark the selected file or directory to move |
| `P` | Paste the clipboard into the current directory |
| `X` | Delete the selected entry after confirmation |
| `T` | Refresh the directory listing |

Pasting over an existing destination requires confirmation. Deleting a
directory removes its contents recursively, so check the selected path before
confirming.

### Preview and edit

- Text and configuration formats can be previewed, edited, and saved.
- JPEG, PNG, and BMP images open in the image preview.
- Other file types show file information; press `E` to try opening them as text.
- In text preview, `^` / `v` scroll and `E` enters the editor.
- In the editor, `ESC` saves changed text and returns to the list; Backspace
  deletes the character before the cursor. Cursor and line movement use
  `<`, `>`, `^`, and `v`.
- Create, rename, delete, and overwrite actions report failures and ask before
  destructive replacements.

Working buffers and directory entries are allocated while FILES is open and
released on exit. The text-edit buffer is allocated only for text preview or
editing.

### Other modes

| Mode | Role |
| --- | --- |
| **Spectrum** | Channel / air view; scene can suspend to save CPU |
| **PigPass** | Offline PSK try from captures + wordlist |
| **EvilPig** | Portal-style lab tool |
| **BLE / IR / USB SD** | Extra toys as implemented |
| **LOOT** | Browse captures and synchronize with supported services |

---

## Music (MP3 player)

**MP3** turns the Cardputer into an SD music player. Open it from the root
menu (**MP3**) and the farm is replaced by the cassette scene: track name,
position, spinning reels and a VU meter. The bottom bar becomes the transport.

### Keys

| Key | Action |
| --- | --- |
| `1` | Volume down 5% |
| `2` | Previous track |
| `3` | Play / Stop (Stop remembers the position) |
| `4` | Next track |
| `5` | Volume up 5% |
| `R` | Rescan the music folder |
| `ESC` | Exit MP3 |

When a track ends the player starts the next one and wraps around at the end
of the list. The volume is stored in the device configuration.

### Minimized over the farm

Backspace hides the scene and gives the farm back to you — the song keeps
playing (it also survives the G0 screen-off). The bottom bar then shows the
transport state (`MIN PLAY 03/12 01:23`), and `1`..`5` still steer volume and
tracks while you watch the pig. Backspace again brings the player back, `ESC` stops the music and returns to
the menu. Digits that you bound as farm
hotkeys in **SET → KEYS** keep their hotkey role instead.

### Files

```text
/0N3P0rK/music/*.mp3
```

FAT32 SD, top level of that folder only, up to 32 files, case-insensitive
`.mp3` / `.MP3` extension.

### How the sound is made

M5Unified already owns the I2S bus and the ES8311 codec on both Cardputer
boards, so the player does **not** open its own I2S. The Helix MP3 decoder
(`libhelix` / PlatformIO dependency `codec-helix`) decodes each frame to PCM,
the player downmixes to mono and streams it into `M5.Speaker.playRaw()` using
three rotating buffers. Beeps are muted while a song plays, and the capture
engine is stopped on entry so the decoder has room in the internal heap.

If memory is too tight the scene shows `LOW MEM` instead of playing — stop
other services (capture, XFER, portal) and press `3` again.

---

## InspectorPig (INSPECT)

Offline capture and handshake inspector. Reads the files in
`/0N3P0rK/handshakes/` and dissects them the way Wireshark would: container,
radiotap headers, 802.11 frame classes, EAPOL message numbers (M1..M4),
replay counter alignment, and RSN parameters (WPA/WPA2, CCMP/TKIP, PSK/SAE).

Use it before syncing to WPASec, Pwncrack, or OnlineHashCrack to verify that a
capture really holds a complete, crackable handshake instead of an empty
container or an unmatched frame pair.

### Also reachable from LOOT

The same dissection is available without opening this mode: in **LOOT**, `i`
checks the highlighted capture and `I` checks the whole folder, both headless
and with the same report output (see *Checking a capture before upload* above).
The screen below is the interactive version of it.

### Memory

The file list (~7 KB) and the report screen (~3.3 KB) are taken from the heap
when the mode opens and are released when it is left. Between visits — and for
every LOOT check — the module holds no RAM at all, and while it is open it
never grows beyond those two buffers.

### Controls

| Key | Action |
| --- | --- |
| `^` / `v` | Move selection up / down |
| `<` / `>` | Switch tabs |
| `ENT` | Inspect the selected capture (opens the detailed report) |
| `A` | Inspect **all** captures in `/0N3P0rK/handshakes/` |
| `R` | Rescan the handshakes folder |
| `ESC` | Return to list (from report) or exit to main menu |

### Reports on SD

Every inspection is saved to `/0N3P0rK/inspector/`:

- Single-file run: `/0N3P0rK/inspector/<capture_name>.txt` (full field dissection)
- All-captures run: `/0N3P0rK/inspector/report.txt` (summary table plus per-file details)

Open them on the device with **FILES** (FileMgr) or pull them over **XFER**.

### Verdicts

- **GOOD** (score 85–100): complete PMKID or full EAPOL pair with matching
  replay counters and valid RSN data.
- **USABLE** (score 60–84): valid material, but missing secondary fields
  (e.g. no beacon seen so ESSID is unknown, or slight header anomaly).
- **PARTIAL** (score 30–59): incomplete exchange (e.g. M1 without M2, or M2
  without M1). Not crackable yet.
- **BROKEN** (score 0–29): empty container, truncated packets, or non-capture
  data.

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
  wpa-sec/        key.txt, results.txt, uploaded.txt
  pwncrack/       key.txt, results.txt, uploaded.txt
  ohc/            email.txt, uploaded.txt (OnlineHashCrack)
  inspector/      handshake check reports (one .txt per capture + report.txt)
  pigpass/        crack state / results
  Passworld/      wordlists
  talk/           optional monologue lines
  music/          MP3 player tracks (SD music)
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

For the chronological release notes from the early builds through 1.3.6f, see
[README_HISTORY.md](README_HISTORY.md).

---

## Legal & credits

For **education and authorized testing** only. You are responsible for how you use radio features.

License: see `LICENSE`.  
Not affiliated with M5Stack.

**Thanks** to everyone who tested builds, to the Cardputer community, and to **Oct0sec** for handshake-path inspiration.

**0N3P0rK** — oink responsibly.
