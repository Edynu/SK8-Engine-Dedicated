-- Teleport, and the harness that settles which route we keep.
--
-- Three routes exist and they are NOT equivalent:
--   SetEntityPosition(0,...)   moves only the skateboard's physics body.
--                              The rider does not come; if you are on the
--                              board it fights back and resolves at spawn.
--   TeleportToSessionMarker()  retail's own reposition. Rider and camera
--                              come along, but the destination is whatever
--                              the session marker already is.
--   SetPlayerCoords(x,y,z)     posts retail's cMsgTeleport with an arbitrary
--                              destination. The candidate.
--
-- Commands:
--   teleport [x y z]   board-only move (the old behaviour, kept to compare)
--   tp x y z           SetPlayerCoords - the candidate route
--   gomarker           session-marker reposition
--   tptest             the edge-case sweep below, reporting measured landings

-- Waits until the local skater exists in the world, then runs `fn`.
--
-- Teleporting before this point does NOT fail loudly: the engine accepts
-- the request and posts it, and then the spawn sequence runs and puts the
-- player back where it wanted them. Measured directly - a teleport fired at
-- connect logged "post sent" and the player still ended up at the default
-- spawn. Anything that moves a player on join goes through here.
function WhenSpawned(fn, timeout_ms)
    Skate.CreateThread(function()
        local deadline = Skate.GetGameTimer() + (timeout_ms or 60000)
        while not IsPlayerSpawned() do
            if Skate.GetGameTimer() > deadline then
                print('[Teleport] gave up waiting for spawn')
                return
            end
            Skate.Wait(100)
        end
        fn()
    end)
end

local function pos()
    local ok, x, y, z = pcall(GetEntityCoords, 0)
    if not ok then return nil end
    return x, y, z
end

local function report(label, wantX, wantY, wantZ)
    local x, y, z = pos()
    if x == nil then
        print(string.format('  %-22s NO POSITION (not spawned)', label))
        return
    end
    if wantX == nil then
        print(string.format('  %-22s at %.1f, %.1f, %.1f', label, x, y, z))
        return
    end
    local dx, dy, dz = x - wantX, y - wantY, z - wantZ
    local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
    print(string.format('  %-22s at %.1f, %.1f, %.1f  (off by %.2f)',
        label, x, y, z, dist))
end

RegisterCommand('teleport', function(source, args)
    if args[1] == nil then
        local x, y, z = pos()
        if x == nil then print('[Teleport] no position yet') return end
        SetEntityPosition(0, x, y + 5.0, z)
        print(string.format('[Teleport] board -> %.2f, %.2f, %.2f', x, y + 5.0, z))
        return
    end
    SetEntityPosition(0, tonumber(args[1]) or 0.0, tonumber(args[2]) or 0.0,
        tonumber(args[3]) or 0.0)
end)

RegisterCommand('tp', function(source, args)
    local x = tonumber(args[1]) or 0.0
    local y = tonumber(args[2]) or 0.0
    local z = tonumber(args[3]) or 0.0
    SetPlayerCoords(x, y, z)
    print(string.format('[Teleport] player -> %.2f, %.2f, %.2f', x, y, z))
end)

-- The join pattern the game mode will use: queue a destination now, land
-- there as soon as the player is actually in the world.
RegisterCommand('tpwhenready', function(source, args)
    local x = tonumber(args[1]) or 0.0
    local y = tonumber(args[2]) or 0.0
    local z = tonumber(args[3]) or 0.0
    print(string.format('[Teleport] queued until spawn -> %.1f, %.1f, %.1f', x, y, z))
    WhenSpawned(function()
        SetPlayerCoords(x, y, z)
        Skate.Wait(1200)
        report('landed', x, y, z)
    end)
end)

RegisterCommand('gomarker', function()
    TeleportToSessionMarker()
end)

-- The sweep. Each case teleports, waits for the engine to settle, and
-- reports where the player actually ended up, so "it worked" is a measured
-- distance rather than an impression.
RegisterCommand('tptest', function()
    Skate.CreateThread(function()
        local ox, oy, oz = pos()
        if ox == nil then
            print('[tptest] no position yet - spawn first')
            return
        end
        print(string.format('[tptest] origin %.1f, %.1f, %.1f', ox, oy, oz))

        -- 1. Does a plain teleport land where it was asked to?
        print('[tptest] 1. short hop')
        local ax, ay, az = ox + 20.0, oy, oz + 20.0
        SetPlayerCoords(ax, ay, az)
        Skate.Wait(1200)
        report('after short hop', ax, ay, az)

        -- 2. Two requests one after another: the newer must win, and the
        --    older must not resurface a tick later.
        print('[tptest] 2. two in quick succession')
        SetPlayerCoords(ox + 60.0, oy, oz)
        SetPlayerCoords(ox, oy, oz + 60.0)
        Skate.Wait(1200)
        report('after double', ox, oy, oz + 60.0)

        -- 3. Airborne: teleport high, then teleport again while still
        --    falling. This is the case the board-only route resolves at
        --    spawn on, so it is the one that matters most.
        print('[tptest] 3. airborne re-teleport')
        SetPlayerCoords(ox, oy + 40.0, oz)
        Skate.Wait(300)
        report('mid-fall', nil)
        SetPlayerCoords(ox + 15.0, oy + 5.0, oz)
        Skate.Wait(1500)
        report('after airborne tp', ox + 15.0, oy + 5.0, oz)

        -- 4. A long hop. Note this only checks where it LANDS - whether
        --    it stays there is case 6's job.
        print('[tptest] 4. long distance')
        SetPlayerCoords(ox + 400.0, oy + 10.0, oz + 400.0)
        Skate.Wait(2500)
        report('after long hop', ox + 400.0, oy + 10.0, oz + 400.0)

        -- 5. Rapid fire: ten requests across consecutive frames. Proves the
        --    one-shot pending flag cannot wedge or lose the last one.
        print('[tptest] 5. rapid fire')
        for i = 1, 10 do
            SetPlayerCoords(ox + i, oy + 2.0, oz + i)
            Skate.Wait(0)
        end
        Skate.Wait(1500)
        report('after rapid fire', ox + 10.0, oy + 2.0, oz + 10.0)

        -- 6. Persistence. Landing somewhere is not the same as STAYING
        --    there: retail has its own out-of-bounds logic that returns the
        --    player to the session marker several seconds later, so a check
        --    taken a second after the teleport reports success for a
        --    destination the player is about to be dragged out of. Measured:
        --    a hop to (807, 80, 63.6) read as landed at 4s and was back at
        --    spawn by 12s, while a nearby in-bounds hop held indefinitely.
        print('[tptest] 6. persistence (far vs near)')
        SetPlayerCoords(ox + 400.0, oy + 10.0, oz + 400.0)
        Skate.Wait(2000)
        report('far, after 2s', ox + 400.0, oy + 10.0, oz + 400.0)
        Skate.Wait(12000)
        report('far, after 14s', ox + 400.0, oy + 10.0, oz + 400.0)

        SetPlayerCoords(ox + 40.0, oy, oz + 40.0)
        Skate.Wait(2000)
        report('near, after 2s', ox + 40.0, oy, oz + 40.0)
        Skate.Wait(12000)
        report('near, after 14s', ox + 40.0, oy, oz + 40.0)

        -- 7. Home again, so the sweep leaves the player where it found them.
        SetPlayerCoords(ox, oy, oz)
        Skate.Wait(1200)
        report('back home', ox, oy, oz)
        print('[tptest] done')
    end)
end)
