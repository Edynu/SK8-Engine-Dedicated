-- Calls into ExportLib across a separate Lua state.
RegisterNetEvent('exporttest:run', function()
    local sum = exports.ExportLib:add(20, 22)
    print('exports.ExportLib:add(20, 22) = ' .. tostring(sum))

    local text, ok = exports.ExportLib:describe({ name = 'skate', items = { 1, 2, 3 } })
    print('describe -> ' .. tostring(text) .. ' / ' .. tostring(ok))

    TriggerClientEvent('exporttest:result', source, sum, text)
end)
