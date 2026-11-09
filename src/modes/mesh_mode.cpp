#include "mesh_mode.h"

#include "mesh_pig.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../ui/settings_menu.h"
#include "../audio/sfx.h"

#include <M5Cardputer.h>
#include <stdio.h>
#include <string.h>

namespace MeshMode {

static bool s_keyWas = true;
static char s_draft[48] = {};
static bool s_typing = false;

bool isTyping() {
    return s_typing;
}

void update() {
    if (App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;

    auto keys = M5Cardputer.Keyboard.keysState();
    if (s_typing) {
        if (keyEsc()) {
            s_typing = false;
            s_draft[0] = '\0';
            SFX::play(SFX::BACK_NAV);
        } else if (keys.del || keyMin()) {
            size_t len = strlen(s_draft);
            if (len) s_draft[len - 1] = '\0';
        } else if (keys.enter) {
            if (!s_draft[0]) {
                Display::showToast("EMPTY MESSAGE", 900);
            } else if (MeshPig::sendText(s_draft)) {
                s_typing = false;
                s_draft[0] = '\0';
                SFX::play(SFX::CONFIRM);
                Display::showToast("MESH SENT", 900);
            } else {
                Display::showToast("MESH SEND FAILED", 1100);
            }
        } else {
            size_t len = strlen(s_draft);
            for (char ch : keys.word) {
                if (ch < 32 || ch >= 127 || len + 1 >= sizeof(s_draft)) continue;
                s_draft[len++] = ch;
                s_draft[len] = '\0';
            }
        }
        return;
    }

    if (keyMin()) return;
    if (keyEsc()) {
        App::setMode(AppMode::MENU);
        return;
    }
    if (keys.enter) {
        if (!MeshPig::isRunning()) {
            Display::showToast("ENABLE MESH IN SET", 1100);
            return;
        }
        s_typing = true;
        s_draft[0] = '\0';
        MeshPig::clearNew();
        SFX::play(SFX::MENU_CLICK);
    } else if (M5Cardputer.Keyboard.isKeyPressed('s') ||
               M5Cardputer.Keyboard.isKeyPressed('S')) {
        SettingsMenu::show(SettingsPage::MESH);
        App::setMode(AppMode::MESH_SETUP);
    }
}

void draw(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    canvas.setTextColor(UiStyle::TITLE);
    canvas.drawString("MESH", 8, 0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_right);
    canvas.drawString(MeshPig::isRunning() ? "ONLINE" : "OFFLINE", DISPLAY_W - 8, 5);
    canvas.setTextDatum(top_left);
    canvas.drawLine(8, 18, DISPLAY_W - 8, 18, UiStyle::TITLE);

    const uint16_t y0 = 24;
    const uint16_t midX = DISPLAY_W / 2;
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("NODES", 8, y0);
    canvas.drawFastVLine(midX - 2, y0, MAIN_H - y0 - 4, UiStyle::PANEL);

    const uint8_t nodeCount = MeshPig::nodeCount();
    if (!nodeCount) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(MeshPig::isRunning() ? "searching..." : "mesh is off", 8, y0 + 14);
    }
    for (uint8_t i = 0; i < nodeCount && i < 7; ++i) {
        const MeshNode* node = MeshPig::nodeAt(i);
        if (!node) continue;
        const int y = y0 + 12 + i * 14;
        canvas.setTextColor(node->isSelf ? UiStyle::GOLD : UiStyle::TEXT);
        canvas.drawString(node->shortName[0] ? node->shortName : "????", 8, y);
        canvas.setTextDatum(top_right);
        canvas.setTextColor(node->rssi > -80 ? UiStyle::GREEN :
                            node->rssi > -100 ? UiStyle::GOLD : UiStyle::RED);
        char rssi[12];
        snprintf(rssi, sizeof(rssi), "%d", (int)node->rssi);
        canvas.drawString(rssi, midX - 7, y);
        canvas.setTextDatum(top_left);
    }

    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("MSGS", midX + 4, y0);
    const uint8_t msgCount = MeshPig::msgCount();
    const uint8_t shownMsgs = msgCount < 5 ? msgCount : 5;
    for (uint8_t i = 0; i < shownMsgs; ++i) {
        const MeshMsg* msg = MeshPig::msgAt(i);
        if (!msg) continue;
        const int y = y0 + 12 + i * 22;
        canvas.setTextColor(msg->from == MeshPig::selfNodeNum() ? UiStyle::GOLD : UiStyle::PINK);
        canvas.drawString(msg->fromName, midX + 4, y);
        canvas.setTextColor(UiStyle::TEXT);
        char text[22];
        strncpy(text, msg->text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        canvas.drawString(text, midX + 4, y + 9);
    }
    if (!msgCount) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("no messages", midX + 4, y0 + 14);
    }

    if (s_typing) {
        const int inputY = MAIN_H - 14;
        canvas.fillRect(4, inputY, DISPLAY_W - 8, 12, UiStyle::PANEL);
        canvas.setTextColor(UiStyle::TEXT);
        char input[40];
        const size_t len = strlen(s_draft);
        const char* visible = len > sizeof(input) - 2
            ? s_draft + len - (sizeof(input) - 2) : s_draft;
        snprintf(input, sizeof(input), ">%s", visible);
        canvas.drawString(input, 7, inputY + 2);
    }
}

void getStatusLine(char* out, size_t size) {
    if (!out || !size) return;
    if (s_typing) {
        snprintf(out, size, "TYPE MESSAGE  ENT SEND  ESC CANCEL");
    } else if (MeshPig::isRunning()) {
        strncpy(out, MeshPig::getStatusLine(), size - 1);
        out[size - 1] = '\0';
    } else {
        snprintf(out, size, "ENTER SEND  S SETUP  ESC BACK");
    }
}

}
