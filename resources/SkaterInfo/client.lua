-- Player state readout.
--
-- The board flags come from retail's own state (the provider bytes:
-- IsOffboard is 673 || 675, IsAirOffboard is 674, plus retail's ground
-- predicate). Speed is DERIVED by differentiating the sampled board
-- position - nothing has located retail's own velocity yet.

local function line()
    local x, y, z = 0.0, 0.0, 0.0
    if IsPlayerSpawned() then
        x, y, z = GetEntityCoords(0)
    end
    return string.format(
        'pos %7.1f %6.1f %7.1f | speed %5.1f | ground=%-5s air=%-5s offboard=%-5s airOffboard=%-5s',
        x, y, z, GetPlayerSpeed(),
        tostring(IsSkaterOnGround()), tostring(IsSkaterInAir()),
        tostring(IsOffBoard()), tostring(IsAirOffBoard()))
end

RegisterCommand('state', function()
    print(line())
    local vx, vy, vz = GetPlayerVelocity()
    print(string.format('  velocity %.2f, %.2f, %.2f', vx, vy, vz))
end)

-- Samples for a few seconds so the flags can be watched changing while you
-- skate, rather than caught in one snapshot.
RegisterCommand('statewatch', function(source, args)
    local seconds = tonumber(args[1]) or 5
    Skate.CreateThread(function()
        print(string.format('[state] watching for %ds', seconds))
        local deadline = Skate.GetGameTimer() + seconds * 1000
        while Skate.GetGameTimer() < deadline do
            print('  ' .. line())
            Skate.Wait(500)
        end
        print('[state] done')
    end)
end)

print('SkaterInfo loaded - try "state" or "statewatch 5"')
