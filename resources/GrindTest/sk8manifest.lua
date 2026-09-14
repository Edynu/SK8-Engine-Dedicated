name = "GrindTest"

-- Verifies grind detection, length and reward against the running game.
--
-- "Is grinding" comes from retail: its ScoreModule tracks which typed collector
-- is live, and a grind is that word reading Grind. Length does NOT - retail has
-- no duration or distance field, so both are measured here. That makes them
-- worth checking against the game rather than trusting: a wrong collector-state
-- offset would read as "never grinding" or "always grinding", and a wrong
-- reward offset yields a plausible number that is simply not the score.
--
--   grindwatch   live readout while you grind, printed on change
--   grindlog     print a line for every completed grind (the event)
client_scripts = { "client.lua" }
