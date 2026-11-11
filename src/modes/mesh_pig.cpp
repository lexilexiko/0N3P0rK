// mesh_pig.cpp — Full Meshtastic-compatible LoRa mesh for 0N3P0rK
// SX1262 (CAP LoRa-1262) via RadioLib.
//
// UI states (like plai):
//   VIEW_NODES   — scrollable node list
//   VIEW_DETAIL  — node detail (name, coords, signal)
//   VIEW_CHAT    — broadcast channel chat   [TAB from NODES]
//   VIEW_DM      — direct message to one node [ENTER from DETAIL]
//   VIEW_COMPOSE — text input (from CHAT or DM)
//
// Keys:
//   NODES:  ↑↓ scroll  ENTER=detail  TAB=chat  ESC=exit
//   DETAIL: ENTER=DM   ESC=back
//   CHAT:   ↑↓ scroll  T=compose  ESC=back
//   DM:     ↑↓ scroll  T=compose  ESC=back
//   COMPOSE: type text  ENTER=send  ESC=cancel  DEL=backspace
//
// Custom frequency: set freqOverride=true in config, freqKHz = your freq.

#include "mesh_pig.h"
#include "../board/board.h"
#include "../core/config.h"
#include "../storage/littlefs_ops.h"
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
enum View : uint8_t { V_NODES, V_DETAIL, V_CHAT, V_DM, V_COMPOSE };
enum Pane : uint8_t { PANE_LEFT, PANE_CENTER, PANE_RIGHT };
static View     s_view = V_CHAT;
static Pane     s_pane = PANE_CENTER;
static View     s_composeFrom = V_CHAT;  // which view launched compose
static uint8_t  s_nodeScroll=0;          // NODES view scroll offset
static uint8_t  s_detailNode=0;          // index in s_nodes of selected node
static uint8_t  s_chatScroll=0;          // CHAT scroll (bottom=newest)
static uint8_t  s_dmScroll=0;            // DM scroll
static bool     s_chatStick=true;        // follow the newest channel line
static bool     s_dmStick=true;
static uint32_t s_listenSince=0;
static char     s_cmpBuf[48]={};         // compose buffer
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
#define COL_HDR     0x0010   // dark navy
#define COL_HDR2    0x2104   // dark grey (tab bar)
#define COL_SEL     0x2965   // selected row highlight
#define COL_TEXT    TFT_WHITE
#define COL_DIM     0x8410   // grey
#define COL_CYAN    TFT_CYAN
#define COL_GREEN   TFT_GREEN
#define COL_YELLOW  TFT_YELLOW
#define COL_PINK    0xF81F
#define COL_RED     TFT_RED
#define COL_ORANGE  0xFD20
#define LH          10       // line height px

// draw a one-line header bar
static void drawHdr(M5Canvas& cv, const char* title, int W){
    cv.fillRect(0,0,W,LH+1,COL_HDR);
    cv.setTextColor(COL_CYAN,(uint16_t)COL_HDR);
    cv.setTextSize(1); cv.setCursor(2,1); cv.print(title);
}
// Bottom key line. Same words as the other tools: ENT, ESC, BKSP.
static void drawHint(M5Canvas& cv, const char* hint, int W, int H){
    cv.fillRect(0, H - 10, W, 10, COL_HDR);
    cv.setTextColor(0xC618, (uint16_t)COL_HDR);
    cv.setTextSize(1);
    cv.setCursor(2, H - 9);
    cv.print(hint);
}

// signal colour: >-80=green, >-100=yellow, else red
static uint16_t sigCol(int16_t rssi){ return rssi>-80?COL_GREEN:(rssi>-100?COL_YELLOW:COL_RED); }

// age string: "5s" "3m" "2h"
static void ageStr(uint32_t lastMs, char* out, uint8_t cap){
    uint32_t s=(millis()-lastMs)/1000;
    if(s<60)      snprintf(out,cap,"%lus",(unsigned long)s);
    else if(s<3600)snprintf(out,cap,"%lum",(unsigned long)(s/60));
    else           snprintf(out,cap,"%luh",(unsigned long)(s/3600));
}

// ════════════════════════════════════════════════════════════════════════════
// VIEW DRAW FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static void drawCompose(M5Canvas& cv, int W, int H){
    // Show context (chat or DM)
    char hdr[48];
    if(s_composeFrom==V_DM){
        char nm[5]="????";
        for(uint8_t i=0;i<s_nodeN;i++) if(s_nodes[i].nodeNum==s_dmNode&&s_nodes[i].shortName[0]){strncpy(nm,s_nodes[i].shortName,4);nm[4]=0;break;}
        snprintf(hdr,sizeof(hdr),"DM to: %s",nm);
    } else {
        snprintf(hdr,sizeof(hdr),"MSG > %s",Config::mesh().channel);
    }
    drawHdr(cv,hdr,W);

    // Compose box
    int bY=LH+4, bH=H-bY-LH-2;
    cv.drawRect(1,bY,W-2,bH,COL_CYAN);

    // Word-wrap the compose buffer
    cv.setTextSize(1);
    cv.setTextColor(COL_TEXT,COL_BG);
    int chW=6, maxChLine=(W-8)/chW;
    int lines=(s_cmpLen/maxChLine)+1;
    for(int l=0;l<lines;l++){
        int start=l*maxChLine, end=start+maxChLine;
        if(end>(int)s_cmpLen) end=s_cmpLen;
        char row[48]={};
        memcpy(row,s_cmpBuf+start,end-start);
        cv.setCursor(4,bY+2+l*LH); cv.print(row);
    }
    // Cursor
    int curLine=s_cmpLen/maxChLine, curCol=s_cmpLen%maxChLine;
    int cxPx=4+curCol*chW, cyPx=bY+2+curLine*LH;
    if(cyPx+LH<bY+bH) cv.drawFastVLine(cxPx,cyPx,LH-1,COL_CYAN);

    // Char counter
    cv.setTextColor(COL_DIM,COL_BG);
    cv.setCursor(W-24,H-LH-1);
    cv.printf("%d/47",s_cmpLen);

    drawHint(cv,"[ENT]send [ESC]cancel [DEL]backspace",W,H);
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC NAMESPACE
// ════════════════════════════════════════════════════════════════════════════
namespace MeshPig {

void start(){
    if(s_run) return;
    s_radioError[0] = '\0';
    if (!Config::mesh().enabled) {
        snprintf(s_status,sizeof(s_status),"MESH OFF");
        s_view=V_CHAT; s_pane=PANE_CENTER;
        s_run=true;
        return;
    }
    if (!allocateMeshResources()) {
        snprintf(s_radioError,sizeof(s_radioError),"Mesh buffers allocation failed");
        snprintf(s_status,sizeof(s_status),"MESH: OUT OF MEMORY");
        Serial.println("[MESH] buffer allocation failed");
        s_view=V_CHAT; s_pane=PANE_CENTER;
        s_run=true;
        return;
    }
    s_nodeN=0; s_chatN=0; s_chatHead=0; s_chatNew=0;
    s_dmN=0; s_dmHead=0; s_dmNew=0; s_dmNode=0;
    s_txN=0; s_hHead=0; s_hCnt=0;
    s_pktId=esp_random() | 1u; s_view=V_CHAT; s_pane=PANE_CENTER; s_nodeScroll=0; s_chatScroll=0;
    s_dmScroll=0; s_detailNode=0; s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
    s_rs=RS_IDLE; s_rxFlag=false; s_txFlag=false;
    s_seenEver=0; s_rxPkts=0; s_txPkts=0; s_haveSig=false; s_lastRssi=0; s_lastSnr=0;

    s_nodeNum=genNodeNum();
    if (!expandPsk(Config::mesh().psk,s_aesKey,s_aesKeyLen)) {
        freeMeshResources();
        snprintf(s_radioError,sizeof(s_radioError),"Invalid channel PSK");
        snprintf(s_status,sizeof(s_status),"MESH: INVALID PSK");
        Serial.println("[MESH] invalid PSK");
        s_view=V_CHAT; s_pane=PANE_CENTER;
        s_run=true;
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
        s_view=V_CHAT; s_pane=PANE_CENTER;
        s_run=true;
        return;
    }
    s_lastNodeBcast=0; s_run=true;
    snprintf(s_status,sizeof(s_status),"MESH %s !%08lx",
             s_radioOk?"OK":"NO RADIO",(unsigned long)s_nodeNum);
}

void stop(){
    s_run=false; s_radioOk=false; s_rs=RS_IDLE;
    freeMeshResources();
}

bool isRunning(){ return s_run; }

void update(){
    if(!s_run||!s_radioOk) return;
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
    uint8_t tl=(uint8_t)strlen(text); if(tl>47) tl=47;
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
        "EU868","US915","ANZ","EU433","JP","KR","TW","RU",
        "IN","NZ","TH","UA433","UA868","MY433","MY919","SG","2.4"
    };
    return i < 17 ? names[i] : "?";
}
static const char* meshModemName(uint8_t i) {
    static const char* names[] = {
        "LongFast","LongSlow","VLongS","MedSlow","MedFast",
        "ShortSlow","ShortFast","LongMod","ShortT","LongT"
    };
    return i < 10 ? names[i] : "?";
}

static void drawCollapsedLeft(M5Canvas& cv, int x, int w, int H) {
    cv.fillRect(x, 0, w, H, COL_HDR2);
    cv.drawFastVLine(x + w - 1, 0, H, COL_DIM);
    cv.setTextSize(1);
    cv.setTextColor(COL_GREEN, COL_HDR2);
    cv.setCursor(x + 2, 6); cv.print("ON");
    cv.setCursor(x + 2, 16); cv.printf("%u", countOnline());
    cv.setTextColor(COL_DIM, COL_HDR2);
    cv.setCursor(x + 2, 36); cv.print("ALL");
    cv.setCursor(x + 2, 46); cv.printf("%u", s_seenEver);
    cv.setCursor(x + 2, H - 10); cv.print(";");
}

static void drawCollapsedRight(M5Canvas& cv, int x, int w, int H) {
    cv.fillRect(x, 0, w, H, COL_HDR2);
    cv.drawFastVLine(x, 0, H, COL_DIM);
    cv.setTextSize(1);
    uint16_t col = s_haveSig ? sigCol(s_lastRssi) : COL_DIM;
    cv.setTextColor(col, COL_HDR2);
    cv.setCursor(x + 2, 6);
    if (s_haveSig) cv.printf("%d", s_lastRssi);
    else cv.print("--");
    cv.setTextColor(COL_YELLOW, COL_HDR2);
    cv.setCursor(x + 2, 18);
    if (s_haveSig) cv.printf("%+d", s_lastSnr);
    else cv.print("snr");
    cv.setTextColor(s_rs == RS_TX ? COL_PINK : (s_radioOk ? COL_GREEN : COL_RED), COL_HDR2);
    cv.setCursor(x + 2, 34);
    cv.print(s_rs == RS_TX ? "TX" : (s_radioOk ? "RX" : "NO"));
    cv.setTextColor(COL_DIM, COL_HDR2);
    cv.setCursor(x + 2, H - 10); cv.print(".");
}

static void drawPeople(M5Canvas& cv, int x, int y, int w, int h) {
    cv.fillRect(x, y, w, h, COL_BG);
    cv.fillRect(x, y, w, LH + 1, COL_HDR);
    cv.setTextColor(COL_CYAN, COL_HDR);
    cv.setCursor(x + 2, y + 1);
    cv.printf("PEOPLE  on %u  all %u", countOnline(), s_seenEver);
    if (!s_nodes || s_nodeN == 0) {
        cv.setTextColor(COL_DIM, COL_BG);
        cv.setCursor(x + 2, y + LH + 6);
        cv.print("nobody yet");
        return;
    }
    int rows = (h - LH - 2) / LH;
    if (rows < 1) rows = 1;
    if (s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
    int top = s_nodeScroll;
    if (top > (int)s_nodeN - rows) top = (int)s_nodeN - rows;
    if (top < 0) top = 0;
    for (int i = 0; i < rows && top + i < s_nodeN; i++) {
        MeshNode& n = s_nodes[top + i];
        int yy = y + LH + 2 + i * LH;
        bool sel = (uint8_t)(top + i) == s_nodeScroll;
        if (sel) cv.fillRect(x, yy - 1, w, LH, COL_SEL);
        char name[8];
        if (n.shortName[0]) snprintf(name, sizeof(name), "%s", n.shortName);
        else snprintf(name, sizeof(name), "!%04lx", (unsigned long)(n.nodeNum & 0xFFFF));
        char age[8]; ageStr(n.lastSeen, age, sizeof(age));
        cv.setTextColor(n.isSelf ? COL_PINK : COL_TEXT, sel ? COL_SEL : COL_BG);
        cv.setCursor(x + 2, yy);
        cv.printf("%-4s", name);
        cv.setTextColor(sigCol(n.rssi), sel ? COL_SEL : COL_BG);
        cv.printf(" %4d", n.rssi);
        cv.setTextColor(COL_DIM, sel ? COL_SEL : COL_BG);
        cv.printf(" %s", age);
    }
}

static void drawChatPane(M5Canvas& cv, int x, int y, int w, int h, bool dm) {
    cv.fillRect(x, y, w, h, COL_BG);
    cv.fillRect(x, y, w, LH + 1, COL_HDR);
    cv.setTextColor(COL_CYAN, COL_HDR);
    cv.setCursor(x + 2, y + 1);
    if (dm) cv.printf("DM !%04lx", (unsigned long)(s_dmNode & 0xFFFF));
    else cv.printf("CHAT %s", Config::mesh().channel);
    uint8_t nmsg = dm ? s_dmN : s_chatN;
    int rows = (h - LH - 12) / LH;
    if (rows < 1) rows = 1;
    uint8_t& scroll = dm ? s_dmScroll : s_chatScroll;
    if (nmsg > rows && scroll + rows > nmsg) scroll = (uint8_t)(nmsg - rows);
    for (int i = 0; i < rows; i++) {
        uint8_t idx = (uint8_t)(scroll + i);
        Msg* m = dm ? dmGet(idx) : chatGet(idx);
        if (!m) break;
        int yy = y + LH + 2 + i * LH;
        cv.setTextColor(m->sent ? COL_GREEN : COL_TEXT, COL_BG);
        cv.setCursor(x + 2, yy);
        char line[42];
        snprintf(line, sizeof(line), "%s:%s", m->fromName[0] ? m->fromName : "?", m->text);
        int maxc = (w - 4) / 6;
        if (maxc < 4) maxc = 4;
        if ((int)strlen(line) > maxc) line[maxc] = 0;
        cv.print(line);
    }
    if (nmsg == 0) {
        cv.setTextColor(!dm && s_radioError[0] ? COL_RED : COL_DIM, COL_BG);
        cv.setCursor(x + 2, y + LH + 6);
        if (!dm && s_radioError[0]) cv.print(s_radioError);
        else cv.print(dm ? "no direct msgs" : "channel is quiet");
    }
    cv.setTextColor(COL_DIM, COL_BG);
    cv.setCursor(x + 2, y + h - 9);
    cv.print(dm ? "T write  ,/ scroll" : "T type  ; people  . radio");
}

static void drawRadio(M5Canvas& cv, int x, int y, int w, int h) {
    cv.fillRect(x, y, w, h, COL_BG);
    cv.fillRect(x, y, w, LH + 1, COL_HDR);
    cv.setTextColor(s_radioOk ? COL_GREEN : COL_RED, COL_HDR);
    cv.setCursor(x + 2, y + 1);
    cv.print(s_radioOk ? "LORA OK" : "LORA ??");
    const MeshConfig& mc = Config::mesh();
    float freq=0, bw=0; uint8_t sf=0, cr=0; int8_t power=0;
    bool planned = planRadio(mc, freq, bw, sf, cr, power);
    char rows[12][28];
    int n = 0;
    if (!s_radioOk && s_radioError[0]) {
        snprintf(rows[n++], 28, "%s", s_radioError);
    }
    if (planned) snprintf(rows[n++], 28, "%.3f MHz%s", freq, mc.freqOverride ? " *" : "");
    snprintf(rows[n++], 28, "SF%u BW%.0f CR4/%u", sf, bw, cr);
    snprintf(rows[n++], 28, "%ddBm  hop %u", power, mc.hopLimit);
    snprintf(rows[n++], 28, "%s %s", meshRegionName(mc.region), meshModemName(mc.modem));
    snprintf(rows[n++], 28, "ch %s", mc.channel);
    if (s_haveSig) snprintf(rows[n++], 28, "last %d dBm %+d", s_lastRssi, s_lastSnr);
    else snprintf(rows[n++], 28, "last no packet");
    snprintf(rows[n++], 28, "rx %u  tx %u  q %u", s_rxPkts, s_txPkts, s_txN);
    snprintf(rows[n++], 28, "!%04lx %s", (unsigned long)(s_nodeNum & 0xFFFF), mc.shortName);
    snprintf(rows[n++], 28, "sync 2B  %s", s_rfExpanderReady ? "ant on" : "IOE mute");
    int rowsFit = (h - LH - 2) / LH;
    cv.setTextColor(COL_TEXT, COL_BG);
    for (int i = 0; i < n && i < rowsFit; i++) {
        cv.setCursor(x + 2, y + LH + 2 + i * LH);
        cv.print(rows[i]);
    }
}

static void drawPanes(M5Canvas& cv, int W, int H) {
    int lw, cw, rw;
    if (s_pane == PANE_LEFT) { lw = W - 78; cw = 36; rw = 42; }
    else if (s_pane == PANE_RIGHT) { lw = 34; cw = 36; rw = W - 70; }
    else { lw = 34; rw = 42; cw = W - lw - rw; }
    if (s_view == V_DM) { lw = 34; rw = 42; cw = W - lw - rw; }
    drawCollapsedLeft(cv, 0, lw, H);
    if (s_pane == PANE_LEFT && s_view != V_DM) drawPeople(cv, 0, 0, lw, H);
    int cx = lw;
    if (s_pane == PANE_CENTER || s_view == V_DM) {
        drawChatPane(cv, cx, 0, cw, H, s_view == V_DM);
    } else {
        cv.fillRect(cx, 0, cw, H, COL_HDR2);
        cv.setTextColor(COL_DIM, COL_HDR2);
        cv.setCursor(cx + 2, 6); cv.print("CH");
        cv.setCursor(cx + 2, 16); cv.printf("%u", s_chatN);
    }
    int rx = cx + cw;
    if (s_pane == PANE_RIGHT && s_view != V_DM) drawRadio(cv, rx, 0, rw, H);
    else drawCollapsedRight(cv, rx, rw, H);
    if (s_pane == PANE_LEFT) cv.drawRect(0, 0, lw, H, COL_CYAN);
    else if (s_pane == PANE_RIGHT && s_view != V_DM) cv.drawRect(rx, 0, rw, H, COL_CYAN);
    else cv.drawRect(cx, 0, cw, H, COL_CYAN);
}

void draw(M5Canvas& canvas){
    uint32_t now=(uint32_t)millis();
    if(s_view!=V_COMPOSE && now-s_lastDraw<120) return;
    s_lastDraw=now;

    int W=canvas.width(), H=canvas.height();
    canvas.fillScreen(COL_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);

    if (s_view == V_COMPOSE) { drawCompose(canvas, W, H); return; }
    drawPanes(canvas, W, H);
}

static bool keyHas(char a, char b = 0) {
    if (M5Cardputer.Keyboard.isKeyPressed(a)) return true;
    if (b && M5Cardputer.Keyboard.isKeyPressed(b)) return true;
    for (char c : M5Cardputer.Keyboard.keysState().word) {
        if (c == a || (b && c == b)) return true;
    }
    return false;
}
static bool keyPaneLeft()  { return keyHas(';'); }
static bool keyPaneRight() { return keyHas('.'); }
static bool keyUp()        { return M5Cardputer.Keyboard.keysState().fn || keyHas(','); }
static bool keyDown()      { return keyHas('/'); }

void handleKeys(){
    if(!s_run) return;
    // App::loop already reads isChange() for the dimmer and for
    // Backspace-minimize. A second isChange() here is always false,
    // so the mesh keys never ran. keyNewPress keeps its own latch.
    static bool keyWas = false;
    if(!keyNewPress(keyWas)) return;
    auto ks=M5Cardputer.Keyboard.keysState();
    // Backspace minimizes every other tool. In the compose box it
    // deletes a letter instead, and the app loop skips minimize.
    const bool esc=keyEsc();
    s_lastDraw = 0;

    if(s_view==V_COMPOSE){
        if(ks.enter){
            if(s_cmpLen>0){
                s_cmpBuf[s_cmpLen]=0;
                uint32_t dest=(s_composeFrom==V_DM)?s_dmNode:BROADCAST_ID;
                sendText(s_cmpBuf,dest);
                if(s_composeFrom==V_DM){ if(s_dmN>1) s_dmScroll=(uint8_t)(s_dmN-1); }
                else if(s_chatN>1) s_chatScroll=(uint8_t)(s_chatN-1);
            }
            s_view=s_composeFrom; s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
        } else if(ks.del && s_cmpLen>0){
            s_cmpBuf[--s_cmpLen]=0;
        } else if(esc){
            s_view=s_composeFrom; s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
        } else {
            for(char c:ks.word) if(c>=0x20&&c<0x7F&&s_cmpLen<47){
                s_cmpBuf[s_cmpLen++]=c; s_cmpBuf[s_cmpLen]=0;
            }
        }
        return;
    }

    if(s_view==V_DM){
        if(esc){ s_view=V_CHAT; s_pane=PANE_LEFT; return; }
        if(keyUp() && s_dmScroll>0) s_dmScroll--;
        if(keyDown() && s_dmN && s_dmScroll+1<s_dmN) s_dmScroll++;
        for(char c:ks.word) if(c=='t'||c=='T'){
            s_composeFrom=V_DM; s_view=V_COMPOSE;
            s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
            return;
        }
        return;
    }

    if(keyPaneLeft()){
        s_pane = (s_pane==PANE_LEFT) ? PANE_CENTER : PANE_LEFT;
        return;
    }
    if(keyPaneRight()){
        s_pane = (s_pane==PANE_RIGHT) ? PANE_CENTER : PANE_RIGHT;
        return;
    }
    if(esc){
        if(s_pane!=PANE_CENTER){ s_pane=PANE_CENTER; return; }
        MeshPig::stop();
        return;
    }

    if(s_pane==PANE_LEFT && s_nodes && s_nodeN){
        if(keyUp() && s_nodeScroll>0) s_nodeScroll--;
        else if(keyDown() && s_nodeScroll+1<s_nodeN) s_nodeScroll++;
        else if(ks.enter && !s_nodes[s_nodeScroll].isSelf && s_radioOk){
            dmLoad(s_nodes[s_nodeScroll].nodeNum);
            s_dmScroll=0; s_view=V_DM; s_dmNew=0;
        }
        return;
    }
    if(s_pane==PANE_CENTER){
        if(keyUp() && s_chatScroll>0) s_chatScroll--;
        else if(keyDown() && s_chatN && s_chatScroll+1<s_chatN) s_chatScroll++;
        else {
            for(char c:ks.word) if(c=='t'||c=='T'){
                s_composeFrom=V_CHAT; s_view=V_COMPOSE;
                s_cmpLen=0; memset(s_cmpBuf,0,sizeof(s_cmpBuf));
                return;
            }
        }
    }
}

bool isComposing() { return s_run && s_view == V_COMPOSE; }

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
