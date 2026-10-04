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

static constexpr int16_t GROUND_Y = 117;
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

static uint8_t countCardEffects(const Card* cards, CType type);

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
static int8_t s_firstDamage, s_secondDamage;
static uint32_t s_phaseUntil;
static uint32_t s_resolveStart;
static uint8_t s_youHpBefore, s_aiHpBefore;
static bool s_resolveYouFirst;
static bool s_firstHitCuePlayed, s_secondHitCuePlayed;
static bool s_secondExchange, s_roundEndPending;
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
    s_roundEndPending = false;
    s_secondExchange = false;
    s_youSum = {};
    s_aiSum = {};
    s_dmgYou = s_dmgAi = s_healYou = s_healAi = 0;
    s_firstWasYou = (esp_random() & 1) != 0;
    beginRound(s_firstWasYou);
    snprintf(s_msg, sizeof(s_msg), "R%d GO", s_round);
}

static void resolveTurn() {
    s_youHpBefore = s_youHp;
    s_aiHpBefore = s_aiHp;
    s_resolveYouFirst = s_youFirst;
    uint8_t p = 0;
    for (uint8_t i = 0; i < HAND_N && p < PICK_N; i++) {
        if (s_sel[i]) s_youPlay[p++] = s_hand[i];
    }
    while (p < PICK_N) { s_youPlay[p].empty = true; p++; }

    aiPick();
    s_youSum = sumPlay(s_youPlay, PICK_N);
    s_aiSum  = sumPlay(s_aiPlay, PICK_N);
    s_dmgYou = s_dmgAi = s_healYou = s_healAi = 0;
    s_firstDamage = s_secondDamage = 0;

    auto actFirst = [&](bool youAreFirst) {
        if (youAreFirst) {
            int8_t dT = 0, dA = 0;
            applyAttack(s_youSum.atk, s_aiSum.def, s_aiHp, s_youHp, dT, dA);
            s_firstDamage = dT;
            s_dmgAi  = (int8_t)(s_dmgAi + dT);
            s_dmgYou = (int8_t)(s_dmgYou + dA);
            int8_t h = 0;
            applyHeal(s_youSum.heal, s_youHp, h);
            s_healYou = (int8_t)(s_healYou + h);
        } else {
            int8_t dT = 0, dA = 0;
            applyAttack(s_aiSum.atk, s_youSum.def, s_youHp, s_aiHp, dT, dA);
            s_firstDamage = dT;
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
            s_secondDamage = dT;
            s_dmgYou = (int8_t)(s_dmgYou + dT);
            s_dmgAi  = (int8_t)(s_dmgAi + dA);
            int8_t h = 0;
            applyHeal(s_aiSum.heal, s_aiHp, h);
            s_healAi = (int8_t)(s_healAi + h);
        } else {
            int8_t dT = 0, dA = 0;
            applyAttack(s_youSum.atk, s_aiSum.def, s_aiHp, s_youHp, dT, dA);
            s_secondDamage = dT;
            s_dmgAi  = (int8_t)(s_dmgAi + dT);
            s_dmgYou = (int8_t)(s_dmgYou + dA);
            int8_t h = 0;
            applyHeal(s_youSum.heal, s_youHp, h);
            s_healYou = (int8_t)(s_healYou + h);
        }
    };

    actFirst(s_youFirst);
    // Defender's DEF was spent on the first exchange — second hit faces 0 DEF
    s_secondExchange = s_youHp > 0 && s_aiHp > 0;
    if (s_secondExchange) {
        if (s_youFirst) s_aiSum.def = 0;
        else             s_youSum.def = 0;
        actSecond(s_youFirst);
    }

    s_youFirst = !s_youFirst;
    s_roundEndPending = s_youHp == 0 || s_aiHp == 0;
    s_phase = Phase::RESOLVE;
    s_resolveStart = millis();
    s_phaseUntil = s_resolveStart + 4000;
    s_firstHitCuePlayed = false;
    s_secondHitCuePlayed = false;
    s_msg[0] = '\0';
}

void end(); // fwd

static void advanceAfterPause() {
    if (s_phase == Phase::MATCH_OVER) {
        end();
        return;
    }
    if (s_phase == Phase::RESOLVE) {
        if (!s_roundEndPending) {
            beginTurn();
            return;
        }

        if (s_youHp == 0 && s_aiHp == 0) {
            s_youWonLastRound = false;
            snprintf(s_msg, sizeof(s_msg), "DRAW R%u", (unsigned)s_round);
        } else if (s_aiHp == 0) {
            s_youWins++;
            s_youWonLastRound = true;
            snprintf(s_msg, sizeof(s_msg), "YOU WIN R%u", (unsigned)s_round);
            SFX::play(SFX::MENU_CLICK);
        } else {
            s_aiWins++;
            s_youWonLastRound = false;
            snprintf(s_msg, sizeof(s_msg), "AI WIN R%u", (unsigned)s_round);
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
        } else {
            s_phase = Phase::ROUND_OVER;
            s_phaseUntil = millis() + 2200;
        }
        s_roundEndPending = false;
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

        if (s_phase == Phase::RESOLVE) {
            const uint32_t elapsed = now - s_resolveStart;
            if (!s_firstHitCuePlayed && elapsed >= 750) {
                s_firstHitCuePlayed = true;
                const PlaySum& first = s_resolveYouFirst ? s_youSum : s_aiSum;
                const Card* firstCards = s_resolveYouFirst ? s_youPlay : s_aiPlay;
                const uint8_t attackCount = countCardEffects(firstCards, CType::ATK);
                const uint8_t healCount = countCardEffects(firstCards, CType::HEAL);
                if (first.atk) {
                    SFX::play(SFX::ATTACK_HOP);
                    if (attackCount >= 2) SFX::play(SFX::BIRD_IMPACT);
                } else if (first.heal) {
                    SFX::play(SFX::CONFIRM);
                    if (healCount >= 2) SFX::play(SFX::CONFIRM);
                }
                else if (first.def) {
                    SFX::play(SFX::MENU_CLICK);
                    if (countCardEffects(firstCards, CType::DEF) >= 2)
                        SFX::play(SFX::BIRD_HIT);
                }
            }
            if (s_secondExchange && !s_secondHitCuePlayed && elapsed >= 2400) {
                s_secondHitCuePlayed = true;
                const PlaySum& second = s_resolveYouFirst ? s_aiSum : s_youSum;
                const Card* secondCards = s_resolveYouFirst ? s_aiPlay : s_youPlay;
                const uint8_t attackCount = countCardEffects(secondCards, CType::ATK);
                const uint8_t healCount = countCardEffects(secondCards, CType::HEAL);
                if (second.atk) {
                    SFX::play(SFX::ATTACK_HOP);
                    if (attackCount >= 2) SFX::play(SFX::BIRD_IMPACT);
                } else if (second.heal) {
                    SFX::play(SFX::CONFIRM);
                    if (healCount >= 2) SFX::play(SFX::CONFIRM);
                }
                else if (second.def) {
                    SFX::play(SFX::MENU_CLICK);
                    if (countCardEffects(secondCards, CType::DEF) >= 2)
                        SFX::play(SFX::BIRD_HIT);
                }
            }
        }

        if (s_phase == Phase::RESOLVE || s_phase == Phase::ROUND_OVER ||
            s_phase == Phase::MATCH_OVER) {
            // Timer OR Enter — never both in one frame (would skip a phase)
            if (now >= s_phaseUntil) {
                advanceAfterPause();
            } else if ((s_phase != Phase::RESOLVE ||
                        now - s_resolveStart >= 3650u) &&
                       keyNewPress(s_entLatch) && keyEnter()) {
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

static const char* typeName(CType t) {
    if (t == CType::ATK) return "ATK";
    if (t == CType::DEF) return "DEF";
    return "HEAL";
}

static uint16_t cardInk(CType t) {
    if (t == CType::ATK) return 0xFCA0;
    if (t == CType::DEF) return 0x4DFF;
    return 0xF96A;
}

static uint16_t cardShade(CType t) {
    if (t == CType::ATK) return 0x5108;
    if (t == CType::DEF) return 0x112B;
    return 0x5009;
}

static uint16_t tint565(uint16_t color, uint16_t bg, uint8_t amount) {
    const uint16_t inverse = (uint16_t)(255 - amount);
    const uint16_t red = (uint16_t)((((color >> 11) & 0x1F) * amount +
        ((bg >> 11) & 0x1F) * inverse) / 255);
    const uint16_t green = (uint16_t)((((color >> 5) & 0x3F) * amount +
        ((bg >> 5) & 0x3F) * inverse) / 255);
    const uint16_t blue = (uint16_t)(((color & 0x1F) * amount +
        (bg & 0x1F) * inverse) / 255);
    return (uint16_t)((red << 11) | (green << 5) | blue);
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
                      uint8_t hp, uint16_t fill, bool rightSide) {
    canvas.setTextSize(1);
    char hpText[12];
    snprintf(hpText, sizeof(hpText), rightSide ? "%u/%u AI" : "YOU %u/%u",
             (unsigned)hp, (unsigned)MAX_HP);
    canvas.setTextColor(rightSide ? 0xFCA0 : 0x07E0, 0x1082);
    const int16_t textX = rightSide ? x + 34 : x;
    canvas.setCursor(textX, y);
    canvas.print(hpText);
    const int16_t bx = rightSide ? x : (int16_t)(x + 56);
    const int16_t bw = 31;
    canvas.fillRect(bx, y + 1, bw, 6, 0x2104);
    int w = (int)hp * bw / MAX_HP;
    if (w > 0) canvas.fillRect(bx, y + 1, w, 6, fill);
    canvas.drawRect(bx, y + 1, bw, 6, 0x8410);
}

static uint8_t animatedHp(uint8_t before, uint8_t after, uint32_t elapsed) {
    uint8_t progress = elapsed < 900 ? 0 : elapsed < 2100 ? 1 :
                       elapsed < 3250 ? 2 : 3;
    int value = before + ((int)after - (int)before) * progress / 3;
    if (value < 0) value = 0;
    if (value > MAX_HP) value = MAX_HP;
    return (uint8_t)value;
}

static void drawImpact(M5Canvas& canvas, int16_t cx, int16_t cy,
                       uint16_t color, uint32_t age, uint8_t power) {
    if (age > 680u) return;
    const int radius = 5 + (int)(age / 42u);
    const int pulse = (int)((age / 85u) % 3u);
    const uint16_t bright = (age / 110u) % 2u ? 0xFFE0 : 0xFFFF;
    canvas.drawCircle(cx, cy, radius + pulse, color);
    if (age < 360u)
        canvas.drawCircle(cx, cy, radius / 2 + 2, bright);
    for (int ray = 0; ray < 12; ray++) {
        const int dx = (ray & 1) ? radius / 2 : radius;
        const int dy = (ray & 1) ? radius : radius / 2;
        const int sx = (ray & 2) ? -dx : dx;
        const int sy = (ray & 4) ? -dy : dy;
        const int startX = cx + sx / 3;
        const int startY = cy + sy / 3;
        const int endX = cx + sx + ((ray % 3) - 1) * pulse;
        const int endY = cy + sy - ((ray % 2) * pulse);
        canvas.drawLine(startX, startY, endX, endY,
                        (ray + (int)(age / 80u)) % 3 == 0 ? bright : color);
    }
    for (int spark = 0; spark < 4; spark++) {
        const int side = (spark & 1) ? -1 : 1;
        const int sx = cx + side * (radius + 2 + (spark / 2) * 3);
        const int sy = cy + ((spark & 2) ? -radius / 2 : radius / 2);
        canvas.drawPixel(sx, sy, bright);
        canvas.drawPixel(sx + side, sy - 1, color);
    }
    canvas.fillCircle(cx, cy, 2, 0xFFFF);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(color);
    char damage[6];
    snprintf(damage, sizeof(damage), "-%u", (unsigned)power);
    canvas.drawString(damage, cx, cy - radius - 10);
    canvas.setTextDatum(top_left);
}

static void drawActionEffect(M5Canvas& canvas, const PlaySum& play,
                             uint32_t age, uint8_t power,
                             uint16_t cardColor, uint8_t effectCount) {
    if (!play.atk && !play.def && !play.heal) return;
    const int16_t targetX = 120;
    const int16_t selfX = 120;
    if (play.atk) {
        if (power == 0 && age < 680u) {
            const int radius = 5 + (int)(age / 42u);
            const uint16_t color = cardInk(CType::DEF);
            canvas.drawCircle(targetX, 53, radius + 2, 0xFFFF);
            canvas.drawCircle(targetX, 53, radius, color);
            canvas.drawFastHLine(targetX - radius, 53, radius * 2 + 1, color);
            canvas.drawFastVLine(targetX, 53 - radius, radius * 2 + 1, color);
            canvas.setTextColor(color);
            canvas.setTextDatum(top_center);
            canvas.drawString("BLOCK", targetX, 53 - radius - 10);
            canvas.setTextDatum(top_left);
            return;
        }
        drawImpact(canvas, targetX, 53, cardColor, age, power);
        if (effectCount >= 2 && age < 900u) {
            const int boom = 13 + (int)(age / 60u);
            canvas.drawCircle(targetX, 53, boom, 0xFFE0);
            canvas.drawCircle(targetX, 53, boom + 4, cardColor);
            canvas.drawFastHLine(targetX - boom - 6, 53,
                                 boom * 2 + 13, 0xFFE0);
            canvas.drawFastVLine(targetX, 53 - boom - 6,
                                 boom * 2 + 13, 0xFFE0);
            canvas.drawLine(targetX - boom, 53 - boom,
                            targetX - boom - 4, 53 - boom - 4, cardColor);
            canvas.drawLine(targetX + boom, 53 + boom,
                            targetX + boom + 4, 53 + boom + 4, cardColor);
        }
        return;
    }

    if (age > 680u) return;
    const int radius = 5 + (int)(age / 45u);
    const uint16_t color = play.def ? cardInk(CType::DEF) : cardInk(CType::HEAL);
    const int16_t cx = selfX;
    const int16_t cy = 54;
    canvas.drawCircle(cx, cy, radius, color);
    if (radius > 3) canvas.drawCircle(cx, cy, radius - 2, color);
    if (play.def) {
        canvas.drawFastVLine(cx, cy - radius + 2, radius, color);
        canvas.drawLine(cx - 4, cy, cx, cy + 4, color);
        canvas.drawLine(cx, cy + 4, cx + 5, cy - 5, color);
        if (effectCount >= 2) {
            canvas.drawCircle(cx, cy, radius + 4, 0xBFFF);
            canvas.drawCircle(cx, cy, radius + 7, color);
            canvas.drawFastHLine(cx - radius - 5, cy - 2,
                                 radius * 2 + 11, 0xBFFF);
            canvas.drawFastHLine(cx - radius - 5, cy + 2,
                                 radius * 2 + 11, color);
        }
        canvas.setTextColor(color);
        canvas.setTextDatum(top_center);
        canvas.drawString("BLOCK", cx, cy - radius - 9);
        canvas.setTextDatum(top_left);
    } else if (play.heal) {
        canvas.fillRect(cx - 2, cy - 6, 5, 13, color);
        canvas.fillRect(cx - 6, cy - 2, 13, 5, color);
        canvas.drawCircle(cx, cy, radius, 0xF96A);
        canvas.drawPixel(cx - radius, cy - radius / 2, 0xFFFF);
        canvas.drawPixel(cx + radius, cy + radius / 2, 0xFFFF);
        if (effectCount >= 2) {
            for (int drop = 0; drop < 5; drop++) {
                const int16_t dx = (int16_t)((drop - 2) * 7);
                const int16_t fall = (int16_t)((age / 45u + drop * 3u) % 12u);
                const int16_t dropY = (int16_t)(cy - 15 + fall);
                canvas.fillCircle(cx + dx, dropY, 2,
                                  (drop & 1) ? 0xF800 : 0xFFE0);
                canvas.fillTriangle(cx + dx - 2, dropY,
                                    cx + dx + 2, dropY,
                                    cx + dx, dropY - 4, 0xF800);
            }
            canvas.drawCircle(cx, cy, radius + 4, 0xF800);
        }
        canvas.setTextColor(color);
        canvas.setTextDatum(top_center);
        char healed[6];
        snprintf(healed, sizeof(healed), "+%u", (unsigned)power);
        canvas.drawString(healed, cx, cy - radius - 9);
        canvas.setTextDatum(top_left);
    }
}

static const char* actionLabel(const PlaySum& play) {
    if (play.atk) return "ATTACK";
    if (play.def) return "DEFEND";
    if (play.heal) return "HEAL";
    return "REST";
}

static uint8_t countCardEffects(const Card* cards, CType type) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < PICK_N; i++) {
        if (cards[i].empty) continue;
        if (cards[i].e0.type == type) count++;
        if (cards[i].combo && cards[i].e1.type == type) count++;
    }
    return count;
}

static void drawCardEffect(M5Canvas& canvas, CType type, int16_t cx,
                           int16_t cy, int16_t size, uint32_t now) {
    const int16_t half = size / 2;
    const uint8_t phase = (uint8_t)((now / 180u) % 3u);
    if (type == CType::ATK) {
        const int16_t sway = (int16_t)phase - 1;
        const uint16_t flame = 0xF800;
        const uint16_t fire = 0xFD20;
        const uint16_t core = 0xFFE0;
        canvas.fillTriangle(cx, cy + half, cx - half, cy + 1,
                            cx - half / 3 + sway, cy - half, flame);
        canvas.fillTriangle(cx, cy + half, cx + half, cy + 1,
                            cx + half / 3 + sway, cy - half, flame);
        canvas.fillTriangle(cx, cy + half - 1, cx - half + 2, cy + 2,
                            cx + half - 2, cy + 2, fire);
        canvas.fillTriangle(cx, cy + half / 2, cx - half / 2,
                            cy + half / 3, cx + sway, cy - half / 2, core);
        canvas.drawPixel(cx - half / 2, cy - half / 2 + phase, 0xFFFF);
        canvas.drawPixel(cx + half / 2, cy - half / 3 - phase, 0xFFFF);
    } else if (type == CType::DEF) {
        const uint8_t pulse = (uint8_t)(phase * 2u);
        const uint16_t aura = phase == 1 ? 0xBFFF : 0x4DFF;
        canvas.drawCircle(cx, cy, half - 1 + pulse, tint565(aura, 0x0841, 190));
        canvas.drawCircle(cx, cy, half - 4, tint565(aura, 0x0841, 140));
        canvas.fillRect(cx - half / 2, cy - half / 2, half, half / 2,
                        0x3A9F);
        canvas.fillTriangle(cx - half / 2, cy - 1, cx + half / 2, cy - 1,
                            cx, cy + half, 0x3A9F);
        canvas.drawFastHLine(cx - half / 2, cy - half / 2, half, 0xBFFF);
        canvas.drawLine(cx - half / 2, cy - half / 2, cx - half / 2 + 1,
                        cy + 1, 0x8E7F);
        canvas.drawLine(cx + half / 2, cy - half / 2, cx + half / 2 - 1,
                        cy + 1, 0x8E7F);
        canvas.drawLine(cx - 2, cy - 1, cx, cy + 2, 0xFFFF);
        canvas.drawLine(cx, cy + 2, cx + 3, cy - 2, 0xFFFF);
        canvas.drawPixel(cx - half, cy - phase, 0xFFFF);
        canvas.drawPixel(cx + half, cy + phase, 0xFFFF);
    } else {
        const int16_t spacing = (size >= 18) ? 7 : 4;
        const int16_t dropY = (int16_t)((now / 120u) % 7u);
        for (int8_t i = -1; i <= 1; i++) {
            const int16_t dx = (int16_t)(i * spacing);
            const int16_t dy = (int16_t)((dropY + i * 2 + 7) % 7);
            const int16_t dropX = cx + dx;
            const int16_t top = cy - half / 2 + dy - 2;
            const uint16_t drop = (i == 0) ? 0xFFE0 : 0xF800;
            canvas.fillCircle(dropX, top + 3, 2, drop);
            canvas.fillTriangle(dropX - 2, top + 2, dropX + 2, top + 2,
                                dropX, top - 2, drop);
        }
        canvas.drawPixel(cx - half + 1, cy + half / 2, 0xF96A);
        canvas.drawPixel(cx + half - 1, cy + half / 2, 0xF96A);
    }
}

static void drawPowerBadge(M5Canvas& canvas, int16_t x, int16_t y,
                           uint8_t power) {
    canvas.fillRoundRect(x, y, 12, 11, 3, 0xFFE0);
    canvas.drawRoundRect(x, y, 12, 11, 3, 0xFFFF);
    canvas.setTextColor(0x0841);
    canvas.setCursor(x + 4, y + 2);
    canvas.printf("%u", (unsigned)power);
}

// Sized card face for duel UI (fits MAIN_H)
static void drawCardFaceSized(M5Canvas& canvas, int16_t x, int16_t y,
                              int16_t W, int16_t H,
                              const Card& c, bool selected) {
    const CType primary = c.empty ? CType::DEF : c.e0.type;
    const uint16_t bg = c.empty ? 0x18E3 : cardShade(primary);
    const uint16_t ink = cardInk(primary);
    if (selected) {
        canvas.drawRoundRect(x - 2, y - 2, W + 4, H + 4, 4, 0xFFE0);
        canvas.drawRoundRect(x - 1, y - 1, W + 2, H + 2, 3, 0xFCA0);
    }
    canvas.fillRoundRect(x, y, W, H, 3, bg);
    canvas.drawRoundRect(x, y, W, H, 3, selected ? 0xFFE0 : ink);
    canvas.drawRoundRect(x + 2, y + 2, W - 4, H - 4, 2,
                         tint565(ink, bg, 132));
    if (c.empty) {
        canvas.setTextSize(1);
        canvas.setTextColor(0x8410, bg);
        canvas.setTextDatum(top_center);
        canvas.drawString("REST", x + W / 2, y + H / 2 - 3);
        canvas.setTextDatum(top_left);
        return;
    }
    canvas.setTextSize(1);
    if (c.combo) {
        const int16_t halfH = (H - 5) / 2;
        const int16_t firstY = y + 2;
        const int16_t secondY = firstY + halfH + 1;
        canvas.fillRoundRect(x + 3, firstY, W - 6, halfH, 2,
                             cardShade(c.e0.type));
        canvas.fillRoundRect(x + 3, secondY, W - 6, halfH, 2,
                             cardShade(c.e1.type));
        drawCardEffect(canvas, c.e0.type, x + W - 10,
                       firstY + halfH / 2, 12, millis());
        drawCardEffect(canvas, c.e1.type, x + W - 10,
                       secondY + halfH / 2, 12, millis());
        drawPowerBadge(canvas, x + 5, firstY + (halfH - 11) / 2,
                       c.e0.pow);
        drawPowerBadge(canvas, x + 5, secondY + (halfH - 11) / 2,
                       c.e1.pow);
    } else {
        const char* label = typeName(c.e0.type);
        canvas.fillRoundRect(x + 3, y + 3, W - 6, 10, 2, ink);
        canvas.setTextColor(0x0841);
        canvas.setTextDatum(top_center);
        canvas.drawString(label, x + W / 2, y + 5);
        canvas.setTextDatum(top_left);
        drawCardEffect(canvas, c.e0.type, x + W / 2, y + 22, 17, millis());
        drawPowerBadge(canvas, x + W - 15, y + H - 14, c.e0.pow);
    }
}

static void drawCardBack(M5Canvas& canvas, int16_t x, int16_t y,
                         int16_t W, int16_t H, uint32_t elapsed) {
    canvas.fillRoundRect(x, y, W, H, 3, 0x1834);
    canvas.drawRoundRect(x, y, W, H, 3, 0x7BEF);
    canvas.drawRoundRect(x + 2, y + 2, W - 4, H - 4, 2, 0x4DFF);
    for (int row = 0; row < 5; row++) {
        const int inset = 4 + (row & 1);
        canvas.drawFastHLine(x + inset, y + 6 + row * 5, W - inset * 2,
                             ((elapsed / 70u + row) & 1u) ? 0x4DFF : 0x315A);
    }
    canvas.fillCircle(x + W / 2, y + H / 2, 5, 0x1082);
    canvas.drawCircle(x + W / 2, y + H / 2, 5, 0xFFE0);
    canvas.setTextColor(0xFFE0);
    canvas.setTextDatum(top_center);
    canvas.drawString("*", x + W / 2, y + H / 2 - 4);
    canvas.setTextDatum(top_left);
}

static void drawCombatStats(M5Canvas& canvas) {
    canvas.fillRoundRect(2, 79, 114, 10, 2, 0x1082);
    canvas.fillRoundRect(124, 79, 114, 10, 2, 0x1082);
    canvas.setTextSize(1);
    char youTotals[24];
    char aiTotals[24];
    char youResult[24];
    char aiResult[24];
    snprintf(youTotals, sizeof(youTotals), "YOU A%u D%u H%u",
             (unsigned)s_youSum.atk, (unsigned)s_youSum.def,
             (unsigned)s_youSum.heal);
    snprintf(aiTotals, sizeof(aiTotals), "AI A%u D%u H%u",
             (unsigned)s_aiSum.atk, (unsigned)s_aiSum.def,
             (unsigned)s_aiSum.heal);
    snprintf(youResult, sizeof(youResult), "DMG %d  HEAL %d",
             (int)s_dmgYou, (int)s_healYou);
    snprintf(aiResult, sizeof(aiResult), "DMG %d  HEAL %d",
             (int)s_dmgAi, (int)s_healAi);

    canvas.setTextDatum(top_left);
    canvas.setTextColor(0x07E0, 0x1082);
    canvas.drawString(youTotals, 5, 80);
    canvas.setTextDatum(top_right);
    canvas.setTextColor(0xFCA0, 0x1082);
    canvas.drawString(aiTotals, 235, 80);

    canvas.setTextDatum(top_left);
    canvas.setTextColor(0xC618, 0x1082);
    canvas.drawString(youResult, 5, 94);
    canvas.setTextDatum(top_right);
    canvas.drawString(aiResult, 235, 94);
    canvas.setTextDatum(top_left);
}

void drawActive(M5Canvas& canvas) {
    // MAIN_H = 105. Strict vertical bands — no overlap.
    // 0..14   header HP + score
    // 15..26  status (one line)
    // 27..78  cards
    // 79..104 clear breathing room below the cards
    const int16_t W = 240;
    const int16_t H = 105;
    const uint32_t elapsed = s_phase == Phase::RESOLVE
        ? millis() - s_resolveStart : 0;

    canvas.fillSprite(0x0841);

    // --- HEADER band ---
    canvas.fillRect(0, 0, W, 15, 0x1082);
    canvas.fillRect(0, 14, W, 1, 0x2104);
    const uint8_t shownYouHp = s_phase == Phase::RESOLVE
        ? animatedHp(s_youHpBefore, s_youHp, elapsed) : s_youHp;
    const uint8_t shownAiHp = s_phase == Phase::RESOLVE
        ? animatedHp(s_aiHpBefore, s_aiHp, elapsed) : s_aiHp;
    drawHpBar(canvas, 2, 3, shownYouHp, 0x07E0, false);
    drawHpBar(canvas, 151, 3, shownAiHp, 0xF800, true);
    canvas.setTextSize(1);
    canvas.setTextColor(0xFFE0, 0x1082);
    canvas.setTextDatum(top_center);
    char centerScore[16];
    snprintf(centerScore, sizeof(centerScore), "%u-R%u-%u",
             (unsigned)s_youWins, (unsigned)s_round, (unsigned)s_aiWins);
    canvas.drawString(centerScore, 120, 3);
    canvas.setTextDatum(top_left);

    // --- STATUS band ---
    canvas.fillRect(0, 15, W, 12, 0x0841);
    canvas.setTextDatum(top_center);
    if (s_phase == Phase::SELECT) {
        canvas.setTextColor(0xEF7D, 0x0841);
        const char* selectionStatus = s_selCount == PICK_N
            ? (s_youFirst ? "2/2 READY  /  YOU FIRST"
                          : "2/2 READY  /  YOU SECOND")
            : (s_youFirst ? "PICK TWO  /  YOU FIRST"
                          : "PICK TWO  /  YOU SECOND");
        canvas.drawString(selectionStatus, W / 2, 18);
    } else if (s_phase == Phase::RESOLVE) {
        const bool firstAction = elapsed < 1900 || !s_secondExchange;
        const bool actorYou = firstAction ? s_resolveYouFirst : !s_resolveYouFirst;
        const PlaySum& action = actorYou ? s_youSum : s_aiSum;
        const char* actor = actorYou ? "YOU" : "RIVAL";
        const char* actionText = actionLabel(action);
        uint16_t actionColor = action.atk ? 0xFCA0 :
                               action.def ? 0x4DFF : 0xF96A;
        canvas.setTextColor(actionColor, 0x0841);
        const bool knockout = !s_secondExchange && elapsed >= 1800;
        canvas.drawString(elapsed < 250 ||
                          (elapsed >= 1750 && elapsed < 1900)
                              ? "REVEAL!" : knockout ? "KNOCKOUT!" : actor,
                          W / 2, 18);
        canvas.setTextColor(0x9CD3, 0x0841);
        canvas.drawString(knockout ? "ROUND DECIDED" : actionText, W / 2, 25);
    } else {
        canvas.setTextColor(s_phase == Phase::MATCH_OVER ? 0xFFE0 : 0xEF7D,
                            0x0841);
        canvas.drawString(s_msg[0] ? s_msg :
                          s_phase == Phase::ROUND_OVER ? "ROUND COMPLETE" :
                          "MATCH COMPLETE", W / 2, 19);
    }
    canvas.setTextDatum(top_left);

    // --- FOOTER band background ---
    canvas.fillRect(0, 78, W, H - 78, 0x1082);
    canvas.fillRect(0, 77, W, 1, 0x2104);

    const int16_t CW = 36, CH = 41;

    if (s_phase == Phase::SELECT) {
        const int16_t gap = 5;
        const int16_t total = (int16_t)(HAND_N * CW + (HAND_N - 1) * gap);
        const int16_t x0 = (int16_t)((W - total) / 2);
        for (uint8_t i = 0; i < HAND_N; i++) {
            int16_t x = (int16_t)(x0 + i * (CW + gap));
            const int16_t cardY = s_sel[i] ? 32 : 35;
            drawCardFaceSized(canvas, x, cardY, CW, CH, s_hand[i], s_sel[i]);
        }
        drawCombatStats(canvas);
        return;
    }

    // Resolve cards advance in two clear exchanges before the result settles.
    int16_t youShift = 0;
    int16_t aiShift = 0;
    bool showActionEffect = false;
    PlaySum activeAction{};
    uint32_t activeEffectAge = 0;
    uint8_t activeEffectPower = 0;
    uint16_t activeEffectColor = 0xFFFF;
    uint8_t activeEffectCount = 0;
    if (s_phase == Phase::RESOLVE) {
        bool actionYou = false;
        uint32_t actionStart = 0;
        if (elapsed >= 250 && elapsed < 1800) {
            actionYou = s_resolveYouFirst;
            actionStart = 250;
        } else if (s_secondExchange && elapsed >= 1900 && elapsed < 3450) {
            actionYou = !s_resolveYouFirst;
            actionStart = 1900;
        }
        if (actionStart) {
            const uint32_t actionElapsed = elapsed - actionStart;
            int16_t travel = 0;
            if (actionElapsed < 450) {
                travel = (int16_t)(16 * actionElapsed / 450);
            } else if (actionElapsed < 1200) {
                travel = 16;
            } else if (actionElapsed < 1650) {
                travel = (int16_t)(16 * (1650 - actionElapsed) / 450);
            }
            if (actionYou) {
                youShift = -travel;
            } else {
                aiShift = travel;
            }

            const uint32_t impactAt = actionStart + 500u;
            if (elapsed >= impactAt) {
                const PlaySum& action = actionYou
                    ? (s_resolveYouFirst ? s_youSum : s_aiSum)
                    : (s_resolveYouFirst ? s_aiSum : s_youSum);
                const uint8_t shownPower = action.atk
                    ? (uint8_t)(actionStart == 250u ? s_firstDamage : s_secondDamage)
                    : action.heal ? (actionYou ? (uint8_t)(s_healYou > 0 ? s_healYou : 0)
                                                : (uint8_t)(s_healAi > 0 ? s_healAi : 0))
                                  : action.def;
                activeAction = action;
                activeEffectAge = elapsed - impactAt;
                activeEffectPower = shownPower;
                const CType effectType = action.atk ? CType::ATK :
                                         action.def ? CType::DEF : CType::HEAL;
                activeEffectColor = cardInk(effectType);
                const Card* effectCards = actionYou ? s_youPlay : s_aiPlay;
                activeEffectCount = countCardEffects(effectCards, effectType);
                showActionEffect = true;
            }
        }
    }

    canvas.setTextDatum(top_center);
    canvas.setTextColor(0x07E0, 0x0841);
    canvas.drawString("YOU", 60, 29);
    canvas.setTextColor(0xFCA0, 0x0841);
    canvas.drawString("RIVAL", 180, 29);
    canvas.setTextDatum(top_left);

    for (uint8_t i = 0; i < PICK_N; i++) {
        drawCardFaceSized(canvas, (int16_t)(22 + i * (CW + 4) + youShift), 36, CW, CH,
                          s_youPlay[i], false);
        const int16_t aiX = (int16_t)(142 + i * (CW + 4) + aiShift);
        if (s_phase == Phase::RESOLVE && elapsed < 250) {
            drawCardBack(canvas, aiX, 36, CW, CH, elapsed);
        } else {
            drawCardFaceSized(canvas, aiX, 36, CW, CH, s_aiPlay[i], false);
        }
    }

    if (showActionEffect) {
        drawActionEffect(canvas, activeAction, activeEffectAge, activeEffectPower,
                         activeEffectColor, activeEffectCount);
    }

    drawCombatStats(canvas);
}




void getStatusLine(char* buf, size_t n) {
    if (!buf || n == 0) return;
    if (!s_active) { buf[0] = '\0'; return; }
    if (s_phase == Phase::SELECT) {
        if (s_selCount == PICK_N)
            snprintf(buf, n, "1-5 CARD  ENT PLAY  ESC EXIT");
        else
            snprintf(buf, n, "1-5 PICK 2 CARDS  ESC EXIT");
    } else if (s_phase == Phase::MATCH_OVER) {
        snprintf(buf, n, "ENT CONFIRM  ESC EXIT");
    } else {
        snprintf(buf, n, "ENT NEXT  ESC EXIT");
    }
}

}  // namespace CardsTable
