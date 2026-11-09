// mesh_pig.h — Meshtastic-compatible LoRa mesh for 0N3P0rK
// SX1262 (CAP LoRa-1262) via RadioLib.
// OTA protocol: 16-byte Meshtastic header + AES-128-CTR(nanopb MeshPacket payload).
// LongFast EU868: 869.525 MHz, SF11, BW250 kHz, CR4/5, sync 0x2B
// Default PSK: d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01
#pragma once

#include <Arduino.h>
#include <stdint.h>

// ── Node entry ───────────────────────────────────────────────────────────────
struct MeshNode {
    uint32_t nodeNum   = 0;
    int32_t  lat_i     = 0;    // degrees × 1e7
    int32_t  lon_i     = 0;
    int16_t  alt       = 0;
    int8_t   snr       = 0;
    int16_t  rssi      = 0;
    uint32_t lastSeen  = 0;
    char     shortName[5]  = {};
    char     longName[20]  = {};
    bool     hasPos    = false;
    bool     isSelf    = false;
};

// ── Message log entry ────────────────────────────────────────────────────────
struct MeshMsg {
    uint32_t from      = 0;
    uint32_t time      = 0;
    char     text[48]  = {};
    char     fromName[8] = {};
    bool     broadcast = true;
};

// ── MeshPig namespace ────────────────────────────────────────────────────────
namespace MeshPig {

// Lifecycle
void start();
void stop();
void update();          // call every loop()
void requestNodeInfo();
bool validateChannelKey(const char* base64Key);
uint8_t regionCount();
const char* regionName(uint8_t region);
uint32_t frequencyKHz();
bool validateFrequency(uint8_t region, uint32_t frequencyKHz);
bool isRunning();

// Node table (ring, max 16)
uint8_t   nodeCount();
MeshNode* nodeAt(uint8_t i);    // 0-based, newest first

// Message log (circular, max 24)
uint8_t   msgCount();
MeshMsg*  msgAt(uint8_t i);     // 0-based, newest first
uint8_t   newMsgCount();
void      clearNew();

// Our identity
uint32_t    selfNodeNum();
const char* selfShortName();

// Send a text broadcast (to=0xFFFFFFFF)
bool sendText(const char* text);

// Status summary for GPS mode status bar
const char* getStatusLine();

}  // namespace MeshPig
