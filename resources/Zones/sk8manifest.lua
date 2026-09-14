name = "Zones"

-- PolyZone-style spatial triggers, in pure Lua. No engine work: it samples
-- the position the client already publishes and does its own containment
-- maths, so zones cost one distance check per zone per sample.
--
-- Console commands:
--   zones          list every zone and whether the player is inside it
--   zonehere NAME  drop a 10-unit radius zone at the player's feet
--   zonemark       move the demo marker zone to where the player is standing
shared_scripts = { "shared.lua" }
client_scripts = { "client.lua" }

ui_page = "ui/index.html"
files = {
    "ui/index.html",
    "ui/style.css",
    "ui/app.js",
}
