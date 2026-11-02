-- Per-loop trace of the Silent Hill TAS (TAS Testing Plan, Phase 2). The core has no memory hooks, so this
-- samples every video frame and writes a line when the game's loop counter or the map changes. Fields:
--   movie frame, gameStateCounter, gameState, sysState, g_VBlanks, g_DeltaTime, RNG seed, map index,
--   in-game timer (raw), controller 0 data (hex, 8 bytes), player position X Y Z (hex, 12 bytes),
--   eventFlags CRC-like sum.
-- Addresses: configs/USA/sym.*.txt. Ends with END at the movie's last frame.
local LAST, RAM = 185305, "MainRAM"
local home = os.getenv("HOME")
local name = os.getenv("TRACE_OUT") or "trace.log"
local out = io.open(home .. "/emu/sh-tas/" .. name, "w")
local function a(x) return x % 0x200000 end
local function u32(x) return memory.read_u32_le(a(x), RAM) end
local function s32(x) return memory.read_s32_le(a(x), RAM) end
local function hex(x, n)
  local t, s = memory.readbyterange(a(x), n, RAM), {}
  for i = 0, n - 1 do s[#s + 1] = string.format("%02x", t[i] or 0) end
  return table.concat(s)
end
local SYS = 0x800B9FC0
local PLAYER = SYS + 0x4C
local GAME = 0x800BC728
emu.limitframerate(false)
client.speedmode(800)
local prev
while true do
  local f = emu.framecount()
  local save, pad = u32(0x80024D48), u32(0x80024D4C)
  local map, igt, sum = -1, 0, 0
  if save >= 0x80000000 and save < 0x80200000 then
    map = memory.read_s8(a(save + 0xA4), RAM)
    igt = s32(save + 0x250)
    for i = 0, 51 do sum = (sum * 31 + u32(save + 0x168 + i * 4)) % 0xFFFFFFFB end
  end
  local cnt = s32(SYS + 0x1C)
  local key = cnt .. ":" .. map .. ":" .. s32(GAME + 0x594)
  if key ~= prev then
    prev = key
    out:write(string.format("%d %d %d %d %d %d %08x %d %d %s %s %08x\n", f, cnt, s32(GAME + 0x594), s32(SYS + 8),
      s32(0x800B5C34), s32(0x800B5CC0), u32(0x80022C90), map, igt,
      (pad >= 0x80000000 and pad < 0x80200000) and hex(pad, 8) or "-", hex(PLAYER + 0x18, 12), sum))
  end
  if f % 6000 == 0 then out:flush() end
  if f >= LAST - 1 then out:write("END\n"); out:close(); client.exit() end
  emu.frameadvance()
end
