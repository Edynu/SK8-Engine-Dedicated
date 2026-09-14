-- Inspecting and overriding difficulty from script.
--
-- ENFORCEMENT IS NOT HERE. The engine fetches game_difficulty from the server
-- (GET /api/settings) on connect and applies it, retrying until the game's
-- memory exists to write - see docs/online_session.md. That is deliberate: a
-- rule everyone is playing by must not depend on a resource choosing to apply
-- it.
--
-- What a game mode legitimately wants is a TEMPORARY change for a scenario -
-- dropping to easy for a big-jump section, say - and that is what this is for.

RegisterCommand('difficulty', function(source, args)
    if args[1] == nil then
        local name, index = GetGameDifficulty()
        if name == nil then
            print('[difficulty] not known yet - are you in the world?')
            return
        end
        print(string.format('[difficulty] %s (index %d)', name, index))
        return
    end
    local ok, message = SetGameDifficulty(args[1])
    print('[difficulty] ' .. tostring(message))
end)

print('[difficulty] ready - "difficulty" to check, "difficulty easy" to override')
