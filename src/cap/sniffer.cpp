// Passive WPasec capture engine.
// Captures only beacon/probe-response context and EAPOL data frames.
// No deauthentication, authentication flooding, CSA, or other injection.

#include "sniffer.h"
#include "pcap.h"
#include "capture_name.h"
#include "../storage/littlefs_ops.h"
#include "../core/config.h"
#include <WiFi.h>
#include <SD.h>
#include <esp_wifi.h>
#include <string.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, int32_t) {
    return 0;
}

namespace Cap {

static const uint8_t HOP_ALL[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
static const uint8_t HOP_CORE[] = {1, 6, 11};
static const uint8_t RING_SLOTS = 16;
static const uint16_t FRAME_MAX = 1400;
static const uint16_t MAX_FILES = 200;
static const uint32_t MAX_FILE_SIZE = 8UL * 1024UL * 1024UL;

struct QueuedFrame {
    uint16_t len;
    uint32_t ts;
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t frame[FRAME_MAX];
};

static QueuedFrame s_ring[RING_SLOTS];
static volatile uint8_t s_write = 0;
static volatile uint8_t s_read = 0;
static volatile bool s_running = false;
static RunMode s_mode = RunMode::Off;
static bool s_hopping = false;
static uint8_t s_channelIndex = 0;
static uint32_t s_lastHop = 0;
static File s_file;
static uint8_t s_fileBssid[6] = {};
static uint32_t s_fileSize = 0;
static bool s_fileOpen = false;
static Counters s_count = {};
static uint8_t s_hopSet = 0;
static uint16_t s_hopMs = 300;
static uint8_t s_pinBssid[6] = {};
static uint8_t s_pinChannel = 6;
static bool s_pinned = false;

static const uint8_t* hopTable(uint8_t& count) {
    if (s_hopSet == (uint8_t)HopSet::CORE) {
        count = sizeof(HOP_CORE);
        return HOP_CORE;
    }
    count = sizeof(HOP_ALL);
    return HOP_ALL;
}

static bool isZero(const uint8_t* mac) {
    for (uint8_t i = 0; i < 6; ++i) if (mac[i]) return false;
    return true;
}

static bool sameBssid(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

static bool isEapol(const uint8_t* frame, uint16_t len,
                    const uint8_t*& bssid) {
    if (!frame || len < 32 || (frame[0] & 0x0c) != 0x08) return false;
    const bool toDs = (frame[1] & 0x01) != 0;
    const bool fromDs = (frame[1] & 0x02) != 0;
    uint16_t offset = 24;
    if (toDs && fromDs) {
        if (len < 34) return false;
        bssid = frame + 16;
        offset = 30;
    } else if (toDs) {
        bssid = frame + 4;
    } else if (fromDs) {
        bssid = frame + 10;
    } else {
        bssid = frame + 16;
    }
    const uint8_t subtype = (frame[0] >> 4) & 0x0f;
    if (subtype & 0x08) offset += 2;
    if ((frame[1] & 0x80) && offset + 4 <= len) offset += 4;
    return offset + 8 <= len &&
           frame[offset] == 0xaa && frame[offset + 1] == 0xaa &&
           frame[offset + 2] == 0x03 && frame[offset + 6] == 0x88 &&
           frame[offset + 7] == 0x8e;
}

static bool beaconBssid(const uint8_t* frame, uint16_t len, uint8_t out[6]) {
    if (len < 24) return false;
    const uint8_t type = frame[0] & 0xfc;
    if (type != 0x80 && type != 0x50) return false;
    memcpy(out, frame + 16, 6);
    return true;
}

static void closeCapture() {
    if (!s_fileOpen) return;
    s_file.flush();
    s_file.close();
    s_fileOpen = false;
}

static bool openCapture(const uint8_t bssid[6]) {
    if (s_fileOpen && sameBssid(s_fileBssid, bssid)) return true;
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
    if (s_pinned && !sameBssid(q.bssid, s_pinBssid)) return;
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

static void IRAM_ATTR promiscuousCallback(void* raw,
                                           wifi_promiscuous_pkt_type_t type) {
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
    const bool keep = beaconBssid(frame, len, bssid) ||
                      (isEapol(frame, len, eapolBssid) &&
                       (memcpy(bssid, eapolBssid, 6), true));
    if (!keep || (s_pinned && !sameBssid(bssid, s_pinBssid))) return;
    if (isEapol(frame, len, eapolBssid)) s_count.framesEapol++;
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

static void setChannel(uint8_t channel) {
    if (channel < 1 || channel > 13) return;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_count.currentChannel = channel;
}

void begin() {
    Storage::begin();
    Storage::ensureDir(Storage::DIR_HANDSHAKES);
    s_count = {};
}

static void start(RunMode mode) {
    if (s_running) stop();
    s_mode = mode;
    s_running = true;
    s_hopping = mode != RunMode::Pinned;
    s_write = s_read = 0;
    s_count = {};
    s_channelIndex = 0;
    s_hopSet = Config::radio().hopSet;
    s_hopMs = Config::radio().hopMs < 100 ? 100 : Config::radio().hopMs;
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    uint8_t n = 0;
    const uint8_t* hops = hopTable(n);
    setChannel(s_pinned ? s_pinChannel : hops[0]);
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    s_lastHop = millis();
}

void startLight() { s_pinned = false; start(RunMode::Light); }
void startAggressive() { s_pinned = false; start(RunMode::Aggressive); }
void startPinned(uint8_t ch, const uint8_t* bssid, const char*) {
    if (!bssid) return;
    memcpy(s_pinBssid, bssid, 6);
    s_pinChannel = (ch >= 1 && ch <= 13) ? ch : 6;
    s_pinned = true;
    start(RunMode::Pinned);
    setChannel(s_pinChannel);
}

void stop() {
    if (!s_running && s_mode == RunMode::Off) return;
    s_running = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    while (s_read != s_write) {
        writeFrame(s_ring[s_read]);
        s_read = (uint8_t)((s_read + 1) % RING_SLOTS);
    }
    closeCapture();
    s_mode = RunMode::Off;
    s_hopping = false;
}

bool isRunning() { return s_running; }
RunMode runMode() { return s_mode; }
bool isLocked() { return false; }
bool skipCurrent() { return false; }
bool isSkipped(const uint8_t*) { return false; }
void setHsDepth(uint8_t) {}
uint8_t hsDepth() { return 0; }
const Counters& counters() { return s_count; }

void loop() {
    if (!s_running) return;
    while (s_read != s_write) {
        writeFrame(s_ring[s_read]);
        s_read = (uint8_t)((s_read + 1) % RING_SLOTS);
    }
    if (s_fileOpen) s_file.flush();
    if (s_hopping && millis() - s_lastHop >= s_hopMs) {
        uint8_t n = 0;
        const uint8_t* hops = hopTable(n);
        s_channelIndex = (uint8_t)((s_channelIndex + 1) % n);
        setChannel(hops[s_channelIndex]);
        s_lastHop = millis();
    }
}

} // namespace Cap
