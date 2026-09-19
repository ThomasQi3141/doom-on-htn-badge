-- sim.lua -- drive a badge app: on_enter, N ticks of scripted input, on_exit.
--
-- Reports per-tick native call count, live widget count, peak heap and
-- wall-clock ms for each callback.
--
-- IMPORTANT: the millisecond figures are desktop arm64 CPU time. The badge is a
-- 160 MHz ESP32-C3 with NO FPU. Treat ms only as a before/after ratio on this
-- machine. The device-independent numbers are native calls, widget count and
-- heap churn.

local mock    = require("mock_badge")
local sandbox = require("sandbox")

local M = {}

-- Budgets from BADGE_GUIDE.md. Exceeding one on desktop means a runaway, since
-- desktop is far faster than the badge; it is a failure cutoff, not a target.
local BUDGET = {main = 3000, enter = 3000, tick = 250, button = 1000, exit = 1000}
local SLOW_TICK_WARN = 5  -- desktop ms; "target a few ms" per the guide

local function now_ms() return os.clock() * 1000 end
local function heap_bytes() return math.floor(collectgarbage("count") * 1024) end

local function percentile(sorted, p)
  if #sorted == 0 then return 0 end
  local i = math.max(1, math.ceil(#sorted * p))
  return sorted[i]
end

-- Run one callback with timing, heap accounting and a traceback on failure.
local function call(res, inst, phase, fn, ...)
  if not fn then return true end
  inst.phase = phase
  local h0 = heap_bytes()
  local t0 = now_ms()
  local ok, err = xpcall(fn, debug.traceback, ...)
  local dt = now_ms() - t0
  local h1 = heap_bytes()
  if h1 > res.heap_peak then res.heap_peak = h1 end
  if not ok then
    res.ok = false
    res.err = err
    res.err_phase = phase
    return false
  end
  local budget = BUDGET[phase]
  if budget and dt > budget then
    res.ok = false
    res.err = ("%s took %.1f ms on desktop, past the %d ms failure cutoff -- "
               .. "this is a runaway, not a timing artefact"):format(phase, dt, budget)
    res.err_phase = phase
    return false
  end
  return true, dt, h1 - h0
end

function M.run(o)
  o = o or {}
  local path    = o.app or "../main.lua"
  local ticks   = o.ticks or 600
  local tick_ms = o.tick_ms or 20
  -- heap_kb from the manifest. Desktop 64-bit inflates the live set, so this
  -- fails EARLIER than hardware would; that direction is the safe one.
  local heap_cap = (o.heap_kb or 96) * 1024

  local src, rerr = sandbox.read(path)
  if not src then return {ok = false, err = rerr, err_phase = "read"} end

  local badge, inst = mock.build{
    strict_int = o.strict_int ~= false,
    seed = o.seed,
    store = o.store,
  }
  local env, sreport = sandbox.env(badge)

  -- Calibrate the cost of one mock widget table. On the badge, LVGL widgets
  -- live in NATIVE memory and do not touch the Lua quota at all, so the mock's
  -- stand-in tables must be taken back out of every heap figure below.
  local CAL = 200
  collectgarbage("collect")
  local cal0 = heap_bytes()
  local tmp = {}
  for i = 1, CAL do
    tmp[i] = badge.ui.box(inst.root, 1, 1)
    -- Style it like a real one: the _style table is most of a mock widget's cost.
    tmp[i]:style{bg_color = 0, border_width = 0, radius = 0, border_color = 0}
  end
  collectgarbage("collect")
  local per_widget = (heap_bytes() - cal0) / CAL
  for i = 1, CAL do tmp[i]:delete() end
  tmp = nil
  collectgarbage("collect")
  inst.calls_total = 0
  inst.by_name_total = {}
  inst.widgets_created, inst.widgets_deleted, inst.widgets_peak = 0, 0, 0
  inst.style_calls, inst.style_props, inst.led_sets = 0, 0, 0

  -- Pre-allocate the recording arrays so their memory lands in the baseline
  -- instead of masquerading as app heap growth partway through the run.
  local a_calls, a_ms, a_churn, a_widgets = {}, {}, {}, {}
  local samples, nsamp = {}, 0
  for i = 1, ticks do
    a_calls[i] = 0; a_ms[i] = 0; a_churn[i] = 0; a_widgets[i] = 0
  end
  for i = 1, (ticks // 25) * 2 + 4 do samples[i] = 0 end
  collectgarbage("collect")
  inst.heap_baseline = heap_bytes()
  local function app_heap()
    -- Lua-side bytes only: drop the mock's widget stand-ins.
    return heap_bytes() - inst.heap_baseline - inst.widgets_live * per_widget
  end

  -- Per-tick data goes into flat numeric arrays, not a table per tick: a table
  -- per tick would be harness garbage charged to the app's heap figure.
  local res = {
    ok = true, path = path, inst = inst, sandbox = sreport,
    a_calls = a_calls, a_ms = a_ms, a_churn = a_churn, a_widgets = a_widgets,
    samples = samples,                  -- {tick, live_bytes} after a full collect
    heap_peak = inst.heap_baseline, heap_baseline = inst.heap_baseline,
    script = o.name or "demo", n = 0,
  }

  -- Phase 1: compile + run the top-level chunk. This is where Packman died.
  local chunk, lerr = sandbox.load(src, "main.lua", env)
  if not chunk then
    res.ok = false; res.err = "compile failed: " .. tostring(lerr)
    res.err_phase = "compile"
    return res
  end
  local ok, dt = call(res, inst, "main", chunk)
  if not ok then return res end
  res.ms_main = dt
  collectgarbage("collect")
  res.heap_after_main = app_heap()

  -- Phase 2: on_enter.
  inst.reset_frame()
  ok, dt = call(res, inst, "enter", rawget(env, "on_enter"), inst.root)
  if not ok then return res end
  res.ms_enter = dt
  res.calls_enter = inst.calls
  collectgarbage("collect")
  res.heap_after_enter = app_heap()
  if res.heap_after_enter > heap_cap then
    res.ok = false; res.err_phase = "enter"
    res.err = ("HEAP: %.1f KB live after on_enter exceeds the %d KB heap_kb limit")
              :format(res.heap_after_enter / 1024, heap_cap // 1024)
    return res
  end

  -- Build the per-tick event map.
  local at = {}
  for _, e in ipairs(o.script or {}) do
    local t = e.t or 0
    at[t] = at[t] or {}
    at[t][#at[t] + 1] = e
  end

  -- Phase 3: ticks.
  local buttons_delivered = 0
  for i = 1, ticks do
    inst.ms = inst.ms + tick_ms
    inst.reset_frame()
    local h0 = heap_bytes()
    local t0 = now_ms()

    for _, e in ipairs(at[i] or {}) do
      local name = e.press or e.release
      local b = mock.BUTTON[name]
      if not b then
        res.ok = false
        res.err = "script names an unknown button: " .. tostring(name)
        res.err_phase = "script"
        return res
      end
      inst.held[b] = (e.press ~= nil) or nil
      buttons_delivered = buttons_delivered + 1
      local kind = e.press and mock.KIND.PRESSED or mock.KIND.RELEASED
      if not call(res, inst, "button", rawget(env, "on_button"), b, kind) then return res end
    end

    if not call(res, inst, "tick", rawget(env, "on_tick")) then return res end

    local dt2 = now_ms() - t0
    local h1 = heap_bytes()
    res.n = i
    res.a_calls[i] = inst.calls
    res.a_ms[i] = dt2
    res.a_churn[i] = h1 - h0
    res.a_widgets[i] = inst.widgets_live

    -- Sample the live set (what the badge's quota actually holds) every 25
    -- ticks. The raw count above is dominated by uncollected garbage.
    if i % 25 == 0 then
      collectgarbage("collect")
      local live = app_heap()
      samples[nsamp + 1] = i
      samples[nsamp + 2] = live
      nsamp = nsamp + 2
      if live > heap_cap then
        res.ok = false
        res.err_phase = "tick"
        res.err = ("HEAP: live set %.1f KB at tick %d exceeds the %d KB heap_kb "
                   .. "limit (desktop 64-bit, so this trips before hardware would)")
                  :format(live / 1024, i, heap_cap // 1024)
        return res
      end
    end
    if inst.exited then res.exited_at = i break end
  end

  -- Phase 4: on_exit.
  inst.reset_frame()
  ok, dt = call(res, inst, "exit", rawget(env, "on_exit"))
  if not ok then return res end
  res.ms_exit = dt
  res.buttons = buttons_delivered

  -- Summarise.
  local calls, mss, churn = {}, {}, 0
  local ms_total, calls_total, slow, zero = 0, 0, 0, 0
  for i = 1, res.n do
    calls[i] = res.a_calls[i]
    mss[i] = res.a_ms[i]
    ms_total = ms_total + res.a_ms[i]
    calls_total = calls_total + res.a_calls[i]
    if res.a_calls[i] == 0 then zero = zero + 1 end
    if res.a_churn[i] > 0 then churn = churn + res.a_churn[i] end
    if res.a_ms[i] > SLOW_TICK_WARN then slow = slow + 1 end
  end
  local n = math.max(1, res.n)
  table.sort(calls); table.sort(mss)
  res.sum = {
    n = res.n,
    calls_mean = calls_total / n,
    calls_p95 = percentile(calls, 0.95),
    calls_max = calls[#calls] or 0,
    calls_zero = zero,
    ms_mean = ms_total / n,
    ms_p95 = percentile(mss, 0.95),
    ms_max = mss[#mss] or 0,
    ms_total = ms_total,
    slow_ticks = slow,
    widgets_peak = inst.widgets_peak,
    widgets_live = inst.widgets_live,
    heap_after_enter_kb = res.heap_after_enter / 1024,
    churn_per_tick = churn / n,
    led_shows = inst.led_shows,
    style_calls = inst.style_calls,
    style_props = inst.style_props,
    store_writes = inst.store_writes,
  }
  -- Tare the harness's own bookkeeping out of the heap figures: collect first
  -- so the comparison is live-set against live-set, then drop the recording
  -- arrays and collect again.
  collectgarbage("collect")
  local with_arrays = app_heap()
  res.a_calls, res.a_ms, res.a_churn, res.a_widgets = nil, nil, nil, nil
  calls, mss = nil, nil          -- these are still live locals; drop them too
  collectgarbage("collect")
  local without = app_heap()
  local tare = math.max(0, with_arrays - without)
  res.sum.heap_live_exit_kb = without / 1024
  res.sum.bytes_per_mock_widget = per_widget

  -- Correct each live sample by the share of the tare accumulated by then.
  local peak = 0
  local sp = res.samples
  for k = 1, nsamp, 2 do
    local tick, live = sp[k], sp[k + 1]
    local corrected = live - tare * (tick / n)
    if corrected > peak then peak = corrected end
  end
  res.sum.heap_peak_kb = math.max(peak, res.heap_after_enter, without) / 1024
  res.sum.harness_tare_kb = tare / 1024
  return res
end

-- Sorted list of the native calls that dominate a run.
function M.hot(res, top)
  local t = {}
  for k, v in pairs(res.inst.by_name_total) do t[#t + 1] = {k, v} end
  table.sort(t, function(a, b) return a[2] > b[2] end)
  local out = {}
  for i = 1, math.min(top or 10, #t) do out[i] = t[i] end
  return out
end

function M.report(res, opts)
  opts = opts or {}
  local p = function(...) print(string.format(...)) end
  p("")
  p("  script: %-10s  app: %s", res.script, res.path)
  if not res.ok then
    p("  RESULT: FAIL in %s", res.err_phase or "?")
    p("")
    for line in tostring(res.err):gmatch("[^\n]+") do p("    %s", line) end
    p("")
    return false
  end
  local s = res.sum
  p("  RESULT: ok   %d ticks, %d button events", s.n, res.buttons or 0)
  p("")
  p("  native calls/frame   mean %7.1f   p95 %5d   max %5d   idle frames %d/%d",
    s.calls_mean, s.calls_p95, s.calls_max, s.calls_zero, s.n)
  p("  widgets              peak %7d   live %5d   cap 512",
    s.widgets_peak, s.widgets_live)
  p("  lua heap (live)      peak %7.1f KB  after on_enter %.1f KB  at exit %.1f KB  cap 96 KB",
    s.heap_peak_kb, s.heap_after_enter_kb, s.heap_live_exit_kb)
  p("  gc churn/tick        %10.0f B   heap growth over run %.1f KB",
    s.churn_per_tick, s.heap_live_exit_kb - (res.heap_after_enter or 0) / 1024)
  p("    ^ desktop 64-bit live set, ~2x the badge's 32-bit build, and it still")
  p("      includes a ~6 KB harness floor. Use the TREND and growth, not the cap.")
  p("  style{} calls        %10d   props set %d", s.style_calls, s.style_props)
  p("  desktop ms/tick      mean %7.3f   p95 %5.3f   max %5.3f   (>%d ms: %d)",
    s.ms_mean, s.ms_p95, s.ms_max, 5, s.slow_ticks)
  p("  desktop ms           main %.2f  on_enter %.2f  on_exit %.2f",
    res.ms_main or 0, res.ms_enter or 0, res.ms_exit or 0)
  p("  on_enter native calls %9d", res.calls_enter or 0)
  p("  led show() calls     %10d   store writes %d", s.led_shows, s.store_writes)

  if opts.hot ~= false then
    p("")
    p("  hottest native calls (whole run)")
    for _, kv in ipairs(M.hot(res, 8)) do
      p("    %-22s %8d   %6.1f /frame", kv[1], kv[2], kv[2] / math.max(1, s.n))
    end
  end

  local g = res.sandbox.globals
  if #g > 0 then
    local seen, uniq = {}, {}
    for _, name in ipairs(g) do
      if not seen[name] then seen[name] = true; uniq[#uniq + 1] = name end
    end
    p("")
    p("  WARNING accidental globals (%d): %s", #uniq, table.concat(uniq, ", "))
  end
  if #res.inst.warnings > 0 then
    p("")
    p("  warnings (%d):", #res.inst.warnings)
    for i = 1, math.min(10, #res.inst.warnings) do p("    %s", res.inst.warnings[i]) end
  end
  p("")
  return true
end

return M
