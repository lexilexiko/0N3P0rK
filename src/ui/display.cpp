#include "display.h"
#include "menu.h"
#include "keys.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../piglet/avatar.h"
#include "../piglet/cards_table.h"
#include "../piglet/credits.h"
#include "../piglet/mood.h"
#include "../piglet/weather.h"
#include "../piglet/seasonal_fx.h"
#include "../piglet/wolf.h"
#include "../piglet/trees.h"
#include "../piglet/scene_layers.h"
#include "../audio/sfx.h"
#include "../cap/sniffer.h"
#include "../cap/hc22000.h"
#include "../cap/packs/pack_ctx.h"
#include "loot_menu.h"
#include "settings_menu.h"
#include "../modes/evilpig.h"
#include "../modes/pigpass.h"
#include "../modes/blepig.h"
#include "../modes/irport.h"
#include "../modes/spectrum.h"
#include "../modes/usbsd.h"
#include "../modes/filemgr.h"
#include "boot_splash.h"
#include <M5Cardputer.h>
#include <string.h>
#include <stdio.h>

uint16_t getColorFG() {
    if (Weather::getActiveSeason() == Season::RETRO) return 0xE73C;
    return UiStyle::TEXT;
}

uint16_t getColorBG() {
    if (Weather::getActiveSeason() == Season::RETRO) return 0x1082;
    return UiStyle::BG;
}

void uiListBackground(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.fillRect(0, MAIN_H - 5, DISPLAY_W, 5, UiStyle::DIRT);
    canvas.fillRect(0, MAIN_H - 6, DISPLAY_W, 1, 0x45A0);
}

void uiListRow(M5Canvas& canvas, int y, int lineH, bool selected, uint16_t accent) {
    if (selected) {
        canvas.fillRect(4, y - 1, DISPLAY_W - 8, lineH, accent);
        canvas.fillRect(4, y - 1, 3, lineH, UiStyle::TITLE);
    }
}

void uiDrawMarquee(M5Canvas& canvas, const char* s, int x, int y, int maxPx, int charW) {
    if (!s) s = "";
    if (charW < 1) charW = 6;
    if (maxPx < charW) maxPx = charW;
    canvas.setTextWrap(false);
    size_t len = strlen(s);
    int textPx = (int)len * charW;
    if (textPx <= maxPx) {
        canvas.drawString(s, x, y);
        return;
    }
    int extra = textPx - maxPx;
    const uint32_t hold = 900;
    const uint32_t slide = (uint32_t)extra * 70u + 400u;
    uint32_t cycle = hold + slide + hold + 500u;
    uint32_t t = millis() % cycle;
    int off = 0;
    if (t > hold && t < hold + slide)
        off = (int)(((t - hold) * (uint32_t)extra) / slide);
    else if (t >= hold + slide)
        off = extra;
    int startCh = off / charW;
    int pix = off % charW;
    int visCh = (maxPx / charW) + 2;
    char tmp[48];
    size_t n = 0;
    while (n + 1 < sizeof(tmp) && n < (size_t)visCh && s[startCh + n]) {
        tmp[n] = s[startCh + n];
        n++;
    }
    tmp[n] = '\0';
    canvas.drawString(tmp, x - pix, y);
}

M5Canvas Display::topBar(&M5.Display);
M5Canvas Display::mainCanvas(&M5.Display);
M5Canvas Display::bottomBar(&M5.Display);

static constexpr size_t kMainCanvasBytes = (size_t)DISPLAY_W * MAIN_H;
alignas(16) static uint8_t s_mainCanvasBuf[kMainCanvasBytes];

bool Display::screenShakeActive = false;
uint32_t Display::screenShakeStart = 0;
uint16_t Display::screenShakeDuration = 200;
uint8_t Display::screenShakeIntensity = 3;
uint32_t Display::lastActivityTime = 0;
bool Display::dimmed = false;
bool Display::screenForcedOff = false;
char Display::toastMessage[160] = {0};
uint32_t Display::toastStartTime = 0;
uint32_t Display::toastDurationMs = 2000;
bool Display::toastActive = false;
char Display::topBarMessage[96] = {0};
uint32_t Display::topBarMessageStart = 0;
uint32_t Display::topBarMessageDuration = 0;
char Display::bottomHint[96] = {0};

uint8_t* Display::mainCanvasBuffer() { return s_mainCanvasBuf; }
size_t Display::mainCanvasBufferSize() { return kMainCanvasBytes; }

void Display::init() {
    M5.Display.setRotation(1);
    M5.Display.setColorDepth(8);
    M5.Display.fillScreen(COLOR_BG);
    M5.Display.setTextColor(COLOR_FG);

    topBar.setColorDepth(8);
    topBar.createSprite(DISPLAY_W, TOP_BAR_H);

    mainCanvas.setColorDepth(8);
    mainCanvas.setBuffer(s_mainCanvasBuf, DISPLAY_W, MAIN_H, 8);

    bottomBar.setColorDepth(8);
    bottomBar.createSprite(DISPLAY_W, BOTTOM_BAR_H);

    topBar.setTextSize(1);
    mainCanvas.setTextSize(1);
    bottomBar.setTextSize(1);

    lastActivityTime = millis();
    dimmed = false;
    screenForcedOff = false;

    Weather::init();
    SeasonalFx::init();
    Wolf::init();
    Trees::init();
    SceneLayers::init();

    Serial.println("[DISPLAY] ok");
}

void Display::showBootSplash() {
    runBootSplash();
}

// Night: setting-20 (floor 10). Day: setting+20 (cap 100). Never 0.
static uint8_t skyAdjusted(uint8_t base) {
    int v = (int)base;
    if (Avatar::isNightTime()) v -= 20;
    else v += 20;
    if (v < 10) v = 10;
    if (v > 100) v = 100;
    return (uint8_t)v;
}

static void applyLcdBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t raw = (uint8_t)((uint16_t)pct * 255 / 100);
    // Cardputer: PWM can stick off after 0 — nudge then set.
    if (raw > 0) {
        M5.Display.setBrightness(1);
        delay(2);
    }
    M5.Display.setBrightness(raw);
}

static void applyLiveBrightness() {
    applyLcdBrightness(skyAdjusted(Config::personality().brightness));
}

void Display::resetDimTimer() {
    lastActivityTime = millis();
    if (dimmed) {
        dimmed = false;
        applyLiveBrightness();
    }
}

void Display::refreshBrightness() {
    if (screenForcedOff || dimmed) return;
    applyLiveBrightness();
}

void Display::updateDimming() {
    uint16_t timeout = Config::personality().dimTimeout;
    if (timeout == 0 || screenForcedOff) return;
    if (!dimmed && (millis() - lastActivityTime) > (uint32_t)timeout * 1000UL) {
        dimmed = true;
        applyLcdBrightness(Config::personality().dimLevel);
    }
}

void Display::toggleScreenPower() {
    screenForcedOff = !screenForcedOff;
    if (screenForcedOff) {
        Avatar::suspendScene();
        SFX::setScreenOffMuted(true);
        M5.Display.setBrightness(0);
    } else {
        Avatar::resumeScene();
        SFX::setScreenOffMuted(false);
        applyLiveBrightness();
        resetDimTimer();
    }
}

void Display::showToast(const char* message, uint32_t durationMs) {
    if (!message) return;
    strncpy(toastMessage, message, sizeof(toastMessage) - 1);
    toastMessage[sizeof(toastMessage) - 1] = '\0';
    toastStartTime = millis();
    toastDurationMs = durationMs;
    toastActive = true;
}

void Display::notify(NoticeKind kind, const char* message,
                     uint32_t durationMs, NoticeChannel channel) {
    (void)kind;
    uint32_t ms = durationMs ? durationMs : 2000;
    if (channel == NoticeChannel::TOP_BAR) {
        setTopBarMessage(message, ms);
    } else {
        showToast(message, ms);
    }
}

void Display::setTopBarMessage(const char* message, uint32_t durationMs) {
    if (!message) {
        topBarMessage[0] = '\0';
        return;
    }
    strncpy(topBarMessage, message, sizeof(topBarMessage) - 1);
    topBarMessage[sizeof(topBarMessage) - 1] = '\0';
    topBarMessageStart = millis();
    topBarMessageDuration = durationMs;
}

void Display::clearTopBarMessage() { topBarMessage[0] = '\0'; }

void Display::setBottomHint(const char* message) {
    if (!message) {
        bottomHint[0] = '\0';
        return;
    }
    strncpy(bottomHint, message, sizeof(bottomHint) - 1);
    bottomHint[sizeof(bottomHint) - 1] = '\0';
}

void Display::setBottomOverlay(const char* message) { setBottomHint(message); }
void Display::clearBottomOverlay() { setBottomHint(""); }

bool Display::showConfirmBox(const char* title, const char* message) {
    M5Canvas& c = mainCanvas;
    for (;;) {
        M5Cardputer.update();
        c.fillSprite(UiStyle::BG);
        c.fillRoundRect(12, 18, 216, 72, 6, 0x18C3);
        c.drawRoundRect(12, 18, 216, 72, 6, UiStyle::GOLD);
        c.setTextDatum(top_center);
        c.setTextColor(UiStyle::GOLD);
        c.setTextSize(2);
        c.drawString(title ? title : "?", 120, 24);
        c.setTextSize(1);
        c.setTextColor(UiStyle::TEXT);
        c.drawString(message ? message : "", 120, 48);
        c.setTextColor(UiStyle::DIM);
        c.drawString("ENT=YES   ` =NO", 120, 70);
        c.setTextDatum(top_left);
        pushAll();

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto k = M5Cardputer.Keyboard.keysState();
            if (k.enter) return true;
            if (keyEsc()) return false;
        }
        delay(16);
    }
}

void Display::triggerScreenShake(uint8_t intensity, uint16_t durationMs) {
    screenShakeActive = true;
    screenShakeStart = millis();
    screenShakeDuration = durationMs;
    screenShakeIntensity = intensity;
}

bool Display::isShaking() {
    if (!screenShakeActive) return false;
    if ((millis() - screenShakeStart) >= screenShakeDuration) {
        screenShakeActive = false;
        return false;
    }
    return true;
}

float Display::getShakeDecay() {
    if (!isShaking()) return 0.0f;
    float t = (float)(millis() - screenShakeStart) / (float)screenShakeDuration;
    if (t > 1.0f) t = 1.0f;
    return 1.0f - t;
}

uint8_t Display::getShakeIntensity() { return screenShakeIntensity; }

void Display::drawToast() {
    if (!toastActive) return;
    if ((millis() - toastStartTime) > toastDurationMs) {
        toastActive = false;
        return;
    }
    int w = (int)strlen(toastMessage) * 6 + 12;
    if (w > 220) w = 220;
    int x = (DISPLAY_W - w) / 2;
    int y = 4;
    mainCanvas.fillRoundRect(x, y, w, 14, 3, UiStyle::PINK);
    mainCanvas.setTextColor(UiStyle::BG);
    mainCanvas.setTextDatum(top_center);
    mainCanvas.drawString(toastMessage, DISPLAY_W / 2, y + 3);
    mainCanvas.setTextDatum(TL_DATUM);
}

void Display::drawFarm() {
    const uint16_t fg = getColorFG();
    const uint16_t bg = getColorBG();
    const bool sceneLive = !Avatar::isSceneSuspended();
    const bool wolfLive = sceneLive && App::mode() == AppMode::FARM &&
        (SceneLayers::wolf || Config::personality().animTest);

    if (sceneLive) {
        if (SceneLayers::weather) {
            Weather::setMoodLevel(Mood::getEffectiveHappiness());
            Weather::update();
            Avatar::setThunderFlash(Weather::isThunderFlashing());
        } else {
            Avatar::setThunderFlash(false);
        }
        if (SceneLayers::seasonFx) SeasonalFx::update();
        if (wolfLive) Wolf::update();
        else if (Wolf::isActive()) Wolf::reset();
    } else {
        if (Wolf::isActive()) Wolf::reset();
        Avatar::setThunderFlash(false);
    }

    uint16_t bgColor = sceneLive && Weather::isThunderFlashing() ? fg : bg;
    mainCanvas.fillSprite(bgColor);
    mainCanvas.setTextColor(fg);
    mainCanvas.setTextDatum(TL_DATUM);
    mainCanvas.setFont(&fonts::Font0);

    if (Cap::isRunning()) {
        static uint32_t lastWaveMs = 0;
        static uint32_t lastEapol = 0;
        static char toastedHs[33] = {0};
        const Cap::Counters& c = Cap::counters();
        if (c.framesEapol > lastEapol) {
            Avatar::waveRipple(WaveMode::OUTGOING, 5);
            Avatar::sniff();
            lastEapol = c.framesEapol;
        } else if (millis() - lastWaveMs > 450) {
            lastWaveMs = millis();
            if (Cap::runMode() == Cap::RunMode::Aggressive)
                Avatar::waveRipple(WaveMode::OUTGOING, 3);
            else
                Avatar::waveRipple(WaveMode::INCOMING, 2);
        }
        if (c.lastHsSsid[0] && strcmp(toastedHs, c.lastHsSsid) != 0) {
            strncpy(toastedHs, c.lastHsSsid, sizeof(toastedHs) - 1);
            toastedHs[sizeof(toastedHs) - 1] = '\0';
            char msg[28];
            snprintf(msg, sizeof(msg), "HS %s", c.lastHsSsid);
            showToast(msg, 1800);
        }
        if (c.framesWritten == 0) toastedHs[0] = '\0';
    }

    if (sceneLive) {
        Avatar::draw(mainCanvas);
        if (wolfLive) Wolf::draw(mainCanvas);
        if (SceneLayers::weather) {
            Weather::drawBirds(mainCanvas, fg);
            Weather::draw(mainCanvas, fg, bg);
        }
        if (SceneLayers::seasonFx) SeasonalFx::draw(mainCanvas);
        if (SceneLayers::mood) Mood::draw(mainCanvas);
    } else if (CardsTable::isActive()) {
        // Duel UI while farm scene is parked (same idea as PigPass suspend)
        CardsTable::update();
        CardsTable::drawActive(mainCanvas);
    }

    Credits::update();
    if (Credits::isPlaying()) Credits::draw(mainCanvas);
    drawToast();
}

void Display::drawTopBar() {
    const uint16_t fg = getColorFG();
    const uint16_t bg = getColorBG();
    const bool farm = (App::mode() == AppMode::FARM);
    const bool retro = (Weather::getActiveSeason() == Season::RETRO);

    if (topBarMessage[0] != '\0') {
        if (topBarMessageDuration > 0 &&
            (millis() - topBarMessageStart) > topBarMessageDuration) {
            topBarMessage[0] = '\0';
        } else {
            topBar.fillSprite(fg);
            topBar.setTextColor(bg);
            topBar.setTextSize(1);
            topBar.setTextDatum(top_left);
            topBar.drawString(topBarMessage, 2, 3);
            return;
        }
    }

    const uint16_t barBg = farm
        ? (Weather::isThunderFlashing() ? (uint16_t)0xFFFF : Avatar::getSkyColor())
        : bg;
    const uint16_t barFg = farm
        ? (Weather::isThunderFlashing() ? (uint16_t)0x2104
           : (retro ? (uint16_t)0xE73C : (uint16_t)0xEF5D))
        : fg;

    topBar.fillSprite(barBg);
    if (farm) {
        if (SceneLayers::weather) Weather::drawClouds(topBar, barFg, (int16_t)TOP_BAR_H);
        if (SceneLayers::trees) Avatar::drawTreeBarOverflow(topBar);
    }

    topBar.setTextColor(barFg);
    topBar.setTextSize(1);
    topBar.setTextDatum(top_left);

    // Hearts left, clock/season center, [apple][food%] [batt%][snout-batt]
    const int hearts = Mood::getHearts();
    const int food = Mood::getHunger();
    const uint16_t heartOn = 0xF800;
    const uint16_t heartOff = retro ? (uint16_t)0x6B4D : (uint16_t)0x7BEF;
    uint16_t appleOn = 0xE2C0;
    uint16_t stemOn = 0x4A00;
    switch (Weather::getActiveSeason()) {
        case Season::SPRING: appleOn = 0xFDB6; stemOn = 0x07E0; break;
        case Season::SUMMER: appleOn = 0xE2C0; stemOn = 0x4A00; break;
        case Season::AUTUMN: appleOn = 0xFD20; stemOn = 0x8200; break;
        case Season::WINTER: appleOn = 0xC618; stemOn = 0x7BEF; break;
        case Season::RETRO:  appleOn = 0xC618; stemOn = 0x8410; break;
        case Season::NOIR:   appleOn = 0xFE60; stemOn = 0xC480; break;
        case Season::CITY:   appleOn = 0xFD20; stemOn = 0x7BEF; break;
        case Season::DESERT: appleOn = 0xFFE0; stemOn = 0x6B40; break;
    }
    if (retro) { appleOn = 0xC618; stemOn = 0x8410; }
    auto fat = [&](int px, int py, uint16_t c) {
        topBar.fillRect(px, py, 2, 2, c);
    };
    auto drawHeart = [&](int ox, uint16_t c) {
        fat(ox + 0, 2, c); fat(ox + 8, 2, c);
        fat(ox + 0, 4, c); fat(ox + 2, 4, c); fat(ox + 6, 4, c); fat(ox + 8, 4, c);
        fat(ox + 0, 6, c); fat(ox + 2, 6, c); fat(ox + 4, 6, c); fat(ox + 6, 6, c); fat(ox + 8, 6, c);
        fat(ox + 2, 8, c); fat(ox + 4, 8, c); fat(ox + 6, 8, c);
        fat(ox + 4, 10, c);
    };
    auto drawApple = [&](int ox, uint16_t body, uint16_t stem) {
        fat(ox + 5, 1, stem);
        fat(ox + 0, 3, body); fat(ox + 2, 3, body); fat(ox + 4, 3, body); fat(ox + 6, 3, body);
        fat(ox + 0, 5, body); fat(ox + 2, 5, body); fat(ox + 4, 5, body); fat(ox + 6, 5, body);
        fat(ox + 0, 7, body); fat(ox + 2, 7, body); fat(ox + 4, 7, body); fat(ox + 6, 7, body);
        fat(ox + 2, 9, body); fat(ox + 4, 9, body);
    };
    int x = 2;
    for (int i = 0; i < 5; i++) {
        drawHeart(x, (i < hearts) ? heartOn : heartOff);
        x += 12;
    }
    char lv[8];
    snprintf(lv, sizeof(lv), "L%u", (unsigned)XP::getLevel());
    topBar.drawString(lv, x + 2, 4);

    char sky[22];
    Avatar::getSkyHud(sky, sizeof(sky));
    topBar.setTextDatum(top_center);
    topBar.drawString(sky, DISPLAY_W / 2, 4);
    topBar.setTextDatum(top_left);

    static int battPct = 100;
    static uint32_t battMs = 0;
    if (battMs == 0 || (millis() - battMs) > 2000) {
        battMs = millis();
        int32_t lv = M5.Power.getBatteryLevel();
        battPct = (lv < 0) ? 0 : ((lv > 100) ? 100 : (int)lv);
    }
    const bool charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    uint16_t battCol = battPct > 40 ? (retro ? (uint16_t)0xC618 : (uint16_t)0x07E0)
                      : battPct > 15 ? (uint16_t)0xFE60 : (uint16_t)0xF800;
    if (charging) battCol = retro ? (uint16_t)0xC618 : (uint16_t)0xFD78;

    char foodBuf[8];
    char battBuf[8];
    snprintf(foodBuf, sizeof(foodBuf), "%d%%", food);
    snprintf(battBuf, sizeof(battBuf), "%d%%", battPct);
    const int foodW = topBar.textWidth(foodBuf);
    const int battW = topBar.textWidth(battBuf);
    const int appleW = 8;
    const int iconX = DISPLAY_W - 15;
    const int battPctRight = iconX - 2;
    const int foodPctRight = battPctRight - battW - 4;
    const int appleX = foodPctRight - foodW - 2 - appleW;

    drawApple(appleX, appleOn, stemOn);

    topBar.setTextDatum(top_right);
    topBar.drawString(foodBuf, foodPctRight, 4);
    topBar.setTextColor(battCol);
    topBar.drawString(battBuf, battPctRight, 4);
    topBar.setTextColor(barFg);
    topBar.setTextDatum(top_left);

    // Snout-battery: body + pig-nose nub
    topBar.drawRoundRect(iconX, 2, 12, 12, 2, barFg);
    topBar.fillRect(iconX + 12, 6, 3, 4, barFg);
    topBar.drawPixel(iconX + 13, 7, charging ? 0xF800 : barBg);
    topBar.drawPixel(iconX + 13, 9, charging ? 0xF800 : barBg);
    int fill = (battPct * 8 + 50) / 100;
    if (fill < 0) fill = 0;
    if (fill > 8) fill = 8;
    if (fill) topBar.fillRoundRect(iconX + 2, 4, fill, 8, 1, battCol);
}

void Display::drawBottomBar() {
    Season season = Weather::getActiveSeason();
    uint16_t DIRT_MID  = 0x8A40;
    uint16_t fringeTop = 0x45A0;
    uint16_t TEXT_COL  = 0xEF5D;
    switch (season) {
        case Season::SPRING: fringeTop = 0x8F20; break;
        case Season::SUMMER: fringeTop = 0x45A0; break;
        case Season::AUTUMN: fringeTop = 0x8AC0; break;
        case Season::WINTER: fringeTop = 0xDEFB; DIRT_MID = 0x6B6D; break;
        case Season::RETRO:  fringeTop = 0x9CF3; DIRT_MID = 0x4208; TEXT_COL = 0xE73C; break;
        case Season::NOIR:   fringeTop = 0xFE60; DIRT_MID = 0x2104; TEXT_COL = 0xFE60; break;
        case Season::CITY:   fringeTop = 0x8410; DIRT_MID = 0x4208; TEXT_COL = 0xC618; break;
        case Season::DESERT: fringeTop = 0xE5C0; DIRT_MID = 0xD4A0; TEXT_COL = 0xFEA0; break;
    }

    bottomBar.fillSprite(DIRT_MID);
    bottomBar.fillRect(0, 0, DISPLAY_W, 2, fringeTop);
    bottomBar.setTextColor(TEXT_COL);
    bottomBar.setTextSize(1);
    bottomBar.setTextDatum(top_left);

    char left[48];
    left[0] = '\0';
    char rightName[32];
    rightName[0] = '\0';
    bool capLive = Cap::isRunning();

    if (CardsTable::isActive()) {
        CardsTable::getStatusLine(left, sizeof(left));
        snprintf(rightName, sizeof(rightName), "DUEL");
    } else if (App::mode() == AppMode::SPECTRUM && SpectrumMode::isRunning()) {
        SpectrumMode::getStatusLine(left, sizeof(left));
    } else if (Cap::isRunning()) {
        const Cap::Counters& c = Cap::counters();
        // Real focus only (lock / pin / last HS). Hopping beacons no longer
        // overwrite the left label — that looked like random SSIDs.
        // targetMode: 0=SCAN 1=LOCK 2=HS 3=PIN 4=KICK
        const char* net = nullptr;
        // Names only — skip "?" placeholder and never show MAC.
        if (c.targetSsid[0] && strcmp(c.targetSsid, "?") != 0) net = c.targetSsid;
        else if (c.lastHsSsid[0]) net = c.lastHsSsid;
        if (net) {
            size_t n = 0;
            while (net[n] && n < 10) {
                char ch = net[n];
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                left[n++] = ch;
            }
            left[n] = '\0';
        } else {
            snprintf(left, sizeof(left), "SCAN#%02u", (unsigned)c.currentChannel);
        }
        const char* tag = "L";
        if (Cap::runMode() == Cap::RunMode::Aggressive) tag = "A";
        else if (Cap::runMode() == Cap::RunMode::Pinned) tag = "P";
        // Compact pack letter: QUIET/SOFT/NORMAL/FOCUS/LOUD/MAX/CUSTOM/STOCK
        uint8_t packVal = Config::radio().pack;
        char packCh = '-';
        if (packVal == RADIO_PACK_CUSTOM) {
            packCh = 'C';
        } else if (packVal != 0) {
            const char* n = Cap::Packs::name((uint8_t)(packVal - 1));
            if (n && n[0]) {
                if (!strcmp(n, "QUIET"))       packCh = 'Q';
                else if (!strcmp(n, "SOFT"))   packCh = 'S';
                else if (!strcmp(n, "NORMAL")) packCh = 'N';
                else if (!strcmp(n, "FOCUS"))  packCh = 'F';
                else if (!strcmp(n, "LOUD"))   packCh = 'L';
                else if (!strcmp(n, "MAX"))    packCh = 'X';
                else packCh = (char)n[0]; // first letter fallback
            }
        }
        // Compact method letter: ALL/CLIENTS/FOCUS/HERD (+ AUTO)
        const char* mtag = c.methodTag[0] ? c.methodTag : "ALL";
        char methCh = mtag[0] ? mtag[0] : '?';
        if (!strcmp(mtag, "ALL"))          methCh = 'A';
        else if (!strcmp(mtag, "CLIENTS")) methCh = 'C';
        else if (!strcmp(mtag, "FOCUS"))   methCh = 'F';
        else if (!strcmp(mtag, "HERD"))    methCh = 'H';
        else if (!strcmp(mtag, "AUTO"))    methCh = '~';
        // Unique networks with saved HS/PMKID — not raw EAPOL frame count
        // (that grew huge and looked like "kilobyte" garbage on the bar).
        uint16_t hsN = Hc22000::pairCount();
        // e.g. "A* F/F &3 #06" — &N = handshakes, #ch = channel
        snprintf(rightName, sizeof(rightName), "%s%s %c/%c &%u #%02u",
                 tag,
                 Cap::isLocked() ? "*" : "",
                 packCh,
                 methCh,
                 (unsigned)hsN,
                 (unsigned)c.currentChannel);
    } else if (bottomHint[0]) {
        strncpy(left, bottomHint, sizeof(left) - 1);
    } else {
        switch (App::mode()) {
            case AppMode::FARM:
                left[0] = '\0';
                break;
            case AppMode::LOOT:
                strncpy(left, LootMenu::getBottomHint(), sizeof(left) - 1);
                break;
            case AppMode::ATTACK:
                strncpy(left, "LIGHT  AGGRO  STOP", sizeof(left) - 1);
                break;
            case AppMode::WIFI:
            case AppMode::PIG:
            case AppMode::TUNE:
                strncpy(left, SettingsMenu::bottomHint(), sizeof(left) - 1);
                break;
            case AppMode::MENU:
                strncpy(left, Menu::selectedHint(), sizeof(left) - 1);
                break;
            case AppMode::EVILPIG:
                strncpy(left, EvilPigMode::getBottomHint(), sizeof(left) - 1);
                if (EvilPigMode::getPhase() == EvilPigMode::Phase::PORTAL) {
                    const char* ss = EvilPigMode::getApSsid();
                    size_t n = 0;
                    while (ss && ss[n] && n < 10) {
                        char ch = ss[n];
                        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                        rightName[n++] = ch;
                    }
                    rightName[n] = '\0';
                }
                break;
            case AppMode::BLE:
                BlePigMode::getStatusLine(left, sizeof(left));
                break;
            case AppMode::IR:
                IrPortMode::getStatusLine(left, sizeof(left));
                break;
            case AppMode::SPECTRUM:
                SpectrumMode::getStatusLine(left, sizeof(left));
                break;
            case AppMode::USBSD:
                UsbSdMode::getStatusLine(left, sizeof(left));
                break;
            case AppMode::FILEMGR:
                FileMgrMode::getStatusLine(left, sizeof(left));
                break;
            case AppMode::PIGPASS:
                PigpassMode::getStatusLine(left, sizeof(left));
                {
                    const char* ss = PigpassMode::getSSID();
                    size_t n = 0;
                    while (ss && ss[n] && n < 13) {
                        char ch = ss[n];
                        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                        rightName[n++] = ch;
                    }
                    rightName[n] = '\0';
                }
                break;
            default:
                left[0] = '\0';
                break;
        }
    }

    if (App::windowHidden()) {
        char tmp[48];
        if (left[0]) snprintf(tmp, sizeof(tmp), "MIN %s", left);
        else snprintf(tmp, sizeof(tmp), "MIN %s", App::modeName());
        strncpy(left, tmp, sizeof(left) - 1);
        left[sizeof(left) - 1] = '\0';
    }

    if (capLive && App::mode() != AppMode::SPECTRUM &&
        left[0] && strcmp(left, "SCAN") != 0)
        bottomBar.setTextColor(0xFE60);
    bottomBar.setTextWrap(false);
    int rightPx = rightName[0] ? ((int)strlen(rightName) * 6 + 8) : 0;
    int leftMax = DISPLAY_W - 6 - rightPx;
    if (leftMax < 48) leftMax = 48;
    uiDrawMarquee(bottomBar, left, 2, 3, leftMax);
    bottomBar.setTextColor(TEXT_COL);
    if (rightName[0]) {
        bottomBar.setTextDatum(top_right);
        bottomBar.drawString(rightName, DISPLAY_W - 2, 3);
        bottomBar.setTextDatum(top_left);
    }
}

void Display::blitFrame() {
    drawTopBar();
    drawBottomBar();
    pushAll();
}

void Display::pushAll() {
    int ox = 0, oy = 0;
    if (isShaking()) {
        float d = getShakeDecay();
        int mag = (int)(screenShakeIntensity * d);
        ox = (int)random(-mag, mag + 1);
        oy = (int)random(-mag, mag + 1);
    }
    topBar.pushSprite(ox, oy);
    mainCanvas.pushSprite(ox, TOP_BAR_H + oy);
    bottomBar.pushSprite(ox, TOP_BAR_H + MAIN_H + oy);
}

void Display::update() {
    if (screenForcedOff) return;

    SceneLayers::beginFrame();
    updateDimming();

    if (!Avatar::isSceneSuspended()) Mood::update();

    static int8_t lastSkyNight = -1;
    bool night = Avatar::isNightTime();
    if (lastSkyNight != (int8_t)night) {
        lastSkyNight = (int8_t)night;
        refreshBrightness();
    }

    const bool hid = App::windowHidden();
    const bool coverFarm = !hid &&
        (App::mode() == AppMode::SPECTRUM || App::mode() == AppMode::USBSD ||
         App::mode() == AppMode::PIGPASS);
    if (!coverFarm) drawFarm();

    if (App::mode() != AppMode::FARM && !hid) {
        if (App::mode() == AppMode::LOOT) LootMenu::draw(mainCanvas);
        else if (App::mode() == AppMode::EVILPIG) EvilPigMode::draw(mainCanvas);
        else if (App::mode() == AppMode::PIGPASS) PigpassMode::draw(mainCanvas);
        else if (App::mode() == AppMode::BLE) BlePigMode::draw(mainCanvas);
        else if (App::mode() == AppMode::IR) IrPortMode::draw(mainCanvas);
        else if (App::mode() == AppMode::SPECTRUM) SpectrumMode::draw(mainCanvas);
        else if (App::mode() == AppMode::USBSD) UsbSdMode::draw(mainCanvas);
        else if (App::mode() == AppMode::FILEMGR) FileMgrMode::draw(mainCanvas);
        else Menu::draw(mainCanvas);
        drawToast();
    } else if (hid) {
        drawToast();
    }

    drawTopBar();
    drawBottomBar();
    pushAll();
    SceneLayers::endFrame();
}
