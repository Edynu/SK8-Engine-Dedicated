name = "NuiTest"

-- Exercises the whole NUI surface: SendNUIMessage in, RegisterNUICallback
-- out, SetNuiFocus, SetNuiFocusKeepInput, and controller navigation of the
-- page itself.
--
-- Try it from the F7 console:
--   nui        toggle the panel (takes focus + cursor)
--   nuihud     show it as a HUD instead - visible, but the skater still
--              moves, because the page asked to keep gameplay input
client_scripts = { "client.lua" }

ui_page = "ui/index.html"
files = {
    "ui/index.html",
    "ui/style.css",
    "ui/app.js",
}
