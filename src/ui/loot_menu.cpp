#include "loot_menu.h"
#include "display.h"
#include "keys.h"
#include "../core/app.h"
#include "../storage/littlefs_ops.h"
#include "../sync/wpasec.h"
#include "../sync/pwncrack.h"
#include "../net/ap_sta.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../cap/sniffer.h"
#include "../cap/capture_name.h"
#include "../sync/net_io.h"
#include "../sync/tls.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

bool LootMenu::active = false;
bool LootMenu::keyWasPressed = false;
bool LootMenu::detailView = false;
bool LootMenu::syncModal = false;
bool LootMenu::diagModal = false;
LootMenu::Tab LootMenu::tab = LootMenu::Tab::WPASEC;
uint8_t LootMenu::selected = 0;
uint8_t LootMenu::scroll = 0;
uint8_t LootMenu::count = 0;
uint8_t LootMenu::page = 0;
bool LootMenu::hasMore = false;
uint16_t LootMenu::totalItems = 0;

enum class St : uint8_t { LOCAL, UPLOADED, CRACKED };

struct Row {
    char filename[48];
    char ssid[33];
    char id[18];
    char hex[13];
    uint32_t fileSize;
    bool isPMKID;
    St status;
    char password[64];
};

static Row s_rows[48];
static char s_syncText[48] = "";
static char s_syncHost[12] = "";
static char s_diag[16][42];
static uint8_t s_diagN = 0;
static uint8_t s_diagScroll = 0;
static const uint8_t VISIBLE = 4;
static const uint8_t DIAG_VIS = 7;
enum class SyncGo : uint8_t { Off, Wifi, Work };
static SyncGo s_syncGo = SyncGo::Off;
static uint8_t s_oneIdx = 0xFF;  // 0xFF = all pending, 0xFE = potfile only

static void paintSyncLive() {
    const IoXfer& x = ioXfer();
    if (x.files) {
        snprintf(s_syncText, sizeof(s_syncText), "%s %u/%u",
                 x.phase[0] ? x.phase : "...", x.file, x.files);
    }

    auto& d = M5.Display;
    const int y0 = TOP_BAR_H;
    d.fillRect(0, y0, DISPLAY_W, MAIN_H, UiStyle::BG);
    d.fillRect(0, y0 + MAIN_H - 5, DISPLAY_W, 5, UiStyle::DIRT);
    d.setTextSize(1);
    d.setTextWrap(false);
    d.setTextDatum(top_left);
    d.setTextColor(UiStyle::GOLD);
    d.setCursor(8, y0 + 8);
    d.print(s_syncHost[0] ? s_syncHost : "LOOT");

    d.setTextColor(UiStyle::TEXT);
    d.setCursor(8, y0 + 22);
    d.print(s_syncText);

    if (!x.files) return;

    char line[42];
    snprintf(line, sizeof(line), "FILE %u/%u  ok %u  fail %u",
             x.file, x.files, x.ok, x.fail);
    d.setCursor(8, y0 + 38);
    d.print(line);

    const int bx = 8, by = y0 + 56, bw = DISPLAY_W - 16, bh = 12;
    d.drawRect(bx, by, bw, bh, UiStyle::PINK);
    uint32_t den = x.size ? x.size : (uint32_t)x.files;
    uint32_t num = x.size ? x.sent : (uint32_t)x.file;
    int inner = bw - 2;
    int fill = (int)((uint64_t)inner * num / den);
    if (fill < 0) fill = 0;
    if (fill > inner) fill = inner;
    if (fill > 0) d.fillRect(bx + 1, by + 1, fill, bh - 2, UiStyle::PINK);

    if (x.size) {
        unsigned pct = (unsigned)((uint64_t)x.sent * 100u / x.size);
        snprintf(line, sizeof(line), "%u%%  %uK/%uK", pct,
                 (unsigned)((x.sent + 512) / 1024),
                 (unsigned)((x.size + 512) / 1024));
        d.setTextColor(UiStyle::DIM);
        d.setCursor(8, y0 + 74);
        d.print(line);
    }
}

static bool endsWith(const char* name, const char* suf) {
    size_t n = strlen(name), s = strlen(suf);
    if (n < s) return false;
    for (size_t i = 0; i < s; i++) {
        char a = name[n - s + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void fillIdentity(Row& r, const char* dir) {
    r.hex[0] = r.id[0] = '\0';
    if (!r.ssid[0]) r.ssid[0] = '\0';
    CapName::extractBssidHex(r.filename, r.hex);
    if (r.hex[0]) CapName::prettyMac(r.hex, r.id);

    char fromName[33] = {0};
    CapName::extractSsidFromName(r.filename, fromName);
    if (fromName[0]) strncpy(r.ssid, fromName, sizeof(r.ssid) - 1);

    const bool needSsid = !r.ssid[0] || strcasecmp(r.ssid, "HIDDEN") == 0;
    if (needSsid) {
        char fromTxt[33] = {0};
        if (CapName::readCompanionSsid(dir, r.filename, fromTxt) && fromTxt[0])
            strncpy(r.ssid, fromTxt, sizeof(r.ssid) - 1);
    }

    // Opening every .22000 on the list is what made LOOT stall. Name is enough.
    if ((!r.hex[0] || !r.ssid[0]) &&
        (endsWith(r.filename, ".22000") || endsWith(r.filename, ".hc22000"))) {
        char hex[13] = {0}, ss[33] = {0};
        if (CapName::metaFrom22000File(dir, r.filename, hex, ss)) {
            if (hex[0] && !r.hex[0]) {
                strncpy(r.hex, hex, sizeof(r.hex) - 1);
                CapName::prettyMac(r.hex, r.id);
            }
            if (ss[0] && (!r.ssid[0] || strcasecmp(r.ssid, "HIDDEN") == 0))
                strncpy(r.ssid, ss, sizeof(r.ssid) - 1);
        }
    }
}

static void formatSize(char* out, size_t len, uint32_t bytes) {
    if (bytes < 1024) snprintf(out, len, "%uB", (unsigned)bytes);
    else snprintf(out, len, "%uKB", (unsigned)(bytes / 1024));
}

static bool connectHome() {
    return Net::joinHome(22000);
}

static void dropWifi() {
    Net::leaveHome();
}

// True if this filename belongs to the current tab's file type.
static bool matchesTab(bool wpa, const char* name) {
    bool pcap = endsWith(name, ".pcap") || endsWith(name, ".pcapng") ||
                endsWith(name, ".cap");
    bool h220 = endsWith(name, ".22000") || endsWith(name, ".hc22000");
    return wpa ? pcap : h220;
}

void LootMenu::scan() {
    count = 0;
    selected = 0;
    scroll = 0;
    hasMore = false;
    if (!Storage::available()) return;

    const bool wpa = (tab == Tab::WPASEC);
    if (wpa) WPASec::loadCache();
    else Pwncrack::loadCache();

    File root = SD.open(Storage::DIR_HS);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }

    // Skip past every page before this one - cheap name-only check, no
    // identity fill or wpasec/pwncrack cache lookups, so paging stays fast
    // even with hundreds of captures on the card.
    uint16_t toSkip = (uint16_t)page * PAGE_SIZE;
    File f = root.openNextFile();
    while (f && toSkip > 0) {
        if (!f.isDirectory()) {
            const char* name = Storage::baseName(f.name());
            if (matchesTab(wpa, name)) toSkip--;
        }
        f.close();
        f = root.openNextFile();
    }

    // Fill this page - full identity fill + dedup, scoped to just the rows
    // landing here. A repeat capture of the same BSSID landing on a
    // different page than its sibling won't merge with it - same tradeoff
    // the wpasec-only pager already made for the sake of a cheap skip pass.
    while (f && count < PAGE_SIZE) {
        if (!f.isDirectory()) {
            const char* name = Storage::baseName(f.name());
            if (matchesTab(wpa, name)) {
                bool h220 = endsWith(name, ".22000") || endsWith(name, ".hc22000");
                {
                    bool dup = false;
                    for (uint8_t i = 0; i < count; i++) {
                        if (strcmp(s_rows[i].filename, name) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        Row tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        strncpy(tmp.filename, name, sizeof(tmp.filename) - 1);
                        tmp.fileSize = (uint32_t)f.size();
                        tmp.isPMKID = h220 && !endsWith(name, "_hs.22000");
                        fillIdentity(tmp, Storage::DIR_HS);
                        int same = -1;
                        if (tmp.hex[0]) {
                            for (uint8_t i = 0; i < count; i++) {
                                if (s_rows[i].hex[0] && strcmp(s_rows[i].hex, tmp.hex) == 0) {
                                    same = (int)i;
                                    break;
                                }
                            }
                        }
                        if (same >= 0) {
                            Row& o = s_rows[same];
                            bool betterName = tmp.ssid[0] && strcasecmp(tmp.ssid, "HIDDEN") != 0 &&
                                (!o.ssid[0] || strcasecmp(o.ssid, "HIDDEN") == 0);
                            bool betterFile = (!tmp.isPMKID && o.isPMKID) ||
                                (tmp.fileSize > o.fileSize && tmp.isPMKID == o.isPMKID);
                            if (betterName) strncpy(o.ssid, tmp.ssid, sizeof(o.ssid) - 1);
                            if (betterFile) {
                                strncpy(o.filename, tmp.filename, sizeof(o.filename) - 1);
                                o.fileSize = tmp.fileSize;
                                o.isPMKID = tmp.isPMKID;
                            }
                        }
                        Row& r = (same >= 0) ? s_rows[same] : s_rows[count];
                        if (same < 0) r = tmp;
                        if (wpa) {
                            const char* potSs = r.hex[0] ? WPASec::getSSID(r.hex) : nullptr;
                            if (potSs && potSs[0]) strncpy(r.ssid, potSs, sizeof(r.ssid) - 1);
                            if (!r.ssid[0]) strncpy(r.ssid, r.id[0] ? r.id : name, sizeof(r.ssid) - 1);
                            const char* pw = r.hex[0] ? WPASec::getPassword(r.hex) : "";
                            if (!pw[0] && r.ssid[0]) pw = WPASec::getPassword(r.ssid);
                            if (!pw[0] && r.id[0]) pw = WPASec::getPassword(r.id);
                            if (!pw[0]) pw = WPASec::getPassword(name);
                            if (pw && pw[0]) {
                                strncpy(r.password, pw, sizeof(r.password) - 1);
                                r.status = St::CRACKED;
                            } else if (r.status != St::CRACKED &&
                                       ((r.hex[0] && WPASec::isUploaded(r.hex)) ||
                                        WPASec::isUploaded(name) ||
                                        (r.id[0] && WPASec::isUploaded(r.id)))) {
                                r.status = St::UPLOADED;
                            } else if (r.status != St::CRACKED && r.status != St::UPLOADED) {
                                r.status = St::LOCAL;
                            }
                        } else {
                            char stem[48];
                            strncpy(stem, name, sizeof(stem) - 1);
                            stem[sizeof(stem) - 1] = '\0';
                            char* dot = strchr(stem, '.');
                            if (dot) *dot = '\0';
                            const char* pw = "";
                            if (r.hex[0]) pw = Pwncrack::getPassword(r.hex);
                            if (!pw[0] && r.ssid[0]) pw = Pwncrack::getPassword(r.ssid);
                            if (!pw[0] && r.id[0]) pw = Pwncrack::getPassword(r.id);
                            if (!pw[0]) pw = Pwncrack::getPassword(stem);
                            if (!pw[0]) pw = Pwncrack::getPassword(name);
                            if (pw && pw[0]) {
                                strncpy(r.password, pw, sizeof(r.password) - 1);
                                r.status = St::CRACKED;
                            } else if (r.status != St::CRACKED &&
                                       (Pwncrack::isUploaded(name) ||
                                        Pwncrack::isUploaded(stem) ||
                                        (r.hex[0] && Pwncrack::isUploaded(r.hex)))) {
                                r.status = St::UPLOADED;
                            } else if (r.status != St::CRACKED && r.status != St::UPLOADED) {
                                r.status = St::LOCAL;
                            }
                            if (!r.ssid[0]) strncpy(r.ssid, r.id[0] ? r.id : name, sizeof(r.ssid) - 1);
                        }
                        if (same < 0) count++;
                    }
                }
            }
        }
        f.close();
        f = root.openNextFile();
    }

    // One more matching name past this page? -> hasMore, without loading it.
    while (f) {
        if (!f.isDirectory()) {
            const char* name = Storage::baseName(f.name());
            if (matchesTab(wpa, name)) {
                hasMore = true;
                f.close();
                break;
            }
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();
    countTotal();
}

// Cheap directory pass for the summary line's "N total" - just name checks,
// no per-file wpasec/pwncrack cache lookups, so this stays fast even with
// hundreds of captures. Runs once per scan()/page change, not per frame.
void LootMenu::countTotal() {
    totalItems = 0;
    if (!Storage::available()) return;
    const bool wpa = (tab == Tab::WPASEC);
    File root = SD.open(Storage::DIR_HS);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }
    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char* name = Storage::baseName(f.name());
            if (matchesTab(wpa, name)) totalItems++;
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();
}

void LootMenu::gotoPage(uint8_t newPage, bool landOnLast) {
    page = newPage;
    scan();
    if (landOnLast && count > 0) {
        selected = (uint8_t)(count - 1);
        scroll = (selected + 1 > VISIBLE) ? (uint8_t)(selected - VISIBLE + 1) : 0;
    }
    SFX::play(SFX::MENU_CLICK);
}

void LootMenu::show() {
    active = true;
    detailView = false;
    syncModal = false;
    diagModal = false;
    keyWasPressed = true;
    page = 0;
    scan();
}

void LootMenu::openWpaSec() {
    tab = Tab::WPASEC;
    show();
}

void LootMenu::openPwncrack() {
    tab = Tab::PWNCRACK;
    show();
}

void LootMenu::hide() {
    active = false;
    detailView = false;
    syncModal = false;
    diagModal = false;
    if (s_syncGo != SyncGo::Off) {
        dropWifi();
        Avatar::resumeScene();
        s_syncGo = SyncGo::Off;
    }
}

const char* LootMenu::getBottomHint() {
    uint8_t hintCycle = (uint8_t)((millis() / 2500u) % 8u);
    if (syncModal) return "ENT close";
    if (diagModal) {
        if (hintCycle & 1) return ";/.  scroll log";
        return "ENT  close test";
    }
    if (detailView) {
        if (hintCycle == 0) return "U  send this file";
        if (hintCycle == 1) return "Q  pull results";
        if (hintCycle == 2) return "D  delete this file";
        if (hintCycle == 3) return "R  reload list";
        return "ENT  close card";
    }
    if (!count) {
        if (hintCycle == 0) return "Q  pull results";
        if (hintCycle == 1) return "R  reload list";
        if (hintCycle == 2) return "T  test wifi / api";
        if (hintCycle == 3) return ",/  wpasec / pwncrack";
        return "`  back";
    }
    switch (hintCycle) {
        case 0: return "S  send all pending";
        case 1: return "U  send this file";
        case 2: return "Q  pull results";
        case 3: return "D  delete this file";
        case 4: return "R  reload list";
        case 5: return "T  test wifi / api";
        case 6: return "[ / ]  prev / next page";
        default: return ",/  wpasec / pwncrack";
    }
}

static void paintLoot() {
    Display::update();
}

static void addDiag(const char* s) {
    if (s_diagN >= 16) {
        for (uint8_t i = 0; i < 15; i++) memcpy(s_diag[i], s_diag[i + 1], 42);
        s_diagN = 15;
    }
    strncpy(s_diag[s_diagN], s ? s : "", 41);
    s_diag[s_diagN][41] = '\0';
    s_diagN++;
    if (s_diagN > DIAG_VIS)
        s_diagScroll = (uint8_t)(s_diagN - DIAG_VIS);
    paintLoot();
}

static bool httpHeadLine(WiFiClient& c, char* out, size_t n) {
    if (!out || n < 4) return false;
    out[0] = '\0';
    unsigned long t0 = millis();
    while (c.connected() && !c.available() && millis() - t0 < 8000) {
        delay(15);
        yield();
    }
    if (!c.available()) return false;
    size_t got = c.readBytesUntil('\n', out, n - 1);
    out[got] = '\0';
    while (got > 0 && (out[got - 1] == '\r' || out[got - 1] == '\n'))
        out[--got] = '\0';
    return out[0] != '\0';
}

void LootMenu::runDiag() {
    s_diagN = 0;
    s_diagScroll = 0;
    diagModal = true;
    const bool wpa = (tab == Tab::WPASEC);
    addDiag(wpa ? "WPA-SEC LIVE TEST" : "PWNCRACK LIVE TEST");

    if (Cap::isRunning()) Cap::stop();
    Avatar::suspendScene();
    SFX::stop();
    Storage::loadKeysIntoNet();
    Storage::brewHeap();

    char line[42];
    if (wpa) {
        addDiag(WPASec::hasApiKey() ? "KEY ok 32 hex" : "KEY missing key.txt");
    } else {
        addDiag(Pwncrack::hasApiKey() ? "KEY ok" : "KEY missing key.txt");
    }
    snprintf(line, sizeof(line), "SD %s  LOOT %u",
             Storage::available() ? "ok" : "NO", (unsigned)count);
    addDiag(line);
    snprintf(line, sizeof(line), "HEAP %uK  BIG %uK",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getMaxAllocHeap() / 1024));
    addDiag(line);
    if (count && selected < count) {
        snprintf(line, sizeof(line), "SEL %s", s_rows[selected].filename);
        addDiag(line);
    }

    if (!Net::hasStaCreds()) {
        addDiag("WIFI no home in SET");
        addDiag("TEST STOP");
        Avatar::resumeScene();
        return;
    }

    snprintf(line, sizeof(line), "WIFI join %s", Net::cfg().staSsid);
    addDiag(line);
    if (!connectHome()) {
        addDiag("WIFI FAIL timeout");
        dropWifi();
        Avatar::resumeScene();
        return;
    }
    snprintf(line, sizeof(line), "IP %s", WiFi.localIP().toString().c_str());
    addDiag(line);
    snprintf(line, sizeof(line), "RSSI %d  CH %u",
             (int)WiFi.RSSI(), (unsigned)WiFi.channel());
    addDiag(line);

    const char* host = wpa ? "wpa-sec.stanev.org" : "pwncrack.org";
    addDiag(wpa ? "DNS wpa-sec..." : "DNS pwncrack...");
    IPAddress ip;
    if (!Net::resolveHost(host, ip, 3)) {
        addDiag("DNS FAIL");
        dropWifi();
        Avatar::resumeScene();
        return;
    }
    snprintf(line, sizeof(line), "DNS %s", ip.toString().c_str());
    addDiag(line);

    char status[48] = "";
    if (wpa) {
        addDiag("TLS 443...");
        WiFiClientSecure c;
        if (!ioTlsOpen(c, host, 443)) {
            addDiag("TLS FAIL");
            c.stop();
            dropWifi();
            Avatar::resumeScene();
            return;
        }
        addDiag("TLS ok  GET /");
        c.print("GET / HTTP/1.0\r\nHost: ");
        c.print(host);
        c.print("\r\nConnection: close\r\n\r\n");
        if (!httpHeadLine(c, status, sizeof(status))) {
            addDiag("HTTP no reply");
        } else {
            snprintf(line, sizeof(line), "%s", status);
            addDiag(line);
        }
        c.stop();
    } else {
        addDiag("HTTP 80...");
        WiFiClientSecure tls;
        WiFiClient plain;
        bool useTls = false;
        if (!ioPwnOpen(tls, plain, useTls, host)) {
            addDiag("HTTP+TLS FAIL");
            tls.stop();
            plain.stop();
            dropWifi();
            Avatar::resumeScene();
            return;
        }
        WiFiClient& c = useTls ? (WiFiClient&)tls : (WiFiClient&)plain;
        addDiag(useTls ? "TLS 443 ok  GET /" : "HTTP 80 ok  GET /");
        c.print("GET / HTTP/1.0\r\nHost: ");
        c.print(host);
        c.print("\r\nConnection: close\r\n\r\n");
        if (!httpHeadLine(c, status, sizeof(status))) {
            addDiag("HTTP no reply");
        } else {
            snprintf(line, sizeof(line), "%s", status);
            addDiag(line);
        }
        tls.stop();
        plain.stop();
    }

    if (strstr(status, "200") || strstr(status, "301") || strstr(status, "302") ||
        strstr(status, "303") || strstr(status, "307") || strstr(status, "308"))
        addDiag("TEST OK");
    else if (status[0])
        addDiag("TEST REACH but odd HTTP");
    else
        addDiag("TEST FAIL");

    dropWifi();
    Avatar::resumeScene();
}

void LootMenu::startSync(bool oneFile) {
    if (Cap::isRunning()) Cap::stop();
    if (!Storage::available()) {
        Display::showToast("NO SD", 1500);
        return;
    }
    if (oneFile) {
        if (!count || selected >= count) {
            Display::showToast("NO FILE", 1500);
            return;
        }
        s_oneIdx = selected;
    } else {
        s_oneIdx = 0xFF;
    }
    Storage::loadKeysIntoNet();
    if (tab == Tab::WPASEC && !WPASec::hasApiKey()) {
        Display::showToast("NO WPA KEY", 1500);
        return;
    }
    if (tab == Tab::PWNCRACK && !Pwncrack::hasApiKey()) {
        Display::showToast("NO PWN KEY", 1500);
        return;
    }
    if (!Net::hasStaCreds()) {
        Display::showToast("SET HOME WIFI", 1500);
        return;
    }
    syncModal = true;
    strncpy(s_syncHost, tab == Tab::WPASEC ? "WPA-SEC" : "PWNCRACK", sizeof(s_syncHost) - 1);
    s_syncHost[sizeof(s_syncHost) - 1] = '\0';
    strncpy(s_syncText, oneFile ? "ONE FILE..." : "CONNECTING...", sizeof(s_syncText) - 1);
    ioXferClear();
    Avatar::suspendScene();
    SFX::stop();
    WPASec::freeCacheMemory();
    Pwncrack::freeCacheMemory();
    s_syncGo = SyncGo::Wifi;
}

void LootMenu::startPullResults() {
    if (Cap::isRunning()) Cap::stop();
    if (!Storage::available()) {
        Display::showToast("NO SD", 1500);
        return;
    }
    Storage::loadKeysIntoNet();
    if (tab == Tab::WPASEC && !WPASec::hasApiKey()) {
        Display::showToast("NO WPA KEY", 1500);
        return;
    }
    if (tab == Tab::PWNCRACK && !Pwncrack::hasApiKey()) {
        Display::showToast("NO PWN KEY", 1500);
        return;
    }
    if (!Net::hasStaCreds()) {
        Display::showToast("SET HOME WIFI", 1500);
        return;
    }
    s_oneIdx = 0xFE;
    syncModal = true;
    strncpy(s_syncHost, tab == Tab::WPASEC ? "WPA-SEC" : "PWNCRACK", sizeof(s_syncHost) - 1);
    s_syncHost[sizeof(s_syncHost) - 1] = '\0';
    strncpy(s_syncText, "RESULTS...", sizeof(s_syncText) - 1);
    ioXferClear();
    Avatar::suspendScene();
    SFX::stop();
    WPASec::freeCacheMemory();
    Pwncrack::freeCacheMemory();
    s_syncGo = SyncGo::Wifi;
}

void LootMenu::deleteSelected() {
    if (!count || selected >= count) {
        Display::showToast("NO FILE", 800);
        return;
    }
    const char* name = s_rows[selected].filename;
    if (!name[0]) return;
    bool ok = Storage::removeCapture(name);
    Display::showToast(ok ? "DELETED" : "DEL FAIL", 900);
    SFX::play(ok ? SFX::CONFIRM : SFX::ERROR);
    if (!ok) return;
    uint8_t keep = selected;
    bool wasDetail = detailView;
    scan();
    if (count == 0 && page > 0) {
        // Deleted the last row on the last page - step back one page
        // instead of leaving the view stranded on an empty page.
        page--;
        scan();
    }
    if (keep >= count && count) keep = (uint8_t)(count - 1);
    selected = count ? keep : 0;
    if (selected < scroll) scroll = selected;
    if (count && selected >= scroll + VISIBLE)
        scroll = (uint8_t)(selected - VISIBLE + 1);
    detailView = wasDetail && count > 0;
}

void LootMenu::reloadList() {
    uint8_t keep = selected;
    bool wasDetail = detailView;
    WPASec::freeCacheMemory();
    Pwncrack::freeCacheMemory();
    scan();
    if (count == 0 && page > 0) {
        // The card changed since the last scan and this page ran dry -
        // fall back a page instead of showing an empty list.
        page--;
        scan();
    }
    if (keep >= count && count) keep = (uint8_t)(count - 1);
    selected = count ? keep : 0;
    if (selected < scroll) scroll = selected;
    if (count && selected >= scroll + VISIBLE)
        scroll = (uint8_t)(selected - VISIBLE + 1);
    detailView = wasDetail && count > 0;
    char msg[24];
    snprintf(msg, sizeof(msg), "RELOAD %u", (unsigned)count);
    Display::showToast(msg, 800);
    SFX::play(SFX::MENU_CLICK);
}

void LootMenu::handleInput() {
    if (!keyNewPress(keyWasPressed)) return;

    auto keys = M5Cardputer.Keyboard.keysState();
    bool esc = keyEsc();
    if (esc) {
        if (detailView) { detailView = false; return; }
        if (syncModal) { syncModal = false; return; }
        if (diagModal) { diagModal = false; return; }
        hide();
        return;
    }

    if (syncModal || diagModal) {
        if (diagModal) {
            if (M5Cardputer.Keyboard.isKeyPressed(';') && s_diagScroll > 0)
                s_diagScroll--;
            if (M5Cardputer.Keyboard.isKeyPressed('.') &&
                s_diagN > DIAG_VIS &&
                s_diagScroll + DIAG_VIS < s_diagN)
                s_diagScroll++;
        }
        if (M5Cardputer.Keyboard.keysState().enter) {
            syncModal = false;
            diagModal = false;
        }
        return;
    }
    if (detailView) {
        if (M5Cardputer.Keyboard.isKeyPressed('u') ||
            M5Cardputer.Keyboard.isKeyPressed('U')) {
            startSync(true);
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('q') ||
            M5Cardputer.Keyboard.isKeyPressed('Q')) {
            startPullResults();
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('d') ||
            M5Cardputer.Keyboard.isKeyPressed('D')) {
            deleteSelected();
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('r') ||
            M5Cardputer.Keyboard.isKeyPressed('R')) {
            reloadList();
            return;
        }
        if (M5Cardputer.Keyboard.keysState().enter) detailView = false;
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed('/')) {
        tab = (tab == Tab::WPASEC) ? Tab::PWNCRACK : Tab::WPASEC;
        page = 0;
        SFX::play(SFX::MENU_CLICK);
        scan();
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed('[')) {
        if (page > 0) gotoPage((uint8_t)(page - 1), false);
        else Display::showToast("FIRST PAGE", 500);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(']')) {
        if (hasMore) gotoPage((uint8_t)(page + 1), false);
        else Display::showToast("LAST PAGE", 500);
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selected > 0) {
            selected--;
            if (selected < scroll) scroll = selected;
        } else if (page > 0) {
            // Already at the top of this page - step back a page and land
            // on its last row, so paging feels like one continuous list.
            gotoPage((uint8_t)(page - 1), true);
            return;
        }
        SFX::play(SFX::MENU_CLICK);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (count && selected + 1 < count) {
            selected++;
            if (selected >= scroll + VISIBLE) scroll = (uint8_t)(selected - VISIBLE + 1);
        } else if (hasMore) {
            // Already at the bottom of this page and more exist on disk -
            // load the next page instead of just stopping here.
            gotoPage((uint8_t)(page + 1), false);
            return;
        }
        SFX::play(SFX::MENU_CLICK);
    }
    if (M5Cardputer.Keyboard.keysState().enter && count) detailView = true;
    if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S'))
        startSync(false);
    if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U'))
        startSync(true);
    if (M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed('Q'))
        startPullResults();
    if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D'))
        deleteSelected();
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R'))
        reloadList();
    if (M5Cardputer.Keyboard.isKeyPressed('t') || M5Cardputer.Keyboard.isKeyPressed('T')) runDiag();
}

void LootMenu::update() {
    if (!active) return;

    if (s_syncGo == SyncGo::Wifi) {
        Storage::brewHeap();
        if (!connectHome()) {
            strncpy(s_syncText, "WIFI FAIL", sizeof(s_syncText) - 1);
            dropWifi();
            Avatar::resumeScene();
            s_syncGo = SyncGo::Off;
            return;
        }
        strncpy(s_syncText, "TLS...", sizeof(s_syncText) - 1);
        s_syncGo = SyncGo::Work;
        return;
    }

    if (s_syncGo == SyncGo::Work) {
        auto onProg = [](const char* st, uint16_t p, uint16_t t) {
            if (t)
                snprintf(s_syncText, sizeof(s_syncText), "%s %u/%u", st, p, t);
            else
                strncpy(s_syncText, st ? st : "...", sizeof(s_syncText) - 1);
            ioXferPaint(true);
        };
        ioXferClear();
        ioXfer().paint = paintSyncLive;
        Tls::arenaBegin(Display::mainCanvasBuffer(), Display::mainCanvasBufferSize());
        Storage::brewHeap();
        if (s_oneIdx == 0xFE) {
            onProg("Potfile", 1, 1);
            ioXferPhase("POTFILE", 1, 1);
            uint16_t n = 0;
            bool ok = false;
            if (tab == Tab::WPASEC)
                ok = WPASec::pullPotfile(Net::cfg().wpaSecKey, n);
            else
                ok = Pwncrack::pullPotfile(Net::cfg().pwncrackKey, n);
            if (ok) {
                ioXfer().ok = 1;
                snprintf(s_syncText, sizeof(s_syncText), "OK crk%u", (unsigned)n);
            } else {
                ioXfer().fail = 1;
                const char* err = (tab == Tab::WPASEC)
                    ? WPASec::getLastError() : Pwncrack::getLastError();
                snprintf(s_syncText, sizeof(s_syncText), "FAIL %s",
                         err && err[0] ? err : "?");
            }
            ioXferPaint(true);
        } else if (s_oneIdx != 0xFF && s_oneIdx < count) {
            char path[80];
            snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HS, s_rows[s_oneIdx].filename);
            onProg("Upload 1", 1, 1);
            ioXferPhase("UPLOAD", 1, 1);
            bool ok = false;
            if (tab == Tab::WPASEC) {
                const char* id = s_rows[s_oneIdx].hex[0] ? s_rows[s_oneIdx].hex
                                                        : s_rows[s_oneIdx].filename;
                ok = WPASec::uploadOneFile(path, id, Net::cfg().wpaSecKey);
                if (ok) {
                    ioXfer().ok = 1;
                    snprintf(s_syncText, sizeof(s_syncText), "OK 1 crk%u",
                             (unsigned)WPASec::getCrackedCount());
                } else {
                    ioXfer().fail = 1;
                    snprintf(s_syncText, sizeof(s_syncText), "FAIL %s",
                             WPASec::getLastError()[0] ? WPASec::getLastError() : "?");
                }
            } else {
                ok = Pwncrack::uploadOneFile(path, Net::cfg().pwncrackKey);
                if (ok) {
                    ioXfer().ok = 1;
                    snprintf(s_syncText, sizeof(s_syncText), "OK 1 crk%u",
                             (unsigned)Pwncrack::getCrackedCount());
                } else {
                    ioXfer().fail = 1;
                    snprintf(s_syncText, sizeof(s_syncText), "FAIL %s",
                             Pwncrack::getLastError()[0] ? Pwncrack::getLastError() : "?");
                }
            }
            ioXferPaint(true);
        } else if (tab == Tab::WPASEC) {
            WPASecSyncResult r = WPASec::syncCaptures(Net::cfg().wpaSecKey, onProg);
            if (r.success)
                snprintf(s_syncText, sizeof(s_syncText), "OK up%u skip%u crk%u",
                         r.uploaded, r.skipped, r.cracked);
            else
                snprintf(s_syncText, sizeof(s_syncText), "FAIL %s", r.error[0] ? r.error : "?");
        } else {
            PwncrackSyncResult r = Pwncrack::syncCaptures(Net::cfg().pwncrackKey, onProg);
            if (r.success)
                snprintf(s_syncText, sizeof(s_syncText), "OK up%u skip%u crk%u",
                         r.uploaded, r.skipped, r.cracked);
            else
                snprintf(s_syncText, sizeof(s_syncText), "FAIL %s", r.error[0] ? r.error : "?");
        }
        Tls::arenaEnd();
        ioXfer().paint = nullptr;
        dropWifi();
        Avatar::resumeScene();
        scan();
        s_syncGo = SyncGo::Off;
        return;
    }

    if (App::windowHidden()) return;
    handleInput();
}

void LootMenu::draw(M5Canvas& canvas) {
    uiListBackground(canvas);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

    canvas.fillRect(4, 2, 112, 13, tab == Tab::WPASEC ? UiStyle::PINK : UiStyle::PANEL);
    canvas.fillRect(124, 2, 112, 13, tab == Tab::PWNCRACK ? UiStyle::PINK : UiStyle::PANEL);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(tab == Tab::WPASEC ? UiStyle::BG : UiStyle::TEXT);
    canvas.drawString("WPASEC", 60, 5);
    canvas.setTextColor(tab == Tab::PWNCRACK ? UiStyle::BG : UiStyle::TEXT);
    canvas.drawString("PWNCRACK", 180, 5);
    canvas.setTextDatum(top_left);

    if (!Storage::available()) {
        canvas.setTextColor(UiStyle::RED);
        canvas.setCursor(4, 40);
        canvas.print("NO SD CARD");
        return;
    }

    if (syncModal) {
        canvas.setTextWrap(false);
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setCursor(8, 8);
        canvas.print(tab == Tab::WPASEC ? "WPA-SEC" : "PWNCRACK");
        canvas.setTextColor(UiStyle::TEXT);
        canvas.setCursor(8, 22);
        canvas.print(s_syncText);
        const IoXfer& x = ioXfer();
        if (x.files) {
            char line[42];
            snprintf(line, sizeof(line), "FILE %u/%u  ok %u  fail %u",
                     x.file, x.files, x.ok, x.fail);
            canvas.setCursor(8, 38);
            canvas.print(line);
            const int bx = 8, by = 56, bw = DISPLAY_W - 16, bh = 12;
            canvas.drawRect(bx, by, bw, bh, UiStyle::PINK);
            uint32_t den = x.size ? x.size : (uint32_t)x.files;
            uint32_t num = x.size ? x.sent : (uint32_t)x.file;
            if (den == 0) den = 1;
            int inner = bw - 2;
            int fill = (int)((uint64_t)inner * num / den);
            if (fill < 0) fill = 0;
            if (fill > inner) fill = inner;
            if (fill > 0) canvas.fillRect(bx + 1, by + 1, fill, bh - 2, UiStyle::PINK);
            if (x.size) {
                unsigned pct = (unsigned)((uint64_t)x.sent * 100u / x.size);
                snprintf(line, sizeof(line), "%u%%  %uK/%uK", pct,
                         (unsigned)((x.sent + 512) / 1024),
                         (unsigned)((x.size + 512) / 1024));
                canvas.setTextColor(UiStyle::DIM);
                canvas.setCursor(8, 74);
                canvas.print(line);
            }
        }
        return;
    }
    if (diagModal) {
        canvas.setTextWrap(false);
        canvas.setTextColor(UiStyle::CYAN);
        if (s_diagScroll > s_diagN) s_diagScroll = 0;
        uint8_t vis = DIAG_VIS;
        for (uint8_t i = 0; i < vis; i++) {
            uint8_t idx = (uint8_t)(s_diagScroll + i);
            if (idx >= s_diagN) break;
            uiDrawMarquee(canvas, s_diag[idx], 6, 18 + (int)i * 12, DISPLAY_W - 14);
        }
        canvas.setTextColor(UiStyle::DIM);
        if (s_diagScroll > 0) canvas.drawString("^", DISPLAY_W - 10, 18);
        if (s_diagN > vis && s_diagScroll + vis < s_diagN)
            canvas.drawString("v", DISPLAY_W - 10, 18 + (int)(vis - 1) * 12);
        return;
    }
    if (detailView && selected < count) {
        const Row& c = s_rows[selected];
        canvas.setTextColor(UiStyle::PINK);
        canvas.drawString(c.ssid[0] ? c.ssid : "[UNKNOWN]", 6, 18);
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(c.id[0] ? c.id : c.filename, 6, 30);
        canvas.drawString(c.isPMKID ? "PMKID" : "HANDSHAKE", 6, 42);
        if (c.status == St::CRACKED && c.password[0]) {
            canvas.setTextColor(UiStyle::GREEN);
            canvas.drawString("PASS", 6, 56);
            canvas.drawString(c.password, 6, 68);
        } else if (c.status == St::UPLOADED) {
            canvas.setTextColor(UiStyle::GOLD);
            canvas.drawString("uploaded, waiting", 6, 56);
        } else {
            canvas.setTextColor(UiStyle::DIM);
            canvas.drawString("local only", 6, 56);
        }
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(c.filename, 6, 82);
        return;
    }

    if (count == 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setCursor(4, 36);
        canvas.print(tab == Tab::WPASEC ? "NO PCAP IN LOOT" : "NO 22000 IN LOOT");
        canvas.setTextColor(UiStyle::TEXT);
        canvas.setCursor(4, 52);
        canvas.print("/0N3P0rK/handshakes/");
        return;
    }

    uint16_t ok = 0, up = 0, loc = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (s_rows[i].status == St::CRACKED) ok++;
        else if (s_rows[i].status == St::UPLOADED) up++;
        else loc++;
    }
    uint16_t totalPages = (uint16_t)((totalItems + PAGE_SIZE - 1) / PAGE_SIZE);
    if (totalPages == 0) totalPages = 1;
    char summary[48];
    if (totalPages > 1) {
        // Multiple pages: OK/UP/LOC below are for THIS page only (computing
        // them for every capture on disk would mean re-running the wpasec/
        // pwncrack lookups for pages we're not even showing) - P x/y makes
        // that scoping obvious instead of silently under-reporting the total.
        snprintf(summary, sizeof(summary), "%u P%u/%u OK%u UP%u LOC%u",
                 (unsigned)totalItems, (unsigned)(page + 1), (unsigned)totalPages,
                 (unsigned)ok, (unsigned)up, (unsigned)loc);
    } else {
        snprintf(summary, sizeof(summary), "%u  OK %u  UP %u  LOC %u",
                 (unsigned)count, (unsigned)ok, (unsigned)up, (unsigned)loc);
    }
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString(summary, 6, 18);

    canvas.setTextColor(UiStyle::CYAN);
    canvas.drawString("SSID", 6, 28);
    canvas.drawString("ST", 116, 28);
    canvas.drawString("TYPE", 146, 28);
    canvas.drawString("SIZE", 188, 28);

    int y = 38;
    for (uint8_t i = scroll; i < count && i < scroll + VISIBLE; i++) {
        const Row& cap = s_rows[i];
        bool sel = (i == selected);
        uiListRow(canvas, y, 14, sel, UiStyle::PINK);
        canvas.setTextColor(sel ? UiStyle::BG : UiStyle::TEXT);

        char ssidBuf[19];
        size_t pos = 0;
        const char* src = cap.ssid[0] ? cap.ssid : cap.filename;
        while (*src && pos < 17) {
            char ch = *src++;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            ssidBuf[pos++] = ch;
        }
        ssidBuf[pos] = '\0';
        if (*src && pos >= 2) {
            ssidBuf[pos - 2] = '.';
            ssidBuf[pos - 1] = '.';
        }
        canvas.drawString(ssidBuf, 6, y + 3);
        canvas.drawString(cap.status == St::CRACKED ? "[OK]" :
                          (cap.status == St::UPLOADED ? "[..]" : "[--]"), 116, y + 3);
        canvas.drawString(cap.isPMKID ? "PM" : "HS", 146, y + 3);
        char sizeBuf[12];
        formatSize(sizeBuf, sizeof(sizeBuf), cap.fileSize);
        canvas.drawString(sizeBuf, 188, y + 3);
        y += 14;
    }
}
