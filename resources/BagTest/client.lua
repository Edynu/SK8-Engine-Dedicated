-- Client half: proves state-bag changes made on the SERVER replicate down to
-- clients. Bags ride the same outbound path as net events, so this is the
-- regression check that path still works end to end.
AddStateBagChangeHandler(nil, nil, function(bagName, key, value, _, replicated)
    print(string.format('client saw bag change: %s.%s = %s (replicated=%s)',
        bagName, key, tostring(value), tostring(replicated)))
end)

RegisterCommand('bags', function()
    print('-> TriggerServerEvent bagtest:run')
    TriggerServerEvent('bagtest:run')
end)

-- Reading a replicated bag back through the same interface the server uses.
RegisterCommand('bagread', function()
    print('GlobalState.round = ' .. tostring(GlobalState.round))
    print('GlobalState.mode  = ' .. tostring(GlobalState.mode))
    local s = GlobalState.scores
    print('GlobalState.scores.beta = ' .. tostring(s and s.beta))
end)
