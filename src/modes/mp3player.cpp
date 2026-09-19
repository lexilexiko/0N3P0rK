// 0N3P0rK — SD music player (MP3 -> Helix decoder -> M5.Speaker PCM stream).
//
// Audio path note: M5Unified owns the I2S + ES8311 codec on both Cardputer
// boards, so the player never opens its own I2S. Helix (libhelix / codec-helix)
// decodes MP3 to PCM and we stream mono frames into M5.Speaker.playRaw() on one
// virtual channel using three rotating buffers — the pattern M5Unified
// documents for audio generated at runtime. SFX beeps are muted while music
// plays so nothing steps on the stream.

#include "mp3player.h"
#include "../cap/sniffer.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include "../storage/littlefs_ops.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "MP3DecoderHelix.h"

namespace {

using libhelix::MP3DecoderHelix;
// MP3FrameInfo comes from the C API (libhelix-mp3/mp3dec.h) and is global.

const char* const MUSIC_DIR   = "/0N3P0rK/music";
const uint8_t  MAX_TRACKS     = 32;
// Track names are card file names, so they share the 0N3P0rK cap with the
// capture files. Do NOT call this NAME_MAX: newlib's <limits.h> (reached from
// Arduino.h -> FreeRTOS -> limits.h) already #defines NAME_MAX as 255, which
// turns the declaration into "const uint8_t 255 = 48;".
const uint8_t  TRACK_NAME_MAX = 64;
const uint16_t TRACK_PATH_MAX = 192;  // full SD path; do not truncate long filenames
const size_t   READ_CHUNK     = 4096;  // larger SD reads reduce underruns on long/VBR files
const uint8_t  PCM_SLOTS      = 3;      // rotating PCM buffers (3 = safe, per M5Unified)
const uint16_t PCM_MAX        = 1152;   // MPEG-1 layer III frame = 1152 samples/ch
const uint8_t  MUSIC_CH       = 0;      // M5.Speaker virtual channel used for music
const uint8_t  VOL_STEP       = 5;      // % per press on keys 1 / 5
const uint32_t PUMP_MAX_MS    = 60;     // max time one tick() may spend decoding
const size_t   PCM_SCRATCH    = 6144;   // Helix output scratch (4608 is the real max)
const uint32_t MIN_LARGEST    = 40000;  // refuse to start below this (lib allocator hangs)
const uint32_t MIN_FREE       = 60000;

bool    s_playing = false;
bool    s_eof     = false;
bool    s_hasResume = false;
uint8_t s_idx     = 0;
uint8_t s_n       = 0;
char*   s_names = nullptr;
char*   s_paths = nullptr;
uint8_t s_vol     = 70;
char    s_msg[28] = "";

File   s_file;
MP3DecoderHelix* s_mp3 = nullptr;
uint8_t* s_rd = nullptr;
size_t  s_rdLen = 0;
size_t  s_rdPos = 0;
int16_t* s_pcm = nullptr;
uint8_t s_slot = 0;
uint32_t s_rate = 44100;
uint32_t s_played = 0;
uint32_t s_totalSec = 0;
uint32_t s_bytes = 0;
uint32_t s_resumePos = 0;
uint16_t s_bitrate = 0;
int16_t  s_level = 0;
uint8_t  s_keyWas = 0;
bool     s_rWas = false;
bool     s_escWas = false;
uint8_t  s_feedStalled = 0;

// Lives with the SD scan further down, but togglePlay()/step() call it when the
// track list is still empty, so it needs a declaration up here.
void rescan();

void setMsg(const char* m);

// ---------------------------------------------------------- lazy MP3 memory
// Keep the music buffers completely out of RAM while the MP3 mode is not used.
// The track list is allocated when the mode opens; the audio buffers are only
// allocated when playback actually starts. Everything is released on stop.
bool allocListMemory() {
    if (s_names && s_paths) return true;
    s_names = (char*)calloc((size_t)MAX_TRACKS, TRACK_NAME_MAX);
    s_paths = (char*)calloc((size_t)MAX_TRACKS, TRACK_PATH_MAX);
    if (!s_names || !s_paths) {
        free(s_names); s_names = nullptr;
        free(s_paths); s_paths = nullptr;
        return false;
    }
    return true;
}

void freeAudioMemory() {
    free(s_rd);  s_rd = nullptr;
    free(s_pcm); s_pcm = nullptr;
    s_rdLen = 0;
    s_rdPos = 0;
}

bool allocAudioMemory() {
    if (s_rd && s_pcm) return true;
    s_rd = (uint8_t*)malloc(READ_CHUNK);
    s_pcm = (int16_t*)malloc((size_t)PCM_SLOTS * PCM_MAX * sizeof(int16_t));
    if (!s_rd || !s_pcm) {
        freeAudioMemory();
        setMsg("NO MEM");
        return false;
    }
    return true;
}

void freeListMemory() {
    free(s_names); s_names = nullptr;
    free(s_paths); s_paths = nullptr;
    s_n = 0;
}

// -------------------------------------------------------------- small helpers
void setMsg(const char* m) {
    strncpy(s_msg, m ? m : "", sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = '\0';
    if (s_msg[0]) Display::showToast(s_msg, 1200);   // surface why nothing plays
}

void applyVolume() {
    M5.Speaker.setVolume((uint8_t)((uint16_t)s_vol * 255 / 100));
}

void fmtTime(char* out, size_t n, uint32_t sec) {
    snprintf(out, n, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

void closeFile() {
    if (s_file) s_file.close();
    s_rdLen = 0;
    s_rdPos = 0;
}

// ---------------------------------------------------- decoder -> speaker bridge
// Helix calls this for every fully decoded frame. `len` is the interleaved
// sample count (frames * channels). The caller (Helix write/decode) is blocked
// until we hand the frame over, which is exactly the backpressure we want.
void onPcm(MP3FrameInfo& info, short* pcm, size_t len, void*) {
    if (!s_playing || !pcm || len == 0) return;
    uint16_t nch = info.nChans ? (uint16_t)info.nChans : 1;
    size_t frames = len / nch;
    if (frames == 0) return;
    if (frames > PCM_MAX) frames = PCM_MAX;

    // Never overwrite a buffer the speaker task may still be reading: with
    // 3 buffers and a 2-slot queue, waiting for room means the slot we are
    // about to write is already released.
    uint32_t t0 = millis();
    while (s_playing && M5.Speaker.isPlaying(MUSIC_CH) >= 2) {
        if (millis() - t0 > 1500) break;   // wedged — drop this frame
        delay(1);
        yield();
    }
    if (!s_playing) return;
    if (millis() - t0 > 1500) return;

    int16_t* out = s_pcm + ((size_t)s_slot * PCM_MAX);
    if (nch >= 2) {
        // Cardputer has one small speaker — downmix to mono.
        for (size_t i = 0; i < frames; i++) {
            int32_t mix = (int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1];
            out[i] = (int16_t)(mix / 2);
        }
    } else {
        memcpy(out, pcm, frames * sizeof(int16_t));
    }

    int32_t peak = 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t v = out[i] < 0 ? -(int32_t)out[i] : (int32_t)out[i];
        if (v > peak) peak = v;
    }
    if (peak > 32767) peak = 32767;
    if (peak > (int32_t)s_level) s_level = (int16_t)peak;

    if (info.samprate > 0) s_rate = (uint32_t)info.samprate;
    if (s_bitrate == 0 && info.bitrate > 0 && s_bytes > 0) {
        s_bitrate = (uint16_t)info.bitrate;
        // Rough total for CBR files; VBR lands close enough for a display.
        s_totalSec = (uint32_t)(((uint64_t)s_bytes * 8ULL) /
                                ((uint64_t)s_bitrate * 1000ULL));
    }

    M5.Speaker.playRaw(out, frames, s_rate, false, 1, MUSIC_CH, false);
    s_played += (uint32_t)frames;
    s_slot = (uint8_t)((s_slot + 1) % PCM_SLOTS);
}

// ------------------------------------------------------------- track open/close
// Size of the ID3v2 tag in front of the first MP3 frame: 10-byte header +
// synchsafe size (+ 10-byte footer when the flag is set). 0 = no usable tag.
//
// Big files almost always carry a tag (usually with cover art), and feeding it
// to Helix is a trap: the wrapper only drops junk when it finds a sync word
// *after* byte 4 (presync: `if (pos > 3)`), while the artwork is full of false
// 0xFF 0xEx pairs whose bogus frame length makes MP3Decode return
// ERR_MP3_INDATA_UNDERFLOW. resynch() then bails out on a nearly full frame
// buffer, so the decoder grinds through the tag instead of draining it and the
// track looks like it never opens. Little files with a 100-byte tag survive
// that; 100 KB of artwork does not.
uint32_t id3v2Size() {
    if (!s_file) return 0;

    uint8_t h[10];
    if (s_file.position() != 0) s_file.seek(0);
    if (s_file.read(h, sizeof(h)) != sizeof(h)) return 0;
    if (memcmp(h, "ID3", 3) != 0) {
        s_file.seek(0);
        return 0;
    }

    // ID3v2.3/v2.4 store the tag length as a 4-byte synchsafe integer.
    // Reject malformed headers instead of accidentally seeking into the file.
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80u) {
        s_file.seek(0);
        return 0;
    }

    uint32_t sz = ((uint32_t)(h[6] & 0x7Fu) << 21) |
                  ((uint32_t)(h[7] & 0x7Fu) << 14) |
                  ((uint32_t)(h[8] & 0x7Fu) << 7)  |
                  (uint32_t)(h[9] & 0x7Fu);

    uint32_t total = sz + 10u + ((h[5] & 0x10u) ? 10u : 0u);
    s_file.seek(0);
    return total;
}

// Check the fixed bits of an MPEG audio frame header. This deliberately does
// not try to calculate the frame length; Helix remains the authority on that.
bool plausibleMp3Header(const uint8_t* h) {
    if (!h) return false;
    if (h[0] != 0xFF || (h[1] & 0xE0u) != 0xE0u) return false;

    uint8_t version = (h[1] >> 3) & 0x03u;
    uint8_t layer   = (h[1] >> 1) & 0x03u;
    uint8_t br      = (h[2] >> 4) & 0x0Fu;
    uint8_t sr      = (h[2] >> 2) & 0x03u;

    if (version == 1 || layer != 1 || br == 15 || br == 0 || sr == 3) return false;
    return true;
}

uint32_t mp3FrameLength(const uint8_t* h) {
    if (!plausibleMp3Header(h)) return 0;

    static const uint16_t brV1[] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
    };
    static const uint16_t brV2[] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
    };
    static const uint32_t srBase[] = { 44100, 48000, 32000 };

    uint8_t version = (h[1] >> 3) & 0x03u;
    uint8_t brIdx   = (h[2] >> 4) & 0x0Fu;
    uint8_t srIdx   = (h[2] >> 2) & 0x03u;
    uint8_t pad     = (h[2] >> 1) & 0x01u;

    uint32_t br = (version == 3) ? brV1[brIdx] : brV2[brIdx];
    uint32_t sr = srBase[srIdx];
    if (version == 2) sr /= 2;
    else if (version == 0) sr /= 4;
    if (!br || !sr) return 0;

    // Layer III: MPEG-1 uses 144*kbps/samplerate; MPEG-2/2.5 uses 72.
    uint32_t len = (version == 3)
        ? (144000u * br) / sr
        : (72000u * br) / sr;
    return len + pad;
}

// Skip ID3 and any small amount of junk before the first real MP3 frame.
// Candidate headers are confirmed by the following frame, so random FFEx
// bytes in metadata/artwork are not mistaken for the stream start.
uint32_t firstMp3Frame(uint32_t start) {
    if (!s_file || start >= s_bytes) return start;

    const uint32_t MAX_SCAN = 64u * 1024u;
    const uint32_t end = (start + MAX_SCAN < s_bytes) ? start + MAX_SCAN : s_bytes;

    uint8_t buf[512];
    uint8_t h2[4];
    uint32_t pos = start;

    while (pos < end) {
        s_file.seek(pos);
        size_t got = s_file.read(buf, sizeof(buf));
        if (got < 4) break;

        for (size_t i = 0; i + 4 <= got; ++i) {
            const uint8_t* h = buf + i;
            if (!plausibleMp3Header(h)) continue;

            uint32_t candidate = pos + (uint32_t)i;
            uint32_t flen = mp3FrameLength(h);
            if (flen < 24 || candidate + flen + 4 > s_bytes) continue;

            s_file.seek(candidate + flen);
            if (s_file.read(h2, 4) != 4 || !plausibleMp3Header(h2)) continue;

            // Confirm one more consecutive frame when possible.
            uint32_t flen2 = mp3FrameLength(h2);
            if (flen2 < 24 || candidate + flen + flen2 + 4 > s_bytes) continue;

            s_file.seek(candidate + flen + flen2);
            uint8_t h3[4];
            if (s_file.read(h3, 4) != 4 || !plausibleMp3Header(h3)) continue;

            s_file.seek(candidate);
            return candidate;
        }

        if (got <= 4) break;
        pos += (uint32_t)(got - 3);
    }

    s_file.seek(start);
    return start;
}

bool openTrack(uint8_t idx, uint32_t pos) {
    // Open the exact path captured during rescan.  Do NOT rebuild it from
    // the display name: Storage::FILE_NAME_MAX is 48 bytes and long UTF-8
    // filenames (for example Cyrillic names) can exceed that limit.  The old
    // code truncated the name, so SD.open() returned false even for a healthy
    // 3-6 MB MP3.
    const char* path = s_paths + ((size_t)idx * TRACK_PATH_MAX);
    if (!path[0]) {
        setMsg("OPEN FAIL");
        return false;
    }
    s_file = SD.open(path, "r");
    if (!s_file) {
        setMsg("OPEN FAIL");
        return false;
    }

    uint64_t fileSize = s_file.size();
    if (fileSize == 0 || fileSize > 0xFFFFFFFFULL) {
        setMsg("BAD FILE");
        s_file.close();
        return false;
    }
    s_bytes = (uint32_t)fileSize;

    // Resume positions are already frame-aligned. A fresh track skips ID3v2.
    uint32_t start = pos;
    if (start == 0) {
        uint32_t tag = id3v2Size();
        if (tag >= s_bytes) {
            setMsg("BAD ID3");
            s_file.close();
            return false;
        }
        start = tag;

        // Don't feed arbitrary post-tag junk/artwork to Helix.
        start = firstMp3Frame(start);
    }

    if (start >= s_bytes) {
        setMsg("NO MP3 DATA");
        s_file.close();
        return false;
    }

    s_file.seek(start);
    return true;
}

void releaseDecoder() {
    if (!s_mp3) return;
    s_mp3->end();
    delete s_mp3;
    s_mp3 = nullptr;
}

bool ensureDecoder() {
    if (s_mp3) {
        if (s_mp3->begin()) return true;   // resets buffers for the new track
        releaseDecoder();
        setMsg("DEC FAIL");
        return false;
    }
    // Heap check BEFORE the library allocates: codec-helix spins forever
    // (while(true)) when a block cannot be served, so never let it fail.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_LARGEST ||
        ESP.getFreeHeap() < MIN_FREE) {
        Storage::brewHeap();
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_LARGEST ||
            ESP.getFreeHeap() < MIN_FREE) {
            setMsg("LOW MEM");
            return false;
        }
    }
    s_mp3 = new MP3DecoderHelix(onPcm);
    if (!s_mp3) {
        setMsg("NO MEM");
        return false;
    }
    s_mp3->setMaxPCMSize(PCM_SCRATCH);
    if (!s_mp3->begin()) {
        releaseDecoder();
        setMsg("DEC FAIL");
        return false;
    }
    return true;
}

void stopAudio() {
    s_playing = false;
    M5.Speaker.stop();
    SFX::setMp3Muted(false);
    SFX::refreshVolume();
}

// Byte offset of the next un-decoded MP3 frame (used by STOP -> PLAY resume).
uint32_t resumePos() {
    uint32_t pos = s_file ? (uint32_t)s_file.position() : 0;
    size_t pending = s_rdLen - s_rdPos;
    return (pos > pending) ? (uint32_t)(pos - pending) : 0;
}

void playTrack(uint8_t idx, uint32_t pos) {
    if (s_n == 0) return;
    if (idx >= s_n) idx = 0;
    stopAudio();
    if (!Storage::available()) {
        setMsg("NO SD");
        return;
    }
    s_idx = idx;
    closeFile();
    if (!allocAudioMemory()) return;
    if (!openTrack(s_idx, pos)) { freeAudioMemory(); return; }
    if (!ensureDecoder()) {
        closeFile();
        freeAudioMemory();
        return;
    }
    s_eof = false;
    s_slot = 0;
    s_rdLen = 0;
    s_rdPos = 0;
    s_feedStalled = 0;
    if (pos == 0) {
        s_played = 0;
        s_bitrate = 0;
        s_totalSec = 0;
    }
    s_level = 0;
    s_playing = true;
    applyVolume();
    SFX::setMp3Muted(true);   // no beeps over the music
    setMsg("");
}

// STOP keeps the byte offset so PLAY picks the track up where it was cut.
void userStop() {
    if (!s_playing) return;
    s_resumePos = resumePos();
    s_hasResume = s_resumePos > 0;
    stopAudio();
    s_eof = false;
    closeFile();
    releaseDecoder();
    freeAudioMemory();
    setMsg("STOP");
}

void togglePlay() {
    if (s_n == 0) {
        rescan();
        return;
    }
    if (s_playing) {
        userStop();
        return;
    }
    uint32_t pos = s_hasResume ? s_resumePos : 0;
    s_hasResume = false;
    playTrack(s_idx, pos);
}

void step(int8_t dir) {
    if (s_n == 0) {
        rescan();
        return;
    }
    int16_t n = (int16_t)s_n;
    int16_t i = (int16_t)s_idx + dir;
    while (i < 0) i = (int16_t)(i + n);
    while (i >= n) i = (int16_t)(i - n);
    s_hasResume = false;
    playTrack((uint8_t)i, 0);
}

void bumpVolume(int8_t dir) {
    int16_t v = (int16_t)s_vol + (int16_t)dir * (int16_t)VOL_STEP;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_vol = (uint8_t)v;
    applyVolume();
    char m[18];
    snprintf(m, sizeof(m), "VOL %u%%", (unsigned)s_vol);
    Display::showToast(m, 700);
}

// 1 vol-  2 prev  3 play/stop  4 next  5 vol+
void keyAction(uint8_t i) {
    SFX::play(SFX::MENU_CLICK);   // silent while music plays (MUTE_MP3)
    switch (i) {
        case 0: bumpVolume(-1); break;
        case 1: step(-1); break;
        case 2: togglePlay(); break;
        case 3: step(+1); break;
        case 4: bumpVolume(+1); break;
        default: break;
    }
}

void rescan() {
    s_n = 0;
    if (!allocListMemory()) { setMsg("NO MEM"); return; }
    memset(s_names, 0, (size_t)MAX_TRACKS * TRACK_NAME_MAX);
    memset(s_paths, 0, (size_t)MAX_TRACKS * TRACK_PATH_MAX);
    s_msg[0] = '\0';
    if (!Storage::available()) {
        setMsg("NO SD");
        return;
    }
    SD.mkdir(MUSIC_DIR);
    File dir = SD.open(MUSIC_DIR);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        setMsg("NO DIRECTORY");
        return;
    }
    File f = dir.openNextFile();
    while (f && s_n < MAX_TRACKS) {
        if (!f.isDirectory()) {
            const char* nm = Storage::baseName(f.name());
            size_t l = nm ? strlen(nm) : 0;
            if (l > 4 && strcasecmp(nm + l - 4, ".mp3") == 0) {
                // Keep the complete filesystem path separately from the
                // short UI label.  Opening from the truncated label was the
                // reason long filenames reported OPEN FAIL.
                const char* full = f.name();
                if (full && full[0]) {
                    strncpy((s_paths + ((size_t)s_n * TRACK_PATH_MAX)), full, TRACK_PATH_MAX - 1);
                    (s_paths + ((size_t)s_n * TRACK_PATH_MAX))[TRACK_PATH_MAX - 1] = '\0';

                    // Some FS implementations return only the basename from
                    // File::name(); make the stored path absolute in that case.
                    if ((s_paths + ((size_t)s_n * TRACK_PATH_MAX))[0] != '/') {
                        char tmp[TRACK_PATH_MAX];
                        snprintf(tmp, sizeof(tmp), "%s/%s", MUSIC_DIR, (s_paths + ((size_t)s_n * TRACK_PATH_MAX)));
                        strncpy((s_paths + ((size_t)s_n * TRACK_PATH_MAX)), tmp, TRACK_PATH_MAX - 1);
                        (s_paths + ((size_t)s_n * TRACK_PATH_MAX))[TRACK_PATH_MAX - 1] = '\0';
                    }

                    const char* shown = Storage::baseName((s_paths + ((size_t)s_n * TRACK_PATH_MAX)));
                    strncpy((s_names + ((size_t)s_n * TRACK_NAME_MAX)), shown ? shown : nm, TRACK_NAME_MAX - 1);
                    (s_names + ((size_t)s_n * TRACK_NAME_MAX))[TRACK_NAME_MAX - 1] = '\0';
                    s_n++;
                }
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    if (f) f.close();
    dir.close();
    s_idx = 0;
    if (s_n == 0) setMsg("NO MP3");
}

// ------------------------------------------------------------- decode + feed
// One MP3 chunk (4 KB) in, decoded frames out (via onPcm). Returns false at
// end of file so the pump can stop hammering the SD card.
bool feed() {
    if (!s_file || !s_mp3) return false;

    // Keep unconsumed bytes. A decoder may consume only part of a read buffer;
    // never throw the remainder away. This is especially important around VBR
    // frame boundaries and for long files where one dropped frame can make the
    // stream appear to stop.
    if (s_rdPos >= s_rdLen) {
        size_t got = s_file.read(s_rd, READ_CHUNK);
        if (got == 0) {
            if (!s_eof) {
                s_mp3->flush();   // drain the final complete frame(s)
                s_eof = true;
            }
            return false;
        }
        s_rdLen = got;
        s_rdPos = 0;
    }

    size_t avail = s_rdLen - s_rdPos;
    size_t used = s_mp3->write(s_rd + s_rdPos, avail);

    if (used > avail) used = avail;

    if (used != 0) {
        s_feedStalled = 0;
        s_rdPos += used;
        return true;
    }

    // Do NOT discard data when Helix returns 0. Give it a little more input
    // while retaining the bytes already supplied. If the wrapper has consumed
    // nothing after several attempts, report a decoder error instead of
    // silently corrupting the stream.
    if (++s_feedStalled < 4) {
        delay(1);
        return true;
    }
    s_feedStalled = 0;

    // Last-resort resync: scan the unconsumed bytes for a plausible MPEG
    // header and continue there. This recovers from a stray corrupt byte
    // without throwing away the entire 4 KB SD block.
    for (size_t i = 1; i + 4 <= avail; ++i) {
        if (plausibleMp3Header(s_rd + s_rdPos + i)) {
            s_rdPos += i;
            return true;
        }
    }

    // No sync in this block. Drop only this block and continue reading.
    s_rdPos = s_rdLen;
    return true;
}

void pump() {
    if (!s_playing) return;
    uint32_t t0 = millis();
    while (s_playing && !s_eof && M5.Speaker.isPlaying(MUSIC_CH) < 2) {
        if (!feed()) break;
        if (millis() - t0 >= PUMP_MAX_MS) break;   // keep the UI responsive
    }
    if (!s_playing) return;
    if (s_eof && M5.Speaker.isPlaying(MUSIC_CH) == 0) {
        s_hasResume = false;
        if (s_n > 1) {
            playTrack((uint8_t)((s_idx + 1) % s_n), 0);   // play the list through
        } else {
            stopAudio();
            setMsg("END");
        }
    }
}

// -------------------------------------------------------------- transport keys
// Digits the user bound as farm/menu hotkeys stay with the hotkey handler —
// the player must not steal them while it is minimized over the farm.
uint8_t digitHotkeyMask() {
    uint8_t mask = 0;
    const HotkeyConfig& hk = Config::hotkeys();
    for (uint8_t i = 0; i < 5; i++) {
        char c = (char)('1' + i);
        for (uint8_t k = 0; k < HOTKEY_COUNT; k++) {
            if (hk.key[k] == c) {
                mask |= (uint8_t)(1u << i);
                break;
            }
        }
    }
    return mask;
}

// Edge-detected 1..5 handling; skipMask bits are left for someone else.
bool pollTransportKeys(uint8_t skipMask) {
    bool consumed = false;
    for (uint8_t i = 0; i < 5; i++) {
        char c = (char)('1' + i);
        bool down = M5Cardputer.Keyboard.isKeyPressed(c);
        uint8_t bit = (uint8_t)(1u << i);
        if (down && !(s_keyWas & bit)) {
            s_keyWas |= bit;
            if (!(skipMask & bit)) {
                keyAction(i);
                consumed = true;
            }
        } else if (!down) {
            s_keyWas &= (uint8_t)~bit;
        }
    }
    return consumed;
}

}  // namespace

bool Mp3PlayerMode::running = false;

void Mp3PlayerMode::start() {
    running = true;
    s_playing = false;
    s_eof = false;
    s_hasResume = false;
    s_resumePos = 0;
    s_level = 0;
    s_keyWas = 0;
    s_rWas = M5Cardputer.Keyboard.isKeyPressed('r') ||
             M5Cardputer.Keyboard.isKeyPressed('R');
    s_escWas = keyEsc();   // `` ` `` still held from the menu must not exit on entry
    s_vol = Config::personality().mp3Volume;
    if (s_vol > 100) s_vol = 100;
    if (Cap::isRunning()) Cap::stop();   // hand the capture heap to the decoder
    applyVolume();
    rescan();
}

void Mp3PlayerMode::stop() {
    if (!running) return;
    running = false;
    s_escWas = false;
    stopAudio();
    closeFile();
    releaseDecoder();
    freeAudioMemory();
    freeListMemory();
    if (Config::personality().mp3Volume != s_vol) {
        Config::personality().mp3Volume = s_vol;
        Config::save();
    }
    s_msg[0] = '\0';
}

void Mp3PlayerMode::tick() {
    if (!running) return;
    pump();
}

void Mp3PlayerMode::update() {
    if (!running) return;
    if (App::windowHidden()) return;

    // Overlay modes own their Esc. App::loop()'s generic `` ` `` handler is
    // unreachable here: its keyNewPress(s_minLatch) call already consumes
    // Keyboard.isChange() (isChange() latches _last_key_size), so the later
    // isChange() check bails out before the Esc branch. FileMgr / XFER / USB SD
    // exit on their own keyEsc() — the player has to do the same.
    if (keyEsc()) {
        if (!s_escWas) {
            s_escWas = true;
            App::setMode(AppMode::MENU);   // stops the audio via Mp3PlayerMode::stop()
        }
        return;
    }
    s_escWas = false;

    pollTransportKeys(0);

    bool r = M5Cardputer.Keyboard.isKeyPressed('r') ||
             M5Cardputer.Keyboard.isKeyPressed('R');
    if (r && !s_rWas) {
        rescan();
        Display::showToast(s_n ? "RESCAN" : (s_msg[0] ? s_msg : "NO MP3"), 900);
    }
    s_rWas = r;
}

bool Mp3PlayerMode::minimizedKey() {
    if (!running) return false;
    return pollTransportKeys(digitHotkeyMask());
}

void Mp3PlayerMode::getStatusLine(char* buf, size_t n) {
    if (!buf || !n) return;
    buf[0] = '\0';
    if (s_n == 0) {
        snprintf(buf, n, "MP3 %s", s_msg[0] ? s_msg : "NO MP3");
        return;
    }
    uint32_t el = s_rate
        ? (uint32_t)((uint64_t)s_played * 1000ULL / s_rate / 1000ULL)
        : 0;
    snprintf(buf, n, "%s %02u/%02u %02u:%02u", s_playing ? "PLAY" : "STOP",
             (unsigned)(s_idx + 1), (unsigned)s_n,
             (unsigned)(el / 60), (unsigned)(el % 60));
}

void Mp3PlayerMode::draw(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    // ---- track name ticker ----
    if (s_n) {
        uiDrawMarquee(canvas, (s_names + ((size_t)s_idx * TRACK_NAME_MAX)), 6, 3, 226, 6);
    } else {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString(s_msg[0] ? s_msg : "NO MUSIC", 6, 3);
    }

    // ---- state + position (left) / time (right) ----
    if (s_n) {
        char st[30];
        const char* what = s_playing ? "PLAY" : (s_eof ? "END" : "STOP");
        snprintf(st, sizeof(st), "%s %02u/%02u", what, (unsigned)(s_idx + 1),
                 (unsigned)s_n);
        uint32_t el = s_rate
            ? (uint32_t)((uint64_t)s_played * 1000ULL / s_rate / 1000ULL)
            : 0;
        char t1[10];
        char tb[24];
        fmtTime(t1, sizeof(t1), el);
        if (s_totalSec) {
            char t2[10];
            fmtTime(t2, sizeof(t2), s_totalSec);
            snprintf(tb, sizeof(tb), "%s/%s", t1, t2);
        } else {
            snprintf(tb, sizeof(tb), "%s", t1);
        }
        canvas.setTextColor(s_playing ? UiStyle::GREEN : UiStyle::DIM);
        canvas.drawString(st, 6, 15);
        if (s_playing && ((millis() / 350) % 2) == 0)
            canvas.fillCircle(84, 18, 2, UiStyle::GREEN);
        canvas.setTextColor(UiStyle::TEXT);
        canvas.setTextDatum(top_right);
        canvas.drawString(tb, 234, 15);
        canvas.setTextDatum(top_left);
    } else {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("R RESCAN   SD /0N3P0rK/music", 6, 15);
        canvas.setTextColor(UiStyle::DIM);
        canvas.setTextDatum(top_right);
        canvas.drawString("` EXIT", 234, 15);
        canvas.setTextDatum(top_left);
    }

    // ---- cassette (reels spin while playing) ----
    canvas.fillRoundRect(14, 28, 212, 46, 4, UiStyle::PANEL);
    canvas.drawRoundRect(14, 28, 212, 46, 4, UiStyle::CYAN);
    canvas.fillTriangle(26, 28, 38, 17, 50, 28, UiStyle::PINK);
    canvas.fillTriangle(190, 28, 202, 17, 214, 28, UiStyle::PINK);

    const int cy = 51;
    canvas.drawLine(90, cy, 150, cy, UiStyle::DIRT);
    float ang = s_playing ? (float)((millis() % 60000UL) * 0.006f) : 0.0f;
    auto reel = [&](int x) {
        canvas.fillCircle(x, cy, 14, UiStyle::DIRT);    // ring: disc + window
        canvas.fillCircle(x, cy, 11, UiStyle::PANEL);
        canvas.fillCircle(x, cy, 4, UiStyle::CYAN);     // hub
        for (int i = 0; i < 6; i++) {
            float t = ang + (float)i * 1.0472f;   // 60 degrees
            canvas.drawLine(x, cy,
                            x + (int)(cosf(t) * 13.0f),
                            cy + (int)(sinf(t) * 13.0f), UiStyle::CYAN);
        }
    };
    reel(72);
    reel(168);

    // ---- VU ----
    const int vx = 16, vy = 80, vw = 168, vh = 10;
    canvas.drawRect(vx, vy, vw, vh, UiStyle::DIM);
    int fill = (int)((int32_t)s_level * (vw - 2) / 32767);
    if (fill > vw - 2) fill = vw - 2;
    if (fill > 0) {
        uint16_t col = UiStyle::GREEN;
        if (fill > (vw - 2) * 85 / 100) col = UiStyle::RED;
        else if (fill > (vw - 2) * 65 / 100) col = UiStyle::GOLD;
        canvas.fillRect(vx + 1, vy + 1, fill, vh - 2, col);
    }
    char vb[10];
    snprintf(vb, sizeof(vb), "VOL%3u", (unsigned)s_vol);
    canvas.setTextColor(UiStyle::TEXT);
    canvas.drawString(vb, vx + vw + 6, vy + 2);

    // ---- footer: sample rate / bitrate / heap ----
    char info[40];
    canvas.setTextColor(UiStyle::DIM);
    snprintf(info, sizeof(info), "%u.%01uKHZ %uKBPS",
             (unsigned)(s_rate / 1000), (unsigned)((s_rate % 1000) / 100),
             (unsigned)s_bitrate);
    canvas.drawString(info, 6, 93);
    // The transport bar is full (1..5 + volume), so the exit key lives here.
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("` EXIT", 104, 93);
    canvas.setTextColor(UiStyle::DIM);
    snprintf(info, sizeof(info), "HEAP%3uK", (unsigned)(ESP.getFreeHeap() / 1024));
    canvas.setTextDatum(top_right);
    canvas.drawString(info, 234, 93);
    canvas.setTextDatum(top_left);

    // meter falls back when the music goes quiet
    s_level = (int16_t)((int32_t)s_level * 7 / 8);
}

// 5 transport cells, drawn into the 240x14 bottom bar.
void Mp3PlayerMode::drawBar(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(0xEF5D);
    canvas.drawString("1-", 6, 3);
    canvas.drawString("2<<", 30, 3);
    canvas.setTextColor(s_playing ? UiStyle::GREEN : UiStyle::GOLD);
    canvas.drawString(s_playing ? "3STOP" : "3PLAY", 72, 3);
    canvas.setTextColor(0xEF5D);
    canvas.drawString("4>>", 138, 3);
    canvas.drawString("5+", 176, 3);
    char vb[10];
    snprintf(vb, sizeof(vb), "%3u%%", (unsigned)s_vol);
    canvas.setTextColor(UiStyle::CYAN);
    canvas.setTextDatum(top_right);
    canvas.drawString(vb, 238, 3);
    canvas.setTextDatum(top_left);
}