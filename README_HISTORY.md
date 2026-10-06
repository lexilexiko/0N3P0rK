# 0N3P0rK - Version history

[Back to the project guide](README.md)

Chronological release notes. Historical version labels are retained as recorded.

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

### 1.3.0

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
- Documented the BadUSB / BadBLE Scripts, Live, and Panel workflows, including
  USB/BLE transport selection and SD script storage.
- Documented the configurable WS2812 status LED, seasonal ambient indication,
  and green handshake-capture flashes.
- Documented the local XFER access point and browser-based SD file manager,
  including its default address, credentials, upload/download actions, and
  safe shutdown behavior.

#### Compatibility and build

- Updated the firmware version to `1.3.0`.
- Verified the PlatformIO build for the M5Stack StampS3 target.
- Kept the same firmware target for the original M5Cardputer and Cardputer
  ADV hardware.


### 1.3.2
---Soon__

### 1.3.4

- Added the **MP3 player**: SD music scene, cassette + VU meter, five-key
  transport in the bottom bar (`1-` `2<<` `3 PLAY/STOP` `4>>` `5+`), position
  and resume, auto-advance through the playlist.
- New root menu entry **MP3** and the SD folder `/0N3P0rK/music/`.
- Uses the Helix MP3 decoder (`codec-helix`) and streams PCM into the existing
  M5Unified speaker path — no second I2S driver and no codec re-initialization.
- SFX beeps are muted while music plays; playback volume is stored in the
  device configuration (`mp3vol`).
- Backspace minimizes the player over the live farm scene: the song keeps
  playing, the bottom bar shows `MIN PLAY 03/12 01:23`, and `1`..`5` keep
  working from the farm.
- Starting the player stops an active capture session to free heap for the
  decoder.

### 1.3.5 (beta)

#### OnlineHashCrack

- New **OHC** tab in **LOOT**, next to WPASec and Pwncrack. It uploads the same
  `.pcap` captures to the public OnlineHashCrack WPA API
  (`POST https://api.onlinehashcrack.com`).
- No API key: the credential is the email of an existing OnlineHashCrack
  account, read from `/0N3P0rK/ohc/email.txt`. Requests are sent as
  `email` + `file`, and the JSON batch summary is parsed for
  `accepted` / `skipped` / `rejected`.
- Tabs are switched with `,` and `/` and now cycle through three services.
  `S`, `U`, `D`, `R`, `T` and paging behave as before.
- `no_hash_found` captures stay listed as local; `already_sent` ones are marked
  and never re-uploaded, so repeated runs stay cheap.
- WPA captures (hashcat mode 22000) are exempt from the account's monthly task
  quota.
- Added `Net::setOhcEmail()`, the `ohcmail` NVS key, and the `/0N3P0rK/ohc/`
  directory with `email.txt` and `uploaded.txt`.

#### Memory recovery

- `WPASec::freeCacheMemory()` and `Pwncrack::freeCacheMemory()` now release the
  cache capacity instead of only clearing it, without `shrink_to_fit` (a failed
  realloc aborts the firmware).
- **LOOT** releases both caches, the WPASec/Pwncrack uploaded lists, the new OHC
  cache, and compacts the heap when the view is closed.
- **BLE** drains the advertiser, waits for Bluedroid to release its buffers, and
  compacts the heap on exit.
- **Capture** compacts the heap after the capture buffers are deleted, and the
  release log now reports the largest free block instead of only the total.

---

### 1.3.5f

#### Handshake check in LOOT

- New in **LOOT**: `i` vets the highlighted capture and `I` checks every capture
  in `/0N3P0rK/handshakes/`. Both write a report to `/0N3P0rK/inspector/` and
  answer with a verdict toast (`GOOD 92/100 SAVED`, `12 FILES  OK 9`). Both work
  from the list and from the open card.
- The check is **headless**: no mode switch, no file list, no report screen. The
  INSPECT view keeps working exactly as before, it is just no longer required to
  get an answer.
- Meant to be pressed before `U` / `S`, so an upload only carries a capture that
  really holds a handshake.

#### Memory

- **InspectorPig** no longer keeps its file list (`MAX_ENTRIES * sizeof(Entry)`,
  ≈ 7 KB) and report buffer (`MAX_LINES * LINE_LEN`, ≈ 3.3 KB) in `.bss`: both
  move to the heap on entry and are returned in `stop()`, so the module costs
  nothing between visits. A failed allocation exits with a `LOW MEM` notice
  instead of a half-built view.
- **OHC** takes its 2 KB reply-scrape buffer from the heap per upload instead of
  parking it in `.bss` as a function-local static; it is released on every exit
  path.
- `InspectorPig::checkAll()` walks the folder in two passes — names first, then
  analysis — over a short-lived heap list, so no capture is opened while the
  directory handle is still held.

### 1.3.6 (previous)

#### File Manager

- Reworked the SD File Manager with directory navigation, file details,
  text preview/editing, and JPEG/BMP/PNG image preview.
- Added create-file, create-folder, rename, copy, move, paste, and confirmed
  delete operations. Backspace/ESC now navigate back; Backspace removes a
  character while editing or entering a name.
- File Manager working memory is allocated only while the mode is open.
  The text buffer is allocated only for text preview/editing and released
  when returning to the browser; browsing images and folders does not reserve
  that 6 KB editor buffer.
- Directory entries grow in heap-backed batches instead of using a fixed
  small file limit. The list is sorted and scrollable; if memory runs out,
  the UI reports that the displayed list is partial.

#### Task Manager and memory

- Reworked **TASKS** to show active services and current heap statistics:
  free heap, largest free block, minimum free heap, internal free memory, and
  available SD space.
- Added scrollable service controls and status details for active tools.
  WPA-Sec/Pwncrack synchronization is left to its normal shutdown path.
- Reduced File Manager's static RAM footprint by moving its working buffers
  and directory entries out of `.bss` and onto the heap for the duration of use.
  The PlatformIO static RAM report is about 11 KB lower than the preceding
  build; the main display canvas remains permanently allocated.

#### Interface polish

- Consolidated keyboard guidance in the bottom hint bar and improved the
  displayed navigation symbols and Escape/Backspace labels.
- Kept Loot's existing sync presentation while retaining the updated key
  labels.

### 1.3.6f (FixFIxFIx)

#### Farm and grass

- Stabilized grass blade shape and layer assignment while the field scrolls,
  removing visual flicker without changing wind or grass physics.
- Made back- and foreground grass draw independently; the back layer remains
  full even when the foreground layer is toggled off.

#### Spectrum

- Refined the spectrum view with a clearer signal trace, more visible noise,
  and a selected-network profile using an animated dotted fill.
- Added season-based signal and selection colors.
- Reworked the signal-history waterfall to use clean horizontal density steps
  and subdued seasonal colors instead of a white, diagonal-looking pattern.

#### Living bottom bar

- Replaced stationary seasonal specks with a scrolling underground ant-farm
  layer: branching tunnels dug by independently wandering ants, which gradually
  crumble before the ants open new paths.
- Thick, forked roots match each tree's bark palette and follow its growth,
  collapse, and world-scroll animations; small roots also grow under grass.
- City scenes keep the ant tunnels and grass roots, but omit roots under city props.
- The **PIG → UNDERGROUND** toggle controls the layer.
- Kept the seasonal soil palette and status text readable above the animation.

#### Card duel

- Redesigned the duel hand and played-card layout: cards stay in aligned,
  separate player/rival lanes, and slide outward during actions so the effects
  remain visible.
- Replaced the tiny card glyphs with animated fire, shield-aura, and healing-drop
  effects; card power values remain prominent on both basic and combo cards.
- Added mirrored combat summaries for both sides, showing attack, defense, healing,
  damage, and healing totals. The previous turn's totals remain visible while
  choosing the next hand.
- Extended the action timing and added stronger, distinct effects for double
  attacks, defenses, and heals, with sound cues synchronized to each exchange.
- Compact HP bars now show current/max health beside the centered match score and
  round number.

#### Radio capture fixes

- Added **MIN DWELL** to the RADIO menu with `OFF` and `50–600 ms` options.
  When enabled, it is a lower bound on the channel-hop interval alongside
  **HOP MS**.
- Kept **HS DEPTH** under the user's control. Pack presets remain at `M1–M2`;
  collecting `+M3` or the full `M1–M4` exchange requires selecting that depth
  manually in RADIO.
- Corrected the FOCUS frame counter so broadcast management frames sent through
  the sniffer callback are not counted twice.
- Clarified **SOFT** as passive capture: it disables kick, EAPOL TX, PMKID
  probing, CSA and auth-flood actions while capture and file saving continue.
- Updated the Russian contributor guides for [capture methods](src/cap/methods/README.md)
  and [radio packs](src/cap/packs/README.md), including their registration
  examples, RADIO settings and behavior notes.

### 1.3.7 (Beta)

#### Radio capture and PCAP safety

- Added a **PENDING** RADIO setting for `4–16` simultaneous unfinished
  BSSID/client handshake captures, in steps of `2`.
- Added **AUTO SKIP** on/off control. When enabled, a network is added to the
  persistent skip list after a successful capture at the selected handshake
  depth and confirmation in `.22000`.
- Added the in-capture `Q` skip-list menu for viewing saved and session
  networks, toggling persistent entries, and returning to capture. The saved
  list supports up to 64 networks; the session-only list holds up to 16.
- Prevented new captures from appending to an existing PCAP for the same
  BSSID when that file has a complete global header. Existing files are kept
  unchanged; this guard does not determine whether every packet in an old file
  is valid or repair files already damaged.
- Unified EAPOL message classification and replay-counter extraction around
  the validated EAPOL-Key offset, and reject mismatched M1/M2 replay counters
  before writing the pair.
- Fixed the M2-before-M1 case so a tentative M2 with a different replay
  counter is discarded when the first M1 arrives.
- Protected beacon-table access with short critical sections and snapshots
  passed to capture methods; corrected EAPOL queue-drop accounting.
- Updated the RADIO guide and capture controls, including the one-line Q-menu
  navigation hint.

---
