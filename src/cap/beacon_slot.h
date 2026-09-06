// One tracked AP, shared between the orchestrator (cap/sniffer.cpp, which
// owns the array) and every capture method in cap/ (which only read
// it). Moved out of sniffer.cpp so methods don't need to reach into its
// internals to see what they're aiming at.
#pragma once

#include <stdint.h>

namespace Cap {

static const uint16_t BEACON_MAX = 400;

struct BeaconSlot {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint16_t len;
    char     ssid[33];
    // Per-AP client list. Bumped from 4 -> 20 to match M5PORKCHOP's
    // MAX_CLIENTS_PER_NETWORK; busy APs (offices, cafes, classrooms) easily
    // have more than 4 active stations and we don't want to ignore them.
    // 20 * 6 = 120 bytes per slot; 16 slots in the sniffer pool = +1536 B
    // of BSS, still comfortable on the StampS3's internal SRAM.
    uint8_t  clients[20][6];
    uint8_t  clientN;
    bool     pmfCapable;   // MFPC/MFPR bit seen in RSN IE -> deauth/disassoc ignored
    // Recent non-EAPOL data frames for this BSSID (sniffer bumps when
    // RadioConfig::dataAct is on). FOCUS uses this for the activity term
    // instead of beacon-only bumps. Decayed by the method / orchestrator.
    uint16_t dataRecent;
    uint8_t  frame[BEACON_MAX];
};

} // namespace Cap
