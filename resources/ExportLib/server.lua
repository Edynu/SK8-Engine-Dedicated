-- A library resource: exposes functions for other resources to call.
exports('add', function(a, b)
    return a + b
end)

exports('describe', function(tbl)
    return string.format('got %s with %d items', tostring(tbl.name), #tbl.items), true
end)

AddEventHandler('onResourceStart', function(name)
    print('lifecycle: ' .. tostring(name) .. ' started')
end)

AddEventHandler('onResourceStop', function(name)
    print('lifecycle: ' .. tostring(name) .. ' stopped')
end)
