-- scripts.lua -- scripted button timelines for the simulator.
--
-- An event is {t = <tick index>, press = "UP"} or {t = ..., release = "UP"}.
-- Helpers build them so a run reads like the demo it is meant to imitate.

local S = {}

local function hold(ev, t, btn, dur)
  ev[#ev + 1] = {t = t, press = btn}
  ev[#ev + 1] = {t = t + dur, release = btn}
  return t + dur
end

local function tap(ev, t, btn)
  ev[#ev + 1] = {t = t, press = btn}
  ev[#ev + 1] = {t = t + 1, release = btn}
  return t + 1
end

S.hold, S.tap = hold, tap

-- The 60-second demo: the motion every phase is measured against.
-- At 20 ms nominal cadence, 600 ticks is ~12 s of badge time; the shape is
-- what matters, not the duration.
function S.demo()
  local ev = {}
  hold(ev, 10, "UP", 60)            -- walk down the opening corridor
  hold(ev, 75, "RIGHT", 25)         -- steady turn
  hold(ev, 105, "UP", 40)
  tap(ev, 150, "A")                 -- fire
  tap(ev, 158, "A")
  tap(ev, 166, "A")
  hold(ev, 180, "LEFT", 45)         -- longer turn
  hold(ev, 185, "UP", 55)           -- walk while turning: worst case for render
  ev[#ev + 1] = {t = 240, press = "B"}      -- strafe modifier down
  hold(ev, 245, "LEFT", 30)
  hold(ev, 280, "RIGHT", 30)
  ev[#ev + 1] = {t = 315, release = "B"}
  hold(ev, 320, "RIGHT", 90)        -- fast sustained spin
  hold(ev, 420, "UP", 70)
  tap(ev, 470, "A")
  tap(ev, 480, "A")
  hold(ev, 500, "DOWN", 30)         -- back up
  hold(ev, 540, "LEFT", 40)
  hold(ev, 545, "UP", 45)
  return ev
end

-- Nothing moves: measures the idle floor, which frame-skipping should drive to
-- near zero native calls.
function S.idle()
  return {}
end

-- Sustained fast turning only: the pathological case for column churn.
function S.spin()
  local ev = {}
  hold(ev, 5, "RIGHT", 400)
  return ev
end

-- Pure forward movement: every column changes height every frame.
function S.charge()
  local ev = {}
  hold(ev, 5, "UP", 400)
  return ev
end

-- Restart path: exercises reset_game and any rebuild.
function S.restart()
  local ev = {}
  hold(ev, 10, "UP", 40)
  tap(ev, 60, "START")
  hold(ev, 70, "UP", 40)
  tap(ev, 120, "START")
  return ev
end

S.all = {demo = S.demo, idle = S.idle, spin = S.spin,
         charge = S.charge, restart = S.restart}

return S
