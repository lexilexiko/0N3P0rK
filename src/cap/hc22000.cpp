// cap/hc22000.cpp
#include "hc22000.h"
#include "capture_name.h"
#include "../storage/littlefs_ops.h"
#include "../core/config.h"
#include <SD.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

namespace Hc22000 {

static const uint8_t MAX_HS = 12;
static const uint16_t MAX_EAPOL = 192;

struct Hs {
    uint8_t bssid[6];
    uint8_t sta[6];
    uint8_t essid[32];
    uint8_t essidLen;
    uint8_t anonce[32];     // from M1
    uint8_t anonce3[32];    // from M3 (M2+M3 fallback)
    uint8_t anonceReplay[8];  // M1's EAPOL key replay counter
    uint8_t m2Replay[8];      // M2's own EAPOL key replay counter (must equal M1's)
    uint8_t m3Replay[8];      // M3 replay; Pair 02 requires M2.replay + 1
    uint8_t pmkid[16];
    uint8_t m2[MAX_EAPOL];
    uint16_t m2Len;
    bool used;
    bool haveEssid;
    bool haveAnonce;
    bool haveAnonce3;
    bool havePmkid;
    bool haveM2;
    bool haveM4;      // M4 carries no nonce we need, just note it arrived
    bool wrotePmkid;
    bool wroteEapol;
    // Set true by feed() (which can run from the WiFi promiscuous IRQ) when
    // an in-memory slot field changed. Cleared by flushPending() in loop()
    // after maybeWrite() has had a chance to actually open the SD file.
    // Without this, maybeWrite() would SD.open()/write()/close() straight
    // from the ISR on every beacon/EAPOL - guaranteed WDT/panic under load.
    bool dirty;
};

// Checklist §1: AP metadata (ESSID) is owned by BSSID, not by a handshake
// slot. Per-STA Hs rows inherit ESSID; they never "claim" a BSSID-only row.
struct ApMeta {
    uint8_t bssid[6];
    uint8_t essid[32];
    uint8_t essidLen;
    bool used;
    bool haveEssid;
};

static Hs s_hs[MAX_HS];
static ApMeta s_ap[MAX_HS];
static const uint8_t ZERO_MAC[6] = {};

static void hexEnc(const uint8_t* in, size_t n, char* out) {
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static uint32_t s_lastM1Ms = 0;

static void essidOf(const Hs* h, char ssid[33]) {
    ssid[0] = '\0';
    if (!h || !h->haveEssid || h->essidLen == 0) return;
    size_t n = h->essidLen < 32 ? h->essidLen : 32;
    memcpy(ssid, h->essid, n);
    ssid[n] = '\0';
}

static void makePath(const Hs* h, const char* suffix, char* path, size_t pathLen) {
    char ssid[33];
    essidOf(h, ssid);
    char stem[40];
    CapName::buildStem(ssid, h->bssid, stem, sizeof(stem));
    snprintf(path, pathLen, "%s/%s%s", Storage::DIR_HS, stem, suffix);
}

static bool macEq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static bool macZero(const uint8_t* m) {
    return !m || memcmp(m, ZERO_MAC, 6) == 0;
}

static void setHsEssid(Hs* h, const uint8_t* essid, uint8_t len) {
    if (!h || !essid || len == 0) return;
    if (len > 32) len = 32;
    memcpy(h->essid, essid, len);
    h->essidLen = len;
    h->haveEssid = true;
}

static ApMeta* findAp(const uint8_t* bssid) {
    if (!bssid) return nullptr;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_ap[i].used && macEq(s_ap[i].bssid, bssid))
            return &s_ap[i];
    }
    return nullptr;
}

static uint8_t countHsForBssid(const uint8_t* bssid) {
    uint8_t n = 0;
    if (!bssid) return 0;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && macEq(s_hs[i].bssid, bssid)) n++;
    }
    return n;
}

// Follow-up §1/§2/§5: dropping AP metadata also drops every handshake /
// PMKID row for that BSSID so a later reuse cannot inherit nonce/PMKID/ESSID.
static void wipeHsForBssid(const uint8_t* bssid) {
    if (!bssid) return;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && macEq(s_hs[i].bssid, bssid))
            memset(&s_hs[i], 0, sizeof(Hs));
    }
}

static void inheritEssid(Hs* h);

static Hs* initHsSlot(Hs* h, const uint8_t* bssid, const uint8_t* sta) {
    memset(h, 0, sizeof(Hs));
    memcpy(h->bssid, bssid, 6);
    memcpy(h->sta, sta, 6);
    h->used = true;
    inheritEssid(h);
    return h;
}

static ApMeta* apFor(const uint8_t* bssid) {
    ApMeta* hit = findAp(bssid);
    if (hit) return hit;
    if (!bssid) return nullptr;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_ap[i].used) {
            memset(&s_ap[i], 0, sizeof(ApMeta));
            memcpy(s_ap[i].bssid, bssid, 6);
            s_ap[i].used = true;
            return &s_ap[i];
        }
    }
    // Table full: evict a *different* BSSID. Prefer an AP with no Hs rows,
    // then an AP whose Hs are already exported, then the first other slot.
    int victim = -1;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_ap[i].used || macEq(s_ap[i].bssid, bssid)) continue;
        if (countHsForBssid(s_ap[i].bssid) == 0) { victim = i; break; }
    }
    if (victim < 0) {
        for (uint8_t i = 0; i < MAX_HS; i++) {
            if (!s_ap[i].used || macEq(s_ap[i].bssid, bssid)) continue;
            bool allDone = true;
            bool any = false;
            for (uint8_t k = 0; k < MAX_HS; k++) {
                if (!s_hs[k].used || !macEq(s_hs[k].bssid, s_ap[i].bssid)) continue;
                any = true;
                if (!s_hs[k].wroteEapol && !s_hs[k].wrotePmkid) { allDone = false; break; }
            }
            if (any && allDone) { victim = i; break; }
        }
    }
    if (victim < 0) {
        for (uint8_t i = 0; i < MAX_HS; i++) {
            if (!macEq(s_ap[i].bssid, bssid)) { victim = i; break; }
        }
    }
    if (victim < 0) return findAp(bssid);
    wipeHsForBssid(s_ap[victim].bssid);
    memset(&s_ap[victim], 0, sizeof(ApMeta));
    memcpy(s_ap[victim].bssid, bssid, 6);
    s_ap[victim].used = true;
    return &s_ap[victim];
}

static void inheritEssid(Hs* h) {
    if (!h || (h->haveEssid && h->essidLen > 0)) return;
    ApMeta* a = findAp(h->bssid);
    if (a && a->haveEssid && a->essidLen > 0)
        setHsEssid(h, a->essid, a->essidLen);
}

// First non-empty ESSID for a BSSID wins; copy it onto every STA slot.
static void applyEssid(const uint8_t* bssid, const uint8_t* essid, uint8_t len) {
    if (!bssid || !essid || len == 0) return;
    if (len > 32) len = 32;
    ApMeta* a = apFor(bssid);
    if (!a) return;
    if (!(a->haveEssid && a->essidLen > 0)) {
        memcpy(a->essid, essid, len);
        a->essidLen = len;
        a->haveEssid = true;
    }
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used || !macEq(s_hs[i].bssid, bssid)) continue;
        if (!s_hs[i].haveEssid || s_hs[i].essidLen == 0)
            setHsEssid(&s_hs[i], a->essid, a->essidLen);
        s_hs[i].dirty = true;
    }
}

// Checklist §1: handshake state is keyed on (BSSID, STA) only.
// sta=nullptr is not used for EAPOL; beacons go through applyEssid().
static Hs* slotFor(const uint8_t* bssid, const uint8_t* sta) {
    if (!bssid || macZero(sta)) return nullptr;

    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used &&
            macEq(s_hs[i].bssid, bssid) &&
            macEq(s_hs[i].sta, sta))
            return &s_hs[i];
    }
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used)
            return initHsSlot(&s_hs[i], bssid, sta);
    }
    // Evict an exported row first (memset wipes PMKID/nonce — follow-up §5).
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].wroteEapol || s_hs[i].wrotePmkid)
            return initHsSlot(&s_hs[i], bssid, sta);
    }
    return initHsSlot(&s_hs[0], bssid, sta);
}

static void maybeWrite(Hs* h);

static bool writeLine(Hs* h, const char* suffix, const char* line) {
    if (!h) return false;
    Storage::ensureDir(Storage::DIR_HS);
    char path[80];
    makePath(h, suffix, path, sizeof(path));

    // Same rule as pcap: if a good file is already on SD, do not open "w"
    // (that truncates). A real WPA*01 / WPA*02 line is well over 64 bytes.
    // Returning true marks wroteEapol/wrotePmkid so we stop retrying.
    if (SD.exists(path)) {
        File probe = SD.open(path, "r");
        size_t sz = probe ? probe.size() : 0;
        if (probe) probe.close();
        if (sz >= 64) {
            Serial.printf("[22000] keep existing %s (%u bytes)\n",
                          Storage::baseName(path), (unsigned)sz);
            return true;
        }
        // Tiny/corrupt leftover — safe to replace.
        if (sz > 0 && sz < 64) SD.remove(path);
    }

    File f = SD.open(path, "w");
    if (!f) return false;
    f.println(line);
    f.close();
    char ssid[33];
    essidOf(h, ssid);
    if (ssid[0]) CapName::writeCompanionSsid(Storage::DIR_HS, Storage::baseName(path), ssid);
    char legacy[64];
    snprintf(legacy, sizeof(legacy),
             "%s/%02X-%02X-%02X-%02X-%02X-%02X%s",
             Storage::DIR_HS,
             h->bssid[0], h->bssid[1], h->bssid[2],
             h->bssid[3], h->bssid[4], h->bssid[5], suffix);
    if (strcmp(legacy, path) != 0 && SD.exists(legacy)) SD.remove(legacy);
    Serial.printf("[22000] wrote %s\n", Storage::baseName(path));
    return true;
}

static void seedEssid(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid || !ssid[0]) return;
    if (strcasecmp(ssid, "HIDDEN") == 0 || strcmp(ssid, "[UNKNOWN]") == 0) return;
    size_t n = strlen(ssid);
    if (n > 32) n = 32;
    applyEssid(bssid, (const uint8_t*)ssid, (uint8_t)n);
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && macEq(s_hs[i].bssid, bssid))
            maybeWrite(&s_hs[i]);
    }
}

// M3 replay is authenticator increment of M2's counter (WPA 4-way: n then n+1).
static bool replayIsNext(const uint8_t prev[8], const uint8_t next[8]) {
    uint8_t inc[8];
    memcpy(inc, prev, 8);
    for (int i = 7; i >= 0; i--) {
        inc[i] = (uint8_t)(inc[i] + 1);
        if (inc[i] != 0) break;
    }
    return memcmp(inc, next, 8) == 0;
}

// Follow-up §3: Pair 02 needs a complete M2 in this same Hs row (BSSID+STA)
// AND M3 whose replay is exactly M2+1. That is one 4-way progression.
static bool pair02Chained(const Hs* h) {
    if (!h || !h->haveM2 || !h->haveAnonce3) return false;
    if (h->m2Len < 97) return false;
    return replayIsNext(h->m2Replay, h->m3Replay);
}

static void maybeWrite(Hs* h) {
    if (!h || !h->haveEssid || h->essidLen == 0) return;
    if (macZero(h->sta)) return;
    char ap[13], sta[13], ess[65];
    hexEnc(h->bssid, 6, ap);
    hexEnc(h->sta, 6, sta);
    hexEnc(h->essid, h->essidLen, ess);

    // Same pairs as M5PORKCHOP: M1+M2 (00) or M2+M3 (02).
    if (!h->wroteEapol && h->haveM2 && h->m2Len >= 97) {
        uint8_t pair = 0xFF;
        const uint8_t* nonce = nullptr;
        // Pair 00: M1 and M2 same replay counter.
        if (h->haveAnonce && memcmp(h->anonceReplay, h->m2Replay, 8) == 0) {
            pair = 0x00;
            nonce = h->anonce;
        } else if (pair02Chained(h)) {
            pair = 0x02;
            nonce = h->anonce3;
        }
        if (nonce) {
            uint16_t eapolLen = (uint16_t)((h->m2[2] << 8) | h->m2[3]);
            eapolLen = (uint16_t)(eapolLen + 4);
            if (eapolLen > h->m2Len) eapolLen = h->m2Len;
            if (eapolLen >= 97 && eapolLen <= MAX_EAPOL) {
                uint8_t eapol[MAX_EAPOL];
                memcpy(eapol, h->m2, eapolLen);
                memset(eapol + 81, 0, 16);
                char mic[33], an[65];
                hexEnc(h->m2 + 81, 16, mic);
                hexEnc(nonce, 32, an);
                char ehex[MAX_EAPOL * 2 + 1];
                hexEnc(eapol, eapolLen, ehex);
                char line[768];
                snprintf(line, sizeof(line), "WPA*02*%s*%s*%s*%s*%s*%s*%02x",
                         mic, ap, sta, ess, an, ehex, (unsigned)pair);
                if (writeLine(h, "_hs.22000", line)) h->wroteEapol = true;
            }
        }
    }
    if (!h->wroteEapol && h->havePmkid && !h->wrotePmkid) {
        bool z = true;
        for (int i = 0; i < 16; i++) if (h->pmkid[i]) { z = false; break; }
        if (!z) {
            char pmk[33];
            hexEnc(h->pmkid, 16, pmk);
            char line[160];
            snprintf(line, sizeof(line), "WPA*01*%s*%s*%s*%s***01", pmk, ap, sta, ess);
            if (writeLine(h, ".22000", line)) h->wrotePmkid = true;
        }
    }
}

static uint16_t hdrLen80211(const uint8_t* f, uint16_t len) {
    if (len < 24) return 0;
    uint16_t off = 24;
    uint8_t type = (f[0] >> 2) & 0x03;
    if (type == 2 && (f[0] & 0x80)) off += 2; // QoS data
    if (f[1] & 0x80) off += 4;                 // HT ctrl / order
    if ((f[1] & 0x03) == 0x03) off += 6;       // 4-address
    if (off > len) return 0;
    return off;
}

static bool parseRsnPmkid(const uint8_t* ie, uint8_t ielen, uint8_t out[16]) {
    // version(2)+group(4)+pairCnt(2)+pair*4+akmCnt(2)+akm*4+caps(2)+pmkidCnt(2)+pmkid
    // Checklist §3: check remaining bytes BEFORE count * 4.
    if (ielen < 20) return false;
    uint16_t off = 2 + 4;
    if (off + 2 > ielen) return false;
    uint16_t pairCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    uint16_t remain = (uint16_t)(ielen - (off + 2));
    if (pairCnt > (remain / 4)) return false;
    off = (uint16_t)(off + 2 + pairCnt * 4);

    if (off + 2 > ielen) return false;
    uint16_t akmCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    remain = (uint16_t)(ielen - (off + 2));
    if (akmCnt > (remain / 4)) return false;
    off = (uint16_t)(off + 2 + akmCnt * 4);

    if (off + 2 > ielen) return false;
    off = (uint16_t)(off + 2); // rsn caps
    if (off + 2 > ielen) return false;
    uint16_t pmkCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    off = (uint16_t)(off + 2);
    if (pmkCnt == 0 || off + 16 > ielen) return false;
    bool z = true;
    for (int i = 0; i < 16; i++) if (ie[off + i]) { z = false; break; }
    if (z) return false;
    memcpy(out, ie + off, 16);
    return true;
}

static void parseAssoc(const uint8_t* f, uint16_t len) {
    if (len < 24) return;
    uint8_t subtype = (f[0] >> 4) & 0x0F;
    // PMKID rides in Association/Reassociation REQUEST (STA -> AP).
    // subtype 0 = Assoc Request, 2 = Reassoc Request.
    if (subtype != 0 && subtype != 2) return;
    // Checklist §5: min length before any fixed-offset MAC / IE walk.
    uint16_t fixedLen = (subtype == 2) ? 10 : 4;
    if (len < (uint16_t)(24 + fixedLen)) return;
    const uint8_t* bssid = f + 4;  // addr1: destination = AP
    const uint8_t* sta = f + 10;   // addr2: source = station
    uint16_t off = (uint16_t)(24 + fixedLen);
    if (off > len) return;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 48) {
            uint8_t pmk[16];
            if (parseRsnPmkid(f + off + 2, l, pmk)) {
                Hs* h = slotFor(bssid, sta);
                if (h && !h->wroteEapol && !h->wrotePmkid) {
                    memcpy(h->sta, sta, 6);
                    memcpy(h->pmkid, pmk, 16);
                    h->havePmkid = true;
                    h->dirty = true;
                }
            }
        }
        off = (uint16_t)(off + 2 + l);
    }
}

static void parseBeacon(const uint8_t* f, uint16_t len) {
    uint8_t fc = f[0] & 0xFC;
    if (fc != 0x80 && fc != 0x50) return;
    if (len < 24 + 12 + 2) return;
    const uint8_t* bssid = f + 16;
    uint16_t off = 24 + 12;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 0 && l > 0 && l <= 32) {
            applyEssid(bssid, f + off + 2, l);
            return;
        }
        off = (uint16_t)(off + 2 + l);
    }
}

static bool findPmkidKde(const uint8_t* payload, uint16_t len, uint8_t out[16]) {
    // EAPOL key descriptor 0x02, key data at 99, PMKID KDE dd 14 00 0f ac 04
    if (len < 121 || payload[4] != 0x02) return false;
    uint16_t keyDataLen = (uint16_t)((payload[97] << 8) | payload[98]);
    if (keyDataLen < 22 || len < 99 + keyDataLen) return false;
    const uint8_t* keyData = payload + 99;
    for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {
        if (keyData[i] == 0xdd && keyData[i + 1] == 0x14 &&
            keyData[i + 2] == 0x00 && keyData[i + 3] == 0x0f &&
            keyData[i + 4] == 0xac && keyData[i + 5] == 0x04) {
            const uint8_t* p = keyData + i + 6;
            bool z = true;
            for (int k = 0; k < 16; k++) if (p[k]) { z = false; break; }
            if (z) return false;
            memcpy(out, p, 16);
            return true;
        }
    }
    return false;
}

static void parseEapol(const uint8_t* f, uint16_t len) {
    uint16_t off = hdrLen80211(f, len);
    if (off + 8 + 4 > len) return;
    if (f[off] != 0xAA || f[off + 1] != 0xAA || f[off + 2] != 0x03) return;
    if (f[off + 6] != 0x88 || f[off + 7] != 0x8E) return;
    const uint8_t* e = f + off + 8;
    uint16_t elen = (uint16_t)(len - off - 8);
    if (elen < 99 || e[1] != 0x03) return;

    uint16_t body = (uint16_t)((e[2] << 8) | e[3]);
    uint32_t trueTotal = 4u + (uint32_t)body;
    bool eapolIncomplete = trueTotal > elen;
    uint16_t total = (uint16_t)((trueTotal < elen) ? trueTotal : elen);
    bool eapolOverCap = total > MAX_EAPOL;
    if (eapolOverCap) total = MAX_EAPOL;
    bool m2Storable = !eapolIncomplete && !eapolOverCap;

    uint16_t ki = (uint16_t)((e[5] << 8) | e[6]);
    uint8_t install = (uint8_t)((ki >> 6) & 1);
    uint8_t keyAck = (uint8_t)((ki >> 7) & 1);
    uint8_t keyMic = (uint8_t)((ki >> 8) & 1);
    uint8_t secure = (uint8_t)((ki >> 9) & 1);

    uint8_t msg = 0;
    if (keyAck && !keyMic) msg = 1;
    else if (!keyAck && keyMic && !secure) msg = 2;
    else if (keyAck && keyMic && install) msg = 3;
    else if (!keyAck && keyMic && secure) msg = 4;
    if (msg == 0) return;

    // Diagnostic only: pause deauth after M1 even if the frame is truncated.
    if (msg == 1) s_lastM1Ms = millis();

    // Checklist §6: incomplete EAPOL is not exportable handshake state.
    if (eapolIncomplete) return;

    const uint8_t* srcMac = f + 10;
    const uint8_t* dstMac = f + 4;
    uint8_t bssid[6], sta[6];
    if (msg == 1 || msg == 3) {
        memcpy(bssid, srcMac, 6);
        memcpy(sta, dstMac, 6);
    } else {
        memcpy(bssid, dstMac, 6);
        memcpy(sta, srcMac, 6);
    }

    Hs* h = slotFor(bssid, sta);
    if (!h) return;
    memcpy(h->sta, sta, 6);

    // Follow-up §4: after a successful EAPOL export this (BSSID,STA) row
    // is immutable. No mix of old nonce/M2 with a later attempt. A new
    // handshake from the same STA needs a fresh capture session (or this
    // row evicted after wroteEapol).
    if (h->wroteEapol) return;

    if (msg == 1) {
        if (!h->haveAnonce) {
            memcpy(h->anonce, e + 17, 32);
            memcpy(h->anonceReplay, e + 9, 8);
            h->haveAnonce = true;
        } else if (h->haveM2 &&
                   memcmp(h->anonceReplay, h->m2Replay, 8) != 0 &&
                   memcmp(e + 9, h->m2Replay, 8) == 0) {
            memcpy(h->anonce, e + 17, 32);
            memcpy(h->anonceReplay, e + 9, 8);
        }
        uint8_t pmk[16];
        if (!h->wrotePmkid && findPmkidKde(e, total, pmk)) {
            memcpy(h->pmkid, pmk, 16);
            h->havePmkid = true;
        }
    } else if (msg == 3) {
        if (pair02Chained(h)) {
            // Frozen: already have a proven M2→M3 progression.
        } else if (!h->haveAnonce3) {
            memcpy(h->anonce3, e + 17, 32);
            memcpy(h->m3Replay, e + 9, 8);
            h->haveAnonce3 = true;
        } else if (h->haveM2 &&
                   !replayIsNext(h->m2Replay, h->m3Replay) &&
                   replayIsNext(h->m2Replay, e + 9)) {
            memcpy(h->anonce3, e + 17, 32);
            memcpy(h->m3Replay, e + 9, 8);
        }
    } else if (msg == 2) {
        // Do not break a chained M2+M3 (M2a+M3a then M2b must not export 02
        // from M2b+M3a). Do not replace a M1-matching M2.
        if (pair02Chained(h)) {
            // Frozen.
        } else if (m2Storable && !h->haveM2) {
            memcpy(h->m2, e, total);
            h->m2Len = total;
            memcpy(h->m2Replay, e + 9, 8);
            h->haveM2 = true;
        } else if (m2Storable && h->haveM2) {
            bool heldMatchesM1 = h->haveAnonce &&
                memcmp(h->anonceReplay, h->m2Replay, 8) == 0;
            if (!heldMatchesM1) {
                bool newMatchesM1 = h->haveAnonce &&
                    memcmp(e + 9, h->anonceReplay, 8) == 0;
                bool newChainsM3 = h->haveAnonce3 &&
                    replayIsNext(e + 9, h->m3Replay);
                // Latest-M2 only while there is no M1 and no M3 to bind to.
                bool unboundRetry = !h->haveAnonce && !h->haveAnonce3;
                if (newMatchesM1 || newChainsM3 || unboundRetry) {
                    memcpy(h->m2, e, total);
                    h->m2Len = total;
                    memcpy(h->m2Replay, e + 9, 8);
                }
            }
        }
    } else if (msg == 4) {
        h->haveM4 = true;
    }
    h->dirty = true;
}

void reset() {
    memset(s_hs, 0, sizeof(s_hs));
    memset(s_ap, 0, sizeof(s_ap));
    s_lastM1Ms = 0;
}

void flushPending() {
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used) continue;
        if (!s_hs[i].dirty) continue;
        s_hs[i].dirty = false;
        inheritEssid(&s_hs[i]);
        if (s_hs[i].haveEssid && s_hs[i].essidLen > 0) {
            maybeWrite(&s_hs[i]);
        }
    }
}

bool shouldPauseDeauth() {
    uint16_t pause = Config::radio().pauseMs;
    if (pause < 200) pause = 200;
    return s_lastM1Ms != 0 && (millis() - s_lastM1Ms) < pause;
}

bool hasPair(const uint8_t* bssid) {
    if (!bssid) return false;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && macEq(s_hs[i].bssid, bssid) &&
            (s_hs[i].wroteEapol || s_hs[i].wrotePmkid))
            return true;
    }
    return false;
}

uint16_t pairCount() {
    uint16_t n = 0;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].wroteEapol || s_hs[i].wrotePmkid) n++;
    }
    return n;
}

uint8_t handshakeMask(const uint8_t* bssid) {
    if (!bssid) return 0;
    uint8_t m = 0;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used || !macEq(s_hs[i].bssid, bssid)) continue;
        if (s_hs[i].haveAnonce)  m |= 0x01;
        if (s_hs[i].haveM2)      m |= 0x02;
        if (s_hs[i].haveAnonce3) m |= 0x04;
        if (s_hs[i].haveM4)      m |= 0x08;
    }
    return m;
}

bool hasHandshake(const uint8_t* bssid, uint8_t depth) {
    if (!hasPair(bssid)) return false;
    if (depth == 0) return true;
    uint8_t m = handshakeMask(bssid);
    if (depth >= 1 && !(m & 0x04)) return false;
    if (depth >= 2 && !(m & 0x08)) return false;
    return true;
}

void feed(const uint8_t* frame, uint16_t len) {
    if (!frame || len < 24) return;
    uint8_t type = (frame[0] >> 2) & 0x03;
    if (type == 0) {
        uint8_t subtype = (frame[0] >> 4) & 0x0F;
        if (subtype == 8 || subtype == 5) parseBeacon(frame, len);
        else if (subtype == 0 || subtype == 2) parseAssoc(frame, len);
    } else if (type == 2) parseEapol(frame, len);
}

uint16_t convertPcap(const char* pcapPath) {
    if (!pcapPath) return 0;
    reset();
    char hex[13] = {0}, nameSsid[33] = {0};
    CapName::extractBssidHex(pcapPath, hex);
    CapName::extractSsidFromName(pcapPath, nameSsid);
    if (!nameSsid[0]) {
        CapName::readCompanionSsid(Storage::DIR_HS, Storage::baseName(pcapPath), nameSsid);
    }

    File f = SD.open(pcapPath, "r");
    if (!f) return 0;
    uint8_t fh[24];
    if (f.read(fh, 24) != 24) {
        f.close();
        return 0;
    }
    while (f.available()) {
        uint8_t ph[16];
        if (f.read(ph, 16) != 16) break;
        uint32_t incl = (uint32_t)ph[8] | ((uint32_t)ph[9] << 8) |
                        ((uint32_t)ph[10] << 16) | ((uint32_t)ph[11] << 24);
        if (incl < 8 || incl > 2048) break;
        uint8_t rth[8];
        if (f.read(rth, 8) != 8) break;
        uint16_t rtLen = (uint16_t)(rth[2] | (rth[3] << 8));
        if (rtLen < 8 || rtLen > 64 || incl < rtLen) break;
        if (rtLen > 8) {
            uint8_t extra[56];
            uint16_t nskip = (uint16_t)(rtLen - 8);
            if (f.read(extra, nskip) != nskip) break;
        }
        uint32_t flen = incl - rtLen;
        if (flen > 400) {
            uint8_t dump[64];
            while (flen) {
                size_t c = flen > sizeof(dump) ? sizeof(dump) : flen;
                if (f.read(dump, c) != c) break;
                flen -= c;
            }
            continue;
        }
        uint8_t frame[400];
        if (f.read(frame, flen) != (int)flen) break;
        feed(frame, (uint16_t)flen);
        yield();
    }
    f.close();

    uint8_t mac[6];
    if (hex[0] && CapName::hexToMac(hex, mac)) {
        seedEssid(mac, nameSsid);
    }

    uint16_t n = 0;
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].wroteEapol || s_hs[i].wrotePmkid) n++;
    }
    return n;
}

struct ConvCtx {
    uint16_t n;
};

static void convOne(const char* name, size_t, void* raw) {
    size_t L = strlen(name);
    if (L < 6 || strcasecmp(name + L - 5, ".pcap") != 0) return;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, name);
    uint16_t add = convertPcap(path);
    ((ConvCtx*)raw)->n = (uint16_t)(((ConvCtx*)raw)->n + add);
}

uint16_t convertAllPcaps() {
    ConvCtx ctx{0};
    Storage::forEachHandshake(convOne, &ctx);
    return ctx.n;
}

} // namespace Hc22000
