name = "ScoreTest"

-- Verifies the retail score and session-marker reads against the running game.
--
-- These natives read guest memory at offsets recovered from the recompiled
-- bindings rather than calling retail (see skate3_retail_score.h for why). That
-- makes them cheap and thread-safe, but it also means a wrong offset produces a
-- plausible-looking number rather than an error - so they have to be checked
-- against what the HUD actually shows.
--
--   scorewatch    live score/multiplier print while you skate
--   scoreonce     one snapshot
--   scoreblock    the raw block addresses, for probing the rest
client_scripts = { "client.lua" }
