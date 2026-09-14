name = "WaitTest"

-- Proves Skate.Wait works everywhere it is supposed to, and fails cleanly
-- where it cannot.
--
-- Waiting is the first thing anybody writes, and for a long time it only
-- worked inside Skate.CreateThread: everywhere else raised "attempt to yield
-- across a C-call boundary", because C++ called the handler through lua_pcall
-- and a pcall frame cannot be suspended. Handlers now run as scheduler
-- threads (CallYieldable, src/skate3_lua_script_host.cpp), so a wait suspends
-- the handler instead.
--
-- The top-level wait below is the load-order half of the same change: a
-- resource's scripts are queued and then run as ONE thread, so waiting during
-- load suspends loading rather than erroring - and script 2 still cannot start
-- before script 1 has returned.
--
--   waittest        waits inside a command handler
--   waittestevent   waits inside an event handler
client_scripts = { "first.lua", "second.lua" }
server_scripts = { "first.lua", "second.lua" }
