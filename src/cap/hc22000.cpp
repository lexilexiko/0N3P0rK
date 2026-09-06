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

static Hs s_hs[MAX_HS];

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

static Hs* slotFor(const uint8_t* bssid) {
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && memcmp(s_hs[i].bssid, bssid, 6) == 0) return &s_hs[i];
    }
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used) {
            memset(&s_hs[i], 0, sizeof(Hs));
            memcpy(s_hs[i].bssid, bssid, 6);
            s_hs[i].used = true;
            return &s_hs[i];
        }
    }
    memset(&s_hs[0], 0, sizeof(Hs));
    memcpy(s_hs[0].bssid, bssid, 6);
    s_hs[0].used = true;
    return &s_hs[0];
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
    Hs* h = slotFor(bssid);
    if (h->haveEssid && h->essidLen > 0) {
        maybeWrite(h);
        return;
    }
    size_t n = strlen(ssid);
    if (n > 32) n = 32;
    memcpy(h->essid, ssid, n);
    h->essidLen = (uint8_t)n;
    h->haveEssid = true;
    maybeWrite(h);
}

static void maybeWrite(Hs* h) {
    if (!h || !h->haveEssid || h->essidLen == 0) return;
    char ap[13], sta[13], ess[65];
    hexEnc(h->bssid, 6, ap);
    hexEnc(h->sta, 6, sta);
    hexEnc(h->essid, h->essidLen, ess);

    // Same pairs as M5PORKCHOP: M1+M2 (00) or M2+M3 (02).
    if (!h->wroteEapol && h->haveM2 && h->m2Len >= 97) {
        uint8_t pair = 0xFF;
        const uint8_t* nonce = nullptr;
        // Pair 00 requires M1 and M2 to belong to the same exchange (matching
        // replay counter) — otherwise anonce and MIC come from different
        // attempts and hashcat will never recover the key.
        if (h->haveAnonce && memcmp(h->anonceReplay, h->m2Replay, 8) == 0) {
            pair = 0x00;
            nonce = h->anonce;
        } else if (h->haveAnonce3) {
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
    return off;
}

static bool parseRsnPmkid(const uint8_t* ie, uint8_t ielen, uint8_t out[16]) {
    // version(2)+group(4)+pairCnt(2)+pair*4+akmCnt(2)+akm*4+caps(2)+pmkidCnt(2)+pmkid
    if (ielen < 20) return false;
    uint16_t off = 2 + 4;
    if (off + 2 > ielen) return false;
    uint16_t pairCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
    off = (uint16_t)(off + 2 + pairCnt * 4);
    if (off + 2 > ielen) return false;
    uint16_t akmCnt = (uint16_t)(ie[off] | (ie[off + 1] << 8));
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
    uint8_t subtype = (f[0] >> 4) & 0x0F;
    if (subtype != 1) return; // association response
    const uint8_t* bssid = f + 16;
    const uint8_t* sta = f + 4;
    uint16_t off = (uint16_t)(24 + 6);
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 48) {
            uint8_t pmk[16];
            if (parseRsnPmkid(f + off + 2, l, pmk)) {
                Hs* h = slotFor(bssid);
                memcpy(h->sta, sta, 6);
                memcpy(h->pmkid, pmk, 16);
                h->havePmkid = true;
                // No SD I/O from the IRQ - flushPending() handles it.
                h->dirty = true;
            }
        }
        off = (uint16_t)(off + 2 + l);
    }
}

static void parseBeacon(const uint8_t* f, uint16_t len) {
    uint8_t fc = f[0] & 0xFC;
    if (fc != 0x80 && fc != 0x50) return;
    const uint8_t* bssid = f + 16;
    uint16_t off = 24 + 12;
    if (off + 2 > len) return;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 0 && l > 0 && l <= 32) {
            Hs* h = slotFor(bssid);
            memcpy(h->essid, f + off + 2, l);
            h->essidLen = l;
            h->haveEssid = true;
            // No SD I/O here - this runs from the WiFi promiscuous IRQ.
            // flushPending() in loop() will call maybeWrite() shortly.
            h->dirty = true;
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
    uint16_t total = (uint16_t)(4 + body);
    if (total > elen) total = elen;
    if (total > MAX_EAPOL) total = MAX_EAPOL;

    // keyInfo at EAPOL payload[5..6]
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

    Hs* h = slotFor(bssid);
    memcpy(h->sta, sta, 6);

    // First copy of each message wins — retransmit can change nonce/MIC.
    // Key replay counter (e[9..16]) ties M1 and M2 to the *same* handshake attempt:
    // an AP retry sends a new M1 with a bumped counter, and a stale M1/M2 pair
    // produces a PMK/MIC that will never crack. Only pair when counters match.
    if (msg == 1) {
        if (!h->haveAnonce) {
            memcpy(h->anonce, e + 17, 32);
            memcpy(h->anonceReplay, e + 9, 8);
            h->haveAnonce = true;
        } else if (h->haveM2 && !h->wroteEapol &&
                   memcmp(h->anonceReplay, h->m2Replay, 8) != 0 &&
                   memcmp(e + 9, h->m2Replay, 8) == 0) {
            // Earlier M1 didn't match the M2 we're holding; this one does — replace it.
            memcpy(h->anonce, e + 17, 32);
            memcpy(h->anonceReplay, e + 9, 8);
        }
        s_lastM1Ms = millis();
        uint8_t pmk[16];
        if (findPmkidKde(e, total, pmk)) {
            memcpy(h->pmkid, pmk, 16);
            h->havePmkid = true;
        }
    } else if (msg == 3) {
        if (!h->haveAnonce3) {
            memcpy(h->anonce3, e + 17, 32);
            h->haveAnonce3 = true;
        }
    } else if (msg == 2 && !h->haveM2) {
        memcpy(h->m2, e, total);
        h->m2Len = total;
        memcpy(h->m2Replay, e + 9, 8);
        h->haveM2 = true;
    } else if (msg == 4) {
        h->haveM4 = true;
    }
    // No SD I/O here - this runs from the WiFi promiscuous IRQ on every
    // EAPOL frame. Mark the slot dirty and let flushPending() in loop()
    // call maybeWrite() instead.
    h->dirty = true;
}

void reset() {
    memset(s_hs, 0, sizeof(s_hs));
    s_lastM1Ms = 0;
}

void flushPending() {
    // Loop-context only. Walks every slot and, for the ones feed() marked
    // dirty from the IRQ, runs maybeWrite() to actually open/close the
    // .22000 files on SD. Without this, parseBeacon/parseEapol/parseAssoc
    // would have to do SD I/O directly from the promiscuous callback -
    // SD isn't ISR-safe and the radio would WDT the moment any beacon or
    // EAPOL arrived under load.
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (!s_hs[i].used) continue;
        if (!s_hs[i].dirty) continue;
        s_hs[i].dirty = false;
        // haveEssid is required by maybeWrite() anyway, and we want to
        // drop the dirty bit even if no write was actually performed,
        // otherwise we'd re-check the same slot every loop tick forever.
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
        if (s_hs[i].used && memcmp(s_hs[i].bssid, bssid, 6) == 0)
            return s_hs[i].wroteEapol || s_hs[i].wrotePmkid;
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
    for (uint8_t i = 0; i < MAX_HS; i++) {
        if (s_hs[i].used && memcmp(s_hs[i].bssid, bssid, 6) == 0) {
            uint8_t m = 0;
            if (s_hs[i].haveAnonce)  m |= 0x01; // M1
            if (s_hs[i].haveM2)      m |= 0x02; // M2
            if (s_hs[i].haveAnonce3) m |= 0x04; // M3
            if (s_hs[i].haveM4)      m |= 0x08; // M4
            return m;
        }
    }
    return 0;
}

bool hasHandshake(const uint8_t* bssid, uint8_t depth) {
    // M1+M2 (validated, crackable) is the floor no matter what depth asks
    // for - depth only ever adds stricter requirements on top of it.
    if (!hasPair(bssid)) return false;
    if (depth == 0) return true;
    uint8_t m = handshakeMask(bssid);
    if (depth >= 1 && !(m & 0x04)) return false; // +M3
    if (depth >= 2 && !(m & 0x08)) return false; // +M4 (full 4-way)
    return true;
}

void feed(const uint8_t* frame, uint16_t len) {
    if (!frame || len < 24) return;
    uint8_t type = (frame[0] >> 2) & 0x03;
    if (type == 0) {
        uint8_t subtype = (frame[0] >> 4) & 0x0F;
        if (subtype == 8 || subtype == 5) parseBeacon(frame, len);
        else if (subtype == 1) parseAssoc(frame, len);
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
