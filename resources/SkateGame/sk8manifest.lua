name = "SkateGame"

-- A game of S.K.A.T.E.
--
-- One player sets a trick; everyone else has to match it. Miss it and you
-- take a letter. Spell S-K-A-T-E and you are out. Last player standing
-- wins.
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
