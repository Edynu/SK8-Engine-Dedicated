-- Console front end for the engine's prop system. No state lives here: the
-- server owns the list and the client's prop service streams it.

local DEFAULT_LOD = 300.0

local function resolve(filter)
    local found = ListProps(filter or 'largeQuarterPipe', 1)
    if #found == 0 then
        print(string.format('[props] no package matches "%s"',
            tostring(filter)))
        return nil
    end
    return found[1]
end

RegisterCommand('propplace', function(source, args)
    if not IsPlayerSpawned() then
        print('[props] not spawned yet')
        return
    end
    local path = resolve(args[1])
    if path == nil then return end
    local x, y, z = GetEntityCoords(0)
    -- networked: the server assigns the id and streams it back, so this
    -- client receives it the same way every other client does.
    CreateProp(path, x, y, z, tonumber(args[2]) or DEFAULT_LOD, true)
    print(string.format('[props] placed %s for everyone', path))
end)

RegisterCommand('proplocal', function(source, args)
    if not IsPlayerSpawned() then
        print('[props] not spawned yet')
        return
    end
    local path = resolve(args[1])
    if path == nil then return end
    local x, y, z = GetEntityCoords(0)
    local handle, message = CreateProp(path, x, y, z,
        tonumber(args[2]) or DEFAULT_LOD, false)
    print(string.format('[props] local %s handle=%s (%s)', path,
        tostring(handle), tostring(message)))
end)

RegisterCommand('proplist', function(source, args)
    local found = ListProps(args[1] or '', 25)
    for _, path in ipairs(found) do
        print('  ' .. path)
    end
    print(string.format('[props] %d match(es)', #found))
end)

print('[props] ready - "propplace" places a prop for everyone')
