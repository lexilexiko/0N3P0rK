// net/ap_sta.h
// AP, STA, or AP+STA (same radio / same channel).

#pragma once

#include <Arduino.h>
#include <WiFi.h>

namespace Net {

enum class Mode {
    AP,
    STA,
    APSTA,  // stay on OneLPig AP while connected to another WiFi
};

struct Status {
    Mode mode;
    bool connected;       // AP: AP up; STA/APSTA: STA associated
    bool staConnected;
    bool napt;
    char ssid[33];        // primary: AP ssid or STA ssid
    char apSsid[33];
    char staSsidShow[33];
    char ip[16];          // AP IP, or STA IP if STA-only
    char apIp[16];
    char staIp[16];
    char rssi[8];
    uint8_t apClients;
    char mac[18];
};

struct Cfg {
    Mode mode;
    char apSsid[33];
    char apPass[65];
    uint8_t apChannel;
    char staSsid[33];
    char staPass[65];
    char wpaSecKey[33];
    char pwncrackKey[65];
    // OnlineHashCrack public WPA API credential: the address of an existing
    // account. No API key — unknown addresses answer HTTP 401 per upload.
    char ohcEmail[65];
};

static const char* const AP_SSID_IDLE = "OneLPig";
static const char* const AP_SSID_CAP  = "OneLPig AGG";
static const char* const AP_PASS_DEFAULT = "onelpig123";

void begin();
const Cfg& cfg();
Status status();

bool setMode(Mode m);
bool setAp(const char* ssid, const char* pass, uint8_t channel);
bool setSta(const char* ssid, const char* pass);
void clearSta();
bool hasStaCreds();
bool staLinked();   // STA associated (STA or APSTA)

// Home STA for WPA-SEC / Pwncrack. Does not WIFI_OFF (kills DNS/TLS).
bool joinHome(uint32_t timeoutMs = 22000);
void leaveHome();
bool resolveHost(const char* host, IPAddress& ip, uint8_t tries = 3);

bool setWpaSecKey(const char* key);
bool setPwncrackKey(const char* key);
// OnlineHashCrack credential. Empty string clears it. Returns false when the
// address does not look usable, so a typo is caught before the upload runs.
bool setOhcEmail(const char* email);

void save();
void loadDefaults();

bool setApSsidTemporary(const char* ssid);
bool restoreApRadio();

} // namespace Net
