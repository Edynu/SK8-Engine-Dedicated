-- State bags: shared, replicated state without hand-writing an event per field.
AddStateBagChangeHandler(nil, nil, function(bagName, key, value, _, replicated)
    print(string.format('bag change: %s.%s = %s (replicated=%s)',
        bagName, key, tostring(value), tostring(replicated)))
end)

RegisterNetEvent('bagtest:run', function()
    GlobalState.round = 3
    GlobalState.mode = 'SKATE'

    Player(source).state.letters = 'SKA'
    Player(source).state.ready = true

    -- Read them back through the same interface.
    print('GlobalState.round   = ' .. tostring(GlobalState.round))
    print('GlobalState.mode    = ' .. tostring(GlobalState.mode))
    print('Player(' .. source .. ').state.letters = ' .. tostring(Player(source).state.letters))
    print('Player(' .. source .. ').state.ready   = ' .. tostring(Player(source).state.ready))

    -- Tables survive too.
    GlobalState.scores = { alpha = 1, beta = 2 }
    local s = GlobalState.scores
    print('GlobalState.scores.beta = ' .. tostring(s and s.beta))
end)
