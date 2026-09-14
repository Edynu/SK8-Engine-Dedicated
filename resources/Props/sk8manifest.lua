name = "Props"

-- Console commands for the networked prop system.
--
-- The system itself is ENGINE code, not this resource: the server owns the
-- prop list (skate3_prop_store.h) and the client streams what is near it on
-- its own thread (skate3_prop_service.h). Which geometry a client loads runs
-- against the player's position every second and has to keep working while
-- scripts restart, so it does not belong in Lua. This resource only exposes
-- the natives to a person typing in the console.
--
--   propplace [filter] [lod]   place a NETWORKED prop here, for everyone
--   proplocal [filter] [lod]   place a LOCAL one, gone on disconnect
--   proplist [filter]          search the 652-package library
--
-- Scripts call the native directly:
--   CreateProp(path, x, y, z, lod, networked)
client_scripts = { "client.lua" }
