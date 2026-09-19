local r
function on_enter(root) r = root end
function on_tick() for i = 1, 40 do badge.ui.box(r, 4, 4) end end
