// modes/inspectorpig.cpp
#include "inspectorpig.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../core/app.h"
#include "../storage/littlefs_ops.h"
#include "../cap/capture_name.h"
#include <SD.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---- container constants ---------------------------------------------------
static const uint32_t PCAP_MAGIC_LE = 0xD4C3B2A1u;   // file bytes D4 C3 B2 A1
static const uint32_t PCAP_MAGIC_BE = 0xA1B2C3D4u;
static const uint32_t PCAPNG_MAGIC  = 0x0A0D0D0Au;
static const uint16_t LINK_RADIOTAP = 127;
static const uint16_t LINK_80211    = 105;

// File handle used by emit() while a report is being written.
static File s_reportOut;
// When set, emit() sends text to the report file only and leaves the screen
// buffer alone (used by the ALL run, which would otherwise overflow it).
static bool s_streamOnly = false;

bool InspectorPig::running = false;
InspectorPig::Phase InspectorPig::phase = InspectorPig::Phase::LIST;
InspectorPig::Entry InspectorPig::entries[InspectorPig::MAX_ENTRIES];
uint8_t InspectorPig::entryCount = 0;
uint8_t InspectorPig::sel = 0;
uint8_t InspectorPig::scroll = 0;
bool InspectorPig::keyLatch = false;
char InspectorPig::statusMsg[40] = "";
char InspectorPig::lines[InspectorPig::MAX_LINES][InspectorPig::LINE_LEN];
uint8_t InspectorPig::lineCount = 0;
uint8_t InspectorPig::lineScroll = 0;

// ---- small helpers ---------------------------------------------------------
static bool endsWithCI(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t n = strlen(s), m = strlen(suffix);
    if (m == 0 || n < m) return false;
    return strcasecmp(s + n - m, suffix) == 0;
}

static bool isPcapName(const char* n) {
    return endsWithCI(n, ".pcap") || endsWithCI(n, ".cap") || endsWithCI(n, ".pcapng");
}

static bool isHc22000Name(const char* n) {
    return endsWithCI(n, ".22000") || endsWithCI(n, ".hc22000");
}

void InspectorPig::macToStr(const uint8_t* mac, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ESSID and similar fields travel as hex in the 22000 line.
void InspectorPig::hexToAscii(const char* hex, char* out, size_t outLen) {
    if (!out || outLen < 2) return;
    size_t o = 0;
    if (hex) {
        for (size_t i = 0; hex[i] && hex[i + 1] && o + 1 < outLen; i += 2) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = nib(hex[i]), lo = nib(hex[i + 1]);
            if (hi < 0 || lo < 0) break;
            uint8_t b = (uint8_t)((hi << 4) | lo);
            // Keep the report readable: unprintable bytes become '.'.
            out[o++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
    }
    out[o] = '\0';
}

const char* InspectorPig::verdictFor(uint8_t score) {
    if (score >= 85) return "GOOD";
    if (score >= 60) return "USABLE";
    if (score >= 30) return "PARTIAL";
    return "BROKEN";
}

// ---- hex helpers -----------------------------------------------------------
static size_t hexToBytes(const char* hex, uint8_t* out, size_t maxOut) {
    if (!hex || !out) return 0;
    size_t o = 0;
    for (size_t i = 0; hex[i] && hex[i + 1] && o < maxOut; i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out[o++] = (uint8_t)((hi << 4) | lo);
    }
    return o;
}

static bool allZero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

static bool isHexLen(const char* s, size_t want) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return n == want;
}

// EAPOL-Key message number from the key information bits.
static uint8_t eapolMsgOf(const uint8_t* e, size_t total) {
    if (!e || total < 7) return 0;
    uint16_t ki = (uint16_t)((e[5] << 8) | e[6]);
    bool install = ((ki >> 6) & 1) != 0;
    bool ack     = ((ki >> 7) & 1) != 0;
    bool mic     = ((ki >> 8) & 1) != 0;
    bool secure  = ((ki >> 9) & 1) != 0;
    if (ack && !mic) return 1;
    if (!ack && mic && !secure) return 2;
    if (ack && mic && install) return 3;
    if (!ack && mic && secure) return 4;
    return 0;
}

// ---------------------------------------------------------------------------
// .22000 hash line. Field layout:
//   WPA*01*PMKID*BSSID*STA*ESSID***01
//   WPA*02*MIC*BSSID*STA*ESSID*ANONCE*EAPOL*PAIR
// ---------------------------------------------------------------------------
uint8_t InspectorPig::analyze22000(const uint8_t* data, size_t len) {
    char line[1024];
    size_t n = 0;
    while (n < len && n + 1 < sizeof(line) && data[n] != '\n' && data[n] != '\r') {
        line[n] = (char)data[n];
        n++;
    }
    line[n] = '\0';

    emit("source     : .22000 hash line");
    emit("bytes      : %u", (unsigned)len);

    if (strncmp(line, "WPA*", 4) != 0) {
        emit("magic      : missing WPA* prefix");
        emit("result     : not a hashcat 22000 line");
        return 0;
    }

    char* f[10] = {0};
    uint8_t nf = 0;
    char* p = line;
    f[nf++] = p;
    while (*p && nf < 10) {
        if (*p == '*') { *p = '\0'; f[nf++] = p + 1; }
        p++;
    }
    const char* type = (nf > 1) ? f[1] : "";
    const char* f2 = (nf > 2) ? f[2] : "";
    const char* f3 = (nf > 3) ? f[3] : "";
    const char* f4 = (nf > 4) ? f[4] : "";
    const char* f5 = (nf > 5) ? f[5] : "";
    const char* f6 = (nf > 6) ? f[6] : "";
    const char* f7 = (nf > 7) ? f[7] : "";
    const char* f8 = (nf > 8) ? f[8] : "";

    bool pmkid = (strcmp(type, "01") == 0);
    bool eapol = (strcmp(type, "02") == 0);
    emit("type       : WPA*%s %s", type,
         pmkid ? "(PMKID)" : (eapol ? "(EAPOL pair)" : "(unknown)"));
    if (!pmkid && !eapol) {
        emit("result     : unknown subtype");
        return 0;
    }

    char ssid[33];
    hexToAscii(f5, ssid, sizeof(ssid));
    emit("bssid      : %s", f3);
    emit("station    : %s", f4);
    emit("essid      : \"%s\"", ssid);

    uint8_t score = 30;
    if (ssid[0]) score += 15;
    else emit("probe      : empty essid");

    if (pmkid) {
        emit("pmkid      : %s", f2);
        if (!isHexLen(f2, 32)) { emit("probe      : pmkid is not 16 bytes"); return 25; }
        uint8_t raw[16];
        hexToBytes(f2, raw, sizeof(raw));
        if (allZero(raw, sizeof(raw))) { emit("probe      : pmkid is all zeros"); return 25; }
        if (!isHexLen(f3, 12)) { emit("probe      : bad bssid"); score -= 15; }
        score += 50;
        emit("result     : complete PMKID, no client needed");
        return (score > 100) ? 100 : score;
    }

    // ---- WPA*02: EAPOL pair -------------------------------------------------
    emit("mic        : %s", f2);
    emit("anonce     : %s", f6);
    emit("pair       : 0x%s", f8);

    if (!isHexLen(f2, 32)) emit("probe      : mic is not 16 bytes");
    else {
        uint8_t raw[16];
        hexToBytes(f2, raw, sizeof(raw));
        if (allZero(raw, sizeof(raw))) emit("probe      : mic is all zeros");
        else score += 15;
    }
    if (!isHexLen(f6, 64)) emit("probe      : anonce is not 32 bytes");
    else {
        uint8_t raw[32];
        hexToBytes(f6, raw, sizeof(raw));
        if (allZero(raw, sizeof(raw))) emit("probe      : anonce is all zeros");
        else score += 15;
    }
    if (strcmp(f8, "00") == 0 || strcmp(f8, "02") == 0) score += 10;
    else {
        emit("probe      : pair should be 00 or 02");
        score = (score > 20) ? (uint8_t)(score - 20) : 0;
    }

    // Decode the embedded EAPOL and check it against 802.11i.
    uint8_t* e = (uint8_t*)malloc(512);
    if (!e) {
        emit("probe      : out of memory for eapol");
        return score;
    }
    size_t el = hexToBytes(f7, e, 512);
    emit("eapol      : %u bytes", (unsigned)el);
    if (el < 99) {
        emit("probe      : eapol shorter than 99 bytes");
        free(e);
        return (score > 25) ? (uint8_t)(score - 25) : 0;
    }

    uint16_t body  = (uint16_t)((e[2] << 8) | e[3]);
    uint16_t total = (uint16_t)(4 + body);
    emit("eapol hdr  : %u %s", (unsigned)total,
         (total == el) ? "(length ok)" : "(length mismatch!)");
    if (total == el) score += 10;
    if (e[1] != 3) emit("probe      : not an EAPOL-Key frame");
    emit("descriptor : %u %s", (unsigned)e[4],
         e[4] == 2 ? "RSN/WPA2" : (e[4] == 254 ? "WPA1" : "?"));

    uint8_t msg = eapolMsgOf(e, el);
    emit("message    : M%u", (unsigned)msg);
    if (msg == 0) emit("probe      : key info does not match M1..M4");
    uint16_t ki = (uint16_t)((e[5] << 8) | e[6]);
    emit("key info   : 0x%04X", (unsigned)ki);
    emit("replay     : %02x%02x%02x%02x%02x%02x%02x%02x",
         e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16]);
    emit("nonce      : %s", allZero(e + 17, 32) ? "all zeros (!)" : "present");

    // hashcat re-derives the MIC, so the copy inside the EAPOL payload must be
    // zeroed. A non-zero copy means the line was built from a raw frame.
    bool micZeroed = allZero(e + 81, 16);
    emit("mic in blob: %s", micZeroed ? "zeroed (correct)" : "NOT zeroed (!)");
    if (micZeroed) score += 10;

    uint16_t kdLen = (uint16_t)((e[97] << 8) | e[98]);
    emit("key data   : %u bytes", (unsigned)kdLen);
    if (kdLen >= 20 && (size_t)(99 + kdLen) <= el && e[99] == 0x30) {
        const uint8_t* kd = e + 99;
        const uint8_t* g  = kd + 4;
        const char* gname = (g[0] == 0x00 && g[1] == 0x0F && g[2] == 0xAC)
            ? (g[3] == 0x04 ? "CCMP" : (g[3] == 0x02 ? "TKIP" : "other"))
            : "unknown";
        emit("rsn ver    : %u", (unsigned)(kd[2] | (kd[3] << 8)));
        emit("group ciph : %s", gname);
        uint16_t pc = (uint16_t)(kd[8] | (kd[9] << 8));
        if (pc >= 1 && (uint16_t)(14) <= (uint16_t)(kd[1] + 2)) {
            const uint8_t* pw = kd + 10;
            const char* pname = (pw[0] == 0x00 && pw[1] == 0x0F && pw[2] == 0xAC)
                ? (pw[3] == 0x04 ? "CCMP" : (pw[3] == 0x02 ? "TKIP" : "other"))
                : "unknown";
            emit("pairwise   : %s", pname);
        }
        uint16_t akOff = (uint16_t)(10 + pc * 4);
        if ((uint16_t)(akOff + 6) <= (uint16_t)(kd[1] + 2)) {
            const uint8_t* ak = kd + akOff + 2;
            const char* aname = (ak[0] == 0x00 && ak[1] == 0x0F && ak[2] == 0xAC)
                ? (ak[3] == 0x01 ? "WPA" : (ak[3] == 0x02 ? "PSK" :
                   (ak[3] == 0x04 ? "WPA2-ENT" : (ak[3] == 0x06 ? "PSK-SHA256" :
                   (ak[3] == 0x08 ? "SAE" : "other")))))
                : "unknown";
            emit("akm        : %s", aname);
        }
        score += 5;
    } else {
        emit("key data   : no RSN IE in this message");
    }
    free(e);

    emit("result     : %s",
         (score >= 85) ? "complete EAPOL pair"
                       : "EAPOL pair looks incomplete");
    return (score > 100) ? 100 : score;
}

// ---- 802.11 information elements -------------------------------------------
// Beacon/probe-response tagged parameters start after the 24-byte MAC header
// plus the 12-byte fixed field.
static void parseIes(const uint8_t* f, uint16_t flen, uint16_t off,
                     char* ssid, size_t ssidLen, uint8_t* chan) {
    while ((uint32_t)off + 2 <= (uint32_t)flen) {
        uint8_t id = f[off];
        uint8_t ln = f[off + 1];
        if ((uint32_t)off + 2 + ln > (uint32_t)flen) break;
        if (id == 0 && ssid && ssidLen > 1 && ssid[0] == '\0' && ln > 0) {
            size_t k = (ln < ssidLen - 1) ? ln : ssidLen - 1;
            memcpy(ssid, f + off + 2, k);
            ssid[k] = '\0';
        } else if (id == 3 && chan && ln == 1 && *chan == 0) {
            *chan = f[off + 2];
        }
        off = (uint16_t)(off + 2 + ln);
    }
}

// ---------------------------------------------------------------------------
// Classic PCAP. Walks the record stream the way Wireshark does: container
// header, then {packet header, radiotap, 802.11 frame} for every packet.
// ---------------------------------------------------------------------------
uint8_t InspectorPig::analyzePcap(const uint8_t* data, size_t len) {
    emit("source     : pcap capture");

    if (len < 24) {
        emit("error      : smaller than a pcap global header");
        emit("result     : broken container");
        return 0;
    }
    uint32_t magic = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    if (magic == PCAPNG_MAGIC) {
        emit("magic      : 0A0D0D0A  pcapng");
        emit("note       : pcapng blocks are not decoded by this inspector");
        emit("result     : container recognised, contents not inspected");
        return 40;
    }
    const bool le = (magic == PCAP_MAGIC_LE);
    const bool be = (magic == PCAP_MAGIC_BE);
    if (!le && !be) {
        emit("magic      : %08X  NOT a pcap", (unsigned)magic);
        emit("result     : not a capture container");
        return 0;
    }
    auto rd16 = [&](const uint8_t* p) -> uint16_t {
        return le ? (uint16_t)(p[0] | (p[1] << 8))
                  : (uint16_t)((p[0] << 8) | p[1]);
    };
    auto rd32 = [&](const uint8_t* p) -> uint32_t {
        return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
                  : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
    };

    emit("magic      : %s  pcap v%u.%u", le ? "D4C3B2A1 (LE)" : "A1B2C3D4 (BE)",
         (unsigned)rd16(data + 4), (unsigned)rd16(data + 6));
    emit("snaplen    : %u", (unsigned)rd32(data + 16));
    uint32_t linktype = rd32(data + 20);
    emit("linktype   : %u %s", (unsigned)linktype,
         linktype == LINK_RADIOTAP ? "(radiotap+802.11)"
                                   : (linktype == LINK_80211 ? "(raw 802.11)"
                                                             : "(unexpected!)"));

    uint32_t off = 24;
    uint16_t frames = 0, badRecords = 0, beacons = 0, proberesp = 0;
    uint16_t eapolSeen = 0, shownEapol = 0;
    uint8_t  chan = 0;
    int8_t   rssi = 0;
    bool     hasRadiotap = false;
    char     ssid[33] = "";
    bool     mSeen[5] = {false, false, false, false, false};
    uint8_t  replayM1[8] = {0}, replayM2[8] = {0};
    bool     haveM1 = false, haveM2 = false;

    emit("-- packet records --");
    while ((uint32_t)off + 16 <= (uint32_t)len) {
        uint32_t incl = rd32(data + off + 8);
        if (incl == 0 || (uint32_t)off + 16 + incl > (uint32_t)len) {
            emit("  record %u truncated (%u bytes claimed)",
                 (unsigned)frames, (unsigned)incl);
            badRecords++;
            break;
        }
        uint16_t flen = (uint16_t)incl;
        const uint8_t* pkt = data + off + 16;
        off += 16 + incl;
        frames++;

        const uint8_t* f = pkt;
        if (linktype == LINK_RADIOTAP) {
            if (flen < 8) continue;
            uint8_t itLen = pkt[2];
            if (itLen < 8 || itLen > 80 || itLen > flen) continue;
            hasRadiotap = true;
            uint32_t present = (uint32_t)pkt[4] | ((uint32_t)pkt[5] << 8) |
                               ((uint32_t)pkt[6] << 16) | ((uint32_t)pkt[7] << 24);
            uint16_t rp = 8;
            if (present & (1u << 0)) { rp = (uint16_t)((rp + 7) & ~7u); rp += 8; }
            if (present & (1u << 1)) rp += 1;
            if (present & (1u << 2)) rp += 1;
            if (present & (1u << 3)) {
                rp = (uint16_t)((rp + 1) & ~1u);
                if ((uint32_t)rp + 2 <= (uint32_t)itLen) {
                    uint16_t freq = (uint16_t)(pkt[rp] | (pkt[rp + 1] << 8));
                    if (freq >= 2412 && freq <= 2472) chan = (uint8_t)((freq - 2407) / 5);
                    else if (freq == 2484) chan = 14;
                }
                rp += 4;
            }
            if (present & (1u << 4)) { rp = (uint16_t)((rp + 1) & ~1u); rp += 2; }
            if (present & (1u << 5)) {
                if ((uint32_t)rp < (uint32_t)itLen) rssi = (int8_t)pkt[rp];
                rp += 1;
            }
            f = pkt + itLen;
            flen = (uint16_t)(flen - itLen);
        }
        if (flen < 24) continue;

        uint8_t type = (f[0] >> 2) & 0x03;
        uint8_t sub  = (f[0] >> 4) & 0x0F;

        if (type == 0) {
            if (sub == 8) {                       // beacon
                beacons++;
                if (ssid[0] == '\0')
                    parseIes(f, flen, 36, ssid, sizeof(ssid), &chan);
            } else if (sub == 5) {                // probe response
                proberesp++;
                if (ssid[0] == '\0')
                    parseIes(f, flen, 36, ssid, sizeof(ssid), &chan);
            }
            continue;
        }
        if (type != 2) continue;                  // only data can carry EAPOL

        uint16_t ho = 24;
        if (f[0] & 0x80) ho += 2;                 // QoS data
        if (f[1] & 0x80) ho += 4;                 // HT control
        if ((f[1] & 0x03) == 0x03) ho += 6;       // four-address
        if ((uint32_t)ho + 8 > (uint32_t)flen) continue;
        if (f[ho] != 0xAA || f[ho + 1] != 0xAA || f[ho + 2] != 0x03) continue;
        if (f[ho + 6] != 0x88 || f[ho + 7] != 0x8E) continue;

        const uint8_t* e = f + ho + 8;
        uint16_t elen = (uint16_t)(flen - ho - 8);
        if (elen < 99 || e[1] != 0x03) continue;
        uint8_t msg = eapolMsgOf(e, elen);
        if (msg == 0) continue;

        eapolSeen++;
        mSeen[msg] = true;
        char sa[18], da[18];
        macToStr(f + 10, sa);
        macToStr(f + 4, da);
        if (shownEapol < 6) {
            shownEapol++;
            emit("  M%u %s -> %s", (unsigned)msg, sa, da);
            emit("     replay %02x%02x%02x%02x%02x%02x%02x%02x",
                 e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16]);
        }
        if (msg == 1 && !haveM1) { memcpy(replayM1, e + 9, 8); haveM1 = true; }
        if (msg == 2 && !haveM2) { memcpy(replayM2, e + 9, 8); haveM2 = true; }
    }

    emit("-- summary --");
    emit("records    : %u (%u bad)", (unsigned)frames, (unsigned)badRecords);
    emit("radiotap   : %s", hasRadiotap ? "present" : "none");
    if (chan) emit("channel    : %u", (unsigned)chan);
    if (rssi) emit("rssi       : %d dBm", (int)rssi);
    emit("essid      : \"%s\"", ssid[0] ? ssid : "(none seen)");
    emit("beacons    : %u", (unsigned)beacons);
    emit("probe resp : %u", (unsigned)proberesp);
    emit("eapol      : %u", (unsigned)eapolSeen);
    emit("messages   : M1:%s M2:%s M3:%s M4:%s",
         mSeen[1] ? "y" : "-", mSeen[2] ? "y" : "-",
         mSeen[3] ? "y" : "-", mSeen[4] ? "y" : "-");

    if (frames == 0) {
        emit("probe      : global header only, zero packets");
        emit("result     : empty capture");
        return 5;
    }

    uint8_t score = 20;
    if (ssid[0]) score += 15;
    else emit("probe      : no beacon seen -> essid unknown");
    if (eapolSeen) score += 15;

    bool pairOk = haveM1 && haveM2 && memcmp(replayM1, replayM2, 8) == 0;
    if (pairOk) {
        score += 45;
        emit("pair       : M1+M2 with matching replay");
    } else if (haveM2 && !haveM1) {
        emit("probe      : M2 without M1 -> not crackable");
    } else if (haveM1 && !haveM2) {
        emit("probe      : M1 without M2 -> not crackable");
    } else if (haveM1 && haveM2) {
        emit("probe      : M1+M2 but replay counters differ");
    } else if (eapolSeen == 0) {
        emit("probe      : no EAPOL in this capture");
    }

    if (score > 100) score = 100;
    emit("result     : %s (%u/100)", verdictFor(score), (unsigned)score);
    return score;
}

// ---- file list -------------------------------------------------------------
void InspectorPig::refreshList() {
    entryCount = 0;
    sel = 0;
    scroll = 0;
    if (!Storage::available()) {
        snprintf(statusMsg, sizeof(statusMsg), "NO SD");
        return;
    }
    File dir = SD.open(Storage::DIR_HS);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        snprintf(statusMsg, sizeof(statusMsg), "NO /handshakes");
        return;
    }
    File f = dir.openNextFile();
    while (f && entryCount < MAX_ENTRIES) {
        if (!f.isDirectory()) {
            const char* nm = Storage::baseName(f.name());
            if (nm && nm[0] && (isPcapName(nm) || isHc22000Name(nm))) {
                Entry& e = entries[entryCount];
                memset(&e, 0, sizeof(e));
                strncpy(e.name, nm, sizeof(e.name) - 1);
                e.size = (uint32_t)f.size();
                e.kind = isHc22000Name(nm) ? Kind::HC22000 : Kind::PCAP;
                entryCount++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    if (f) f.close();
    dir.close();
    snprintf(statusMsg, sizeof(statusMsg), "%u CAPTURES", (unsigned)entryCount);
}

// Read the file into the heap only for the duration of the analysis, then let
// it go. Nothing stays in RAM between inspections.
uint8_t InspectorPig::analyzePath(const char* path, Kind kind) {
    if (!Storage::available() || !path) return 0;
    File f = SD.open(path, "r");
    if (!f) {
        emit("error      : cannot open file");
        return 0;
    }
    size_t n = f.size();
    if (n == 0) {
        f.close();
        emit("error      : file is empty");
        return 0;
    }
    if (n > READ_MAX) {
        f.close();
        emit("error      : file larger than %u bytes", (unsigned)READ_MAX);
        return 0;
    }
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) {
        f.close();
        emit("error      : out of memory for %u bytes", (unsigned)n);
        return 0;
    }
    size_t got = f.read(buf, n);
    f.close();
    if (got != n) {
        free(buf);
        emit("error      : short read (%u of %u)", (unsigned)got, (unsigned)n);
        return 0;
    }
    uint8_t score = (kind == Kind::HC22000) ? analyze22000(buf, n)
                                           : analyzePcap(buf, n);
    free(buf);
    return score;
}

static void stemOf(const char* name, char* out, size_t outLen) {
    size_t i = 0;
    for (; name && name[i] && i + 1 < outLen; i++) {
        if (name[i] == '.') break;
        out[i] = name[i];
    }
    out[i] = '\0';
}

// One capture: full report on screen, full copy on the card.
void InspectorPig::inspectSelected() {
    if (!entryCount || sel >= entryCount) return;
    Entry& e = entries[sel];

    reportBegin();
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    char out[96];
    char stem[40];
    stemOf(e.name, stem, sizeof(stem));
    snprintf(out, sizeof(out), "%s/%s.txt", Storage::DIR_INSPECTOR, stem);
    if (s_reportOut) s_reportOut.close();
    s_streamOnly = false;
    s_reportOut = SD.open(out, "w");

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, e.name);
    emit("file       : %s", e.name);
    emit("size       : %u bytes", (unsigned)e.size);
    emit("saved to   : %s", out);

    uint8_t score = analyzePath(path, e.kind);
    e.checked = true;
    e.score = score;
    snprintf(e.verdict, sizeof(e.verdict), "%s", verdictFor(score));
    emit("verdict    : %u/100 %s", (unsigned)score, e.verdict);

    if (s_reportOut) {
        s_reportOut.flush();
        s_reportOut.close();
    }
    snprintf(statusMsg, sizeof(statusMsg), "%s %u/100", e.verdict, (unsigned)score);
    phase = Phase::DETAIL;
    lineScroll = 0;
}

// Every capture. The verbose text goes to /0N3P0rK/inspector/report.txt while
// the screen keeps one line per file plus the totals, so a 48-file card is
// still readable.
void InspectorPig::inspectAll() {
    if (!entryCount) return;

    reportBegin();
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    if (s_reportOut) s_reportOut.close();
    s_reportOut = SD.open("/0N3P0rK/inspector/report.txt", "w");

    s_streamOnly = true;
    emit("scope      : ALL captures in /0N3P0rK/handshakes");
    emit("count      : %u", (unsigned)entryCount);
    s_streamOnly = false;
    emit("ALL %u CAPTURES", (unsigned)entryCount);

    uint8_t good = 0, usable = 0, partial = 0, broken = 0;
    for (uint8_t i = 0; i < entryCount; i++) {
        Entry& e = entries[i];
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, e.name);

        s_streamOnly = true;
        emit("");
        emit("--- %s (%u bytes) ---", e.name, (unsigned)e.size);
        uint8_t score = analyzePath(path, e.kind);
        e.checked = true;
        e.score = score;
        snprintf(e.verdict, sizeof(e.verdict), "%s", verdictFor(score));
        emit("verdict    : %u/100 %s", (unsigned)score, e.verdict);
        s_streamOnly = false;

        char shortName[18];
        strncpy(shortName, e.name, sizeof(shortName) - 1);
        shortName[sizeof(shortName) - 1] = '\0';
        emit("%-17s %3u %s", shortName, (unsigned)score, e.verdict);

        if (score >= 85) good++;
        else if (score >= 60) usable++;
        else if (score >= 30) partial++;
        else broken++;
        yield();
    }

    char tot[48];
    snprintf(tot, sizeof(tot), "TOT ok=%u use=%u part=%u bad=%u",
             (unsigned)good, (unsigned)usable, (unsigned)partial, (unsigned)broken);
    emit("%s", tot);
    if (s_reportOut) {
        s_reportOut.println(tot);
        s_reportOut.flush();
        s_reportOut.close();
    }
    snprintf(statusMsg, sizeof(statusMsg), "GOOD %u/%u", (unsigned)good,
             (unsigned)entryCount);
    phase = Phase::DETAIL;
    lineScroll = 0;
    Display::showToast("SAVED /inspector/report.txt", 1600);
}

// ---- lifecycle -------------------------------------------------------------
void InspectorPig::start() {
    running = true;
    phase = Phase::LIST;
    // The Enter that opened the menu is still held: latch it so it cannot be
    // read as "inspect this file".
    keyLatch = true;
    reportBegin();
    snprintf(statusMsg, sizeof(statusMsg), "SCAN...");
    refreshList();
    Display::showToast("INSPECT", 700);
}

void InspectorPig::stop() {
    if (s_reportOut) {
        s_reportOut.flush();
        s_reportOut.close();
    }
    running = false;
    phase = Phase::LIST;
}

void InspectorPig::update() {
    if (!running) return;
    if (App::windowHidden()) return;
    handleInput();
}

void InspectorPig::getStatusLine(char* buf, size_t n) {
    if (!buf || !n) return;
    if (phase == Phase::DETAIL) {
        snprintf(buf, n, "REPORT %u/%u  A all  ` back",
                 (unsigned)lineScroll, (unsigned)lineCount);
        return;
    }
    snprintf(buf, n, "%s  ENT one  A all", statusMsg);
}

void InspectorPig::handleInput() {
    if (!keyNewPress(keyLatch)) return;

    if (keyEsc()) {
        if (phase == Phase::DETAIL) {
            phase = Phase::LIST;
            return;
        }
        stop();
        return;
    }

    bool all = M5Cardputer.Keyboard.isKeyPressed('a') ||
               M5Cardputer.Keyboard.isKeyPressed('A');

    if (phase == Phase::DETAIL) {
        if (all) {
            inspectAll();
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (lineScroll > 0) lineScroll--;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if ((uint8_t)(lineScroll + VIS_ROWS) < lineCount) lineScroll++;
            return;
        }
        if (M5Cardputer.Keyboard.keysState().enter) phase = Phase::LIST;
        return;
    }

    // ---- list phase ----
    if (all) {
        inspectAll();
        return;
    }
    if (M5Cardputer.Keyboard.keysState().enter) {
        inspectSelected();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('r') ||
        M5Cardputer.Keyboard.isKeyPressed('R')) {
        refreshList();
        Display::showToast("RESCAN", 600);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';') ||
        M5Cardputer.Keyboard.isKeyPressed(',')) {
        if (sel > 0) sel--;
        if (sel < scroll) scroll = sel;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.') ||
        M5Cardputer.Keyboard.isKeyPressed('/')) {
        if (sel + 1 < entryCount) sel++;
        if (sel >= scroll + VIS_ROWS) scroll = (uint8_t)(sel - VIS_ROWS + 1);
        return;
    }
}

// ---- drawing ---------------------------------------------------------------
void InspectorPig::drawList(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

    canvas.fillRect(4, 2, 232, 13, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("INSPECTORPIG", 8, 5);
    canvas.setTextColor(UiStyle::DIM);
    canvas.setTextDatum(top_right);
    canvas.drawString(statusMsg, 232, 5);
    canvas.setTextDatum(top_left);

    if (entryCount == 0) {
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString("NO CAPTURES IN", 8, 30);
        canvas.drawString("/0N3P0rK/handshakes", 8, 42);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("` back    R rescan", 8, 62);
        return;
    }

    canvas.setTextColor(UiStyle::CYAN);
    canvas.drawString("CAPTURE          TYPE   SIZE  VERDICT", 6, 20);

    int y = 31;
    for (uint8_t i = scroll; i < entryCount && i < scroll + VIS_ROWS; i++) {
        const Entry& e = entries[i];
        bool s = (i == sel);
        uiListRow(canvas, y, 14, s, UiStyle::PINK);
        canvas.setTextColor(s ? UiStyle::BG : UiStyle::TEXT);

        char nm[19];
        strncpy(nm, e.name, sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = '\0';
        char sz[8];
        if (e.size < 1024) snprintf(sz, sizeof(sz), "%ub", (unsigned)e.size);
        else snprintf(sz, sizeof(sz), "%uk", (unsigned)(e.size / 1024));

        char row[48];
        snprintf(row, sizeof(row), "%-16s %-4s %5s  %s", nm,
                 e.kind == Kind::HC22000 ? "22K" : "PCAP", sz,
                 e.checked ? e.verdict : "...");
        canvas.drawString(row, 6, y + 3);
        y += 14;
    }

    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("ENT inspect   A all   R rescan", 6, 100);
}

void InspectorPig::drawDetail(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

    canvas.fillRect(4, 2, 232, 13, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("REPORT", 8, 5);
    canvas.setTextColor(UiStyle::DIM);
    canvas.setTextDatum(top_right);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%u/%u", (unsigned)(lineScroll + 1),
             (unsigned)lineCount);
    canvas.drawString(hdr, 232, 5);
    canvas.setTextDatum(top_left);

    if (lineCount == 0) {
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString("EMPTY REPORT", 8, 40);
        return;
    }

    int y = 20;
    for (uint8_t k = lineScroll; k < lineCount && k < lineScroll + 7; k++) {
        canvas.setTextColor(lines[k][0] == '-' ? UiStyle::CYAN : UiStyle::TEXT);
        canvas.drawString(lines[k], 6, y);
        y += 12;
    }
}

void InspectorPig::draw(M5Canvas& canvas) {
    if (phase == Phase::DETAIL) drawDetail(canvas);
    else drawList(canvas);
}





// ---- report sinks ----------------------------------------------------------
void InspectorPig::reportBegin() {
    lineCount = 0;
    lineScroll = 0;
    for (uint8_t i = 0; i < MAX_LINES; i++) lines[i][0] = '\0';
}

void InspectorPig::emit(const char* fmt, ...) {
    if (!fmt) return;
    // The SD report keeps whole lines; the screen buffer clips them to a
    // readable width. s_streamOnly is used by the ALL run, which sends the
    // verbose text to the file and only one summary line per capture to the UI.
    char tmp[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (s_reportOut) {
        s_reportOut.println(tmp);
        yield();
    }
    if (!s_streamOnly && lineCount < MAX_LINES) {
        strncpy(lines[lineCount], tmp, LINE_LEN - 1);
        lines[lineCount][LINE_LEN - 1] = '\0';
        lineCount++;
    }
}

void InspectorPig::reportFlush(const char* path) {
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    File f = SD.open(path, "w");
    if (!f) return;
    f.printf("# 0N3P0rK InspectorPig report\n");
    for (uint8_t i = 0; i < lineCount; i++) f.println(lines[i]);
    f.close();
}
