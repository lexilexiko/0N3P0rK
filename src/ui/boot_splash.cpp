#include "boot_splash.h"
#include "display.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../build_info.h"
#include <M5Cardputer.h>
#include <stdio.h>
#include <string.h>

static bool wantSkip() {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) return true;
    if (M5Cardputer.BtnA.isPressed()) return true;
    return false;
}

static void restoreGfx() {
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.setTextWrap(false);
    M5.Display.setTextColor(UiStyle::TEXT);
}

static void drawTalk(M5Canvas& canvas, int pigX, const char* ph) {
    if (!ph || !ph[0]) return;
    int chars = (int)strlen(ph);
    int bubbleW = chars * 6 + 12;
    if (bubbleW < 44) bubbleW = 44;
    if (bubbleW > 168) bubbleW = 168;
    int bubbleH = 16;
    int bubbleX = pigX + 20;
    int bubbleY = 8;
    if (bubbleX + bubbleW > 236) bubbleX = pigX - bubbleW - 4;
    if (bubbleX < 2) bubbleX = 2;

    const uint16_t fg = 0xEF5D;
    const uint16_t bg = 0x2145;
    canvas.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 4, fg);
    canvas.fillTriangle(bubbleX + 12, bubbleY + bubbleH,
                        bubbleX + 20, bubbleY + bubbleH,
                        bubbleX + 16, bubbleY + bubbleH + 5, fg);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(bg);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(ph, bubbleX + 6, bubbleY + 4);
}

void runBootSplash() {
    SFX::play(SFX::OINK_HAPPY);
    Avatar::setFacingRight();
    Avatar::setGrassMoving(true, false, true);

    M5Canvas& farm = Display::getMain();
    const int pigFrom = -36;
    const int pigTo = 88;
    const int frames = 56;
    bool skipped = false;
    char greeting[40];
    snprintf(greeting, sizeof(greeting), "%s v%s", ON3PORK_NAME, ON3PORK_VERSION);

    for (int f = 0; f <= frames; f++) {
        if (wantSkip()) {
            skipped = true;
            break;
        }
        float t = (float)f / (float)frames;
        float e = t * t * (3.0f - 2.0f * t);
        int px = pigFrom + (int)((float)(pigTo - pigFrom) * e);
        Avatar::setX(px);
        Avatar::setManualWalk(true);
        Avatar::draw(farm);
        if (px > 8) drawTalk(farm, px, greeting);
        Display::blitFrame();
        delay(28);
        yield();
    }

    if (!skipped) {
        Avatar::setX(pigTo);
        Avatar::setManualWalk(true);
        uint32_t hold = millis();
        while ((millis() - hold) < 700) {
            if (wantSkip()) break;
            Avatar::draw(farm);
            drawTalk(farm, pigTo, greeting);
            Display::blitFrame();
            delay(28);
            yield();
        }
    } else {
        Avatar::setX(pigTo);
        Avatar::draw(farm);
        Display::blitFrame();
    }

    Avatar::setManualWalk(false);
    Avatar::setGrassMoving(false, true, true);
    restoreGfx();
}
