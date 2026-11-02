-- Logs the map and in-game timer every 600 frames while the movie plays (sync check).
local out = io.open(os.getenv("HOME") .. "/emu/sh-tas/synccheck.log", "w")
out:write("hash " .. tostring(gameinfo.getromhash()) .. "\n"); out:flush()
local function rd(a) return memory.read_u32_le(a % 0x200000, "MainRAM") end
emu.limitframerate(false)
client.speedmode(800)
while true do
  local f = emu.framecount()
  if f % 600 == 0 then
    local s = rd(0x80024D48)
    local map, timer = -1, 0
    if s >= 0x80000000 and s < 0x80200000 then
      map = memory.read_s8((s + 0xA4) % 0x200000, "MainRAM")
      timer = rd(s + 0x250)
    end
    out:write(string.format("%d map=%d igt=%.1f\n", f, map, timer / 4096))
    out:flush()
  end
  if f % 600 == 0 and f >= 183000 or f == 185300 then
    client.screenshot(os.getenv("HOME") .. "/emu/sh-tas/end_" .. f .. ".png")
  end
  if f >= 185305 then out:write("END\n"); out:close(); client.exit() end
  emu.frameadvance()
end
