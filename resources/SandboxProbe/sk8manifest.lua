name = "SandboxProbe"

-- Reports what the Lua sandbox actually allows, from inside a real resource
-- state.
--
-- A resource on a CLIENT is downloaded from whatever server the player just
-- joined and then executed, so the standard library it gets handed is a
-- security boundary, not a convenience. That boundary is enforced in C++
-- (RestrictStandardLibrary, src/skate3_lua_script_host.cpp) and is easy to
-- weaken by accident - adding a library, reordering openlibs, or upgrading
-- Lua all silently hand some of it back.
--
-- `sandboxcheck` prints the verdict. Expect every line to say "blocked".
client_scripts = { "client.lua" }
server_scripts = { "client.lua" }
