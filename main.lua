-- Badge Doom - a raycaster drawn with 40 column widgets.
-- Up/Down walk, Left/Right turn, hold B to strafe,
-- A fires, Start restarts, HOME exits and saves your best.

local COLS, COLW = 40, 8
local VIEWH, CY = 176, 88
local FRAME = 55

local MAP = {
"################",
"#....#.........#",
"#....#...####..#",
"#....#...#..#..#",
"#........#..#..#",
"####.#####..#..#",
"#..............#",
"#..####..####..#",
"#..#........#..#",
"#..#..#..#..#..#",
"#.....#..#.....#",
"#######..#######",
"#..............#",
"#..##########..#",
"#X.............#",
"################",
}

local root_ref
local px, py, pa = 8.5, 1.5, math.pi / 2
local hp, ammo, kills, best = 100, 40, 0, 0
local state, built, stage = "build", 0, 1
local last_ms, atk_ms, flash_ms, hurt_ms = 0, 0, 0, 0
local msg_ms, frame_ms, led_ms = 0, 0, 0
local flash_on, best_dirty = false, false
local vhp, vam, vk = -1, -1, -1

local sky, flr, gun, flash, cross, hudbg
local lhp, lam, lk, lmsg, loading
local cols, colh, colv, colc, zbuf = {}, {}, {}, {}, {}
local sprites, foes, PAL = {}, {}, {}
local LEFT, RIGHT = {1, 6, 5}, {2, 3, 4}

local function tb(x, y)
  if x < 0 or y < 0 or x > 15 or y > 15 then return 35 end
  return string.byte(MAP[y + 1], x + 1)
end

local function set_msg(t, hold)
  if lmsg then
    lmsg:set_text(t)
    msg_ms = badge.sys.ms() + (hold or 1800)
  end
end

local function reset_game()
  px, py, pa = 8.5, 1.5, math.pi / 2
  hp, ammo, kills = 100, 40, 0
  local d = {3.5, 7.5, 8.5, 10.5, 13.5, 13.5}
  for i = 1, 3 do
    local f = foes[i]
    if not f then f = {}; foes[i] = f end
    f.x, f.y = d[i * 2 - 1], d[i * 2]
    f.hp, f.alive = 2, true
  end
  state = "play"
  atk_ms = 0
end

local function try_move(nx, ny)
  local m = 0.22
  local ox = nx >= px and m or -m
  local oy = ny >= py and m or -m
  if tb(math.floor(nx + ox), math.floor(py)) ~= 35 then px = nx end
  if tb(math.floor(px), math.floor(ny + oy)) ~= 35 then py = ny end
end

local function leds(now)
  badge.led.clear()
  if state == "win" then
    badge.led.set(math.floor(now / 120) % 6 + 1, 0, 200, 60)
  elseif state == "dead" then
    local p = (now % 1600) / 800
    if p > 1 then p = 2 - p end
    local v = math.floor(30 + 170 * p)
    badge.led.set_all(v, 0, 0)
  else
    local seg = math.ceil(hp / 34)
    for i = 1, 3 do
      if i <= seg then
        if hp > 60 then badge.led.set(LEFT[i], 0, 170, 40)
        elseif hp > 30 then badge.led.set(LEFT[i], 200, 110, 0)
        else badge.led.set(LEFT[i], 200, 0, 0) end
      end
    end
    local a = math.ceil(ammo / 14)
    if a > 3 then a = 3 end
    for i = 1, 3 do
      if i <= a then badge.led.set(RIGHT[i], 150, 85, 0) end
    end
    if now < flash_ms then badge.led.set_all(255, 240, 190) end
    if now < hurt_ms then badge.led.set_all(220, 0, 0) end
  end
  badge.led.show()
end

local function render()
  local dx, dy = math.cos(pa), math.sin(pa)
  local plx, ply = -dy * 0.66, dx * 0.66
  for i = 1, COLS do
    local cam = 2 * (i - 0.5) / COLS - 1
    local rx, ry = dx + plx * cam, dy + ply * cam
    local mx, my = math.floor(px), math.floor(py)
    local ddx = rx == 0 and 1e9 or math.abs(1 / rx)
    local ddy = ry == 0 and 1e9 or math.abs(1 / ry)
    local sx, sy, stx, sty
    if rx < 0 then stx = -1; sx = (px - mx) * ddx
    else stx = 1; sx = (mx + 1 - px) * ddx end
    if ry < 0 then sty = -1; sy = (py - my) * ddy
    else sty = 1; sy = (my + 1 - py) * ddy end
    local side, dist = 0, 20
    for _ = 1, 40 do
      if sx < sy then mx = mx + stx; dist = sx; sx = sx + ddx; side = 0
      else my = my + sty; dist = sy; sy = sy + ddy; side = 1 end
      if tb(mx, my) == 35 then break end
      if mx < 0 or my < 0 or mx > 15 or my > 15 then break end
    end
    if dist < 0.05 then dist = 0.05 end
    zbuf[i] = dist
    local h = math.floor(VIEWH / dist)
    if h > VIEWH then h = VIEWH end
    if h < 2 then h = 2 end
    local y = CY - math.floor(h / 2)
    local l = 7 - math.floor(dist)
    if l < 0 then l = 0 end
    if l > 7 then l = 7 end
    local c = PAL[side * 8 + l + 1]
    local w = cols[i]
    if colh[i] ~= h or colv[i] ~= y then
      w:set_size(COLW, h)
      w:set_pos((i - 1) * COLW, y)
      colh[i] = h
      colv[i] = y
    end
    if colc[i] ~= c then
      w:set_color(c)
      colc[i] = c
    end
  end
  local det = plx * dy - dx * ply
  local inv = det == 0 and 1e9 or 1 / det
  for k = 1, 3 do
    local f, sp = foes[k], sprites[k]
    local shown = false
    if f.alive then
      local ex, ey = f.x - px, f.y - py
      local tx = inv * (dy * ex - dx * ey)
      local ty = inv * (-ply * ex + plx * ey)
      if ty > 0.35 then
        local scx = math.floor(160 * (1 + tx / ty))
        local sh = math.floor(VIEWH / ty * 0.75)
        if sh > VIEWH then sh = VIEWH end
        if sh < 4 then sh = 4 end
        local sw = math.floor(sh * 0.55)
        local ci = math.floor(scx / COLW) + 1
        if ci >= 1 and ci <= COLS and zbuf[ci] > ty
           and scx > -sw and scx < 320 + sw then
          local base = math.floor(CY + VIEWH / (2 * ty))
          sp:set_size(sw, sh)
          sp:set_pos(scx - math.floor(sw / 2), base - sh)
          sp:hidden(false)
          shown = true
        end
      end
    end
    if not shown then sp:hidden(true) end
  end
end

local function fire()
  local now = badge.sys.ms()
  if ammo <= 0 then
    set_msg("Out of ammo")
    return
  end
  ammo = ammo - 1
  flash_ms = now + 90
  local wall = zbuf[math.floor(COLS / 2)] or 12
  local dx, dy = math.cos(pa), math.sin(pa)
  local bi, bd = 0, 99
  for k = 1, 3 do
    local f = foes[k]
    if f.alive then
      local ex, ey = f.x - px, f.y - py
      local d = math.sqrt(ex * ex + ey * ey)
      if d > 0.1 and d < wall + 0.6 and d < bd
         and (ex * dx + ey * dy) / d > 0.985 then
        bi, bd = k, d
      end
    end
  end
  if bi > 0 then
    local f = foes[bi]
    f.hp = f.hp - 1
    if f.hp <= 0 then
      f.alive = false
      kills = kills + 1
      if kills > best then best = kills; best_dirty = true end
      set_msg("Demon down")
    else
      set_msg("Hit")
    end
  end
end

local function foes_tick(now, dt)
  for k = 1, 3 do
    local f = foes[k]
    if f.alive then
      local ex, ey = px - f.x, py - f.y
      local d = math.sqrt(ex * ex + ey * ey)
      if d < 9 and d > 0.75 then
        local sp = 1.1 * dt
        local nx, ny = f.x + ex / d * sp, f.y + ey / d * sp
        if tb(math.floor(nx), math.floor(f.y)) ~= 35 then f.x = nx end
        if tb(math.floor(f.x), math.floor(ny)) ~= 35 then f.y = ny end
      elseif d <= 0.75 and now > atk_ms then
        atk_ms = now + 800
        hp = hp - 9
        hurt_ms = now + 220
        if hp <= 0 then
          hp = 0
          state = "dead"
          set_msg("You died. Best " .. best .. ". Start to retry", 60000)
        end
      end
    end
  end
end

local function hud(now)
  if hp ~= vhp then lhp:set_text("HP " .. hp); vhp = hp end
  if ammo ~= vam then lam:set_text("AMMO " .. ammo); vam = ammo end
  if kills ~= vk then lk:set_text("KILLS " .. kills); vk = kills end
  if msg_ms > 0 and now > msg_ms then lmsg:set_text(""); msg_ms = 0 end
  local fl = now < flash_ms
  if fl ~= flash_on then flash:hidden(not fl); flash_on = fl end
end

local function build_step()
  if built < COLS then
    local n = 0
    while built < COLS and n < 10 do
      built = built + 1
      n = n + 1
      local b = badge.ui.box(root_ref, COLW, 8)
      b:style({bg_color = 0x000000, border_width = 0, radius = 0})
      b:set_pos((built - 1) * COLW, CY)
      cols[built] = b
      colh[built] = 8
      colv[built] = CY
      colc[built] = 0
      zbuf[built] = 20
    end
    return
  end
  if stage == 1 then
    for k = 1, 3 do
      local s = badge.ui.box(root_ref, 10, 10)
      s:style({bg_color = 0x8a3020, border_width = 2,
               border_color = 0x140805, radius = 3})
      s:hidden(true)
      sprites[k] = s
    end
    stage = 2
    return
  end
  if stage == 2 then
    gun = badge.ui.box(root_ref, 72, 34)
    gun:set_pos(124, 142)
    gun:style({bg_color = 0x3b3f4a, border_width = 2,
               border_color = 0x181b22, radius = 4})
    flash = badge.ui.box(root_ref, 22, 16)
    flash:set_pos(149, 128)
    flash:style({bg_color = 0xffe08a, border_width = 0, radius = 8})
    flash:hidden(true)
    cross = badge.ui.box(root_ref, 4, 4)
    cross:set_pos(158, 86)
    cross:style({bg_color = 0xd8d8d8, border_width = 0, radius = 2})
    stage = 3
    return
  end
  hudbg = badge.ui.box(root_ref, 320, 64)
  hudbg:set_pos(0, 176)
  hudbg:style({bg_color = 0x14161c, border_width = 0, radius = 0})
  lhp = badge.ui.label(root_ref, "HP 100")
  lhp:set_pos(10, 184)
  lhp:style({text_font = 20, text_color = 0xff6b5a})
  lam = badge.ui.label(root_ref, "AMMO 40")
  lam:set_pos(112, 184)
  lam:style({text_font = 20, text_color = 0xf0c24a})
  lk = badge.ui.label(root_ref, "KILLS 0")
  lk:set_pos(226, 184)
  lk:style({text_font = 20, text_color = 0x9fd66b})
  lmsg = badge.ui.label(root_ref, "")
  lmsg:set_pos(10, 214)
  lmsg:style({text_font = 14, text_color = 0xbfc4cf})
  loading:delete()
  loading = nil
  reset_game()
  set_msg("Find the exit. Best " .. best, 2500)
end

function on_enter(root)
  root_ref = root
  best = badge.store.get_int("best_kills", 0)
  for s = 0, 1 do
    for l = 0, 7 do
      local f = (s == 0 and 1.0 or 0.6) * (0.16 + 0.84 * l / 7)
      PAL[s * 8 + l + 1] = math.floor(176 * f) * 65536
                         + math.floor(58 * f) * 256
                         + math.floor(44 * f)
    end
  end
  sky = badge.ui.box(root, 320, CY)
  sky:set_pos(0, 0)
  sky:style({bg_color = 0x10141c, border_width = 0, radius = 0})
  flr = badge.ui.box(root, 320, VIEWH - CY)
  flr:set_pos(0, CY)
  flr:style({bg_color = 0x241d18, border_width = 0, radius = 0})
  loading = badge.ui.label(root, "Loading...")
  loading:align("center", 0, 0)
  last_ms = badge.sys.ms()
end

function on_tick()
  local now = badge.sys.ms()
  if state == "build" then
    build_step()
    last_ms = now
    return
  end
  local dt = (now - last_ms) / 1000
  last_ms = now
  if dt > 0.12 then dt = 0.12 end
  if state == "play" then
    local B = badge.input.BUTTON
    local dx, dy = math.cos(pa), math.sin(pa)
    local mv = 2.4 * dt
    local nx, ny = px, py
    if badge.input.is_down(B.UP) then nx = nx + dx * mv; ny = ny + dy * mv end
    if badge.input.is_down(B.DOWN) then nx = nx - dx * mv; ny = ny - dy * mv end
    if badge.input.is_down(B.B) then
      if badge.input.is_down(B.LEFT) then nx = nx + dy * mv; ny = ny - dx * mv end
      if badge.input.is_down(B.RIGHT) then nx = nx - dy * mv; ny = ny + dx * mv end
    else
      if badge.input.is_down(B.LEFT) then pa = pa - 2.4 * dt end
      if badge.input.is_down(B.RIGHT) then pa = pa + 2.4 * dt end
    end
    if nx ~= px or ny ~= py then try_move(nx, ny) end
    foes_tick(now, dt)
    if tb(math.floor(px), math.floor(py)) == 88 then
      state = "win"
      set_msg("EXIT FOUND. Kills " .. kills .. ". Start to replay", 60000)
    end
  end
  if now >= frame_ms then
    frame_ms = now + FRAME
    render()
    hud(now)
  end
  if now >= led_ms then
    led_ms = now + 60
    leds(now)
  end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if state == "build" then return end
  if button == B.START then
    reset_game()
    set_msg("Restarted")
  elseif button == B.A then
    if state == "play" then
      fire()
    else
      reset_game()
      set_msg("Go")
    end
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
  if best_dirty then
    badge.store.set_int("best_kills", best)
    best_dirty = false
  end
end

