-- NUI round-trip test.
--
-- The page is always loaded (an iframe exists for every resource with a
-- ui_page), but it draws nothing until told to. That is the normal shape
-- for NUI: visibility is the page's business, focus is the script's.

local mode = 'hidden'  -- 'hidden' | 'menu' | 'hud'

local function apply(next_mode)
    mode = next_mode

    -- Tell the page what to draw. Arrives in JS as an ordinary window
    -- message: window.addEventListener('message', e => e.data).
    SendNUIMessage({
        action = 'setMode',
        mode = mode,
        player = GetPlayerName(GetPlayerId()),
        playerId = GetPlayerId(),
    })

    if mode == 'menu' then
        -- Focus + cursor: the panel owns mouse and pad, the skater stops.
        SetNuiFocusKeepInput(false)
        SetNuiFocus(true, true)
    elseif mode == 'hud' then
        -- Focus WITH gameplay still live. The pad drives the page's focused
        -- element (D-pad + A) while the skater keeps riding.
        SetNuiFocusKeepInput(true)
        SetNuiFocus(true, true)
    else
        SetNuiFocus(false, false)
        SetNuiFocusKeepInput(false)
    end
    print('nui mode: ' .. mode)
end

RegisterCommand('nui', function()
    apply(mode == 'menu' and 'hidden' or 'menu')
end)

RegisterCommand('nuihud', function()
    apply(mode == 'hud' and 'hidden' or 'hud')
end)

-- The page announces itself once it can receive messages. Until this
-- arrives, a SendNUIMessage would land before any listener existed.
RegisterNUICallback('ready', function(data, cb)
    print('NUI page ready: ' .. tostring(data and data.href))
    cb({ resource = GetCurrentResourceName() })
end)

-- Page -> script. The page POSTs to https://<resource>/<name>; `data` is
-- the decoded body and `cb` answers the page's fetch().
RegisterNUICallback('close', function(_, cb)
    apply('hidden')
    cb({ ok = true })
end)

RegisterNUICallback('teleport', function(data, cb)
    local x = tonumber(data and data.x) or 0.0
    local y = tonumber(data and data.y) or 0.0
    local z = tonumber(data and data.z) or 0.0
    print(string.format('nui teleport -> %.1f, %.1f, %.1f', x, y, z))
    SetPlayerCoords(x, y, z)
    apply('hidden')
    cb({ ok = true })
end)

-- Answers with a value the page displays, rather than just an ack. Errors
-- here (GetEntityCoords fails when the player has no position yet) are
-- caught host-side and the page's fetch still settles, so its await never
-- hangs.
RegisterNUICallback('whereAmI', function(_, cb)
    local x, y, z = GetEntityCoords(0)
    cb({ x = x, y = y, z = z })
end)

print('NuiTest loaded - type "nui" in the F7 console')
