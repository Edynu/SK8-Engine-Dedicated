name = "PropTest"

-- Answers one question: does spawning a prop show up on the RETAIL map?
--
-- Props go into the owned custom world (AppendSpawnedObject), which on the
-- retail map is instantiated but is not the world being rendered. Whether a
-- spawned prop is visible there cannot be settled by reading the code, so
-- this exists to settle it by looking.
--
--   propspawn            drop the test ledge at your feet
--   propspawn <path>     drop a specific .skateobj
client_scripts = { "client.lua" }
