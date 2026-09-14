name = "Probe"

-- Read-only exploration of the running game's memory.
--
-- Retail entry points have to be found in THIS build: the shipped image is
-- encrypted so its strings cannot be read from disk, and the Ghidra decompile
-- in the sk3o tree is a different build whose addresses do not carry over
-- (confirmed - functions that are real here are absent there).
--
-- The front end registers its script bindings under readable names, so those
-- names are in memory and the tables pointing at them can be found from there.
--
--   findstr <text>        addresses whose bytes are <text>
--   findref <0x addr>     word-aligned places holding that value
--   readstr <0x addr>     the string at an address
--   readu32 <0x addr>     the word at an address
--   cacnames              find the CAC/editor binding names in one go
client_scripts = { "bindings.lua", "client.lua" }
