-- The polling loop that turns zone containment into enter/exit events, plus
-- a demo marker zone that shows a NUI prompt while you stand in it.
--
-- One thread samples the player position and tests every registered zone.
-- That is deliberately simple: with a handful of zones the cost is a few
-- distance checks per sample, and anything cleverer (spatial hashing) would
-- be optimising a cost we cannot yet measure.

local registered = {}

-- How often containment is re-tested. 100ms is well inside human reaction
-- time for walking into a marker, and an order of magnitude cheaper than
-- testing every frame for something that cannot change meaningfully between
-- samples.
local SAMPLE_MS = 100

function Zones.Add(zone)
    registered[#registered + 1] = zone
    return zone
end

function Zones.All()
    return registered
end

local function fire(handlers, zone)
    for _, fn in ipairs(handlers) do
        -- Guarded so one bad handler cannot stop the loop and take every
        -- other zone's events with it.
        local ok, err = pcall(fn, zone)
        if not ok then
            print(string.format('[Zones] %s handler error: %s', zone.name,
                tostring(err)))
        end
    end
end

Skate.CreateThread(function()
    while true do
        if IsPlayerSpawned() then
            local x, y, z = GetEntityCoords(0)
            for _, zone in ipairs(registered) do
                local now = zone:contains(x, y, z)
                if now ~= zone.inside then
                    zone.inside = now
                    fire(now and zone.enterHandlers or zone.exitHandlers, zone)
                end
            end
        end
        Skate.Wait(SAMPLE_MS)
    end
end)

-- --------------------------------------------------------------------
-- Prompt UI
-- --------------------------------------------------------------------
-- Deliberately NOT focused: this is a passive HUD, so it must never take
-- the pointer. Only a resource that calls SetNuiFocus receives clicks, so
-- leaving focus alone keeps the prompt click-through.

local function showPrompt(text)
    SendNUIMessage({ action = 'prompt', visible = true, text = text })
end

local function hidePrompt()
    SendNUIMessage({ action = 'prompt', visible = false })
end

-- --------------------------------------------------------------------
-- The demo: a marker you can stand in
-- --------------------------------------------------------------------
-- Placed at the session marker's own position, which is where the game puts
-- you on spawn and where it returns you if you stray out of bounds.

local marker = Zones.Add(Zones.Sphere('session-marker', 406.9, 70.0, -336.4, 8.0, {
    -- Clipped vertically so standing on something directly above the spawn
    -- does not count as being in it.
    minY = 66.0,
    maxY = 76.0,
}))

marker:onEnter(function(zone)
    print('[Zones] entered ' .. zone.name)
    showPrompt('You are at the session marker')
end)

marker:onExit(function(zone)
    print('[Zones] left ' .. zone.name)
    hidePrompt()
end)

-- --------------------------------------------------------------------
-- Console commands
-- --------------------------------------------------------------------

RegisterCommand('zones', function()
    if not IsPlayerSpawned() then
        print('[Zones] not spawned yet')
        return
    end
    local x, y, z = GetEntityCoords(0)
    print(string.format('[Zones] player at %.1f, %.1f, %.1f', x, y, z))
    for _, zone in ipairs(registered) do
        print(string.format('  %-18s %-6s inside=%s  flat distance %.1f',
            zone.name, zone.kind, tostring(zone.inside),
            Zones.FlatDistance(zone, x, z)))
    end
end)

RegisterCommand('zonehere', function(source, args)
    if not IsPlayerSpawned() then
        print('[Zones] not spawned yet')
        return
    end
    local name = args[1] or ('zone' .. tostring(#registered + 1))
    local radius = tonumber(args[2]) or 10.0
    local x, y, z = GetEntityCoords(0)
    local zone = Zones.Add(Zones.Sphere(name, x, y, z, radius))
    zone:onEnter(function(self)
        print('[Zones] entered ' .. self.name)
        showPrompt('Inside ' .. self.name)
    end)
    zone:onExit(function(self)
        print('[Zones] left ' .. self.name)
        hidePrompt()
    end)
    print(string.format('[Zones] created %s r=%.1f at %.1f, %.1f, %.1f',
        name, radius, x, y, z))
end)

-- Moves the demo marker zone to wherever the player is standing, so the
-- enter/exit edges can be exercised without walking across the map.
RegisterCommand('zonemark', function()
    if not IsPlayerSpawned() then
        print('[Zones] not spawned yet')
        return
    end
    local x, y, z = GetEntityCoords(0)
    marker:moveTo(x, y, z)
    marker.minY, marker.maxY = y - 4.0, y + 6.0
    print(string.format('[Zones] marker moved to %.1f, %.1f, %.1f', x, y, z))
end)

print('Zones loaded - try "zones", "zonehere NAME", "zonemark"')
