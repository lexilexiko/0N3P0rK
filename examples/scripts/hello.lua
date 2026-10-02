-- 0N3P0rK — hello.lua (Lua 5.4)
--
-- The smallest thing that works: copy it to the card as
--     /0N3P0rK/scripts/hello.lua
-- then open Menu → <>/SCRIPTS, highlight it and press ENT.
-- Its output appears in the SCRIPTS console, or in the REPL if you paste it.
--
-- Try the REPL too: press TAB on the SCRIPTS screen and type
--     print("oink " .. heap())
-- one line at a time — variables you create stay alive between lines.

print("hello from 0N3P0rK")
print(_VERSION)
print("free heap: " .. heap() .. " bytes")