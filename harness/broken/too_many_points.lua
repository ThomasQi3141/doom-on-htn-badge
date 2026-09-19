function on_enter(root)
  local p = {}
  for i = 1, 200 do p[i] = {i, i} end
  badge.ui.line(root, p)
end
function on_tick() end
