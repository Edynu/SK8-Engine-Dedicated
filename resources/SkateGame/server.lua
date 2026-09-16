-- Authoritative match state for a game of S.K.A.T.E.
--
-- WHY THE SERVER OWNS THIS. Whose turn it is, who has which letters, and
-- who won are facts every player must agree on. Deciding them on one
-- player's client would make the host's opinion the truth and leave the
-- others' scoreboards to drift; deciding them per-client would let two
-- clients disagree about who is out. So the server holds the state and
-- broadcasts it, and clients only ever report what they did.
--
-- WHAT IS AND IS NOT TRUSTED. Clients report the tricks they land, and the
-- server takes their word for it - it has no way to observe a trick itself.
-- A determined player could therefore claim a trick they did not do. That
-- is the right trade for a game among friends, and it is worth stating
-- plainly rather than implying a guarantee that is not there. What the
-- server DOES enforce is everything it can see: turn order, who is allowed
-- to report right now, which trick counts, the clock, and whether the
-- landing was inside the play area.

local state = {
    phase = SkateGame.PHASE_LOBBY,
    host = nil,
    area = nil,
    players = {},      -- [id] = { name, letters, out }
    order = {},        -- join order, which is also the rotation
    setter = nil,
    trick = nil,       -- { id, name } the trick that must be matched
    pending = {},      -- ids that still owe an attempt this round
    attempting = nil,  -- id currently on the clock
    deadline = nil,    -- Skate.GetGameTimer() milliseconds
    winner = nil,
    -- Incremented every time a set begins. The client uses it to open the
    -- trick menu ONCE per set for whoever is setting: without a round to key
    -- on, a menu the setter deliberately closed would be reopened by the
    -- very next broadcast (they arrive every 250ms while the clock runs).
    round = 0,
    log = {},
}

local function say(text)
    print('[skate] ' .. text)
    state.log[#state.log + 1] = text
    while #state.log > 6 do
        table.remove(state.log, 1)
    end
end

local function nameOf(id)
    local p = state.players[id]
    if p and p.name then
        return p.name
    end
    return 'player ' .. tostring(id)
end

-- The wire form. Deliberately a whole snapshot rather than a diff: it is a
-- handful of players and fits in one event, and a diff protocol would be a
-- source of desync for no measurable gain at this size.
local function broadcast()
    local players = {}
    for _, id in ipairs(state.order) do
        local p = state.players[id]
        if p then
            players[#players + 1] = {
                id = id,
                name = p.name,
                letters = p.letters,
                out = p.out,
                isSetter = (id == state.setter),
                isAttempting = (id == state.attempting),
            }
        end
    end
    local remaining = nil
    if state.deadline then
        remaining = math.max(0,
            math.floor((state.deadline - Skate.GetGameTimer()) / 1000))
    end
    TriggerClientEvent('skategame:state', -1, {
        phase = state.phase,
        host = state.host,
        area = state.area,
        players = players,
        setter = state.setter,
        attempting = state.attempting,
        trick = state.trick,
        remaining = remaining,
        winner = state.winner,
        round = state.round,
        log = state.log,
    })
end

local function activePlayers()
    local active = {}
    for _, id in ipairs(state.order) do
        local p = state.players[id]
        if p and not p.out then
            active[#active + 1] = id
        end
    end
    return active
end

local function finishIfWon()
    if state.phase == SkateGame.PHASE_LOBBY then
        return false
    end
    local active = activePlayers()
    if #active > 1 then
        return false
    end
    state.phase = SkateGame.PHASE_OVER
    state.winner = active[1]
    state.setter, state.attempting, state.deadline, state.trick =
        nil, nil, nil, nil
    state.pending = {}
    if state.winner then
        say(nameOf(state.winner) .. ' WINS')
    else
        say('game over - nobody left standing')
    end
    return true
end

-- Hands the set to the player after `from` in join order, skipping anyone
-- who is out. Real S.K.A.T.E.: missing your own set costs no letter, the
-- set just moves on.
local function nextSetter(from)
    local active = activePlayers()
    if #active == 0 then
        return nil
    end
    local startIndex = 1
    for index, id in ipairs(active) do
        if id == from then
            startIndex = index + 1
            break
        end
    end
    return active[((startIndex - 1) % #active) + 1]
end

local beginSet

local function beginMatch()
    state.phase = SkateGame.PHASE_MATCH
    state.pending = {}
    for _, id in ipairs(activePlayers()) do
        if id ~= state.setter then
            state.pending[#state.pending + 1] = id
        end
    end
    if #state.pending == 0 then
        -- A set with nobody left to answer it: keep the same setter rather
        -- than rotating, so the set does not quietly travel round an empty
        -- table while waiting for a second player.
        beginSet(state.setter)
        return
    end
    state.attempting = state.pending[1]
    state.deadline = Skate.GetGameTimer() + SkateGame.MATCH_SECONDS * 1000
    broadcast()
end

beginSet = function(setter)
    if setter == nil then
        finishIfWon()
        broadcast()
        return
    end
    state.phase = SkateGame.PHASE_SET
    state.round = state.round + 1
    state.setter = setter
    state.attempting = setter
    state.trick = nil
    state.pending = {}
    state.deadline = Skate.GetGameTimer() + SkateGame.SET_SECONDS * 1000
    say(nameOf(setter) .. ' is setting')
    broadcast()
end

-- Moves to the next player owing an attempt, or ends the round.
local function advanceMatch()
    table.remove(state.pending, 1)
    if #state.pending == 0 then
        if finishIfWon() then
            broadcast()
            return
        end
        -- THE SETTER KEEPS THE SET. This used to rotate on every completed
        -- round, which meant landing your set handed the next one away - so
        -- setting well was punished and the set travelled round the table
        -- regardless of who could actually skate.
        --
        -- Real S.K.A.T.E.: you keep calling tricks for as long as you keep
        -- landing them, and the set only moves when YOU miss your own. Those
        -- paths already rotate (a wrong trick, a pass, or the clock running
        -- out during a set all call nextSetter), so the rule lives in exactly
        -- one place per outcome rather than being split across both.
        --
        -- It follows that a player on a run can spell out an opponent without
        -- ever giving up the set, which is the point of the game.
        beginSet(state.setter)
        return
    end
    state.attempting = state.pending[1]
    state.deadline = Skate.GetGameTimer() + SkateGame.MATCH_SECONDS * 1000
    broadcast()
end

local function giveLetter(id, reason)
    local p = state.players[id]
    if not p then
        return
    end
    p.letters = p.letters + 1
    if SkateGame.IsOut(p.letters) then
        p.out = true
        say(nameOf(id) .. ' is OUT (' .. SkateGame.LettersOf(p.letters) ..
            ') - ' .. reason)
    else
        say(nameOf(id) .. ' gets ' .. SkateGame.WORD:sub(p.letters, p.letters) ..
            ' (' .. SkateGame.LettersOf(p.letters) .. ') - ' .. reason)
    end
end

-- ------------------------------------------------------------------ joining

RegisterNetEvent('skategame:join', function(payload)
    local id = source
    if state.players[id] then
        return
    end
    if state.phase == SkateGame.PHASE_SET or
       state.phase == SkateGame.PHASE_MATCH then
        -- Joining mid-game would hand the newcomer either a free pass or an
        -- arbitrary handicap. Neither is a game, so they wait it out.
        TriggerClientEvent('skategame:message', id,
            'a game is already running - wait for it to finish')
        return
    end
    state.players[id] = {
        name = (payload and payload.name) or GetPlayerName(id) or
               ('player ' .. id),
        letters = 0,
        out = false,
    }
    state.order[#state.order + 1] = id
    if state.host == nil then
        state.host = id
    end
    say(nameOf(id) .. ' joined')
    broadcast()
end)

RegisterNetEvent('skategame:leave', function()
    local id = source
    if not state.players[id] then
        return
    end
    local wasSetter = (state.setter == id)
    local wasAttempting = (state.attempting == id)
    say(nameOf(id) .. ' left')
    state.players[id] = nil
    for index, existing in ipairs(state.order) do
        if existing == id then
            table.remove(state.order, index)
            break
        end
    end
    for index, existing in ipairs(state.pending) do
        if existing == id then
            table.remove(state.pending, index)
            break
        end
    end
    if state.host == id then
        state.host = state.order[1]
    end
    -- They may have been exactly who everyone was waiting for.
    if state.phase == SkateGame.PHASE_SET or
       state.phase == SkateGame.PHASE_MATCH then
        if finishIfWon() then
            broadcast()
            return
        end
        if wasSetter then
            beginSet(nextSetter(id))
            return
        end
        if wasAttempting then
            state.attempting = state.pending[1]
            if state.attempting == nil then
                -- The last player owing an answer left. The round is over
                -- without the setter having missed anything, so the set stays
                -- with them - same reason as advanceMatch. Rotating here would
                -- let someone take the set off a player simply by quitting.
                beginSet(state.setter)
            else
                state.deadline =
                    Skate.GetGameTimer() + SkateGame.MATCH_SECONDS * 1000
                broadcast()
            end
            return
        end
    end
    broadcast()
end)

RegisterNetEvent('skategame:setArea', function(area)
    if source ~= state.host then
        return
    end
    if state.phase ~= SkateGame.PHASE_LOBBY and
       state.phase ~= SkateGame.PHASE_OVER then
        TriggerClientEvent('skategame:message', source,
            'the play area can only be changed between games')
        return
    end
    if type(area) ~= 'table' or type(area.x) ~= 'number' then
        return
    end
    state.area = {
        x = area.x, y = area.y, z = area.z,
        size = math.max(SkateGame.MIN_AREA_SIZE,
                        math.min(area.size or SkateGame.DEFAULT_AREA_SIZE,
                                 SkateGame.MAX_AREA_SIZE)),
    }
    say('play area set')
    broadcast()
end)

RegisterNetEvent('skategame:start', function()
    if source ~= state.host then
        return
    end
    if state.phase == SkateGame.PHASE_SET or
       state.phase == SkateGame.PHASE_MATCH then
        return
    end
    if #state.order < 2 then
        TriggerClientEvent('skategame:message', source,
            'need at least two players')
        return
    end
    for _, id in ipairs(state.order) do
        state.players[id].letters = 0
        state.players[id].out = false
    end
    state.winner = nil
    state.log = {}
    state.round = 0
    say('game on - ' .. #state.order .. ' players')
    -- The first setter is RANDOM rather than the host, so that hosting is
    -- not an advantage. After that it rotates in join order.
    beginSet(state.order[math.random(#state.order)])
end)

-- ------------------------------------------------------------------- playing

-- The setter declares what they are about to do. Matched later by scorable
-- ID, never by name: the name is a lookup into retail's metadata table and
-- can come back untrusted, whereas the id is what the game itself recorded.
RegisterNetEvent('skategame:declare', function(trick)
    if state.phase ~= SkateGame.PHASE_SET or source ~= state.setter then
        return
    end
    if type(trick) ~= 'table' or type(trick.id) ~= 'number' then
        return
    end
    state.trick = { id = trick.id, name = trick.name or '?' }
    say(nameOf(source) .. ' calls ' .. state.trick.name)
    broadcast()
end)

RegisterNetEvent('skategame:landed', function(trick)
    local id = source
    if type(trick) ~= 'table' or type(trick.id) ~= 'number' then
        return
    end
    -- Only the player actually on the clock can score. Without this, anyone
    -- skating nearby lands tricks into someone else's turn.
    if id ~= state.attempting then
        return
    end
    if state.area and type(trick.x) == 'number' then
        if not SkateGame.InArea(state.area, trick.x, trick.y, trick.z) then
            TriggerClientEvent('skategame:message', id,
                'that was outside the play area')
            return
        end
    end

    -- ONE ATTEMPT, and only a real trick spends it.
    --
    -- A landing that is not a callable trick - hopping on the board, an
    -- ollie, an air, anything unclassified - is ignored entirely rather than
    -- counted as a miss. Otherwise simply getting moving would fail a player
    -- before they could try, and the menu never offered those tricks in the
    -- first place (SkateGame.CALLABLE_CLASSES is the same list both sides
    -- use).
    if not SkateGame.IsCallable(trick.class) then
        return
    end

    if state.phase == SkateGame.PHASE_SET then
        -- The setter's FIRST real trick is their set. If they called one and
        -- landed something else, the set is gone - they do not get to keep
        -- trying until the clock runs out, and calling a trick has to mean
        -- something. It costs no letter: missing your own set never does.
        if state.trick and state.trick.id ~= trick.id then
            say(nameOf(id) .. ' landed ' .. (trick.name or '?') .. ', not ' ..
                state.trick.name .. ' - set lost')
            beginSet(nextSetter(id))
            return
        end
        state.trick = { id = trick.id, name = trick.name or '?' }
        say(nameOf(id) .. ' set ' .. state.trick.name)
        beginMatch()
        return
    end

    if state.phase == SkateGame.PHASE_MATCH then
        if not state.trick then
            return
        end
        -- Their first real trick has to BE the set trick. Landing anything
        -- else is the miss - that is what makes it one attempt rather than
        -- unlimited tries inside the clock.
        if state.trick.id ~= trick.id then
            giveLetter(id, 'landed ' .. (trick.name or '?') .. ', not ' ..
                state.trick.name)
            if finishIfWon() then
                broadcast()
                return
            end
            advanceMatch()
            return
        end
        say(nameOf(id) .. ' matched ' .. state.trick.name)
        advanceMatch()
    end
end)

-- A player can give up their attempt rather than wait out the clock.
RegisterNetEvent('skategame:pass', function()
    local id = source
    if id ~= state.attempting then
        return
    end
    if state.phase == SkateGame.PHASE_SET then
        say(nameOf(id) .. ' passed the set')
        beginSet(nextSetter(id))
    elseif state.phase == SkateGame.PHASE_MATCH then
        giveLetter(id, 'passed')
        if finishIfWon() then
            broadcast()
            return
        end
        advanceMatch()
    end
end)

-- A client that just started asks for the current picture rather than
-- waiting for the next change.
RegisterNetEvent('skategame:sync', function()
    broadcast()
end)

-- --------------------------------------------------------------------- clock

Skate.CreateThread(function()
    while true do
        Skate.Wait(250)
        if state.deadline and state.attempting then
            if Skate.GetGameTimer() >= state.deadline then
                local id = state.attempting
                if state.phase == SkateGame.PHASE_SET then
                    -- Missing your own set costs nothing but the set.
                    say(nameOf(id) .. ' ran out of time setting')
                    beginSet(nextSetter(id))
                elseif state.phase == SkateGame.PHASE_MATCH then
                    giveLetter(id, 'ran out of time')
                    if finishIfWon() then
                        broadcast()
                    else
                        advanceMatch()
                    end
                end
            else
                -- The countdown is part of what clients render, so it has to
                -- keep being published even when nothing else changes.
                broadcast()
            end
        end
    end
end)

RegisterCommand('skatestate', function()
    print('[skate] phase=' .. state.phase ..
          ' setter=' .. tostring(state.setter) ..
          ' attempting=' .. tostring(state.attempting) ..
          ' trick=' .. tostring(state.trick and state.trick.name))
    for _, id in ipairs(state.order) do
        local p = state.players[id]
        print(string.format('  %d %-16s %-5s %s', id, p.name,
            SkateGame.LettersOf(p.letters), p.out and 'OUT' or ''))
    end
end)

print('[skate] SkateGame server ready')
