-- selftest.lua -- prove the harness catches deliberately broken apps.
--
-- Every fixture in broken/ must fail with the expected error. If one PASSES,
-- the harness is not trustworthy and this exits nonzero. No number the harness
-- reports means anything until this is green.
package.path = "./?.lua;" .. package.path
local sim     = require("sim")
local check   = require("check")
local scripts = require("scripts")

-- {fixture, pattern the error must match, what it proves}
local CASES = {
  {"uses_pcall",        "SANDBOX VIOLATION.*pcall",        "pcall is removed by the sandbox"},
  {"uses_os",           "SANDBOX VIOLATION.*'os'",         "the os library is absent"},
  {"undefined_global",  "SANDBOX VIOLATION.*COLZ",         "a typo'd global is loud, not nil"},
  {"widget_flood",      "BADGE%-VIOLATION%[widget_cap%]",  "the 512 live-widget cap"},
  {"float_coord",       "BADGE%-VIOLATION%[float%]",       "a fractional widget coordinate"},
  {"whole_float",       "BADGE%-VIOLATION%[float%]",       "160/2 = 80.0 is still a float"},
  {"heap_bomb",         "HEAP: live set",                  "the heap_kb ceiling"},
  {"use_after_delete",  "BADGE%-VIOLATION%[deleted%]",     "using a deleted widget"},
  {"bad_style_key",     "BADGE%-VIOLATION%[style%]",       "an unknown style key"},
  {"store_in_tick",     "BADGE%-VIOLATION%[flash%]",       "a flash write at frame rate"},
  {"invented_api",      "BADGE%-VIOLATION%[api%]",         "an API not in BADGE_GUIDE.md"},
  {"too_many_points",   "BADGE%-VIOLATION%[cap%]",         "the 128-point line cap"},
  {"float_led",         "BADGE%-VIOLATION%[type%]",        "a non-integer LED channel"},
  {"syntax_error",      "compile failed",                  "a file that does not compile"},
  {"slow_tick",         "past the 250 ms failure cutoff",  "a runaway tick"},
}

local pass, fail = 0, 0
local function ok(cond, fmt, ...)
  if cond then pass = pass + 1 else fail = fail + 1 print(("  FAIL  " .. fmt):format(...)) end
  return cond
end

print("")
print("  == harness selftest ==")
print("")
for _, c in ipairs(CASES) do
  local name, pat, why = c[1], c[2], c[3]
  local r = sim.run{
    app = "broken/" .. name .. ".lua",
    ticks = 60, script = scripts.demo(), name = name,
  }
  if r.ok then
    fail = fail + 1
    print(("  FAIL  %-18s RAN CLEAN -- harness misses %s"):format(name, why))
  elseif not tostring(r.err):match(pat) then
    fail = fail + 1
    print(("  FAIL  %-18s wrong error for %s"):format(name, why))
    print(("        wanted /%s/, got: %s"):format(pat, tostring(r.err):gsub("\n.*", "")))
  else
    pass = pass + 1
    print(("  ok    %-18s catches %s"):format(name, why))
  end
end

-- A clean app must NOT trip anything: guards against a harness that fails
-- everything and therefore proves nothing.
local r = sim.run{app = "broken/ok_null.lua", ticks = 120, script = scripts.demo(), name = "ok_null"}
ok(r.ok, "%-18s a clean app was rejected: %s", "ok_null", tostring(r.err))
if r.ok then print(("  ok    %-18s a clean app still passes"):format("ok_null")) end

-- Static: an oversized source must fail check.lua's size gate.
local tmp = os.tmpname() .. "_dir"
os.execute("mkdir -p " .. tmp .. "/dist")
local f = io.open(tmp .. "/main.lua", "w")
f:write("-- pad\n" .. ("-- " .. ("x"):rep(60) .. "\n"):rep(1100) .. "function on_tick() end\n")
f:close()
f = io.open(tmp .. "/manifest.cfg", "w")
f:write("slug=htn_doom\nname=Badge Doom\napi=2\nheap_kb=96\n")
f:close()
print("")
io.write("  (size gate on a 70 KiB source)")
local saved = print
print = function() end
local sok = check.run{root = tmp}
print = saved
print("")
if ok(not sok, "%-18s a 70 KiB main.lua was accepted", "oversize") then
  print(("  ok    %-18s catches a source over the 64 KiB cap"):format("oversize"))
end
os.execute("rm -rf " .. tmp)

print("")
print(("  selftest: %s  (%d passed, %d failed)"):format(
  fail == 0 and "TRUSTWORTHY" or "NOT TRUSTWORTHY", pass, fail))
print("")
os.exit(fail == 0 and 0 or 1)
