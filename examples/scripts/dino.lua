-- dino.lua — Dino Jump.
-- Two small per-element sprites (UniGeek-style):
--   * dino lane: fixed-X strip covering the full jump arc, atomic dino push
--   * cactus lane: travelling sprite that includes a black erase-trail so
--     each push covers both the new position and where the cactus just was
-- Where the cactus crosses the dino lane, the dino sprite composes the
-- overlapping cactus slice itself, so the cactus stays visible behind the
-- dino. Both lanes degrade to overdraw if their sprites can't allocate.

local lcd = require("uni.lcd")
local sd  = require("uni.sd")
local nav = require("uni.nav")

local W, H = lcd.w(), lcd.h()
local GY         = H - 18
local DX, DW, DH = 22, 13, 17
local OW         = 11
local GRAVITY    = 0.75
local JUMP_FORCE = -9.5

local C_WHITE  = lcd.color(255, 255, 255)
local C_GREEN  = lcd.color( 60, 210,  80)
local C_DARK   = lcd.color(  0, 100,  30)
local C_ORANGE = lcd.color(255, 140,   0)
local C_RED    = lcd.color(255,  60,  60)
local C_YELLOW = lcd.color(255, 220,   0)
local C_GREY   = lcd.color( 70,  70,  70)
local C_BLACK  = lcd.color(  0,   0,   0)

-- Dino lane — fixed x band; tall enough for the full jump arc. With
-- JUMP_FORCE=-9.5 / GRAVITY=0.75 the peak rise is ~65 px above ground rest;
-- add room for the head (4 px above body top) plus a margin.
local LANE_X = DX - 2
local LANE_W = DW + 8
local LANE_Y = GY - DH - 80
local LANE_H = (GY + 5) - LANE_Y
local GROUND_Y_IN_LANE = (GY + 1) - LANE_Y

local dino_sp = lcd.sprite(LANE_W, LANE_H)
if not dino_sp then uni.debug("dino: lane sprite OOM") end

-- Cactus lane — travels with the cactus. Width must cover one frame's worth
-- of leftward motion (≈speed) on top of the cactus's own footprint, so each
-- push paints the new cactus and erases where it just was in one shot.
local CACTUS_LANE_W   = 32
local CACTUS_LANE_H   = 42
local CACTUS_LANE_TOP = GY - CACTUS_LANE_H

local cactus_sp = lcd.sprite(CACTUS_LANE_W, CACTUS_LANE_H)
if not cactus_sp then uni.debug("cactus: lane sprite OOM") end

-- Pre-alloc drawRect closures — avoids creating new closure objects every frame,
-- which fragments internal SRAM and eventually causes Lua OOM after extended play.
local _dinoSpDraw   = dino_sp   and function(x,y,w,h,c) dino_sp:rect(x,y,w,h,c)   end or nil
local _cactusSpDraw = cactus_sp and function(x,y,w,h,c) cactus_sp:rect(x,y,w,h,c) end or nil

local hiScore = 0
local raw = sd.read("/unigeek/games/lua_dino.txt")
if raw and #raw > 0 then hiScore = math.floor(tonumber(raw) or 0) end

math.randomseed(math.floor(uni.millis()))

local gstate = "idle"
local score, dinoY, jumpV, onGnd
local obsX, obsH, speed, tick, leg, newHi
local prevDY, prevOX, prevOH

local function _reset()
  score  = 0
  dinoY  = GY - DH
  jumpV  = 0
  onGnd  = true
  obsX   = W + 40
  obsH   = 18 + math.random(0, 18)
  speed  = 2.5
  tick   = 0
  newHi  = false
  leg    = 0
  prevDY = math.floor(GY - DH)
  prevOX = math.floor(W + 40)
  prevOH = math.floor(obsH)
end

-- Compose dino into a generic surface. drawRect(x,y,w,h,c) is lcd.rect or a
-- sprite-rect closure; (ox, oy) is where the body's top-left lands.
local function _composeDino(drawRect, ox, oy, running)
  drawRect(ox,          oy,     DW, DH, C_GREEN)
  drawRect(ox + DW - 4, oy - 4,  7,  6, C_GREEN)
  drawRect(ox + DW + 1, oy - 3,  2,  2, C_BLACK)
  drawRect(ox + DW - 4, oy + 2,  4,  2, C_DARK)
  if not running or math.floor(leg / 4) % 2 == 0 then
    drawRect(ox + 2, oy + DH,     4, 4, C_GREEN)
    drawRect(ox + 8, oy + DH + 1, 4, 3, C_GREEN)
  else
    drawRect(ox + 2, oy + DH + 1, 4, 3, C_GREEN)
    drawRect(ox + 8, oy + DH,     4, 4, C_GREEN)
  end
end

-- Compose cactus into a generic surface. The (origin_x, origin_y) is the
-- screen position of the surface's (0, 0) — pass (0, 0) for direct lcd
-- drawing, or (sprite_origin_x, sprite_origin_y) for a sprite.
local function _composeCactus(drawRect, ox, oh, origin_x, origin_y)
  local ix = math.floor(ox) - origin_x
  local iy = math.floor(oh)
  local sy = (GY - iy) - origin_y
  drawRect(ix, sy, OW, iy, C_ORANGE)
  if iy > 14 then
    drawRect(ix - 4, sy + 5, 4, 5, C_ORANGE)
    drawRect(ix - 4, sy + 5, 5, 3, C_ORANGE)
  end
  if iy > 10 then
    drawRect(ix + OW,     sy + 8, 4, 4, C_ORANGE)
    drawRect(ix + OW - 1, sy + 8, 5, 3, C_ORANGE)
  end
end

local function _drawDinoLcd(dy, running)
  _composeDino(lcd.rect, DX, math.floor(dy), running)
end

local function _drawCactusLcd(ox, oh)
  _composeCactus(lcd.rect, ox, oh, 0, 0)
end

local function _drawDinoSprite(dy, running, cactus_x, cactus_h)
  dino_sp:fill(C_BLACK)
  dino_sp:rect(0, GROUND_Y_IN_LANE, LANE_W, 2, C_GREY)

  -- Compose any cactus pixels that fall inside the lane so the cactus stays
  -- visible behind the dino. Sprite drawing clips to bounds, so calling
  -- _composeCactus when the cactus is far away is harmless.
  local cleft  = math.floor(cactus_x) - 4
  local cright = math.floor(cactus_x) + OW + 4
  if cright >= LANE_X and cleft < LANE_X + LANE_W then
    _composeCactus(_dinoSpDraw, cactus_x, cactus_h, LANE_X, LANE_Y)
  end

  _composeDino(_dinoSpDraw, DX - LANE_X, math.floor(dy) - LANE_Y, running)
  dino_sp:push(LANE_X, LANE_Y)
end

local function _drawCactusSprite(ox, oh)
  -- Sprite anchored at the cactus's leftmost pixel (left arm extends -4).
  -- The sprite's right edge intentionally extends past the cactus's right
  -- arm so the trailing area (left blank by fill) erases the previous
  -- frame's cactus position in one push.
  local origin_x = math.floor(ox) - 4
  cactus_sp:fill(C_BLACK)
  _composeCactus(_cactusSpDraw, ox, oh, origin_x, CACTUS_LANE_TOP)
  cactus_sp:push(origin_x, CACTUS_LANE_TOP)
end

local function _collision()
  local dy  = math.floor(dinoY)
  local ox  = math.floor(obsX)
  local oh  = math.floor(obsH)
  local TOL = 3
  return (DX + DW - TOL > ox + TOL) and
         (DX + TOL < ox + OW - TOL) and
         (dy + DH - TOL > GY - oh + TOL)
end

-- ── Main loop ─────────────────────────────────────────────────────────

local prevState = ""

while true do
  local btn     = nav.btn()
  if btn == "back" then break end
  local entered = (gstate ~= prevState)
  prevState = gstate

  -- ── IDLE ──────────────────────────────────────────────────────────
  if gstate == "idle" then
    if entered then
      lcd.clear()
      lcd.rect(0, GY + 1, W, 2, C_GREY)
      lcd.textSize(2)
      lcd.textColor(C_WHITE)
      lcd.print(math.floor(W/2) - 52, math.floor(H/2) - 28, "DINO JUMP")
      lcd.textSize(1)
      lcd.textColor(C_GREY)
      lcd.print(math.floor(W/2) - 30, math.floor(H/2) - 4, "OK / UP to start")
      if hiScore > 0 then
        lcd.textColor(C_YELLOW)
        lcd.print(math.floor(W/2) - 28, math.floor(H/2) + 10, "Best: " .. hiScore)
      end
      _drawDinoLcd(GY - DH, false)
    end

    if btn == "ok" or btn == "up" then
      _reset()
      lcd.clear()
      lcd.rect(0, GY + 1, W, 2, C_GREY)
      gstate = "play"
    end

  -- ── GAME OVER ─────────────────────────────────────────────────────
  elseif gstate == "over" then
    if entered then
      lcd.clear()
      lcd.rect(0, GY + 1, W, 2, C_GREY)
      lcd.textSize(2)
      lcd.textColor(C_RED)
      lcd.print(math.floor(W/2) - 52, math.floor(H/2) - 26, "GAME OVER")
      lcd.textSize(1)
      lcd.textColor(C_WHITE)
      lcd.print(math.floor(W/2) - 26, math.floor(H/2) - 4, "Score: " .. score)
      if newHi then
        lcd.textColor(C_YELLOW)
        lcd.print(math.floor(W/2) - 28, math.floor(H/2) + 8, "NEW BEST!")
      else
        lcd.textColor(C_GREY)
        lcd.print(math.floor(W/2) - 28, math.floor(H/2) + 8, "Best:  " .. hiScore)
      end
      lcd.textColor(C_GREY)
      lcd.print(math.floor(W/2) - 32, math.floor(H/2) + 22, "OK: retry")
      _drawDinoLcd(GY - DH, false)
    end

    if btn == "ok" or btn == "up" then
      _reset()
      lcd.clear()
      lcd.rect(0, GY + 1, W, 2, C_GREY)
      gstate = "play"
    end

  -- ── PLAY ──────────────────────────────────────────────────────────
  else
    tick = tick + 1
    leg  = leg + 1

    local pox = prevOX
    local poh = prevOH

    if (btn == "ok" or btn == "up") and onGnd then
      jumpV = JUMP_FORCE
      onGnd = false
      uni.beep(1200, 20)
    end

    dinoY = dinoY + jumpV
    jumpV = jumpV + GRAVITY
    if dinoY >= GY - DH then
      dinoY = GY - DH
      jumpV = 0
      onGnd = true
    end

    obsX = obsX - speed
    if obsX < -OW - 10 then
      obsX  = W + math.random(30, 70)
      obsH  = 16 + math.random(0, 22)
      speed = math.min(speed + 0.04, 10.0)
    end

    if tick % 8 == 0 then score = score + 1 end
    if score > 0 and score % 50 == 0 and tick % 8 == 0 then
      uni.beep(660, 30)
    end

    -- Ground line outside the dino lane (the lane redraws its own slice).
    lcd.rect(0, GY + 1, LANE_X, 2, C_GREY)
    lcd.rect(LANE_X + LANE_W, GY + 1, W - (LANE_X + LANE_W), 2, C_GREY)

    -- Cactus first — its sprite both erases the prior frame and paints the
    -- new cactus. Push order matters: dino sprite goes last so it owns the
    -- final pixels in the lane area and the cactus slice is composed inside.
    if cactus_sp then
      _drawCactusSprite(obsX, obsH)
    else
      if pox < W + 5 then
        lcd.rect(pox - 5, GY - poh - 2, OW + 11, poh + 5, C_BLACK)
      end
      _drawCactusLcd(obsX, obsH)
    end

    if dino_sp then
      _drawDinoSprite(dinoY, onGnd, obsX, obsH)
    else
      lcd.rect(DX - 1, prevDY - 5, DW + 6, DH + 11, C_BLACK)
      _drawDinoLcd(dinoY, onGnd)
    end

    -- HUD: bg fills behind glyphs — no erase rect needed.
    lcd.textSize(1)
    lcd.textColor(C_WHITE, C_BLACK)
    lcd.print(0, 0, string.format("Score:%-5d", score))
    if hiScore > 0 then
      lcd.textColor(C_GREY, C_BLACK)
      lcd.print(W - 54, 0, string.format("Best:%-4d", hiScore))
    end
    lcd.textColor(C_WHITE)

    prevDY = math.floor(dinoY)
    prevOX = math.floor(obsX)
    prevOH = math.floor(obsH)

    if _collision() then
      uni.beep(150, 120)
      if score > hiScore then
        hiScore = score
        newHi   = true
        sd.write("/unigeek/games/lua_dino.txt", tostring(hiScore))
        uni.debug("new high score: " .. hiScore)
      end
      gstate = "over"
    end
  end

  uni.delay(16)
end

if dino_sp   then dino_sp:free()   end
if cactus_sp then cactus_sp:free() end
