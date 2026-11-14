// mesh_pig.h — Meshtastic-compatible LoRa mesh for 0N3P0rK
// Modem: mesh_pig.cpp. Messages and keymap: mesh_text.cpp. Screen: mesh_ui.cpp.
// SX1262 (CAP LoRa-1262) via RadioLib.
// OTA protocol: 16-byte Meshtastic header + AES-128-CTR(nanopb MeshPacket payload).
// LongFast EU868: 869.525 MHz, SF11, BW250 kHz, CR4/5, sync 0x2B
// Default PSK: d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <M5GFX.h>

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
    uint8_t  pubKey[32] = {};  // peer X25519 public key, when hasKey
    bool     hasKey    = false;
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
void update();          // call every loop(), including while another screen is open
bool isRunning();
// Bytes held in heap. Zero unless MESH is enabled and the radio came up.
size_t heapHeld();

// Node table (ring, max 16)
uint8_t   nodeCount();
MeshNode* nodeAt(uint8_t i);    // 0-based

// Message log (circular, max 32)
uint8_t   msgCount();
MeshMsg*  msgAt(uint8_t i);     // 0-based, oldest first
uint8_t   newMsgCount();
void      clearNew();

// Our identity
uint32_t    selfNodeNum();
const char* selfShortName();

// Send text message
//   to = 0xFFFFFFFF → broadcast (channel chat)
//   to = nodeNum    → direct message
bool sendText(const char* text, uint32_t to = 0xFFFFFFFFu);

// Draw the MESH screen onto the 240x105 main canvas.
void draw(M5Canvas& canvas);

// Handle keyboard input. Esc on the tab strip leaves the screen.
void handleKeys();
// True while a text box is open. Backspace deletes one letter then.
bool isComposing();
// ASCII hint for the real bottom bar.
const char* bottomHint();

// Status summary for the status bar
const char* getStatusLine();

}  // namespace MeshPig
