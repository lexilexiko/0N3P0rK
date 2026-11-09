// mesh_pig.cpp — Meshtastic-compatible LoRa mesh for 0N3P0rK
// SX1262 (CAP LoRa-1262) via RadioLib. No external Meshtastic library.
//
// OTA PACKET FORMAT (Meshtastic spec):
//   [16-byte header][AES-128-CTR encrypted nanopb MeshPacket payload]
//
//   Header layout (all little-endian):
//     bytes 0-3   : to       (uint32)
//     bytes 4-7   : from     (uint32)
//     bytes 8-11  : id       (uint32, packet ID)
//     byte  12    : flags    (hop_limit<<5 | want_ack<<4 | hop_start<<0 ... 0x63 default)
//   byte  13    : channel hash
//     byte  14    : next_hop (0 = unset)
//     byte  15    : relay_node (0 = unset)
//
// AES-128-CTR nonce (16 bytes):
//     bytes 0-3   : packet_id LE
//     bytes 4-7   : zero
//     bytes 8-11  : from LE
//     bytes 12-15 : zero
//
// Default PSK (psk=0x01 "AQ==" → 16-byte AES-128 key):
//     d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01
//
// LongFast EU868 radio: 869.525 MHz, SF11, BW250, CR4/5, preamble 16, sync 0x2B
// Channel hash 0x08 (Meshtastic channel_hash("LongFast", default_psk)).

#include "mesh_pig.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include "../board/board.h"
#include "../storage/littlefs_ops.h"

#include <RadioLib.h>
#include <M5Unified.h>
#include <mbedtls/aes.h>   // ESP-IDF AES (included with esp32 Arduino)
#include <new>

// ── SX1262 pin mapping for CAP LoRa-1262 on Cardputer-Adv ──────────────────
// NSS=G5, IRQ=G4, RST=G3, BUSY=G6
// The SX1262 shares the SD SPI bus: MOSI=G14, MISO=G39, SCK=G40.
static Module* s_radioModule = nullptr;
static SX1262* s_radioInstance = nullptr;
static bool s_radioInitialized = false;

static bool createRadio() {
    if (s_radioModule && s_radioInstance) return true;
    s_radioModule = new (std::nothrow) Module(
        5, 4, 3, 6, Storage::spiBus(),
        SPISettings(4000000, MSBFIRST, SPI_MODE0));
    if (!s_radioModule) return false;
    s_radioInstance = new (std::nothrow) SX1262(s_radioModule);
    if (!s_radioInstance) {
        delete s_radioModule;
        s_radioModule = nullptr;
        return false;
    }
    return true;
}

static SX1262& radio() {
    return *s_radioInstance;
}

static void releaseRadio() {
    if (s_radioInstance && s_radioInitialized) {
        s_radioInstance->standby();
    }
    if (s_radioInstance) {
        delete s_radioInstance;
        s_radioInstance = nullptr;
    }
    delete s_radioModule;
    s_radioModule = nullptr;
    s_radioInitialized = false;
}

// ── Meshtastic OTA constants ─────────────────────────────────────────────────
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};
static const uint8_t MESH_HEADER_SIZE   = 16;
static const uint8_t MESH_LORA_SYNC     = 0x2B;
static const float   MESH_BW            = 250.0f;
static const uint8_t MESH_SF            = 11;
static const uint8_t MESH_CR            = 5;    // 4/5
static const uint8_t MESH_PREAMBLE      = 16;
static const float   MESH_TX_POWER      = 22.0f;
static const uint32_t MESH_BROADCAST    = 0xFFFFFFFFu;
static const uint32_t NODEINFO_INTERVAL_MS = 3UL * 60UL * 60UL * 1000UL;
static const uint8_t PACKET_HISTORY_SIZE = 32;

struct RegionInfo {
    const char* name;
    uint32_t startKHz;
    uint32_t endKHz;
    uint8_t maxTxPowerDbm;
};

static const RegionInfo MESH_REGIONS[] = {
    {"EU868",   869400, 869650, 27},
    {"US",      902000, 928000, 30},
    {"EU433",   433000, 434000, 10},
    {"CN",      470000, 510000, 19},
    {"JP",      920500, 923500, 13},
    {"ANZ",     915000, 928000, 30},
    {"ANZ433",  433050, 434790, 14},
    {"RU",      868700, 869200, 20},
    {"KR",      920000, 923000, 23},
    {"TW",      920000, 925000, 27},
    {"IN",      865000, 867000, 30},
    {"NZ865",   864000, 868000, 36},
    {"TH",      920000, 925000, 27},
    {"UA433",   433000, 434700, 10},
    {"UA868",   868000, 868600, 14},
    {"MY433",   433000, 435000, 20},
    {"MY919",   919000, 924000, 27},
    {"SG923",   917000, 925000, 20},
    {"PH433",   433000, 434700, 10},
    {"PH868",   868000, 869400, 14},
    {"PH915",   915000, 918000, 24},
    {"KZ433",   433075, 434775, 10},
    {"KZ863",   863000, 868000, 30},
    {"NP865",   865000, 868000, 30},
    {"BR902",   902000, 907500, 30},
};

static_assert(sizeof(MESH_REGIONS) / sizeof(MESH_REGIONS[0]) == MESH_REGION_COUNT,
              "Meshtastic region table and persisted region IDs must match");

static uint32_t channelNameHash(const char* name) {
    uint32_t hash = 5381;
    int c;
    while ((c = (uint8_t)*name++)) hash = ((hash << 5) + hash) + (uint32_t)c;
    return hash;
}

static const RegionInfo* getRegion(uint8_t region) {
    if (region >= MESH_REGION_COUNT) return nullptr;
    return &MESH_REGIONS[region];
}

static uint32_t defaultFrequencyKHz(uint8_t region) {
    const RegionInfo* info = getRegion(region);
    if (!info) return 0;
    const uint32_t bandwidthKHz = (uint32_t)MESH_BW;
    const uint32_t channelCount = (info->endKHz - info->startKHz) / bandwidthKHz;
    if (!channelCount) return 0;
    const char* channelName = Config::mesh().channel;
    if (!channelName[0]) channelName = "LongFast";
    const uint32_t channel = channelNameHash(channelName) % channelCount;
    return info->startKHz + bandwidthKHz / 2 + channel * bandwidthKHz;
}

static bool isFrequencyInRegion(uint8_t region, uint32_t frequency) {
    const RegionInfo* info = getRegion(region);
    if (!info) return false;
    const uint32_t halfBandwidthKHz = (uint32_t)(MESH_BW / 2.0f);
    return frequency >= info->startKHz + halfBandwidthKHz &&
           frequency <= info->endKHz - halfBandwidthKHz;
}

static uint8_t channelHash(const char* channelName, const uint8_t* key, size_t keyLen) {
    uint8_t hash = 0;
    for (const char* p = channelName; *p; ++p) hash ^= (uint8_t)*p;
    for (size_t i = 0; i < keyLen; ++i) hash ^= key[i];
    return hash;
}

// ── Protobuf field tags (MeshPacket & sub-messages) ─────────────────────────
// Meshtastic protobuf field numbers we care about:
// MeshPacket { from=1 to=2 ... decoded=3(Data) ... hop_limit=10 ... id=11 ... }
// Data { portnum=1 payload=2 ... }
// Position { lat=1 lon=2 alt=3 ... }
// User { id=1 longName=2 shortName=3 ... }
// TEXT_MESSAGE_APP portnum = 1
// POSITION_APP     portnum = 3
// NODEINFO_APP     portnum = 4
static const uint8_t PORT_TEXT     = 1;
static const uint8_t PORT_POSITION = 3;
static const uint8_t PORT_NODEINFO = 4;

// ── State ────────────────────────────────────────────────────────────────────
static bool     s_running   = false;
static uint32_t s_selfNum   = 0;
static char     s_selfShort[5] = "PIG";
static uint32_t s_nextNodeInfoMs = 0;
static uint32_t s_lastPacketIds[PACKET_HISTORY_SIZE] = {};
static uint32_t s_lastPacketFrom[PACKET_HISTORY_SIZE] = {};
static uint8_t  s_lastPacketHead = 0;

// ISR flag
static volatile bool s_rxFlag = false;
static void IRAM_ATTR rxISR() { s_rxFlag = true; }

// Node ring (newest overwrites oldest, max 16)
static const uint8_t NODE_MAX = 16;
static MeshNode* s_nodes = nullptr;
static uint8_t  s_nodeHead = 0;   // next write slot
static uint8_t  s_nodeCount = 0;

// Message circular log (max 24)
static const uint8_t MSG_MAX = 24;
static MeshMsg*  s_msgs = nullptr;
static uint8_t  s_msgHead  = 0;
static uint8_t  s_msgCount = 0;
static uint8_t  s_newMsgs  = 0;

static char s_statusLine[48] = "MESH  NO SIGNAL";

static void releaseMeshStorage() {
    releaseRadio();
    delete[] s_nodes;
    s_nodes = nullptr;
    delete[] s_msgs;
    s_msgs = nullptr;
    s_nodeHead = 0;
    s_nodeCount = 0;
    s_msgHead = 0;
    s_msgCount = 0;
    s_newMsgs = 0;
    s_rxFlag = false;
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static bool rememberPacket(uint32_t from, uint32_t id) {
    for (uint8_t i = 0; i < PACKET_HISTORY_SIZE; ++i) {
        if (s_lastPacketFrom[i] == from && s_lastPacketIds[i] == id) return false;
    }
    s_lastPacketFrom[s_lastPacketHead] = from;
    s_lastPacketIds[s_lastPacketHead] = id;
    s_lastPacketHead = (s_lastPacketHead + 1) % PACKET_HISTORY_SIZE;
    return true;
}

static MeshNode* findOrAddNode(uint32_t nodeNum) {
    // find existing
    for (uint8_t i = 0; i < NODE_MAX; i++) {
        if (s_nodes[i].nodeNum == nodeNum) return &s_nodes[i];
    }
    // add new
    uint8_t slot = s_nodeHead;
    s_nodeHead = (s_nodeHead + 1) % NODE_MAX;
    if (s_nodeCount < NODE_MAX) s_nodeCount++;
    s_nodes[slot] = MeshNode{};
    s_nodes[slot].nodeNum = nodeNum;
    return &s_nodes[slot];
}

static int8_t base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decodeChannelKey(const char* encoded, uint8_t key[32], size_t& keyLen) {
    keyLen = 0;
    if (!encoded) return false;
    size_t inputLen = strlen(encoded);
    if (!inputLen || inputLen > 44 || (inputLen & 3) != 0) return false;

    size_t outLen = 0;
    for (size_t i = 0; i < inputLen; i += 4) {
        const bool last = i + 4 == inputLen;
        int8_t a = base64Value(encoded[i]);
        int8_t b = base64Value(encoded[i + 1]);
        if (a < 0 || b < 0) return false;

        const char c2 = encoded[i + 2];
        const char c3 = encoded[i + 3];
        const bool pad2 = c2 == '=';
        const bool pad3 = c3 == '=';
        if ((pad2 && (!pad3 || !last)) || (pad3 && !last)) return false;
        int8_t c = pad2 ? 0 : base64Value(c2);
        int8_t d = pad3 ? 0 : base64Value(c3);
        if (c < 0 || d < 0 || outLen + (pad2 ? 1 : pad3 ? 2 : 3) > 32) return false;
        if ((pad2 && (b & 0x0F)) || (pad3 && !pad2 && (c & 0x03))) return false;

        key[outLen++] = (uint8_t)(((uint8_t)a << 2) | ((uint8_t)b >> 4));
        if (!pad2) key[outLen++] = (uint8_t)(((uint8_t)b << 4) | ((uint8_t)c >> 2));
        if (!pad3) key[outLen++] = (uint8_t)(((uint8_t)c << 6) | (uint8_t)d);
    }

    if (outLen == 1) {
        const uint8_t pskIndex = key[0];
        if (!pskIndex) {
            keyLen = 0;
            return true;
        }
        memcpy(key, MESH_DEFAULT_PSK, sizeof(MESH_DEFAULT_PSK));
        key[sizeof(MESH_DEFAULT_PSK) - 1] =
            (uint8_t)(key[sizeof(MESH_DEFAULT_PSK) - 1] + pskIndex - 1);
        keyLen = sizeof(MESH_DEFAULT_PSK);
        return true;
    }
    if (outLen < 2 || outLen > 32) return false;
    keyLen = outLen <= 16 ? 16 : 32;
    if (outLen < keyLen) memset(key + outLen, 0, keyLen - outLen);
    return true;
}

static bool getChannelKey(uint8_t key[32], size_t& keyLen) {
    return decodeChannelKey(Config::mesh().psk, key, keyLen);
}

static bool getChannelHash(uint8_t& hash) {
    uint8_t key[32];
    size_t keyLen = 0;
    if (!getChannelKey(key, keyLen)) return false;

    const char* channelName = Config::mesh().channel;
    if (!channelName[0]) channelName = "LongFast";
    hash = channelHash(channelName, key, keyLen);
    return true;
}

// ── Minimal AES-128-CTR (via mbedtls) ────────────────────────────────────────
static bool aes_ctr(const uint8_t* key, size_t keyLen, uint32_t packetId,
                    uint32_t fromNode, const uint8_t* in, uint8_t* out, size_t len) {
    if (keyLen == 0) {
        memcpy(out, in, len);
        return true;
    }
    if (keyLen != 16 && keyLen != 32) return false;

    // Build 16-byte nonce
    uint8_t nonce[16] = {};
    nonce[0] = packetId & 0xFF;
    nonce[1] = (packetId >> 8) & 0xFF;
    nonce[2] = (packetId >> 16) & 0xFF;
    nonce[3] = (packetId >> 24) & 0xFF;
    // bytes 4-7 = 0
    nonce[8]  = fromNode & 0xFF;
    nonce[9]  = (fromNode >> 8) & 0xFF;
    nonce[10] = (fromNode >> 16) & 0xFF;
    nonce[11] = (fromNode >> 24) & 0xFF;
    // bytes 12-15 = 0

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int status = mbedtls_aes_setkey_enc(&ctx, key, (unsigned int)(keyLen * 8));
    if (status != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }

    uint8_t stream[16];
    size_t nc_off = 0;
    uint8_t nc[16];
    memcpy(nc, nonce, 16);

    status = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nc, stream, in, out);
    mbedtls_aes_free(&ctx);
    return status == 0;
}

// ── Minimal protobuf varint / wire readers ───────────────────────────────────
struct PbReader {
    const uint8_t* buf;
    size_t         pos;
    size_t         end;
    bool           failed;

    PbReader(const uint8_t* b, size_t len) : buf(b), pos(0), end(len), failed(false) {}

    bool valid() const { return !failed; }

    bool readVarint(uint64_t& value) {
        value = 0;
        for (uint8_t i = 0; i < 10; ++i) {
            if (pos >= end) {
                failed = true;
                return false;
            }
            uint8_t b = buf[pos++];
            if (i == 9 && (b & 0xFE)) {
                failed = true;
                return false;
            }
            value |= (uint64_t)(b & 0x7F) << (i * 7);
            if (!(b & 0x80)) return true;
        }
        failed = true;
        return false;
    }

    // read field tag → returns false at end
    bool readTag(uint8_t& fieldNum, uint8_t& wireType) {
        if (failed || pos == end) return false;
        uint64_t tag = 0;
        if (!readVarint(tag) || (tag >> 3) == 0 || (tag >> 3) > UINT8_MAX) {
            failed = true;
            return false;
        }
        fieldNum = (uint8_t)(tag >> 3);
        wireType = (uint8_t)(tag & 0x07);
        if (wireType != 0 && wireType != 1 && wireType != 2 && wireType != 5) {
            failed = true;
            return false;
        }
        return true;
    }

    bool skipField(uint8_t wireType) {
        if (wireType == 0) {
            uint64_t ignored;
            return readVarint(ignored);
        }
        size_t length = wireType == 1 ? 8 : wireType == 5 ? 4 : 0;
        if (wireType == 2) {
            uint64_t encodedLength = 0;
            if (!readVarint(encodedLength) || encodedLength > end - pos) {
                failed = true;
                return false;
            }
            length = (size_t)encodedLength;
        } else if (wireType != 1 && wireType != 5) {
            failed = true;
            return false;
        }
        if (length > end - pos) {
            failed = true;
            return false;
        }
        pos += length;
        return true;
    }

    // read length-delimited bytes, return pointer + length
    const uint8_t* readBytes(size_t& outLen) {
        uint64_t encodedLength = 0;
        outLen = 0;
        if (!readVarint(encodedLength) || encodedLength > end - pos) {
            failed = true;
            return nullptr;
        }
        outLen = (size_t)encodedLength;
        const uint8_t* p = buf + pos;
        pos += outLen;
        return p;
    }

    void readString(char* dst, size_t maxLen) {
        if (!maxLen) return;
        size_t l = 0;
        const uint8_t* p = readBytes(l);
        if (!p) return;
        size_t n = (l < maxLen - 1) ? l : maxLen - 1;
        memcpy(dst, p, n); dst[n] = '\0';
    }
};

// ── Protobuf writer ──────────────────────────────────────────────────────────
struct PbWriter {
    uint8_t* buf;
    size_t   pos;
    size_t   cap;
    bool     failed;

    PbWriter(uint8_t* b, size_t c) : buf(b), pos(0), cap(c), failed(false) {}

    void writeVarint(uint64_t v) {
        while (!failed) {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            if (pos >= cap) {
                failed = true;
                return;
            }
            buf[pos++] = b;
            if (!v) return;
        }
    }

    void writeTag(uint8_t fieldNum, uint8_t wireType) {
        writeVarint(((uint64_t)fieldNum << 3) | wireType);
    }

    void writeU32(uint8_t field, uint32_t v) {
        writeTag(field, 0); writeVarint(v);
    }
    void writeI32(uint8_t field, int32_t v) {
        writeTag(field, 0); writeVarint((uint64_t)(uint32_t)v);
    }
    void writeBytes(uint8_t field, const uint8_t* b, size_t l) {
        writeTag(field, 2); writeVarint(l);
        if (failed || l > cap - pos) {
            failed = true;
            return;
        }
        memcpy(buf + pos, b, l);
        pos += l;
    }
    void writeStr(uint8_t field, const char* s) {
        writeBytes(field, (const uint8_t*)s, strlen(s));
    }

    // Begin a nested message: write tag+length later (patch after)
    // Simple approach: return position, write 0 len, fill, then patch
    size_t beginMsg(uint8_t field) {
        writeTag(field, 2);
        size_t lenPos = pos;
        pos++; // placeholder (1 byte varint for length ≤127)
        return lenPos;
    }
    void endMsg(size_t lenPos) {
        size_t msgLen = pos - lenPos - 1;
        buf[lenPos] = (uint8_t)msgLen; // works as long as msgLen < 128
    }
};

// ── Handle decoded Position payload ─────────────────────────────────────────
static void handlePosition(uint32_t from, int8_t snr, int16_t rssi,
                            const uint8_t* payload, size_t len) {
    PbReader r(payload, len);
    int32_t lat = 0, lon = 0; int16_t alt = 0;
    uint8_t fn, wt;
    while (r.readTag(fn, wt)) {
        if (fn == 1 && wt == 0) {
            uint64_t value = 0;
            if (!r.readVarint(value)) break;
            lat = (int32_t)value;
        } else if (fn == 2 && wt == 0) {
            uint64_t value = 0;
            if (!r.readVarint(value)) break;
            lon = (int32_t)value;
        } else if (fn == 3 && wt == 0) {
            uint64_t value = 0;
            if (!r.readVarint(value)) break;
            alt = (int16_t)value;
        } else if (!r.skipField(wt)) {
            break;
        }
    }
    if (!r.valid()) return;
    MeshNode* n = findOrAddNode(from);
    n->lat_i    = lat;
    n->lon_i    = lon;
    n->alt      = alt;
    n->snr      = snr;
    n->rssi     = rssi;
    n->lastSeen = (uint32_t)(millis() / 1000);
    n->hasPos   = true;
}

// ── Handle decoded User (NodeInfo) payload ───────────────────────────────────
static void handleNodeInfo(uint32_t from, int8_t snr, int16_t rssi,
                            const uint8_t* payload, size_t len) {
    PbReader r(payload, len);
    char shortN[5] = {}, longN[20] = {};
    uint8_t fn, wt;
    while (r.readTag(fn, wt)) {
        if (fn == 3 && wt == 2) r.readString(shortN, sizeof(shortN));
        else if (fn == 2 && wt == 2) r.readString(longN, sizeof(longN));
        else if (!r.skipField(wt)) break;
    }
    if (!r.valid()) return;
    MeshNode* n = findOrAddNode(from);
    if (shortN[0]) strncpy(n->shortName, shortN, sizeof(n->shortName) - 1);
    if (longN[0])  strncpy(n->longName,  longN,  sizeof(n->longName) - 1);
    n->snr      = snr;
    n->rssi     = rssi;
    n->lastSeen = (uint32_t)(millis() / 1000);

    // Detect self: if nodeNum was set during start() we know our ID
    if (s_selfNum && from == s_selfNum) {
        n->isSelf = true;
        if (shortN[0]) strncpy(s_selfShort, shortN, sizeof(s_selfShort) - 1);
    }
}

// ── Handle decoded Text Message payload ─────────────────────────────────────
static void handleText(uint32_t from, int8_t snr, int16_t rssi,
                       bool broadcast, const char* text) {
    uint8_t slot = s_msgHead;
    s_msgHead = (s_msgHead + 1) % MSG_MAX;
    if (s_msgCount < MSG_MAX) s_msgCount++;

    MeshMsg& m = s_msgs[slot];
    m = MeshMsg{};
    m.from      = from;
    m.time      = (uint32_t)(millis() / 1000);
    m.broadcast = broadcast;
    strncpy(m.text, text, sizeof(m.text) - 1);

    // fill from_name from known nodes
    for (uint8_t i = 0; i < NODE_MAX; i++) {
        if (s_nodes[i].nodeNum == from && s_nodes[i].shortName[0]) {
            strncpy(m.fromName, s_nodes[i].shortName, sizeof(m.fromName) - 1);
            break;
        }
    }
    if (!m.fromName[0]) {
        snprintf(m.fromName, sizeof(m.fromName), "%04X", (unsigned)(from & 0xFFFF));
    }
    if (s_newMsgs < UINT8_MAX) s_newMsgs++;
    SFX::play(SFX::MENU_CLICK);
}

// ── Decode received LoRa packet ──────────────────────────────────────────────
static void decodePacket(const uint8_t* raw, size_t rawLen, int8_t snr, int16_t rssi) {
    if (rawLen <= MESH_HEADER_SIZE) return;

    // Parse 16-byte header (little-endian)
    uint32_t to   = (uint32_t)raw[0] | ((uint32_t)raw[1]<<8) | ((uint32_t)raw[2]<<16) | ((uint32_t)raw[3]<<24);
    uint32_t from = (uint32_t)raw[4] | ((uint32_t)raw[5]<<8) | ((uint32_t)raw[6]<<16) | ((uint32_t)raw[7]<<24);
    uint32_t pid  = (uint32_t)raw[8] | ((uint32_t)raw[9]<<8) | ((uint32_t)raw[10]<<16)| ((uint32_t)raw[11]<<24);
    uint8_t  chHash = raw[13];

    uint8_t expectedHash = 0;
    uint8_t key[32];
    size_t keyLen = 0;
    if (!from || !pid || !getChannelKey(key, keyLen) ||
        !getChannelHash(expectedHash) || chHash != expectedHash ||
        !rememberPacket(from, pid)) return;
    const bool broadcast = to == MESH_BROADCAST;

    // Decrypt payload
    const uint8_t* ciphertext = raw + MESH_HEADER_SIZE;
    size_t         cipherLen  = rawLen - MESH_HEADER_SIZE;

    uint8_t plainBuf[256];
    if (cipherLen > sizeof(plainBuf)) cipherLen = sizeof(plainBuf);
    if (!aes_ctr(key, keyLen, pid, from, ciphertext, plainBuf, cipherLen)) return;

    // Parse outer MeshPacket (decoded = field 3, which is a Data message)
    // But in the OTA format, the encrypted payload IS the Data submessage directly.
    // Data { portnum=1 payload=2 }
    PbReader r(plainBuf, cipherLen);
    uint8_t portnum = 0;
    const uint8_t* innerPayload = nullptr;
    size_t innerLen = 0;

    uint8_t fn, wt;
    while (r.readTag(fn, wt)) {
        if (fn == 1 && wt == 0) {
            uint64_t value = 0;
            if (!r.readVarint(value)) break;
            portnum = (uint8_t)value;
        } else if (fn == 2 && wt == 2) {
            innerPayload = r.readBytes(innerLen);
        } else if (!r.skipField(wt)) {
            break;
        }
    }

    if (!r.valid()) return;
    if (!innerPayload || innerLen == 0) return;

    if (portnum == PORT_TEXT) {
        // text is raw string in payload
        char text[48] = {};
        size_t n = innerLen < sizeof(text)-1 ? innerLen : sizeof(text)-1;
        memcpy(text, innerPayload, n);
        handleText(from, snr, rssi, broadcast, text);
    } else if (portnum == PORT_POSITION) {
        handlePosition(from, snr, rssi, innerPayload, innerLen);
    } else if (portnum == PORT_NODEINFO) {
        handleNodeInfo(from, snr, rssi, innerPayload, innerLen);
    }
    // Update lastSeen regardless
    MeshNode* n = findOrAddNode(from);
    if (!n->shortName[0]) {
        snprintf(n->shortName, sizeof(n->shortName), "%04X", (unsigned)(from & 0xFFFF));
    }
    n->lastSeen = (uint32_t)(millis() / 1000);
    n->snr  = snr;
    n->rssi = rssi;
}

// ── Build + send a Meshtastic text packet ────────────────────────────────────
static bool txData(uint32_t portnum, const uint8_t* payload, size_t payloadLen) {
    // Data { portnum = 1, payload = 2 }
    uint8_t dataBuf[128];
    PbWriter dw(dataBuf, sizeof(dataBuf));
    dw.writeU32(1, portnum);
    dw.writeBytes(2, payload, payloadLen);
    if (dw.failed) return false;
    size_t dataLen = dw.pos;

    // IDs must stay unique with this sender/key pair across reboot cycles.
    uint32_t pid = esp_random();
    if (!pid) pid = 1;
    uint32_t from = s_selfNum ? s_selfNum : 0xDEADC0DEu;
    uint8_t key[32];
    size_t keyLen = 0;
    uint8_t hash = 0;
    if (!getChannelKey(key, keyLen) || !getChannelHash(hash)) return false;
    uint8_t encrypted[128];
    if (dataLen > sizeof(encrypted)) return false;
    if (!aes_ctr(key, keyLen, pid, from, dataBuf, encrypted, dataLen)) return false;

    // Build the Meshtastic packet header.
    uint8_t header[MESH_HEADER_SIZE] = {};
    header[0] = 0xFF; header[1] = 0xFF; header[2] = 0xFF; header[3] = 0xFF;
    header[4]=(from)&0xFF; header[5]=(from>>8)&0xFF; header[6]=(from>>16)&0xFF; header[7]=(from>>24)&0xFF;
    header[8]=(pid)&0xFF; header[9]=(pid>>8)&0xFF; header[10]=(pid>>16)&0xFF; header[11]=(pid>>24)&0xFF;
    header[12] = (uint8_t)((3 << 5) | 3);
    header[13] = hash;
    header[14] = 0;
    header[15] = 0;

    uint8_t pkt[16 + 128];
    memcpy(pkt, header, MESH_HEADER_SIZE);
    memcpy(pkt + MESH_HEADER_SIZE, encrypted, dataLen);
    size_t pktLen = MESH_HEADER_SIZE + dataLen;

    int state = radio().transmit(pkt, (size_t)pktLen);
    const bool sent = state == RADIOLIB_ERR_NONE;
    radio().startReceive();
    if (!sent) log_w("[MeshPig] transmit failed: %d", state);
    return sent;
}

static bool txText(const char* text) {
    return text && txData(PORT_TEXT, (const uint8_t*)text, strlen(text));
}

static bool txNodeInfo() {
    char nodeId[10];
    snprintf(nodeId, sizeof(nodeId), "!%08x", (unsigned)s_selfNum);
    uint8_t userBuf[64];
    PbWriter user(userBuf, sizeof(userBuf));
    user.writeStr(1, nodeId);
    user.writeStr(2, s_selfShort);
    user.writeStr(3, s_selfShort);
    if (user.failed) return false;
    return txData(PORT_NODEINFO, userBuf, user.pos);
}

// ── Public API ───────────────────────────────────────────────────────────────
namespace MeshPig {

void start() {
    if (s_running) return;
    if (!Board::isAdv()) {
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH ADV ONLY");
        log_w("[MeshPig] SX1262 mesh requires Cardputer ADV");
        return;
    }
    uint8_t channelKey[32];
    size_t channelKeyLen = 0;
    if (!getChannelKey(channelKey, channelKeyLen)) {
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH KEY INVALID");
        log_w("[MeshPig] invalid channel PSK; use AQ==, AA==, or 16/32-byte Base64 key");
        return;
    }
    const MeshConfig& mc = Config::mesh();
    const RegionInfo* region = getRegion(mc.region);
    const uint32_t frequency = mc.frequencyOverride ? mc.freqKHz : defaultFrequencyKHz(mc.region);
    if (!region || !isFrequencyInRegion(mc.region, frequency)) {
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH FREQ INVALID");
        log_w("[MeshPig] invalid region/frequency: region=%u freq=%lu kHz",
              (unsigned)mc.region, (unsigned long)frequency);
        return;
    }
    if (!s_nodes) s_nodes = new (std::nothrow) MeshNode[NODE_MAX]();
    if (!s_msgs) s_msgs = new (std::nothrow) MeshMsg[MSG_MAX]();
    if (!s_nodes || !s_msgs || !createRadio()) {
        releaseMeshStorage();
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH LOW MEM");
        log_w("[MeshPig] unable to allocate radio state");
        return;
    }

    const float freqMHz = frequency / 1000.0f;
    const float txPower = MESH_TX_POWER < region->maxTxPowerDbm
        ? MESH_TX_POWER : (float)region->maxTxPowerDbm;

    // Storage initializes the shared ADV SPI bus during boot.
    int state = radio().begin(
        freqMHz,
        MESH_BW,
        MESH_SF,
        MESH_CR,
        MESH_LORA_SYNC,
        txPower,
        MESH_PREAMBLE
    );

    if (state != RADIOLIB_ERR_NONE) {
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH RADIO ERR");
        log_w("[MeshPig] radio.begin failed: %d", state);
        releaseMeshStorage();
        return;
    }
    s_radioInitialized = true;

    radio().setCurrentLimit(140.0f);
    radio().setPacketReceivedAction(rxISR);

    // Generate a node ID from ESP32 MAC if we don't have one saved
    if (!s_selfNum) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        s_selfNum = (uint32_t)mac[2]<<24 | (uint32_t)mac[3]<<16 |
                    (uint32_t)mac[4]<<8  | (uint32_t)mac[5];
    }

    // Apply shortName from config
    if (mc.shortName[0]) {
        strncpy(s_selfShort, mc.shortName, sizeof(s_selfShort) - 1);
        s_selfShort[sizeof(s_selfShort) - 1] = '\0';
    }

    s_rxFlag  = false;
    memset(s_lastPacketIds, 0, sizeof(s_lastPacketIds));
    memset(s_lastPacketFrom, 0, sizeof(s_lastPacketFrom));
    s_lastPacketHead = 0;
    s_running = true;

    state = radio().startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        log_w("[MeshPig] startReceive failed: %d", state);
        s_running = false;
        snprintf(s_statusLine, sizeof(s_statusLine), "MESH RX ERR");
        releaseMeshStorage();
        return;
    }

    snprintf(s_statusLine, sizeof(s_statusLine), "MESH LISTENING");
    s_nextNodeInfoMs = millis() + 5000 + (esp_random() % 15000);
    log_i("[MeshPig] started. NodeNum=0x%08X short=%s", s_selfNum, s_selfShort);
}

void stop() {
    if (!s_running && !s_radioInstance && !s_nodes && !s_msgs) return;
    s_running = false;
    releaseMeshStorage();
    snprintf(s_statusLine, sizeof(s_statusLine), "MESH OFF");
}

void requestNodeInfo() {
    if (s_running) s_nextNodeInfoMs = 0;
}

bool validateChannelKey(const char* base64Key) {
    uint8_t key[32];
    size_t keyLen = 0;
    return decodeChannelKey(base64Key, key, keyLen);
}

uint8_t regionCount() {
    return MESH_REGION_COUNT;
}

const char* regionName(uint8_t region) {
    const RegionInfo* info = getRegion(region);
    return info ? info->name : "INVALID";
}

uint32_t frequencyKHz() {
    const MeshConfig& mc = Config::mesh();
    return mc.frequencyOverride ? mc.freqKHz : defaultFrequencyKHz(mc.region);
}

bool validateFrequency(uint8_t region, uint32_t frequency) {
    return isFrequencyInRegion(region, frequency);
}

void update() {
    if (!s_running) return;
    if (!s_nextNodeInfoMs || (int32_t)(millis() - s_nextNodeInfoMs) >= 0) {
        s_nextNodeInfoMs = millis() + NODEINFO_INTERVAL_MS;
        if (!txNodeInfo()) log_w("[MeshPig] NodeInfo announcement failed");
    }
    if (!s_rxFlag) return;
    s_rxFlag = false;

    uint8_t buf[256];
    size_t  rxLen = radio().getPacketLength();
    if (rxLen == 0 || rxLen > sizeof(buf)) {
        radio().startReceive();
        return;
    }

    int state = radio().readData(buf, rxLen);
    if (state == RADIOLIB_ERR_NONE) {
        int8_t  snr  = (int8_t)radio().getSNR();
        int16_t rssi = (int16_t)radio().getRSSI();
        decodePacket(buf, rxLen, snr, rssi);

        // Update status line
        snprintf(s_statusLine, sizeof(s_statusLine),
                 "MESH %u NODES %u MSG%s",
                 (unsigned)s_nodeCount,
                 (unsigned)s_msgCount,
                 s_newMsgs ? " NEW!" : "");
    }

    radio().startReceive();
}

bool isRunning() { return s_running; }

uint8_t nodeCount() { return s_nodeCount; }
MeshNode* nodeAt(uint8_t i) {
    if (!s_nodes || i >= s_nodeCount) return nullptr;
    // Return newest first: walk back from head
    int8_t slot = (int8_t)s_nodeHead - 1 - i;
    while (slot < 0) slot += NODE_MAX;
    return &s_nodes[slot];
}

uint8_t msgCount()  { return s_msgCount; }
MeshMsg* msgAt(uint8_t i) {
    if (!s_msgs || i >= s_msgCount) return nullptr;
    int8_t slot = (int8_t)s_msgHead - 1 - i;
    while (slot < 0) slot += MSG_MAX;
    return &s_msgs[slot];
}
uint8_t  newMsgCount() { return s_newMsgs; }
void     clearNew()    { s_newMsgs = 0; }

uint32_t    selfNodeNum()   { return s_selfNum; }
const char* selfShortName() { return s_selfShort; }

bool sendText(const char* text) {
    if (!s_running || !text || !text[0]) return false;
    if (!txText(text)) return false;

    uint8_t slot = s_msgHead;
    s_msgHead = (s_msgHead + 1) % MSG_MAX;
    if (s_msgCount < MSG_MAX) s_msgCount++;
    MeshMsg& message = s_msgs[slot];
    message = MeshMsg{};
    message.from = s_selfNum;
    message.time = (uint32_t)(millis() / 1000);
    message.broadcast = true;
    strncpy(message.text, text, sizeof(message.text) - 1);
    strncpy(message.fromName, s_selfShort, sizeof(message.fromName) - 1);
    return true;
}

const char* getStatusLine() { return s_statusLine; }

}  // namespace MeshPig
