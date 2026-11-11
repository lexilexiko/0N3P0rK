// mesh_pig.cpp — Full Meshtastic-compatible LoRa mesh for 0N3P0rK
// SX1262 (CAP LoRa-1262) via RadioLib.
//
// Tab strip on the 240x105 canvas. Hints are the real bottom bar.
//   , /     move the tab. ENT opens it. ESC steps back out.
//   Inside  ; . move a list. , / also scroll, or change a SET value.
//   T       write. Fn alone cycles EN / RU / UA. BKSP erases one letter.
//   Esc on the strip leaves the screen and leaves the radio running.
//
// Custom frequency: set freqOverride=true in config, freqKHz = your freq.

#include "mesh_pig.h"
#include "mesh_glyph.h"
#include "../board/board.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../storage/littlefs_ops.h"
#include "../ui/display.h"
#include "../ui/keys.h"

#include <RadioLib.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <utility/PI4IOE5V6408_Class.hpp>
#include <mbedtls/aes.h>
#include <esp_mac.h>
#include <new>

// ── SX1262 pins for CAP LoRa-1262 on Cardputer-ADV ─────────────────────────
static Module* s_radioModule = nullptr;
static SX1262* s_radio = nullptr;
static m5::PI4IOE5V6408_Class s_rfEnableExpander(0x43, 400000, &m5::In_I2C);
static bool s_rfExpanderReady = false;

// ════════════════════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════════════════════
// MESHTASTIC CONSTANTS
// ════════════════════════════════════════════════════════════════════════════
static constexpr uint8_t  HDR_SZ        = 16;
static constexpr uint32_t BROADCAST_ID  = 0xFFFFFFFFu;
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
static TxPkt* s_txQ=nullptr; static uint8_t s_txN=0;
static bool txEnq(const uint8_t* d, uint16_t l, TxPri p){
    if(s_txN>=16||l>sizeof(s_txQ[0].data)) return false;
    uint8_t ins=s_txN;
    for(uint8_t i=0;i<s_txN;i++) if(p<s_txQ[i].prio){ins=i;break;}
    for(uint8_t i=s_txN;i>ins;i--) s_txQ[i]=s_txQ[i-1];
    memcpy(s_txQ[ins].data,d,l); s_txQ[ins].len=l; s_txQ[ins].prio=p; s_txN++; return true;
}
static bool txDeq(TxPkt& p){ if(!s_txN) return false; p=s_txQ[0]; memmove(s_txQ,s_txQ+1,sizeof(TxPkt)*(s_txN-1)); s_txN--; return true; }

// ════════════════════════════════════════════════════════════════════════════
// MESSAGE STORE  (broadcast + DM)
// ════════════════════════════════════════════════════════════════════════════
static constexpr uint8_t MAX_CHAT = 32;   // broadcast channel messages
static constexpr uint8_t MAX_DM   = 16;   // per-node DM (we store last 16 for the selected node)

struct Msg {
    uint32_t from, to;      // to=0xFFFFFFFF → broadcast
    uint32_t timeMs;
    char     text[48];
    char     fromName[8];
    bool     sent;          // true = we sent this
};

static Msg*  s_chat=nullptr; static uint8_t s_chatHead=0, s_chatN=0, s_chatNew=0;
static Msg*  s_dm=nullptr;    static uint8_t s_dmHead=0,   s_dmN=0,   s_dmNew=0;
static uint32_t s_dmNode = 0; // which node's DM is loaded

static void chatAdd(const Msg& m){
    s_chat[s_chatHead]=m; s_chatHead=(s_chatHead+1)%MAX_CHAT;
    if(s_chatN<MAX_CHAT) s_chatN++; s_chatNew++;
}
// get i-th message oldest-first from ring (0=oldest)
static Msg* chatGet(uint8_t i){
    if(i>=s_chatN) return nullptr;
    uint8_t idx=(s_chatHead+MAX_CHAT-s_chatN+i)%MAX_CHAT;
    return &s_chat[idx];
}
static void dmLoad(uint32_t node){ s_dmNode=node; s_dmN=0; s_dmHead=0; }
static void dmAdd(const Msg& m){
    s_dm[s_dmHead]=m; s_dmHead=(s_dmHead+1)%MAX_DM;
    if(s_dmN<MAX_DM) s_dmN++; s_dmNew++;
}
static Msg* dmGet(uint8_t i){
    if(i>=s_dmN) return nullptr;
    uint8_t idx=(s_dmHead+MAX_DM-s_dmN+i)%MAX_DM;
    return &s_dm[idx];
}

// ════════════════════════════════════════════════════════════════════════════
// NODE DB
// ════════════════════════════════════════════════════════════════════════════
static MeshNode* s_nodes=nullptr; static uint8_t s_nodeN=0;
static MeshNode* nodeFindOrAdd(uint32_t num){
    for(uint8_t i=0;i<s_nodeN;i++) if(s_nodes[i].nodeNum==num) return &s_nodes[i];
    if(s_nodeN<16){ s_nodes[s_nodeN]={}; s_nodes[s_nodeN].nodeNum=num; return &s_nodes[s_nodeN++]; }
    uint8_t old=0; uint32_t oldMs=UINT32_MAX;
    for(uint8_t i=0;i<s_nodeN;i++) if(!s_nodes[i].isSelf&&s_nodes[i].lastSeen<oldMs){oldMs=s_nodes[i].lastSeen;old=i;}
    s_nodes[old]={}; s_nodes[old].nodeNum=num; return &s_nodes[old];
}

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════
static bool     s_run=false, s_radioOk=false;
static uint32_t s_nodeNum=0;
static uint8_t  s_aesKey[32];
static uint8_t  s_aesKeyLen=0;
static uint8_t  s_chanHash=0x08;
static uint32_t s_pktId=1;

enum RadioSt : uint8_t { RS_IDLE, RS_RX, RS_TX };
static RadioSt s_rs=RS_IDLE;
static uint32_t s_txStartMs=0;
static volatile bool s_rxFlag=false, s_txFlag=false;

// UI view states
enum Layer : uint8_t { LY_STRIP = 0, LY_PAGE, LY_DM, LY_WRITE, LY_FIELD };
static uint8_t  s_tab = 0;               // 0 chat, 1 people, 2 radio, 3 set
static Layer    s_layer = LY_STRIP;
static Layer    s_writeFrom = LY_PAGE;
static uint8_t  s_setRow = 0;
static uint8_t  s_setScroll = 0;
static uint8_t  s_field = 0;
static bool     s_fieldAscii = false;
static uint8_t  s_fieldMax = 47;
static bool     s_holdUi = false;        // radio restart keeps the open tab
static bool     s_swallowEnter = false;  // Enter that opened MESH must not enter a tab
static uint8_t  s_lang = 0;              // 0 EN, 1 RU, 2 UA. This session only.
static uint8_t  s_nodeScroll=0;
static uint8_t  s_chatScroll=0;
static uint8_t  s_dmScroll=0;
static bool     s_chatStick=true;
static bool     s_dmStick=true;
static uint32_t s_listenSince=0;
static char     s_cmpBuf[48]={};
static uint8_t  s_cmpLen=0;

static uint32_t s_lastNodeBcast=0;
static uint32_t s_lastDraw=0;
static char     s_status[64]="";
static char     s_radioError[48]="";
static int16_t  s_lastRssi = 0;
static int8_t   s_lastSnr = 0;
static bool     s_haveSig = false;
static uint16_t s_rxPkts = 0;
static uint16_t s_txPkts = 0;
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
static bool buildEnq(uint32_t to, const uint8_t* pb, uint8_t pbLen, TxPri prio, bool wantAck, uint8_t hop=0xFF){
    if (hop == 0xFF) {
        hop = Config::mesh().hopLimit;
        if (hop < 1 || hop > 7) hop = 3;
    }
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

static void sendAck(uint32_t to, uint32_t reqId){
    uint8_t pb[12]; PbW w{pb,sizeof(pb)};
    w.vi(1,reqId);
    uint8_t data[20]; PbW d{data,sizeof(data)};
    d.vi(1,PORT_ROUTING); d.by(2,pb,w.len);
    if (!w.ok || !d.ok) return;
    buildEnq(to,data,d.len,PRI_ACK,false,0);
}

static void broadcastNodeInfo(){
    const MeshConfig& mc=Config::mesh();
    char ln[32]; snprintf(ln,sizeof(ln),"0N3P0rK/%s",mc.shortName);
    char id[12]; snprintf(id,sizeof(id),"!%08lx",(unsigned long)s_nodeNum);
    uint8_t user[80]; PbW u{user,sizeof(user)};
    u.st(1,id); u.st(2,ln); u.st(3,mc.shortName); u.vi(7,0);
    uint8_t data[100]; PbW d{data,sizeof(data)};
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
    while(!r.done()){ uint8_t f,wt; if(!r.tag(f,wt)) break;
        if(wt==WT_LEN){ const uint8_t* d; uint16_t s; r.ld(d,s);
            if(f==2&&s<20){memcpy(ln,d,s);ln[s]=0;}
            if(f==3&&s<5) {memcpy(sn,d,s);sn[s]=0;}
        } else { uint64_t v; r.vi(v); }
    }
    MeshNode* n=nodeFindOrAdd(from);
    if(sn[0]) strncpy(n->shortName,sn,4);
    if(ln[0]) strncpy(n->longName,ln,19);
    n->rssi=rssi; n->snr=snr; n->lastSeen=(uint32_t)millis();
}

static void handleText(uint32_t from, uint32_t to, int16_t rssi, int8_t snr, const uint8_t* b, uint16_t l){
    if(!l) return;
    Msg m{}; m.from=from; m.to=to; m.timeMs=(uint32_t)millis(); m.sent=false;
    uint8_t cp=l<47?l:47; memcpy(m.text,b,cp); m.text[cp]=0;
    strncpy(m.fromName,nodeName(from),7); m.fromName[7]=0;

    if(to==BROADCAST_ID){
        chatAdd(m);
    } else if(to==s_nodeNum || from==s_nodeNum){
        // DM — add to dm store if it matches the currently open DM node
        uint32_t peer=(from==s_nodeNum)?to:from;
        if(s_dmNode==peer || s_dmNode==0){
            if(s_dmNode==0) s_dmNode=peer;
            dmAdd(m);
        }
        // Also show as chat notification
        m.to=to; chatAdd(m);
    }
}

static void dispatch(uint32_t from, uint32_t to, uint32_t pktId,
                     bool wantAck, int16_t rssi, float snrF,
                     const uint8_t* pb, uint16_t pbLen)
{
    PbR r{pb,pbLen}; uint8_t port=0;
    const uint8_t* payload=nullptr; uint16_t payLen=0;
    while(!r.done()){ uint8_t f,wt; if(!r.tag(f,wt)) break;
        if(f==1&&wt==WT_VARINT){uint64_t v;r.vi(v);port=(uint8_t)v;}
        else if(f==2&&wt==WT_LEN){r.ld(payload,payLen);}
        else r.skip(wt);
    }
    if(wantAck && from!=BROADCAST_ID && from!=s_nodeNum) sendAck(from,pktId);
    if(!payload) return;
    int8_t snr=(int8_t)snrF;
    switch(port){
        case PORT_TEXT:    handleText(from,to,rssi,snr,payload,payLen); break;
        case PORT_POSITION:handlePos(from,rssi,snr,payload,payLen); break;
        case PORT_NODEINFO:handleNI(from,rssi,snr,payload,payLen); break;
        default: break;
    }
}

static void decodePacket(const uint8_t* raw, uint16_t rawLen, int16_t rssi, float snr){
    if(rawLen<HDR_SZ+1) return;
    uint32_t to,from,pktId;
    memcpy(&to,raw+0,4); memcpy(&from,raw+4,4); memcpy(&pktId,raw+8,4);
    uint8_t flags=raw[12], chanHash=raw[13];
    uint8_t hop=flags&FLAGS_HOP_MASK;
    bool wantAck=(flags&FLAGS_WANT_ACK)!=0;
    if(to!=BROADCAST_ID&&to!=s_nodeNum){
        if(hop>1&&chanHash==s_chanHash&&!seenRecently(from,pktId)){
            markSeen(from,pktId);
            uint8_t relay[256]; memcpy(relay,raw,rawLen);
            relay[12]=(flags&~FLAGS_HOP_MASK)|(hop-1);
            relay[14]=0; relay[15]=(uint8_t)(s_nodeNum&0xFF);
            txEnq(relay,rawLen,PRI_ROUTING);
        }
        return;
    }
    if(from==s_nodeNum) return;
    if(chanHash!=s_chanHash) return;
    if(seenRecently(from,pktId)) return;
    markSeen(from,pktId);
    if(to==BROADCAST_ID&&hop>1){
        uint8_t relay[256]; memcpy(relay,raw,rawLen);
        relay[12]=(flags&~FLAGS_HOP_MASK)|(hop-1);
        relay[14]=0; relay[15]=(uint8_t)(s_nodeNum&0xFF);
        txEnq(relay,rawLen,PRI_ROUTING);
    }
    uint8_t encLen=rawLen-HDR_SZ, plain[256];
    if(!aesCtr(s_aesKey,s_aesKeyLen,pktId,from,raw+HDR_SZ,plain,encLen)) return;
    bool known=false;
    if(s_nodes){ for(uint8_t i=0;i<s_nodeN;i++) if(s_nodes[i].nodeNum==from){ known=true; break; } }
    if(!known) s_seenEver++;
    MeshNode* n=nodeFindOrAdd(from);
    n->lastSeen=(uint32_t)millis(); n->rssi=rssi; n->snr=(int8_t)snr;
    s_lastRssi=rssi; s_lastSnr=(int8_t)snr; s_haveSig=true; s_rxPkts++;
    dispatch(from,to,pktId,wantAck,rssi,snr,plain,encLen);
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

static bool planRadio(const MeshConfig& mc, float& freq, float& bw, uint8_t& sf, uint8_t& cr, int8_t& power) {
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
// DRAW HELPERS
// ════════════════════════════════════════════════════════════════════════════
#define COL_BG      TFT_BLACK
#define COL_HDR     0x0010
#define COL_HDR2    0x2104
#define COL_SEL     0x2965
#define COL_TEXT    TFT_WHITE
#define COL_DIM     0x8410
#define COL_CYAN    TFT_CYAN
#define COL_GREEN   TFT_GREEN
#define COL_YELLOW  TFT_YELLOW
#define COL_PINK    0xF81F
#define COL_RED     TFT_RED
#define COL_ORANGE  0xFD20
#define LH          10
#define TAB_H       11
#define MESH_H      105

// Rows under the tab strip. The key line lives in the real bottom bar.
static int meshRows() {
    int rows = (MESH_H - TAB_H - 2) / LH;
    return rows < 1 ? 1 : rows;
}

static int utf8Next(const char* s, uint32_t& cp) {
    if (!s || !s[0]) { cp = 0; return 0; }
    const uint8_t a = (uint8_t)s[0];
    if (a < 0x80) { cp = a; return 1; }
    if ((a & 0xE0) == 0xC0 && (uint8_t)s[1] >= 0x80) {
        cp = ((uint32_t)(a & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
        return 2;
    }
    if ((a & 0xF0) == 0xE0 && (uint8_t)s[1] >= 0x80 && (uint8_t)s[2] >= 0x80) {
        cp = ((uint32_t)(a & 0x0F) << 12) |
             ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) |
             ((uint8_t)s[2] & 0x3F);
        return 3;
    }
    cp = '?';
    return 1;
}

static int utf8Fit(const char* s, int cap) {
    int i = 0;
    if (!s || cap < 1) return 0;
    while (s[i]) {
        uint32_t cp;
        int n = utf8Next(s + i, cp);
        if (n <= 0 || i + n > cap) break;
        i += n;
    }
    return i;
}

static int utf8Cols(const char* s, int nbytes) {
    int cols = 0;
    int i = 0;
    if (!s) return 0;
    while (s[i] && i < nbytes) {
        uint32_t cp;
        int n = utf8Next(s + i, cp);
        if (n <= 0) break;
        i += n;
        cols++;
    }
    return cols;
}

static void blitGlyph(M5Canvas& cv, int x, int y, const uint8_t* col, uint16_t fg) {
    for (int c = 0; c < 5; c++) {
        uint8_t bits = col[c];
        for (int r = 0; r < 7; r++) {
            if (bits & (uint8_t)(1u << (6 - r))) cv.drawPixel(x + c, y + r, fg);
        }
    }
}

static int drawUtf8(M5Canvas& cv, int x, int y, int xMax, const char* s,
                    uint16_t fg, uint16_t bg) {
    if (!s) return x;
    while (*s && x + 6 <= xMax) {
        uint32_t cp;
        int n = utf8Next(s, cp);
        if (n <= 0) break;
        s += n;
        if (cp < 0x80) {
            cv.setTextColor(fg, bg);
            cv.setCursor(x, y);
            char ch[2] = { (char)cp, 0 };
            cv.print(ch);
        } else {
            const uint8_t* g = meshGlyph((uint16_t)cp);
            if (g) blitGlyph(cv, x, y, g, fg);
            else {
                cv.setTextColor(fg, bg);
                cv.setCursor(x, y);
                cv.print("?");
            }
        }
        x += 6;
    }
    return x;
}

// Cardputer Esc is the ` key, so yo/ghe-upturn sit on the backslash key.
struct LayKey { char ascii; uint16_t ru; uint16_t ua; };
static const LayKey LAY[] = {
    {'q',0x0439,0}, {'Q',0x0419,0}, {'w',0x0446,0}, {'W',0x0426,0},
    {'e',0x0443,0}, {'E',0x0423,0}, {'r',0x043A,0}, {'R',0x041A,0},
    {'t',0x0435,0}, {'T',0x0415,0}, {'y',0x043D,0}, {'Y',0x041D,0},
    {'u',0x0433,0}, {'U',0x0413,0}, {'i',0x0448,0}, {'I',0x0428,0},
    {'o',0x0449,0}, {'O',0x0429,0}, {'p',0x0437,0}, {'P',0x0417,0},
    {'[',0x0445,0}, {'{',0x0425,0}, {']',0x044A,0x0457}, {'}',0x042A,0x0407},
    {'\\',0x0451,0x0491}, {'|',0x0401,0x0490},
    {'a',0x0444,0}, {'A',0x0424,0}, {'s',0x044B,0x0456}, {'S',0x042B,0x0406},
    {'d',0x0432,0}, {'D',0x0412,0}, {'f',0x0430,0}, {'F',0x0410,0},
    {'g',0x043F,0}, {'G',0x041F,0}, {'h',0x0440,0}, {'H',0x0420,0},
    {'j',0x043E,0}, {'J',0x041E,0}, {'k',0x043B,0}, {'L',0x041B,0},
    {'l',0x0434,0}, {'L',0x0414,0}, {';',0x0436,0}, {':',0x0416,0},
    {'\'',0x044D,0x0454}, {'"',0x042D,0x0404},
    {'z',0x044F,0}, {'Z',0x042F,0}, {'x',0x0447,0}, {'X',0x0427,0},
    {'c',0x0441,0}, {'C',0x0421,0}, {'v',0x043C,0}, {'V',0x041C,0},
    {'b',0x0438,0}, {'B',0x0418,0}, {'n',0x0442,0}, {'N',0x0422,0},
    {'m',0x044C,0}, {'M',0x042C,0}, {',',0x0431,0}, {'<',0x0411,0},
    {'.',0x044E,0}, {'>',0x042E,0}, {'/',0x002E,0}, {'?',0x002C,0},
};

static uint32_t mapLay(char c, uint8_t lang) {
    if (!lang) return (uint8_t)c;
    for (unsigned i = 0; i < sizeof(LAY) / sizeof(LAY[0]); i++) {
        if (LAY[i].ascii != c) continue;
        if (lang == 2 && LAY[i].ua) return LAY[i].ua;
        return LAY[i].ru;
    }
    return (uint8_t)c;
}

static const char* langName() {
    return s_lang == 1 ? "RU" : s_lang == 2 ? "UA" : "EN";
}

static void pushCp(uint32_t cp) {
    char tmp[4];
    uint8_t n = 0;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else return;
    if ((uint16_t)s_cmpLen + n > s_fieldMax) return;
    memcpy(s_cmpBuf + s_cmpLen, tmp, n);
    s_cmpLen = (uint8_t)(s_cmpLen + n);
    s_cmpBuf[s_cmpLen] = 0;
}

static void popCp() {
    if (!s_cmpLen) return;
    do { s_cmpLen--; } while (s_cmpLen && (((uint8_t)s_cmpBuf[s_cmpLen] & 0xC0) == 0x80));
    s_cmpBuf[s_cmpLen] = 0;
}

static uint16_t sigCol(int16_t rssi) {
    return rssi > -80 ? COL_GREEN : (rssi > -100 ? COL_YELLOW : COL_RED);
}

static void ageStr(uint32_t lastMs, char* out, uint8_t cap) {
    uint32_t s = (millis() - lastMs) / 1000;
    if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
}

static int putLine(M5Canvas& cv, int y, int yMax, int w, const char* s, uint16_t col) {
    if (y > yMax - 8) return y;
    drawUtf8(cv, 2, y, w - 2, s, col, COL_BG);
    return y + LH;
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

static void resetUi() {
    if (s_holdUi) return;
    s_tab = 0;
    s_layer = LY_STRIP;
    s_setRow = 0;
    s_setScroll = 0;
}

static void meshOffScreen(const char* status) {
    resetUi();
    snprintf(s_status, sizeof(s_status), "%s", status);
}

void start(){
    // setMode calls start on every open. Already-running returns below,
    // so the opener's Enter is dropped here, not only on a fresh radio.
    s_swallowEnter = true;
    if(s_run) return;
    s_chatStick = true;
    s_dmStick = true;
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
    s_pktId=esp_random() | 1u; resetUi(); s_nodeScroll=0; s_chatScroll=0;
    s_dmScroll=0; s_chatStick=true; s_dmStick=true; s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
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

    MeshNode* self=nodeFindOrAdd(s_nodeNum);
    strncpy(self->shortName,Config::mesh().shortName,4);
    self->isSelf=true; self->lastSeen=(uint32_t)millis();

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
        resetUi();
        return;
    }
    s_lastNodeBcast=0; s_run=true;
    snprintf(s_status,sizeof(s_status),"MESH %s !%08lx",
             s_radioOk?"OK":"NO RADIO",(unsigned long)s_nodeNum);
    Serial.printf("[MESH] took %u heap=%u\n",
                  (unsigned)heapHeld(), (unsigned)ESP.getFreeHeap());
}

void stop(){
    if (!s_run && !s_nodes && !s_chat && !s_dm && !s_hist && !s_txQ && !s_radio) return;
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

    // NodeInfo broadcast every 15 min
    if(now-s_lastNodeBcast>15UL*60UL*1000UL){ s_lastNodeBcast=now; broadcastNodeInfo(); }

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
    bool ok=buildEnq(to,data,d.len,PRI_DEFAULT,to!=BROADCAST_ID);
    if(ok){
        Msg m{}; m.from=s_nodeNum; m.to=to; m.timeMs=(uint32_t)millis(); m.sent=true;
        uint8_t cp=tl<47?tl:47; memcpy(m.text,text,cp); m.text[cp]=0;
        strncpy(m.fromName,Config::mesh().shortName,7); m.fromName[7]=0;
        if(to==BROADCAST_ID){ chatAdd(m); }
        else {
            if(s_dmNode==to||s_dmNode==0){ if(!s_dmNode) s_dmNode=to; dmAdd(m); }
        }
    }
    return ok;
}

static uint8_t countOnline() {
    if (!s_nodes) return 0;
    uint32_t now = (uint32_t)millis();
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_nodeN; i++) {
        if (s_nodes[i].isSelf) continue;
        if (now - s_nodes[i].lastSeen < 8UL * 60UL * 1000UL) n++;
    }
    return n;
}

static const char* meshRegionName(uint8_t i) {
    static const char* names[] = {
        "EU868","US915","ANZ923","EU433","JP923","KR921","TW923","RU868",
        "IN865","NZ865","TH923","UA433","UA868","MY433","MY919","SG923","2.4GHz"
    };
    return i < 17 ? names[i] : "?";
}
static const char* meshModemName(uint8_t i) {
    static const char* names[] = {
        "LongFast","LongSlow","VLongSlow","MedSlow","MedFast",
        "ShortSlow","ShortFast","LongMod","ShortTurbo","LongTurbo"
    };
    return i < 10 ? names[i] : "?";
}

static bool heardOther() {
    if (!s_nodes) return false;
    for (uint8_t i = 0; i < s_nodeN; i++) if (!s_nodes[i].isSelf) return true;
    return false;
}

static const char* SET_LBL[] = {
    "MESH", "REGION", "MODEM", "TX dBm", "HOPS", "SLOT",
    "FREQ", "CHANNEL", "NAME", "PSK", "RX BOOST"
};
static const uint8_t SET_N = 11;

static bool keyHas(char a) {
    if (M5Cardputer.Keyboard.isKeyPressed(a)) return true;
    for (char c : M5Cardputer.Keyboard.keysState().word) {
        if (c == a) return true;
    }
    return false;
}

// Comma and slash move the tab from the strip and from inside a page.
// A DM closes onto the new tab. Text boxes keep those keys for letters.
static void moveTab(int dir) {
    s_tab = (uint8_t)((s_tab + (dir < 0 ? 3 : 1)) % 4);
    if (s_layer == LY_DM) s_layer = LY_PAGE;
}

static void radioRestart() {
    if (!Config::mesh().enabled) return;
    s_holdUi = true;
    if (isRunning()) stop();
    start();
    s_holdUi = false;
}

static void drawTabs(M5Canvas& cv, int w) {
    static const char* tabs[] = { "CHAT", "PEOPLE", "RADIO", "SET" };
    const int tw = w / 4;
    for (int i = 0; i < 4; i++) {
        int x = i * tw;
        bool on = (i == (int)s_tab);
        uint16_t bg = on ? (uint16_t)COL_HDR : (uint16_t)COL_HDR2;
        cv.fillRect(x, 0, tw, TAB_H, bg);
        uint16_t fg = on ? (uint16_t)COL_CYAN : (uint16_t)COL_DIM;
        cv.setTextColor(fg, bg);
        int lw = (int)strlen(tabs[i]) * 6;
        cv.setCursor(x + (tw - lw) / 2, 1);
        cv.print(tabs[i]);
        uint16_t mark = (s_layer == LY_STRIP) ? (uint16_t)COL_YELLOW : (uint16_t)COL_CYAN;
        if (on) cv.fillRect(x, TAB_H - 2, tw, 2, mark);
        if (i == 0 && !s_chatStick) {
            cv.setTextColor(COL_YELLOW, bg);
            cv.setCursor(x + 2, 1);
            cv.print("+");
        }
    }
    uint16_t pip = COL_RED;
    if (s_radioOk && s_rs == RS_TX) pip = COL_PINK;
    else if (s_radioOk) pip = COL_GREEN;
    cv.fillRect(tw - 7, 3, 4, 4, pip);
}

static void drawNodeRow(M5Canvas& cv, int y, int w, const MeshNode& n, bool sel) {
    uint16_t bg = COL_BG;
    if (sel) {
        cv.fillRect(0, y - 1, w, LH, COL_SEL);
        cv.fillRect(0, y - 1, 2, LH, COL_CYAN);
        bg = COL_SEL;
    }
    bool fresh = n.isSelf || ((uint32_t)millis() - n.lastSeen < 8UL * 60UL * 1000UL);
    uint16_t nameCol = n.isSelf ? (uint16_t)COL_PINK : (fresh ? (uint16_t)COL_TEXT : (uint16_t)COL_DIM);
    if (n.isSelf) drawUtf8(cv, 4, y, 40, "you", nameCol, bg);
    else if (n.shortName[0]) drawUtf8(cv, 4, y, 40, n.shortName, nameCol, bg);
    else {
        char id[8];
        snprintf(id, sizeof(id), "!%04lx", (unsigned long)(n.nodeNum & 0xFFFF));
        drawUtf8(cv, 4, y, 40, id, nameCol, bg);
    }
    cv.setTextColor(n.isSelf ? (uint16_t)COL_DIM : sigCol(n.rssi), bg);
    cv.setCursor(42, y);
    if (n.isSelf) cv.print("  --");
    else cv.printf("%4d", n.rssi);
    char age[8];
    ageStr(n.lastSeen, age, sizeof(age));
    cv.setTextColor(COL_DIM, bg);
    cv.setCursor(72, y);
    cv.printf("%4s", age);
    if (n.longName[0]) drawUtf8(cv, 104, y, w - 2, n.longName, nameCol, bg);
}

static void drawChat(M5Canvas& cv, int w, int h, bool dm) {
    int y = TAB_H + 2;
    int yMax = h - 2;
    int rows = meshRows();
    uint8_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    uint8_t nmsg = dm ? s_dmN : s_chatN;
    uint8_t maxTop = (nmsg > (uint8_t)rows) ? (uint8_t)(nmsg - (uint8_t)rows) : 0;
    if (scroll > maxTop) scroll = maxTop;
    if (stick) scroll = maxTop;

    if (!nmsg) {
        if (!Config::mesh().enabled) {
            y = putLine(cv, y, yMax, w, "Mesh is off.", COL_RED);
            putLine(cv, y, yMax, w, "Turn it on in the SET tab.", COL_DIM);
        } else if (!dm && s_radioError[0]) {
            y = putLine(cv, y, yMax, w, s_radioError, COL_RED);
            const MeshConfig& mc = Config::mesh();
            char match[40];
            snprintf(match, sizeof(match), "%s  %s  same key",
                     meshRegionName(mc.region), meshModemName(mc.modem));
            putLine(cv, y, yMax, w, match, COL_DIM);
        } else if (dm) {
            y = putLine(cv, y, yMax, w, "No direct messages yet.", COL_DIM);
            putLine(cv, y, yMax, w, "T writes to this person.", COL_DIM);
        } else {
            y = putLine(cv, y, yMax, w, "Listening for this channel.", COL_TEXT);
            y = putLine(cv, y, yMax, w, "Other people's messages", COL_DIM);
            y = putLine(cv, y, yMax, w, "show up in this chat.", COL_DIM);
            const MeshConfig& mc = Config::mesh();
            char match[40];
            snprintf(match, sizeof(match), "%s  %s  same key",
                     meshRegionName(mc.region), meshModemName(mc.modem));
            y = putLine(cv, y, yMax, w, match, COL_CYAN);
            if (s_radioOk && !s_rfExpanderReady)
                putLine(cv, y, yMax, w, "Antenna switch is off.", COL_YELLOW);
        }
        return;
    }
    for (int i = 0; i < rows; i++) {
        Msg* m = dm ? dmGet((uint8_t)(scroll + i)) : chatGet((uint8_t)(scroll + i));
        if (!m) break;
        int yy = y + i * LH;
        uint16_t col = m->sent ? (uint16_t)COL_GREEN : (uint16_t)COL_TEXT;
        const char* who = m->fromName[0] ? m->fromName : "?";
        int x = drawUtf8(cv, 2, yy, 56, who, col, COL_BG);
        cv.setTextColor(col, (uint16_t)COL_BG);
        cv.setCursor(x, yy);
        cv.print(m->to == BROADCAST_ID ? ":" : ">");
        drawUtf8(cv, x + 6, yy, w - 2, m->text, col, COL_BG);
    }
}

static void drawPeople(M5Canvas& cv, int w, int h) {
    int y = TAB_H + 2;
    int rows = meshRows();
    if (rows < 2) rows = 2;
    cv.setTextColor(COL_DIM, (uint16_t)COL_BG);
    cv.setCursor(4, y);
    if (!s_nodes || !heardOther()) cv.print("nobody yet");
    else cv.printf("%u on", countOnline());
    cv.setCursor(72, y);
    cv.print("dBm  age");
    int listRows = rows - 1;
    if (!s_nodes || !s_nodeN) {
        putLine(cv, y + LH, h - 2, w, "Names land here after a packet.", COL_DIM);
        return;
    }
    if (s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
    int cur = s_nodeScroll;
    int top = cur - listRows + 1;
    if (top < 0) top = 0;
    for (int i = 0; i < listRows && top + i < s_nodeN; i++) {
        int idx = top + i;
        drawNodeRow(cv, y + LH + i * LH, w, s_nodes[idx], idx == cur && s_layer == LY_PAGE);
    }
}

static void drawRadio(M5Canvas& cv, int w, int h) {
    const MeshConfig& mc = Config::mesh();
    float freq = 0, bw = 0;
    uint8_t sf = 0, cr = 0;
    int8_t power = 0;
    bool planned = planRadio(mc, freq, bw, sf, cr, power);
    (void)cr;
    char line[44];
    int y = TAB_H + 2;
    int yMax = h - 2;
    if (!mc.enabled) y = putLine(cv, y, yMax, w, "MESH OFF", COL_RED);
    else if (!s_radioOk && s_radioError[0]) y = putLine(cv, y, yMax, w, s_radioError, COL_RED);
    else if (s_radioOk && s_rs == RS_TX) y = putLine(cv, y, yMax, w, "LORA TX", COL_PINK);
    else if (s_radioOk) y = putLine(cv, y, yMax, w, "LORA OK", COL_GREEN);
    if (planned) {
        snprintf(line, sizeof(line), "%.3f MHz%s", freq, mc.freqOverride ? "  set" : "");
        y = putLine(cv, y, yMax, w, line, COL_TEXT);
    } else {
        y = putLine(cv, y, yMax, w, "Frequency not set", COL_RED);
    }
    snprintf(line, sizeof(line), "%s   %s", meshRegionName(mc.region), meshModemName(mc.modem));
    y = putLine(cv, y, yMax, w, line, COL_CYAN);
    if (planned)
        snprintf(line, sizeof(line), "SF%u  BW%.0f  %ddBm  hop %u", sf, bw, power, mc.hopLimit);
    else
        snprintf(line, sizeof(line), "hop %u", mc.hopLimit);
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    snprintf(line, sizeof(line), "channel  %s", mc.channel[0] ? mc.channel : "-");
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    snprintf(line, sizeof(line), "you  !%04lx  %s",
             (unsigned long)(s_nodeNum & 0xFFFF), mc.shortName);
    y = putLine(cv, y, yMax, w, line, COL_PINK);
    snprintf(line, sizeof(line), "rx %u   tx %u   queue %u", s_rxPkts, s_txPkts, s_txN);
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    if (s_haveSig) {
        snprintf(line, sizeof(line), "last  %d dBm  snr %+d", s_lastRssi, s_lastSnr);
        y = putLine(cv, y, yMax, w, line, sigCol(s_lastRssi));
    } else {
        char age[8] = "--";
        if (s_listenSince) ageStr(s_listenSince, age, sizeof(age));
        snprintf(line, sizeof(line), "last  none yet   %s", age);
        y = putLine(cv, y, yMax, w, line, COL_DIM);
    }
    if (s_rfExpanderReady) putLine(cv, y, yMax, w, "switch on", COL_GREEN);
    else if (s_radioOk) putLine(cv, y, yMax, w, "switch off  short range", COL_YELLOW);
    else putLine(cv, y, yMax, w, "switch off", COL_DIM);
}

static void setValue(uint8_t row, char* out, int cap) {
    const MeshConfig& mc = Config::mesh();
    switch (row) {
        case 0: snprintf(out, cap, "%s", mc.enabled ? "ON" : "OFF"); break;
        case 1: snprintf(out, cap, "%s", meshRegionName(mc.region)); break;
        case 2: snprintf(out, cap, "%s", meshModemName(mc.modem)); break;
        case 3: snprintf(out, cap, "%d", (int)mc.txPower); break;
        case 4: snprintf(out, cap, "%u", mc.hopLimit); break;
        case 5:
            if (!mc.freqSlot) snprintf(out, cap, "AUTO");
            else snprintf(out, cap, "%u", mc.freqSlot);
            break;
        case 6:
            if (!mc.freqOverride) snprintf(out, cap, "AUTO");
            else snprintf(out, cap, "%.3f", (float)mc.freqKHz / 1000.0f);
            break;
        case 7: snprintf(out, cap, "%s", mc.channel[0] ? mc.channel : "-"); break;
        case 8: snprintf(out, cap, "%s", mc.shortName[0] ? mc.shortName : "-"); break;
        case 9:
            snprintf(out, cap, "%s",
                     strcmp(mc.psk, "AQ==") == 0 ? "DEFAULT" :
                     strcmp(mc.psk, "AA==") == 0 ? "OPEN" : "CUSTOM");
            break;
        default: snprintf(out, cap, "%s", mc.rxBoost ? "ON" : "OFF"); break;
    }
}

static void drawSet(M5Canvas& cv, int w, int h) {
    int rows = meshRows();
    if (s_setRow >= SET_N) s_setRow = SET_N - 1;
    if (s_setRow < s_setScroll) s_setScroll = s_setRow;
    if (s_setRow >= s_setScroll + rows)
        s_setScroll = (uint8_t)(s_setRow - rows + 1);
    int y0 = TAB_H + 2;
    for (int i = 0; i < rows && s_setScroll + i < SET_N; i++) {
        uint8_t row = (uint8_t)(s_setScroll + i);
        int y = y0 + i * LH;
        bool sel = (row == s_setRow && s_layer == LY_PAGE);
        uint16_t bg = sel ? (uint16_t)COL_SEL : (uint16_t)COL_BG;
        if (sel) {
            cv.fillRect(0, y - 1, w, LH, COL_SEL);
            cv.fillRect(0, y - 1, 2, LH, COL_CYAN);
        }
        char val[24];
        setValue(row, val, sizeof(val));
        uint16_t vc = COL_TEXT;
        if ((row == 0 || row == 10) && val[1] == 'N') vc = COL_GREEN;
        if ((row == 0 || row == 10) && val[1] == 'F') vc = COL_RED;
        drawUtf8(cv, 4, y, 80, SET_LBL[row], sel ? (uint16_t)COL_CYAN : (uint16_t)COL_TEXT, bg);
        drawUtf8(cv, 84, y, w - 2, val, vc, bg);
    }
    (void)h;
}

static const char* KEY_ROWS[] = { "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./" };

static void drawWriter(M5Canvas& cv, int w, int h, const char* title) {
    cv.fillRect(0, 0, w, TAB_H, COL_HDR);
    cv.setTextColor(COL_CYAN, (uint16_t)COL_HDR);
    cv.setCursor(2, 1);
    cv.print(langName());
    if (s_fieldAscii) cv.print(" ASCII");
    drawUtf8(cv, s_fieldAscii ? 56 : 26, 1, w - 36, title ? title : "", COL_TEXT, COL_HDR);
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", utf8Cols(s_cmpBuf, s_cmpLen));
    cv.setTextColor(COL_DIM, (uint16_t)COL_HDR);
    cv.setCursor(w - 6 * (int)strlen(cnt) - 2, 1);
    cv.print(cnt);

    cv.drawRect(1, TAB_H + 1, w - 2, h - TAB_H - 3, COL_CYAN);
    int x0 = 4;
    int y0 = TAB_H + 4;
    int xMax = w - 4;
    int yMax = h - 4;
    int maxC = (xMax - x0) / 6;
    if (maxC < 1) maxC = 1;
    int col = 0;
    int x = x0;
    int y = y0;
    for (int i = 0; i < s_cmpLen && s_cmpBuf[i];) {
        uint32_t cp;
        int n = utf8Next(s_cmpBuf + i, cp);
        if (n <= 0) break;
        i += n;
        if (col >= maxC) { col = 0; x = x0; y += LH; }
        if (y + 8 > yMax) break;
        if (cp < 0x80) {
            cv.setTextColor(COL_TEXT, (uint16_t)COL_BG);
            cv.setCursor(x, y);
            char ch[2] = { (char)cp, 0 };
            cv.print(ch);
        } else {
            const uint8_t* g = meshGlyph((uint16_t)cp);
            if (g) blitGlyph(cv, x, y, g, COL_TEXT);
            else {
                cv.setTextColor(COL_TEXT, (uint16_t)COL_BG);
                cv.setCursor(x, y);
                cv.print("?");
            }
        }
        x += 6;
        col++;
    }
    if (((uint32_t)millis() / 400u) & 1u) {
        int cx, cy, ccol;
        int nch = utf8Cols(s_cmpBuf, s_cmpLen);
        ccol = maxC ? (nch % maxC) : 0;
        cx = x0 + ccol * 6;
        cy = y0 + (maxC ? (nch / maxC) : 0) * LH;
        if (cy + 8 <= yMax) cv.drawFastVLine(cx, cy, 8, COL_CYAN);
    }
    if (s_cmpLen || s_fieldAscii) return;
    int ky = y0 + LH * 2;
    uint8_t lang = s_lang;
    for (int r = 0; r < 3 && ky + 8 < yMax; r++) {
        int kx = x0;
        for (const char* p = KEY_ROWS[r]; *p && kx + 6 <= xMax; p++) {
            uint32_t cp = mapLay(*p, lang);
            if (cp < 0x80) {
                cv.setTextColor(COL_DIM, (uint16_t)COL_BG);
                cv.setCursor(kx, ky);
                char ch[2] = { (char)cp, 0 };
                cv.print(ch);
            } else {
                const uint8_t* g = meshGlyph((uint16_t)cp);
                if (g) blitGlyph(cv, kx, ky, g, COL_DIM);
            }
            kx += 6;
        }
        ky += LH;
    }
}

static void drawDmHdr(M5Canvas& cv, int w) {
    cv.fillRect(0, 0, w, TAB_H, COL_HDR);
    const char* nm = "?";
    if (s_nodes) {
        for (uint8_t i = 0; i < s_nodeN; i++) {
            if (s_nodes[i].nodeNum == s_dmNode && s_nodes[i].shortName[0]) {
                nm = s_nodes[i].shortName;
                break;
            }
        }
    }
    cv.setTextColor(COL_CYAN, (uint16_t)COL_HDR);
    cv.setCursor(2, 1);
    cv.print("DM");
    drawUtf8(cv, 20, 1, w - 20, nm, COL_TEXT, COL_HDR);
}

void draw(M5Canvas& canvas) {
    uint32_t now = (uint32_t)millis();
    bool typing = (s_layer == LY_WRITE || s_layer == LY_FIELD);
    if (!typing && now - s_lastDraw < 120) return;
    s_lastDraw = now;

    int w = canvas.width();
    int h = canvas.height();
    canvas.fillScreen(COL_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);

    if (s_layer == LY_WRITE || s_layer == LY_FIELD) {
        char title[32];
        if (s_layer == LY_FIELD) {
            const char* t = "PSK";
            if (s_field == 6) t = "FREQ";
            else if (s_field == 7) t = "CHANNEL";
            else if (s_field == 8) t = "NAME";
            snprintf(title, sizeof(title), "%s", t);
        } else if (s_writeFrom == LY_DM) {
            const char* nm = "?";
            if (s_nodes) {
                for (uint8_t i = 0; i < s_nodeN; i++) {
                    if (s_nodes[i].nodeNum == s_dmNode && s_nodes[i].shortName[0]) {
                        nm = s_nodes[i].shortName;
                        break;
                    }
                }
            }
            snprintf(title, sizeof(title), "DM %s", nm);
        } else {
            snprintf(title, sizeof(title), "%s", Config::mesh().channel);
        }
        drawWriter(canvas, w, h, title);
        return;
    }
    if (s_layer == LY_DM) {
        drawDmHdr(canvas, w);
        drawChat(canvas, w, h, true);
        return;
    }
    drawTabs(canvas, w);
    if (s_tab == 0) drawChat(canvas, w, h, false);
    else if (s_tab == 1) drawPeople(canvas, w, h);
    else if (s_tab == 2) drawRadio(canvas, w, h);
    else drawSet(canvas, w, h);
}

static bool fieldCharOk(uint32_t cp) {
    if (!s_fieldAscii) return cp >= 0x20;
    if (cp < 0x20 || cp >= 0x7F) return false;
    char c = (char)cp;
    if (s_field == 6) return (c >= '0' && c <= '9') || c == '.';
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

static void openWrite() {
    if (!s_run || !s_radioOk) {
        Display::showToast(Config::mesh().enabled ? "NO RADIO" : "MESH OFF", 800);
        return;
    }
    s_writeFrom = s_layer;
    s_layer = LY_WRITE;
    s_fieldAscii = false;
    s_fieldMax = 47;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
}

static void openField() {
    const MeshConfig& mc = Config::mesh();
    s_field = s_setRow;
    s_cmpBuf[0] = 0;
    s_cmpLen = 0;
    s_fieldAscii = (s_field == 6 || s_field == 9);
    if (s_field == 6) {
        s_fieldMax = 8;
        if (mc.freqOverride) {
            snprintf(s_cmpBuf, sizeof(s_cmpBuf), "%.3f", (float)mc.freqKHz / 1000.0f);
            s_cmpLen = (uint8_t)strlen(s_cmpBuf);
        }
    } else if (s_field == 7) {
        s_fieldMax = 19;
        memcpy(s_cmpBuf, mc.channel, 19);
        s_cmpBuf[19] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    } else if (s_field == 8) {
        s_fieldMax = 4;
        memcpy(s_cmpBuf, mc.shortName, 4);
        s_cmpBuf[4] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    } else {
        s_fieldMax = 44;
        memcpy(s_cmpBuf, mc.psk, 44);
        s_cmpBuf[44] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    }
    s_layer = LY_FIELD;
}

static void commitWrite() {
    if (s_cmpLen && s_run && s_radioOk) {
        s_cmpBuf[s_cmpLen] = 0;
        uint32_t dest = (s_writeFrom == LY_DM) ? s_dmNode : BROADCAST_ID;
        if (sendText(s_cmpBuf, dest)) {
            if (s_writeFrom == LY_DM) s_dmStick = true;
            else s_chatStick = true;
        }
    }
    s_layer = s_writeFrom;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
}

static void rememberName() {
    if (!s_nodes) return;
    for (uint8_t i = 0; i < s_nodeN; i++) {
        if (!s_nodes[i].isSelf) continue;
        memset(s_nodes[i].shortName, 0, sizeof(s_nodes[i].shortName));
        memcpy(s_nodes[i].shortName, Config::mesh().shortName, 4);
    }
}

static void commitField() {
    MeshConfig& mc = Config::mesh();
    s_cmpBuf[s_cmpLen] = 0;
    bool restart = false;
    if (s_field == 6) {
        float fMHz = atof(s_cmpBuf);
        if (fMHz >= 137.0f && fMHz <= 960.0f) {
            mc.freqKHz = (uint32_t)(fMHz * 1000.0f + 0.5f);
            mc.freqOverride = true;
            restart = true;
            Display::showToast("FREQ SET", 800);
        } else if (!s_cmpBuf[0] || s_cmpBuf[0] == '0') {
            mc.freqOverride = false;
            restart = true;
            Display::showToast("FREQ AUTO", 800);
        } else {
            Display::showToast("BAD FREQ", 800);
            return;
        }
    } else if (s_field == 7) {
        memset(mc.channel, 0, sizeof(mc.channel));
        if (s_cmpLen) memcpy(mc.channel, s_cmpBuf, s_cmpLen);
        if (!mc.channel[0]) strncpy(mc.channel, "LongFast", sizeof(mc.channel) - 1);
        restart = true;
        Display::showToast("CHANNEL SET", 800);
    } else if (s_field == 8) {
        memset(mc.shortName, 0, sizeof(mc.shortName));
        if (s_cmpLen) memcpy(mc.shortName, s_cmpBuf, s_cmpLen > 4 ? 4 : s_cmpLen);
        if (!mc.shortName[0]) strncpy(mc.shortName, "PIG", sizeof(mc.shortName) - 1);
        rememberName();
        Display::showToast("NAME SET", 800);
    } else {
        memset(mc.psk, 0, sizeof(mc.psk));
        if (s_cmpLen) memcpy(mc.psk, s_cmpBuf, s_cmpLen);
        if (!mc.psk[0]) strncpy(mc.psk, "AQ==", sizeof(mc.psk) - 1);
        restart = true;
        Display::showToast("PSK SET", 800);
    }
    Config::save();
    s_layer = LY_PAGE;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
    if (restart) radioRestart();
}

static void bumpSet(int dir) {
    MeshConfig& mc = Config::mesh();
    switch (s_setRow) {
        case 0:
            if (dir > 0 && !mc.enabled) {
                mc.enabled = true;
                Config::save();
                s_holdUi = true;
                if (!isRunning()) start();
                s_holdUi = false;
            } else if (dir < 0 && mc.enabled) {
                mc.enabled = false;
                Config::save();
                if (isRunning()) stop();
            }
            break;
        case 1: {
            int v = (int)mc.region + dir;
            if (v < 0 || v > 16) break;
            mc.region = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 2: {
            int v = (int)mc.modem + dir;
            if (v < 0 || v > 9) break;
            mc.modem = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 3: {
            int v = (int)mc.txPower + dir;
            if (v < 2 || v > 22) break;
            mc.txPower = (int8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 4: {
            int v = (int)mc.hopLimit + dir;
            if (v < 1 || v > 7) break;
            mc.hopLimit = (uint8_t)v;
            Config::save();
            break;
        }
        case 5: {
            int v = (int)mc.freqSlot + dir;
            if (v < 0 || v > 104) break;
            mc.freqSlot = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 10:
            if (dir > 0 && !mc.rxBoost) { mc.rxBoost = true; Config::save(); radioRestart(); }
            else if (dir < 0 && mc.rxBoost) { mc.rxBoost = false; Config::save(); radioRestart(); }
            break;
        default:
            break;
    }
}

static void typingKeys(const Keyboard_Class::KeysState& ks, bool esc) {
    if (ks.enter) {
        if (s_layer == LY_WRITE) commitWrite();
        else commitField();
        return;
    }
    if (esc) {
        Layer back = (s_layer == LY_WRITE) ? s_writeFrom : LY_PAGE;
        s_layer = back;
        s_cmpLen = 0;
        s_cmpBuf[0] = 0;
        return;
    }
    if (ks.del) { popCp(); return; }
    uint8_t lang = s_fieldAscii ? 0 : s_lang;
    for (char c : ks.word) {
        if (c < 0x20 || c >= 0x7F) continue;
        uint32_t cp = mapLay(c, lang);
        if (!fieldCharOk(cp)) continue;
        pushCp(cp);
    }
}

static void chatKeys(const Keyboard_Class::KeysState& ks, bool dm) {
    if (!ks.fn) {
        for (char c : ks.word) {
            if (c == 't' || c == 'T') { openWrite(); return; }
        }
    }
    int dir = 0;
    if (keyHas(';')) dir = -1;
    else if (keyHas('.')) dir = 1;
    if (!dir) return;
    uint8_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    uint8_t nmsg = dm ? s_dmN : s_chatN;
    int rows = meshRows();
    uint8_t maxTop = (nmsg > (uint8_t)rows) ? (uint8_t)(nmsg - (uint8_t)rows) : 0;
    if (dir < 0 && scroll > 0) { scroll--; stick = false; }
    else if (dir > 0 && scroll < maxTop) {
        scroll++;
        if (scroll >= maxTop) stick = true;
    }
}

static void peopleKeys(const Keyboard_Class::KeysState& ks) {
    if (!s_nodes || !s_nodeN) return;
    if (s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
    if (keyHas(';')) {
        if (s_nodeScroll > 0) s_nodeScroll--;
    } else if (keyHas('.')) {
        if (s_nodeScroll + 1 < s_nodeN) s_nodeScroll++;
    } else if (ks.enter && !s_nodes[s_nodeScroll].isSelf) {
        if (!s_run || !s_radioOk) {
            Display::showToast(Config::mesh().enabled ? "NO RADIO" : "MESH OFF", 800);
            return;
        }
        dmLoad(s_nodes[s_nodeScroll].nodeNum);
        s_dmScroll = 0;
        s_dmStick = true;
        s_layer = LY_DM;
        s_dmNew = 0;
    }
}

static void setKeys(const Keyboard_Class::KeysState& ks) {
    if (s_setRow >= SET_N) s_setRow = 0;
    if (keyHas(';')) {
        if (s_setRow > 0) s_setRow--;
        return;
    }
    if (keyHas('.')) {
        if (s_setRow + 1 < SET_N) s_setRow++;
        return;
    }
    // Shift+, and Shift+.  Plain comma and slash already moved the tab.
    if (keyHas('<')) { bumpSet(-1); return; }
    if (keyHas('>')) { bumpSet(1); return; }
    if (!ks.enter) return;
    if (s_setRow == 0 || s_setRow == 10) {
        bool on = (s_setRow == 0) ? Config::mesh().enabled : Config::mesh().rxBoost;
        bumpSet(on ? -1 : 1);
        return;
    }
    if (s_setRow >= 6 && s_setRow <= 9) openField();
}

void handleKeys() {
    if (App::windowHidden()) return;
    // Menu Enter is still down on the next frame. keyNewPress would treat
    // that held key as a fresh press and fall straight into CHAT.
    if (s_swallowEnter) {
        if (M5Cardputer.Keyboard.keysState().enter) return;
        s_swallowEnter = false;
    }
    static bool keyWas = false;
    if (!keyNewPress(keyWas)) return;
    auto ks = M5Cardputer.Keyboard.keysState();
    const bool esc = keyEsc();
    s_lastDraw = 0;

    // Fn alone cycles EN / RU / UA. A letter in the same press still types.
    if (ks.fn && ks.word.empty() && !ks.enter && !ks.del && !esc) {
        s_lang = (uint8_t)((s_lang + 1) % 3);
        return;
    }

    if (s_layer == LY_WRITE || s_layer == LY_FIELD) {
        typingKeys(ks, esc);
        return;
    }
    if (esc) {
        if (s_layer == LY_DM) { s_layer = LY_PAGE; s_tab = 1; return; }
        if (s_layer == LY_PAGE) { s_layer = LY_STRIP; return; }
        App::setMode(AppMode::MENU);
        return;
    }
    if (keyHas(',')) { moveTab(-1); return; }
    if (keyHas('/')) { moveTab(1); return; }
    if (s_layer == LY_STRIP) {
        if (ks.enter) s_layer = LY_PAGE;
        return;
    }
    if (s_layer == LY_DM || s_tab == 0) { chatKeys(ks, s_layer == LY_DM); return; }
    if (s_tab == 1) { peopleKeys(ks); return; }
    if (s_tab == 2) return;
    setKeys(ks);
}

const char* bottomHint() {
    static char b[48];
    const char* rest = ",/ TAB  ENT OPEN  ESC CLOSE";
    if (s_layer == LY_WRITE) rest = "FN LANG  ENT SEND  BKSP  ESC";
    else if (s_layer == LY_FIELD)
        rest = s_fieldAscii ? "ENT SAVE  BKSP  ESC" : "FN LANG  ENT SAVE  BKSP  ESC";
    else if (s_layer == LY_DM) rest = ",/ TAB  T WRITE  ;/. MOVE  ESC";
    else if (s_layer == LY_PAGE && s_tab == 0) rest = ",/ TAB  T WRITE  ;/. MOVE  ESC";
    else if (s_layer == LY_PAGE && s_tab == 1) {
        bool self = s_nodes && s_nodeN && s_nodeScroll < s_nodeN && s_nodes[s_nodeScroll].isSelf;
        rest = self ? ",/ TAB  ;/. MOVE  ESC BACK" : ",/ TAB  ENT CHAT  ;/. MOVE  ESC";
    } else if (s_layer == LY_PAGE && s_tab == 2) rest = ",/ TAB  ESC BACK";
    else if (s_layer == LY_PAGE) rest = ",/ TAB  <> CHG  ;/. MOVE  ENT";
    snprintf(b, sizeof(b), "%s  %s", langName(), rest);
    return b;
}

bool isComposing() { return s_layer == LY_WRITE || s_layer == LY_FIELD; }

uint8_t   nodeCount()       { return s_nodeN; }
MeshNode* nodeAt(uint8_t i) { return i<s_nodeN?&s_nodes[i]:nullptr; }
uint8_t   msgCount()        { return s_chatN; }
MeshMsg*  msgAt(uint8_t i)  {
    Msg* m=chatGet(i); if(!m) return nullptr;
    // convert Msg → MeshMsg on-the-fly using static buffer
    static MeshMsg mm;
    mm.from=m->from; mm.time=m->timeMs; mm.broadcast=(m->to==BROADCAST_ID);
    strncpy(mm.text,m->text,47); mm.text[47]=0;
    strncpy(mm.fromName,m->fromName,7); mm.fromName[7]=0;
    return &mm;
}
uint8_t   newMsgCount()     { return s_chatNew; }
void      clearNew()        { s_chatNew=0; s_dmNew=0; }
uint32_t  selfNodeNum()     { return s_nodeNum; }
const char* selfShortName() { return Config::mesh().shortName; }
const char* getStatusLine() { return s_status; }

} // namespace MeshPig
