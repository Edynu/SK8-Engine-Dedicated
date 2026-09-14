name = "Markers"

-- World markers with a hold-to-activate prompt, the shape Skate 3's own
-- mission sign-up markers use: an icon floating at a world point, the
-- marker's name when you get close, and a D-pad-up prompt you HOLD to
-- confirm.
--
-- Nothing here touches retail's challenge system - see client.lua for why
-- that is deliberate rather than a shortcut.
--
-- Console commands:
--   markers        list every marker and its distance
--   markerhere N   drop a marker where the player is standing
shared_scripts = { "shared.lua" }
client_scripts = { "client.lua" }

ui_page = "ui/index.html"
files = {
    "ui/index.html",
    "ui/style.css",
    "ui/app.js",
}
