RunThreadDemo('client')

-- Also reachable from the F7 console, to re-run on demand.
RegisterCommand('threads', function()
    RunThreadDemo('client')
end)
