-- check.lua -- static checks on main.lua and manifest.cfg. Runs every time.

local mock    = require("mock_badge")
local sandbox = require("sandbox")

local M = {}

local SIZE_HARD = 64 * 1024   -- firmware cap on main.lua
local SIZE_WARN = 18 * 1024   -- stop adding features, start consolidating
local SIZE_NOTE = 14 * 1024   -- the size at which Packman failed to compile

-- Blank out comments and string bodies, preserving length and newlines, so the
-- scans below cannot fire on prose or on text inside a label.
function M.strip(src)
  local out, i, n = {}, 1, #src
  local function keep(c) out[#out + 1] = c end
  local function blank(c) keep(c == "\n" and "\n" or " ") end
  while i <= n do
    local c = src:sub(i, i)
    local two = src:sub(i, i + 1)
    -- long bracket, as a comment or as a string
    local eqs = nil
    local start = i
    if two == "--" then
      local e = src:match("^%-%-%[(=*)%[", i)
      if e then eqs = e; start = i + 2 + #e + 2
      else
        while i <= n and src:sub(i, i) ~= "\n" do blank(src:sub(i, i)); i = i + 1 end
        goto continue
      end
    else
      local e = src:match("^%[(=*)%[", i)
      if e then eqs = e; start = i + 1 + #e + 1 end
    end
    if eqs then
      local close = "]" .. eqs .. "]"
      local fin = src:find(close, start, true)
      local stop = fin and (fin + #close - 1) or n
      for j = i, stop do blank(src:sub(j, j)) end
      i = stop + 1
      goto continue
    end
    if c == '"' or c == "'" then
      keep(c); i = i + 1
      while i <= n do
        local d = src:sub(i, i)
        if d == "\\" then blank(d); blank(src:sub(i + 1, i + 1)); i = i + 2
        elseif d == c then keep(d); i = i + 1; break
        elseif d == "\n" then keep(d); i = i + 1; break
        else blank(d); i = i + 1 end
      end
      goto continue
    end
    keep(c); i = i + 1
    ::continue::
  end
  return table.concat(out)
end

local function lines_of(src)
  local starts, pos = {1}, 1
  while true do
    local nl = src:find("\n", pos, true)
    if not nl then break end
    starts[#starts + 1] = nl + 1
    pos = nl + 1
  end
  return starts
end

local function line_at(starts, off)
  local lo, hi = 1, #starts
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if starts[mid] <= off then lo = mid else hi = mid - 1 end
  end
  return lo
end

-- Names the badge sandbox does not provide.
local FORBIDDEN = {
  "pcall", "xpcall", "load", "loadfile", "dofile", "require", "setmetatable",
}
local FORBIDDEN_LIB = {"os", "io", "package", "debug", "coroutine"}

local STRING_METHODS = {
  format = true, match = true, gmatch = true, gsub = true, find = true,
  byte = true, sub = true, rep = true, len = true, lower = true, upper = true,
  reverse = true, char = true,
}

function M.run(opts)
  opts = opts or {}
  local root = opts.root or ".."
  local main_path = root .. "/main.lua"
  local man_path = root .. "/manifest.cfg"
  local p = function(...) print(string.format(...)) end
  local fails, warns = {}, {}
  local function fail(...) fails[#fails + 1] = string.format(...) end
  local function warn(...) warns[#warns + 1] = string.format(...) end

  local src = sandbox.read(main_path)
  if not src then
    p("  FATAL: cannot read %s", main_path)
    return false
  end
  local stripped = M.strip(src)
  local starts = lines_of(src)

  p("")
  p("  == static checks ==")

  -- size --------------------------------------------------------------------
  local size = #src
  local bar_n = math.min(40, math.floor(size / SIZE_HARD * 40))
  p("  main.lua        %6d bytes  %5.1f KiB  [%s%s] %.1f%% of 64 KiB cap",
    size, size / 1024, ("#"):rep(bar_n), ("."):rep(40 - bar_n), size / SIZE_HARD * 100)
  if size > SIZE_HARD then fail("main.lua is %d bytes, over the 64 KiB hard cap", size) end
  if size > SIZE_WARN then
    warn("main.lua is %.1f KiB, past the %.0f KiB line: stop adding features, consolidate",
         size / 1024, SIZE_WARN / 1024)
  end
  if size > SIZE_NOTE then
    warn("main.lua is past %.0f KiB -- the guide's Packman case failed to COMPILE at "
         .. "~14 KiB / 32 functions. Watch the compile-heap number below.",
         SIZE_NOTE / 1024)
  end

  -- compiles ----------------------------------------------------------------
  local badge = mock.build{}
  local env = sandbox.env(badge)
  collectgarbage("collect")
  local h0 = math.floor(collectgarbage("count") * 1024)
  local chunk, lerr = sandbox.load(src, "main.lua", env)
  local h1 = math.floor(collectgarbage("count") * 1024)
  if not chunk then
    fail("main.lua does not compile under Lua 5.4: %s", tostring(lerr))
    p("  compiles        NO -- %s", tostring(lerr))
  else
    p("  compiles        yes (Lua %s, text mode)", _VERSION:match("%d+%.%d+"))
    p("  compile heap    %6d bytes retained by load() (desktop 64-bit; the badge's "
      .. "32-bit build is smaller, but watch this GROW)", h1 - h0)
  end
  chunk = nil
  collectgarbage("collect")

  -- functions ---------------------------------------------------------------
  local nfun = select(2, stripped:gsub("%f[%w]function%f[%W]", ""))
  p("  functions       %6d   (Packman failed to compile at 32)", nfun)
  if nfun > 32 then
    warn("%d function definitions -- at or past the count that failed to compile "
         .. "in the guide's worked example", nfun)
  end

  -- forbidden ---------------------------------------------------------------
  local nviol = 0
  for _, name in ipairs(FORBIDDEN) do
    for off in stripped:gmatch("()%f[%w_]" .. name .. "%f[^%w_]") do
      nviol = nviol + 1
      fail("main.lua:%d uses '%s', which the badge sandbox removes", line_at(starts, off), name)
    end
  end
  for _, name in ipairs(FORBIDDEN_LIB) do
    for off in stripped:gmatch("()%f[%w_]" .. name .. "%f[^%w_]%s*%.") do
      nviol = nviol + 1
      fail("main.lua:%d uses the '%s' library, which is absent on the badge",
           line_at(starts, off), name)
    end
  end
  p("  sandbox         %s", nviol == 0 and "clean (no removed globals referenced)"
    or (nviol .. " VIOLATIONS"))

  -- badge API whitelist -----------------------------------------------------
  local unknown = {}
  for off, a, b in stripped:gmatch("()badge%.([%w_]+)%.([%w_]+)") do
    local path = ("badge.%s.%s"):format(a, b)
    if not mock.API[path] then
      unknown[#unknown + 1] = ("main.lua:%d %s"):format(line_at(starts, off), path)
    end
  end
  for off, m in stripped:gmatch("():([%w_]+)%s*[%(%{]") do
    if not mock.WIDGET_METHODS[m] and not STRING_METHODS[m] then
      unknown[#unknown + 1] = ("main.lua:%d :%s()"):format(line_at(starts, off), m)
    end
  end
  if #unknown > 0 then
    for _, u in ipairs(unknown) do fail("not in BADGE_GUIDE.md: %s", u) end
    p("  badge API       %d UNKNOWN calls", #unknown)
  else
    p("  badge API       all calls documented")
  end

  -- float hazards -----------------------------------------------------------
  local hz = {}
  local function haz(off, what)
    hz[#hz + 1] = {line = line_at(starts, off), what = what}
  end
  for off in stripped:gmatch("()/") do
    local before = stripped:sub(off - 1, off - 1)
    local after = stripped:sub(off + 1, off + 1)
    if before ~= "/" and after ~= "/" then haz(off, "/ (returns a FLOAT in Lua 5.4; use //)") end
  end
  for _, fn in ipairs{"sin", "cos", "sqrt", "floor", "ceil", "abs", "atan", "tan", "pi", "fmod"} do
    for off in stripped:gmatch("()math%." .. fn .. "%f[^%w_]") do
      haz(off, "math." .. fn .. " (software float on a no-FPU ESP32-C3)")
    end
  end
  for off in stripped:gmatch("()%f[%w_]%d+%.%d+") do haz(off, "float literal") end
  for off in stripped:gmatch("()%f[%w_]%d+[eE][%-+]?%d+") do haz(off, "float literal (exponent)") end
  table.sort(hz, function(a, b) return a.line < b.line end)
  p("  float hazards   %6d   (target: 0 after the fixed-point conversion)", #hz)
  if opts.hazards and #hz > 0 then
    local byline = {}
    for _, h in ipairs(hz) do
      byline[h.line] = byline[h.line] or {}
      local t = byline[h.line]
      t[#t + 1] = h.what
    end
    local ls = {}
    for l in pairs(byline) do ls[#ls + 1] = l end
    table.sort(ls)
    for _, l in ipairs(ls) do
      local seen, u = {}, {}
      for _, w in ipairs(byline[l]) do
        if not seen[w] then seen[w] = true; u[#u + 1] = w end
      end
      p("      main.lua:%-4d %s", l, table.concat(u, ", "))
    end
  end

  -- manifest ----------------------------------------------------------------
  local man = sandbox.read(man_path)
  if not man then
    fail("cannot read %s", man_path)
  else
    local kv, order = {}, {}
    for line in man:gmatch("[^\n]+") do
      if not line:match("^%s*#") and line:match("%S") then
        local k, v = line:match("^([%w_]+)=(.*)$")
        if not k then
          fail("manifest.cfg: bad line %q (key=value only, no inline comments)", line)
        elseif kv[k] then
          fail("manifest.cfg: duplicate key '%s'", k)
        else
          kv[k] = v; order[#order + 1] = k
        end
      end
    end
    local want = {slug = "htn_doom", api = "2"}
    for k, v in pairs(want) do
      if kv[k] ~= v then fail("manifest.cfg: %s must be %s, got %s", k, v, tostring(kv[k])) end
    end
    if kv.heap_kb and kv.heap_kb ~= "48" and kv.heap_kb ~= "96" then
      fail("manifest.cfg: heap_kb must be 48 or 96, got %s", kv.heap_kb)
    end
    if not kv.name or #kv.name < 1 or #kv.name > 48 then
      fail("manifest.cfg: name must be 1-48 bytes")
    end
    if kv.icon and #kv.icon > 12 then fail("manifest.cfg: icon must be <=12 bytes") end
    if kv.home_button == "1" and kv.confirm_home == "1" then
      fail("manifest.cfg: home_button=1 cannot be combined with confirm_home=1")
    end
    p("  manifest        slug=%s api=%s heap_kb=%s wake_lock=%s",
      kv.slug, kv.api, kv.heap_kb or "48", kv.wake_lock or "0")
  end

  -- dist round-trip ---------------------------------------------------------
  local dist = sandbox.read(root .. "/dist/htn_doom.lua")
  if dist then
    local hdr, body = dist:match("^%-%-%[==%[badge%-app\n(.-)\n%]==%]\n(.*)$")
    if not hdr then
      fail("dist/htn_doom.lua does not start with a valid badge-app header block")
    else
      local function trim(x) return (x:gsub("%s*$", "")) end
      if trim(hdr) ~= trim(man) then
        fail("dist/htn_doom.lua header does not match manifest.cfg")
      end
      if body:gsub("^\n", "") ~= src then
        fail("dist/htn_doom.lua body does not match main.lua (run tools/bundle.lua)")
      end
      if not fails[1] then p("  dist            round-trips to manifest.cfg + main.lua") end
    end
  else
    warn("dist/htn_doom.lua not built yet (run tools/bundle.lua)")
  end

  -- verdict -----------------------------------------------------------------
  p("")
  for _, w in ipairs(warns) do p("  WARN  %s", w) end
  for _, f in ipairs(fails) do p("  FAIL  %s", f) end
  p("")
  p("  static: %s (%d failures, %d warnings)",
    #fails == 0 and "PASS" or "FAIL", #fails, #warns)
  return #fails == 0, {size = size, functions = nfun, hazards = #hz,
                       compile_heap = h1 - h0}
end

return M
