// Scene trees — separate from Avatar so we can mix kinds freely.
//
// (1) FRUIT  — seasonal main tree (spring cherry / summer apple /
//              autumn old apple / winter fir); drops produce when applicable
// (2) DECOR  — seasonal scenery; spring = weeping willow, winter = snowy
//              classic (never a 2nd fir / 2nd cherry)
// (3) BERRY  — bush; berries fall when stomped
// Spring cherry: pink blossom canopy + paired cherries
// Autumn old apple: soft green apples (bush stays berries)
//
// None push the pig. All can be broken by airborne stomps (jumps).
// Call Trees::draw after sky/clouds, before pig.
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace Trees {

enum class Kind : uint8_t {
    FRUIT = 0,  // (1) fruit tree
    DECOR = 1,  // (2) decorative
    BERRY = 2   // (3) berry bush
};

// Looping farm width (px). Screen is 240; old loop was 300 (~125%).
static constexpr int16_t WORLD_SPAN = 600;
static constexpr int16_t WORLD_WRAP_HI = 540;  // SPAN - 60
static constexpr int16_t WORLD_WRAP_LO = -60;

void init();
void reset();

// === Spawn / hide ===
void showFruit(uint8_t fruitCount);
void hideFruit();
bool isFruitVisible();

void showDecor();   // kind (2)
void showBerry();   // kind (3)
void hideDecor();
void hideBerry();
void hideAll();

// === Gameplay ===
void dropFruit();   // fruit tree only (ambient rain)
bool tryCollectNearbyFruit(int pigCenterX, int pigFeetY, int radius = 18);
// Idle ambient: random fruit-tree grow/drop + auto-collect near pig.
// Returns how many fruit/berries were picked up this call.
uint8_t updateAmbient(int pigCenterX, int pigFeetY, int pigHintX, bool pigOnRight);
// Airborne stomp near ANY tree/bush; 3 hits collapses. Returns true if hit landed.
bool tryStompFruitTree(int pigFeetX, bool airborne);
int16_t getFruitTreeScreenX();  // -1 if none
int16_t nearestFloraScreenX(int fromX);  // nearest live tree/bush, or -1

// Always false — trees do not push (kept for API parity)
bool checkFruitPush(int pigLeft, int pigRight, int16_t& treeScreenX);

// Scroll with grass treadmill
void scroll(int dir);  // +1 or -1 per grass step

// Wave hit shake (optional)
void shakeFromWave();

// Extra jitter while pig stomps
void setStompShake(int8_t shake);

// Draw all trees (+ falling fruit/berries). yOffset for top-bar bleed.
void draw(M5Canvas& canvas, int16_t yOffset = 0);
void drawUnderground(M5Canvas& canvas, uint16_t rootColor,
                     uint16_t rootHighlight);
// Draw falling drops/splashes in the foreground (call after front grass layer)
void drawDropsForeground(M5Canvas& canvas);
void drawBarOverflow(M5Canvas& bar);

// Pig position hints for spawn placement
void setPigHint(int pigX, bool pigOnRight);

}  // namespace Trees
