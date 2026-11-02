-- From a state saved 10 frames before the movie's end (~/emu/sh-tas/end.state): holds the movie's last
-- input (fire) until the boss falls, then presses Start every 300 frames, with a screenshot before each press.
local home = os.getenv("HOME")
local dir = home .. "/emu/sh-tas/after/"
local HOLD, EXTRA = 900, 36000
emu.limitframerate(false)
client.speedmode(800)
savestate.load(home .. "/emu/sh-tas/end.state")
client.unpause()
for i = 1, EXTRA do
  if i % 300 == 0 then client.screenshot(dir .. string.format("%06d.png", i)) end
  if i <= HOLD then joypad.set({["P1 X"] = true, ["P1 L2"] = true, ["P1 R2"] = true})
  elseif i % 300 < 3 then joypad.set({["P1 Start"] = true}) end
  emu.frameadvance()
end
client.exit()
