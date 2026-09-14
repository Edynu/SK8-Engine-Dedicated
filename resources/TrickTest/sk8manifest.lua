name = "TrickTest"

-- Reads out every trick the game records, so the trick NAMES can be checked
-- against what the HUD shows before anything is built on top of them.
--
-- Console commands:
--   trickdump [first] [last]   dump retail's trick-metadata table
--   tricks                     tally of landed tricks seen so far
client_scripts = { "client.lua" }
