-- Doom Bench - measures box columns against line columns on real hardware.
--
-- Installs alongside htn_doom under its own slug; it does not touch that app.
-- The camera spins on its own and every column is rewritten every frame, so
-- both modes do identical work and the numbers are comparable. It alternates
-- modes every 3 seconds and keeps both results on screen.
--
-- A toggles auto-spin, START clears the results, HOME exits.
--
-- Two numbers per mode:
--   ms   Lua+binding time inside the render loop (badge.sys.ms around it)
--   fps  ticks actually delivered per second -- this one includes LVGL's own
--        redraw cost, which happens after the callback returns and which the
--        ms figure cannot see. fps is the number that matters.

local COLS, COLW = 40, 8
local VIEWH, CY = 176, 88
local PERIOD = 3000

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
"#..............#",
"################",
}

local root_ref, loading
local px, py, pa = 8.5, 1.5, 0.0
local boxes, lines, pts, colc = {}, {}, {}, {}
local PAL = {}
local built, stage = 0, 0
local mode = 0
local spin = true
local phase_end, frames, acc = 0, 0, 0
local res_ms, res_fps, res_n = {0, 0}, {0, 0}, {0, 0}
local lbox, lline, lstat
-- One reusable style table: allocating a fresh one per call would feed the GC
-- 40 times a frame, which is the whole point of comparing the two paths.
local STY = {line_color = 0}

local function tb(x, y)
  if x < 0 or y < 0 or x > 15 or y > 15 then return 35 end
  return string.byte(MAP[y + 1], x + 1)
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
    local h = math.floor(VIEWH / dist)
    if h > VIEWH then h = VIEWH end
    if h < 2 then h = 2 end
    local y = CY - math.floor(h / 2)
    local l = 7 - math.floor(dist)
    if l < 0 then l = 0 end
    if l > 7 then l = 7 end
    local c = PAL[side * 8 + l + 1]
    if mode == 0 then
      local w = boxes[i]
      w:set_size(COLW, h)
      w:set_pos((i - 1) * COLW, y)
      if colc[i] ~= c then w:set_color(c) colc[i] = c end
    else
      local p = pts[i]
      p[1][2] = y
      p[2][2] = y + h
      local w = lines[i]
      w:set_points(p)
      if colc[i] ~= c then STY.line_color = c w:style(STY) colc[i] = c end
    end
  end
end

local function swap_mode()
  local to_box = (mode == 1)
  for i = 1, COLS do
    boxes[i]:hidden(not to_box)
    lines[i]:hidden(to_box)
    colc[i] = -1
  end
  mode = to_box and 0 or 1
end

local function build_step()
  if built < COLS then
    local n = 0
    while built < COLS and n < 8 do
      built = built + 1
      n = n + 1
      local i = built
      local b = badge.ui.box(root_ref, COLW, 8)
      b:style({bg_color = 0x000000, border_width = 0, radius = 0})
      b:set_pos((i - 1) * COLW, CY)
      boxes[i] = b
      local cx = (i - 1) * COLW + COLW // 2
      local p = {{cx, CY}, {cx, CY + 8}}
      local ln = badge.ui.line(root_ref, p)
      ln:style({line_color = 0x000000, line_width = COLW})
      ln:hidden(true)
      lines[i] = ln
      pts[i] = p
      colc[i] = -1
    end
    return
  end
  if stage == 0 then
    lbox = badge.ui.label(root_ref, "BOX   --")
    lbox:set_pos(8, 182)
    lbox:style({text_font = 20, text_color = 0xf0c24a})
    lline = badge.ui.label(root_ref, "LINE  --")
    lline:set_pos(8, 206)
    lline:style({text_font = 20, text_color = 0x9fd66b})
    lstat = badge.ui.label(root_ref, "warming up")
    lstat:set_pos(176, 182)
    lstat:style({text_font = 14, text_color = 0xbfc4cf})
    loading:delete()
    loading = nil
    stage = 1
    phase_end = badge.sys.ms() + PERIOD
    return
  end
end

local function finish_phase(now)
  local m = mode + 1
  local ms = frames > 0 and (acc * 100) // frames or 0
  local fps = (frames * 1000) // PERIOD
  -- Keep a running mean so a glance at the screen is stable, not jittery.
  res_n[m] = res_n[m] + 1
  res_ms[m] = res_ms[m] + (ms - res_ms[m]) // res_n[m]
  res_fps[m] = res_fps[m] + (fps - res_fps[m]) // res_n[m]
  lbox:set_text(string.format("BOX   %d.%02d ms  %d fps",
    res_ms[1] // 100, res_ms[1] % 100, res_fps[1]))
  lline:set_text(string.format("LINE  %d.%02d ms  %d fps",
    res_ms[2] // 100, res_ms[2] % 100, res_fps[2]))
  lstat:set_text(string.format("%d runs each\n%d cols, forced\nA:spin START:clr",
    res_n[1], COLS))
  frames, acc = 0, 0
  swap_mode()
  phase_end = now + PERIOD
end

function on_enter(root)
  root_ref = root
  for s = 0, 1 do
    for l = 0, 7 do
      local f = (s == 0 and 1.0 or 0.6) * (0.16 + 0.84 * l / 7)
      PAL[s * 8 + l + 1] = math.floor(176 * f) * 65536
                         + math.floor(58 * f) * 256
                         + math.floor(44 * f)
    end
  end
  local sky = badge.ui.box(root, 320, CY)
  sky:set_pos(0, 0)
  sky:style({bg_color = 0x10141c, border_width = 0, radius = 0})
  local flr = badge.ui.box(root, 320, VIEWH - CY)
  flr:set_pos(0, CY)
  flr:style({bg_color = 0x241d18, border_width = 0, radius = 0})
  local hb = badge.ui.box(root, 320, 64)
  hb:set_pos(0, 176)
  hb:style({bg_color = 0x14161c, border_width = 0, radius = 0})
  loading = badge.ui.label(root, "Building bench...")
  loading:align("center", 0, 0)
end

function on_tick()
  if stage == 0 then build_step() return end
  local now = badge.sys.ms()
  if spin then pa = pa + 0.035 end
  local t0 = badge.sys.ms()
  render()
  acc = acc + (badge.sys.ms() - t0)
  frames = frames + 1
  if now >= phase_end then finish_phase(now) end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  if stage == 0 then return end
  local B = badge.input.BUTTON
  if button == B.A then
    spin = not spin
  elseif button == B.START then
    res_ms = {0, 0}
    res_fps = {0, 0}
    res_n = {0, 0}
    frames, acc = 0, 0
    phase_end = badge.sys.ms() + PERIOD
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
