#pragma once
// Private glue for the modem, the message store, and the screen.
// Public entry points stay in mesh_pig.h.

#include "mesh_pig.h"
#include "../core/config.h"

constexpr uint32_t BROADCAST_ID = 0xFFFFFFFFu;

struct Msg {
    uint32_t from, to;
    uint32_t timeMs;
    char     text[48];
    char     fromName[8];
    bool     sent;
};

constexpr uint8_t MAX_CHAT = 32;
constexpr uint8_t MAX_DM   = 16;

enum RadioSt : uint8_t { RS_IDLE, RS_RX, RS_TX };

extern bool     s_run;
extern bool     s_radioOk;
extern bool     s_pkiReady;
extern bool     s_rfExpanderReady;
extern uint32_t s_nodeNum;
extern uint32_t s_listenSince;
extern RadioSt  s_rs;
extern uint8_t  s_txN;
extern char     s_radioError[48];
extern int16_t  s_lastRssi;
extern int8_t   s_lastSnr;
extern bool     s_haveSig;
extern uint16_t s_rxPkts;
extern uint16_t s_txPkts;
extern MeshNode* s_nodes;
extern uint8_t  s_nodeN;

extern Msg*     s_chat;
extern uint8_t  s_chatHead, s_chatN, s_chatNew;
extern Msg*     s_dm;
extern uint8_t  s_dmHead, s_dmN, s_dmNew;
extern uint32_t s_dmNode;

void chatAdd(const Msg& m);
void dmAdd(const Msg& m);
Msg* chatGet(uint8_t i);
Msg* dmGet(uint8_t i);
void dmLoad(uint32_t node);
// Modem: write the open DM ring before it is swapped, then fill it from SD.
void meshFlushDm();
void meshReloadDm(uint32_t node);

int utf8Next(const char* s, uint32_t& cp);
int utf8Fit(const char* s, int cap);
int utf8Cols(const char* s, int nbytes);
uint32_t mapLay(char c, uint8_t lang);

bool planRadio(const MeshConfig& mc, float& freq, float& bw,
               uint8_t& sf, uint8_t& cr, int8_t& power);

namespace MeshPig {
void uiArmOpen();
void uiStick();
void uiReset();
void uiClearNav();
}
