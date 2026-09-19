local keep = {}
function on_enter(root) end
function on_tick() for i = 1, 200 do keep[#keep + 1] = {1, 2, 3, 4, 5, 6, 7, 8} end end
