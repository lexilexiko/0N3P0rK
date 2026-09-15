// WSL Bypasser - Bypass ESP32 WiFi frame validation
#pragma once

#include <Arduino.h>

namespace WSLBypasser {

// Initialize the bypasser (call early in setup)
void init();

// Randomize MAC address (call before promiscuous mode or scan)
// Sets a locally-administered random MAC to avoid device fingerprinting
void randomizeMAC();

// Send a deauth frame (returns true if sent successfully)
bool sendDeauthFrame(const uint8_t* bssid, uint8_t channel, const uint8_t* staMac, uint8_t reason);

// Send a disassoc frame
bool sendDisassocFrame(const uint8_t* bssid, uint8_t channel, const uint8_t* staMac, uint8_t reason);

bool sendAuthentication(const uint8_t* bssid);
bool sendAssociationRequest(const uint8_t* bssid, const char* ssid);
bool sendEAPOLStart(const uint8_t* bssid, const uint8_t* clientMac);
bool sendEAPOLLogoff(const uint8_t* bssid, const uint8_t* clientMac);
bool sendCSABeacon(const uint8_t* bssid, const char* ssid,
                   uint8_t currentChan, uint8_t targetChannel, uint8_t switchCount = 1);
bool sendAuthFlood(const uint8_t* bssid, uint8_t count = 8);
void sendBidirectionalKick(const uint8_t* bssid, const uint8_t* client, uint8_t reason, uint8_t rounds);

// Session knobs — set once per capture session from Cap::begin(). Applied to
// injected deauth/disassoc serialization.
void setTxPowerDb(int8_t dbm);       // 1..20 dBm injected-frame TX power
void setBurstPattern(uint8_t pattern); // 0=STRAIGHT 1=RANDOM 2=CLUSTER 3=PULSE

// Inter-frame gaps (ms) for the currently active burst pattern. Methods
// that send raw deauth+disassoc pairs via sendRawMgmt() call these so the
// BURST knob shapes their burst too, without duplicating the logic.
uint16_t burstLegGapMs();    // gap between the deauth and disassoc frames
uint16_t burstRoundGapMs();  // gap between consecutive kick rounds

}
