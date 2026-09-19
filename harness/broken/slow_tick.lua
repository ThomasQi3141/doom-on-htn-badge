function on_enter(root) end
function on_tick()
  local s = 0.0
  for i = 1, 8000000 do s = s + math.sin(i) * math.cos(i) end
  if s == 1234.5 then badge.sys.log("never") end
end
