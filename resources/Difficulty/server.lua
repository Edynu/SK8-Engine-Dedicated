-- The server's difficulty lives in server.cfg and is applied by the engine.
-- This only reports it, so an admin can see what clients are being told.
RegisterCommand('difficulty', function()
    print('[difficulty] game_difficulty is ' ..
        GetConvar('game_difficulty', DifficultyGame.DEFAULT))
end)

print('[difficulty] server ready - clients are set by the engine on connect')
