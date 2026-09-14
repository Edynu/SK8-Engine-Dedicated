name = "BailTest"

-- Checks that a crash and deliberately stepping off are told apart.
--
-- That distinction is the whole point: IsOffBoard cannot make it, so a game
-- mode built on IsOffBoard alone counts walking to the spawn point as a failed
-- attempt. Two signals are combined - the ScoreModule collector reading
-- Offboard, and retail asking for a wipeout - and the classification is only
-- as good as that combination, so it is worth checking by hand.
--
--   bailwatch    live state, printed on change
--
-- Events print automatically. Test BOTH: slam into something (expect "BAILED"),
-- then stop and step off the board with the walk button (expect "stepped off").
client_scripts = { "client.lua" }
