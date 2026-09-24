// net/ap_sta.cpp
#include "ap_sta.h"
#include <WiFi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <string.h>
#include <ctype.h>
#include "lwip/lwip_napt.h"

namespace Net {

static Cfg s_cfg{};
static Status s_status{};
static char s_liveApSsid[33];
static bool s_liveApSsidActive = false;
static bool s_napt = false;

static const char* NVS_NS = "pork";
static const char* KEY_MODE     = "mode";
static const char* KEY_AP_SSID  = "apssid";
static const char* KEY_AP_PASS  = "appass";
static const char* KEY_AP_CH    = "apch";
static const char* KEY_STA_SSID = "stssid";
static const char* KEY_STA_PASS = "stpass";
static const char* KEY_WPASEC   = "wpakey";
static const char* KEY_PWN      = "pwnkey";
static const char* KEY_OHC      = "ohcmail";

static void nvsGetStr(nvs_handle_t h, const char* key, char* dst, size_t dstLen, const char* defv) {
    size_t len = dstLen;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strncpy(dst, defv, dstLen - 1);
        dst[dstLen - 1] = '\0';
    }
}

static void nvsGetU8(nvs_handle_t h, const char* key, uint8_t* dst, uint8_t defv) {
    if (nvs_get_u8(h, key, dst) != ESP_OK) *dst = defv;
}

static void nvsGetI32(nvs_handle_t h, const char* key, int32_t* dst, int32_t defv) {
    if (nvs_get_i32(h, key, dst) != ESP_OK) *dst = defv;
}

static void startMdns() {
    MDNS.end();
    MDNS.begin("onelpig");
    MDNS.addService("http", "tcp", 80);
}

static void enableNapt() {
    s_napt = false;
#if defined(IP_NAPT) && IP_NAPT
    delay(150);
    ip_napt_enable(WiFi.softAPIP(), 1);
    s_napt = true;
    Serial.println("[NET] NAPT on (phone internet shared)");
#else
    Serial.println("[NET] NAPT not in this SDK - AP+STA keeps UI + Stamp internet");
#endif
}

static bool startAp(const char* ssid) {
    wifi_mode_t m = (s_cfg.mode == Mode::APSTA) ? WIFI_AP_STA : WIFI_AP;
    WiFi.mode(m);
    WiFi.softAPdisconnect(false);
    bool ok = WiFi.softAP(ssid, s_cfg.apPass, s_cfg.apChannel, 0, 8);
    Serial.printf("[NET] AP start: ssid=%s pass=%s ch=%u -> %s\n",
                  ssid, s_cfg.apPass[0] ? s_cfg.apPass : "(open)",
                  s_cfg.apChannel, ok ? "OK" : "FAIL");
    return ok;
}

static Mode modeFromInt(int32_t v) {
    if (v == (int32_t)Mode::STA || v == (int32_t)Mode::APSTA) return Mode::APSTA;
    return Mode::AP;
}

void loadDefaults() {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.mode = Mode::AP;
    strncpy(s_cfg.apSsid, AP_SSID_IDLE, sizeof(s_cfg.apSsid) - 1);
    strncpy(s_cfg.apPass, AP_PASS_DEFAULT, sizeof(s_cfg.apPass) - 1);
    s_cfg.apChannel = 6;
}

static void loadFromNvs() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        loadDefaults();
        return;
    }
    int32_t mode = (int32_t)Mode::AP;
    nvsGetI32(h, KEY_MODE, &mode, (int32_t)Mode::AP);
    s_cfg.mode = modeFromInt(mode);
    nvsGetStr(h, KEY_AP_SSID, s_cfg.apSsid, sizeof(s_cfg.apSsid), AP_SSID_IDLE);
    nvsGetStr(h, KEY_AP_PASS, s_cfg.apPass, sizeof(s_cfg.apPass), "");
    nvsGetU8(h, KEY_AP_CH, &s_cfg.apChannel, 6);
    nvsGetStr(h, KEY_STA_SSID, s_cfg.staSsid, sizeof(s_cfg.staSsid), "");
    nvsGetStr(h, KEY_STA_PASS, s_cfg.staPass, sizeof(s_cfg.staPass), "");
    nvsGetStr(h, KEY_WPASEC, s_cfg.wpaSecKey, sizeof(s_cfg.wpaSecKey), "");
    nvsGetStr(h, KEY_PWN, s_cfg.pwncrackKey, sizeof(s_cfg.pwncrackKey), "");
    nvsGetStr(h, KEY_OHC, s_cfg.ohcEmail, sizeof(s_cfg.ohcEmail), "");
    nvs_close(h);
    bool migrated = false;
    if (s_cfg.apSsid[0] == '\0' ||
        strcmp(s_cfg.apSsid, "OnePork Start") == 0 ||
        strcmp(s_cfg.apSsid, "OnePork Stop") == 0 ||
        strcmp(s_cfg.apSsid, "Porkchop-AP") == 0) {
        strncpy(s_cfg.apSsid, AP_SSID_IDLE, sizeof(s_cfg.apSsid) - 1);
        s_cfg.apSsid[sizeof(s_cfg.apSsid) - 1] = '\0';
        migrated = true;
    }
    if (s_cfg.apPass[0] == '\0') {
        strncpy(s_cfg.apPass, AP_PASS_DEFAULT, sizeof(s_cfg.apPass) - 1);
        s_cfg.apPass[sizeof(s_cfg.apPass) - 1] = '\0';
        migrated = true;
    }
    if (s_cfg.apChannel < 1 || s_cfg.apChannel > 13) s_cfg.apChannel = 6;
    if (migrated) save();
}

void save() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        Serial.println("[NET] nvs_open RW failed");
        return;
    }
    nvs_set_i32(h, KEY_MODE, (int32_t)s_cfg.mode);
    nvs_set_str(h, KEY_AP_SSID, s_cfg.apSsid);
    nvs_set_str(h, KEY_AP_PASS, s_cfg.apPass);
    nvs_set_u8(h, KEY_AP_CH, s_cfg.apChannel);
    nvs_set_str(h, KEY_STA_SSID, s_cfg.staSsid);
    nvs_set_str(h, KEY_STA_PASS, s_cfg.staPass);
    nvs_set_str(h, KEY_WPASEC, s_cfg.wpaSecKey);
    nvs_set_str(h, KEY_PWN, s_cfg.pwncrackKey);
    nvs_set_str(h, KEY_OHC, s_cfg.ohcEmail);
    nvs_commit(h);
    nvs_close(h);
    Serial.println("[NET] saved");
}

static void ipToBuf(IPAddress ip, char* dst, size_t dstLen) {
    snprintf(dst, dstLen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void refreshStatus() {
    s_status.mode = s_cfg.mode;
    s_status.napt = s_napt;
    s_status.staConnected = (WiFi.status() == WL_CONNECTED);

    const char* apName = (s_liveApSsidActive && s_liveApSsid[0]) ? s_liveApSsid : s_cfg.apSsid;
    strncpy(s_status.apSsid, apName, sizeof(s_status.apSsid) - 1);
    s_status.apSsid[sizeof(s_status.apSsid) - 1] = '\0';
    ipToBuf(WiFi.softAPIP(), s_status.apIp, sizeof(s_status.apIp));
    strncpy(s_status.staSsidShow, s_cfg.staSsid, sizeof(s_status.staSsidShow) - 1);
    s_status.staSsidShow[sizeof(s_status.staSsidShow) - 1] = '\0';

    if (s_status.staConnected) {
        ipToBuf(WiFi.localIP(), s_status.staIp, sizeof(s_status.staIp));
        snprintf(s_status.rssi, sizeof(s_status.rssi), "%ddBm", (int)WiFi.RSSI());
    } else {
        strcpy(s_status.staIp, "0.0.0.0");
        s_status.rssi[0] = '\0';
    }

    if (s_cfg.mode == Mode::APSTA) {
        s_status.connected = s_status.staConnected;
        strncpy(s_status.ssid, s_cfg.staSsid[0] ? s_cfg.staSsid : s_status.apSsid,
                sizeof(s_status.ssid) - 1);
        strncpy(s_status.ip, s_status.apIp, sizeof(s_status.ip) - 1);
        s_status.apClients = WiFi.softAPgetStationNum();
    } else {
        s_status.connected = (WiFi.getMode() & WIFI_AP) != 0;
        strncpy(s_status.ssid, s_status.apSsid, sizeof(s_status.ssid) - 1);
        strncpy(s_status.ip, s_status.apIp, sizeof(s_status.ip) - 1);
        s_status.apClients = WiFi.softAPgetStationNum();
        s_status.rssi[0] = '\0';
    }
    s_status.ssid[sizeof(s_status.ssid) - 1] = '\0';
    s_status.ip[sizeof(s_status.ip) - 1] = '\0';

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_status.mac, sizeof(s_status.mac),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void applyMode() {
    s_liveApSsidActive = false;
    s_napt = false;
    if (s_cfg.mode == Mode::AP) {
        startAp(s_cfg.apSsid);
    } else {
        WiFi.mode(WIFI_AP_STA);
        delay(50);
        bool apOk = WiFi.softAP(s_cfg.apSsid, s_cfg.apPass, s_cfg.apChannel, 0, 8);
        Serial.printf("[NET] APSTA AP=%s -> %s\n", s_cfg.apSsid, apOk ? "OK" : "FAIL");
        if (s_cfg.staSsid[0]) {
            WiFi.begin(s_cfg.staSsid, s_cfg.staPass);
            Serial.printf("[NET] APSTA joining %s\n", s_cfg.staSsid);
        }
        enableNapt();
    }
    startMdns();
}

void begin() {
    loadFromNvs();
    // No web AP. Keep the driver from the boot fence; do not start SoftAP.
    refreshStatus();
}

const Cfg& cfg() { return s_cfg; }
Status status() { refreshStatus(); return s_status; }

bool hasStaCreds() {
    return s_cfg.staSsid[0] != '\0';
}

bool staLinked() {
    return WiFi.status() == WL_CONNECTED;
}

bool setMode(Mode m) {
    if (m == Mode::STA) m = Mode::APSTA;
    if (m != Mode::AP && m != Mode::APSTA) return false;
    if (m == Mode::APSTA && !hasStaCreds()) {
        Serial.println("[NET] STA/APSTA requested but no creds");
        return false;
    }
    s_cfg.mode = m;
    applyMode();
    save();
    refreshStatus();
    return true;
}

bool setAp(const char* ssid, const char* pass, uint8_t channel) {
    if (!ssid || !*ssid || strlen(ssid) > 32) return false;
    if (pass && strlen(pass) > 0 && strlen(pass) < 8) return false;
    strncpy(s_cfg.apSsid, ssid, sizeof(s_cfg.apSsid) - 1);
    s_cfg.apSsid[sizeof(s_cfg.apSsid) - 1] = '\0';
    if (pass) {
        strncpy(s_cfg.apPass, pass, sizeof(s_cfg.apPass) - 1);
        s_cfg.apPass[sizeof(s_cfg.apPass) - 1] = '\0';
    }
    s_cfg.apChannel = (channel >= 1 && channel <= 13) ? channel : 6;
    s_liveApSsidActive = false;
    if (s_cfg.mode == Mode::AP || s_cfg.mode == Mode::APSTA) applyMode();
    save();
    return true;
}

bool setSta(const char* ssid, const char* pass) {
    if (!ssid || !*ssid || strlen(ssid) > 32) return false;
    strncpy(s_cfg.staSsid, ssid, sizeof(s_cfg.staSsid) - 1);
    s_cfg.staSsid[sizeof(s_cfg.staSsid) - 1] = '\0';
    if (pass) {
        strncpy(s_cfg.staPass, pass, sizeof(s_cfg.staPass) - 1);
        s_cfg.staPass[sizeof(s_cfg.staPass) - 1] = '\0';
    } else {
        s_cfg.staPass[0] = '\0';
    }
    save();
    if (s_cfg.mode == Mode::APSTA) applyMode();
    return true;
}

void clearSta() {
    s_cfg.staSsid[0] = '\0';
    s_cfg.staPass[0] = '\0';
    save();
}

static bool validHex32(const char* key) {
    if (!key || strlen(key) != 32) return false;
    for (int i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)key[i])) return false;
    }
    return true;
}

bool setWpaSecKey(const char* key) {
    if (!key || !key[0]) {
        s_cfg.wpaSecKey[0] = '\0';
        save();
        return true;
    }
    if (!validHex32(key)) return false;
    strncpy(s_cfg.wpaSecKey, key, sizeof(s_cfg.wpaSecKey) - 1);
    s_cfg.wpaSecKey[sizeof(s_cfg.wpaSecKey) - 1] = '\0';
    save();
    return true;
}

bool setPwncrackKey(const char* key) {
    if (!key) return false;
    size_t n = strlen(key);
    if (n > 64) return false;
    if (n > 0 && (n < 4)) return false;
    for (size_t i = 0; i < n; i++) {
        if (key[i] < 0x20 || key[i] > 0x7E) return false;
    }
    strncpy(s_cfg.pwncrackKey, key, sizeof(s_cfg.pwncrackKey) - 1);
    s_cfg.pwncrackKey[sizeof(s_cfg.pwncrackKey) - 1] = '\0';
    save();
    return true;
}

// OnlineHashCrack credential. The public WPA API has no key: the email of an
// existing account is what identifies the submission. Accept anything shaped
// like a single-@ address with a dotted host; the server answers HTTP 401 for
// addresses it does not know, which is reported per upload.
bool setOhcEmail(const char* email) {
    if (!email || !email[0]) {
        s_cfg.ohcEmail[0] = '\0';
        save();
        return true;
    }
    size_t n = strlen(email);
    if (n < 6 || n > 64) return false;
    uint8_t atCount = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)email[i];
        if (c <= 0x20 || c >= 0x7F) return false;
        if (c == '@') atCount++;
    }
    if (atCount != 1) return false;
    const char* at = strchr(email, '@');
    if (!at || at == email) return false;
    const char* dot = strrchr(at + 1, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0') return false;
    strncpy(s_cfg.ohcEmail, email, sizeof(s_cfg.ohcEmail) - 1);
    s_cfg.ohcEmail[sizeof(s_cfg.ohcEmail) - 1] = '\0';
    save();
    return true;
}

bool setApSsidTemporary(const char* ssid) {
    if (!ssid || !*ssid || strlen(ssid) > 32) return false;
    if (s_cfg.mode != Mode::AP && s_cfg.mode != Mode::APSTA) return false;
    strncpy(s_liveApSsid, ssid, sizeof(s_liveApSsid) - 1);
    s_liveApSsid[sizeof(s_liveApSsid) - 1] = '\0';
    s_liveApSsidActive = true;
    bool ok = startAp(s_liveApSsid);
    Serial.printf("[NET] live AP SSID=%s -> %s\n", s_liveApSsid, ok ? "OK" : "FAIL");
    return ok;
}

bool restoreApRadio() {
    if (s_cfg.mode != Mode::AP && s_cfg.mode != Mode::APSTA) return false;
    s_liveApSsidActive = false;
    bool ok = startAp(s_cfg.apSsid);
    Serial.printf("[NET] AP radio restored ssid=%s ch=%u -> %s\n",
                  s_cfg.apSsid, s_cfg.apChannel, ok ? "OK" : "FAIL");
    return ok;
}

bool resolveHost(const char* host, IPAddress& ip, uint8_t tries) {
    if (!host || !host[0]) return false;
    for (uint8_t i = 0; i < tries; i++) {
        if (WiFi.hostByName(host, ip) && ip != IPAddress()) {
            Serial.printf("[NET] dns %s -> %s\n", host, ip.toString().c_str());
            return true;
        }
        delay(200);
        yield();
    }
    Serial.printf("[NET] dns fail %s\n", host);
    return false;
}

bool joinHome(uint32_t timeoutMs) {
    if (!hasStaCreds()) return false;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(false, false);
    delay(80);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s_cfg.staSsid, s_cfg.staPass);
    Serial.printf("[NET] join home %s\n", s_cfg.staSsid);

    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress()) {
            delay(300);
            IPAddress ip;
            resolveHost("wpa-sec.stanev.org", ip, 2);
            resolveHost("pwncrack.org", ip, 2);
            resolveHost("api.onlinehashcrack.com", ip, 2);
            Serial.printf("[NET] home ip %s rssi %d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
        delay(150);
        yield();
    }
    Serial.println("[NET] join home timeout");
    return false;
}

void leaveHome() {
    // Keep the WiFi driver up — WIFI_OFF breaks DNS/TLS on the next sync.
    WiFi.disconnect(false, false);
}

} // namespace Net
