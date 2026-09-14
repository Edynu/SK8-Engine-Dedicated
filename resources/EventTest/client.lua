-- Client half of the event round-trip test.
--
--   ping          -> fires a local event (no network involved)
--   pingserver    -> TriggerServerEvent, server answers with TriggerClientEvent
--
-- The reply proves the full loop: client -> server -> client, with argument
-- values surviving the crossing in both directions.
print(DescribeSide('client loaded'))

-- Local events need no RegisterNetEvent - that gate only applies to events
-- arriving from the network.
AddEventHandler('eventtest:local', function(message, count)
    print(string.format('local event: %s (count=%s)', tostring(message), tostring(count)))
end)

-- Anything the server may send has to be declared, or it is refused.
RegisterNetEvent('eventtest:pong', function(message, payload)
    print(string.format('server replied: %s', tostring(message)))
    if type(payload) == 'table' then
        print(string.format('  payload.from   = %s', tostring(payload.from)))
        print(string.format('  payload.number = %s', tostring(payload.number)))
        print(string.format('  payload.list   = %s, %s',
            tostring(payload.list and payload.list[1]),
            tostring(payload.list and payload.list[2])))
    end
end)

RegisterCommand('ping', function()
    TriggerEvent('eventtest:local', 'hello from the same VM', 42)
end)

RegisterCommand('pingserver', function()
    print('-> TriggerServerEvent eventtest:ping')
    TriggerServerEvent('eventtest:ping', 'hello from the client', {
        nested = true,
        values = { 1, 2, 3 },
    })
end)
