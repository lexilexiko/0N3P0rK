#include "menu.h"
#include "display.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../net/ap_sta.h"
#include "../cap/sniffer.h"
#include "loot_menu.h"
#include "settings_menu.h"
#include "keys.h"
#include "../modes/evilpig.h"
#include "../modes/pigpass.h"
#include "../modes/blepig.h"
#include "../modes/irport.h"
#include "../modes/spectrum.h"
#include "../modes/usbsd.h"
#include "../modes/filemgr.h"
#include "../build_info.h"
#include <M5Cardputer.h>
#include <string.h>
#include <stdio.h>

namespace Menu {

enum class GroupId : int8_t { NONE = -1, ATTACK = 0, SET = 1 };

enum class RootType : uint8_t { DIRECT, GROUP };

struct RootItem {
    const char* icon;
    const char* label;
    const char* const* hints;
    uint8_t hintCount;
    RootType type;
    GroupId groupId;
    uint8_t actionId;
};

struct Item {
    const char* icon;
    const char* label;
    uint8_t actionId;
    const char* const* hints;
    uint8_t hintCount;
};

static const char* const H_ATTACK[] = {
    "LIGHT LISTEN OR AGGRO HUNT.",
    "RINGS ON THE SNOUT. LOOT ON SD."
};
static const char* const H_LOOT[] = {
    "WPASEC + PWNCRACK. ONE BAG.",
    ",/ SWITCH TAB. S SYNC."
};
static const char* const H_PIG[] = {
    "HER FACE. HER WORLD.",
    "SKIN SEASON SKY LAYERS LIFE."
};
static const char* const H_SET[] = {
    "SYSTEM STATUS RADIO.",
    "EACH PAGE ITS OWN KNOBS."
};
static const char* const H_SYS[] = {
    "BRIGHT SOUND DIM.",
    "WHEN THE SCREEN SLEEPS."
};
static const char* const H_STAT[] = {
    "BOARD BATT SD WIFI KEYS.",
    "READ ONLY. ` BACK."
};
static const char* const H_RADIO[] = {
    "HOP LOCK DEAUTH RSSI MAC.",
    "AGGRO AND EVILPIG READ THIS."
};
static const char* const H_BLESET[] = {
    "BURST AND ADV TIME.",
    "ATTACK > BLE USES THESE."
};
static const char* const H_CONN[] = {
    "PICK A NET. TYPE ONLY PASS.",
    "HOME WIFI FOR S-SYNC."
};
static const char* const H_USB[] = {
    "SD AS A DISK ON THE PC.",
    "PLUG USB. EJECT THEN `."
};
static const char* const H_FILEMGR[] = {
    "SD + INTERNAL MEM. TXT VIEW/EDIT.",
    "V VOL  ENT OPEN  N NEW  X DEL."
};
static const char* const H_KEYS[] = {
    "BIND FARM / MENU SHORTCUTS.",
    "RADIO R. ENT SET. BS CLEAR."
};
static const char* const H_BLE[] = {
    "APPLE / WIN / ANDROID FRAMES.",
    "OWN DEVICES. ;/. FAMILY."
};
static const char* const H_IR[] = {
    "IR PORT. POINT AT THE TV.",
    "SPC FIRE. R NA/EU. E FILE."
};
static const char* const H_SPEC[] = {
    "2.4 SWEEP. LOBES AND FALL.",
    "ENT LOCK. SPC KICK. W WAKE."
};

static const char* const H_LIGHT[] = {
    "SAME CHANNEL. QUIET SNIFF.",
    "INCOMING RINGS. UI STAYS CALM."
};
static const char* const H_AGGRO[] = {
    "HOP 1-13. KICK. CATCH.",
    "OUTGOING RINGS. SSID HUNT."
};
static const char* const H_STOP[] = {
    "RADIO SLEEP. RINGS DIE.",
    "LOOT STAYS ON /0N3P0rK/."
};
static const char* const H_EVIL[] = {
    "LAB PORTAL. OWN NETS ONLY.",
    "ENT CLONE. V LOOT. D KICK."
};
static const char* const H_PASS[] = {
    "OFFLINE WPA LAB. WORDLIST/MASK.",
    "HS /handshakes/  LISTS /Passworld/"
};
static const char* const H_HASHES[] = {
    "FEED YO HASHCAT.",
    "S SYNC  T TEST  ENT DETAIL."
};
static const char* const H_PWN[] = {
    "PWNCRACK.ORG — NOT WPA-SEC.",
    "KEY IN /0N3P0rK/pwncrack/  HS /handshakes/"
};
static const char* const H_LIFE[] = {
    "SHE WALKS, JUMPS, HIDES.",
    "OFF = YOU STEER. ON = TAMAGOTCHI."
};
static const char* const H_TWEAK[] = {
    "SKIN SEASON SKY SOUND.",
    ",/ CYCLE  ENT NAME."
};

static const RootItem ROOT[] = {
    {"/>", "ATTACK", H_ATTACK, 2, RootType::GROUP,  GroupId::ATTACK, 0},
    {"[$", "LOOT",   H_LOOT,   2, RootType::DIRECT, GroupId::NONE,   4},
    {"^.", "PIG",    H_PIG,    2, RootType::DIRECT, GroupId::NONE,   7},
    {"::", "SET",    H_SET,    2, RootType::GROUP,  GroupId::SET,    0}
};
static const uint8_t ROOT_COUNT = 4;

static const Item G_ATTACK[] = {
    {"/>", "LIGHT",   1,  H_LIGHT, 2},
    {"!!", "AGGRO",   2,  H_AGGRO, 2},
    {"EP", "EVILPIG", 9,  H_EVIL,  2},
    {"PP", "PIGPASS", 10, H_PASS,  2},
    {"BL", "BLE",     13, H_BLE,   2},
    {"IR", "IR PORT", 15, H_IR,    2},
    {"~)", "SPECTRUM",16, H_SPEC,  2},
    {"xx", "STOP",    3,  H_STOP,  2}
};
static const Item G_SET[] = {
    {"[]", "SYSTEM",  14, H_SYS,    2},
    {"::", "STATUS",  19, H_STAT,   2},
    {"))", "RADIO",   11, H_RADIO,  2},
    {"BT", "BLE",     12, H_BLESET, 2},
    {"))", "CONNECT",  6, H_CONN,   2},
    {"**", "KEYS",    18, H_KEYS,   2},
    {"U:", "USB SD",  17, H_USB,    2},
    {"[:", "FILES",   20, H_FILEMGR,2}
};

static uint8_t s_rootIdx = 0;
static uint8_t s_rootScroll = 0;
static GroupId s_group = GroupId::NONE;
static uint8_t s_modalIdx = 0;
static uint8_t s_modalScroll = 0;
static bool s_active = false;
static bool s_keyWas = false;
static uint32_t s_openMs = 0;
static const uint8_t VISIBLE = 4;
static const uint8_t MODAL_VIS = 4;

// leftover screens (PIG tweak / WIFI)
static int s_sel = 0;
static int s_count = 0;
static bool s_editing = false;
static char s_edit[65];
static uint8_t s_editMax = 32;

static const Item* groupItems(GroupId g) {
    if (g == GroupId::ATTACK) return G_ATTACK;
    if (g == GroupId::SET) return G_SET;
    return nullptr;
}

static uint8_t groupSize(GroupId g) {
    if (g == GroupId::ATTACK) return 8;
    if (g == GroupId::SET) return 8;
    return 0;
}

static const char* groupName(GroupId g) {
    if (g == GroupId::ATTACK) return "ATTACK";
    if (g == GroupId::SET) return "SET";
    return "";
}

static void doAction(uint8_t id) {
    switch (id) {
        case 1:
            if (Cap::isRunning() && Cap::runMode() == Cap::RunMode::Light) {
                Cap::stop();
                Display::showToast("LIGHT OFF", 900);
            } else {
                Cap::startLight();
                App::setMode(AppMode::FARM);
                Display::showToast("LIGHT", 900);
            }
            break;
        case 2:
            if (Cap::isRunning() && Cap::runMode() == Cap::RunMode::Aggressive) {
                Cap::stop();
                Display::showToast("AGGRO OFF", 900);
            } else {
                Cap::startAggressive();
                App::setMode(AppMode::FARM);
                Display::showToast("AGGRO", 900);
            }
            break;
        case 3:
            Cap::stop();
            Display::showToast("STOP", 900);
            break;
        case 4:
            LootMenu::show();
            App::setMode(AppMode::LOOT);
            break;
        case 6:
            SettingsMenu::show(SettingsPage::CONNECT);
            App::setMode(AppMode::WIFI);
            break;
        case 7:
            SettingsMenu::show(SettingsPage::SCENE);
            App::setMode(AppMode::PIG);
            break;
        case 14:
            SettingsMenu::show(SettingsPage::SYSTEM);
            App::setMode(AppMode::TUNE);
            break;
        case 19:
            SettingsMenu::show(SettingsPage::STATUS);
            App::setMode(AppMode::TUNE);
            break;
        case 11:
            SettingsMenu::show(SettingsPage::RADIO);
            App::setMode(AppMode::TUNE);
            break;
        case 12:
            SettingsMenu::show(SettingsPage::BLE);
            App::setMode(AppMode::TUNE);
            break;
        case 18:
            SettingsMenu::show(SettingsPage::KEYS);
            App::setMode(AppMode::TUNE);
            break;
        case 13:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::BLE);
            break;
        case 15:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::IR);
            break;
        case 16:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::SPECTRUM);
            break;
        case 17:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::USBSD);
            break;
        case 20:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::FILEMGR);
            break;
        case 9:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::EVILPIG);
            break;
        case 10:
            if (Cap::isRunning()) Cap::stop();
            App::setMode(AppMode::PIGPASS);
            break;
        case 8: {
            PersonalityConfig& p = Config::personality();
            p.freeLife = !p.freeLife;
            Config::save();
            Display::showToast(p.freeLife ? "LIFE ON" : "LIFE OFF", 1200);
            break;
        }
        default:
            break;
    }
}

void begin() {
    s_active = false;
    s_group = GroupId::NONE;
    s_rootIdx = 0;
    s_editing = false;
}

void show() {
    s_active = true;
    s_group = GroupId::NONE;
    s_rootIdx = 0;
    s_rootScroll = 0;
    s_modalIdx = 0;
    s_modalScroll = 0;
    s_keyWas = true;
    s_openMs = millis();
}

void hide() {
    s_active = false;
    s_group = GroupId::NONE;
}

bool isActive() { return s_active; }
bool isInModal() { return s_group != GroupId::NONE; }

bool closeModal() {
    if (s_group == GroupId::NONE) return false;
    s_group = GroupId::NONE;
    SFX::play(SFX::BACK_NAV);
    return true;
}

void onEnter(AppMode mode) {
    s_editing = false;
    s_sel = 0;
    if (mode == AppMode::MENU) show();
    else hide();
    s_count = 0;
}

const char* hint() {
    if (s_editing) return "type  ENT save  ` cancel";
    if (App::mode() == AppMode::MENU) {
        return s_group == GroupId::NONE ? ";/.  ENT open  ` farm" : ";/.  ENT  ` back";
    }
    if (App::mode() == AppMode::PIG || App::mode() == AppMode::TUNE ||
        App::mode() == AppMode::WIFI)
        return SettingsMenu::bottomHint();
    return ";/.  ENT  ` back";
}

const char* selectedHint() {
    if (s_group != GroupId::NONE) {
        const Item* it = groupItems(s_group);
        uint8_t n = groupSize(s_group);
        if (it && s_modalIdx < n && it[s_modalIdx].hintCount)
            return it[s_modalIdx].hints[0];
        return "";
    }
    if (s_rootIdx < ROOT_COUNT && ROOT[s_rootIdx].hintCount)
        return ROOT[s_rootIdx].hints[0];
    return "";
}

static void startEdit(const char* cur, uint8_t maxLen) {
    s_editing = true;
    s_editMax = maxLen;
    strncpy(s_edit, cur ? cur : "", sizeof(s_edit) - 1);
    s_edit[sizeof(s_edit) - 1] = '\0';
}

static void applyWifiField() {
    Net::Cfg c = Net::cfg();
    if (s_sel == 0) Net::setSta(s_edit, c.staPass);
    else if (s_sel == 1) Net::setSta(c.staSsid, s_edit);
    Display::showToast("home wifi saved", 1000);
}

void handleKey(char c, bool enter, bool del, bool fn) {
    (void)c;
    (void)enter;
    (void)del;
    (void)fn;
}

bool tryHotkey() {
    static const uint8_t ACT[HOTKEY_COUNT] = {
        2, 1, 10, 9, 13, 15, 16, 4, 11, 20,
        7, 22, 21, 17, 6, 3
    };
    const HotkeyConfig& hk = Config::hotkeys();
    for (uint8_t i = 0; i < HOTKEY_COUNT; i++) {
        char k = hk.key[i];
        if (!k) continue;
        char up = k;
        if (up >= 'a' && up <= 'z') up = (char)(up - 'a' + 'A');
        if (M5Cardputer.Keyboard.isKeyPressed(k) ||
            (up != k && M5Cardputer.Keyboard.isKeyPressed(up))) {
            doAction(ACT[i]);
            return true;
        }
    }
    return false;
}

void update() {
    if (!s_active || App::mode() != AppMode::MENU) return;
    if (!keyNewPress(s_keyWas)) return;

    auto keys = M5Cardputer.Keyboard.keysState();
    bool esc = keyEsc();
    if (esc) {
        if (s_group != GroupId::NONE) closeModal();
        else App::setMode(AppMode::FARM);
        return;
    }
    if (tryHotkey()) return;

    if (s_group != GroupId::NONE) {
        uint8_t n = groupSize(s_group);
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (s_modalIdx > 0) {
                s_modalIdx--;
                if (s_modalIdx < s_modalScroll) s_modalScroll = s_modalIdx;
                SFX::play(SFX::MENU_CLICK);
            }
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (s_modalIdx + 1 < n) {
                s_modalIdx++;
                if (s_modalIdx >= s_modalScroll + MODAL_VIS)
                    s_modalScroll = (uint8_t)(s_modalIdx - MODAL_VIS + 1);
                SFX::play(SFX::MENU_CLICK);
            }
        }
        if (keys.enter) {
            SFX::play(SFX::MENU_CLICK);
            const Item* it = groupItems(s_group);
            uint8_t id = (it && s_modalIdx < n) ? it[s_modalIdx].actionId : 0;
            closeModal();
            doAction(id);
        }
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (s_rootIdx > 0) {
            s_rootIdx--;
            if (s_rootIdx < s_rootScroll) s_rootScroll = s_rootIdx;
            SFX::play(SFX::MENU_CLICK);
        }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (s_rootIdx + 1 < ROOT_COUNT) {
            s_rootIdx++;
            if (s_rootIdx >= s_rootScroll + VISIBLE)
                s_rootScroll = (uint8_t)(s_rootIdx - VISIBLE + 1);
            SFX::play(SFX::MENU_CLICK);
        }
    }
    if (keys.enter) {
        SFX::play(SFX::MENU_CLICK);
        const RootItem& it = ROOT[s_rootIdx];
        if (it.type == RootType::GROUP) {
            s_group = it.groupId;
            s_modalIdx = 0;
            s_modalScroll = 0;
        } else {
            doAction(it.actionId);
        }
    }
}

static void drawRoot(M5Canvas& canvas) {
    const uint16_t UI_BG = 0x2145, UI_PANEL = 0x3A8A, UI_TITLE = 0xFFE0;
    const uint16_t UI_TEXT = 0xEF5D, UI_DIM = 0x9CD3;
    static const uint16_t CAT[] = {0xF800, 0xFE60, 0xFDB6, 0x07E0};

    canvas.fillSprite(UI_BG);
    canvas.fillRect(0, MAIN_H - 6, DISPLAY_W, 6, 0x6A20);
    canvas.fillRect(0, MAIN_H - 7, DISPLAY_W, 1, 0x45A0);

    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.setTextColor(UI_TITLE);
    canvas.drawString("0N3P0rK", DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, UI_TITLE);

    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    int y0 = 25, lh = 18;
    for (uint8_t i = 0; i < VISIBLE && (s_rootScroll + i) < ROOT_COUNT; i++) {
        uint8_t idx = s_rootScroll + i;
        int y = y0 + i * lh;
        const RootItem& item = ROOT[idx];
        uint16_t cat = CAT[idx % 4];
        bool sel = (idx == s_rootIdx) && (s_group == GroupId::NONE);
        if (sel) {
            canvas.fillRect(5, y - 2, DISPLAY_W - 10, lh, cat);
            canvas.fillRect(5, y - 2, 3, lh, UI_TITLE);
            canvas.setTextColor(UI_BG);
        } else {
            canvas.fillRect(5, y - 1, DISPLAY_W - 10, lh - 2, UI_PANEL);
            canvas.fillRect(5, y - 1, 3, lh - 2, cat);
            canvas.setTextColor(UI_TEXT);
        }
        char buf[40];
        if (item.type == RootType::GROUP)
            snprintf(buf, sizeof(buf), "%s %s >", item.icon, item.label);
        else
            snprintf(buf, sizeof(buf), "%s %s", item.icon, item.label);
        canvas.drawString(buf, 10, y);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(UI_DIM);
    if (s_rootScroll > 0) canvas.drawString("^", DISPLAY_W - 12, 22);
    if (s_rootScroll + VISIBLE < ROOT_COUNT)
        canvas.drawString("v", DISPLAY_W - 12, y0 + (VISIBLE - 1) * lh);
}

static void drawModal(M5Canvas& canvas) {
    const uint16_t BOX_BG = 0x18C3, BOX_EDGE = 0xFE60, BOX_TITLE = 0xFFE0;
    const uint16_t BOX_TEXT = 0xEF5D, BOX_SEL = 0x2D20, BOX_SEL_T = 0xFFE0;
    int boxW = 220, boxH = 90;
    int boxX = (DISPLAY_W - boxW) / 2, boxY = 20;
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 6, BOX_BG);
    canvas.drawRoundRect(boxX, boxY, boxW, boxH, 6, BOX_EDGE);
    canvas.drawRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 5, BOX_EDGE);

    canvas.setTextColor(BOX_TITLE);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.drawString(groupName(s_group), boxX + boxW / 2, boxY + 4);
    canvas.drawLine(boxX + 10, boxY + 20, boxX + boxW - 10, boxY + 20, BOX_EDGE);
    canvas.setTextDatum(top_left);

    const Item* items = groupItems(s_group);
    uint8_t n = groupSize(s_group);
    canvas.setTextSize(2);
    for (int i = 0; i < MODAL_VIS && (s_modalScroll + i) < n; i++) {
        int idx = s_modalScroll + i;
        int y = boxY + 24 + i * 16;
        bool sel = (idx == s_modalIdx);
        if (sel) {
            canvas.fillRect(boxX + 6, y, boxW - 12, 15, BOX_SEL);
            canvas.setTextColor(BOX_SEL_T);
        } else {
            canvas.setTextColor(BOX_TEXT);
        }
        canvas.setCursor(boxX + 10, y);
        canvas.print(sel ? "> " : "  ");
        if (items[idx].icon) {
            canvas.print(items[idx].icon);
            canvas.print(" ");
        }
        if (items[idx].actionId == 8) {
            canvas.print(Config::personality().freeLife ? "LIFE ON" : "LIFE OFF");
        } else {
            canvas.print(items[idx].label);
        }
    }
}

static void line(M5Canvas& canvas, int i, int y, const char* text, bool sel) {
    uiListRow(canvas, y, 10, sel);
    canvas.setTextColor(sel ? UiStyle::BG : UiStyle::TEXT);
    canvas.setTextSize(1);
    canvas.drawString(text, 10, y + 1);
}

void draw(M5Canvas& canvas) {
    if (App::mode() == AppMode::MENU) {
        drawRoot(canvas);
        if (s_group != GroupId::NONE) drawModal(canvas);
        return;
    }
    if (App::mode() == AppMode::PIG || App::mode() == AppMode::TUNE ||
        App::mode() == AppMode::WIFI) {
        SettingsMenu::draw(canvas);
        return;
    }
}

}  // namespace Menu
