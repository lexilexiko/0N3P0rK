-- 0N3P0rK — bench.lua (Lua 5.4)
--
-- A tiny on-device diagnostic: how much heap the VM itself costs, how fast the
-- interpreter runs, and how much a string-heavy workload moves the heap.
--
-- Copy to /0N3P0rK/scripts/bench.lua and run it from Menu → <>/SCRIPTS.
-- It stays well inside the 48 KB budget and the run budget.

local before = heap()
print("heap before strings: " .. before)

-- Build a table of strings and join it: exercises table + string + concat.
local t = {}
for i = 1, 200 do t[i] = "item" .. i end
local joined = table.concat(t, ",")
print("joined length: " .. #joined)

-- A tight arithmetic loop: gives a rough interpreter speed number.
local sum = 0
local t0 = millis()
for i = 1, 20000 do sum = sum + (i % 7) end
local dt = millis() - t0
print("20000 iters in " .. dt .. " ms  (sum " .. sum .. ")")

print("heap now: " .. heap())
print("delta: " .. (heap() - before))