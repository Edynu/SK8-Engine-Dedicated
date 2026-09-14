-- Server half of the event round-trip test. Runs in skate3_dedicated.
print(DescribeSide('server loaded'))

RegisterNetEvent('eventtest:ping', function(message, options)
    -- `source` is the sender's player id - the role the relay assigned when
    -- that client connected. Set as a global for the duration of the call,
    -- exactly like FiveM.
    local sender = source
    print(string.format('client %s sent: %s', tostring(sender), tostring(message)))
    if type(options) == 'table' then
        print(string.format('  options.nested = %s', tostring(options.nested)))
    end

    -- Addressed reply: only this player receives it.
    TriggerClientEvent('eventtest:pong', sender, 'private pong, just for you', {
        from = 'skate3_dedicated',
        yourId = sender,
    })

    -- -1 means "every client", matching the FiveM convention.
    TriggerClientEvent('eventtest:pong', -1, 'broadcast pong to everyone', {
        from = 'skate3_dedicated',
        number = 1337,
        list = { 'alpha', 'beta' },
    })
end)

-- Server-side player registry, fed by the relay's own presence tracking.
RegisterNetEvent('eventtest:who', function()
    local ids = GetSkaters()
    print(string.format('%d skater(s) connected', #ids))
    for _, id in ipairs(ids) do
        local s = GetSkater(id)
        if s and s.valid then
            print(string.format('  id=%d at %.1f, %.1f, %.1f (map %s)',
                s.id, s.x, s.y, s.z, s.mapHash))
        elseif s then
            print(string.format('  id=%d (no position yet, map %s)', s.id, s.mapHash))
        end
    end
    -- Answer the caller so this is observable from the client too.
    TriggerClientEvent('eventtest:whoresult', source, #ids)
end)
