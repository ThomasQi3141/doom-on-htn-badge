-- run.lua -- the harness entry point: static checks, then the sim scripts.
package.path = "./?.lua;" .. package.path
local check   = require("check")
local sim     = require("sim")
local scripts = require("scripts")

local want = {}
for _, a in ipairs(arg) do want[a] = true end
local root = ".."

print("")
print(("=== badge harness  (Lua %s)  ==="):format(_VERSION))
local sok, stats = check.run{root = root, hazards = want["--hazards"]}

local order = {"demo", "idle", "spin", "charge", "restart"}
local ok = sok
local rows = {}
print("  == simulation ==")
for _, name in ipairs(order) do
  local r = sim.run{
    app = root .. "/main.lua",
    ticks = tonumber(want["--ticks"]) or 600,
    script = scripts.all[name](),
    name = name,
  }
  ok = sim.report(r, {hot = (name == "demo")}) and ok
  if r.ok then rows[#rows + 1] = {name, r.sum} end
end

print("  == scoreboard ==")
print("")
print(("  %-9s %8s %8s %8s %9s %9s %8s"):format(
  "script", "calls/f", "p95", "widgets", "heap KB", "churn B", "ms/f"))
for _, row in ipairs(rows) do
  local n, s = row[1], row[2]
  print(("  %-9s %8.1f %8d %8d %9.1f %9.0f %8.3f"):format(
    n, s.calls_mean, s.calls_p95, s.widgets_peak, s.heap_peak_kb,
    s.churn_per_tick, s.ms_mean))
end
print("")
if stats then
  print(("  source %.1f KiB | %d functions | %d float hazards | compile heap %d B"):format(
    stats.size / 1024, stats.functions, stats.hazards, stats.compile_heap))
end
print(("  OVERALL: %s"):format(ok and "PASS" or "FAIL"))
print("")
os.exit(ok and 0 or 1)
