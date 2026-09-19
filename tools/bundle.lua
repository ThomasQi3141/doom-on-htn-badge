-- bundle.lua -- <src>/manifest.cfg + <src>/main.lua -> dist/<name>.lua
-- Produces the single-file "Import app" format: manifest header, then code.
local src_dir = arg[1] or "."
local out     = arg[2] or "dist/htn_doom.lua"
local function read(p)
  local f = assert(io.open(p, "rb"), "cannot read " .. p)
  local s = f:read("a"); f:close(); return s
end
local man = read(src_dir .. "/manifest.cfg"):gsub("%s*$", "")
local code = read(src_dir .. "/main.lua")
local blob = ("--[==[badge-app\n%s\n]==]\n\n%s"):format(man, code)
local f = assert(io.open(out, "wb"))
f:write(blob); f:close()
print(("%-28s %6d bytes  (main.lua %d, manifest %d)"):format(out, #blob, #code, #man))
