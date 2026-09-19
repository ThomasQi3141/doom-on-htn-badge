-- mock_badge.lua -- mock of the badge.* API surface documented in BADGE_GUIDE.md.
--
-- build(opts) -> badge, inst
--   badge : the table handed to the sandboxed app
--   inst  : instrumentation handle the sim reports on
--
-- Every badge.* function and every widget method counts as one native call.
-- Violations raise; the sim catches them with a traceback.

local WIDGET_CAP   = 512
local TEXT_CAP     = 1024
local LINE_POINTS  = 128
local STORE_KEYS   = 32
local STORE_STRLEN = 128
local FS_QUOTA     = 64 * 1024
local FS_FILE_MAX  = 16 * 1024

local function set(list)
  local t = {}
  for _, v in ipairs(list) do t[v] = true end
  return t
end

-- Style keys accepted by :style{} and by factory tables (guide: "Supported style keys").
local STYLE_KEYS = set{
  "bg_color", "bg_opa", "color", "opa", "radius",
  "border_color", "border_opa", "border_width",
  "text_color", "text_opa", "text_font", "text_align",
  "arc_color", "arc_opa", "arc_width",
  "line_color", "line_opa", "line_width",
  "pad_all", "pad_top", "pad_bottom", "pad_left", "pad_right",
  "pad_row", "pad_column",
  "shadow_color", "shadow_opa", "shadow_width", "shadow_spread",
  "shadow_offset_x", "shadow_offset_y",
  "flex_flow",
}

local SEL_PART  = set{"main", "indicator", "knob", "items", "scrollbar"}
local SEL_STATE = set{"pressed", "checked", "disabled", "focused"}

local FONT_OK = set{"small", "default", "large"}
local FONT_PX = set{14, 16, 18, 20, 22, 24}
local ALIGN_OK = set{
  "center", "top_left", "top_mid", "top_right",
  "bottom_left", "bottom_mid", "bottom_right", "left_mid", "right_mid",
}
local FLEX_OK = set{"row", "column", "row_wrap", "column_wrap"}
local TEXT_ALIGN_OK = set{"left", "center", "right"}

local WIDGET_TYPES = set{
  "label", "box", "bar", "arc", "slider", "image", "line",
  "button", "switch", "checkbox", "roller", "textarea",
}

-- Methods the firmware type-checks per call.
local METHOD_TYPES = {
  set_text     = set{"label", "checkbox", "textarea"},
  set_value    = set{"bar", "arc", "slider"},
  set_range    = set{"bar", "arc", "slider"},
  set_src      = set{"image"},
  set_points   = set{"line"},
  set_checked  = set{"switch", "checkbox"},
  get_checked  = set{"switch", "checkbox"},
  set_options  = set{"roller"},
  get_selected = set{"roller"},
}

-- Factory table keys that are not style keys.
local FACTORY_KEYS = set{
  "parent", "x", "y", "w", "h", "align", "align_x", "align_y",
  "hidden", "clickable",
  "text", "value", "min", "max", "checked", "options", "src", "points",
}

local BUTTON = {
  A = 1, B = 2, HOME = 3, DOWN = 4, LEFT = 5,
  RIGHT = 6, UP = 7, AUX1 = 8, START = 9,
}
local KIND = { PRESSED = 1, RELEASED = 2 }

local THEME = {
  background = 0x0b0d12, panel = 0x14161c, surface = 0x1c1f27,
  track = 0x2a2e38, border = 0x3a3f4b, accent = 0xd24b2a,
  accent_detail = 0xf0c24a, text = 0xe6e8ee, text_soft = 0xbfc4cf,
  text_muted = 0x8a8f9c, text_dim = 0x5a5f6b,
}

local M = {}
M.WIDGET_CAP = WIDGET_CAP
M.BUTTON = BUTTON
M.KIND = KIND

-- Every badge.* path this mock implements. check.lua reads this so the static
-- whitelist and the runtime mock cannot drift apart.
M.API = set{
  "badge.ui.label", "badge.ui.box", "badge.ui.bar", "badge.ui.arc",
  "badge.ui.slider", "badge.ui.image", "badge.ui.line", "badge.ui.button",
  "badge.ui.switch", "badge.ui.checkbox", "badge.ui.roller", "badge.ui.textarea",
  "badge.ui.screen_width", "badge.ui.screen_height", "badge.ui.theme",
  "badge.led.set", "badge.led.set_all", "badge.led.clear", "badge.led.show",
  "badge.led.count",
  "badge.sensor.accel", "badge.sensor.shake", "badge.sensor.tap",
  "badge.sensor.orientation",
  "badge.input.BUTTON", "badge.input.KIND", "badge.input.is_down",
  "badge.input.held",
  "badge.sys.ms", "badge.sys.uptime", "badge.sys.log", "badge.sys.random",
  "badge.sys.heap", "badge.sys.gc_step", "badge.sys.version",
  "badge.sys.wake_lock", "badge.sys.stats",
  "badge.store.set", "badge.store.get", "badge.store.set_int",
  "badge.store.get_int", "badge.store.set_str", "badge.store.get_str",
  "badge.me.name", "badge.me.role", "badge.me.role_name", "badge.me.color",
  "badge.me.badge_id", "badge.me.provisioned",
  "badge.contacts.count", "badge.contacts.get",
  "badge.app.slug", "badge.app.name", "badge.app.exit",
  "badge.fs.write", "badge.fs.append", "badge.fs.read", "badge.fs.exists",
  "badge.fs.remove", "badge.fs.list", "badge.fs.mkdir",
  "badge.nfc.enable", "badge.nfc.disable", "badge.nfc.card",
  "badge.nfc.read_text", "badge.nfc.clear",
  "badge.radio.enable", "badge.radio.disable", "badge.radio.send",
  "badge.radio.on_recv", "badge.radio.mac", "badge.radio.dropped",
}

M.WIDGET_METHODS = set{
  "set_pos", "set_size", "align", "parent", "child", "child_count", "type",
  "hidden", "clickable", "bring_to_front", "delete", "set_text", "set_value",
  "set_range", "set_src", "set_points", "set_checked", "get_checked",
  "set_options", "get_selected", "set_color", "set_border", "set_font_size",
  "style",
}

function M.build(opts)
  opts = opts or {}

  local inst = {
    widgets_live = 0, widgets_peak = 0, widgets_created = 0, widgets_deleted = 0,
    calls = 0, by_name = {},               -- reset each frame by the sim
    calls_total = 0, by_name_total = {},
    style_calls = 0, style_props = 0,      -- style{} allocates a table per call
    led_sets = 0, led_shows = 0, led_pending = 0,
    store_writes = 0, store_writes_in_tick = 0,
    log = {}, warnings = {},
    phase = "init",
    ms = 0,
    strict_int = (opts.strict_int ~= false),
    heap_baseline = 0,
    exited = false,
    accel_ok = opts.accel_ok or false,
    nfc_ok = opts.nfc_ok or false,
    radio_ok = opts.radio_ok or false,
  }

  local rng = opts.seed or 20260918
  local function nextrand()
    rng = (rng * 1103515245 + 12345) % 2147483648
    return rng
  end

  local function fail(kind, fmt, ...)
    error(("BADGE-VIOLATION[%s] %s"):format(kind, fmt:format(...)), 0)
  end
  inst.fail = fail

  local function hit(name)
    inst.calls = inst.calls + 1
    inst.calls_total = inst.calls_total + 1
    inst.by_name[name] = (inst.by_name[name] or 0) + 1
    inst.by_name_total[name] = (inst.by_name_total[name] or 0) + 1
  end

  local function warn(fmt, ...)
    inst.warnings[#inst.warnings + 1] =
      ("[%s @%dms] %s"):format(inst.phase, inst.ms, fmt:format(...))
  end

  -- The core check: widget geometry must be a Lua *integer*, not a float that
  -- happens to be whole. `8.0` is what a leftover `/` produces. Stricter than
  -- hardware on purpose; opts.strict_int = false downgrades it to a warning.
  local function needint(v, what, where)
    if math.type(v) == "integer" then return v end
    if type(v) ~= "number" then
      fail("type", "%s expects a number for %s, got %s (%s)",
           where, what, type(v), tostring(v))
    end
    if inst.strict_int then
      fail("float", "%s got a FLOAT for %s: %s -- widget geometry must be an "
           .. "integer (use // not /, and drop math.floor once nothing is a float)",
           where, what, tostring(v))
    end
    warn("%s got float %s for %s", where, tostring(v), what)
    return math.floor(v)
  end

  local function needcolor(v, where)
    if math.type(v) ~= "integer" then
      fail("type", "%s expects an integer 0xRRGGBB color, got %s", where, tostring(v))
    end
    if v < 0 or v > 0xffffff then
      fail("range", "%s color out of range: %d", where, v)
    end
    return v
  end

  ----------------------------------------------------------------- widgets --
  local Widget = {}
  Widget.__index = Widget
  Widget.__name = "badge.widget"

  local function alive(w, method)
    if w._deleted then
      fail("deleted", "widget has been deleted (:%s on a dead %s)", method, w._type)
    end
    if w._root and (method == "style" or method == "set_size" or method == "set_pos"
                    or method == "delete" or method == "set_color"
                    or method == "set_border" or method == "align") then
      fail("root", "root is read-only: :%s() on root is refused by the firmware", method)
    end
  end

  local function typecheck(w, method)
    local ok = METHOD_TYPES[method]
    if ok and not ok[w._type] then
      fail("type", ":%s() is not available on a %s widget", method, w._type)
    end
  end

  local function guard(w, method)
    hit(method)
    alive(w, method)
    typecheck(w, method)
  end

  local function apply_style(w, tbl, selector, where)
    if type(tbl) ~= "table" then
      fail("type", "%s expects a table, got %s", where, type(tbl))
    end
    if selector ~= nil then
      if type(selector) ~= "string" then
        fail("type", "%s selector must be a string", where)
      end
      local part, state = selector:match("^([%a_]+):([%a_]+)$")
      if not part then part = selector end
      if not SEL_PART[part] then
        fail("style", "%s unknown selector part '%s'", where, part)
      end
      if state and not SEL_STATE[state] then
        fail("style", "%s unknown selector state '%s'", where, state)
      end
    end
    local n = 0
    for k, v in pairs(tbl) do
      if not STYLE_KEYS[k] then
        fail("style", "%s unknown style key '%s' (the firmware errors with a "
             .. "did-you-mean hint here)", where, tostring(k))
      end
      if k == "text_font" then
        if not (FONT_OK[v] or FONT_PX[v]) then
          fail("style", "%s text_font must be 14/16/18/20/22/24 or small/default/large, got %s",
               where, tostring(v))
        end
      elseif k == "flex_flow" then
        if not FLEX_OK[v] then
          fail("style", "%s flex_flow must be row/column/row_wrap/column_wrap", where)
        end
      elseif k == "text_align" then
        if not TEXT_ALIGN_OK[v] then
          fail("style", "%s text_align must be left/center/right", where)
        end
      elseif k:match("_opa$") then
        if math.type(v) ~= "integer" or v < 0 or v > 255 then
          fail("range", "%s %s must be an integer 0-255, got %s", where, k, tostring(v))
        end
      elseif k:match("_color$") or k == "color" then
        needcolor(v, where .. "." .. k)
      else
        needint(v, k, where)
      end
      n = n + 1
      w._style[k] = v
    end
    inst.style_props = inst.style_props + n
    return n
  end

  local function destroy(w)
    if w._deleted then return end
    w._deleted = true
    inst.widgets_live = inst.widgets_live - 1
    inst.widgets_deleted = inst.widgets_deleted + 1
    for _, c in ipairs(w._children) do destroy(c) end
    local p = w._parent
    if p then
      for i, c in ipairs(p._children) do
        if c == w then table.remove(p._children, i) break end
      end
    end
  end

  function Widget:set_pos(x, y)
    guard(self, "set_pos")
    self._x = needint(x, "x", "set_pos")
    self._y = needint(y, "y", "set_pos")
  end

  function Widget:set_size(w, h)
    guard(self, "set_size")
    self._w = needint(w, "w", "set_size")
    self._h = needint(h, "h", "set_size")
  end

  function Widget:align(name, dx, dy)
    guard(self, "align")
    if not ALIGN_OK[name] then
      fail("align", "unknown alignment '%s'", tostring(name))
    end
    self._align = name
    self._x = needint(dx or 0, "dx", "align")
    self._y = needint(dy or 0, "dy", "align")
  end

  function Widget:parent() hit("parent") alive(self, "parent") return self._parent end
  function Widget:child_count() hit("child_count") alive(self, "child_count") return #self._children end

  function Widget:child(i)
    hit("child") alive(self, "child")
    if math.type(i) ~= "integer" then fail("type", "child(i) needs an integer") end
    return self._children[i]
  end

  function Widget:type() hit("type") alive(self, "type") return self._type end

  function Widget:hidden(v)
    guard(self, "hidden")
    if type(v) ~= "boolean" then fail("type", "hidden() needs a boolean") end
    self._hidden = v
  end

  function Widget:clickable(v)
    guard(self, "clickable")
    if type(v) ~= "boolean" then fail("type", "clickable() needs a boolean") end
  end

  function Widget:bring_to_front() guard(self, "bring_to_front") end
  function Widget:delete() guard(self, "delete") destroy(self) end

  function Widget:set_text(t)
    guard(self, "set_text")
    if type(t) ~= "string" then
      if type(t) == "number" then
        fail("type", "set_text needs a string; pass tostring(n) (LVGL will not coerce)")
      end
      fail("type", "set_text needs a string, got %s", type(t))
    end
    if #t > TEXT_CAP then fail("cap", "text is %d bytes, cap is %d", #t, TEXT_CAP) end
    self._text = t
  end

  function Widget:set_value(v) guard(self, "set_value") self._value = needint(v, "value", "set_value") end

  function Widget:set_range(a, b)
    guard(self, "set_range")
    self._min = needint(a, "min", "set_range")
    self._max = needint(b, "max", "set_range")
  end

  function Widget:set_src(p)
    guard(self, "set_src")
    if type(p) ~= "string" then fail("type", "set_src needs a string path") end
    if p:match("^/") or p:match("^%a:") or p:find("%.%.") then
      fail("fs", "set_src refuses absolute paths, 'A:' and '..': %s", p)
    end
  end

  local function check_points(pts, where)
    if type(pts) ~= "table" then fail("type", "%s needs a table of points", where) end
    if #pts > LINE_POINTS then
      fail("cap", "%s got %d points, cap is %d", where, #pts, LINE_POINTS)
    end
    if #pts < 2 then fail("cap", "%s needs at least 2 points, got %d", where, #pts) end
    for i, p in ipairs(pts) do
      if type(p) ~= "table" then fail("type", "%s point %d is not a table", where, i) end
      local x = p.x or p[1]
      local y = p.y or p[2]
      if x == nil or y == nil then fail("type", "%s point %d needs x and y", where, i) end
      needint(x, ("point[%d].x"):format(i), where)
      needint(y, ("point[%d].y"):format(i), where)
    end
  end

  function Widget:set_points(pts)
    guard(self, "set_points")
    check_points(pts, "set_points")
    self._points = pts
  end

  function Widget:set_checked(v)
    guard(self, "set_checked")
    if type(v) ~= "boolean" then fail("type", "set_checked needs a boolean") end
    self._checked = v
  end

  function Widget:get_checked() guard(self, "get_checked") return self._checked end

  function Widget:set_options(s)
    guard(self, "set_options")
    if type(s) ~= "string" then fail("type", "set_options needs a string") end
    self._options = s
  end

  function Widget:get_selected() guard(self, "get_selected") return self._selected or 0 end

  function Widget:set_color(c)
    guard(self, "set_color")
    self._style.bg_color = needcolor(c, "set_color")
    inst.style_props = inst.style_props + 1
  end

  function Widget:set_border(c, w)
    guard(self, "set_border")
    needcolor(c, "set_border")
    needint(w, "width", "set_border")
    inst.style_props = inst.style_props + 2
  end

  function Widget:set_font_size(s)
    guard(self, "set_font_size")
    if not (FONT_OK[s] or FONT_PX[s]) then
      fail("style", "set_font_size takes small/default/large or 14..24, got %s", tostring(s))
    end
    inst.style_props = inst.style_props + 1
  end

  function Widget:style(tbl, selector)
    guard(self, "style")
    inst.style_calls = inst.style_calls + 1
    apply_style(self, tbl, selector, "style()")
  end

  ---------------------------------------------------------------- factories --
  local function new_widget(kind, parent, isroot)
    if not isroot then
      if inst.widgets_live >= WIDGET_CAP then
        fail("widget_cap", "live widget count would exceed the %d cap (created %d so far)",
             WIDGET_CAP, inst.widgets_created)
      end
      inst.widgets_live = inst.widgets_live + 1
      inst.widgets_created = inst.widgets_created + 1
      if inst.widgets_live > inst.widgets_peak then inst.widgets_peak = inst.widgets_live end
    end
    local w = setmetatable({
      _type = kind, _parent = parent, _children = {}, _style = {},
      _x = 0, _y = 0, _w = 0, _h = 0, _hidden = false, _root = isroot or nil,
    }, Widget)
    if parent then parent._children[#parent._children + 1] = w end
    return w
  end

  local root = new_widget("box", nil, true)

  local function need_parent(p, where)
    if getmetatable(p) ~= Widget then
      fail("type", "%s needs a widget as parent, got %s", where, type(p))
    end
    if p._deleted then fail("deleted", "%s parent has been deleted", where) end
    return p
  end

  -- Factory table form: apply the documented keys, reject anything else.
  local function from_table(kind, t)
    local where = "badge.ui." .. kind .. "{}"
    local parent = need_parent(t.parent, where)
    local w = new_widget(kind, parent)
    local style = {}
    for k, v in pairs(t) do
      if STYLE_KEYS[k] then
        style[k] = v
      elseif not FACTORY_KEYS[k] then
        fail("style", "%s unknown key '%s'", where, tostring(k))
      end
    end
    if next(style) then apply_style(w, style, nil, where) end
    if t.x then w._x = needint(t.x, "x", where) end
    if t.y then w._y = needint(t.y, "y", where) end
    if t.w then w._w = needint(t.w, "w", where) end
    if t.h then w._h = needint(t.h, "h", where) end
    if t.align then
      if not ALIGN_OK[t.align] then fail("align", "%s unknown alignment '%s'", where, tostring(t.align)) end
      w._align = t.align
    end
    if t.align_x then needint(t.align_x, "align_x", where) end
    if t.align_y then needint(t.align_y, "align_y", where) end
    if t.text ~= nil then
      if type(t.text) ~= "string" then fail("type", "%s text must be a string", where) end
      w._text = t.text
    end
    if t.value ~= nil then w._value = needint(t.value, "value", where) end
    if t.min ~= nil then w._min = needint(t.min, "min", where) end
    if t.max ~= nil then w._max = needint(t.max, "max", where) end
    if t.checked ~= nil then w._checked = t.checked end
    if t.hidden ~= nil then w._hidden = t.hidden end
    if t.points ~= nil then check_points(t.points, where) w._points = t.points end
    return w
  end

  local ui = {}

  local function factory(kind, positional)
    return function(a, b, c, d)
      hit("ui." .. kind)
      if type(a) == "table" and getmetatable(a) ~= Widget then
        return from_table(kind, a)
      end
      need_parent(a, "badge.ui." .. kind)
      local w = new_widget(kind, a)
      positional(w, b, c, d, "badge.ui." .. kind)
      return w
    end
  end

  ui.label = factory("label", function(w, text, _, _, where)
    if text ~= nil and type(text) ~= "string" then
      fail("type", "%s(parent, text) needs a string", where)
    end
    w._text = text or ""
  end)

  ui.textarea = factory("textarea", function(w, text, _, _, where)
    if text ~= nil and type(text) ~= "string" then
      fail("type", "%s(parent, text) needs a string", where)
    end
    w._text = text or ""
  end)

  local function box_pos(w, width, height, _, where)
    w._w = needint(width, "width", where)
    w._h = needint(height, "height", where)
  end
  ui.box = factory("box", box_pos)
  ui.button = factory("button", box_pos)

  local function range_pos(w, mn, mx, val, where)
    w._min = needint(mn, "min", where)
    w._max = needint(mx, "max", where)
    w._value = needint(val, "value", where)
  end
  ui.bar = factory("bar", range_pos)
  ui.arc = factory("arc", range_pos)
  ui.slider = factory("slider", range_pos)

  ui.image = factory("image", function(w, src, _, _, where)
    if type(src) ~= "string" then fail("type", "%s(parent, path) needs a string", where) end
    if src:match("^/") or src:match("^%a:") then
      fail("fs", "%s refuses absolute paths and 'A:'", where)
    end
  end)

  ui.line = factory("line", function(w, pts, _, _, where)
    check_points(pts, where)
    w._points = pts
  end)

  ui.switch = factory("switch", function(w, checked, _, _, where)
    if type(checked) ~= "boolean" then fail("type", "%s(parent, checked) needs a boolean", where) end
    w._checked = checked
  end)

  ui.checkbox = factory("checkbox", function(w, text, checked, _, where)
    if type(text) ~= "string" then fail("type", "%s(parent, text, checked) needs a string", where) end
    w._text, w._checked = text, checked or false
  end)

  ui.roller = factory("roller", function(w, options, _, _, where)
    if type(options) ~= "string" then fail("type", "%s(parent, options) needs a string", where) end
    w._options = options
  end)

  ui.screen_width = 320
  ui.screen_height = 240
  ui.theme = THEME

  -------------------------------------------------------------------- leds --
  local led_state = {}
  for i = 1, 6 do led_state[i] = {0, 0, 0} end

  local function chan(v, name)
    if math.type(v) ~= "integer" then
      fail("type", "badge.led.%s needs integer channels (round with math.floor), got %s",
           name, tostring(v))
    end
    if v < 0 or v > 255 then fail("range", "badge.led.%s channel out of 0-255: %d", name, v) end
    return v
  end

  local led = {
    set = function(i, r, g, b)
      hit("led.set")
      if math.type(i) ~= "integer" or i < 1 or i > 6 then
        fail("range", "badge.led.set index must be an integer 1-6 (Lua is 1-based), got %s",
             tostring(i))
      end
      led_state[i] = {chan(r, "set"), chan(g, "set"), chan(b, "set")}
      inst.led_sets = inst.led_sets + 1
      inst.led_pending = inst.led_pending + 1
    end,
    set_all = function(r, g, b)
      hit("led.set_all")
      r, g, b = chan(r, "set_all"), chan(g, "set_all"), chan(b, "set_all")
      for i = 1, 6 do led_state[i] = {r, g, b} end
      inst.led_sets = inst.led_sets + 1
      inst.led_pending = inst.led_pending + 1
    end,
    clear = function()
      hit("led.clear")
      for i = 1, 6 do led_state[i] = {0, 0, 0} end
      inst.led_pending = inst.led_pending + 1
    end,
    show = function()
      hit("led.show")
      inst.led_shows = inst.led_shows + 1
      inst.led_pending = 0
    end,
    count = function() hit("led.count") return 6 end,
  }
  inst.led_state = led_state

  ------------------------------------------------------------------- input --
  local held = {}
  inst.held = held

  local input = {
    BUTTON = BUTTON,
    KIND = KIND,
    is_down = function(b)
      hit("input.is_down")
      if math.type(b) ~= "integer" then
        fail("type", "is_down needs a badge.input.BUTTON constant, got %s", tostring(b))
      end
      return held[b] == true
    end,
    held = function()
      hit("input.held")
      local mask = 0
      for b, v in pairs(held) do if v then mask = mask | (1 << b) end end
      return mask
    end,
  }

  --------------------------------------------------------------------- sys --
  local sys = {
    ms = function() hit("sys.ms") return inst.ms end,
    uptime = function() hit("sys.uptime") return inst.ms // 1000 end,
    log = function(s)
      hit("sys.log")
      inst.log[#inst.log + 1] = tostring(s)
    end,
    random = function(n)
      hit("sys.random")
      local r = nextrand()
      if n == nil then return r end
      if math.type(n) ~= "integer" or n < 1 then
        fail("range", "badge.sys.random(n) needs a positive integer")
      end
      return r % n
    end,
    heap = function()
      hit("sys.heap")
      return math.floor(collectgarbage("count") * 1024) - inst.heap_baseline
    end,
    gc_step = function() hit("sys.gc_step") collectgarbage("step") end,
    version = function() hit("sys.version") return "mock-2026.09.18" end,
    wake_lock = function(v) hit("sys.wake_lock") return true end,
  }
  sys.stats = function()
    hit("sys.stats")
    local used = math.floor(collectgarbage("count") * 1024) - inst.heap_baseline
    return {
      lua_used = used, lua_peak = used, lua_limit = 96 * 1024,
      widgets = inst.widgets_live, uptime_ms = inst.ms, free_heap = 60000,
    }
  end

  ------------------------------------------------------------------- store --
  local store = opts.store or {}
  local function store_key(k)
    if type(k) ~= "string" or not k:match("^[A-Za-z0-9_]+$") or #k > 24 then
      fail("store", "store key must match [A-Za-z0-9_] and be <=24 bytes, got %q", tostring(k))
    end
    return k
  end
  local function store_write(k, v)
    store_key(k)
    if store[k] == nil then
      local n = 0
      for _ in pairs(store) do n = n + 1 end
      if n >= STORE_KEYS then fail("store", "store is limited to %d keys", STORE_KEYS) end
    end
    if type(v) == "string" then
      if #v > STORE_STRLEN then fail("store", "store string is %d bytes, cap is %d", #v, STORE_STRLEN) end
      if v:find("[\r\n]") then fail("store", "store strings may not contain line breaks") end
    elseif math.type(v) ~= "integer" then
      fail("store", "store values must be integers or strings, got %s", tostring(v))
    end
    inst.store_writes = inst.store_writes + 1
    if inst.phase == "tick" then
      inst.store_writes_in_tick = inst.store_writes_in_tick + 1
      fail("flash", "badge.store write during on_tick -- flash writes belong on an "
           .. "explicit action or on_exit, never at frame rate")
    end
    store[k] = v
  end
  inst.store = store

  local storeapi = {
    set = function(k, v) hit("store.set") store_write(k, v) end,
    set_int = function(k, v) hit("store.set_int") store_write(k, v) end,
    set_str = function(k, v) hit("store.set_str") store_write(k, tostring(v)) end,
    get = function(k, d) hit("store.get") store_key(k) local v = store[k] if v == nil then return d end return v end,
    get_int = function(k, d) hit("store.get_int") store_key(k) local v = store[k] if math.type(v) ~= "integer" then return d end return v end,
    get_str = function(k, d) hit("store.get_str") store_key(k) local v = store[k] if type(v) ~= "string" then return d end return v end,
  }

  ---------------------------------------------------------------- the rest --
  local sensor = {
    accel = function()
      hit("sensor.accel")
      if not inst.accel_ok then return nil, "accelerometer unavailable" end
      return 0, 0, 1000
    end,
    shake = function() hit("sensor.shake") return false end,
    tap = function() hit("sensor.tap") return false end,
    orientation = function() hit("sensor.orientation") return "flat_up" end,
  }

  local me = {
    name = function() hit("me.name") return "Tester" end,
    role = function() hit("me.role") return 1 end,
    role_name = function() hit("me.role_name") return "Hacker" end,
    color = function() hit("me.color") return 210, 75, 42 end,
    badge_id = function() hit("me.badge_id") return "MOCK0001" end,
    provisioned = function() hit("me.provisioned") return true end,
  }

  local contacts = {
    count = function() hit("contacts.count") return 0 end,
    get = function(i) hit("contacts.get") return nil, "out of range" end,
  }

  local app = {
    slug = function() hit("app.slug") return opts.slug or "htn_doom" end,
    name = function() hit("app.name") return opts.name or "Badge Doom" end,
    exit = function() hit("app.exit") inst.exited = true end,
  }

  local files = opts.files or {}
  local function fspath(p, where)
    if type(p) ~= "string" then fail("type", "%s needs a string path", where) end
    if p:find("%.%.") or p:match("^/") or p:find("\\") or p:find("%z") then
      fail("fs", "%s rejects '..', absolute paths, backslashes and NUL: %q", where, p)
    end
    return p
  end
  local function fsquota()
    local n = 0
    for _, v in pairs(files) do n = n + #v end
    return n
  end
  local fs = {
    write = function(p, s)
      hit("fs.write") fspath(p, "fs.write")
      if #s > FS_FILE_MAX then fail("fs", "fs.write %d bytes exceeds the %d per-file cap", #s, FS_FILE_MAX) end
      if fsquota() - #(files[p] or "") + #s > FS_QUOTA then fail("fs", "fs quota exceeded") end
      files[p] = s return true
    end,
    append = function(p, s)
      hit("fs.append") fspath(p, "fs.append")
      local cur = files[p] or ""
      if #cur + #s > FS_FILE_MAX then fail("fs", "fs.append exceeds the %d per-file cap", FS_FILE_MAX) end
      files[p] = cur .. s return true
    end,
    read = function(p)
      hit("fs.read") fspath(p, "fs.read")
      local v = files[p]
      if v == nil then return nil, "no such file" end
      return v
    end,
    exists = function(p) hit("fs.exists") fspath(p, "fs.exists") return files[p] ~= nil end,
    remove = function(p) hit("fs.remove") fspath(p, "fs.remove")
      if files[p] == nil then return false end files[p] = nil return true end,
    list = function(d) hit("fs.list") local t = {} for k in pairs(files) do t[#t + 1] = k end
      table.sort(t) return t end,
    mkdir = function(d) hit("fs.mkdir") fspath(d, "fs.mkdir") return true end,
  }
  inst.files = files

  local nfc = {
    enable = function() hit("nfc.enable") return inst.nfc_ok end,
    disable = function() hit("nfc.disable") end,
    card = function() hit("nfc.card") return nil end,
    read_text = function() hit("nfc.read_text") return nil, "no card" end,
    clear = function() hit("nfc.clear") end,
  }

  local radio = {
    enable = function() hit("radio.enable") return inst.radio_ok end,
    disable = function() hit("radio.disable") end,
    send = function(s)
      hit("radio.send")
      if type(s) ~= "string" or #s < 1 or #s > 44 then
        fail("radio", "radio payload must be a 1-44 byte string, got %s", tostring(#s))
      end
      if not inst.radio_ok then return false end
      return true
    end,
    on_recv = function(f) hit("radio.on_recv") end,
    mac = function() hit("radio.mac") return "AA:BB:CC:DD:EE:FF" end,
    dropped = function() hit("radio.dropped") return 0 end,
  }

  local badge = {
    ui = ui, led = led, sensor = sensor, input = input, sys = sys,
    store = storeapi, me = me, contacts = contacts, app = app,
    fs = fs, nfc = nfc, radio = radio,
  }

  -- Reading an undocumented badge.* field must be loud, not nil.
  local function seal(t, path)
    return setmetatable({}, {
      __index = function(_, k)
        local v = t[k]
        if v == nil then
          fail("api", "%s.%s is not in BADGE_GUIDE.md -- invented API", path, tostring(k))
        end
        return v
      end,
      __newindex = function() fail("api", "%s is read-only", path) end,
    })
  end
  for k, v in pairs(badge) do
    if type(v) == "table" then badge[k] = seal(v, "badge." .. k) end
  end
  local sealed = seal(badge, "badge")

  inst.root = root
  inst.reset_frame = function()
    inst.calls = 0
    inst.by_name = {}
  end
  inst.hit = hit
  inst.warn = warn

  return sealed, inst
end

return M
