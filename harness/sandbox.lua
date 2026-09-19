-- sandbox.lua -- load main.lua in an environment matching the badge's sandbox.
--
-- Available per BADGE_GUIDE.md: base (without dofile, loadfile, load, require,
-- pcall, xpcall, setmetatable), table, string, math, utf8. os, io, package,
-- debug and coroutine are absent entirely.
--
-- Any read of a global that is not in that set raises a SANDBOX VIOLATION so it
-- is a loud test failure rather than a runtime nil.

local M = {}

-- base minus the six the firmware removes.
local BASE_ALLOWED = {
  "assert", "collectgarbage", "error", "getmetatable", "ipairs", "next",
  "pairs", "print", "rawequal", "rawget", "rawlen", "rawset", "select",
  "tonumber", "tostring", "type", "_VERSION",
}

-- Names that exist in desktop Lua but NOT on the badge. Reading one of these
-- gets a message that says so, instead of the generic undefined-global text.
M.DOCUMENTED_ABSENT = {
  dofile = "base function removed by the badge sandbox",
  loadfile = "base function removed by the badge sandbox",
  load = "base function removed by the badge sandbox",
  require = "only available as the module sandbox; a single-file app must not use it",
  pcall = "removed: errors are fatal on the badge, so validate inputs instead of catching",
  xpcall = "removed: errors are fatal on the badge",
  setmetatable = "removed by the badge sandbox",
  os = "library absent entirely (use badge.sys.ms for time)",
  io = "library absent entirely",
  package = "library absent entirely",
  debug = "library absent entirely",
  coroutine = "library absent entirely (no thread/coroutine API is exposed)",
}

M.LIFECYCLE = {
  on_enter = true, on_tick = true, on_button = true, on_exit = true,
  on_recv = true,
}

-- Build the app environment. Returns env, report where report.globals lists
-- every global the app defined that is not a lifecycle callback.
function M.env(badge, opts)
  opts = opts or {}
  local report = { globals = {}, reads = {} }
  local env = {}

  for _, n in ipairs(BASE_ALLOWED) do env[n] = _G[n] end
  env.table, env.string, env.math, env.utf8 = table, string, math, utf8
  env.badge = badge
  env._G = env

  setmetatable(env, {
    __index = function(_, k)
      local why = M.DOCUMENTED_ABSENT[k]
      if why then
        error(("SANDBOX VIOLATION: read of '%s' -- %s"):format(tostring(k), why), 0)
      end
      error(("SANDBOX VIOLATION: read of undefined global '%s' (typo, or a global "
             .. "used before it was assigned)"):format(tostring(k)), 0)
    end,
    __newindex = function(t, k, v)
      if not M.LIFECYCLE[k] then
        report.globals[#report.globals + 1] = tostring(k)
      end
      rawset(t, k, v)
    end,
  })

  return env, report
end

-- Load source in text mode only, exactly as the badge does (bytecode refused).
function M.load(src, chunkname, env)
  if src:byte(1) == 27 then
    return nil, "precompiled bytecode is refused by the badge (text mode only)"
  end
  return load(src, "@" .. chunkname, "t", env)
end

function M.read(path)
  local f = io.open(path, "rb")
  if not f then return nil, "cannot open " .. path end
  local s = f:read("a")
  f:close()
  return s
end

return M
