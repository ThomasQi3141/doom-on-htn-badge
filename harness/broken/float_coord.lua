local b
function on_enter(root) b = badge.ui.box(root, 8, 8) end
function on_tick() b:set_pos(1.5, 2) end
