-- 0N3P0rK — example boot script (Lua 5.4)
--
-- Copy this file to the SD card as:
--     /0N3P0rK/scripts/boot.lua
--
-- It runs once at boot with a 3 second budget, inside a 48 KB memory cap, and
-- its output appears on the screen as a toast and on the serial console.
-- If it fails or overruns, the toast says so and the firmware carries on —
-- a script can never take the device down.

print("0N3P0rK lua " .. _VERSION)
print("free heap " .. heap() .. " bytes")

-- Closures and the string library: the real language, not a toy subset.
local function greet(name, n)
  local out = {}
  for i = 1, n do out[i] = name:upper() .. "!" end
  return table.concat(out, " ")
end

print(greet("oink", 3))

local fib = { 0, 1 }
for i = 3, 12 do fib[i] = fib[i - 1] + fib[i - 2] end
print("fib(12) = " .. fib[12])

print("boot script done")
