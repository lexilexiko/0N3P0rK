// PigPass Mode - WiFi Password Cracker (Brute Force WPA2)
// Cracks WPA2 PSK using PCAP handshake + wordlist
//
// Speed path (from Bruce wifi_recover technique, single-core on Cardputer):
//  1) SOFTWARE SHA1 — no mbedtls HW mutex overhead on small blocks
//  2) Precomputed HMAC pads + 20-byte HMAC fast path (PBKDF2 U2..U4096)
//  3) Runs on main loop only — no FreeRTOS worker (heap on Cardputer is tight;
//     xTaskCreate 8–24KB stack often fails → old "TASK FAIL" toast)
//  4) 240 MHz for the crack session

#include "pigpass.h"
#include "pigpass_crypto.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "../ui/display.h"
#include "../ui/keys.h"
#include "../piglet/avatar.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../core/xp.h"

// Static member initialization
PigpassState PigpassMode::state = PigpassState::IDLE;
uint64_t PigpassMode::attempts = 0;
double PigpassMode::rate = 0.0;
uint32_t PigpassMode::elapsedSeconds = 0;
bool PigpassMode::foundPassword = false;
bool PigpassMode::fromCache = false;
char PigpassMode::foundPw[64] = {0};
char PigpassMode::ssid[33] = {0};
uint32_t PigpassMode::startTime = 0;
uint32_t PigpassMode::lastUpdateTime = 0;
char PigpassMode::handshakePath[72] = {0};
char PigpassMode::wordlistPath[72] = {0};
std::vector<PigpassFileEntry> PigpassMode::files;
PigpassHsTab PigpassMode::hsTab = PigpassHsTab::PCAP;
uint16_t PigpassMode::selectedIndex = 0;
uint16_t PigpassMode::scrollOffset = 0;
bool PigpassMode::keyWasPressed = false;
bool PigpassMode::uiDirty = true;
bool PigpassMode::maskMode = false;
uint8_t PigpassMode::maskCharsetId = 0;
uint8_t PigpassMode::maskLen = 8;
uint64_t PigpassMode::maskIndex = 0;
char PigpassMode::maskCharset[72] = {0};
uint8_t PigpassMode::maskCharsetLen = 0;

// Virtual list entry path for mask generator
static const char* kMaskVirtualPath = "@mask";

// Handshake / PMKID material extracted from .pcap or hashcat .22000
struct HandshakeData {
    bool valid;
    bool isPmkid;           // true = crack via PMKID, false = EAPOL MIC
    char ssid[33];
    uint8_t ap_mac[6];
    uint8_t sta_mac[6];
    uint8_t anonce[32];
    uint8_t snonce[32];
    uint8_t eapol[512];     // Full EAPOL frame used for MIC (MIC zeroed at verify)
    uint16_t eapol_len;
    uint8_t mic[16];
    uint8_t pmkid[16];
    uint8_t key_version;    // 1=HMAC-MD5 (WPA), 2=HMAC-SHA1 (WPA2), 3=AES-CMAC
    bool has_m1;
    bool has_m2;
    bool has_m3;
};

static HandshakeData handshake;
static File wordlistFile;

// Buffered wordlist reader (avoids per-byte SD reads)
static constexpr size_t WL_BUF_SIZE = 4096;
static uint8_t* wlBuf = nullptr;
static size_t wlBufPos = 0;
static size_t wlBufLen = 0;
static bool wlEof = false;

// Precomputed per-handshake material (fixed for whole wordlist run)
static uint8_t g_ptkData[76];          // min/max MACs || min/max nonces
static constexpr size_t EAPOL_ZERO_SIZE = 512;
static uint8_t* g_eapolZero = nullptr; // EAPOL with MIC field zeroed
static uint16_t g_eapolLen = 0;
static bool g_hsCryptoReady = false;

static bool allocateRuntimeBuffers() {
    if (wlBuf && g_eapolZero) return true;
    if (!wlBuf) wlBuf = static_cast<uint8_t*>(malloc(WL_BUF_SIZE));
    if (!g_eapolZero) g_eapolZero = static_cast<uint8_t*>(malloc(EAPOL_ZERO_SIZE));
    if (wlBuf && g_eapolZero) return true;
    free(wlBuf);
    free(g_eapolZero);
    wlBuf = nullptr;
    g_eapolZero = nullptr;
    return false;
}

static void freeRuntimeBuffers() {
    free(wlBuf);
    free(g_eapolZero);
    wlBuf = nullptr;
    g_eapolZero = nullptr;
}

// Crack session state (single-core on main loop — no FreeRTOS worker).
// Bruce dual-core task often fails to allocate on Cardputer (tight heap) → TASK FAIL.
// Software SHA1 PBKDF2 alone is the big win; dual-core is optional later.
static volatile bool s_foundFlag = false;
static volatile bool s_wordlistDone = false;
static volatile uint64_t s_attempts = 0;
static char s_foundLocal[64] = {0};
static uint32_t s_savedCpuMhz = 0;
static bool s_sessionStarted = false;
static bool s_crackPause = false;
static char s_currentTry[64] = {0};  // live password under test (UI)

// Resume-from-checkpoint (same handshake + wordlist, survives reboot)
static bool s_resumePending = false;
static uint32_t s_resumeOffset = 0;
static uint64_t s_resumeAttempts = 0;
static uint32_t s_resumeElapsed = 0;
static uint32_t s_lastCheckpointMs = 0;
static constexpr uint32_t kCheckpointIntervalMs = 5000;

// EAPOL-Key field offsets (from start of EAPOL frame: ver|type|len|desc|...)
static constexpr uint16_t EAPOL_KEYINFO_OFF = 5;
static constexpr uint16_t EAPOL_NONCE_OFF = 17;
static constexpr uint16_t EAPOL_MIC_OFF = 81;
static constexpr uint16_t EAPOL_MIN_KEY_LEN = 99;

namespace {

static bool endsWithIgnoreCase(const char* value, const char* suffix) {
    if (!value || !suffix) return false;
    size_t valueLen = strlen(value);
    size_t suffixLen = strlen(suffix);
    if (suffixLen == 0 || valueLen < suffixLen) return false;
    const char* tail = value + valueLen - suffixLen;
    for (size_t i = 0; i < suffixLen; i++) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool dirHasExtFiles(const char* dirPath, const char* const* exts, size_t extCount) {
    if (!dirPath || !SD.exists(dirPath)) return false;

    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return false;
    }

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            const char* rawName = entry.name();
            const char* slash = strrchr(rawName, '/');
            const char* name = slash ? slash + 1 : rawName;
            for (size_t i = 0; i < extCount; i++) {
                if (endsWithIgnoreCase(name, exts[i])) {
                    entry.close();
                    dir.close();
                    return true;
                }
            }
        }
        entry.close();
        entry = dir.openNextFile();
        yield();
    }

    dir.close();
    return false;
}

static const char* resolveHandshakeDir() {
    const char* preferredDir = SDLayout::handshakesDir();
    if (!SD.exists(preferredDir)) SD.mkdir(preferredDir);
    return preferredDir;
}

static void copyBasename(const char* rawName, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!rawName) return;

    const char* slash = strrchr(rawName, '/');
    const char* name = slash ? slash + 1 : rawName;
    strncpy(out, name, outLen - 1);
    out[outLen - 1] = '\0';
}

static void truncateName(char* name, size_t maxChars) {
    if (!name) return;
    size_t len = strlen(name);
    if (len <= maxChars) return;
    if (maxChars < 3) {
        name[maxChars] = '\0';
        return;
    }
    name[maxChars - 2] = '.';
    name[maxChars - 1] = '.';
    name[maxChars] = '\0';
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    if (!hex || !out) return false;
    for (size_t i = 0; i < outLen; i++) {
        int hi = hexNibble(hex[i * 2]);
        int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Precompute PTK data + zero-MIC EAPOL once per handshake
static void prepareHandshakeCrypto() {
    g_hsCryptoReady = false;
    g_eapolLen = 0;
    if (!handshake.valid) return;

    if (memcmp(handshake.ap_mac, handshake.sta_mac, 6) < 0) {
        memcpy(g_ptkData, handshake.ap_mac, 6);
        memcpy(g_ptkData + 6, handshake.sta_mac, 6);
    } else {
        memcpy(g_ptkData, handshake.sta_mac, 6);
        memcpy(g_ptkData + 6, handshake.ap_mac, 6);
    }
    if (memcmp(handshake.anonce, handshake.snonce, 32) < 0) {
        memcpy(g_ptkData + 12, handshake.anonce, 32);
        memcpy(g_ptkData + 44, handshake.snonce, 32);
    } else {
        memcpy(g_ptkData + 12, handshake.snonce, 32);
        memcpy(g_ptkData + 44, handshake.anonce, 32);
    }

    if (!handshake.isPmkid && handshake.eapol_len >= EAPOL_MIN_KEY_LEN &&
        handshake.eapol_len <= EAPOL_ZERO_SIZE && g_eapolZero) {
        memcpy(g_eapolZero, handshake.eapol, handshake.eapol_len);
        memset(g_eapolZero + EAPOL_MIC_OFF, 0, 16);
        g_eapolLen = handshake.eapol_len;
    }
    // PMKID needs only SSID + MACs (always ready if valid)
    g_hsCryptoReady = true;
}

static void freePbkdfCtx() {
    // no-op (software crypto is stack-local)
}
static void extractSsidFromBeacon(const uint8_t* frame, uint16_t frameLen) {
    if (!frame || frameLen < 36) return;
    if (handshake.ssid[0] != '\0') return;

    uint16_t fc = frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;
    uint8_t subtype = (fc >> 4) & 0x0F;
    // Management beacon (8) or probe response (5)
    if (type != 0 || (subtype != 8 && subtype != 5)) return;

    // Fixed params start after 24-byte mgmt header
    uint16_t ieOff = 24 + 12;
    if (ieOff >= frameLen) return;

    while (ieOff + 2 <= frameLen) {
        uint8_t id = frame[ieOff];
        uint8_t len = frame[ieOff + 1];
        if (ieOff + 2 + len > frameLen) break;
        if (id == 0 && len > 0 && len <= 32) {
            memcpy(handshake.ssid, frame + ieOff + 2, len);
            handshake.ssid[len] = '\0';
            return;
        }
        ieOff = (uint16_t)(ieOff + 2 + len);
    }
}

static uint16_t findLlcSnapOffset(const uint8_t* frame, uint16_t frameLen) {
    if (!frame || frameLen < 32) return 0;

    uint16_t fc = frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;
    if (type != 2) return 0;  // Data only

    bool toDs = (fc & 0x0100) != 0;
    bool fromDs = (fc & 0x0200) != 0;
    bool order = (fc & 0x8000) != 0;
    bool qos = ((fc >> 4) & 0x0F) >= 8;  // QoS Data subtypes

    uint16_t hdrLen = 24;
    if (toDs && fromDs) hdrLen = 30;
    if (qos) hdrLen += 2;
    if (qos && order) hdrLen += 4;

    if (hdrLen + 8 > frameLen) return 0;

    // LLC/SNAP for EAPOL: AA AA 03 00 00 00 88 8E
    const uint8_t* p = frame + hdrLen;
    if (p[0] == 0xAA && p[1] == 0xAA && p[2] == 0x03 &&
        p[6] == 0x88 && p[7] == 0x8E) {
        return (uint16_t)(hdrLen + 8);
    }
    return 0;
}

static void getDataAddrs(const uint8_t* frame, uint8_t* addr1, uint8_t* addr2, uint8_t* addr3) {
    memcpy(addr1, frame + 4, 6);
    memcpy(addr2, frame + 10, 6);
    memcpy(addr3, frame + 16, 6);
}

static void ingestEapol(const uint8_t* eapol, uint16_t eapolLen,
                        const uint8_t* srcMac, const uint8_t* dstMac) {
    if (!eapol || eapolLen < EAPOL_MIN_KEY_LEN) return;
    if (eapol[1] != 3) return;  // EAPOL-Key only

    uint16_t keyInfo = ((uint16_t)eapol[EAPOL_KEYINFO_OFF] << 8) | eapol[EAPOL_KEYINFO_OFF + 1];
    uint8_t keyVer = keyInfo & 0x07;
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;

    uint8_t msg = 0;
    if (keyAck && !keyMic) msg = 1;
    else if (!keyAck && keyMic && !secure) msg = 2;
    else if (keyAck && keyMic && install) msg = 3;
    else if (!keyAck && keyMic && secure) msg = 4;
    if (msg == 0) return;

    if (keyVer >= 1 && keyVer <= 3) {
        handshake.key_version = keyVer;
    }

    uint8_t ap[6], sta[6];
    if (msg == 1 || msg == 3) {
        memcpy(ap, srcMac, 6);
        memcpy(sta, dstMac, 6);
    } else {
        memcpy(ap, dstMac, 6);
        memcpy(sta, srcMac, 6);
    }

    // Keep first-seen AP/STA pair
    bool macsEmpty = true;
    for (int i = 0; i < 6; i++) {
        if (handshake.ap_mac[i] || handshake.sta_mac[i]) { macsEmpty = false; break; }
    }
    if (macsEmpty) {
        memcpy(handshake.ap_mac, ap, 6);
        memcpy(handshake.sta_mac, sta, 6);
    }

    if (msg == 1) {
        // ANonce from M1 (only if not already set from M1/M3)
        if (!handshake.has_m1 && !handshake.has_m3) {
            memcpy(handshake.anonce, eapol + EAPOL_NONCE_OFF, 32);
        }
        handshake.has_m1 = true;
    } else if (msg == 2) {
        // SNonce + MIC + full EAPOL from M2 (authoritative for cracking)
        if (!handshake.has_m2) {
            memcpy(handshake.snonce, eapol + EAPOL_NONCE_OFF, 32);
            memcpy(handshake.mic, eapol + EAPOL_MIC_OFF, 16);
            if (eapolLen <= sizeof(handshake.eapol)) {
                memcpy(handshake.eapol, eapol, eapolLen);
                handshake.eapol_len = eapolLen;
            }
            handshake.has_m2 = true;
        }
    } else if (msg == 3) {
        // ANonce from M3 if M1 missing
        if (!handshake.has_m1 && !handshake.has_m3) {
            memcpy(handshake.anonce, eapol + EAPOL_NONCE_OFF, 32);
        }
        handshake.has_m3 = true;
    }
}

static void processDot11Frame(const uint8_t* frame, uint16_t frameLen) {
    if (!frame || frameLen < 24) return;

    extractSsidFromBeacon(frame, frameLen);

    uint16_t eapolOff = findLlcSnapOffset(frame, frameLen);
    if (eapolOff == 0 || eapolOff >= frameLen) return;

    uint8_t addr1[6], addr2[6], addr3[6];
    getDataAddrs(frame, addr1, addr2, addr3);

    uint16_t fc = frame[0] | ((uint16_t)frame[1] << 8);
    bool toDs = (fc & 0x0100) != 0;
    bool fromDs = (fc & 0x0200) != 0;

    // src/dst for EAPOL direction (mirrors Oink processEAPOL)
    uint8_t srcMac[6], dstMac[6];
    if (fromDs && !toDs) {
        // AP -> STA: Addr2=BSSID, Addr1=DA(STA)
        memcpy(dstMac, addr1, 6);
        memcpy(srcMac, addr2, 6);
    } else if (toDs && !fromDs) {
        // STA -> AP: Addr2=SA(STA), Addr1=BSSID
        memcpy(dstMac, addr1, 6);
        memcpy(srcMac, addr2, 6);
    } else {
        memcpy(dstMac, addr1, 6);
        memcpy(srcMac, addr2, 6);
        (void)addr3;
    }

    uint16_t eapolLen = (uint16_t)(frameLen - eapolOff);
    ingestEapol(frame + eapolOff, eapolLen, srcMac, dstMac);
}

static bool finalizeHandshake() {
    if (handshake.isPmkid) {
        if (handshake.ssid[0] == '\0') {
            Serial.println("[PIGPASS] ERROR: PMKID missing SSID");
            return false;
        }
        handshake.valid = true;
        return true;
    }

    // Need M2 (MIC + SNonce) and ANonce from M1 or M3
    bool hasAnonce = false;
    for (int i = 0; i < 32; i++) {
        if (handshake.anonce[i]) { hasAnonce = true; break; }
    }
    if (!handshake.has_m2 || handshake.eapol_len < EAPOL_MIN_KEY_LEN) {
        Serial.println("[PIGPASS] ERROR: No usable EAPOL M2 in capture");
        return false;
    }
    if (!hasAnonce) {
        Serial.println("[PIGPASS] ERROR: Missing ANonce (need M1 or M3)");
        return false;
    }
    if (handshake.ssid[0] == '\0') {
        Serial.println("[PIGPASS] WARN: SSID empty — will still try MIC (SSID required for PMK)");
        // SSID is required for PBKDF2; fail hard
        Serial.println("[PIGPASS] ERROR: SSID required for cracking");
        return false;
    }
    if (handshake.key_version == 0) {
        handshake.key_version = 2;  // default WPA2 HMAC-SHA1
    }

    handshake.valid = true;
    Serial.printf("[PIGPASS] Loaded HS SSID='%s' ver=%u M1=%d M2=%d M3=%d eapol=%u\n",
                  handshake.ssid, handshake.key_version,
                  handshake.has_m1 ? 1 : 0, handshake.has_m2 ? 1 : 0,
                  handshake.has_m3 ? 1 : 0, handshake.eapol_len);
    return true;
}

static bool parse22000Line(const char* line) {
    // WPA*01*PMKID*MAC_AP*MAC_STA*ESSID***...
    // WPA*02*MIC*MAC_AP*MAC_STA*ESSID*ANONCE*EAPOL*MESSAGEPAIR
    if (!line || strncmp(line, "WPA*", 4) != 0) return false;

    // Split into mutable tokens (line is temporary buffer owned by caller)
    char buf[1600];
    size_t lineLen = strlen(line);
    if (lineLen == 0 || lineLen >= sizeof(buf)) return false;
    memcpy(buf, line, lineLen + 1);

    // Strip trailing CR/LF/space
    while (lineLen > 0 &&
           (buf[lineLen - 1] == '\n' || buf[lineLen - 1] == '\r' ||
            buf[lineLen - 1] == ' ' || buf[lineLen - 1] == '\t')) {
        buf[--lineLen] = '\0';
    }

    char* fields[12];
    int n = 0;
    char* cursor = buf;
    fields[n++] = cursor;
    for (char* p = buf; *p && n < 12; p++) {
        if (*p == '*') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    // fields: [0]=WPA [1]=01|02 [2]=mic/pmkid [3]=ap [4]=sta [5]=essid
    //         [6]=anonce (hs) [7]=eapol (hs) [8]=msgpair
    if (n < 6) return false;
    if (strcmp(fields[0], "WPA") != 0) return false;

    const char* type = fields[1];
    const char* micOrPmkidHex = fields[2];
    const char* macApHex = fields[3];
    const char* macStaHex = fields[4];
    const char* essidHex = fields[5];

    if (strlen(micOrPmkidHex) != 32 || strlen(macApHex) != 12 || strlen(macStaHex) != 12) {
        return false;
    }

    bool isPmkid = (strcmp(type, "01") == 0);
    bool isHs = (strcmp(type, "02") == 0);
    if (!isPmkid && !isHs) return false;

    if (!hexToBytes(micOrPmkidHex, isPmkid ? handshake.pmkid : handshake.mic, 16)) return false;
    if (!hexToBytes(macApHex, handshake.ap_mac, 6)) return false;
    if (!hexToBytes(macStaHex, handshake.sta_mac, 6)) return false;

    size_t essidHexLen = strlen(essidHex);
    if (essidHexLen == 0 || (essidHexLen % 2) != 0 || essidHexLen > 64) return false;
    size_t ssidLen = essidHexLen / 2;
    if (!hexToBytes(essidHex, (uint8_t*)handshake.ssid, ssidLen)) return false;
    handshake.ssid[ssidLen] = '\0';

    if (isPmkid) {
        handshake.isPmkid = true;
        handshake.key_version = 2;
        return finalizeHandshake();
    }

    if (n < 8) return false;
    const char* anonceHex = fields[6];
    const char* eapolHex = fields[7];

    if (strlen(anonceHex) != 64) return false;
    if (!hexToBytes(anonceHex, handshake.anonce, 32)) return false;

    size_t eapolHexLen = strlen(eapolHex);
    if (eapolHexLen < (size_t)EAPOL_MIN_KEY_LEN * 2 || (eapolHexLen % 2) != 0) return false;
    size_t eapolLen = eapolHexLen / 2;
    if (eapolLen > sizeof(handshake.eapol)) eapolLen = sizeof(handshake.eapol);
    if (!hexToBytes(eapolHex, handshake.eapol, eapolLen)) return false;
    handshake.eapol_len = (uint16_t)eapolLen;

    // Prefer length from EAPOL header if sane (ver|type|lenBE)
    if (eapolLen >= 4) {
        uint16_t bodyLen = ((uint16_t)handshake.eapol[2] << 8) | handshake.eapol[3];
        uint16_t total = (uint16_t)(bodyLen + 4);
        if (total >= EAPOL_MIN_KEY_LEN && total <= eapolLen) {
            handshake.eapol_len = total;
        }
    }

    // Hashcat 22000 stores EAPOL with MIC zeroed; put captured MIC back for verify path
    memcpy(handshake.eapol + EAPOL_MIC_OFF, handshake.mic, 16);
    memcpy(handshake.snonce, handshake.eapol + EAPOL_NONCE_OFF, 32);

    uint16_t keyInfo = ((uint16_t)handshake.eapol[EAPOL_KEYINFO_OFF] << 8) |
                       handshake.eapol[EAPOL_KEYINFO_OFF + 1];
    uint8_t keyVer = keyInfo & 0x07;
    handshake.key_version = (keyVer >= 1 && keyVer <= 3) ? keyVer : 2;
    handshake.has_m2 = true;
    handshake.has_m1 = true;

    return finalizeHandshake();
}

static bool parse22000File(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[PIGPASS] ERROR: Cannot open 22000: %s\n", path);
        return false;
    }

    char line[1600];
    size_t pos = 0;
    bool ok = false;
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                if (parse22000Line(line)) {
                    ok = true;
                    break;
                }
                pos = 0;
            }
            continue;
        }
        if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        } else {
            // line too long — discard until newline
            pos = sizeof(line) - 1;
        }
        yield();
    }
    // last line without trailing newline
    if (!ok && pos > 0) {
        line[pos] = '\0';
        ok = parse22000Line(line);
    }

    f.close();
    return ok;
}

static uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t readLe16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool skipBytes(File& f, uint32_t n) {
    uint8_t dump[64];
    while (n > 0) {
        size_t chunk = n > sizeof(dump) ? sizeof(dump) : n;
        size_t got = f.read(dump, chunk);
        if (got != chunk) return false;
        n -= (uint32_t)got;
        yield();
    }
    return true;
}

static bool parsePcapFile(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[PIGPASS] ERROR: Cannot open PCAP: %s\n", path);
        return false;
    }

    uint8_t ghdr[24];
    if (f.read(ghdr, 24) != 24) {
        f.close();
        Serial.println("[PIGPASS] ERROR: Bad PCAP header");
        return false;
    }

    uint32_t magic = readLe32(ghdr);
    bool swap = false;
    bool nano = false;
    if (magic == 0xA1B2C3D4u) {
        swap = false;
        nano = false;
    } else if (magic == 0xD4C3B2A1u) {
        swap = true;
        nano = false;
    } else if (magic == 0xA1B23C4Du) {
        swap = false;
        nano = true;
    } else if (magic == 0x4D3CB2A1u) {
        swap = true;
        nano = true;
    } else {
        f.close();
        Serial.printf("[PIGPASS] ERROR: Unknown PCAP magic %08lx\n", (unsigned long)magic);
        return false;
    }
    (void)nano;

    auto u32 = [&](const uint8_t* p) -> uint32_t {
        uint32_t v = readLe32(p);
        if (swap) {
            return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                   ((v & 0xFF0000u) >> 8) | ((v >> 24) & 0xFFu);
        }
        return v;
    };
    auto u16 = [&](const uint8_t* p) -> uint16_t {
        uint16_t v = readLe16(p);
        if (swap) return (uint16_t)((v << 8) | (v >> 8));
        return v;
    };

    uint32_t linktype = u32(ghdr + 20);
    // 105 = IEEE802_11, 127 = IEEE802_11_RADIOTAP, 1 = Ethernet (rare for our captures)
    Serial.printf("[PIGPASS] PCAP linktype=%lu\n", (unsigned long)linktype);

    uint8_t pktBuf[1600];
    int pktCount = 0;

    while (f.available() >= 16) {
        uint8_t phdr[16];
        if (f.read(phdr, 16) != 16) break;

        uint32_t inclLen = u32(phdr + 8);
        if (inclLen == 0) break;

        if (inclLen > sizeof(pktBuf)) {
            if (!skipBytes(f, inclLen)) break;
            continue;
        }

        if (f.read(pktBuf, inclLen) != (int)inclLen) break;
        pktCount++;

        const uint8_t* frame = pktBuf;
        uint16_t frameLen = (uint16_t)inclLen;

        if (linktype == 127) {
            // Radiotap: length at offset 2 (always little-endian per radiotap spec)
            if (inclLen < 8) continue;
            uint16_t rtLen = readLe16(pktBuf + 2);
            if (rtLen < 8 || rtLen > inclLen) continue;
            frame = pktBuf + rtLen;
            frameLen = (uint16_t)(inclLen - rtLen);
            processDot11Frame(frame, frameLen);
        } else if (linktype == 105) {
            processDot11Frame(frame, frameLen);
        } else if (linktype == 1) {
            // Ethernet: dst(6)+src(6)+ethertype(2) then payload; 0x888E = EAPOL
            if (inclLen >= 14 + EAPOL_MIN_KEY_LEN &&
                pktBuf[12] == 0x88 && pktBuf[13] == 0x8E) {
                ingestEapol(pktBuf + 14, (uint16_t)(inclLen - 14),
                            pktBuf + 6, pktBuf);
            }
        }

        if ((pktCount & 0x0F) == 0) yield();
    }

    f.close();
    Serial.printf("[PIGPASS] PCAP packets scanned: %d\n", pktCount);
    return finalizeHandshake();
}

// Parse handshake capture (.pcap / .cap / .22000)
static bool parsePcapHandshake(const char* path) {
    memset(&handshake, 0, sizeof(handshake));
    if (!path || path[0] == '\0') return false;

    if (endsWithIgnoreCase(path, ".22000")) {
        Serial.printf("[PIGPASS] Parsing hashcat 22000: %s\n", path);
        return parse22000File(path);
    }

    Serial.printf("[PIGPASS] Parsing PCAP: %s\n", path);
    return parsePcapFile(path);
}

// Try one password with Bruce-style software PBKDF2
static __attribute__((optimize("O3"), hot)) bool tryPassword(const char* password, uint8_t passLen) {
    if (!handshake.valid || !password || !g_hsCryptoReady) return false;
    if (passLen < 8 || passLen > 63) return false;

    size_t ssidLen = strlen(handshake.ssid);
    if (ssidLen == 0 || ssidLen > 32) return false;

    PigpassHmacPre pwPre;
    pigpass_hmac_precompute((const uint8_t*)password, passLen, pwPre);

    uint8_t pmk[32];
    pigpass_pbkdf2_pmk(pwPre, (const uint8_t*)handshake.ssid, ssidLen, pmk);

    if (handshake.isPmkid) {
        return pigpass_verify_pmkid(pmk, handshake.ap_mac, handshake.sta_mac, handshake.pmkid);
    }

    uint8_t ptk[64];
    if (!pigpass_derive_ptk(pmk, g_ptkData, ptk)) return false;
    if (g_eapolLen < EAPOL_MIN_KEY_LEN) return false;

    if (handshake.key_version == 1) {
        return pigpass_verify_mic_wpa(ptk, g_eapolZero, g_eapolLen, handshake.mic);
    }
    if (handshake.key_version == 3) {
        // AES-CMAC not implemented
        return false;
    }
    // WPA2 default
    return pigpass_verify_mic_wpa2(ptk, g_eapolZero, g_eapolLen, handshake.mic);
}

static bool tryPassword(const char* password) {
    if (!password) return false;
    size_t n = strlen(password);
    if (n > 63) return false;
    return tryPassword(password, (uint8_t)n);
}

// Forward decls
static void resetWordlistBuffer();
static bool readNextPassword(char* password, size_t passwordSize);

static void stopCrackSession() {
    s_crackPause = false;
    s_sessionStarted = false;
    s_wordlistDone = false;
    if (s_savedCpuMhz) {
        setCpuFrequencyMhz(s_savedCpuMhz);
        s_savedCpuMhz = 0;
    }
}

static void startCrackSession(bool keepAttempts) {
    s_foundFlag = false;
    s_wordlistDone = false;
    s_foundLocal[0] = '\0';
    if (!keepAttempts) s_attempts = 0;
    s_crackPause = false;
    s_sessionStarted = true;
    if (!s_savedCpuMhz) {
        s_savedCpuMhz = getCpuFrequencyMhz();
        if (s_savedCpuMhz < 240) setCpuFrequencyMhz(240);
    }
    Serial.printf("[PIGPASS] crack session cpu=%u free=%u keepAtt=%d\n",
                  (unsigned)getCpuFrequencyMhz(),
                  (unsigned)ESP.getFreeHeap(),
                  (int)keepAttempts);
}

// Try one password on main loop (no FreeRTOS task)
static bool tryNextPassword(const char* password) {
    if (!password) return false;
    size_t n = strlen(password);
    if (n < 8 || n > 63) return false;

    // Always surface what is being tested (UI + status bar)
    strncpy(s_currentTry, password, sizeof(s_currentTry) - 1);
    s_currentTry[sizeof(s_currentTry) - 1] = '\0';

    if (tryPassword(password, (uint8_t)n)) {
        memcpy(s_foundLocal, password, n);
        s_foundLocal[n] = '\0';
        s_foundFlag = true;
        Serial.printf("[PIGPASS] MATCH: %s\n", s_foundLocal);
    }
    s_attempts++;
    return s_foundFlag;
}
static void formatApBssid(char* out, size_t outLen) {
    if (!out || outLen < 13) return;
    snprintf(out, outLen, "%02X%02X%02X%02X%02X%02X",
             handshake.ap_mac[0], handshake.ap_mac[1], handshake.ap_mac[2],
             handshake.ap_mac[3], handshake.ap_mac[4], handshake.ap_mac[5]);
}

static void resetWordlistBuffer() {
    wlBufPos = 0;
    wlBufLen = 0;
    wlEof = false;
}

static bool refillWordlistBuffer() {
    if (!wordlistFile || wlEof) return false;
    if (!wlBuf) return false;
    size_t got = wordlistFile.read(wlBuf, WL_BUF_SIZE);
    wlBufPos = 0;
    wlBufLen = got;
    if (got == 0) {
        wlEof = true;
        return false;
    }
    return true;
}

// Read next password line from buffered wordlist. Returns false on EOF.
static bool readNextPassword(char* password, size_t passwordSize) {
    if (!password || passwordSize == 0) return false;
    password[0] = '\0';

    size_t pos = 0;
    bool gotAny = false;

    while (true) {
        if (wlBufPos >= wlBufLen) {
            if (!refillWordlistBuffer()) {
                // EOF: return last partial line if any
                if (gotAny && pos > 0) {
                    password[pos] = '\0';
                    return true;
                }
                return false;
            }
        }

        while (wlBufPos < wlBufLen) {
            char c = (char)wlBuf[wlBufPos++];
            if (c == '\n') {
                password[pos] = '\0';
                if (pos > 0) return true;
                // empty line — keep reading
                pos = 0;
                gotAny = false;
                continue;
            }
            if (c == '\r') continue;
            gotAny = true;
            if (pos + 1 < passwordSize) {
                password[pos++] = c;
            }
            // if longer than buffer, still consume until newline but drop extras
        }
    }
}

static const char* resolvePassworldDir() {
    const char* preferred = SDLayout::passworldDir();
    if (!SD.exists(preferred)) SD.mkdir(preferred);
    return preferred;
}

} // namespace

void PigpassMode::clearFileList() {
    files.clear();
    files.shrink_to_fit();
    selectedIndex = 0;
    scrollOffset = 0;
}

void PigpassMode::scanDirForExt(const char* dirPath, const char* const* exts, size_t extCount) {
    if (!dirPath || !SD.exists(dirPath) || files.size() >= MAX_FILES) return;

    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    uint8_t yieldCounter = 0;
    File entry = dir.openNextFile();
    while (entry && files.size() < MAX_FILES) {
        if (!entry.isDirectory()) {
            const char* rawName = entry.name();
            char base[40];
            copyBasename(rawName, base, sizeof(base));

            bool match = false;
            for (size_t i = 0; i < extCount; i++) {
                if (endsWithIgnoreCase(base, exts[i])) {
                    match = true;
                    break;
                }
            }

            if (match) {
                PigpassFileEntry info;
                memset(&info, 0, sizeof(info));
                strncpy(info.name, base, sizeof(info.name) - 1);
                snprintf(info.path, sizeof(info.path), "%s/%s", dirPath, base);
                files.push_back(info);
            }
        }
        entry.close();
        entry = dir.openNextFile();

        if (++yieldCounter >= 10) {
            yieldCounter = 0;
            yield();
        }
    }

    if (entry) entry.close();
    dir.close();
}

void PigpassMode::scanHandshakeFiles() {
    clearFileList();
    files.reserve(16);

    if (!Config::isSDAvailable()) {
        Serial.println("[PIGPASS] No SD card for handshake scan");
        return;
    }

    // Tab filters: PCAP = frame captures, HC22000 = hashcat lines.
    static const char* const pcapExts[] = {".pcap", ".cap"};
    static const char* const hcExts[] = {".22000", ".hc22000"};
    const char* const* hsExts = (hsTab == PigpassHsTab::HC22000) ? hcExts : pcapExts;
    const size_t nExt = (hsTab == PigpassHsTab::HC22000) ? 2 : 2;

    const char* hsDir = resolveHandshakeDir();
    scanDirForExt(hsDir, hsExts, nExt);
    scanDirForExt(SDLayout::pigpassDir(), hsExts, nExt);
    scanDirForExt("/0N3P0rK/hs", hsExts, nExt);
    scanDirForExt("/0N3P0rK/pwncrack", hsExts, nExt);
    scanDirForExt("/0N3P0rK/wpa-sec", hsExts, nExt);

    selectedIndex = 0;
    scrollOffset = 0;
    Serial.printf("[PIGPASS] Handshake tab=%s files=%u in %s\n",
                  hsTab == PigpassHsTab::HC22000 ? "22000" : "PCAP",
                  (unsigned)files.size(), hsDir);
}

void PigpassMode::saveLastWordlist(const char* path) {
    if (!path || !path[0] || !Config::isSDAvailable()) return;
    if (strcmp(path, kMaskVirtualPath) == 0) return;
    const char* dir = SDLayout::pigpassDir();
    if (dir && dir[0] && !SD.exists(dir)) SD.mkdir(dir);
    const char* p = SDLayout::pigpassLastWordlistPath();
    if (!p) return;
    File f = SD.open(p, FILE_WRITE);
    if (!f) return;
    f.println(path);
    f.close();
}

bool PigpassMode::loadLastWordlist(char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!Config::isSDAvailable()) return false;
    const char* p = SDLayout::pigpassLastWordlistPath();
    if (!p || !SD.exists(p)) return false;
    File f = SD.open(p, FILE_READ);
    if (!f) return false;
    String line = f.readStringUntil('\n');
    f.close();
    line.trim();
    if (line.length() == 0 || line.length() >= (int)outLen) return false;
    strncpy(out, line.c_str(), outLen - 1);
    out[outLen - 1] = '\0';
    return true;
}

void PigpassMode::applyMaskCharset() {
    // Presets only — generate passwords on the fly from these symbols
    switch (maskCharsetId % 6) {
        case 0:
            strncpy(maskCharset, "0123456789", sizeof(maskCharset) - 1);
            break;
        case 1:
            strncpy(maskCharset, "abcdefghijklmnopqrstuvwxyz", sizeof(maskCharset) - 1);
            break;
        case 2:
            strncpy(maskCharset, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", sizeof(maskCharset) - 1);
            break;
        case 3:
            strncpy(maskCharset, "abcdefghijklmnopqrstuvwxyz0123456789", sizeof(maskCharset) - 1);
            break;
        case 4:
            strncpy(maskCharset, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", sizeof(maskCharset) - 1);
            break;
        default:
            strncpy(maskCharset,
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
                    sizeof(maskCharset) - 1);
            break;
    }
    maskCharset[sizeof(maskCharset) - 1] = '\0';
    maskCharsetLen = (uint8_t)strlen(maskCharset);
    if (maskLen < 8) maskLen = 8;
    if (maskLen > 12) maskLen = 12;
}

const char* PigpassMode::maskCharsetName() {
    switch (maskCharsetId % 6) {
        case 0: return "0-9 digits";
        case 1: return "a-z lower";
        case 2: return "A-Z upper";
        case 3: return "a-z0-9";
        case 4: return "A-Z0-9";
        default: return "a-zA-Z0-9";
    }
}

// Convert index → password in charset^len; returns false when space exhausted
static bool maskEmitNext(char* out, size_t outSz, const char* cs, uint8_t csLen,
                         uint8_t len, uint64_t* idx) {
    if (!out || !cs || !idx || csLen == 0 || len < 8 || (size_t)len + 1 > outSz) return false;

    uint64_t max = 1;
    bool huge = false;
    for (uint8_t i = 0; i < len; i++) {
        if (max > (~(uint64_t)0 / (uint64_t)csLen)) { huge = true; break; }
        max *= csLen;
    }
    if (!huge && *idx >= max) return false;

    uint64_t n = *idx;
    for (int i = (int)len - 1; i >= 0; i--) {
        out[i] = cs[n % csLen];
        n /= csLen;
    }
    out[len] = '\0';
    (*idx)++;
    return true;
}

bool PigpassMode::nextMaskPassword(char* out, size_t outSz) {
    applyMaskCharset();
    return maskEmitNext(out, outSz, maskCharset, maskCharsetLen, maskLen, &maskIndex);
}

void PigpassMode::scanWordlistFiles() {
    clearFileList();
    files.reserve(16);

    // Always first: on-device charset generator (no SD file needed)
    if (files.size() < MAX_FILES) {
        PigpassFileEntry e;
        strncpy(e.name, "* MASK GEN *", sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        strncpy(e.path, kMaskVirtualPath, sizeof(e.path) - 1);
        e.path[sizeof(e.path) - 1] = '\0';
        files.push_back(e);
    }

    if (!Config::isSDAvailable()) {
        Serial.println("[PIGPASS] No SD card for wordlist scan");
        selectedIndex = 0;
        scrollOffset = 0;
        return;
    }

    // Ensure Passworld exists on SD (new layout)
    const char* wlDir = resolvePassworldDir();
    if (SDLayout::usingNewLayout() && !SD.exists(SDLayout::passworldDir())) {
        SD.mkdir(SDLayout::passworldDir());
        wlDir = SDLayout::passworldDir();
    }

    static const char* const wlExts[] = {".txt", ".lst", ".dict"};
    scanDirForExt(wlDir, wlExts, 3);

    // One-shot fallback if preferred dir had nothing but root copy exists


    // Prefer last used wordlist (from previous run / reboot)
    selectedIndex = 0;
    scrollOffset = 0;
    char last[72];
    if (loadLastWordlist(last, sizeof(last))) {
        for (size_t i = 0; i < files.size(); i++) {
            if (strcmp(files[i].path, last) == 0) {
                selectedIndex = (uint16_t)i;
                if (selectedIndex >= VISIBLE_ITEMS) {
                    scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
                }
                Serial.printf("[PIGPASS] prefer last WL idx=%u %s\n",
                              (unsigned)selectedIndex, last);
                break;
            }
        }
    }

    Serial.printf("[PIGPASS] Wordlist files found: %u in %s\n",
                  (unsigned)files.size(), wlDir);
}

void PigpassMode::init() {
    Serial.println("[PIGPASS] Initialized");
    state = PigpassState::IDLE;
    clearFileList();
    handshakePath[0] = '\0';
    wordlistPath[0] = '\0';
    ssid[0] = '\0';
    foundPw[0] = '\0';
    fromCache = false;
    freePbkdfCtx();
    resetWordlistBuffer();
}

void PigpassMode::start() {
    Serial.println("[PIGPASS] Starting PigPass mode");
    // Keep last hsTab if user re-enters; only ensure valid.
    if ((uint8_t)hsTab > (uint8_t)PigpassHsTab::HC22000) hsTab = PigpassHsTab::PCAP;
    if (!allocateRuntimeBuffers()) {
        Serial.println("[PIGPASS] ERROR: runtime buffers unavailable");
        Display::showToast("PIGPASS LOW HEAP");
        return;
    }

    // Visible PigPass: kill farm anims so PBKDF2 gets the core. Minimize → resume.
    Avatar::suspendScene();

    attempts = 0;
    rate = 0.0;
    elapsedSeconds = 0;
    foundPassword = false;
    fromCache = false;
    foundPw[0] = '\0';
    ssid[0] = '\0';
    handshakePath[0] = '\0';
    wordlistPath[0] = '\0';
    maskMode = false;
    maskCharsetId = 0;
    maskLen = 8;
    maskIndex = 0;
    s_currentTry[0] = '\0';
    applyMaskCharset();
    startTime = millis();
    lastUpdateTime = startTime;
    keyWasPressed = true;  // ignore Enter that opened this mode
    uiDirty = true;
    freePbkdfCtx();
    resetWordlistBuffer();

    if (wordlistFile) {
        wordlistFile.close();
    }

    transitionState(PigpassState::SELECT_HANDSHAKE);
}

void PigpassMode::stop() {
    Serial.println("[PIGPASS] Stopping PigPass mode");

    // Persist progress so reboot / re-open can resume same HS+WL
    if (state == PigpassState::RUNNING || state == PigpassState::PAUSED) {
        attempts = s_attempts;
        if (attempts > 0 && !foundPassword) {
            saveCheckpoint();
        }
    }

    stopCrackSession();
    if (wordlistFile) {
        wordlistFile.close();
    }
    freePbkdfCtx();
    resetWordlistBuffer();
    g_hsCryptoReady = false;
    freeRuntimeBuffers();

    attempts = s_attempts;
    uint32_t att_lo = (uint32_t)(attempts & 0xFFFFFFFF);
    Serial.printf("[PIGPASS] Final stats - Attempts: %lu, Time: %us rate=%.1f/s\n",
                  att_lo, elapsedSeconds, rate);
    SDLog::log("PIGPASS", "Stopped - attempts=%lu rate=%.1f", att_lo, rate);

    clearFileList();
    state = PigpassState::IDLE;
    Avatar::resumeScene();
    Avatar::setState(AvatarState::NEUTRAL);
}

void PigpassMode::transitionState(PigpassState newState) {
    if (state == newState) return;

    Serial.printf("[PIGPASS] State transition: %d -> %d\n", (int)state, (int)newState);
    state = newState;
    uiDirty = true;
    keyWasPressed = true;  // debounce across screens

    if (newState == PigpassState::SELECT_MASK) {
        applyMaskCharset();
        return;
    }

    if (newState == PigpassState::SELECT_HANDSHAKE) {
        scanHandshakeFiles();
    } else if (newState == PigpassState::SELECT_WORDLIST) {
        scanWordlistFiles();
    }
}

void PigpassMode::markFound(const char* password, bool cached) {
    if (!password) return;
    foundPassword = true;
    fromCache = cached;
    strncpy(foundPw, password, sizeof(foundPw) - 1);
    foundPw[sizeof(foundPw) - 1] = '\0';
    if (!cached) {
        XP::addXP(XPEvent::PIGPASS_CRACK);  // +100 + lifetime counter
        saveCrackedResult(password);
        SDLog::log("PIGPASS", "PASSWORD FOUND: %s", password);
        Serial.printf("[PIGPASS] PASSWORD FOUND: %s\n", password);
    } else {
        SDLog::log("PIGPASS", "PASSWORD FROM CACHE: %s", password);
        Serial.printf("[PIGPASS] PASSWORD FROM CACHE: %s\n", password);
    }
}

bool PigpassMode::tryLoadCachedResult() {
    if (!handshake.valid || !Config::isSDAvailable()) return false;

    char bssid[13];
    formatApBssid(bssid, sizeof(bssid));

    const char* path = SDLayout::pigpassResultsPath();
    if (!SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    // Line format: BSSID:SSID:password  (BSSID = 12 hex chars, no separators)
    char line[160];
    size_t pos = 0;
    bool found = false;

    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n') {
            line[pos] = '\0';
            // trim CR/space
            while (pos > 0 && (line[pos - 1] == '\r' || line[pos - 1] == ' ' ||
                               line[pos - 1] == '\t')) {
                line[--pos] = '\0';
            }
            if (pos >= 14) {
                bool match = true;
                for (int i = 0; i < 12; i++) {
                    char a = line[i];
                    char b = bssid[i];
                    if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');
                    if (b >= 'a' && b <= 'f') b = (char)(b - 'a' + 'A');
                    if (a != b) { match = false; break; }
                }
                if (match && line[12] == ':') {
                    char* second = strchr(line + 13, ':');
                    if (second && second[1] != '\0') {
                        markFound(second + 1, true);
                        found = true;
                        break;
                    }
                }
            }
            pos = 0;
            continue;
        }
        if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        }
    }
    if (!found && pos > 0) {
        line[pos] = '\0';
        while (pos > 0 && (line[pos - 1] == '\r' || line[pos - 1] == ' ' ||
                           line[pos - 1] == '\t')) {
            line[--pos] = '\0';
        }
        if (pos >= 14) {
            bool match = true;
            for (int i = 0; i < 12; i++) {
                char a = line[i];
                char b = bssid[i];
                if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');
                if (b >= 'a' && b <= 'f') b = (char)(b - 'a' + 'A');
                if (a != b) { match = false; break; }
            }
            if (match && line[12] == ':') {
                char* second = strchr(line + 13, ':');
                if (second && second[1] != '\0') {
                    markFound(second + 1, true);
                    found = true;
                }
            }
        }
    }

    f.close();
    return found;
}

uint32_t PigpassMode::wordlistLogicalOffset() {
    // File cursor is ahead of the logical next password by buffered unread bytes
    if (!wordlistFile) return 0;
    size_t filePos = wordlistFile.position();
    size_t unread = (wlBufLen > wlBufPos) ? (wlBufLen - wlBufPos) : 0;
    if (filePos >= unread) return (uint32_t)(filePos - unread);
    return 0;
}

void PigpassMode::saveCheckpoint() {
    if (!Config::isSDAvailable()) return;
    if (handshakePath[0] == '\0' || wordlistPath[0] == '\0') return;
    if (state != PigpassState::RUNNING && state != PigpassState::PAUSED) return;

    const char* dir = SDLayout::pigpassDir();
    if (dir && dir[0] && !SD.exists(dir)) SD.mkdir(dir);

    const char* path = SDLayout::pigpassCheckpointPath();
    if (!path) return;

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[PIGPASS] checkpoint write fail");
        return;
    }

    // File wordlist: SD byte offset. Mask: current maskIndex (lo32).
    uint32_t off = maskMode ? (uint32_t)(maskIndex & 0xFFFFFFFFUL)
                            : wordlistLogicalOffset();
    uint64_t att = s_attempts ? s_attempts : attempts;
    // v1\nhs\nwl\noffset\nattempts\nelapsed
    f.println("v1");
    f.println(handshakePath);
    f.println(wordlistPath);
    f.println((unsigned long)off);
    f.println((unsigned long)(uint32_t)(att & 0xFFFFFFFFUL));
    f.println((unsigned long)elapsedSeconds);
    f.close();
    s_lastCheckpointMs = millis();
    Serial.printf("[PIGPASS] checkpoint off=%lu att=%lu mask=%d\n",
                  (unsigned long)off, (unsigned long)(uint32_t)att, (int)maskMode);
}

bool PigpassMode::loadCheckpoint(const char* hsPath, const char* wlPath,
                                 uint32_t* outOffset, uint64_t* outAttempts,
                                 uint32_t* outElapsed) {
    if (outOffset) *outOffset = 0;
    if (outAttempts) *outAttempts = 0;
    if (outElapsed) *outElapsed = 0;
    if (!hsPath || !wlPath || !Config::isSDAvailable()) return false;

    const char* path = SDLayout::pigpassCheckpointPath();
    if (!path || !SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    String ver = f.readStringUntil('\n');
    ver.trim();
    String hs = f.readStringUntil('\n');
    hs.trim();
    String wl = f.readStringUntil('\n');
    wl.trim();
    String offS = f.readStringUntil('\n');
    offS.trim();
    String attS = f.readStringUntil('\n');
    attS.trim();
    String elS = f.readStringUntil('\n');
    elS.trim();
    f.close();

    if (ver != "v1") return false;
    if (hs != hsPath || wl != wlPath) return false;

    uint32_t off = (uint32_t)offS.toInt();
    uint32_t att = (uint32_t)attS.toInt();
    uint32_t el = (uint32_t)elS.toInt();
    if (att == 0 && off == 0) return false;

    if (outOffset) *outOffset = off;
    if (outAttempts) *outAttempts = att;
    if (outElapsed) *outElapsed = el;
    return true;
}

void PigpassMode::clearCheckpoint() {
    if (!Config::isSDAvailable()) return;
    const char* path = SDLayout::pigpassCheckpointPath();
    if (path && SD.exists(path)) {
        SD.remove(path);
        Serial.println("[PIGPASS] checkpoint cleared");
    }
    s_resumePending = false;
    s_resumeOffset = 0;
    s_resumeAttempts = 0;
    s_resumeElapsed = 0;
}

void PigpassMode::saveCrackedResult(const char* password) {
    if (!password || !handshake.valid || !Config::isSDAvailable()) return;

    // Ensure pigpass dir exists
    const char* dir = SDLayout::pigpassDir();
    if (dir && dir[0] && !SD.exists(dir)) {
        SD.mkdir(dir);
    }

    char bssid[13];
    formatApBssid(bssid, sizeof(bssid));
    const char* path = SDLayout::pigpassResultsPath();

    // Skip write if this BSSID is already cached (do not touch found* flags)
    if (SD.exists(path)) {
        File rf = SD.open(path, FILE_READ);
        if (rf) {
            char line[160];
            size_t pos = 0;
            bool exists = false;
            while (rf.available()) {
                char c = (char)rf.read();
                if (c == '\n' || !rf.available()) {
                    if (c != '\n' && pos + 1 < sizeof(line)) line[pos++] = c;
                    line[pos] = '\0';
                    if (pos >= 13) {
                        bool match = true;
                        for (int i = 0; i < 12; i++) {
                            char a = line[i];
                            char b = bssid[i];
                            if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');
                            if (b >= 'a' && b <= 'f') b = (char)(b - 'a' + 'A');
                            if (a != b) { match = false; break; }
                        }
                        if (match && line[12] == ':') {
                            exists = true;
                            break;
                        }
                    }
                    pos = 0;
                    continue;
                }
                if (pos + 1 < sizeof(line)) line[pos++] = c;
            }
            rf.close();
            if (exists) return;
        }
    }

    // Prefer append mode ("a"); FILE_APPEND is defined by Arduino FS
    File f = SD.open(path, "a");
    if (!f) {
        f = SD.open(path, FILE_WRITE);
        if (!f) {
            Serial.printf("[PIGPASS] ERROR: cannot write cache %s\n", path);
            return;
        }
    }

    // BSSID:SSID:password
    f.printf("%s:%s:%s\n", bssid, handshake.ssid, password);
    f.close();
    Serial.printf("[PIGPASS] Saved result to %s\n", path);
}

void PigpassMode::beginWordlistRun(const char* path) {
    strncpy(wordlistPath, path, sizeof(wordlistPath) - 1);
    wordlistPath[sizeof(wordlistPath) - 1] = '\0';
    maskMode = false;
    saveLastWordlist(wordlistPath);

    if (!handshake.valid && !parsePcapHandshake(handshakePath)) {
        Display::showToast("BAD HANDSHAKE FILE");
        transitionState(PigpassState::SELECT_HANDSHAKE);
        return;
    }
    prepareHandshakeCrypto();
    strncpy(ssid, handshake.ssid, 32);
    ssid[32] = 0;
    clearFileList();
    stopCrackSession();
    resetWordlistBuffer();
    freePbkdfCtx();
    foundPassword = false;
    fromCache = false;
    foundPw[0] = '\0';
    s_foundFlag = false;
    s_wordlistDone = false;
    s_sessionStarted = false;

    s_resumePending = false;
    s_resumeOffset = 0;
    s_resumeAttempts = 0;
    s_resumeElapsed = 0;
    uint32_t ckOff = 0, ckEl = 0;
    uint64_t ckAtt = 0;
    if (loadCheckpoint(handshakePath, wordlistPath, &ckOff, &ckAtt, &ckEl) &&
        ckAtt > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "from try %lu ?",
                 (unsigned long)(uint32_t)ckAtt);
        if (Display::showConfirmBox("RESUME?", msg)) {
            s_resumePending = true;
            s_resumeOffset = ckOff;
            s_resumeAttempts = ckAtt;
            s_resumeElapsed = ckEl;
            attempts = ckAtt;
            s_attempts = ckAtt;
            elapsedSeconds = ckEl;
            startTime = millis() - (ckEl * 1000UL);
            lastUpdateTime = millis();
            Display::showToast("RESUME");
        } else {
            clearCheckpoint();
            attempts = 0;
            s_attempts = 0;
            elapsedSeconds = 0;
            startTime = millis();
            lastUpdateTime = startTime;
        }
    } else {
        attempts = 0;
        s_attempts = 0;
        elapsedSeconds = 0;
        startTime = millis();
        lastUpdateTime = startTime;
    }
    transitionState(PigpassState::RUNNING);
}

void PigpassMode::beginMaskRun() {
    applyMaskCharset();
    maskMode = true;
    // Encode mask config into wordlistPath for checkpoint identity
    snprintf(wordlistPath, sizeof(wordlistPath), "@mask:%u:%u",
             (unsigned)(maskCharsetId % 6), (unsigned)maskLen);

    if (!handshake.valid && !parsePcapHandshake(handshakePath)) {
        Display::showToast("BAD HANDSHAKE FILE");
        transitionState(PigpassState::SELECT_HANDSHAKE);
        return;
    }
    prepareHandshakeCrypto();
    strncpy(ssid, handshake.ssid, 32);
    ssid[32] = 0;
    clearFileList();
    stopCrackSession();
    resetWordlistBuffer();
    freePbkdfCtx();
    if (wordlistFile) { wordlistFile.close(); }

    foundPassword = false;
    fromCache = false;
    foundPw[0] = '\0';
    s_foundFlag = false;
    s_wordlistDone = false;
    s_sessionStarted = false;

    // Resume mask from checkpoint (offset field stores maskIndex lo32)
    s_resumePending = false;
    s_resumeOffset = 0;
    s_resumeAttempts = 0;
    s_resumeElapsed = 0;
    uint32_t ckOff = 0, ckEl = 0;
    uint64_t ckAtt = 0;
    if (loadCheckpoint(handshakePath, wordlistPath, &ckOff, &ckAtt, &ckEl) &&
        ckAtt > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "from try %lu ?",
                 (unsigned long)(uint32_t)ckAtt);
        if (Display::showConfirmBox("RESUME?", msg)) {
            maskIndex = ckOff;
            attempts = ckAtt;
            s_attempts = ckAtt;
            elapsedSeconds = ckEl;
            startTime = millis() - (ckEl * 1000UL);
            lastUpdateTime = millis();
            Display::showToast("RESUME MASK");
        } else {
            clearCheckpoint();
            maskIndex = 0;
            attempts = 0;
            s_attempts = 0;
            elapsedSeconds = 0;
            startTime = millis();
            lastUpdateTime = startTime;
        }
    } else {
        maskIndex = 0;
        attempts = 0;
        s_attempts = 0;
        elapsedSeconds = 0;
        startTime = millis();
        lastUpdateTime = startTime;
    }
    transitionState(PigpassState::RUNNING);
}

void PigpassMode::selectCurrentFile() {
    if (files.empty() || selectedIndex >= files.size()) return;

    const PigpassFileEntry& entry = files[selectedIndex];

    if (state == PigpassState::SELECT_HANDSHAKE) {
        strncpy(handshakePath, entry.path, sizeof(handshakePath) - 1);
        handshakePath[sizeof(handshakePath) - 1] = '\0';
        Serial.printf("[PIGPASS] Selected handshake: %s\n", handshakePath);

        if (!parsePcapHandshake(handshakePath)) {
            Display::showToast("BAD HANDSHAKE FILE");
            return;
        }
        prepareHandshakeCrypto();

        strncpy(ssid, handshake.ssid, 32);
        ssid[32] = 0;

        // Already cracked? skip wordlist + brute
        if (tryLoadCachedResult()) {
            clearFileList();
            Display::showToast("FROM CACHE");
            transitionState(PigpassState::DONE);
            return;
        }

        transitionState(PigpassState::SELECT_WORDLIST);
        return;
    }

    if (state == PigpassState::SELECT_WORDLIST) {
        if (strcmp(entry.path, kMaskVirtualPath) == 0) {
            maskCharsetId = 0;
            maskLen = 8;
            maskIndex = 0;
            applyMaskCharset();
            transitionState(PigpassState::SELECT_MASK);
            return;
        }
        beginWordlistRun(entry.path);
    }
}

void PigpassMode::update() {
    if (state == PigpassState::IDLE) return;

    // Open window → scene off (more crack CPU). Minimized → scene on (farm lives).
    if (App::windowHidden()) {
        if (Avatar::isSceneSuspended()) Avatar::resumeScene();
    } else {
        if (!Avatar::isSceneSuspended()) Avatar::suspendScene();
    }

    if (state == PigpassState::SELECT_HANDSHAKE ||
        state == PigpassState::SELECT_WORDLIST ||
        state == PigpassState::SELECT_MASK) {
        if (!App::windowHidden()) handleInput();
        return;
    }

    if (state == PigpassState::RUNNING) {
        // Pull stats from crack core
        attempts = s_attempts;
        uint32_t now = millis();
        if (now - lastUpdateTime >= 400) {
            elapsedSeconds = (now - startTime) / 1000;
            // Prefer short-window rate after first second for snappier display
            rate = (elapsedSeconds > 0) ? (attempts / (double)elapsedSeconds) : 0.0;
            lastUpdateTime = now;
            uiDirty = true;
        }
        if (!App::windowHidden()) handleInput();
        runBrute();  // harvest found / EOF; task does the work
        return;
    }

    if (state == PigpassState::PAUSED) {
        attempts = s_attempts;
        if (!App::windowHidden()) handleInput();
        return;
    }

    if (state == PigpassState::DONE) {
        if (!App::windowHidden()) handleInput();
    }
}

const char* PigpassMode::getCurrentTry() {
    return s_currentTry;
}

void PigpassMode::draw(M5Canvas& canvas) {
    if (state == PigpassState::IDLE) return;
    drawUI(canvas);
}

void PigpassMode::drawFileBrowser(M5Canvas& canvas, const char* title) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);

    if (!Config::isSDAvailable()) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("NO SD CARD", 4, 36);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("INSERT AND RESTART", 4, 52);
        return;
    }

    canvas.setTextWrap(false);

    const bool hsBrowser = (state == PigpassState::SELECT_HANDSHAKE);
    int listTop = 28;

    if (hsBrowser) {
        // LOOT-style dual tabs: PCAP | 22000
        const bool tabPcap = (hsTab == PigpassHsTab::PCAP);
        canvas.fillRect(4, 2, 112, 13, tabPcap ? UiStyle::PINK : UiStyle::PANEL);
        canvas.fillRect(124, 2, 112, 13, !tabPcap ? UiStyle::PINK : UiStyle::PANEL);
        canvas.setTextDatum(top_center);
        canvas.setTextColor(tabPcap ? UiStyle::BG : UiStyle::TEXT);
        canvas.drawString("PCAP", 60, 5);
        canvas.setTextColor(!tabPcap ? UiStyle::BG : UiStyle::TEXT);
        canvas.drawString("22000", 180, 5);
        canvas.setTextDatum(top_left);

        char countBuf[32];
        snprintf(countBuf, sizeof(countBuf), "%u file(s)", (unsigned)files.size());
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(countBuf, 4, 17);
        listTop = 30;
    } else {
        canvas.setTextColor(UiStyle::TITLE);
        canvas.drawString(title ? title : "SELECT FILE", 4, 2);

        char countBuf[28];
        snprintf(countBuf, sizeof(countBuf), "%u file(s)", (unsigned)files.size());
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(countBuf, 4, 14);
        listTop = 28;
    }

    if (files.empty()) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("NO FILES FOUND", 4, listTop + 12);
        canvas.setTextColor(UiStyle::DIM);
        if (hsBrowser) {
            if (hsTab == PigpassHsTab::PCAP)
                canvas.drawString("NEED .PCAP/.CAP IN HS DIR", 4, listTop + 28);
            else
                canvas.drawString("NEED .22000 IN HS DIR", 4, listTop + 28);
        } else {
            canvas.drawString("PUT .TXT:", 4, listTop + 28);
            canvas.drawString("/0N3P0rK/Passworld", 4, listTop + 40);
        }
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString(hsBrowser ? ",/ tab  ` exit" : "` =EXIT", 4, MAIN_H - 10);
        return;
    }

    const int lineHeight = 13;
    const int nameMaxPx = DISPLAY_W - 18;
    int y = listTop;

    for (uint16_t i = scrollOffset;
         i < files.size() && i < scrollOffset + VISIBLE_ITEMS;
         i++) {
        bool sel = (i == selectedIndex);
        if (sel) {
            canvas.fillRect(0, y - 1, DISPLAY_W, lineHeight, UiStyle::PINK);
            canvas.setTextColor(UiStyle::BG);
        } else if (files[i].path[0] == '@') {
            canvas.setTextColor(UiStyle::CYAN);  // virtual * MASK GEN *
        } else {
            canvas.setTextColor(UiStyle::TEXT);
        }

        const char* nm = files[i].name[0] ? files[i].name : "-";
        if (sel) {
            uiDrawMarquee(canvas, nm, 4, y + 2, nameMaxPx);
        } else {
            char nameBuf[40];
            strncpy(nameBuf, nm, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            truncateName(nameBuf, (size_t)(nameMaxPx / 6));
            canvas.drawString(nameBuf, 4, y + 2);
        }
        y += lineHeight;
    }

    if (scrollOffset > 0) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("^", DISPLAY_W - 10, listTop);
    }
    if (scrollOffset + VISIBLE_ITEMS < files.size()) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("v", DISPLAY_W - 10, listTop + (VISIBLE_ITEMS - 1) * lineHeight);
    }

    canvas.setTextColor(UiStyle::GOLD);
    if (state == PigpassState::SELECT_HANDSHAKE)
        canvas.drawString(";/. move  ,/ tab  ENT  `", 4, MAIN_H - 10);
    else
        canvas.drawString(";/. move  ENT sel  ` back", 4, MAIN_H - 10);
}

void PigpassMode::drawMaskSetup(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);

    canvas.setTextColor(UiStyle::TITLE);
    canvas.drawString("MASK GENERATOR", 4, 2);
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("only these symbols", 4, 14);

    char buf[48];
    applyMaskCharset();

    canvas.fillRect(2, 28, DISPLAY_W - 4, 16, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::CYAN);
    snprintf(buf, sizeof(buf), "Set  %s", maskCharsetName());
    canvas.drawString(buf, 6, 32);

    canvas.fillRect(2, 48, DISPLAY_W - 4, 16, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::TEXT);
    snprintf(buf, sizeof(buf), "Len  %u   (WPA min 8)", (unsigned)maskLen);
    canvas.drawString(buf, 6, 52);

    double space = 1.0;
    for (uint8_t i = 0; i < maskLen; i++) space *= (double)maskCharsetLen;
    if (space >= 1e9) {
        snprintf(buf, sizeof(buf), "~%.1fB combos", space / 1e9);
    } else if (space >= 1e6) {
        snprintf(buf, sizeof(buf), "~%.1fM combos", space / 1e6);
    } else if (space >= 1e3) {
        snprintf(buf, sizeof(buf), "~%.0fK combos", space / 1e3);
    } else {
        snprintf(buf, sizeof(buf), "%llu combos", (unsigned long long)space);
    }
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString(buf, 4, 70);

    char ex[16];
    uint64_t tmp = 0;
    if (maskEmitNext(ex, sizeof(ex), maskCharset, maskCharsetLen, maskLen, &tmp)) {
        canvas.setTextColor(UiStyle::PINK);
        snprintf(buf, sizeof(buf), "ex: %s", ex);
        canvas.drawString(buf, 4, 84);
    }

    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString(";/. set   ,/ len   ENT go", 4, MAIN_H - 10);
}

void PigpassMode::drawUI(M5Canvas& canvas) {
    if (state == PigpassState::SELECT_HANDSHAKE) {
        drawFileBrowser(canvas, "SELECT HANDSHAKE");
        return;
    }
    if (state == PigpassState::SELECT_WORDLIST) {
        drawFileBrowser(canvas, "SELECT WORDLIST");
        return;
    }
    if (state == PigpassState::SELECT_MASK) {
        drawMaskSetup(canvas);
        return;
    }

    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
    canvas.setTextWrap(false);

    // Header row: title + state badge
    canvas.setTextColor(UiStyle::TITLE);
    canvas.drawString("PIGPASS", 4, 2);

    const char* badge = "IDLE";
    uint16_t badgeCol = UiStyle::DIM;
    if (state == PigpassState::RUNNING) {
        badge = maskMode ? "MASK" : "RUN";
        badgeCol = UiStyle::GREEN;
    } else if (state == PigpassState::PAUSED) {
        badge = "PAUSE";
        badgeCol = UiStyle::GOLD;
    } else if (state == PigpassState::DONE && foundPassword) {
        badge = fromCache ? "CACHE" : "FOUND";
        badgeCol = UiStyle::GREEN;
    } else if (state == PigpassState::DONE) {
        badge = "MISS";
        badgeCol = UiStyle::RED;
    }
    canvas.setTextColor(badgeCol);
    canvas.drawString(badge, DISPLAY_W - 40, 2);

    // SSID
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("SSID", 4, 16);
    canvas.setTextColor(UiStyle::TEXT);
    char buf[64];
    uiDrawMarquee(canvas, ssid[0] ? ssid : "-", 36, 16, DISPLAY_W - 44);

    // Big "currently testing" panel
    canvas.fillRect(2, 30, DISPLAY_W - 4, 28, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("TRY", 6, 34);
    canvas.setTextColor(foundPassword ? UiStyle::GREEN : UiStyle::CYAN);
    canvas.setTextSize(2);
    if (foundPassword && foundPw[0]) {
        uiDrawMarquee(canvas, foundPw, 6, 42, DISPLAY_W - 16, 12);
    } else if (s_currentTry[0]) {
        uiDrawMarquee(canvas, s_currentTry, 6, 42, DISPLAY_W - 16, 12);
    } else {
        canvas.setTextSize(1);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(fromCache ? "(cache)" : "...", 6, 46);
        canvas.setTextSize(2);
    }
    canvas.setTextSize(1);

    // Stats row
    int y = 64;
    if (maskMode && !fromCache) {
        canvas.setTextColor(UiStyle::DIM);
        snprintf(buf, sizeof(buf), "%s L%u", maskCharsetName(), (unsigned)maskLen);
        canvas.drawString(buf, 4, y);
        y += 12;
    }

    if (!fromCache) {
        uint32_t att_lo = (uint32_t)(attempts & 0xFFFFFFFF);
        canvas.setTextColor(UiStyle::TEXT);
        snprintf(buf, sizeof(buf), "#%lu", (unsigned long)att_lo);
        canvas.drawString(buf, 4, y);
        canvas.setTextColor(UiStyle::CYAN);
        snprintf(buf, sizeof(buf), "%.1f/s", rate);
        canvas.drawString(buf, 90, y);
        canvas.setTextColor(UiStyle::DIM);
        snprintf(buf, sizeof(buf), "%us", (unsigned)elapsedSeconds);
        canvas.drawString(buf, 160, y);
        y += 12;
    } else {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("loaded from SD cache", 4, y);
        y += 12;
    }

    if (foundPassword) {
        canvas.setTextColor(UiStyle::GOLD);
        snprintf(buf, sizeof(buf), "OK  %s", foundPw);
        canvas.drawString(buf, 4, y);
    }

    canvas.setTextColor(UiStyle::GOLD);
    if (state == PigpassState::RUNNING) {
        canvas.drawString("ENT pause   ` save+exit", 4, MAIN_H - 10);
    } else if (state == PigpassState::PAUSED) {
        canvas.drawString("ENT resume  ` save+exit", 4, MAIN_H - 10);
    } else {
        canvas.drawString("ENT / ` exit", 4, MAIN_H - 10);
    }
}

void PigpassMode::handleMaskInput() {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool back = keyEsc();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool lenDown = M5Cardputer.Keyboard.isKeyPressed(',');
    bool lenUp = M5Cardputer.Keyboard.isKeyPressed('/');

    if (back) {
        transitionState(PigpassState::SELECT_WORDLIST);
        return;
    }
    if (up) {
        maskCharsetId = (uint8_t)((maskCharsetId + 5) % 6);  // prev
        applyMaskCharset();
        uiDirty = true;
        return;
    }
    if (down) {
        maskCharsetId = (uint8_t)((maskCharsetId + 1) % 6);
        applyMaskCharset();
        uiDirty = true;
        return;
    }
    if (lenDown && maskLen > 8) {
        maskLen--;
        uiDirty = true;
        return;
    }
    if (lenUp && maskLen < 12) {
        maskLen++;
        uiDirty = true;
        return;
    }
    if (keys.enter) {
        beginMaskRun();
    }
}

void PigpassMode::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    if (keyWasPressed) return;
    keyWasPressed = true;

    auto keys = M5Cardputer.Keyboard.keysState();
    bool back = keyEsc();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');

    if (state == PigpassState::SELECT_MASK) {
        handleMaskInput();
        return;
    }

    // File selection screens
    if (state == PigpassState::SELECT_HANDSHAKE || state == PigpassState::SELECT_WORDLIST) {
        if (back) {
            if (state == PigpassState::SELECT_WORDLIST) {
                transitionState(PigpassState::SELECT_HANDSHAKE);
            } else {
                stop();
            }
            return;
        }

        // Handshake tabs: , / like LOOT (WPASEC / PWNCRACK)
        if (state == PigpassState::SELECT_HANDSHAKE &&
            (M5Cardputer.Keyboard.isKeyPressed(',') ||
             M5Cardputer.Keyboard.isKeyPressed('/'))) {
            hsTab = (hsTab == PigpassHsTab::PCAP)
                        ? PigpassHsTab::HC22000
                        : PigpassHsTab::PCAP;
            scanHandshakeFiles();
            uiDirty = true;
            return;
        }

        if (up) {
            if (selectedIndex > 0) {
                selectedIndex--;
                if (selectedIndex < scrollOffset) {
                    scrollOffset = selectedIndex;
                }
                uiDirty = true;
            }
            return;
        }

        if (down) {
            if (!files.empty() && selectedIndex + 1 < files.size()) {
                selectedIndex++;
                if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                    scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
                }
                uiDirty = true;
            }
            return;
        }

        if (keys.enter) {
            selectCurrentFile();
        }
        return;
    }

    // Running / paused / done
    if (state == PigpassState::RUNNING) {
        if (keys.enter) {
            s_crackPause = true;
            attempts = s_attempts;
            saveCheckpoint();
            transitionState(PigpassState::PAUSED);
            return;
        }
        if (back) {
            stop();
            return;
        }
        return;
    }

    if (state == PigpassState::PAUSED) {
        if (keys.enter) {
            // Resume with adjusted start so elapsed stays continuous
            startTime = millis() - (elapsedSeconds * 1000UL);
            lastUpdateTime = millis();
            s_crackPause = false;
            transitionState(PigpassState::RUNNING);
            return;
        }
        if (back) {
            stop();
            return;
        }
        return;
    }

    if (state == PigpassState::DONE) {
        if (keys.enter || back) {
            stop();
        }
    }
}

void PigpassMode::runBrute() {
    if (state != PigpassState::RUNNING) {
        return;
    }

    // Harvest match
    if (s_foundFlag) {
        s_foundFlag = false;
        stopCrackSession();
        if (wordlistFile) wordlistFile.close();
        attempts = s_attempts;
        markFound(s_foundLocal, false);
        freePbkdfCtx();
        resetWordlistBuffer();
        clearCheckpoint();  // cracked — no need to resume
        transitionState(PigpassState::DONE);
        return;
    }

    // Boot session once — no FreeRTOS worker (avoids TASK FAIL on low heap)
    if (!s_sessionStarted && !s_wordlistDone) {
        if (!maskMode) {
            if (!wordlistFile) {
                wordlistFile = SD.open(wordlistPath, FILE_READ);
                if (!wordlistFile) {
                    Serial.printf("[PIGPASS] ERROR: Cannot open wordlist: %s\n", wordlistPath);
                    Display::showToast("WORDLIST OPEN FAIL");
                    transitionState(PigpassState::DONE);
                    return;
                }
                resetWordlistBuffer();
                if (s_resumePending && s_resumeOffset > 0) {
                    if (!wordlistFile.seek(s_resumeOffset)) {
                        Serial.printf("[PIGPASS] seek fail off=%lu — start over\n",
                                      (unsigned long)s_resumeOffset);
                        wordlistFile.seek(0);
                        s_resumeAttempts = 0;
                        s_resumeElapsed = 0;
                        s_attempts = 0;
                        attempts = 0;
                        elapsedSeconds = 0;
                        startTime = millis();
                    } else {
                        Serial.printf("[PIGPASS] seeked to off=%lu att=%lu\n",
                                      (unsigned long)s_resumeOffset,
                                      (unsigned long)(uint32_t)s_resumeAttempts);
                    }
                    s_resumePending = false;
                }
            }
        } else {
            applyMaskCharset();
        }
        prepareHandshakeCrypto();
        if (!g_hsCryptoReady || !handshake.valid) {
            Display::showToast("HS CRYPTO FAIL");
            transitionState(PigpassState::DONE);
            return;
        }
        // Keep attempt counter when resuming mid-wordlist / mask
        bool keep = (s_attempts > 0);
        startCrackSession(keep);
        if (keep) s_attempts = attempts;  // restore after start if needed
        s_lastCheckpointMs = millis();
    }

    if (s_crackPause) return;

    // Crack as many passwords as fit in ~150ms per frame (UI still responsive)
    char password[64];
    const uint32_t budgetStart = millis();
    while ((millis() - budgetStart) < 150) {
        if (s_foundFlag) break;

        if (s_wordlistDone) {
            stopCrackSession();
            if (wordlistFile) wordlistFile.close();
            attempts = s_attempts;
            freePbkdfCtx();
            resetWordlistBuffer();
            clearCheckpoint();  // finished list — done
            Serial.printf("[PIGPASS] Wordlist done attempts=%lu rate=%.1f\n",
                          (unsigned long)(uint32_t)attempts, rate);
            transitionState(PigpassState::DONE);
            return;
        }

        bool got = false;
        if (maskMode) {
            got = nextMaskPassword(password, sizeof(password));
            if (!got) s_wordlistDone = true;
        } else {
            got = readNextPassword(password, sizeof(password));
            if (!got) s_wordlistDone = true;
        }
        if (!got) continue;
        tryNextPassword(password);
    }

    attempts = s_attempts;

    // Auto-save progress every few seconds (power-loss / reboot safe)
    if (s_attempts > 0 && (millis() - s_lastCheckpointMs) >= kCheckpointIntervalMs) {
        saveCheckpoint();
    }
}

void PigpassMode::getStatusLine(char* out, size_t len) {
    if (!out || len == 0) return;
    out[0] = '\0';

    switch (state) {
        case PigpassState::SELECT_HANDSHAKE:
            if (files.empty()) {
                snprintf(out, len, "NO HANDSHAKES  ` EXIT");
            } else if (selectedIndex < files.size()) {
                snprintf(out, len, "HS: %s", files[selectedIndex].name);
            } else {
                snprintf(out, len, "SELECT HANDSHAKE");
            }
            break;
        case PigpassState::SELECT_WORDLIST:
            if (files.empty()) {
                snprintf(out, len, "NO WORDLISTS  ` BACK");
            } else if (selectedIndex < files.size()) {
                snprintf(out, len, "WL: %s", files[selectedIndex].name);
            } else {
                snprintf(out, len, "SELECT WORDLIST");
            }
            break;
        case PigpassState::SELECT_MASK:
            snprintf(out, len, "MASK %s L%u", maskCharsetName(), (unsigned)maskLen);
            break;
        case PigpassState::RUNNING: {
            uint32_t att_lo = (uint32_t)(attempts & 0xFFFFFFFF);
            if (s_currentTry[0]) {
                // Show live candidate + attempt count
                snprintf(out, len, "#%lu %s",
                         (unsigned long)att_lo, s_currentTry);
            } else if (maskMode) {
                snprintf(out, len, "MASK %lu  %.1f/s",
                         (unsigned long)att_lo, rate);
            } else {
                snprintf(out, len, "TRY %lu  %.1f/s  %us",
                         (unsigned long)att_lo, rate, (unsigned)elapsedSeconds);
            }
            break;
        }
        case PigpassState::PAUSED:
            if (s_currentTry[0]) {
                snprintf(out, len, "PAUSE %s", s_currentTry);
            } else {
                snprintf(out, len, "PAUSED  ENT=RESUME");
            }
            break;
        case PigpassState::DONE:
            if (foundPassword) {
                snprintf(out, len, "%s: %s", fromCache ? "CACHE" : "FOUND", foundPw);
            } else {
                snprintf(out, len, "DONE  NOT FOUND");
            }
            break;
        default:
            snprintf(out, len, "PIGPASS");
            break;
    }
}
