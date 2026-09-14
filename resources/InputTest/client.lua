-- Control natives, and the hold-to-activate pattern a marker needs.

-- Note DPAD* and LSTICK* are deliberately distinct: the bare names UP/DOWN/
-- LEFT/RIGHT alias the D-PAD only, because steering a skateboard holds the
-- stick almost constantly and that must not read as a D-pad press.
local WATCHED = {
    'A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'START', 'BACK',
    'DPADUP', 'DPADDOWN', 'DPADLEFT', 'DPADRIGHT',
    'LSTICKUP', 'LSTICKDOWN', 'LSTICKLEFT', 'LSTICKRIGHT',
    'KEY_E', 'KEY_SPACE', 'KEY_ENTER',
    'KEY_UP', 'KEY_DOWN', 'KEY_LEFT', 'KEY_RIGHT',
}

RegisterCommand('inputwatch', function(source, args)
    local seconds = tonumber(args[1]) or 8
    Skate.CreateThread(function()
        print(string.format('[input] watching %ds (pad connected: %s)',
            seconds, tostring(IsPadConnected())))
        local deadline = Skate.GetGameTimer() + seconds * 1000
        while Skate.GetGameTimer() < deadline do
            for _, name in ipairs(WATCHED) do
                if IsControlJustPressed(name) then
                    print('  pressed  ' .. name)
                end
                if IsControlJustReleased(name) then
                    print('  released ' .. name)
                end
            end
            -- Wait(0) yields to the next tick, which is the resolution the
            -- edge accumulator works at: nothing can be missed between two
            -- consecutive ticks, but polling slower than this would batch
            -- several presses into one report.
            Skate.Wait(0)
        end
        print('[input] done')
    end)
end)

RegisterCommand('sticks', function()
    print(string.format('[input] L %.2f, %.2f   R %.2f, %.2f   LT %.2f RT %.2f',
        GetControlValue('LX'), GetControlValue('LY'),
        GetControlValue('RX'), GetControlValue('RY'),
        GetControlValue('LT'), GetControlValue('RT')))
end)

-- The pattern a session marker actually needs: hold a direction for a
-- moment to confirm, so brushing the stick while skating past cannot
-- trigger it. Releasing early cancels.
local HOLD_MS = 800

function HoldToActivate(control, onActivated, onProgress)
    Skate.CreateThread(function()
        local heldSince = nil
        while true do
            if IsControlPressed(control) then
                heldSince = heldSince or Skate.GetGameTimer()
                local elapsed = Skate.GetGameTimer() - heldSince
                if onProgress then
                    onProgress(math.min(elapsed / HOLD_MS, 1.0))
                end
                if elapsed >= HOLD_MS then
                    heldSince = nil
                    if onProgress then onProgress(0.0) end
                    onActivated()
                end
            elseif heldSince then
                heldSince = nil
                if onProgress then onProgress(0.0) end
            end
            Skate.Wait(50)
        end
    end)
end

RegisterCommand('holdtest', function()
    print('[input] hold DPAD UP for ' .. HOLD_MS .. 'ms')
    local lastReported = -1
    HoldToActivate('DPADUP', function()
        print('[input] ACTIVATED')
    end, function(progress)
        local step = math.floor(progress * 4)
        if step ~= lastReported then
            lastReported = step
            if progress > 0 then
                print(string.format('  holding %.0f%%', progress * 100))
            end
        end
    end)
end)

print('InputTest loaded - try "inputwatch 8", "sticks", "holdtest"')
