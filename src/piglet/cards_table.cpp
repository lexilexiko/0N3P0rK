// «Дуэль» — full card game at the farm table (lv 45+).
#include "cards_table.h"
#include "avatar.h"
#include "mood.h"
#include "../core/xp.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include "../ui/keys.h"
#include "../ui/display.h"
#include "weather.h"
#include <esp_random.h>
#include <M5Cardputer.h>
#include <string.h>

namespace CardsTable {

static constexpr int16_t GROUND_Y = 106;
static constexpr uint8_t MAX_HP   = 10;
static constexpr uint8_t HAND_N   = 5;
static constexpr uint8_t PICK_N   = 2;

static int16_t s_worldX = 200;
static int16_t s_scroll = 0;
static uint32_t s_cool  = 0;
static bool s_ready     = false;
static bool s_active    = false;
static bool s_nearTable = false;
static bool s_escLatch  = false;
static bool s_entLatch  = false;
static bool s_keyLatch[5] = {};

enum class CType : uint8_t { ATK = 0, DEF = 1, HEAL = 2 };

struct Effect {
    CType   type;
    uint8_t pow;
};

struct Card {
    Effect e0;
    Effect e1;
    bool   combo;
    bool   empty;
};

struct PlaySum {
    uint8_t atk, def, heal;
};

enum class Phase : uint8_t {
    SELECT,
    RESOLVE,
    ROUND_OVER,
    MATCH_OVER
};

static Phase   s_phase;
static uint8_t s_youHp, s_aiHp;
static uint8_t s_youWins, s_aiWins;
static uint8_t s_round;
static bool    s_youFirst;
static bool    s_firstWasYou;
static bool    s_youWonLastRound;
static Card    s_hand[HAND_N];
static Card    s_aiHand[HAND_N];
static bool    s_sel[HAND_N];
static uint8_t s_selCount;
static Card    s_youPlay[PICK_N];
static Card    s_aiPlay[PICK_N];
static PlaySum s_youSum, s_aiSum;
static int8_t  s_dmgYou, s_dmgAi;
static int8_t  s_healYou, s_healAi;
static uint32_t s_phaseUntil;
static char    s_msg[28];

bool unlocked() {
    // Full duel game — lv 45+. (cardsEnabled optional; avoid config skew)
    return XP::getLevel() >= 45;
}
bool isActive() { return s_active; }

static int16_t screenX() {
    int16_t x = (int16_t)(s_worldX + s_scroll);
    while (x > 280) x = (int16_t)(x - 300);
    while (x < -40) x = (int16_t)(x + 300);
    return x;
}

static uint8_t rndPow() { return (uint8_t)(1 + (esp_random() % 3)); }
static CType   rndType() { return (CType)(esp_random() % 3); }

static Card makeBasic() {
    Card c{};
    c.e0.type = rndType();
    c.e0.pow  = rndPow();
    c.e1      = {CType::ATK, 0};
    c.combo   = false;
    c.empty   = false;
    return c;
}

static Card makeCombo() {
    Card c{};
    c.e0.type = rndType();
    c.e0.pow  = rndPow();
    do { c.e1.type = rndType(); } while (c.e1.type == c.e0.type);
    c.e1.pow = rndPow();
    c.combo  = true;
    c.empty  = false;
    return c;
}

static Card makeOf(CType t) {
    Card c{};
    c.e0.type = t;
    c.e0.pow  = rndPow();
    c.e1      = {CType::ATK, 0};
    c.combo   = false;
    c.empty   = false;
    return c;
}

// Hand of 5:
//  [0]=ATK  [1]=DEF  [2]=HEAL  (always one of each)
//  [3]=random basic (A/D/H)
//  [4]=20% combo, else random basic
static void dealHand(Card* hand) {
    hand[0] = makeOf(CType::ATK);
    hand[1] = makeOf(CType::DEF);
    hand[2] = makeOf(CType::HEAL);
    hand[3] = makeBasic();
    hand[4] = ((esp_random() % 100) < 20) ? makeCombo() : makeBasic();
    // Shuffle so fixed types are not always in same slots
    for (int i = 4; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        Card tmp = hand[i];
        hand[i] = hand[j];
        hand[j] = tmp;
    }
}

static PlaySum sumPlay(const Card* play, uint8_t n) {
    PlaySum s{0, 0, 0};
    for (uint8_t i = 0; i < n; i++) {
        if (play[i].empty) continue;
        auto add = [&](const Effect& e) {
            if (e.pow == 0) return;
            if (e.type == CType::ATK)  s.atk  = (uint8_t)(s.atk  + e.pow);
            if (e.type == CType::DEF)  s.def  = (uint8_t)(s.def  + e.pow);
            if (e.type == CType::HEAL) s.heal = (uint8_t)(s.heal + e.pow);
        };
        add(play[i].e0);
        if (play[i].combo) add(play[i].e1);
    }
    return s;
}

static void applyAttack(uint8_t atk, uint8_t def,
                        uint8_t& hpTarget, uint8_t& hpAttacker,
                        int8_t& dmgT, int8_t& dmgA) {
    if (atk == 0) { dmgT = 0; dmgA = 0; return; }
    if (def >= atk) {
        dmgA = (int8_t)atk;
        dmgT = 0;
        if (hpAttacker > atk) hpAttacker = (uint8_t)(hpAttacker - atk);
        else hpAttacker = 0;
    } else {
        uint8_t pass = (uint8_t)(atk - def);
        dmgT = (int8_t)pass;
        dmgA = (int8_t)def;
        if (hpTarget > pass) hpTarget = (uint8_t)(hpTarget - pass);
        else hpTarget = 0;
        if (def > 0) {
            if (hpAttacker > def) hpAttacker = (uint8_t)(hpAttacker - def);
            else hpAttacker = 0;
        }
    }
}

static void applyHeal(uint8_t heal, uint8_t& hp, int8_t& shown) {
    if (heal == 0) { shown = 0; return; }
    uint8_t before = hp;
    uint16_t n = (uint16_t)hp + heal;
    if (n > MAX_HP) n = MAX_HP;
    hp = (uint8_t)n;
    shown = (int8_t)(hp - before);
}

static int scoreCard(const Card& c, uint8_t myHp, uint8_t oppHp) {
    if (c.empty) return -999;
    int s = 0;
    auto val = [&](const Effect& e) {
        if (e.pow == 0) return;
        if (e.type == CType::ATK)  s += (int)e.pow * (oppHp <= 4 ? 3 : 2);
        if (e.type == CType::DEF)  s += (int)e.pow * (myHp <= 4 ? 3 : 1);
        if (e.type == CType::HEAL) s += (int)e.pow * (myHp <= 5 ? 4 : 1);
    };
    val(c.e0);
    if (c.combo) { val(c.e1); s += 2; }
    return s;
}

static void aiPick() {
    int best = -999999;
    int bi = 0, bj = 1;
    for (int i = 0; i < HAND_N; i++) {
        for (int j = i + 1; j < HAND_N; j++) {
            int sc = scoreCard(s_aiHand[i], s_aiHp, s_youHp)
                   + scoreCard(s_aiHand[j], s_aiHp, s_youHp);
            if (sc > best) { best = sc; bi = i; bj = j; }
        }
    }
    s_aiPlay[0] = s_aiHand[bi];
    s_aiPlay[1] = s_aiHand[bj];
}

static void beginTurn() {
    dealHand(s_hand);
    dealHand(s_aiHand);
    memset(s_sel, 0, sizeof(s_sel));
    s_selCount = 0;
    s_phase = Phase::SELECT;
    s_msg[0] = 0;
    s_dmgYou = s_dmgAi = s_healYou = s_healAi = 0;
}

static void beginRound(bool youFirst) {
    s_youHp = MAX_HP;
    s_aiHp  = MAX_HP;
    s_youFirst = youFirst;
    beginTurn();
}

static void startMatch() {
    s_youWins = s_aiWins = 0;
    s_round = 1;
    s_youWonLastRound = false;
    s_firstWasYou = (esp_random() & 1) != 0;
    beginRound(s_firstWasYou);
    snprintf(s_msg, sizeof(s_msg), "R%d GO", s_round);
}

static void resolveTurn() {
    uint8_t p = 0;
    for (uint8_t i = 0; i < HAND_N && p < PICK_N; i++) {
        if (s_sel[i]) s_youPlay[p++] = s_hand[i];
    }
    while (p < PICK_N) { s_youPlay[p].empty = true; p++; }

    aiPick();
    s_youSum = sumPlay(s_youPlay, PICK_N);
    s_aiSum  = sumPlay(s_aiPlay, PICK_N);
    s_dmgYou = s_dmgAi = s_healYou = s_healAi = 0;

    auto actFirst = [&](bool youAreFirst) {
        if (youAreFirst) {
            int8_t dT = 0, dA = 0;
            applyAttack(s_youSum.atk, s_aiSum.def, s_aiHp, s_youHp, dT, dA);
            s_dmgAi  = (int8_t)(s_dmgAi + dT);
            s_dmgYou = (int8_t)(s_dmgYou + dA);
            int8_t h = 0;
            applyHeal(s_youSum.heal, s_youHp, h);
            s_healYou = (int8_t)(s_healYou + h);
        } else {
            int8_t dT = 0, dA = 0;
            applyAttack(s_aiSum.atk, s_youSum.def, s_youHp, s_aiHp, dT, dA);
            s_dmgYou = (int8_t)(s_dmgYou + dT);
            s_dmgAi  = (int8_t)(s_dmgAi + dA);
            int8_t h = 0;
            applyHeal(s_aiSum.heal, s_aiHp, h);
            s_healAi = (int8_t)(s_healAi + h);
        }
    };

    auto actSecond = [&](bool youAreFirst) {
        // Second's DEF already spent vs first attack
        if (youAreFirst) {
            int8_t dT = 0, dA = 0;
            applyAttack(s_aiSum.atk, s_youSum.def, s_youHp, s_aiHp, dT, dA);
            s_dmgYou = (int8_t)(s_dmgYou + dT);
            s_dmgAi  = (int8_t)(s_dmgAi + dA);
            int8_t h = 0;
            applyHeal(s_aiSum.heal, s_aiHp, h);
            s_healAi = (int8_t)(s_healAi + h);
        } else {
            int8_t dT = 0, dA = 0;
            applyAttack(s_youSum.atk, s_aiSum.def, s_aiHp, s_youHp, dT, dA);
            s_dmgAi  = (int8_t)(s_dmgAi + dT);
            s_dmgYou = (int8_t)(s_dmgYou + dA);
            int8_t h = 0;
            applyHeal(s_youSum.heal, s_youHp, h);
            s_healYou = (int8_t)(s_healYou + h);
        }
    };

    actFirst(s_youFirst);
    // Defender's DEF was spent on the first exchange — second hit faces 0 DEF
    if (s_youHp > 0 && s_aiHp > 0) {
        if (s_youFirst) s_aiSum.def = 0;
        else             s_youSum.def = 0;
        actSecond(s_youFirst);
    }

    s_youFirst = !s_youFirst;

    if (s_youHp == 0 || s_aiHp == 0) {
        s_phase = Phase::ROUND_OVER;
        s_phaseUntil = millis() + 2200;
        if (s_youHp == 0 && s_aiHp == 0) {
            s_youWonLastRound = false;
            snprintf(s_msg, sizeof(s_msg), "DRAW R%d", s_round);
        } else if (s_aiHp == 0) {
            s_youWins++;
            s_youWonLastRound = true;
            snprintf(s_msg, sizeof(s_msg), "YOU WIN R%d", s_round);
            SFX::play(SFX::MENU_CLICK);
        } else {
            s_aiWins++;
            s_youWonLastRound = false;
            snprintf(s_msg, sizeof(s_msg), "AI WIN R%d", s_round);
        }
        if (s_youWins >= 2 || s_aiWins >= 2) {
            s_phase = Phase::MATCH_OVER;
            s_phaseUntil = millis() + 3500;
            if (s_youWins >= 2) {
                snprintf(s_msg, sizeof(s_msg), "YOU WIN MATCH");
                Mood::say("WIN!");
                Avatar::setState(AvatarState::HAPPY);
            } else {
                snprintf(s_msg, sizeof(s_msg), "AI WINS MATCH");
                Mood::say("LOST...");
                Avatar::setState(AvatarState::SAD);
            }
        }
    } else {
        s_phase = Phase::RESOLVE;
        s_phaseUntil = millis() + 1400;
        snprintf(s_msg, sizeof(s_msg), "A%d/D%d/H%d", s_youSum.atk, s_youSum.def, s_youSum.heal);
    }
}

void end(); // fwd

static void advanceAfterPause() {
    if (s_phase == Phase::MATCH_OVER) {
        end();
        return;
    }
    if (s_phase == Phase::ROUND_OVER) {
        if (s_youWins >= 2 || s_aiWins >= 2) {
            s_phase = Phase::MATCH_OVER;
            s_phaseUntil = millis() + 2500;
            if (s_youWins > s_aiWins)
                snprintf(s_msg, sizeof(s_msg), "YOU WIN MATCH");
            else if (s_aiWins > s_youWins)
                snprintf(s_msg, sizeof(s_msg), "AI WINS MATCH");
            else
                snprintf(s_msg, sizeof(s_msg), "DRAW MATCH");
            return;
        }
        s_round++;
        if (s_round > 3) {
            s_phase = Phase::MATCH_OVER;
            s_phaseUntil = millis() + 2000;
            if (s_youWins > s_aiWins) {
                snprintf(s_msg, sizeof(s_msg), "YOU WIN MATCH");
            } else if (s_aiWins > s_youWins) {
                snprintf(s_msg, sizeof(s_msg), "AI WINS MATCH");
            } else {
                snprintf(s_msg, sizeof(s_msg), "DRAW MATCH");
            }
            return;
        }
        bool youFirst;
        if (s_round == 2) youFirst = !s_firstWasYou;
        else youFirst = !s_youWonLastRound; // loser of R2 starts R3
        beginRound(youFirst);
        snprintf(s_msg, sizeof(s_msg), "R%d GO", s_round);
        return;
    }
    if (s_phase == Phase::RESOLVE) beginTurn();
}

void begin() {
    s_worldX = 200;
    s_scroll = 0;
    s_cool = 0;
    s_ready = true;
    s_active = false;
    s_escLatch = s_entLatch = false;
    memset(s_keyLatch, 0, sizeof(s_keyLatch));
}

void end() {
    if (!s_active) return;
    s_active = false;
    s_escLatch = s_entLatch = false;
    Avatar::resumeScene();
    Display::showToast("CARDS OUT", 900);
    SFX::play(SFX::MENU_CLICK);
}

void scroll(int8_t dx) {
    if (!unlocked() || s_active) return;
    s_scroll = (int16_t)(s_scroll + dx);
}

static void startDuel() {
    if (s_active) return;
    s_active = true;
    s_escLatch = s_entLatch = false;
    memset(s_keyLatch, 0, sizeof(s_keyLatch));
    Avatar::suspendScene();
    startMatch();
    Display::showToast("DUEL", 1000);
    SFX::play(SFX::MENU_CLICK);
    Avatar::setState(AvatarState::HAPPY);
}

static bool keyEnter() {
    // Cardputer Enter / Return / Space (confirm)
    if (M5Cardputer.Keyboard.isKeyPressed('\n')) return true;
    if (M5Cardputer.Keyboard.isKeyPressed('\r')) return true;
    if (M5Cardputer.Keyboard.isKeyPressed(' ')) return true;
    if (M5Cardputer.Keyboard.isKeyPressed(0x28)) return true; // HID Enter
    return false;
}

void update() {
    if (!unlocked()) {
        if (s_active) end();
        return;
    }
    if (!s_ready) begin();

    if (s_active) {
        if (keyNewPress(s_escLatch) && keyEsc()) {
            end();
            return;
        }

        uint32_t now = millis();

        if (s_phase == Phase::RESOLVE || s_phase == Phase::ROUND_OVER ||
            s_phase == Phase::MATCH_OVER) {
            // Timer OR Enter — never both in one frame (would skip a phase)
            if (now >= s_phaseUntil) {
                advanceAfterPause();
            } else if (keyNewPress(s_entLatch) && keyEnter()) {
                s_phaseUntil = 0;
                advanceAfterPause();
            }
            return;
        }

        if (s_phase == Phase::SELECT) {
            for (int k = 0; k < 5; k++) {
                char ch = (char)('1' + k);
                if (!keyNewPress(s_keyLatch[k])) continue;
                if (!M5Cardputer.Keyboard.isKeyPressed(ch)) continue;
                if (s_sel[k]) {
                    s_sel[k] = false;
                    if (s_selCount) s_selCount--;
                    SFX::play(SFX::MENU_CLICK);
                } else if (s_selCount < PICK_N) {
                    s_sel[k] = true;
                    s_selCount++;
                    SFX::play(SFX::MENU_CLICK);
                }
            }
            if (s_selCount == PICK_N && keyNewPress(s_entLatch) && keyEnter()) {
                resolveTurn();
            }
        }
        return;
    }

    // Jump never starts the duel (tree smash conflict).
    // Stand near table center + press G.
    if (Avatar::isJumping()) return;

    int16_t sx = screenX();
    int pig = Avatar::getCurrentX() + 20;
    bool atTable = abs(pig - (int)sx) < 28;
    s_nearTable = atTable;

    static bool s_gWas = false;
    static uint32_t s_hintMs = 0;
    bool gNow = M5Cardputer.Keyboard.isKeyPressed('g') ||
                M5Cardputer.Keyboard.isKeyPressed('G');
    bool gEdge = gNow && !s_gWas;
    s_gWas = gNow;

    if (!atTable) return;

    uint32_t now = millis();
    if (now - s_hintMs > 2800) {
        s_hintMs = now;
        Mood::say("G = PLAY");
    }
    if (gEdge && now > s_cool) {
        s_cool = now + 800;
        startDuel();
    }
}

static void drawSeasonDecor(M5Canvas& canvas, int16_t cx, int16_t cy) {
    // Seasonal layer only. The table geometry, barrel V5 and cards stay fixed.
    const Season season = Weather::getActiveSeason();

    // Small helpers keep the decoration deliberately pixel-art and cheap.
    auto px = [&](int16_t x, int16_t y, uint16_t c) {
        canvas.drawPixel(x, y, c);
    };
    auto block = [&](int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
        canvas.fillRect(x, y, w, h, c);
    };

    switch (season) {
        case Season::SPRING: {
            // Tiny flowers/grass around the table + a couple of petals on top.
            block(cx - 31, cy - 8, 1, 5, 0x35A8);
            px(cx - 32, cy - 9, 0xF6A0);
            px(cx - 30, cy - 9, 0xF6A0);
            px(cx - 31, cy - 10, 0xFFE0);
            block(cx + 29, cy - 7, 1, 5, 0x35A8);
            px(cx + 28, cy - 8, 0xF6A0);
            px(cx + 30, cy - 8, 0xF6A0);
            px(cx + 29, cy - 9, 0xFFE0);
            px(cx - 14, cy - 21, 0xF6A0);
            px(cx + 15, cy - 18, 0xFFE0);
            // A tiny green sprout on the tabletop.
            px(cx + 10, cy - 25, 0x35A8);
            px(cx + 11, cy - 26, 0x35A8);
            break;
        }

        case Season::SUMMER: {
            // Dry grass and warm little accents around the fixed table.
            block(cx - 32, cy - 7, 1, 5, 0x4C83);
            px(cx - 33, cy - 9, 0x7D94);
            block(cx + 31, cy - 6, 1, 5, 0x4C83);
            px(cx + 32, cy - 8, 0x7D94);
            px(cx - 18, cy - 17, 0xE6A0);
            px(cx + 18, cy - 20, 0xE6A0);
            break;
        }

        case Season::AUTUMN: {
            // Fallen leaves around the table and two leaves resting on the top.
            const uint16_t leafA = 0xD4A0;
            const uint16_t leafB = 0xA940;
            px(cx - 31, cy - 6, leafA);
            px(cx + 32, cy - 9, leafB);
            px(cx - 27, cy - 3, leafB);
            px(cx + 27, cy - 2, leafA);
            px(cx - 13, cy - 22, leafA);
            px(cx + 13, cy - 20, leafB);
            // Small leaf vein.
            px(cx - 12, cy - 21, 0x8200);
            px(cx + 12, cy - 19, 0x8200);
            break;
        }

        case Season::WINTER: {
            // Snow sits on the tabletop, cards and barrel cap without changing geometry.
            const uint16_t snow = 0xFFFF;
            const uint16_t snowShade = 0xD6BA;
            block(cx - 20, cy - 24, 42, 2, snow);
            px(cx - 18, cy - 22, snowShade);
            px(cx + 17, cy - 22, snowShade);
            // Snow caps on both cards.
            block(cx - 8, cy - 34, 14, 2, snow);
            block(cx - 4, cy - 36, 14, 2, snow);
            // Snow on barrel crown.
            block(cx - 6, cy - 14, 12, 1, snow);
            // A few flakes nearby.
            px(cx - 30, cy - 7, snow);
            px(cx + 31, cy - 4, snow);
            px(cx + 25, cy - 11, snowShade);
            break;
        }

        case Season::RETRO: {
            // Retro palette: tiny neon/pixel accents, while the wooden table remains unchanged.
            px(cx - 31, cy - 8, 0xF81F);
            px(cx + 31, cy - 8, 0x07FF);
            px(cx - 17, cy - 21, 0xF81F);
            px(cx + 18, cy - 19, 0x07FF);
            break;
        }

        case Season::NOIR: {
            // Noir: restrained monochrome highlights.
            px(cx - 31, cy - 7, 0xC618);
            px(cx + 31, cy - 7, 0x8410);
            px(cx - 15, cy - 21, 0xC618);
            px(cx + 15, cy - 19, 0x8410);
            break;
        }

        case Season::CITY: {
            // Urban alley: tiny litter/concrete marks around the table and one tabletop detail.
            block(cx - 32, cy - 5, 3, 2, 0x8410);
            block(cx + 29, cy - 9, 3, 2, 0x4208);
            px(cx - 14, cy - 21, 0x7BEF);
            px(cx + 14, cy - 19, 0xC618);
            break;
        }

        case Season::DESERT: {
            // Retro desert: sand, pebble, dry grass, cactus hint + sand on the tabletop/cards.
            const uint16_t sand = 0xD3A0;
            const uint16_t sandDark = 0x9B4D;
            const uint16_t cactus = 0x4A84;
            px(cx - 32, cy - 7, sand);
            px(cx - 29, cy - 3, sandDark);
            px(cx + 31, cy - 8, sand);
            px(cx + 28, cy - 3, sandDark);
            // Tiny cactus at left of the barrel.
            block(cx - 31, cy - 12, 2, 7, cactus);
            block(cx - 33, cy - 9, 2, 2, cactus);
            block(cx - 29, cy - 11, 2, 2, cactus);
            // Sand grains on the tabletop and cards.
            px(cx - 15, cy - 21, sand);
            px(cx + 17, cy - 20, sandDark);
            px(cx - 5, cy - 31, sand);
            px(cx + 4, cy - 33, sand);
            break;
        }
    }
}

static void drawTableAt(M5Canvas& canvas, int16_t cx, int16_t cy) {
    // EXACT tabletop layout from the preview: green cloth, pig emblem,
    // centered barrel support, and the small card that rotates in place.
    const int16_t top = cy - 24;
    const int16_t bottom = top + 1;

    // Tabletop — same proportions as the preview, not the old wide tabletop.
    canvas.fillRect(cx - 17, top - 10, 34, 2, 0x4120);
    canvas.fillRect(cx - 20, top - 8, 40, 7, 0x4120);
    canvas.fillRect(cx - 17, top - 1, 34, 2, 0x4120);

    canvas.fillRect(cx - 17, top - 9, 34, 2, 0x8B46);
    canvas.fillRect(cx - 19, top - 7, 38, 5, 0x8B46);
    canvas.fillRect(cx - 17, top - 2, 34, 2, 0x8B46);
    canvas.fillRect(cx - 16, top - 9, 32, 1, 0xD18A);

    // Green cloth.
    canvas.fillRect(cx - 15, top - 7, 30, 5, 0x315A);
    canvas.fillRect(cx - 13, top - 8, 26, 1, 0x315A);
    canvas.fillRect(cx - 13, top - 2, 26, 1, 0x284A);

    // Tiny pig emblem in the middle of the cloth.
    const uint16_t pig = 0x7C36;
    const int8_t pigPx[][2] = {
        {-2,-5},{-1,-6},{0,-5},{-4,-4},{2,-4},{-1,-3}
    };
    for (size_t i = 0; i < sizeof(pigPx) / sizeof(pigPx[0]); ++i)
        canvas.drawPixel(cx + pigPx[i][0], top + pigPx[i][1], pig);

    // Little chips/cards on the tabletop.
    canvas.fillRect(cx - 10, top - 1, 3, 2, 0xD6C9);
    canvas.drawPixel(cx - 9, top - 2, 0x4D78);
    canvas.fillRect(cx + 8, top - 1, 3, 2, 0xD6C9);
    canvas.drawPixel(cx + 9, top - 2, 0xC447);

    // Rotating card — exactly the preview behavior: it turns edge-on and back.
    const float phase = (float)(millis() % 1800UL) / 1800.0f;
    const float cs = fabsf(cosf(phase * 6.2831853f));
    const int16_t cw = (int16_t)max(1, (int)lroundf(8.0f * cs));
    const int16_t cardX = cx + 7;
    const int16_t cardY = top - 13;
    canvas.fillRect(cardX - 4, cardY + 7, 9, 1, 0x3B28);
    if (cs < 0.10f) {
        canvas.fillRect(cardX, cardY, 1, 7, 0xEF5D);
    } else {
        const int16_t left = cardX - cw / 2;
        canvas.fillRect(left, cardY, cw, 7, 0xEF5D);
        canvas.fillRect(left + 1, cardY + 1, max(1, (int)cw - 2), 5, 0xA63B);
        if (cw >= 5) {
            canvas.drawPixel(cardX - 1, cardY + 3, 0xE6C6);
            canvas.drawPixel(cardX,     cardY + 3, 0xE6C6);
        }
    }

    // Barrel is anchored immediately under the tabletop, centered exactly.
    const int16_t by = bottom;
    canvas.fillRect(cx - 9, by + 11, 18, 2, 0x6241);
    canvas.fillRect(cx - 6, by,     12, 1, 0x4120);
    canvas.fillRect(cx - 8, by + 1, 16, 1, 0x4120);
    canvas.fillRect(cx - 9, by + 2, 18, 7, 0x4120);
    canvas.fillRect(cx - 8, by + 9, 16, 1, 0x4120);
    canvas.fillRect(cx - 6, by + 10, 12, 1, 0x4120);

    canvas.fillRect(cx - 5, by + 1, 10, 1, 0x8B46);
    canvas.fillRect(cx - 7, by + 2, 14, 7, 0x8B46);
    canvas.fillRect(cx - 6, by + 9, 12, 1, 0x8B46);
    canvas.drawPixel(cx - 8, by + 2, 0x9650);
    canvas.drawPixel(cx + 7, by + 2, 0x9650);
    canvas.drawPixel(cx - 9, by + 3, 0x8B46);
    canvas.drawPixel(cx + 8, by + 3, 0x8B46);
    canvas.drawPixel(cx - 9, by + 7, 0x8B46);
    canvas.drawPixel(cx + 8, by + 7, 0x8B46);
    canvas.drawPixel(cx - 8, by + 8, 0x8B46);
    canvas.drawPixel(cx + 7, by + 8, 0x8B46);
    canvas.fillRect(cx - 5, by + 2, 2, 7, 0xA960);
    canvas.fillRect(cx + 4, by + 2, 2, 7, 0x6C35);
    canvas.drawPixel(cx - 2, by + 5, 0xB96B);
    canvas.drawPixel(cx + 2, by + 7, 0x6D35);
    canvas.fillRect(cx - 8, by + 3, 16, 1, 0xD18A);
    canvas.fillRect(cx - 9, by + 7, 18, 1, 0xD18A);

    // Seasonal effects are the final overlay only.
    drawSeasonDecor(canvas, cx, cy);
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    if (!Config::personality().cardsEnabled) return;
    if (!unlocked() || s_active) return;
    int16_t cx = screenX();
    int16_t cy = (int16_t)(GROUND_Y + yOffset);
    drawTableAt(canvas, cx, cy);
}


// Classic RPG pixel icons 10x10 (readable at scale 2–3)
// A=sword vertical  D=heater shield  H=potion flask

static const char* const ICON_SWORD[] = {
    "....##....",
    "...####...",
    "...#++#...",
    "...#++#...",
    "...#++#...",
    ".########.",
    "....##....",
    "....##....",
    "...####...",
    "....##....",
    nullptr
};
static const char* const ICON_SHIELD[] = {
    ".########.",
    "##++++++##",
    "##+####+##",
    "##+#++#+##",
    "##+#++#+##",
    "##+####+##",
    ".##++++##.",
    "..##++##..",
    "...####...",
    "....##....",
    nullptr
};
static const char* const ICON_POTION[] = {
    "...####...",
    "....##....",
    "...####...",
    "..#++++#..",
    ".#++++++#.",
    "#+++##+++#",
    "#++++++++#",
    "#++++++++#",
    ".#++++++#.",
    "..######..",
    nullptr
};

static uint16_t colMain(CType t) {
    if (t == CType::ATK) return 0x9CF3;   // sword steel
    if (t == CType::DEF) return 0x3A9F;   // shield blue
    return 0xC180;                        // potion glass rim (dark red-brown)
}
static uint16_t colLite(CType t) {
    if (t == CType::ATK) return 0xFFFF;   // blade shine
    if (t == CType::DEF) return 0x8E7F;   // shield face
    return 0xF800;                        // red heal liquid
}
static uint16_t colOut() { return 0x4208; }

static void blitIcon(M5Canvas& canvas, int16_t ox, int16_t oy,
                     const char* const* rows, CType t, int scale) {
    uint16_t cm = colMain(t);
    uint16_t cl = colLite(t);
    uint16_t co = colOut();
    for (int r = 0; rows[r]; r++) {
        const char* line = rows[r];
        for (int c = 0; line[c]; c++) {
            char ch = line[c];
            if (ch == '.') continue;
            uint16_t col = (ch == '#') ? cm : (ch == '+') ? cl : co;
            if (scale <= 1) {
                canvas.drawPixel(ox + c, oy + r, col);
            } else {
                canvas.fillRect(ox + c * scale, oy + r * scale, scale, scale, col);
            }
        }
    }
}

static const char* const* iconFor(CType t) {
    if (t == CType::ATK) return ICON_SWORD;
    if (t == CType::DEF) return ICON_SHIELD;
    return ICON_POTION;
}

static char typeLetter(CType t) {
    if (t == CType::ATK) return 'A';
    if (t == CType::DEF) return 'D';
    return 'H';
}

// Card face: 36x44 — icon + letter so type is obvious
static void drawCardFace(M5Canvas& canvas, int16_t x, int16_t y,
                         const Card& c, bool selected) {
    const int16_t W = 36, H = 44;
    uint16_t bg = selected ? 0xFFE0 : 0xFFFF;
    uint16_t bd = selected ? 0xFD20 : 0x4A49;
    canvas.fillRect(x, y, W, H, bg);
    canvas.drawRect(x, y, W, H, bd);
    canvas.drawRect(x + 1, y + 1, W - 2, H - 2, selected ? 0xC480 : 0xC618);
    if (c.empty) return;

    canvas.setTextSize(1);

    if (c.combo) {
        // top: letter+pow + icon scale1
        canvas.setTextColor(colMain(c.e0.type), bg);
        canvas.setCursor(x + 2, y + 2);
        canvas.printf("%c%d", typeLetter(c.e0.type), (unsigned)c.e0.pow);
        blitIcon(canvas, x + 16, y + 1, iconFor(c.e0.type), c.e0.type, 1);

        canvas.drawFastHLine(x + 2, y + H / 2, W - 4, 0x8410);

        canvas.setTextColor(colMain(c.e1.type), bg);
        canvas.setCursor(x + 2, y + H / 2 + 2);
        canvas.printf("%c%d", typeLetter(c.e1.type), (unsigned)c.e1.pow);
        blitIcon(canvas, x + 16, y + H / 2 + 1, iconFor(c.e1.type), c.e1.type, 1);

        canvas.fillRect(x + W - 7, y + H / 2 - 2, 5, 5, 0xF81F);
    } else {
        // Letter top-left, big icon centered
        canvas.setTextColor(colMain(c.e0.type), bg);
        canvas.setCursor(x + 3, y + 3);
        canvas.printf("%c%d", typeLetter(c.e0.type), (unsigned)c.e0.pow);
        // 10px * scale2 = 20 → center in 36-wide card
        blitIcon(canvas, x + 8, y + 14, iconFor(c.e0.type), c.e0.type, 2);
    }
}

// Single-row HP: "YOU 20 ####----" — never wraps, stays in header band
static void drawHpBar(M5Canvas& canvas, int16_t x, int16_t y,
                      uint8_t hp, uint16_t fill, const char* label) {
    canvas.setTextSize(1);
    canvas.setTextColor(0xC618, 0x1082);
    canvas.setCursor(x, y);
    canvas.printf("%s %u", label, (unsigned)hp);
    const int16_t bx = (int16_t)(x + 42);
    const int16_t bw = 48;
    canvas.fillRect(bx, y + 1, bw, 6, 0x2104);
    int w = (int)hp * bw / MAX_HP;
    if (w > 0) canvas.fillRect(bx, y + 1, w, 6, fill);
    canvas.drawRect(bx, y + 1, bw, 6, 0x8410);
}


// Sized card face for duel UI (fits MAIN_H)
static void drawCardFaceSized(M5Canvas& canvas, int16_t x, int16_t y,
                              int16_t W, int16_t H,
                              const Card& c, bool selected) {
    uint16_t bg = selected ? 0xFFE0 : 0xFFFF;
    uint16_t bd = selected ? 0xFD20 : 0x4A49;
    canvas.fillRect(x, y, W, H, bg);
    canvas.drawRect(x, y, W, H, bd);
    if (selected) canvas.drawRect(x + 1, y + 1, W - 2, H - 2, 0xC480);
    if (c.empty) return;
    canvas.setTextSize(1);
    if (c.combo) {
        canvas.setTextColor(colMain(c.e0.type), bg);
        canvas.setCursor(x + 2, y + 2);
        canvas.printf("%c%d", typeLetter(c.e0.type), (unsigned)c.e0.pow);
        blitIcon(canvas, x + W - 12, y + 1, iconFor(c.e0.type), c.e0.type, 1);
        canvas.drawFastHLine(x + 2, y + H / 2, W - 4, 0x8410);
        canvas.setTextColor(colMain(c.e1.type), bg);
        canvas.setCursor(x + 2, y + H / 2 + 2);
        canvas.printf("%c%d", typeLetter(c.e1.type), (unsigned)c.e1.pow);
        blitIcon(canvas, x + W - 12, y + H / 2 + 1, iconFor(c.e1.type), c.e1.type, 1);
    } else {
        canvas.setTextColor(colMain(c.e0.type), bg);
        canvas.setCursor(x + 2, y + 2);
        canvas.printf("%c%d", typeLetter(c.e0.type), (unsigned)c.e0.pow);
        int scale = (H >= 40) ? 2 : 1;
        int iw = 10 * scale;
        blitIcon(canvas, x + (W - iw) / 2, y + 12, iconFor(c.e0.type), c.e0.type, scale);
    }
}

void drawActive(M5Canvas& canvas) {
    // MAIN_H = 105. Strict vertical bands — no overlap.
    // 0..14   header HP + score
    // 15..26  status (one line)
    // 27..78  cards
    // 79..88  combat stats (resolve only)
    // 89..104 footer keys
    const int16_t W = 240;
    const int16_t H = 105;

    canvas.fillSprite(0x0841);

    // --- HEADER band ---
    canvas.fillRect(0, 0, W, 15, 0x1082);
    canvas.fillRect(0, 14, W, 1, 0x2104);
    drawHpBar(canvas, 2, 3, s_youHp, 0x07E0, "YOU");
    drawHpBar(canvas, 148, 3, s_aiHp, 0xF800, "AI");
    // score centered in gap between bars (~x 100)
    canvas.setTextSize(1);
    canvas.setTextColor(0xFFE0, 0x1082);
    canvas.setCursor(100, 2);
    canvas.printf("R%u", (unsigned)s_round);
    canvas.setTextColor(0xC618, 0x1082);
    canvas.setCursor(96, 10);
    canvas.printf("%u-%u", (unsigned)s_youWins, (unsigned)s_aiWins);

    // --- STATUS band (clear) ---
    canvas.fillRect(0, 15, W, 12, 0x0841);
    canvas.setTextColor(0xFFFF, 0x0841);
    canvas.setCursor(4, 17);
    if (s_msg[0]) {
        // truncate so it never wraps under cards
        char buf[28];
        size_t i = 0;
        while (s_msg[i] && i < 27) { buf[i] = s_msg[i]; i++; }
        buf[i] = '\0';
        canvas.print(buf);
    } else if (s_phase == Phase::SELECT) {
        canvas.setTextColor(0xC618, 0x0841);
        canvas.printf("Pick %u/2  %s",
                      (unsigned)s_selCount,
                      s_youFirst ? "You first" : "AI first");
    } else if (s_phase == Phase::RESOLVE) {
        canvas.print("Resolve...");
    } else if (s_phase == Phase::ROUND_OVER) {
        canvas.print("Round over");
    } else if (s_phase == Phase::MATCH_OVER) {
        canvas.print("Match over");
    }

    // --- FOOTER band background ---
    canvas.fillRect(0, 89, W, H - 89, 0x1082);
    canvas.fillRect(0, 88, W, 1, 0x2104);

    const int16_t CW = 32, CH = 38;

    if (s_phase == Phase::SELECT) {
        const int16_t gap = 4;
        const int16_t total = (int16_t)(HAND_N * CW + (HAND_N - 1) * gap);
        const int16_t x0 = (int16_t)((W - total) / 2);
        for (uint8_t i = 0; i < HAND_N; i++) {
            int16_t x = (int16_t)(x0 + i * (CW + gap));
            drawCardFaceSized(canvas, x, 28, CW, CH, s_hand[i], s_sel[i]);
            canvas.setTextColor(s_sel[i] ? 0xFFE0 : 0x8410, 0x0841);
            canvas.setCursor(x + CW / 2 - 3, 68);
            canvas.printf("%u", (unsigned)(i + 1));
        }
        canvas.setTextColor(0xC618, 0x1082);
        canvas.setCursor(4, 94);
        canvas.print("1-5 card");
        canvas.setCursor(70, 94);
        if (s_selCount == PICK_N) {
            canvas.setTextColor(0xFFE0, 0x1082);
            canvas.print("ENT play");
        } else {
            canvas.setTextColor(0x8410, 0x1082);
            canvas.print("need 2");
        }
        canvas.setTextColor(0x8410, 0x1082);
        canvas.setCursor(170, 94);
        canvas.print("` exit");
        return;
    }

    // RESOLVE / ROUND_OVER / MATCH_OVER
    // Labels above cards only
    canvas.setTextColor(0x07E0, 0x0841);
    canvas.setCursor(20, 28);
    canvas.print("YOU");
    canvas.setTextColor(0xF800, 0x0841);
    canvas.setCursor(150, 28);
    canvas.print("AI");

    for (uint8_t i = 0; i < PICK_N; i++) {
        drawCardFaceSized(canvas, (int16_t)(8 + i * (CW + 6)), 38, CW, CH,
                          s_youPlay[i], false);
        drawCardFaceSized(canvas, (int16_t)(130 + i * (CW + 6)), 38, CW, CH,
                          s_aiPlay[i], false);
    }

    // Stats band 79..87 — one short line each side
    canvas.fillRect(0, 78, W, 10, 0x0841);
    canvas.setTextColor(0x07E0, 0x0841);
    canvas.setCursor(4, 79);
    canvas.printf("A%d D%d H%d", s_youSum.atk, s_youSum.def, s_youSum.heal);
    canvas.setTextColor(0xF800, 0x0841);
    canvas.setCursor(130, 79);
    canvas.printf("A%d D%d H%d", s_aiSum.atk, s_aiSum.def, s_aiSum.heal);

    canvas.setTextColor(0xC618, 0x1082);
    canvas.setCursor(4, 94);
    if (s_dmgYou || s_dmgAi || s_healYou || s_healAi) {
        canvas.printf("dmg %d/%d  +%d/+%d",
                      (int)s_dmgYou, (int)s_dmgAi,
                      (int)s_healYou, (int)s_healAi);
    } else {
        canvas.print("—");
    }
    canvas.setTextColor(0xFFE0, 0x1082);
    canvas.setCursor(155, 94);
    if (s_phase == Phase::MATCH_OVER)
        canvas.print("ENT ok");
    else
        canvas.print("ENT next");
}




void getStatusLine(char* buf, size_t n) {
    if (!buf || n == 0) return;
    if (!s_active) { buf[0] = '\0'; return; }
    if (s_phase == Phase::SELECT) {
        if (s_selCount == PICK_N)
            snprintf(buf, n, "1-5 card  ENT play  ` exit");
        else
            snprintf(buf, n, "1-5 pick 2 cards  ` exit");
    } else if (s_phase == Phase::MATCH_OVER) {
        snprintf(buf, n, "ENT confirm  ` exit");
    } else {
        snprintf(buf, n, "ENT next  ` exit");
    }
}

}  // namespace CardsTable

