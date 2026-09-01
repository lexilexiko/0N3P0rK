// Full-color chibi pixel-art pig (uint8 map, not string sprites; API unchanged)
#pragma once

#include <M5Unified.h>

enum class AvatarState {
    NEUTRAL,
    HAPPY,
    EXCITED,
    HUNTING,
    SLEEPY,
    SAD,
    ANGRY
};

enum class WaveMode : uint8_t {
    NONE,      // No waves (idle, cooldown, bored)
    INCOMING,  // Converging toward nose (scanning/sniffing)
    OUTGOING   // Radiating from nose (deauth/sending)
};

enum class TreePhase : uint8_t {
    HIDDEN = 0,   // Not visible
    GROWING,      // Trunk → canopy → fruits animation
    ALIVE,        // Fully grown, gentle sway + fruit bob
    COLLAPSING    // Reverse animation
};

class Avatar {
public:
    static void init();
    static void draw(M5Canvas& canvas);
    // Suspend pig scene (weather / wolf / trees ambient / mood ticks) during
    // heavy work: PigPass, EvilPig, WPA-SEC, WiGLE, File Xfer. Refcounted —
    // pair every suspendScene() with resumeScene(). Frees CPU + avoids
    // competing with TLS / PBKDF2 for cycles (heap stays mostly BSS).
    static void suspendScene();
    static void resumeScene();
    static bool isSceneSuspended();
    // Scene sky color for matching top bar (avatar weather screens)
    static uint16_t getSkyColor();
    static void setState(AvatarState state);
    static AvatarState getState() { return currentState; }
    static bool isFacingRight();  // Get current facing direction
    static bool isOnRightSide();  // Get screen position (for bubble placement)
    static bool isTransitioning();  // True during walk transition (hide bubble)
    static int getCurrentX();  // Get current animated X position
    static void setX(int x);   // Place pig (boot splash / tests)

    // Phase 8: Intensity-based animation modifiers
    static void setMoodIntensity(int intensity);  // -100 to 100, affects blink/flip rates

    static void blink();
    static void sniff();  // Trigger nose sniff animation (600ms animated cycle)
    static void wiggleEars();
    static void cuteJump();  // Trigger cute celebratory jump (higher than walk bounce)
    static bool isJumping() { return jumpActive; }
    // Vertical lift in px (0 on ground, positive = up) — for hitboxes
    static int getJumpLiftPx();
    // Temporary jump tuning (Fruit Run uses higher/longer jump)
    static void setJumpTuning(int heightPx, uint16_t durationMs);
    static void resetJumpTuning();

    // Event reaction animations
    static void perkUp();       // Ears pop up + quick upward bounce (new network)
    static void flinch();       // Duck down + horizontal jitter (error/heap critical)
    static void spin();         // Rapid direction flips (level-up celebration)
    static void pawScratch();   // Small X oscillation (bored)
    static void triggerTailWiggle();  // Burst tail wag on celebrations

    // Sparkle particles (achievements, level-up celebrations)
    static void triggerSparkles(uint8_t count = 6);
    // Full-body continuous sparkles (IR blast aura — pig covered in stars)
    static void setSparkleStorm(bool on);
    static bool isSparkleStorm();

    // Direction control
    static void setFacingLeft();
    static void setFacingRight();

    // Attack shake (visual feedback for captures)
    static void setAttackShake(bool active, bool strong);

    // Attack hop (multi-hop pounce animation for captures / Fruit Run attack)
    static void attackHop();
    static bool isAttackHopping();

    // Fruit Run: stomp tree while airborne (3 hits → collapse). Returns true if hit.
    static bool tryStompTree();
    // Screen X of tree trunk (for gameplay hints); -1 if no tree
    static int16_t getTreeScreenX();

    // Thunder flash (invert colors for weather effect)
    static void setThunderFlash(bool active);
    static bool isThunderFlashing();

    // Wave ripple animation (radio activity feedback)
    // Burst-based: each call extends a 2700ms burst without resetting phase.
    // OUTGOING priority: active OUTGOING burst can't be overridden by INCOMING
    static void waveRipple(WaveMode mode, uint8_t intensity = 3);  // intensity: 1-5 rings
    static WaveMode getWaveMode() { return waveMode; }

    // MicPork dance: pig body becomes the spectrum (0=off, 1=max energy)
    static void setMicDance(float level);
    static float getMicDance() { return micDanceLevel; }

    // Bird-wave collision check (called by Weather bird system)
    static bool checkBirdWaveCollision(int16_t bx, int16_t by);

    // Night sky (RTC / synthetic cycle / SKY setting)
    // isNightTime: true when past dusk mid-point (blend ≥ half night)
    static bool isNightTime();
    static void getSkyHud(char* out, size_t len);
    // Tree crown tops into top bar (call after bar sky fill, before text)
    static void drawTreeBarOverflow(M5Canvas& bar);

    // Walk wind-up animation (smooth slide for coast-back)
    static void startWindupSlide(int targetX, bool faceRight = false);
    // Real walk (legs), not a teleport. sitAfter = hide when she arrives.
    static void walkTo(int targetX, bool sitAfter = false);
    static void fleeToHide();   // walk behind flora / far edge, then sit
    static void walkToFood();   // hungry: walk to nearest tree/bush
    static bool isHiding();

    // Fruit tree visualization (juicy channel indicator)
    static void showTree(uint8_t fruitCount);  // Triggers growth with N fruits
    static void hideTree();                     // Triggers collapse
    static bool isTreeVisible();               // True if not HIDDEN
    static void dropFruit();                   // Detach one fruit, start it falling
    // Fruit Run: pick up a falling/ground fruit near the pig (returns true if collected)
    static bool tryCollectNearbyFruit(int pigCenterX, int pigFeetY, int radius = 18);
    // Free walk for mini-games / IDLE player control (clamped to scene)
    static void nudgeX(int dx);
    // Call while player holds move keys — walk-leg anim + delays auto roam
    static void setManualWalk(bool walking);
    // Player hold-to-walk: scroll grass/trees like Piggy Blues treadmill (no edge park)
    // faceRight=true → pig faces right, world scrolls left
    static void setPlayerWalkScroll(bool walking, bool faceRight = true);
    // Hold walk: dir -1 left / +1 right / 0 stop.
    // Pig walks freely in middle; world scrolls only in outer ~33% edge zones.
    static void playerWalkHold(int dir, bool fromAi = false);
    static void notifyPlayerControl();

    // Player poses (IDLE free roam)
    static void setSitting(bool on);     // sit on haunches
    // Same pixel pig silhouette (friend / NPC). feet = ground contact.
    static void drawCompanion(M5Canvas& canvas, int16_t feetX, int16_t feetY,
                             bool faceRight, bool walking, bool sitting,
                             bool sniffing = false, bool fallen = false,
                             bool asZombie = false);
    static bool isSitting();
    static void setPlayDead(bool on);    // flat on back (X_X)
    static bool isPlayDead();
    // Wolf bite: play-dead + lock walk/jump/attack for ~10s
    static void onWolfBitten();
    static bool isControlLocked();  // true while stunned by wolf

    // Grass animation control (direction: true=right, false=left)
    // force=true: skip rest cooldown (mini-games / mode switches)
    static void setGrassMoving(bool moving, bool directionRight = true, bool force = false);
    static bool isGrassMoving() { return grassMoving; }
    static bool isGrassDirectionRight() { return grassDirection; }
    static uint16_t getGrassSpeed() { return grassSpeed; }
    static void setGrassSpeed(uint16_t ms);  // Speed in ms per shift (lower = faster)
    static void resetGrass();  // Re-randomize blade array

private:
    // Star system state
    struct Star {
        int16_t x;              // screen x range 0-239
        int16_t y;              // screen y range 20-100
        uint8_t size;           // 1-2 px radius
        uint8_t brightness;     // 0-255, 0 means hidden
        bool isBlinking;        // twinkle behavior
        uint32_t fadeInStart;   // when this star started appearing
    };
    static Star stars[15];
    static uint8_t starCount;
    static constexpr uint8_t MAX_STARS = 15;
    static uint32_t lastStarSpawn;
    static uint32_t nextSpawnDelay;
    static bool starsActive;

    static void initStarPositions();
    static void updateStars();
    static void drawStars(M5Canvas& canvas);
    static AvatarState currentState;
    static bool isBlinking;
    static bool isSniffing;
    static bool earsUp;
    static uint32_t lastBlinkTime;
    static uint32_t blinkInterval;
    static int moodIntensity;  // Phase 8: -100 to 100

    // Cute jump animation state
    static bool jumpActive;
    static uint32_t jumpStartTime;
    static uint16_t jumpDurationMs;  // default 400; tunable for mini-games
    static int jumpHeightPx;         // default 8; tunable for mini-games

    // Attack hop animation state (multi-hop pounce for captures)
    static bool attackHopActive;
    static uint32_t attackHopStartTime;
    static uint8_t attackHopIndex;          // current hop (0-based)
    static uint8_t attackHopTotal;          // total hops (3-5)
    static int16_t attackHopOriginX;        // X before attack started
    static int16_t attackHopTargets[5];     // pre-computed X targets
    static constexpr uint16_t ATTACK_HOP_MS = 250;      // ms per hop
    static constexpr int16_t ATTACK_HOP_HEIGHT = 10;     // pixels up

    // Walk transition animation
    static bool transitioning;
    static uint32_t transitionStartTime;
    static int transitionFromX;
    static int transitionToX;
    static bool transitionToFacingRight;
    static int currentX;  // Animated X position
    // Grass animation state
    static bool grassMoving;
    static bool grassDirection;  // true = grass scrolls right, false = scrolls left
    static bool pendingGrassStart;  // Wait for transition before starting grass
    static bool onRightSide;  // Track which side of screen pig is on
    static uint32_t lastGrassUpdate;
    static uint16_t grassSpeed;  // ms per shift

    // Grass blades + ground décor (2px stems — readable, not a green smear)
    struct GrassBlade {
        uint8_t height;  // stem length px
        int8_t  lean;    // tip offset: -4 to +4
        uint8_t width;   // base thickness: 1-2
        uint8_t kind;    // 0=blade 1=tuft 2=flower 3=pebble 4=short stubble
        uint8_t shade;   // color variant 0-3
    };
    static constexpr uint8_t GRASS_BLADE_COUNT = 80;
    static constexpr int16_t GRASS_STRIDE = 3;  // dense enough to read as turf
    static GrassBlade grassBlades[GRASS_BLADE_COUNT];
    static int16_t grassOffset;  // smooth scroll pixel offset

    // Fruit tree state
    struct TreeFruit {
        int16_t offsetX;    // X offset from canopy center
        int16_t offsetY;    // Y offset from base (negative = up)
        uint8_t radius;     // 2-4px
        uint8_t bobPhase;   // Per-fruit bob phase (0-255)
    };

    struct TreeBranch {
        int16_t x1, y1;     // start relative to baseX, baseY
        int16_t x2, y2;     // end relative to baseX, baseY
        uint8_t thickness;  // 1-2px (tapers from parent)
    };

    struct TreeLeafCluster {
        int16_t cx, cy;     // center relative to baseX, baseY
        uint8_t radius;     // 4-7px
    };

    struct TreeTrunk {
        int16_t baseX;         // ground position
        uint8_t trunkHeight;   // 67-81px
        uint8_t trunkWidth;    // half-width 2-3px
        int8_t  trunkLean;     // -3..+3
        uint8_t crownRadius;   // 10-16px canopy circle
    };

    static constexpr uint8_t MAX_BRANCHES = 18;
    static constexpr uint8_t MAX_LEAF_CLUSTERS = 16;
    static constexpr uint8_t MAX_TREE_FRUITS = 8;
    static constexpr uint16_t TREE_GROW_MS = 2200;       // slow grow (was 600)
    static constexpr uint16_t TREE_COLLAPSE_MS = 1100;   // slow fall (was 350)
    static constexpr uint16_t TREE_MIN_ALIVE_MS = 7000;  // stay longer (was 4000)

    static TreePhase treePhase;
    static float treeGrowth;
    static uint32_t treeAnimStart;
    static TreeTrunk treeTrunk;
    static TreeBranch treeBranches[MAX_BRANCHES];
    static uint8_t treeBranchCount;
    static TreeLeafCluster treeLeaves[MAX_LEAF_CLUSTERS];
    static uint8_t treeLeafCount;
    static uint8_t treeEndpointLeafCount;
    static TreeFruit treeFruits[MAX_TREE_FRUITS];
    static uint8_t treeFruitCount;
    static uint32_t treeSeed;
    static bool treePendingHide;
    static bool treePendingShow;
    static uint8_t treePendingFruits;
    static uint32_t treeAliveStart;
    static int16_t treeScrollOffset;  // cumulative grass scroll applied to tree X

    static void generateTree(uint8_t fruitCount);
    static void updateTree();
    // yOffset: TOP_BAR_H when drawing into top bar so crown bleeds under text
    // doUpdate: false when bar-only redraw (main scene already updated)
    static void drawTree(M5Canvas& canvas, int16_t yOffset = 0, bool doUpdate = true);

    static WaveMode waveMode;
    static float micDanceLevel;  // 0..1 MicPork energy
    static uint32_t waveBurstStart;
    static uint32_t waveBurstEnd;
    static void drawWaveRipples(M5Canvas& canvas, bool faceRight, int startX, int startY);

    // Pure pixel-art mouse (no ASCII glyphs). All anim flags applied at draw time.
    static void drawFrame(M5Canvas& canvas, bool blink, bool faceRight, bool sniff);
    // frontLayer=false: turf + blades behind pig; true: tall blades in front of pig feet
    static void drawGrass(M5Canvas& canvas, bool frontLayer = false);
    static void updateGrass();

    // Ear twitch micro-animation (random idle twitch)
    static bool earTwitchActive;
    static uint32_t earTwitchStart;
    static uint32_t nextEarTwitch;
    static constexpr uint16_t EAR_TWITCH_DURATION_MS = 80;

    // Perk up animation (ears pop + quick bounce on new network)
    static bool perkUpActive;
    static uint32_t perkUpStart;
    static constexpr uint16_t PERK_UP_DURATION_MS = 200;
    static constexpr int PERK_UP_HEIGHT = 3;

    // Flinch animation (duck down + jitter on error)
    static bool flinchActive;
    static uint32_t flinchStart;
    static constexpr uint16_t FLINCH_DURATION_MS = 300;

    // Spin animation (rapid direction flips for level-up)
    static bool spinActive;
    static uint32_t spinStart;
    static constexpr uint16_t SPIN_DURATION_MS = 600;
    static constexpr uint8_t SPIN_FLIPS = 4;

    // Paw scratch animation (X oscillation when bored)
    static bool pawScratchActive;
    static uint32_t pawScratchStart;
    static constexpr uint16_t PAW_SCRATCH_DURATION_MS = 800;

    // Tail wiggle burst animation (celebrations)
    static bool tailWiggleActive;
    static uint32_t tailWiggleStart;
    static constexpr uint16_t TAIL_WIGGLE_DURATION_MS = 800;

    // Sparkle particle system (achievements, level-up)
    struct SparkleParticle {
        int16_t x, y;
        int8_t vx, vy;
        uint8_t life;       // frames remaining (0 = inactive)
    };
    static constexpr uint8_t MAX_SPARKLES = 12;
    static SparkleParticle sparkles[MAX_SPARKLES];
    static bool sparkleStorm;
    static uint32_t sparkleStormLastMs;
    static void updateAndDrawSparkles(M5Canvas& canvas);
    static void seedSparkleStorm();
};
