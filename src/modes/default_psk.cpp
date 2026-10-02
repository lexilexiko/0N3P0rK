// modes/default_psk.cpp
#include "default_psk.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

// One handshake is in play at a time, and the derived list is a few hundred
// keys tested over seconds — so a bounded table that is taken for the run and
// given back afterwards is the whole design. See build()/release().
namespace {

// Only the candidate strings are stored. The rule label is kept for the very
// first entry only, since that is the one shown on the stats row.
//
// The table lives on the heap, not in .bss: PigPass spends most of its life
// idle or browsing files, and there is no reason for this feature to sit on
// ~10 KB the whole time. build() takes it, release() gives it back.
struct Cand {
    // Qualified: the constants live in namespace DefaultPsk, but this struct is
    // in an anonymous namespace outside it.
    char pw[DefaultPsk::MAX_PW];
};

Cand* g_cands = nullptr;
size_t g_n = 0;
const char* g_topRule = "NONE";

inline char up(char c) { return (char)toupper((unsigned char)c); }
inline char lo(char c) { return (char)tolower((unsigned char)c); }

void setStr(char* out, size_t outSz, const char* s) {
    if (!outSz) return;
    strncpy(out, s, outSz - 1);
    out[outSz - 1] = '\0';
}

// Push one candidate, applying the WPA length rules. Duplicates are dropped:
// the same string often falls out of two rules ("Home" as SSID and as vendor
// word), and a repeat costs a full PBKDF2 round for zero information.
void push(const char* pw, const char* rule) {
    // No table means build() failed to get memory. Rules are still walked by
    // the caller, so this is a hard stop, not a skip.
    if (!g_cands) return;
    if (g_n >= DefaultPsk::MAX_CANDIDATES) return;
    if (!pw) return;
    size_t len = strlen(pw);
    if (len < DefaultPsk::MIN_PW_LEN) return;
    // Must fit the slot with its terminator. Rejecting is correct here — a
    // silently truncated key is simply a wrong key, and it would cost a full
    // PBKDF2 round to learn nothing.
    if (len >= DefaultPsk::MAX_PW) return;
    for (size_t i = 0; i < g_n; i++) {
        if (strcmp(g_cands[i].pw, pw) == 0) return;
    }
    setStr(g_cands[g_n].pw, DefaultPsk::MAX_PW, pw);
    if (g_n == 0) g_topRule = rule;
    g_n++;
}

bool isHex12(const char* s) {
    if (!s) return false;
    for (size_t i = 0; i < 12; i++) {
        if (!s[i]) return false;
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[12] == '\0';
}

// ---- rule 1: the SSID itself, in the casings people actually type ----------
void ruleSsid(const char* ssid) {
    if (!ssid[0]) return;
    char raw[33], lower[33], upper[33], cap[33];
    setStr(raw, sizeof(raw), ssid);
    for (size_t i = 0; raw[i]; i++) {
        lower[i] = lo(raw[i]);
        upper[i] = up(raw[i]);
    }
    lower[strlen(raw)] = upper[strlen(raw)] = '\0';
    bool start = true;
    for (size_t i = 0; raw[i]; i++) {
        cap[i] = start ? up(raw[i]) : lo(raw[i]);
        start = false;
    }
    cap[strlen(raw)] = '\0';

    push(raw,  "SSID AS-IS");
    push(lower, "SSID LOWER");
    push(upper, "SSID UPPER");
    push(cap,   "SSID CAPS");
}

// ---- rule 2: SSID + digits, the most common factory pattern ---------------
// 0-9, then 00-99, then 000-999. Covers "wifi2023" and "Home_0012" without a
// dictionary. Ordered shortest-first so the cheapest hits come first.
void ruleSsidNum(const char* ssid) {
    if (!ssid[0]) return;
    char buf[48];
    for (int pass = 0; pass < 3; pass++) {
        int width = pass + 1;
        uint32_t max = 1;
        for (int i = 0; i < width; i++) max *= 10;
        for (uint32_t v = 0; v < max; v++) {
            snprintf(buf, sizeof(buf), "%s%0*u", ssid, width, (unsigned)v);
            push(buf, "SSID + NUM");
        }
    }
}

// ---- rule 3: SSID + year, the default of the last decade -------------------
void ruleSsidYear(const char* ssid) {
    if (!ssid[0]) return;
    static const char* years[] = {"2020", "2021", "2022", "2023", "2024",
                                  "2025", "2026", "2019", "2018", "2017"};
    char buf[48];
    for (size_t i = 0; i < sizeof(years) / sizeof(years[0]); i++) {
        snprintf(buf, sizeof(buf), "%s%s", ssid, years[i]);
        push(buf, "SSID + YEAR");
    }
}

// ---- rule 4: BSSID slices, the keys vendors print on the sticker ----------
// The whole MAC, both casings, and the tail digits. Units that ship the MAC as
// the key are common enough to be worth six candidates.
void ruleBssid(const char* bssid) {
    if (!isHex12(bssid)) return;
    char upM[16], lowM[16], mac[24], tail4[8], tail6[8];
    for (size_t i = 0; i < 12; i++) {
        upM[i]  = up(bssid[i]);
        lowM[i] = lo(bssid[i]);
    }
    upM[12] = lowM[12] = '\0';
    setStr(tail4, sizeof(tail4), bssid + 8);
    setStr(tail6, sizeof(tail6), bssid + 6);
    snprintf(mac, sizeof(mac), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
             bssid, bssid + 2, bssid + 4, bssid + 6, bssid + 8, bssid + 10);

    push(lowM,  "BSSID MAC");
    push(upM,   "BSSID MAC UP");
    push(tail6, "BSSID TAIL6");
    push(tail4, "BSSID TAIL4");
    push(mac,   "BSSID COLONS");
    // Dotted form, as on some label stickers: 488f.5a62.3215
    char dot[24];
    snprintf(dot, sizeof(dot), "%.4s.%.4s.%.4s", bssid, bssid + 4, bssid + 8);
    push(dot,  "BSSID DOTS");
}

// ---- rule 5: vendor defaults, matched on the SSID -------------------------
// A .22000 does not carry the OUI, so the SSID is the only vendor hint we
// have — and on home gear it usually still contains the vendor name.
void ruleVendor(const char* ssid) {
    if (!ssid[0]) return;
    char key[33];
    setStr(key, sizeof(key), ssid);
    for (size_t i = 0; key[i]; i++) key[i] = lo(key[i]);

    static const char* kVendorWords[] = {
        "admin", "password", "default", "12345678", "1234567890",
        "1234", "123456", "test", "root", "pass", "guest", "0000",
        "1111", "666666", "888888", "abc123", "654321",
    };
    constexpr size_t NW = sizeof(kVendorWords) / sizeof(kVendorWords[0]);

    // Vendors whose units are known to ship a fixed key regardless of SSID.
    static const char* kSignatures[] = {
        "tp-link", "tplink", "zyxel", "dlink", "netgear", "linksys",
        "asus", "rt-", "huawei", "zte", "dra", "realtek", "technicolor",
        "sagem", "eir", "vodafone", "telkomsel",
    };
    bool known = false;
    for (size_t i = 0; i < sizeof(kSignatures) / sizeof(kSignatures[0]); i++) {
        if (strstr(key, kSignatures[i])) { known = true; break; }
    }
    if (known) {
        for (size_t i = 0; i < NW; i++) push(kVendorWords[i], "VENDOR KEY");
    }

    // Keys embedded in the SSID itself: "WIFI_12345678", "NETGEAR1234".
    const char* digits = NULL;
    for (size_t i = 0; ssid[i]; i++) {
        if (isdigit((unsigned char)ssid[i])) { digits = ssid + i; break; }
    }
    if (digits) {
        size_t n = 0;
        while (isdigit((unsigned char)digits[n])) n++;
        char tail[16];
        if (n > 0 && n < sizeof(tail)) {
            memcpy(tail, digits, n);
            tail[n] = '\0';
            push(tail, "SSID DIGIT RUN");
            char upTail[16];
            for (size_t i = 0; i < n; i++) upTail[i] = up(tail[i]);
            upTail[n] = '\0';
            push(upTail, "SSID DIGIT RUN UP");
        }
    }
}

// ---- rule 6: bare numeric keys -------------------------------------------
// Sticker keys with no relation to the SSID. Kept last: it is the broadest and
// the least likely, so it must not push the targeted rules past the cap.
void ruleNumeric() {
    char buf[16];
    for (uint32_t v = 0; v < 1000; v++) {
        snprintf(buf, sizeof(buf), "%08u", (unsigned)v);
        push(buf, "NUMERIC 8");
    }
}

}  // namespace

namespace DefaultPsk {

size_t build(const Target& t) {
    // Defensive: a second build without a release would leak the first table.
    if (g_cands) free(g_cands);
    g_cands = nullptr;
    g_n = 0;
    g_topRule = "NONE";

    if (!t.ssid[0]) return 0;   // nothing to derive from; do not even allocate

    // Taken up front so a low-heap device fails before the rules run, and so
    // push() can assume the table exists.
    g_cands = (Cand*)malloc(sizeof(Cand) * DefaultPsk::MAX_CANDIDATES);
    if (!g_cands) {
        Serial.println("[DEFPSK] ERROR: no heap for candidate table");
        return 0;
    }

    ruleSsid(t.ssid);
    ruleSsidYear(t.ssid);
    ruleVendor(t.ssid);
    ruleBssid(t.bssid);
    ruleSsidNum(t.ssid);
    ruleNumeric();          // last: broadest, least likely

    // A short SSID (or one with no digits and no BSSID) can legitimately yield
    // nothing after the length filter. Do not leave the table allocated for a
    // run that will never start — the caller treats 0 as "do not run".
    if (g_n == 0) {
        free(g_cands);
        g_cands = nullptr;
        Serial.println("[DEFPSK] no candidates from this network");
        return 0;
    }

    Serial.printf("[DEFPSK] %u candidates (top: %s) ssid=\"%s\" bssid=%s\n",
                  (unsigned)g_n, g_topRule, t.ssid, t.bssid[0] ? t.bssid : "-");
    return g_n;
}

size_t count() { return g_n; }

bool emit(size_t index, char* out, size_t outSz) {
    if (!g_cands || index >= g_n || !out || outSz < 2) return false;
    setStr(out, outSz, g_cands[index].pw);
    return true;
}

const char* topRuleName() { return g_topRule; }

bool release() {
    if (!g_cands) return false;   // never allocated (or already released)
    free(g_cands);
    g_cands = nullptr;
    g_n = 0;
    g_topRule = "NONE";
    Serial.printf("[DEFPSK] table released, free=%u\n", (unsigned)ESP.getFreeHeap());
    return true;
}

}  // namespace DefaultPsk