// mesh_pig.cpp — MODEM. SX1262, packets, start/stop.
// Messages and the keymap live in mesh_text.cpp.
// The screen, tabs, and keys live in mesh_ui.cpp.
// Custom frequency: set freqOverride=true in config, freqKHz = your freq.

#include "mesh_pig.h"
#include "mesh_priv.h"
#include "../board/board.h"
#include "../core/config.h"
#include "../storage/littlefs_ops.h"
#include "../ui/display.h"

#include <RadioLib.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <utility/PI4IOE5V6408_Class.hpp>
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>
#include <mbedtls/cipher.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include <new>

// ── SX1262 pins for CAP LoRa-1262 on Cardputer-ADV ─────────────────────────
static Module* s_radioModule = nullptr;
static SX1262* s_radio = nullptr;
static m5::PI4IOE5V6408_Class s_rfEnableExpander(0x43, 400000, &m5::In_I2C);
bool s_rfExpanderReady = false;

// ════════════════════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════════════════════
// MESHTASTIC CONSTANTS
// ════════════════════════════════════════════════════════════════════════════
static constexpr uint8_t  HDR_SZ        = 16;
static constexpr uint8_t  HOP_LIMIT_DEF = 3;
static constexpr uint8_t  FLAGS_HOP_MASK    = 0x07;
static constexpr uint8_t  FLAGS_WANT_ACK    = 0x08;
static constexpr uint8_t  FLAGS_HOPSTART_SHF = 5;
static constexpr uint8_t  PORT_TEXT         = 1;
static constexpr uint8_t  PORT_POSITION     = 3;
static constexpr uint8_t  PORT_NODEINFO     = 4;
static constexpr uint8_t  PORT_ROUTING      = 5;

// ════════════════════════════════════════════════════════════════════════════
// PSK
// ════════════════════════════════════════════════════════════════════════════
static const uint8_t MESH_DEFAULT_KEY[16] = {
    0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,
    0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01
};

static uint8_t b64Decode(const char* src, uint8_t* dst, uint8_t cap) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    uint8_t out=0; uint32_t buf=0; uint8_t bits=0;
    for (const char* p=src; *p && *p!='='; p++) {
        int8_t v=T[(uint8_t)*p]; if(v<0) return 0xFF;
        buf=(buf<<6)|(uint8_t)v; bits+=6;
        if(bits>=8){ if(out>=cap) return 0xFF; bits-=8; dst[out++]=(buf>>bits)&0xFF; }
    }
    return out;
}

static bool expandPsk(const char* b64, uint8_t out[32], uint8_t& keyLen) {
    uint8_t raw[32] = {};
    keyLen = b64Decode(b64, raw, sizeof(raw));
    if (keyLen == 0xFF) return false;
    if (keyLen == 0) return true;
    if (keyLen == 1) {
        if (raw[0] == 0) {
            keyLen = 0;
            return true;
        }
        memcpy(out, MESH_DEFAULT_KEY, sizeof(MESH_DEFAULT_KEY));
        out[15] = (uint8_t)(out[15] + raw[0] - 1);
        keyLen = 16;
        return true;
    }
    if (keyLen < 16) {
        memset(out, 0, 16);
        memcpy(out, raw, keyLen);
        keyLen = 16;
        return true;
    }
    if (keyLen > 16 && keyLen < 32) {
        memset(out, 0, 32);
        memcpy(out, raw, keyLen);
        keyLen = 32;
        return true;
    }
    if (keyLen == 16 || keyLen == 32) {
        memcpy(out, raw, keyLen);
        return true;
    }
    return false;
}

static uint8_t channelHash(const char* name, const uint8_t* key, uint8_t keyLen) {
    uint8_t hash = 0;
    for (const char* p = name; *p; ++p) hash ^= (uint8_t)*p;
    for (uint8_t i = 0; i < keyLen; ++i) hash ^= key[i];
    return hash;
}

// ════════════════════════════════════════════════════════════════════════════
// AES-128-CTR
// ════════════════════════════════════════════════════════════════════════════
static bool aesCtr(const uint8_t* key, uint8_t keyLen, uint32_t pktId, uint32_t from,
                   const uint8_t* in, uint8_t* out, size_t len) {
    if (keyLen == 0) {
        if (out != in) memcpy(out, in, len);
        return true;
    }
    uint8_t nonce[16]={};
    nonce[0]= pktId     &0xFF; nonce[1]=(pktId>>8) &0xFF;
    nonce[2]=(pktId>>16)&0xFF; nonce[3]=(pktId>>24)&0xFF;
    nonce[8]= from      &0xFF; nonce[9]=(from>>8)  &0xFF;
    nonce[10]=(from>>16)&0xFF; nonce[11]=(from>>24)&0xFF;
    mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx,key,(uint32_t)keyLen*8)!=0){mbedtls_aes_free(&ctx);return false;}
    uint8_t sb[16]={}; size_t nc=0;
    int r=mbedtls_aes_crypt_ctr(&ctx,len,&nc,nonce,sb,in,out);
    mbedtls_aes_free(&ctx); return r==0;
}

// ════════════════════════════════════════════════════════════════════════════
// PROTOBUF (minimal)
// ════════════════════════════════════════════════════════════════════════════
static constexpr uint8_t WT_VARINT=0, WT_LEN=2;

struct PbW {
    uint8_t* b; uint16_t cap, len=0; bool ok=true;
    PbW(uint8_t* buffer, uint16_t capacity) : b(buffer), cap(capacity) {}
    void vi(uint8_t f, uint64_t v){ wr((f<<3)|WT_VARINT); while(v>=0x80){wr((v&0x7F)|0x80);v>>=7;} wr((uint8_t)v); }
    void by(uint8_t f, const uint8_t* d, uint8_t s){ wr((f<<3)|WT_LEN); wv(s); for(uint8_t i=0;i<s;i++) wr(d[i]); }
    void st(uint8_t f, const char* s){ by(f,(const uint8_t*)s,(uint8_t)strlen(s)); }
private:
    void wr(uint8_t v){ if(len<cap) b[len++]=v; else ok=false; }
    void wv(uint32_t v){ while(v>=0x80){wr((v&0x7F)|0x80);v>>=7;} wr((uint8_t)v); }
};

static constexpr uint8_t WT_64=1, WT_32=5;

struct PbR {
    const uint8_t* b; uint16_t len, pos=0;
    PbR(const uint8_t* buffer, uint16_t size) : b(buffer), len(size) {}
    bool done() const { return pos>=len; }
    bool tag(uint8_t& f, uint8_t& wt){ uint64_t v; if(!vi(v)) return false; f=(uint8_t)(v>>3); wt=(uint8_t)(v&7); return true; }
    bool vi(uint64_t& v){ v=0; int s=0; while(pos<len){ uint8_t x=b[pos++]; v|=(uint64_t)(x&0x7F)<<s; if(!(x&0x80)) return true; s+=7; if(s>63) return false; } return false; }
    bool ld(const uint8_t*& d, uint16_t& s){ uint64_t v; if(!vi(v)) return false; if(pos+v>len) return false; d=b+pos; s=(uint16_t)v; pos+=s; return true; }
    bool fixed32(uint32_t& v) {
        if (len - pos < 4) return false;
        v = (uint32_t)b[pos] | ((uint32_t)b[pos + 1] << 8) |
            ((uint32_t)b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 24);
        pos += 4;
        return true;
    }
    bool skip(uint8_t wt){
        if(wt==WT_VARINT){uint64_t v;return vi(v);}
        if(wt==WT_LEN){const uint8_t*d;uint16_t s;return ld(d,s);}
        if(wt==WT_64){if(len-pos<8)return false;pos+=8;return true;}
        if(wt==WT_32){if(len-pos<4)return false;pos+=4;return true;}
        return false;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// DEDUPLICATION
// ════════════════════════════════════════════════════════════════════════════
struct PktRecord { uint32_t id, sender; };
static constexpr uint8_t PACKET_HISTORY_SIZE = 100;
static PktRecord* s_hist=nullptr; static uint8_t s_hHead=0, s_hCnt=0;
static bool seenRecently(uint32_t sndr, uint32_t id){
    for(uint8_t i=0;i<s_hCnt;i++){ uint8_t x=(s_hHead+PACKET_HISTORY_SIZE-1-i)%PACKET_HISTORY_SIZE; if(s_hist[x].id==id&&s_hist[x].sender==sndr) return true; } return false;
}
static void markSeen(uint32_t sndr, uint32_t id){
    s_hist[s_hHead]={id,sndr}; s_hHead=(s_hHead+1)%PACKET_HISTORY_SIZE; if(s_hCnt<PACKET_HISTORY_SIZE) s_hCnt++;
}

// ════════════════════════════════════════════════════════════════════════════
// TX PRIORITY QUEUE
// ════════════════════════════════════════════════════════════════════════════
enum TxPri : uint8_t { PRI_ACK=0, PRI_ROUTING=1, PRI_DEFAULT=4 };
struct TxPkt { uint8_t data[256]; uint16_t len; TxPri prio; };
static TxPkt* s_txQ=nullptr; uint8_t s_txN=0;
static bool txEnq(const uint8_t* d, uint16_t l, TxPri p){
    if(s_txN>=16||l>sizeof(s_txQ[0].data)) return false;
    uint8_t ins=s_txN;
    for(uint8_t i=0;i<s_txN;i++) if(p<s_txQ[i].prio){ins=i;break;}
    for(uint8_t i=s_txN;i>ins;i--) s_txQ[i]=s_txQ[i-1];
    memcpy(s_txQ[ins].data,d,l); s_txQ[ins].len=l; s_txQ[ins].prio=p; s_txN++; return true;
}
static bool txDeq(TxPkt& p){ if(!s_txN) return false; p=s_txQ[0]; memmove(s_txQ,s_txQ+1,sizeof(TxPkt)*(s_txN-1)); s_txN--; return true; }


// ════════════════════════════════════════════════════════════════════════════
// NODE DB
// ════════════════════════════════════════════════════════════════════════════
MeshNode* s_nodes=nullptr; uint8_t s_nodeN=0;
static MeshNode* nodeFindOrAdd(uint32_t num){
    for(uint8_t i=0;i<s_nodeN;i++) if(s_nodes[i].nodeNum==num) return &s_nodes[i];
    if(s_nodeN<16){ s_nodes[s_nodeN]={}; s_nodes[s_nodeN].nodeNum=num; return &s_nodes[s_nodeN++]; }
    uint8_t old=0; uint32_t oldMs=UINT32_MAX;
    for(uint8_t i=0;i<s_nodeN;i++) if(!s_nodes[i].isSelf&&s_nodes[i].lastSeen<oldMs){oldMs=s_nodes[i].lastSeen;old=i;}
    s_nodes[old]={}; s_nodes[old].nodeNum=num; return &s_nodes[old];
}

// ════════════════════════════════════════════════════════════════════════════
// SD — /PigMesh holds the key, the node list, the channel chat, and DMs
// ════════════════════════════════════════════════════════════════════════════
static const char* const PKI_PATH       = "/PigMesh/pki.bin";
static const char* const PKI_PATH_OLD   = "/0N3P0rK/mesh/pki.bin";
static const char* const NODES_PATH     = "/PigMesh/nodes.bin";
static const char* const NODES_PATH_OLD = "/0N3P0rK/mesh/nodes.bin";
static const char* const CHAT_PATH      = "/PigMesh/chat.bin";

// Live files are only under /PigMesh. The old /0N3P0rK/mesh copies are read
// once when the new file is missing, then left alone. GPS, handshakes, and
// the other /0N3P0rK folders are not mesh data.
static constexpr uint32_t NODES_MAGIC    = 0x50494731u; // 'PIG1'
static constexpr uint32_t NODES_MAGIC_V0 = 0x4E4F4445u; // 'NODE'
static constexpr uint32_t PKI_MAGIC   = 0x504B4921u; // 'PKI!'
static constexpr uint32_t CHAT_MAGIC  = 0x43484131u; // 'CHA1'
static constexpr uint32_t DM_MAGIC    = 0x444D4731u; // 'DMG1'

static bool     s_chatDirty = false;
static bool     s_dmDirty   = false;
static bool     s_nodesDirty = false;
static uint8_t  s_storeHash = 0;
static uint8_t  s_storeRegion = 0;
static uint8_t  s_storeModem = 0;
static char     s_storeChannel[20] = {};

static void sdIdle() { digitalWrite(12, HIGH); }

static void meshEnsureDir() {
    if (!Storage::available()) return;
    sdIdle();
    if (!SD.exists("/PigMesh")) SD.mkdir("/PigMesh");
    if (!SD.exists("/PigMesh/dm")) SD.mkdir("/PigMesh/dm");
    sdIdle();
}

static void saveNodes() {
    s_nodesDirty = false;
    if (!Storage::available() || !s_nodes || !s_nodeN) return;
    meshEnsureDir();
    sdIdle();
    File f = SD.open(NODES_PATH, FILE_WRITE);
    if (!f) { sdIdle(); return; }
    f.write((const uint8_t*)&NODES_MAGIC, 4);
    f.write(s_nodeN);
    f.write((const uint8_t*)s_nodes, (size_t)s_nodeN * sizeof(MeshNode));
    f.close();
    sdIdle();
}

// Previous node record, before pubKey. Same layout the old file was written with.
struct MeshNodeV0 {
    uint32_t nodeNum;
    int32_t  lat_i;
    int32_t  lon_i;
    int16_t  alt;
    int8_t   snr;
    int16_t  rssi;
    uint32_t lastSeen;
    char     shortName[5];
    char     longName[20];
    bool     hasPos;
    bool     isSelf;
};

static bool keepNode(const MeshNode& in) {
    if (in.isSelf || in.nodeNum == 0 || s_nodeN >= 16) return false;
    for (uint8_t j = 0; j < s_nodeN; j++)
        if (s_nodes[j].nodeNum == in.nodeNum) return false;
    MeshNode tmp = in;
    tmp.shortName[4] = 0;
    tmp.longName[19] = 0;
    tmp.lastSeen = 0;
    s_nodes[s_nodeN++] = tmp;
    return true;
}

// Reads one file. A matching magic returns true even when every row is self,
// so the caller does not also import the other copy.
static bool loadNodesFile(const char* path, uint32_t magic, bool v0, uint8_t& loaded) {
    loaded = 0;
    if (!SD.exists(path)) return false;
    sdIdle();
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t got = 0;
    if (f.read((uint8_t*)&got, 4) != 4 || got != magic) { f.close(); sdIdle(); return false; }
    uint8_t n = 0;
    const size_t rec = v0 ? sizeof(MeshNodeV0) : sizeof(MeshNode);
    if (f.read(&n, 1) != 1 || n == 0 || n > 16 || (size_t)f.size() < 5u + (size_t)n * rec) {
        f.close();
        sdIdle();
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        MeshNode cur{};
        if (v0) {
            MeshNodeV0 old{};
            if (f.read((uint8_t*)&old, sizeof(old)) != (int)sizeof(old)) break;
            cur.nodeNum = old.nodeNum;
            cur.lat_i = old.lat_i;
            cur.lon_i = old.lon_i;
            cur.alt = old.alt;
            cur.snr = old.snr;
            cur.rssi = old.rssi;
            cur.hasPos = old.hasPos;
            cur.isSelf = old.isSelf;
            memcpy(cur.shortName, old.shortName, 4);
            memcpy(cur.longName, old.longName, 19);
        } else if (f.read((uint8_t*)&cur, sizeof(cur)) != (int)sizeof(cur)) {
            break;
        }
        if (keepNode(cur)) loaded++;
    }
    f.close();
    sdIdle();
    return true;
}

static void loadNodes() {
    if (!Storage::available() || !s_nodes) return;
    meshEnsureDir();
    uint8_t loaded = 0;
    if (loadNodesFile(NODES_PATH, NODES_MAGIC, false, loaded)) {
        if (loaded) Serial.printf("[MESH] loaded %u nodes from SD\n", loaded);
        return;
    }
    // One import. The old file stays where it is and is not written again.
    if (!loadNodesFile(NODES_PATH_OLD, NODES_MAGIC_V0, true, loaded)) return;
    if (loaded) {
        saveNodes();
        Serial.printf("[MESH] imported %u nodes into /PigMesh\n", loaded);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// PKI — X25519 key pair. Phone apps read User.public_key (protobuf field 8).
// ════════════════════════════════════════════════════════════════════════════
static uint8_t s_pkiPub[32]  = {};
static uint8_t s_pkiPriv[32] = {};
static bool    s_pkiReady    = false;

static int meshRng(void*, unsigned char* buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t r = esp_random();
        size_t n = len - i;
        if (n > 4) n = 4;
        memcpy(buf + i, &r, n);
        i += n;
    }
    return 0;
}

// Public key is the little-endian X25519 u-coordinate. A MAC-derived
// stand-in does not match this and must not be advertised.
static bool x25519Public(const uint8_t priv[32], uint8_t pub[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, meshRng, nullptr);
    if (rc == 0) rc = mbedtls_mpi_write_binary_le(&Q.X, pub, 32);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0;
}

static bool pkiAesKey(const uint8_t theirPub[32], uint8_t aes[32]) {
    if (!s_pkiReady) return false;
    uint8_t masked[32];
    memcpy(masked, theirPub, 32);
    masked[31] &= 0x7F;
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Q);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, s_pkiPriv, 32);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &Q, masked, 32);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, meshRng, nullptr);
    uint8_t secret[32] = {};
    if (rc == 0) rc = mbedtls_mpi_write_binary_le(&z, secret, 32);
    if (rc == 0) rc = mbedtls_sha256_ret(secret, 32, aes, 0);
    memset(secret, 0, sizeof(secret));
    memset(masked, 0, sizeof(masked));
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc == 0;
}

static void pkiNonce(uint8_t nonce[16], uint32_t pktId, uint32_t extra, uint32_t from) {
    memset(nonce, 0, 16);
    memcpy(nonce + 0, &pktId, 4);
    memcpy(nonce + 4, &extra, 4);
    memcpy(nonce + 8, &from, 4);
}

static bool readPkiFile(const char* path) {
    if (!Storage::available() || !SD.exists(path)) return false;
    sdIdle();
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    bool ok = false;
    if (f.size() == 4 + 64) {
        uint32_t magic = 0;
        uint8_t priv[32], pub[32], check[32];
        if (f.read((uint8_t*)&magic, 4) == 4 && magic == PKI_MAGIC &&
            f.read(priv, 32) == 32 && f.read(pub, 32) == 32 &&
            x25519Public(priv, check) && memcmp(check, pub, 32) == 0) {
            memcpy(s_pkiPriv, priv, 32);
            memcpy(s_pkiPub, pub, 32);
            ok = true;
        }
        memset(priv, 0, sizeof(priv));
        memset(check, 0, sizeof(check));
    }
    f.close();
    sdIdle();
    return ok;
}

static bool writePkiFile() {
    if (!Storage::available()) return false;
    meshEnsureDir();
    sdIdle();
    File f = SD.open(PKI_PATH, FILE_WRITE);
    if (!f) { sdIdle(); return false; }
    f.write((const uint8_t*)&PKI_MAGIC, 4);
    f.write(s_pkiPriv, 32);
    f.write(s_pkiPub, 32);
    f.close();
    sdIdle();
    return true;
}

static bool loadOrGenPki() {
    s_pkiReady = false;
    memset(s_pkiPriv, 0, sizeof(s_pkiPriv));
    memset(s_pkiPub, 0, sizeof(s_pkiPub));
    bool fromNew = readPkiFile(PKI_PATH);
    if (fromNew || readPkiFile(PKI_PATH_OLD)) {
        s_pkiReady = true;
        if (!fromNew) writePkiFile();
        Serial.println("[MESH] PKI loaded");
        return true;
    }
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr;
    mbedtls_ecp_keypair      kp;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr);
    mbedtls_ecp_keypair_init(&kp);
    const char* pers = "mesh_pig_pki";
    int rc = mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &entropy,
                                    (const uint8_t*)pers, strlen(pers));
    if (rc == 0) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_CURVE25519, &kp,
                                           mbedtls_ctr_drbg_random, &ctr);
    if (rc == 0) rc = mbedtls_mpi_write_binary_le(&kp.d, s_pkiPriv, 32);
    mbedtls_ecp_keypair_free(&kp);
    mbedtls_ctr_drbg_free(&ctr);
    mbedtls_entropy_free(&entropy);
    if (rc != 0 || !x25519Public(s_pkiPriv, s_pkiPub)) {
        memset(s_pkiPriv, 0, sizeof(s_pkiPriv));
        memset(s_pkiPub, 0, sizeof(s_pkiPub));
        Serial.println("[MESH] PKI generate failed");
        return false;
    }
    s_pkiReady = true;
    writePkiFile();
    Serial.println("[MESH] PKI generated");
    return true;
}

static bool keyLive(const MeshNode* n) {
    if (!n || !n->hasKey) return false;
    for (int i = 0; i < 32; i++) if (n->pubKey[i]) return true;
    return false;
}

static MeshNode* nodeByNum(uint32_t num) {
    if (!s_nodes) return nullptr;
    for (uint8_t i = 0; i < s_nodeN; i++) if (s_nodes[i].nodeNum == num) return &s_nodes[i];
    return nullptr;
}

static void saveChat() {
    if (!s_chatDirty) return;
    if (!Storage::available() || !s_chat) return;
    meshEnsureDir();
    sdIdle();
    File f = SD.open(CHAT_PATH, FILE_WRITE);
    if (!f) { sdIdle(); return; }
    uint8_t n = s_chatN > MAX_CHAT ? MAX_CHAT : s_chatN;
    f.write((const uint8_t*)&CHAT_MAGIC, 4);
    f.write(s_storeHash);
    f.write(s_storeRegion);
    f.write(s_storeModem);
    f.write((const uint8_t*)s_storeChannel, sizeof(s_storeChannel));
    f.write(n);
    for (uint8_t i = 0; i < n; i++) {
        Msg* m = chatGet(i);
        if (!m) break;
        Msg copy = *m;
        copy.text[47] = 0;
        copy.fromName[7] = 0;
        f.write((const uint8_t*)&copy, sizeof(copy));
    }
    f.close();
    sdIdle();
    s_chatDirty = false;
}

static void loadChat() {
    if (!Storage::available() || !s_chat) return;
    sdIdle();
    if (!SD.exists(CHAT_PATH)) return;
    File f = SD.open(CHAT_PATH, FILE_READ);
    if (!f) return;
    uint32_t magic = 0;
    uint8_t hash = 0, region = 0, modem = 0, n = 0;
    char channel[20] = {};
    bool ok = f.read((uint8_t*)&magic, 4) == 4 && magic == CHAT_MAGIC &&
              f.read(&hash, 1) == 1 && f.read(&region, 1) == 1 &&
              f.read(&modem, 1) == 1 &&
              f.read((uint8_t*)channel, sizeof(channel)) == (int)sizeof(channel) &&
              f.read(&n, 1) == 1 && n <= MAX_CHAT &&
              hash == s_storeHash && region == s_storeRegion &&
              modem == s_storeModem &&
              strncmp(channel, s_storeChannel, sizeof(channel)) == 0;
    if (!ok) { f.close(); sdIdle(); return; }
    for (uint8_t i = 0; i < n; i++) {
        if (f.read((uint8_t*)&s_chat[i], sizeof(Msg)) != (int)sizeof(Msg)) { n = i; break; }
        s_chat[i].text[47] = 0;
        s_chat[i].fromName[7] = 0;
    }
    f.close();
    sdIdle();
    s_chatN = n;
    s_chatHead = (uint8_t)(n % MAX_CHAT);
    s_chatNew = 0;
    Serial.printf("[MESH] loaded %u chat lines\n", n);
}

static void dmPath(uint32_t peer, char* out, size_t cap) {
    snprintf(out, cap, "/PigMesh/dm/%08lx.bin", (unsigned long)peer);
}

static uint8_t readDmFile(uint32_t peer, Msg* buf, uint8_t cap) {
    char path[32];
    dmPath(peer, path, sizeof(path));
    sdIdle();
    if (!Storage::available() || !SD.exists(path)) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t magic = 0;
    uint8_t n = 0;
    if (f.read((uint8_t*)&magic, 4) != 4 || magic != DM_MAGIC ||
        f.read(&n, 1) != 1 || n > cap) {
        f.close();
        sdIdle();
        return 0;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (f.read((uint8_t*)&buf[i], sizeof(Msg)) != (int)sizeof(Msg)) { n = i; break; }
        buf[i].text[47] = 0;
        buf[i].fromName[7] = 0;
    }
    f.close();
    sdIdle();
    return n;
}

static void writeDmFile(uint32_t peer, const Msg* buf, uint8_t n) {
    if (!Storage::available() || !peer || peer == BROADCAST_ID) return;
    if (n > MAX_DM) n = MAX_DM;
    meshEnsureDir();
    char path[32];
    dmPath(peer, path, sizeof(path));
    sdIdle();
    File f = SD.open(path, FILE_WRITE);
    if (!f) { sdIdle(); return; }
    f.write((const uint8_t*)&DM_MAGIC, 4);
    f.write(n);
    if (n && buf) f.write((const uint8_t*)buf, (size_t)n * sizeof(Msg));
    f.close();
    sdIdle();
}

static void rememberDm(uint32_t peer, const Msg& m);

static Msg s_dmPend[8];
static uint8_t s_dmPendN = 0;

static void queueDm(const Msg& m) {
    if (s_dmPendN < 8) s_dmPend[s_dmPendN++] = m;
    else {
        memmove(s_dmPend, s_dmPend + 1, sizeof(Msg) * 7);
        s_dmPend[7] = m;
    }
    s_dmDirty = true;
}

static void flushPendingDm() {
    for (uint8_t i = 0; i < s_dmPendN; i++) {
        uint32_t peer = (s_dmPend[i].from == s_nodeNum) ? s_dmPend[i].to : s_dmPend[i].from;
        rememberDm(peer, s_dmPend[i]);
    }
    s_dmPendN = 0;
}

static void saveOpenDm() {
    if (!s_dmDirty) return;
    if (!s_dm || !s_dmNode) { s_dmDirty = false; return; }
    Msg* buf = new (std::nothrow) Msg[MAX_DM];
    if (!buf) return;
    uint8_t n = s_dmN > MAX_DM ? MAX_DM : s_dmN;
    for (uint8_t i = 0; i < n; i++) {
        Msg* m = dmGet(i);
        if (m) buf[i] = *m;
    }
    writeDmFile(s_dmNode, buf, n);
    delete[] buf;
    s_dmDirty = false;
}

static void rememberDm(uint32_t peer, const Msg& m) {
    if (!peer || peer == BROADCAST_ID) return;
    Msg* buf = new (std::nothrow) Msg[MAX_DM];
    if (!buf) return;
    uint8_t n = readDmFile(peer, buf, MAX_DM);
    if (n < MAX_DM) buf[n++] = m;
    else {
        memmove(buf, buf + 1, sizeof(Msg) * (MAX_DM - 1));
        buf[MAX_DM - 1] = m;
        n = MAX_DM;
    }
    writeDmFile(peer, buf, n);
    delete[] buf;
}

static void flushMeshDisk() {
    if (s_nodesDirty) saveNodes();
    saveChat();
    flushPendingDm();
    saveOpenDm();
}

void meshFlushDm() {
    flushPendingDm();
    saveOpenDm();
}

void meshReloadDm(uint32_t node) {
    if (!s_dm || !node) return;
    uint8_t n = readDmFile(node, s_dm, MAX_DM);
    s_dmN = n;
    s_dmHead = (uint8_t)(n % MAX_DM);
    s_dmNew = 0;
}

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════
bool     s_run=false, s_radioOk=false;
uint32_t s_nodeNum=0;
static uint8_t  s_aesKey[32];
static uint8_t  s_aesKeyLen=0;
static uint8_t  s_chanHash=0x08;
static uint32_t s_pktId=1;

RadioSt s_rs=RS_IDLE;
static uint32_t s_txStartMs=0;
static volatile bool s_rxFlag=false, s_txFlag=false;

uint32_t s_listenSince=0;

static uint32_t s_lastNodeBcast=0;
static char     s_status[64]="";
char     s_radioError[48]="";
int16_t  s_lastRssi = 0;
int8_t   s_lastSnr = 0;
bool     s_haveSig = false;
uint16_t s_rxPkts = 0;
uint16_t s_txPkts = 0;
static uint16_t s_seenEver = 0;

static bool radioInitFailed(const char* reason) {
    snprintf(s_radioError, sizeof(s_radioError), "%s", reason);
    Serial.printf("[MESH] %s\n", s_radioError);
    return false;
}

static void freeMeshResources() {
    if (s_radio) {
        s_radio->standby();
        delete s_radio;
        s_radio = nullptr;
    }
    delete s_radioModule;
    s_radioModule = nullptr;
    if (s_rfExpanderReady) {
        s_rfEnableExpander.digitalWrite(0, false);
        s_rfExpanderReady = false;
    }
    delete[] s_nodes;
    delete[] s_chat;
    delete[] s_dm;
    delete[] s_hist;
    delete[] s_txQ;
    s_nodes = nullptr;
    s_chat = nullptr;
    s_dm = nullptr;
    s_hist = nullptr;
    s_txQ = nullptr;
    s_nodeN = s_chatN = s_chatNew = s_dmN = s_dmNew = s_txN = s_hHead = s_hCnt = 0;
}

static bool allocateMeshResources() {
    s_nodes = new (std::nothrow) MeshNode[16];
    s_chat = new (std::nothrow) Msg[MAX_CHAT];
    s_dm = new (std::nothrow) Msg[MAX_DM];
    s_hist = new (std::nothrow) PktRecord[PACKET_HISTORY_SIZE];
    s_txQ = new (std::nothrow) TxPkt[16];
    if (!s_nodes || !s_chat || !s_dm || !s_hist || !s_txQ) {
        freeMeshResources();
        return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// RADIO CALLBACKS
// ════════════════════════════════════════════════════════════════════════════
static void IRAM_ATTR onRx(){ s_rxFlag=true; }
static void IRAM_ATTR onTx(){ s_txFlag=true; }

// ════════════════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════════════════
static uint32_t genNodeNum(){
    uint8_t mac[6]={}; esp_efuse_mac_get_default(mac);
    uint32_t id=((uint32_t)mac[2])|((uint32_t)mac[3]<<8)|((uint32_t)mac[4]<<16)|((uint32_t)mac[5]<<24);
    if(!id||id==0xFFFFFFFF) id=0x0F3E0001u;
    return id;
}
static uint32_t nextId(){ if(!s_pktId) s_pktId=1; return s_pktId++; }

static const char* nodeName(uint32_t num){
    for(uint8_t i=0;i<s_nodeN;i++) if(s_nodes[i].nodeNum==num && s_nodes[i].shortName[0]) return s_nodes[i].shortName;
    static char tmp[8]; snprintf(tmp,sizeof(tmp),"!%04lx",(unsigned long)(num&0xFFFF)); return tmp;
}

// ════════════════════════════════════════════════════════════════════════════
// PACKET BUILD
// ════════════════════════════════════════════════════════════════════════════
static uint8_t clampHop(uint8_t hop) {
    if (hop == 0xFF) {
        hop = Config::mesh().hopLimit;
        if (hop < 1 || hop > 7) hop = 3;
    }
    return hop;
}

static bool buildEnq(uint32_t to, const uint8_t* pb, uint8_t pbLen, TxPri prio, bool wantAck, uint8_t hop=0xFF){
    hop = clampHop(hop);
    if(pbLen>240) return false;
    uint32_t id=nextId();
    uint8_t enc[240];
    if(!aesCtr(s_aesKey,s_aesKeyLen,id,s_nodeNum,pb,enc,pbLen)) return false;
    uint8_t raw[256];
    memcpy(raw+0,&to,4); memcpy(raw+4,&s_nodeNum,4); memcpy(raw+8,&id,4);
    raw[12]=(hop&FLAGS_HOP_MASK)|(wantAck?FLAGS_WANT_ACK:0)|((hop&7)<<FLAGS_HOPSTART_SHF);
    raw[13]=s_chanHash; raw[14]=0; raw[15]=(uint8_t)(s_nodeNum&0xFF);
    memcpy(raw+HDR_SZ,enc,pbLen);
    return txEnq(raw,(uint16_t)(HDR_SZ+pbLen),prio);
}

// Direct message: hash 0, AES-256-CCM, 8-byte tag, 4-byte extra nonce.
// Nonce is 13 bytes: packet id, extra nonce at byte 4, sender at byte 8.
static bool buildPkiEnq(uint32_t to, const uint8_t theirPub[32],
                        const uint8_t* pb, uint8_t pbLen, TxPri prio, bool wantAck, uint8_t hop=0xFF){
    hop = clampHop(hop);
    if (!s_pkiReady || pbLen > 220) return false;
    uint8_t aes[32];
    if (!pkiAesKey(theirPub, aes)) return false;
    uint32_t id = nextId();
    uint32_t extra = esp_random();
    if (!extra) extra = 1;
    uint8_t nonce[16];
    pkiNonce(nonce, id, extra, s_nodeNum);
    uint8_t enc[240];
    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    int rc = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, aes, 256);
    if (rc == 0)
        rc = mbedtls_ccm_encrypt_and_tag(&ccm, pbLen, nonce, 13, nullptr, 0,
                                          pb, enc, enc + pbLen, 8);
    mbedtls_ccm_free(&ccm);
    memset(aes, 0, sizeof(aes));
    if (rc != 0) return false;
    memcpy(enc + pbLen + 8, &extra, 4);
    uint16_t body = (uint16_t)(pbLen + 12);
    uint8_t raw[256];
    memcpy(raw + 0, &to, 4);
    memcpy(raw + 4, &s_nodeNum, 4);
    memcpy(raw + 8, &id, 4);
    raw[12] = (hop & FLAGS_HOP_MASK) | (wantAck ? FLAGS_WANT_ACK : 0) | ((hop & 7) << FLAGS_HOPSTART_SHF);
    raw[13] = 0;
    raw[14] = 0;
    raw[15] = (uint8_t)(s_nodeNum & 0xFF);
    memcpy(raw + HDR_SZ, enc, body);
    return txEnq(raw, (uint16_t)(HDR_SZ + body), prio);
}

static bool pkiDecrypt(uint32_t from, uint32_t pktId, const uint8_t theirPub[32],
                       const uint8_t* enc, uint16_t encLen,
                       uint8_t* plain, uint16_t& plainLen) {
    if (encLen < 12 || encLen > 240) return false;
    uint8_t aes[32];
    if (!pkiAesKey(theirPub, aes)) return false;
    uint32_t extra = 0;
    memcpy(&extra, enc + encLen - 4, 4);
    const uint8_t* tag = enc + encLen - 12;
    uint16_t clen = (uint16_t)(encLen - 12);
    uint8_t nonce[16];
    pkiNonce(nonce, pktId, extra, from);
    mbedtls_ccm_context ccm;
    mbedtls_ccm_init(&ccm);
    int rc = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, aes, 256);
    if (rc == 0)
        rc = mbedtls_ccm_auth_decrypt(&ccm, clen, nonce, 13, nullptr, 0,
                                       enc, plain, tag, 8);
    mbedtls_ccm_free(&ccm);
    memset(aes, 0, sizeof(aes));
    if (rc != 0) return false;
    plainLen = clen;
    return true;
}

static void sendAck(uint32_t to, uint32_t reqId, bool pki){
    uint8_t pb[12]; PbW w{pb,sizeof(pb)};
    w.vi(1,reqId);
    uint8_t data[20]; PbW d{data,sizeof(data)};
    d.vi(1,PORT_ROUTING); d.by(2,pb,w.len);
    if (!w.ok || !d.ok) return;
    if (pki) {
        MeshNode* peer = nodeByNum(to);
        if (keyLive(peer) && buildPkiEnq(to, peer->pubKey, data, d.len, PRI_ACK, false, 0))
            return;
    }
    buildEnq(to,data,d.len,PRI_ACK,false,0);
}

static void broadcastNodeInfo(){
    const MeshConfig& mc=Config::mesh();
    char ln[32]; snprintf(ln,sizeof(ln),"0N3P0rK/%s",mc.shortName);
    char id[12]; snprintf(id,sizeof(id),"!%08lx",(unsigned long)s_nodeNum);
    uint8_t user[160]; PbW u{user,sizeof(user)};
    u.st(1,id); u.st(2,ln); u.st(3,mc.shortName); u.vi(7,0);
    // User.public_key is field 8. Field 10 is not the key, so phones ignored it.
    if (s_pkiReady) u.by(8, s_pkiPub, 32);
    uint8_t data[192]; PbW d{data,sizeof(data)};
    d.vi(1,PORT_NODEINFO); d.by(2,user,u.len);
    if (!u.ok || !d.ok) return;
    buildEnq(BROADCAST_ID,data,d.len,PRI_DEFAULT,false);
}

// ════════════════════════════════════════════════════════════════════════════
// PACKET DECODE
// ════════════════════════════════════════════════════════════════════════════
static void handlePos(uint32_t from, int16_t rssi, int8_t snr, const uint8_t* b, uint16_t l){
    PbR r{b,l}; int32_t lat=0,lon=0; int16_t alt=0;
    while(!r.done()){ uint8_t f,wt; if(!r.tag(f,wt)) break;
        if(wt==WT_32 && (f==1 || f==2)){ uint32_t v; if(!r.fixed32(v)) break; if(f==1) lat=(int32_t)v; else lon=(int32_t)v; }
        else if(wt==WT_VARINT && f==3){ uint64_t v; if(!r.vi(v)) break; alt=(int16_t)(int32_t)v; }
        else if(!r.skip(wt)) break;
    }
    MeshNode* n=nodeFindOrAdd(from); n->lat_i=lat; n->lon_i=lon; n->alt=alt;
    n->hasPos=true; n->rssi=rssi; n->snr=snr; n->lastSeen=(uint32_t)millis();
}

static void handleNI(uint32_t from, int16_t rssi, int8_t snr, const uint8_t* b, uint16_t l){
    PbR r{b,l}; char sn[5]={}, ln[20]={};
    uint8_t key[32];
    bool gotKey = false;
    while(!r.done()){
        uint8_t f, wt;
        if(!r.tag(f,wt)) break;
        if(wt==WT_LEN){
            const uint8_t* d; uint16_t s;
            if(!r.ld(d,s)) break;
            if(f==2 && s<sizeof(ln)){ memcpy(ln,d,s); ln[s]=0; }
            if(f==3 && s<sizeof(sn)){ memcpy(sn,d,s); sn[s]=0; }
            if(f==8 && s==32){ memcpy(key,d,32); gotKey=true; }
        } else if(!r.skip(wt)) break;
    }
    MeshNode* n=nodeFindOrAdd(from);
    if(sn[0]) { memcpy(n->shortName,sn,4); n->shortName[4]=0; }
    if(ln[0]) { memcpy(n->longName,ln,19); n->longName[19]=0; }
    if(gotKey) { memcpy(n->pubKey,key,32); n->hasKey=true; }
    n->rssi=rssi; n->snr=snr; n->lastSeen=(uint32_t)millis();
    saveNodes();
}

static void noteDm(const Msg& m){
    uint32_t peer=(m.from==s_nodeNum)?m.to:m.from;
    if(s_dmNode==peer){
        dmAdd(m);
        s_dmDirty=true;
    } else {
        queueDm(m);
    }
}

static void noteMessage(const Msg& m){
    if(m.to==BROADCAST_ID){
        chatAdd(m);
        s_chatDirty=true;
        return;
    }
    noteDm(m);
    chatAdd(m);
    s_chatDirty=true;
}

static void handleText(uint32_t from, uint32_t to, int16_t rssi, int8_t snr, const uint8_t* b, uint16_t l){
    if(!l) return;
    Msg m{}; m.from=from; m.to=to; m.timeMs=(uint32_t)millis(); m.sent=false;
    uint8_t cp=l<47?l:47; memcpy(m.text,b,cp); m.text[cp]=0;
    strncpy(m.fromName,nodeName(from),7); m.fromName[7]=0;
    if(to==BROADCAST_ID || to==s_nodeNum || from==s_nodeNum) noteMessage(m);
}

static void dispatch(uint32_t from, uint32_t to, uint32_t pktId,
                     bool wantAck, bool pki, int16_t rssi, float snrF,
                     const uint8_t* pb, uint16_t pbLen)
{
    PbR r{pb,pbLen}; uint8_t port=0;
    const uint8_t* payload=nullptr; uint16_t payLen=0;
    while(!r.done()){ uint8_t f,wt; if(!r.tag(f,wt)) break;
        if(f==1&&wt==WT_VARINT){uint64_t v;r.vi(v);port=(uint8_t)v;}
        else if(f==2&&wt==WT_LEN){r.ld(payload,payLen);}
        else r.skip(wt);
    }
    if(wantAck && from!=BROADCAST_ID && from!=s_nodeNum) sendAck(from,pktId,pki);
    if(!payload) return;
    int8_t snr=(int8_t)snrF;
    switch(port){
        case PORT_TEXT:    handleText(from,to,rssi,snr,payload,payLen); break;
        case PORT_POSITION:handlePos(from,rssi,snr,payload,payLen); break;
        case PORT_NODEINFO:handleNI(from,rssi,snr,payload,payLen); break;
        default: break;
    }
}

static void relayDown(const uint8_t* raw, uint16_t rawLen, uint8_t flags, uint8_t hop){
    if(rawLen>256 || hop<1) return;
    uint8_t relay[256];
    memcpy(relay,raw,rawLen);
    relay[12]=(flags&~FLAGS_HOP_MASK)|(hop-1);
    relay[14]=0;
    relay[15]=(uint8_t)(s_nodeNum&0xFF);
    txEnq(relay,rawLen,PRI_ROUTING);
}

static void decodePacket(const uint8_t* raw, uint16_t rawLen, int16_t rssi, float snr){
    if(rawLen<HDR_SZ+1 || rawLen>256) return;
    uint32_t to,from,pktId;
    memcpy(&to,raw+0,4); memcpy(&from,raw+4,4); memcpy(&pktId,raw+8,4);
    uint8_t flags=raw[12], chanHash=raw[13];
    uint8_t hop=flags&FLAGS_HOP_MASK;
    bool wantAck=(flags&FLAGS_WANT_ACK)!=0;
    bool forUs=(to==BROADCAST_ID || to==s_nodeNum);
    bool pki=(chanHash==0 && to==s_nodeNum);
    if(!forUs){
        if(hop>1 && (chanHash==s_chanHash || chanHash==0) && !seenRecently(from,pktId)){
            markSeen(from,pktId);
            relayDown(raw,rawLen,flags,hop);
        }
        return;
    }
    if(from==s_nodeNum) return;
    if(!pki && chanHash!=s_chanHash) return;

    uint16_t encLen=(uint16_t)(rawLen-HDR_SZ);
    uint8_t plain[256];
    uint16_t plainLen=encLen;
    if(pki){
        if(seenRecently(from,pktId)) return;
        MeshNode* peer=nodeByNum(from);
        if(!keyLive(peer)) return;
        if(!pkiDecrypt(from,pktId,peer->pubKey,raw+HDR_SZ,encLen,plain,plainLen)){
            markSeen(from,pktId);
            return;
        }
        markSeen(from,pktId);
    } else {
        if(seenRecently(from,pktId)) return;
        markSeen(from,pktId);
        if(to==BROADCAST_ID && hop>1) relayDown(raw,rawLen,flags,hop);
        if(!aesCtr(s_aesKey,s_aesKeyLen,pktId,from,raw+HDR_SZ,plain,encLen)) return;
    }
    bool known=nodeByNum(from)!=nullptr;
    if(!known) s_seenEver++;
    MeshNode* n=nodeFindOrAdd(from);
    n->lastSeen=(uint32_t)millis(); n->rssi=rssi; n->snr=(int8_t)snr;
    if(!known) s_nodesDirty = true;
    s_lastRssi=rssi; s_lastSnr=(int8_t)snr; s_haveSig=true; s_rxPkts++;
    dispatch(from,to,pktId,wantAck,pki,rssi,snr,plain,plainLen);
}

// ════════════════════════════════════════════════════════════════════════════
// RADIO INIT
// ════════════════════════════════════════════════════════════════════════════
// Cap LoRa-1262 schematic V1.1: PI4IOE ADDR is tied to GND, so the
// address is 0x43. Active-low RESET only has R7, and R7 is not fitted,
// so the expander stays in reset and never ACKs. An ID read cannot
// see this cap. P0 is the stamp SW pin; drive it only when the chip
// actually answers. The SX1262 itself is on SPI either way.
static bool raiseCapAntennaSwitch() {
    uint8_t found = 0;
    uint32_t freq = 100000;
    static const uint8_t kAddr[2] = {0x43, 0x44};
    static const uint32_t kFreq[2] = {100000, 400000};
    for (int fi = 0; fi < 2 && !found; fi++) {
        for (int ai = 0; ai < 2; ai++) {
            if (!m5::In_I2C.scanID(kAddr[ai], kFreq[fi])) continue;
            found = kAddr[ai];
            freq = kFreq[fi];
            break;
        }
    }
    if (!found) {
        bool key = m5::In_I2C.scanID(0x34, 100000);
        Serial.printf("[MESH] PI4IOE mute, keyboard 0x34 %s\n",
                      key ? "acks" : "also mute");
        return false;
    }
    uint8_t id = 0;
    bool idOk = m5::In_I2C.readRegister(found, 0x01, &id, 1, freq);
    if (idOk)
        Serial.printf("[MESH] PI4IOE 0x%02X id=0x%02X @%luHz\n",
                      found, id, (unsigned long)freq);
    else
        Serial.printf("[MESH] PI4IOE 0x%02X ack, id read failed\n", found);
    // Whole-register writes. The M5 helpers read-modify-write, and a
    // failed read used to abort before P0 ever went high.
    bool wr = m5::In_I2C.writeRegister8(found, 0x03, 0x01, freq)
           && m5::In_I2C.writeRegister8(found, 0x07, 0x00, freq)
           && m5::In_I2C.writeRegister8(found, 0x05, 0x01, freq);
    if (!wr) {
        Serial.printf("[MESH] PI4IOE P0 write nack\n");
        return false;
    }
    s_rfEnableExpander.setAddress(found);
    s_rfEnableExpander.setClock(freq);
    s_rfExpanderReady = true;
    delay(50);
    return true;
}

static bool powerCapRadio() {
    // Keyboard already opened this bus (I2C1, SDA 8, SCL 9).
    // begin(port, pins) would move it and drop the keys.
    if (!Board::isAdv()) return radioInitFailed("ADV CAP ONLY");
    if (!raiseCapAntennaSwitch())
        Serial.printf("[MESH] antenna switch off, SX1262 still starts\n");
    return true;
}

// Region span, same edges plai uses. Power is clamped to what SX1262 can do.
struct MeshBand { float start; float end; int8_t power; };
static const MeshBand MESH_BANDS[] = {
    {869.4f, 869.65f, 22}, // EU868
    {902.0f, 928.0f, 22},  // US
    {915.0f, 928.0f, 22},  // ANZ
    {433.0f, 434.0f, 10},  // EU433
    {920.5f, 923.5f, 13},  // JP
    {920.0f, 923.0f, 22},  // KR
    {920.0f, 925.0f, 22},  // TW
    {868.7f, 869.2f, 20},  // RU
    {865.0f, 867.0f, 22},  // IN
    {864.0f, 868.0f, 22},  // NZ865
    {920.0f, 925.0f, 16},  // TH
    {433.0f, 434.7f, 22},  // UA433
    {868.0f, 868.6f, 14},  // UA868
    {433.0f, 435.0f, 20},  // MY433
    {919.0f, 924.0f, 22},  // MY919
    {917.0f, 925.0f, 20},  // SG923
};
struct MeshPreset { float bw; uint8_t cr, sf; };
static const MeshPreset MESH_PRESETS[] = {
    {250.0f, 5, 11}, // LongFast
    {125.0f, 8, 12}, // LongSlow
    {62.5f,  8, 12}, // VeryLongSlow
    {250.0f, 5, 10}, // MediumSlow
    {250.0f, 5,  9}, // MediumFast
    {250.0f, 5,  8}, // ShortSlow
    {250.0f, 5,  7}, // ShortFast
    {125.0f, 8, 11}, // LongModerate
    {500.0f, 5,  7}, // ShortTurbo
    {500.0f, 8, 11}, // LongTurbo
};
static uint32_t hashChannelName(const char* str) {
    uint32_t hash = 5381;
    if (!str) return hash;
    for (unsigned char c; (c = (unsigned char)*str++) != 0; )
        hash = ((hash << 5) + hash) + c;
    return hash;
}

bool planRadio(const MeshConfig& mc, float& freq, float& bw, uint8_t& sf, uint8_t& cr, int8_t& power) {
    if (mc.region >= 16) return false;
    uint8_t modem = mc.modem;
    if (modem >= 10) modem = 0;
    const MeshBand& band = MESH_BANDS[mc.region];
    bw = MESH_PRESETS[modem].bw;
    sf = MESH_PRESETS[modem].sf;
    cr = MESH_PRESETS[modem].cr;
    float span = band.end - band.start;
    if (span * 1000.0f < bw) {
        bw = 250.0f; sf = 11; cr = 5;
    }
    uint32_t numCh = (uint32_t)(span / (bw / 1000.0f));
    if (numCh == 0) numCh = 1;
    uint32_t slot = mc.freqSlot ? (uint32_t)(mc.freqSlot - 1) : hashChannelName(mc.channel);
    slot %= numCh;
    freq = band.start + (bw / 2000.0f) + slot * (bw / 1000.0f);
    if (mc.freqOverride && mc.freqKHz >= 137000u && mc.freqKHz <= 960000u)
        freq = (float)mc.freqKHz / 1000.0f;
    power = mc.txPower;
    if (power < 2) power = 2;
    if (power > 22) power = 22;
    if (power > band.power) power = band.power;
    return true;
}

static bool radioInit(){
    if (!powerCapRadio()) return false;

    const MeshConfig& mc=Config::mesh();
    float freq = 0, bw = 0;
    uint8_t sf = 11, cr = 5;
    int8_t power = 22;
    if (!planRadio(mc, freq, bw, sf, cr, power))
        return radioInitFailed("2.4GHz unsupported on SX1262");

    // Same SPI wires as the SD slot (SCK 40, MISO 39, MOSI 14).
    // Hold the card's CS high so it does not answer LoRa commands.
    // NSS stays a plain GPIO. SPI.begin() ignores new pins once the
    // bus is already up, so do not try to move the hardware CS here.
    pinMode(12, OUTPUT);
    digitalWrite(12, HIGH);
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);
    SPIClass& bus = Storage::spiBus();
    s_radioModule = new (std::nothrow) Module(
        5, 4, 3, 6, bus, SPISettings(2000000, MSBFIRST, SPI_MODE0));
    if (!s_radioModule) {
        return radioInitFailed("Radio module allocation failed");
    }
    s_radio = new (std::nothrow) SX1262(s_radioModule);
    if (!s_radio) {
        delete s_radioModule;
        s_radioModule = nullptr;
        return radioInitFailed("SX1262 allocation failed");
    }
    // M5's example uses a 3.0 V TCXO and the LDO. The Meshtastic
    // Cardputer ADV target uses 1.8 V, and a plain crystal is the
    // last try. Sync stays 0x2B and the preamble stays 8.
    int16_t st = s_radio->begin(freq, bw, sf, cr, 0x2B, power, 8, 3.0f, true);
    if (st != RADIOLIB_ERR_NONE)
        st = s_radio->begin(freq, bw, sf, cr, 0x2B, power, 8, 1.8f, true);
    if (st != RADIOLIB_ERR_NONE)
        st = s_radio->begin(freq, bw, sf, cr, 0x2B, power, 8, 0.0f, false);
    if (st != RADIOLIB_ERR_NONE) {
        char error[48];
        snprintf(error, sizeof(error),
                 s_rfExpanderReady ? "SX1262 err %d" : "IOE mute SX %d", st);
        radioInitFailed(error);
        return false;
    }
    s_radio->setCurrentLimit(140);
    s_radio->setRxBoostedGainMode(mc.rxBoost);
    Serial.printf("[MESH] SX1262 ready %.3fMHz SF%u BW%.0fkHz %ddBm\n", freq, sf, bw, power);
    return true;
}


// ════════════════════════════════════════════════════════════════════════════
// PUBLIC NAMESPACE
// ════════════════════════════════════════════════════════════════════════════
namespace MeshPig {

size_t heapHeld() {
    size_t n = 0;
    if (s_nodes) n += 16 * sizeof(MeshNode);
    if (s_chat) n += (size_t)MAX_CHAT * sizeof(Msg);
    if (s_dm) n += (size_t)MAX_DM * sizeof(Msg);
    if (s_hist) n += (size_t)PACKET_HISTORY_SIZE * sizeof(PktRecord);
    if (s_txQ) n += 16 * sizeof(TxPkt);
    if (s_radioModule) n += sizeof(Module);
    if (s_radio) n += sizeof(SX1262);
    return n;
}


static void meshOffScreen(const char* status) {
    uiReset();
    snprintf(s_status, sizeof(s_status), "%s", status);
}

void start(){
    // setMode calls start on every open. Already-running returns below,
    // so the opener's Enter is dropped here, not only on a fresh radio.
    uiArmOpen();
    if(s_run) return;
    uiStick();
    s_listenSince = (uint32_t)millis();
    s_radioError[0] = '\0';
    // Off in SET: no buffers, no SPI, no GPIO. The original Cardputer
    // has neither the cap nor a free GPIO5.
    if (!Config::mesh().enabled) {
        meshOffScreen("MESH OFF");
        return;
    }
    if (!Board::isAdv()) {
        Config::mesh().enabled = false;
        Config::save();
        snprintf(s_radioError, sizeof(s_radioError), "ADV CAP ONLY");
        meshOffScreen("MESH: ADV CAP ONLY");
        Display::showToast("ADV CAP ONLY", 1100);
        Serial.println("[MESH] original board, heap not taken");
        return;
    }
    if (!allocateMeshResources()) {
        Config::mesh().enabled = false;
        Config::save();
        snprintf(s_radioError,sizeof(s_radioError),"Mesh buffers allocation failed");
        meshOffScreen("MESH: OUT OF MEMORY");
        Serial.printf("[MESH] buffer allocation failed heap=%u\n", (unsigned)ESP.getFreeHeap());
        Display::showToast("LOW MEM", 1200);
        return;
    }
    s_nodeN=0; s_chatN=0; s_chatHead=0; s_chatNew=0;
    s_dmN=0; s_dmHead=0; s_dmNew=0; s_dmNode=0;
    s_txN=0; s_hHead=0; s_hCnt=0;
    s_pktId=esp_random() | 1u; uiReset(); uiClearNav();
    s_rs=RS_IDLE; s_rxFlag=false; s_txFlag=false;
    s_seenEver=0; s_rxPkts=0; s_txPkts=0; s_haveSig=false; s_lastRssi=0; s_lastSnr=0;

    s_nodeNum=genNodeNum();
    if (!expandPsk(Config::mesh().psk,s_aesKey,s_aesKeyLen)) {
        freeMeshResources();
        snprintf(s_radioError,sizeof(s_radioError),"Invalid channel PSK");
        meshOffScreen("MESH: INVALID PSK");
        Serial.println("[MESH] invalid PSK");
        return;
    }
    s_chanHash=channelHash(Config::mesh().channel,s_aesKey,s_aesKeyLen);
    s_storeHash=s_chanHash;
    s_storeRegion=Config::mesh().region;
    s_storeModem=Config::mesh().modem;
    memset(s_storeChannel,0,sizeof(s_storeChannel));
    strncpy(s_storeChannel,Config::mesh().channel,sizeof(s_storeChannel)-1);
    s_chatDirty=false;
    s_dmDirty=false;
    s_nodesDirty=false;
    s_dmPendN=0;

    MeshNode* self=nodeFindOrAdd(s_nodeNum);
    strncpy(self->shortName,Config::mesh().shortName,4);
    self->shortName[4]=0;
    self->isSelf=true; self->lastSeen=(uint32_t)millis();

    loadNodes();
    loadOrGenPki();
    loadChat();

    s_radioOk=radioInit();
    if(s_radioOk){
        s_radio->setPacketReceivedAction(onRx);
        int16_t rx = s_radio->startReceive();
        if (rx == RADIOLIB_ERR_NONE) s_rs=RS_RX;
        else {
            char error[48];
            snprintf(error, sizeof(error), "startReceive error %d", rx);
            radioInitFailed(error);
            s_radioOk = false;
        }
    }
    if (!s_radioOk) {
        freeMeshResources();
        snprintf(s_status,sizeof(s_status),"MESH: %.52s",
                 s_radioError[0] ? s_radioError : "RADIO ERROR");
        uiReset();
        return;
    }
    s_run=true;
    s_lastNodeBcast=(uint32_t)millis();
    broadcastNodeInfo();
    snprintf(s_status,sizeof(s_status),"MESH %s !%08lx",
             s_radioOk?"OK":"NO RADIO",(unsigned long)s_nodeNum);
    Serial.printf("[MESH] took %u heap=%u\n",
                  (unsigned)heapHeld(), (unsigned)ESP.getFreeHeap());
}

void stop(){
    if (!s_run && !s_nodes && !s_chat && !s_dm && !s_hist && !s_txQ && !s_radio) return;
    if (s_radio) s_radio->standby();
    flushMeshDisk();
    size_t held = heapHeld();
    s_run=false; s_radioOk=false; s_rs=RS_IDLE;
    freeMeshResources();
    if (held)
        Serial.printf("[MESH] returned %u heap=%u\n",
                      (unsigned)held, (unsigned)ESP.getFreeHeap());
}

bool isRunning(){ return s_run; }

void update(){
    if(!s_run||!s_radioOk) return;
    // SD shares this SPI bus. Keep its CS high for the LoRa transfer.
    digitalWrite(12, HIGH);
    uint32_t now=(uint32_t)millis();

    static uint32_t s_lastDisk = 0;
    if (s_rs != RS_TX &&
        (s_chatDirty || s_dmDirty || s_nodesDirty || s_dmPendN) &&
        (uint32_t)(now - s_lastDisk) > 2000) {
        if (s_radio) s_radio->standby();
        flushMeshDisk();
        s_lastDisk = now;
        digitalWrite(12, HIGH);
        if (s_radio && s_radio->startReceive() == RADIOLIB_ERR_NONE) s_rs = RS_RX;
        else s_rs = RS_IDLE;
    }

    // NodeInfo broadcast every 15 min. start() already sent the first one.
    if(now-s_lastNodeBcast>15UL*60UL*1000UL){ s_lastNodeBcast=now; broadcastNodeInfo(); }

    // Periodic node save every 5 minutes
    static uint32_t s_lastNodeSave = 0;
    if(now - s_lastNodeSave > 5UL*60UL*1000UL){ s_lastNodeSave=now; saveNodes(); }

    // RX
    if(s_rxFlag&&s_rs==RS_RX){
        s_rxFlag=false;
        uint8_t buf[256];
        size_t pktLen=s_radio->getPacketLength();
        if(pktLen>sizeof(buf)) pktLen=sizeof(buf);
        if(pktLen>0 && s_radio->readData(buf,(size_t)pktLen)==RADIOLIB_ERR_NONE){
            decodePacket(buf,(uint16_t)pktLen,(int16_t)s_radio->getRSSI(),s_radio->getSNR());
        }
        s_radio->setPacketReceivedAction(onRx);
        if (s_radio->startReceive() == RADIOLIB_ERR_NONE) s_rs=RS_RX;
        else s_rs=RS_IDLE;
    }
    // TX done
    if(s_txFlag){ s_txFlag=false; s_radio->setPacketReceivedAction(onRx); s_rs=(s_radio->startReceive()==RADIOLIB_ERR_NONE)?RS_RX:RS_IDLE; }
    // TX watchdog
    if(s_rs==RS_TX&&now-s_txStartMs>5000){ s_radio->setPacketReceivedAction(onRx); s_rs=(s_radio->startReceive()==RADIOLIB_ERR_NONE)?RS_RX:RS_IDLE; }
    // Send next queued packet
    if(s_rs==RS_RX&&s_txN>0){
        TxPkt pkt; if(txDeq(pkt)){
            s_radio->setPacketSentAction(onTx);
            if(s_radio->startTransmit(pkt.data,pkt.len)==RADIOLIB_ERR_NONE){ s_rs=RS_TX; s_txStartMs=now; s_txPkts++; }
            else { s_radio->setPacketReceivedAction(onRx); s_rs=(s_radio->startReceive()==RADIOLIB_ERR_NONE)?RS_RX:RS_IDLE; }
        }
    }

    const MeshConfig& mc=Config::mesh();
    float freq=0, bw=0; uint8_t sf=0, cr=0; int8_t power=0;
    if (!planRadio(mc, freq, bw, sf, cr, power)) freq = 0;
    snprintf(s_status,sizeof(s_status),"%s %.3fMHz N:%d Q:%d %s",
             mc.channel,freq,s_nodeN,s_txN,s_radioOk?"":"NORADIO");
}

bool sendText(const char* text, uint32_t to){
    if(!s_run||!s_radioOk||!text||!text[0]) return false;
    int fit = utf8Fit(text, 47);
    if (fit <= 0) return false;
    uint8_t tl = (uint8_t)fit;
    uint8_t data[64]; PbW d{data,sizeof(data)};
    d.vi(1,PORT_TEXT); d.by(2,(const uint8_t*)text,tl);
    if (!d.ok) return false;
    bool ok = false;
    if (to != BROADCAST_ID) {
        MeshNode* peer = nodeByNum(to);
        if (keyLive(peer))
            ok = buildPkiEnq(to, peer->pubKey, data, d.len, PRI_DEFAULT, true);
        else
            ok = buildEnq(to, data, d.len, PRI_DEFAULT, true);
    } else {
        ok = buildEnq(to, data, d.len, PRI_DEFAULT, false);
    }
    if(ok){
        Msg m{}; m.from=s_nodeNum; m.to=to; m.timeMs=(uint32_t)millis(); m.sent=true;
        uint8_t cp=tl<47?tl:47; memcpy(m.text,text,cp); m.text[cp]=0;
        strncpy(m.fromName,Config::mesh().shortName,7); m.fromName[7]=0;
        if(to==BROADCAST_ID){ chatAdd(m); s_chatDirty=true; }
        else noteDm(m);
    }
    return ok;
}


uint8_t   nodeCount()       { return s_nodeN; }
MeshNode* nodeAt(uint8_t i) { return i<s_nodeN?&s_nodes[i]:nullptr; }
uint32_t  selfNodeNum()     { return s_nodeNum; }
const char* selfShortName() { return Config::mesh().shortName; }
const char* getStatusLine() { return s_status; }

} // namespace MeshPig
