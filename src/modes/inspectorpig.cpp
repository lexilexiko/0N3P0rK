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
// Verdict index: one "name|score|verdict" line per checked capture, appended.
static const char* const VERDICT_FILE = "/0N3P0rK/inspector/state.txt";

bool InspectorPig::running = false;
InspectorPig::Phase InspectorPig::phase = InspectorPig::Phase::LIST;
InspectorPig::Tab InspectorPig::tab = InspectorPig::Tab::PCAP;
uint16_t InspectorPig::page = 0;
bool InspectorPig::hasMore = false;
uint16_t InspectorPig::totalItems = 0;
InspectorPig::Entry* InspectorPig::entries = nullptr;
uint8_t InspectorPig::entryCount = 0;
uint8_t InspectorPig::sel = 0;
uint8_t InspectorPig::scroll = 0;
bool InspectorPig::keyLatch = false;
char InspectorPig::statusMsg[40] = "";
char (*InspectorPig::lines)[InspectorPig::LINE_LEN] = nullptr;
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

static bool replayIncremented(const uint8_t* base, const uint8_t* candidate) {
    if (!base || !candidate) return false;
    uint8_t expected[8];
    memcpy(expected, base, sizeof(expected));
    for (int i = 7; i >= 0; i--) {
        if (++expected[i] != 0) break;
    }
    return memcmp(expected, candidate, sizeof(expected)) == 0;
}

static bool skipBytes(File& f, uint32_t n) {
    if (n == 0) return true;
    uint8_t dump[64];
    while (n) {
        size_t chunk = n < sizeof(dump) ? n : sizeof(dump);
        size_t got = f.read(dump, chunk);
        if (got != chunk) return false;
        n -= (uint32_t)got;
    }
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
        if (id == 0 && ssid && ssidLen > 1 && ssid[0] == '\0') {
            if (ln == 0) {
                strncpy(ssid, "(hidden)", ssidLen - 1);
                ssid[ssidLen - 1] = '\0';
            } else {
                size_t k = (ln < ssidLen - 1) ? ln : ssidLen - 1;
                memcpy(ssid, f + off + 2, k);
                ssid[k] = '\0';
            }
        } else if (id == 3 && chan && ln == 1 && *chan == 0) {
            *chan = f[off + 2];
        }
        off = (uint16_t)(off + 2 + ln);
    }
}

// ---------------------------------------------------------------------------
// Classic PCAP. Walks the record stream the way Wireshark does: container
// header, then {packet header, radiotap, 802.11 frame} for every packet.
// Packets are read from SD one at a time so a large capture never needs a
// 128 KB heap buffer (that was marking good files BROKEN).
// ---------------------------------------------------------------------------
uint8_t InspectorPig::analyzePcapFile(File& f, size_t len) {
    emit("source     : pcap capture");

    if (len < 24) {
        emit("error      : smaller than a pcap global header");
        emit("result     : broken container");
        return 0;
    }
    uint8_t gh[24];
    if (f.read(gh, 24) != 24) {
        emit("error      : short global header");
        return 0;
    }
    uint32_t magic = ((uint32_t)gh[0] << 24) | ((uint32_t)gh[1] << 16) |
                     ((uint32_t)gh[2] << 8) | (uint32_t)gh[3];
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
         (unsigned)rd16(gh + 4), (unsigned)rd16(gh + 6));
    emit("snaplen    : %u", (unsigned)rd32(gh + 16));
    uint32_t linktype = rd32(gh + 20);
    emit("linktype   : %u %s", (unsigned)linktype,
         linktype == LINK_RADIOTAP ? "(radiotap+802.11)"
                                   : (linktype == LINK_80211 ? "(raw 802.11)"
                                                             : "(unexpected!)"));

    uint16_t frames = 0, badRecords = 0, beacons = 0, proberesp = 0;
    uint16_t eapolSeen = 0, shownEapol = 0;
    uint8_t  chan = 0;
    int8_t   rssi = 0;
    bool     hasRadiotap = false;
    char     ssid[33] = "";
    bool     mSeen[5] = {false, false, false, false, false};
    // Keep a few replay counters so a retry M1 does not poison the first M2.
    uint8_t  replayM1[4][8] = {{0}};
    uint8_t  replayM2[4][8] = {{0}};
    uint8_t  replayM3[4][8] = {{0}};
    uint8_t  nM1 = 0, nM2 = 0, nM3 = 0;

    emit("-- packet records --");
    uint8_t pkt[InspectorPig::PKT_CAP];
    while (true) {
        uint8_t ph[16];
        int gotHdr = f.read(ph, 16);
        if (gotHdr == 0) break;
        if (gotHdr != 16) {
            emit("  record %u truncated header", (unsigned)frames);
            badRecords++;
            break;
        }
        uint32_t incl = rd32(ph + 8);
        if (incl == 0) {
            emit("  record %u zero length", (unsigned)frames);
            badRecords++;
            break;
        }
        uint32_t take = incl < InspectorPig::PKT_CAP ? incl : InspectorPig::PKT_CAP;
        size_t got = f.read(pkt, take);
        if (got != take) {
            emit("  record %u truncated (%u bytes claimed)",
                 (unsigned)frames, (unsigned)incl);
            badRecords++;
            break;
        }
        if (incl > take && !skipBytes(f, incl - take)) {
            badRecords++;
            break;
        }
        uint16_t flen = (uint16_t)take;
        frames++;
        if ((frames & 31u) == 0) yield();

        const uint8_t* fr = pkt;
        if (linktype == LINK_RADIOTAP) {
            if (flen < 8) continue;
            uint16_t itLen = (uint16_t)(pkt[2] | (pkt[3] << 8));
            if (itLen < 8 || itLen > 80 || itLen > flen) continue;
            hasRadiotap = true;
            uint32_t present = (uint32_t)pkt[4] | ((uint32_t)pkt[5] << 8) |
                               ((uint32_t)pkt[6] << 16) | ((uint32_t)pkt[7] << 24);
            uint16_t rp = 8;
            if (present & (1u << 31)) {
                // another present word; skip so channel/rssi parse stays honest
                rp = (uint16_t)(rp + 4);
            }
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
            fr = pkt + itLen;
            flen = (uint16_t)(flen - itLen);
        }
        if (flen < 24) continue;

        uint8_t type = (fr[0] >> 2) & 0x03;
        uint8_t sub  = (fr[0] >> 4) & 0x0F;

        if (type == 0) {
            if (sub == 8) {
                beacons++;
                if (ssid[0] == '\0')
                    parseIes(fr, flen, 36, ssid, sizeof(ssid), &chan);
            } else if (sub == 5) {
                proberesp++;
                if (ssid[0] == '\0')
                    parseIes(fr, flen, 36, ssid, sizeof(ssid), &chan);
            }
            continue;
        }
        if (type != 2) continue;

        uint16_t ho = 24;
        if (fr[0] & 0x80) ho += 2;
        if (fr[1] & 0x80) ho += 4;
        if ((fr[1] & 0x03) == 0x03) ho += 6;
        if ((uint32_t)ho + 8 > (uint32_t)flen) continue;
        if (fr[ho] != 0xAA || fr[ho + 1] != 0xAA || fr[ho + 2] != 0x03) continue;
        if (fr[ho + 6] != 0x88 || fr[ho + 7] != 0x8E) continue;

        const uint8_t* e = fr + ho + 8;
        uint16_t elen = (uint16_t)(flen - ho - 8);
        if (elen < 99 || e[1] != 0x03) continue;
        uint8_t msg = eapolMsgOf(e, elen);
        if (msg == 0) continue;

        eapolSeen++;
        mSeen[msg] = true;
        char sa[18], da[18];
        macToStr(fr + 10, sa);
        macToStr(fr + 4, da);
        if (shownEapol < 6) {
            shownEapol++;
            emit("  M%u %s -> %s", (unsigned)msg, sa, da);
            emit("     replay %02x%02x%02x%02x%02x%02x%02x%02x",
                 e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16]);
        }
        if (msg == 1 && nM1 < 4) { memcpy(replayM1[nM1], e + 9, 8); nM1++; }
        if (msg == 2 && nM2 < 4) { memcpy(replayM2[nM2], e + 9, 8); nM2++; }
        if (msg == 3 && nM3 < 4) { memcpy(replayM3[nM3], e + 9, 8); nM3++; }
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

    bool pair12 = false;
    for (uint8_t i = 0; i < nM1 && !pair12; i++)
        for (uint8_t j = 0; j < nM2; j++)
            if (memcmp(replayM1[i], replayM2[j], 8) == 0) { pair12 = true; break; }
    bool pair23 = false;
    for (uint8_t i = 0; i < nM2 && !pair23; i++)
        for (uint8_t j = 0; j < nM3; j++)
            if (replayIncremented(replayM2[i], replayM3[j])) { pair23 = true; break; }

    if (pair12) {
        score += 45;
        emit("pair       : M1+M2 with matching replay");
    } else if (pair23) {
        score += 45;
        emit("pair       : M2+M3 (M3 replay = M2+1)");
    } else if (nM2 && !nM1 && !nM3) {
        emit("probe      : M2 without M1/M3 -> not crackable");
    } else if (nM1 && !nM2) {
        emit("probe      : M1 without M2 -> not crackable");
    } else if (nM1 && nM2) {
        emit("probe      : M1+M2 but replay counters differ");
    } else if (eapolSeen == 0) {
        emit("probe      : no EAPOL in this capture");
    }

    if (score > 100) score = 100;
    emit("result     : %s (%u/100)", verdictFor(score), (unsigned)score);
    return score;
}

// Which names belong to the active tab.
static bool tabMatches(bool hcTab, const char* n) {
    return hcTab ? isHc22000Name(n) : isPcapName(n);
}

// Rebuilds the page in `entries`: counts the matches for the active tab, skips
// the pages before `page`, fills up to PAGE_SIZE rows and notes whether more
// remain. Verdicts already stored on the card are applied on the way out, so a
// capture checked earlier shows its answer immediately instead of being redone.
void InspectorPig::refreshList() {
    entryCount = 0;
    sel = 0;
    scroll = 0;
    hasMore = false;
    totalItems = 0;
    if (!entries) {
        snprintf(statusMsg, sizeof(statusMsg), "NO MEM");
        return;
    }
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

    const bool hcTab = (tab == Tab::HC22000);
    const uint16_t toSkip = (uint16_t)(page * PAGE_SIZE);
    uint16_t seen = 0;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char* nm = Storage::baseName(f.name());
            if (nm && nm[0] && tabMatches(hcTab, nm)) {
                if (seen >= toSkip && entryCount < PAGE_SIZE) {
                    Entry& e = entries[entryCount];
                    memset(&e, 0, sizeof(e));
                    strncpy(e.name, nm, sizeof(e.name) - 1);
                    e.size = (uint32_t)f.size();
                    e.kind = isHc22000Name(nm) ? Kind::HC22000 : Kind::PCAP;
                    entryCount++;
                } else if (entryCount >= PAGE_SIZE) {
                    hasMore = true;   // at least one match past this page
                    break;
                }
                seen++;
                totalItems++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    if (f) f.close();
    dir.close();

    loadVerdicts();

    if (totalItems == 0) {
        snprintf(statusMsg, sizeof(statusMsg), hcTab ? "NO .22000" : "NO PCAP");
    } else {
        snprintf(statusMsg, sizeof(statusMsg), "%u P%u", (unsigned)totalItems,
                 (unsigned)(page + 1));
    }
}

void InspectorPig::gotoPage(uint16_t p) {
    page = p;
    refreshList();
    if (entryCount == 0 && page > 0) {
        // Ran past the end (files were removed): step back one page.
        page--;
        refreshList();
    }
}

void InspectorPig::switchTab(int8_t dir) {
    (void)dir;   // two tabs: either direction toggles
    tab = (tab == Tab::PCAP) ? Tab::HC22000 : Tab::PCAP;
    page = 0;
    refreshList();
    Display::showToast(tab == Tab::HC22000 ? "TAB .22000" : "TAB PCAP", 700);
}

// Applies the stored verdicts to the current page. Lines are read in order and
// later lines win, so re-checking a capture simply updates the answer.
void InspectorPig::loadVerdicts() {
    if (!entries || entryCount == 0) return;
    if (!Storage::fileExists(VERDICT_FILE)) return;
    File f = SD.open(VERDICT_FILE, "r");
    if (!f) return;
    char line[96];
    while (f.available()) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (n == 0) continue;
        char* bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        int sc = atoi(bar + 1);
        if (sc < 0) sc = 0;
        if (sc > 100) sc = 100;
        for (uint8_t i = 0; i < entryCount; i++) {
            if (strcmp(entries[i].name, line) == 0) {
                entries[i].checked = true;
                entries[i].score = (uint8_t)sc;
                snprintf(entries[i].verdict, sizeof(entries[i].verdict), "%s",
                         verdictFor((uint8_t)sc));
                break;
            }
        }
        yield();
    }
    f.close();
}

// Append "name|score|verdict". Append-only keeps this cheap — no read/modify/
// write of the whole index per check — and the newest line for a name wins.
void InspectorPig::saveVerdict(const Entry& e) {
    if (!e.name[0]) return;
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    if (Storage::fileSize(VERDICT_FILE) > VERDICT_MAX) SD.remove(VERDICT_FILE);
    File f = SD.open(VERDICT_FILE, "a");
    if (!f) return;
    f.printf("%s|%u|%s\n", e.name, (unsigned)e.score, e.verdict);
    f.flush();
    f.close();
}

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
    uint8_t score = 0;
    if (kind == Kind::HC22000) {
        size_t want = n < READ_MAX_22000 ? n : READ_MAX_22000;
        uint8_t* buf = (uint8_t*)malloc(want);
        if (!buf) {
            f.close();
            emit("error      : out of memory for %u bytes", (unsigned)want);
            return 0;
        }
        size_t got = f.read(buf, want);
        f.close();
        if (got == 0) {
            free(buf);
            emit("error      : short read");
            return 0;
        }
        score = analyze22000(buf, got);
        free(buf);
        return score;
    }
    score = analyzePcapFile(f, n);
    f.close();
    return score;
}

static void stemOf(const char* name, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!name) return;
    const char* dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n + 1 > outLen) n = outLen - 1;
    memcpy(out, name, n);
    out[n] = '\0';
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
    const bool reportOk = (bool)s_reportOut;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, e.name);
    emit("file       : %s", e.name);
    emit("size       : %u bytes", (unsigned)e.size);
    emit("saved to   : %s", reportOk ? out : "(open failed!)");

    uint8_t score = analyzePath(path, e.kind);
    e.checked = true;
    e.score = score;
    snprintf(e.verdict, sizeof(e.verdict), "%s", verdictFor(score));
    emit("verdict    : %u/100 %s", (unsigned)score, e.verdict);

    if (s_reportOut) {
        s_reportOut.flush();
        s_reportOut.close();
    }
    // Remember the verdict on the card: it is what the list shows on the next
    // visit, so the same capture never has to be analysed twice.
    saveVerdict(e);

    snprintf(statusMsg, sizeof(statusMsg), "%s %u/100", e.verdict, (unsigned)score);
    phase = Phase::DETAIL;
    lineScroll = 0;
    Display::showToast(reportOk ? "SAVED /inspector/" : "REPORT SAVE FAIL",
                       reportOk ? 1200 : 1800);
}

// Every capture on the active tab — the whole folder, not just the page on
// screen. Verbose text goes to /0N3P0rK/inspector/report.txt, one summary line
// per capture goes to the screen, and every verdict is written to the index so a
// later visit shows the answers instead of running the analysis again.
void InspectorPig::inspectAll() {
    if (!entries) return;
    if (!Storage::available()) {
        Display::showToast("NO SD", 1200);
        return;
    }

    // Names first, analysis second: the directory handle is closed before any
    // capture is opened, and this buffer is released before returning.
    const uint16_t kMaxList = 128;
    char (*names)[Storage::FILE_NAME_MAX] =
        (char (*)[Storage::FILE_NAME_MAX])malloc((size_t)kMaxList * Storage::FILE_NAME_MAX);
    if (!names) {
        Display::showToast("LOW MEM", 1400);
        return;
    }
    const uint16_t listed = Storage::listHandshakes(names, kMaxList);

    reportBegin();
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    if (s_reportOut) s_reportOut.close();
    s_reportOut = SD.open("/0N3P0rK/inspector/report.txt", "w");
    const bool reportOk = (bool)s_reportOut;

    const bool hcTab = (tab == Tab::HC22000);
    s_streamOnly = true;
    emit("scope      : ALL %s captures in /0N3P0rK/handshakes",
         hcTab ? ".22000" : "PCAP");
    s_streamOnly = false;
    emit("ALL %s", hcTab ? ".22000" : "PCAP");

    uint8_t good = 0, usable = 0, partial = 0, broken = 0;
    uint16_t done = 0;
    for (uint16_t i = 0; i < listed; i++) {
        const char* nm = names[i];
        if (!nm[0] || !tabMatches(hcTab, nm)) continue;

        char path[96];
        snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, nm);

        s_streamOnly = true;
        emit("");
        emit("--- %s ---", nm);
        uint8_t score = analyzePath(path, isHc22000Name(nm) ? Kind::HC22000
                                                            : Kind::PCAP);
        s_streamOnly = false;

        char shortName[18];
        strncpy(shortName, nm, sizeof(shortName) - 1);
        shortName[sizeof(shortName) - 1] = '\0';
        emit("%-16s %3u %s", shortName, (unsigned)score, verdictFor(score));

        // Store the answer, and refresh the row when it is on screen.
        Entry tmp;
        memset(&tmp, 0, sizeof(tmp));
        strncpy(tmp.name, nm, sizeof(tmp.name) - 1);
        tmp.score = score;
        snprintf(tmp.verdict, sizeof(tmp.verdict), "%s", verdictFor(score));
        saveVerdict(tmp);
        for (uint8_t r = 0; r < entryCount; r++) {
            if (strcmp(entries[r].name, nm) == 0) {
                entries[r].checked = true;
                entries[r].score = score;
                snprintf(entries[r].verdict, sizeof(entries[r].verdict), "%s",
                         verdictFor(score));
                break;
            }
        }

        done++;
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
    s_streamOnly = false;
    free(names);

    snprintf(statusMsg, sizeof(statusMsg), "OK %u/%u", (unsigned)good, (unsigned)done);
    phase = Phase::DETAIL;
    lineScroll = 0;
    Display::showToast(reportOk ? "SAVED /inspector/" : "REPORT SAVE FAIL",
                       reportOk ? 1500 : 1900);
}

// ---- lifecycle -------------------------------------------------------------
void InspectorPig::start() {
    phase = Phase::LIST;
    // The Enter that opened the menu is still held: latch it so it cannot be
    // read as "inspect this file".
    keyLatch = true;

    // The page is PAGE_SIZE * sizeof(Entry) and the report screen is
    // MAX_LINES * LINE_LEN. Both are only useful while the view is open, so
    // they are taken from the heap for the visit instead of sitting in .bss
    // for the whole run of the firmware.
    if (!entries) entries = (Entry*)malloc(sizeof(Entry) * PAGE_SIZE);
    if (!lines)   lines   = (char (*)[LINE_LEN])malloc((size_t)MAX_LINES * LINE_LEN);
    if (!entries || !lines) {
        free(entries);
        entries = nullptr;
        free(lines);
        lines = nullptr;
        running = false;
        Display::showToast("LOW MEM", 1500);
        return;
    }

    running = true;
    page = 0;
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

    // Hand the view buffers back: nothing stays resident between visits.
    free(entries);
    entries = nullptr;
    free(lines);
    lines = nullptr;
    entryCount = 0;
    lineCount = 0;
    lineScroll = 0;
}

void InspectorPig::update() {
    if (!running) return;
    if (App::windowHidden()) return;
    handleInput();
}

void InspectorPig::getStatusLine(char* buf, size_t n) {
    if (!buf || !n) return;
    bool help = (millis() % 7500u) >= 5000u;
    if (phase == Phase::DETAIL) {
        if (help) snprintf(buf, n, "^/v SCROLL  ESC BACK");
        else snprintf(buf, n, "REPORT %u/%u", (unsigned)lineScroll, (unsigned)lineCount);
        return;
    }
    if (!help) snprintf(buf, n, "%s", statusMsg);
    else if (totalItems == 0)
        snprintf(buf, n, "R RESCAN  </> TAB  ESC BACK");
    else if (totalItems > PAGE_SIZE)
        snprintf(buf, n, "ENT CHECK  A ALL  [ ] PAGE  </> TAB");
    else
        snprintf(buf, n, "ENT CHECK  A ALL  </> TAB  ESC BACK");
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
        page = 0;
        refreshList();
        Display::showToast("RESCAN", 600);
        return;
    }
    // Tabs first: `,` and `/` switch them, exactly like LOOT and PigPass do.
    if (M5Cardputer.Keyboard.isKeyPressed(',') ||
        M5Cardputer.Keyboard.isKeyPressed('/')) {
        switchTab(M5Cardputer.Keyboard.isKeyPressed(',') ? -1 : +1);
        return;
    }
    // Pages, so a folder with more captures than one page holds stays reachable.
    if (M5Cardputer.Keyboard.isKeyPressed('[')) {
        if (page > 0) gotoPage((uint16_t)(page - 1));
        else Display::showToast("FIRST PAGE", 600);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(']')) {
        if (hasMore) gotoPage((uint16_t)(page + 1));
        else Display::showToast("LAST PAGE", 600);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (sel > 0) sel--;
        if (sel < scroll) scroll = sel;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
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

    // Two tabs, laid out like the LOOT menu's tab bar (2 x 112 px).
    const bool hc = (tab == Tab::HC22000);
    canvas.fillRect(4, 2, 112, 13, hc ? UiStyle::PANEL : UiStyle::PINK);
    canvas.fillRect(124, 2, 112, 13, hc ? UiStyle::PINK : UiStyle::PANEL);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(hc ? UiStyle::TEXT : UiStyle::BG);
    canvas.drawString("PCAP", 60, 5);
    canvas.setTextColor(hc ? UiStyle::BG : UiStyle::TEXT);
    canvas.drawString(".22000", 180, 5);
    canvas.setTextDatum(top_left);

    // The shared bottom bar displays status and rotating control hints.
    if (totalItems) {
        char pg[20];
        if (totalItems > PAGE_SIZE) {
            uint16_t pages = (uint16_t)((totalItems + PAGE_SIZE - 1) / PAGE_SIZE);
            snprintf(pg, sizeof(pg), "%u P%u/%u", (unsigned)totalItems,
                     (unsigned)(page + 1), (unsigned)pages);
        } else {
            snprintf(pg, sizeof(pg), "%u FILES", (unsigned)totalItems);
        }
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(top_right);
        canvas.drawString(pg, 234, 18);
        canvas.setTextDatum(top_left);
    }

    if (entryCount == 0) {
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(statusMsg, 8, 44);
        return;
    }

    canvas.setTextColor(UiStyle::CYAN);
    canvas.drawString("CAPTURE          TYPE   SIZE  VERDICT", 6, 30);

    int y = 40;
    for (uint8_t i = scroll; i < entryCount && i < scroll + VIS_ROWS; i++) {
        const Entry& e = entries[i];
        bool s = (i == sel);
        uiListRow(canvas, y, 12, s, UiStyle::PINK);
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
        canvas.drawString(row, 6, y + 2);
        y += 12;
    }
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

    if (!lines || lineCount == 0) {
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
    if (!lines) return;                 // headless run: nothing to clear
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
    if (!s_streamOnly && lines && lineCount < MAX_LINES) {
        strncpy(lines[lineCount], tmp, LINE_LEN - 1);
        lines[lineCount][LINE_LEN - 1] = '\0';
        lineCount++;
    }
}

void InspectorPig::reportFlush(const char* path) {
    if (!lines) return;                 // nothing buffered outside a visit
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    File f = SD.open(path, "w");
    if (!f) return;
    f.printf("# 0N3P0rK InspectorPig report\n");
    for (uint8_t i = 0; i < lineCount; i++) f.println(lines[i]);
    f.close();
}

// ---------------------------------------------------------------------------
// LOOT hooks: same dissection as a normal visit, but the report on SD is the
// only output - no list, no report screen, no mode switch. LOOT calls these
// straight from its file list, so the capture already in front of the user can
// be vetted before it goes anywhere. Both work fine while the INSPECT view is
// closed, which is the normal case: the view buffers are null and emit() only
// writes to the report file.
// ---------------------------------------------------------------------------
uint8_t InspectorPig::checkOne(const char* filename, char* msg, size_t msgLen) {
    if (msg && msgLen) msg[0] = '\0';
    if (!filename || !filename[0]) {
        if (msg && msgLen) snprintf(msg, msgLen, "NO FILE");
        return 0;
    }
    if (!Storage::available()) {
        if (msg && msgLen) snprintf(msg, msgLen, "NO SD");
        return 0;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, filename);
    char stem[40];
    stemOf(filename, stem, sizeof(stem));
    char report[96];
    snprintf(report, sizeof(report), "%s/%s.txt", Storage::DIR_INSPECTOR, stem);

    reportBegin();
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    if (s_reportOut) s_reportOut.close();
    s_streamOnly = true;                 // file only: no screen lines exist here
    s_reportOut = SD.open(report, "w");

    emit("file       : %s", filename);
    emit("size       : %u bytes", (unsigned)Storage::fileSize(path));
    emit("saved to   : %s", report);
    uint8_t score = analyzePath(path, isHc22000Name(filename) ? Kind::HC22000
                                                             : Kind::PCAP);
    emit("verdict    : %u/100 %s", (unsigned)score, verdictFor(score));
    if (s_reportOut) {
        s_reportOut.flush();
        s_reportOut.close();
    }
    s_streamOnly = false;

    // Persist it too, so the INSPECT list can show the answer without a re-run.
    {
        Entry tmp;
        memset(&tmp, 0, sizeof(tmp));
        strncpy(tmp.name, filename, sizeof(tmp.name) - 1);
        tmp.score = score;
        snprintf(tmp.verdict, sizeof(tmp.verdict), "%s", verdictFor(score));
        saveVerdict(tmp);
    }

    if (msg && msgLen)
        snprintf(msg, msgLen, "%s %u/100 SAVED", verdictFor(score), (unsigned)score);
    return score;
}

uint16_t InspectorPig::checkAll(char* msg, size_t msgLen) {
    if (msg && msgLen) msg[0] = '\0';
    if (!Storage::available()) {
        if (msg && msgLen) snprintf(msg, msgLen, "NO SD");
        return 0;
    }

    // Names first, analysis second: the SD directory handle is closed before
    // any capture is opened, and the name list is a short-lived heap buffer
    // that does not outlive the call.
    const uint16_t kMaxList = 96;
    char (*names)[Storage::FILE_NAME_MAX] =
        (char (*)[Storage::FILE_NAME_MAX])malloc((size_t)kMaxList * Storage::FILE_NAME_MAX);
    if (!names) {
        if (msg && msgLen) snprintf(msg, msgLen, "LOW MEM");
        return 0;
    }
    const uint16_t listed = Storage::listHandshakes(names, kMaxList);

    reportBegin();
    Storage::ensureDir(Storage::DIR_INSPECTOR);
    if (s_reportOut) s_reportOut.close();
    char reportPath[64];
    snprintf(reportPath, sizeof(reportPath), "%s/report.txt", Storage::DIR_INSPECTOR);
    s_streamOnly = true;
    s_reportOut = SD.open(reportPath, "w");

    emit("scope      : ALL captures in /0N3P0rK/handshakes");
    emit("run by     : LOOT checker");

    uint16_t files = 0;
    uint8_t good = 0, usable = 0, partial = 0, broken = 0;
    for (uint16_t i = 0; i < listed; i++) {
        const char* name = names[i];
        if (!name[0]) continue;
        if (!isPcapName(name) && !isHc22000Name(name)) continue;

        char path[96];
        snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, name);
        emit("");
        emit("--- %s (%u bytes) ---", name, (unsigned)Storage::fileSize(path));
        uint8_t score = analyzePath(path, isHc22000Name(name) ? Kind::HC22000
                                                             : Kind::PCAP);
        emit("verdict    : %u/100 %s", (unsigned)score, verdictFor(score));
        // Keep the index up to date, so opening INSPECT later shows this answer.
        {
            Entry tmp;
            memset(&tmp, 0, sizeof(tmp));
            strncpy(tmp.name, name, sizeof(tmp.name) - 1);
            tmp.score = score;
            snprintf(tmp.verdict, sizeof(tmp.verdict), "%s", verdictFor(score));
            saveVerdict(tmp);
        }
        files++;
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
    s_streamOnly = false;
    free(names);

    if (msg && msgLen)
        snprintf(msg, msgLen, "%u FILES  OK %u", (unsigned)files, (unsigned)good);
    return files;
}
