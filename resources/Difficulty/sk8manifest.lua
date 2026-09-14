name = "Difficulty"

-- Inspecting difficulty, and overriding it for a scenario.
--
-- The server's value is enforced by the ENGINE (docs/online_session.md); this
-- resource does not apply it. Kept for the console commands and for a game mode
-- that wants a temporary change.
--
-- Difficulty is the SERVER's decision, not each player's.
--
-- A game of S.K.A.T.E. is only fair if everyone is playing the same game, and
-- difficulty changes how tricks land. So the server holds one value and every
-- client is set to it on join; retail's own Difficulty Settings row is hidden
-- (skate3_menu_settings_skip) so a player cannot quietly diverge.
--
-- Set it in server.cfg:
--
--   game_difficulty "hardcore"
--
-- Console:
--   difficulty            what this client is on
--   difficulty <name>     set it locally (a game mode may do this per scenario)
shared_scripts = { "shared.lua" }
client_scripts = { "client.lua" }
server_scripts = { "server.lua" }
