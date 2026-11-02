-- Dumps the options and savegame at the frame the replay starts from (the first map load), for trace_to_dem.py.
local F, RAM = 2109, "MainRAM"
local path = os.getenv("HOME") .. "/emu/sh-tas/start.hex"
emu.limitframerate(false)
while emu.framecount() < F do emu.frameadvance() end
local function hex(x, n)
  local t, s = memory.readbyterange(x % 0x200000, n, RAM), {}
  for i = 0, n - 1 do s[#s + 1] = string.format("%02x", t[i] or 0) end
  return table.concat(s)
end
local out = io.open(path, "w")
out:write(hex(0x800BC728, 0x38) .. "\n" .. hex(0x800BCA34, 636) .. "\n")
out:close()
client.exit()
