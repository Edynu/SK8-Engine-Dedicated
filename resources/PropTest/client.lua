-- Props on the retail map.
--
-- Runtime-spawned objects now draw over the retail world WITHOUT enabling
-- the sandbox: turning that on swaps the visual world out entirely (test B),
-- so the renderer instead draws the owned base map and individual spawned
-- objects under separate gates, and only the latter appears in Vanilla Mode.
--
-- VISUAL ONLY so far - these have no collision on the retail map, because
-- Vanilla Mode leaves retail's own collision authoritative and never
-- registers owned static geometry. Expect to skate straight through them.
--
--   proplist [filter]    search the 652-package library
--   propspawn [filter]   spawn the first match at your feet

-- A large quarter pipe: if this renders at all it is unmissable, which is
-- the point - a subtle prop would leave "did it work?" ambiguous.
local DEFAULT_FILTER = 'largeQuarterPipe'

RegisterCommand('proplist', function(source, args)
    local filter = args[1] or ''
    local found = ListProps(filter, 25)
    if #found == 0 then
        print(string.format('[prop] nothing matches "%s"', filter))
        return
    end
    print(string.format('[prop] %d match(es) for "%s":', #found, filter))
    for _, path in ipairs(found) do
        print('  ' .. path)
    end
end)

RegisterCommand('propspawn', function(source, args)
    if not IsPlayerSpawned() then
        print('[prop] not spawned yet')
        return
    end
    local filter = args[1] or DEFAULT_FILTER
    local found = ListProps(filter, 1)
    if #found == 0 then
        print(string.format('[prop] no package matches "%s" - try "proplist"',
            filter))
        return
    end
    local path = found[1]
    local x, y, z = GetEntityCoords(0)
    local ok, message = SpawnProp(path, x, y, z)
    print(string.format('[prop] %s', path))
    print(string.format('[prop] -> %s', tostring(message)))
    if ok then
        print('[prop] should be visible now - visual only, no collision yet')
    end
end)

print('[prop] PropTest ready - "proplist rail", then "propspawn"')
