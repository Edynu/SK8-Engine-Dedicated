name = "Threads"

-- Exercises the coroutine scheduler: Skate.CreateThread, Skate.Wait and
-- Skate.TimeOut, on both sides. The client's scheduler is driven once per
-- rendered frame, the server's once per loop iteration, so the same script
-- shape has different resolution on each - which is what the drift numbers
-- printed below are for.
shared_scripts = { "shared.lua" }
client_scripts = { "client.lua" }
server_scripts = { "server.lua" }
