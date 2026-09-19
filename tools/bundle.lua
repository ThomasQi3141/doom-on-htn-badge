-- bundle.lua -- manifest.cfg + main.lua -> dist/htn_doom.lua (Import app format)
local root = arg[1] or "."
local function read(p)
  local f = assert(io.open(p, "rb"), "cannot read " .. p)
  local s = f:read("a"); f:close(); return s
end
local man = read(root .. "/manifest.cfg"):gsub("%s*$", "")
local src = read(root .. "/main.lua")
local out = ("--[==[badge-app\n%s\n]==]\n\n%s"):format(man, src)
local f = assert(io.open(root .. "/dist/htn_doom.lua", "wb"))
f:write(out); f:close()
print(("bundled dist/htn_doom.lua  %d bytes (main.lua %d)"):format(#out, #src))
