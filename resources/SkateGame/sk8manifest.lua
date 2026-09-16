name = "SkateGame"

-- A game of S.K.A.T.E.
--
-- One player sets a trick; everyone else has to match it. Miss it and you
-- take a letter. Spell S-K-A-T-E and you are out. Last player standing
-- wins.
--
-- THE SET STAYS WITH WHOEVER KEEPS LANDING IT. Set a trick and land it and
-- you set the next one too, for as long as you keep making them - so a player
-- on a run can spell an opponent out without ever giving up the set. The set
-- only moves when the SETTER misses: lands a different trick than they
-- called, passes, or runs out the clock. Missing your own set costs no
-- letter, only the set.
--
-- ONE ATTEMPT EACH, and only a real trick spends it. An ollie, an air, or
-- hopping back onto the board is something that happens while skating rather
-- than a trick you chose, so it is ignored entirely - otherwise getting
-- moving would fail you before you could try. The callable list is
-- SkateGame.CALLABLE_CLASSES in shared.lua, and it is the same list the menu
-- offers, so the scoreboard can never punish a trick you were never shown.
--
-- The lobby marker is a NATIVE marker (CreateMarker), drawn with retail's own
-- icon, rather than the Markers resource's approximation of one.
--
-- The match state lives on the SERVER (server.lua) and is broadcast to
-- clients, so every player sees the same scoreboard and nobody's client
-- decides whose turn it is. Clients report the tricks they land and draw
-- the UI.
--
-- Console commands (client):
--   skategame          host a game - drops a lobby marker others hold up on
--   skateareahere      put the play area where you are standing
--   skatearea <size>   resize the play area
--   skateleave         leave the game
shared_scripts = { "shared.lua" }
client_scripts = { "client.lua" }
server_scripts = { "server.lua" }

ui_page = "ui/index.html"
files = {
    "ui/index.html",
    "ui/style.css",
    "ui/app.js",
}
