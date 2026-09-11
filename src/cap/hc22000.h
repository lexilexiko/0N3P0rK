// cap/hc22000.h - build hashcat 22000 / hc22000 from 802.11 frames
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Hc22000 {

void reset();
void feed(const uint8_t* frame, uint16_t len);
// Drains the in-memory dirty slots built up by feed() and writes any
// .22000 / .pmkid files to SD. Must be called from loop() context - it
// is the only place Hc22000 is allowed to do file I/O. Cheap to call
// every loop tick; it iterates at most MAX_HS slots and short-circuits
// when nothing is dirty.
void flushPending();
bool shouldPauseDeauth();
bool hasPair(const uint8_t* bssid);
uint16_t pairCount();
uint16_t convertPcap(const char* pcapPath);
uint16_t convertAllPcaps();

// Bitmask of which EAPOL messages have been seen for this BSSID:
// bit0=M1 bit1=M2 bit2=M3 bit3=M4. 0 if the BSSID isn't tracked at all.
uint8_t handshakeMask(const uint8_t* bssid);

// depth: 0 = M1+M2 only (same as hasPair() - already enough to crack),
// 1 = also require M3, 2 = require the full 4-way (M1..M4). Always
// requires hasPair() first regardless of depth, so this can only ever be
// stricter than hasPair(), never looser.
bool hasHandshake(const uint8_t* bssid, uint8_t depth);

} // namespace Hc22000
