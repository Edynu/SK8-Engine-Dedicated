-- Client half of the game of S.K.A.T.E.: the lobby marker, the play-area
-- box, the trick menu, and reporting what the player landed.
--
-- This side deliberately decides nothing. It reports tricks and renders the
-- state the server sends; whose turn it is and who has which letters are
-- never computed here, so two players can never be looking at two different
-- scoreboards.

local state = nil          -- latest server snapshot
local lobbyMarker = nil
local pendingArea = nil    -- host's in-progress area before it is committed
local menuOpen = false
local tricks = {}          -- selectable tricks, built from retail's own table

-- Forward-declared: the state handler below calls these, and they are
-- defined further down alongside the other marker code.
local ensureLobbyMarker, syncLobbyMarker, maybeAutoOpenTrickMenu
local autoMenuRound = nil  -- set round whose menu has already been offered
local lobbyMarkerAt = nil  -- where the current marker stands, so it is only rebuilt when it moves

local function myId()
    return GetPlayerId()
end

local function me()
    if not state or not state.players then
        return nil
    end
    for _, p in ipairs(state.players) do
        if p.id == myId() then
            return p
        end
    end
    return nil
end

local function joined()
    return me() ~= nil
end

local function isMyTurn()
    return state ~= nil and state.attempting == myId()
end

-- --------------------------------------------------------- the trick menu
--
-- Built from retail's OWN trick table rather than a hand-written list, so
-- the menu can only ever offer tricks the game actually has, and the id we
-- send is the id the game will report back on landing.
--
-- Entries whose name cannot be corroborated are skipped: an untrusted name
-- would show the player one trick and score another. GetTrickName needs
-- guest memory, so this runs after spawn rather than at resource start.
local function buildTrickList()
    if #tricks > 0 then
        return true
    end
    for id = 0, 331 do
        local name, trusted = GetTrickName(id)
        if trusted and name ~= '' then
            local class = GetTrickPatternClass(id)
            -- The SHARED list, so the menu can only ever offer a trick the
            -- server also counts as an attempt (SkateGame.CALLABLE_CLASSES).
            -- Air and the unclassified entries are things that happen, not
            -- tricks you can call.
            local group = SkateGame.CALLABLE_CLASSES[class]
            if group then
                tricks[#tricks + 1] = {
                    id = id,
                    name = name,
                    group = group,
                }
            end
        end
    end
    table.sort(tricks, function(a, b)
        if a.group ~= b.group then
            return a.group < b.group
        end
        return a.name < b.name
    end)
    if #tricks == 0 then
        print('[skate] no trick names could be read yet - try again once ' ..
              'you are skating')
        return false
    end
    return true
end

-- ------------------------------------------------------------------ the UI
--
-- The page shows NOTHING until this player is actually using it. The server
-- broadcasts state to EVERY client, so without this gate a player who never
-- joined would still get a scoreboard and someone else's play-area box drawn
-- over their game - and the resource is meant to be always-on, so that would
-- be permanent furniture rather than something they opted into.
local function uiActive()
    return joined() or pendingArea ~= nil
end

local function pushUi()
    local area = pendingArea or (state and state.area) or nil
    local corners = nil
    if area then
        -- The box is drawn by projecting its eight corners and joining them
        -- in NUI. There is no world-space box drawing to call, and doing it
        -- this way costs eight projections a frame and needs no shader.
        -- A corner behind the camera projects to nil, and an edge with a
        -- missing end is simply dropped rather than drawn to a bogus point.
        corners = {}
        for index, corner in ipairs(SkateGame.AreaCorners(area)) do
            local sx, sy = WorldToScreen(corner.x, corner.y, corner.z)
            corners[index] = (sx ~= nil) and { x = sx, y = sy } or false
        end
    end

    SendNUIMessage({
        action = 'skate',
        visible = uiActive(),
        joined = joined(),
        me = myId(),
        phase = state and state.phase or nil,
        players = state and state.players or {},
        trick = state and state.trick or nil,
        remaining = state and state.remaining or nil,
        winner = state and state.winner or nil,
        log = state and state.log or {},
        myTurn = isMyTurn(),
        -- The setter's two stages are different instructions: pick a trick,
        -- then land the one you picked. The UI cannot tell them apart from
        -- phase alone, because both are PHASE_SET.
        mustLand = state ~= nil and state.phase == SkateGame.PHASE_SET and
            state.setter == myId() and state.trick ~= nil,
        isHost = state ~= nil and state.host == myId(),
        menuOpen = menuOpen,
        word = SkateGame.WORD,
        area = area,
        corners = corners,
        editingArea = pendingArea ~= nil,
        areaSize = area and area.size or nil,
        edges = SkateGame.CORNER_EDGES,
    })
end

-- Sent once when the UI stops being wanted. Without it the page would keep
-- rendering the last frame it was given, since nothing else is coming.
local function hideUi()
    SendNUIMessage({ action = 'skate', visible = false, joined = false })
end

-- RegisterNetEvent, NOT AddEventHandler: these two arrive FROM THE SERVER,
-- and the host drops a network event whose name the resource has not
-- declared (see LuaScriptHost::DispatchEvent's network_only gate). With
-- AddEventHandler the broadcast was silently discarded on every client -
-- which is why a game one player created was invisible to everyone,
-- including the player who created it.
RegisterNetEvent('skategame:state', function(payload)
    state = payload
    syncLobbyMarker()
    maybeAutoOpenTrickMenu()
    if uiActive() then
        pushUi()
    else
        hideUi()
    end
end)

RegisterNetEvent('skategame:message', function(text)
    print('[skate] ' .. tostring(text))
    SendNUIMessage({ action = 'skateToast', text = tostring(text) })
end)

-- Redrawn every frame while in use, because the area box is projected
-- through a camera that moves. Cheap: eight projections and one message -
-- and nothing at all once the player is not in a game, which is the common
-- case for an always-started resource.
Skate.CreateThread(function()
    local wasActive = false
    while true do
        Skate.Wait(0)
        local active = uiActive()
        if active then
            pushUi()
        elseif wasActive then
            hideUi()
        end
        wasActive = active
    end
end)

-- ------------------------------------------------------- reporting tricks

AddEventHandler('skate3:trickLanded', function(trick)
    if not joined() or not isMyTurn() then
        return
    end
    -- An untrusted name is still reported: the ID is what the server matches
    -- on, and dropping the report would stall the round over a cosmetic
    -- problem. The name just travels as whatever it was.
    -- The class travels with the report: the server decides whether this
    -- was an ATTEMPT at all (SkateGame.IsCallable) and has no guest memory
    -- of its own to look it up in.
    local x, y, z = GetEntityCoords(0)
    TriggerServerEvent('skategame:landed', {
        id = trick.id,
        name = trick.name,
        class = trick.patternClass,
        x = x, y = y, z = z,
    })
end)

-- ------------------------------------------------------------- NUI actions

RegisterNUICallback('pickTrick', function(data, cb)
    if data and type(data.id) == 'number' then
        TriggerServerEvent('skategame:declare',
            { id = data.id, name = data.name })
    end
    menuOpen = false
    SetNuiFocus(false)
    pushUi()
    cb({})
end)

RegisterNUICallback('closeMenu', function(data, cb)
    menuOpen = false
    SetNuiFocus(false)
    pushUi()
    cb({})
end)

-- ---------------------------------------------------------------- commands

-- The NATIVE markers, not the Markers resource.
--
-- This used exports.Markers, which drew its own approximation of a marker and
-- lived in a separate lua_State. Two consequences, both gone now: export
-- arguments are marshalled as JSON so a callback could not be passed at all
-- (it arrived as nil), and activation had to come back as a broadcast
-- `markers:activated` event that every marker in every resource also saw, so
-- this had to filter by handle to recognise its own.
--
-- CreateMarker is a resource native running in THIS state, so onUse is just a
-- closure: it can see `joined()` directly, it cannot be confused with another
-- resource's marker, and it may yield (markers run their callback as a
-- scheduler thread). It also draws retail's own extracted icon rather than an
-- imitation of one.
function ensureLobbyMarker(x, y, z)
    if lobbyMarker then
        DestroyMarker(lobbyMarker)
    end
    lobbyMarker = CreateMarker({
        x = x, y = y, z = z,
        -- radius is the ACTIVATION distance, size is how big the icon draws.
        -- Generous radius: the lobby marker sits in the middle of the play
        -- area and players roll past it rather than parking on it.
        radius = 4.0,
        size = 1.5,
        texture = 'challenge_1',
        color = 0xFFFFFFFF,
        hold = 500,
        text = 'Game of S.K.A.T.E.',
        onUse = function()
            if joined() then
                print('[skate] you are already in')
                return
            end
            TriggerServerEvent('skategame:join', { name = GetPlayerName(0) })
        end,
    })
end

-- The marker is owned by the BROADCAST STATE, not by whoever ran
-- `skategame`.
--
-- Creating it only in that command put it on the host's client alone, so
-- nobody else had anything to hold D-pad up on - the game existed on the
-- server and was unreachable. Deriving it from the state every client
-- already receives means a player who joins late, or who reconnects, gets
-- the marker too, without the host having to do anything.
--
-- It is kept during the lobby for players who have ALREADY joined as well:
-- it is what makes the lobby visible on the ground, and its callback
-- already refuses a second join.
function syncLobbyMarker()
    local area = state and state.area or nil
    local want = state ~= nil and state.phase == SkateGame.PHASE_LOBBY and
        area ~= nil
    if want then
        -- Only rebuild when it actually MOVED. Every join rebroadcasts the
        -- state, and tearing the marker down and recreating it each time
        -- would change its handle and flicker it for a lobby that has not
        -- changed at all.
        if lobbyMarker ~= nil and lobbyMarkerAt ~= nil and
            lobbyMarkerAt.x == area.x and lobbyMarkerAt.y == area.y and
            lobbyMarkerAt.z == area.z then
            return
        end
        ensureLobbyMarker(area.x, area.y, area.z)
        lobbyMarkerAt = { x = area.x, y = area.y, z = area.z }
    elseif lobbyMarker then
        DestroyMarker(lobbyMarker)
        lobbyMarker = nil
        lobbyMarkerAt = nil
    end
end

RegisterCommand('skategame', function()
    if not IsPlayerSpawned() then
        print('[skate] not spawned yet')
        return
    end
    local x, y, z = GetEntityCoords(0)
    -- No marker is placed here: the server's broadcast puts one on EVERY
    -- client (syncLobbyMarker), this one included. Placing it here as well
    -- would just be a second owner of the same object.
    --
    -- The host joins their own game, and the area defaults to where the
    -- lobby was dropped so a game is playable without touching the editor.
    TriggerServerEvent('skategame:join', { name = GetPlayerName(0) })
    TriggerServerEvent('skategame:setArea',
        { x = x, y = y, z = z, size = SkateGame.DEFAULT_AREA_SIZE })
    print('[skate] lobby marker dropped - others hold D-pad up on it to join')
    print('[skate] "skatestart" when everyone is in')
end)

RegisterCommand('skatestart', function()
    TriggerServerEvent('skategame:start')
end)

RegisterCommand('skateleave', function()
    TriggerServerEvent('skategame:leave')
    if lobbyMarker then
        DestroyMarker(lobbyMarker)
        lobbyMarker = nil
        lobbyMarkerAt = nil
    end
    state = nil
    hideUi()
end)

-- Opens the trick menu. Returns false when the trick list is not readable
-- yet, so an automatic attempt can simply try again on the next broadcast
-- instead of latching a menu that never appeared.
local function openTrickMenu()
    if not buildTrickList() then
        return false
    end
    menuOpen = true
    -- Sent ONCE, on its own message, rather than inside the per-frame state
    -- push: it is a couple of hundred entries, and re-encoding it 60 times
    -- a second to say nothing new would cost more than the rest of this
    -- resource put together.
    SendNUIMessage({ action = 'skateMenu', tricks = tricks })
    -- The menu takes focus AND the cursor: it is a list to click, and the
    -- pad is needed for steering, not for walking a list mid-run.
    SetNuiFocus(true, true)
    pushUi()
    return true
end

-- Opens the menu for the setter automatically, once per set.
--
-- The setter is the only player with a decision to make before anything can
-- happen, and making them type `skatetrick` to find that out meant a round
-- could sit on the clock while they read the scoreboard. Keyed on the
-- server's round counter so closing the menu is respected - the next
-- broadcast, 250ms later, must not reopen it - while a genuinely new set
-- offers it again.
--
-- Not latched when the list is unreadable: trick names come from guest
-- memory and are only available once the player is skating, so a failed
-- attempt retries on the next broadcast rather than silently never opening.
function maybeAutoOpenTrickMenu()
    if state == nil or state.phase ~= SkateGame.PHASE_SET then
        return
    end
    if state.setter ~= myId() then
        return
    end
    -- Already called one: the job now is to LAND it, not to pick again.
    if state.trick ~= nil then
        return
    end
    if menuOpen or autoMenuRound == state.round then
        return
    end
    if openTrickMenu() then
        autoMenuRound = state.round
    end
end

RegisterCommand('skatetrick', function()
    if not isMyTurn() or state.phase ~= SkateGame.PHASE_SET then
        print('[skate] you are not setting right now')
        return
    end
    if state.trick ~= nil then
        print('[skate] you already called ' .. tostring(state.trick.name) ..
              ' - land it')
        return
    end
    openTrickMenu()
end)

RegisterCommand('skatepass', function()
    TriggerServerEvent('skategame:pass')
end)

-- The play-area editor. Deliberately commands rather than a drag handle:
-- the area is a box around a point the host skates to, so "stand here, this
-- big" is the whole interaction, and it works while the pad is steering.
RegisterCommand('skateareahere', function()
    if not IsPlayerSpawned() then
        print('[skate] not spawned yet')
        return
    end
    local x, y, z = GetEntityCoords(0)
    local size = (pendingArea and pendingArea.size)
        or (state and state.area and state.area.size)
        or SkateGame.DEFAULT_AREA_SIZE
    pendingArea = { x = x, y = y, z = z, size = size }
    print(string.format('[skate] area preview at (%.1f, %.1f, %.1f) size %.0f' ..
        ' - "skateareaok" to commit', x, y, z, size))
end)

RegisterCommand('skatearea', function(source, args)
    local size = tonumber(args[1])
    if size == nil then
        print('[skate] usage: skatearea <half-size in units>')
        return
    end
    size = math.max(SkateGame.MIN_AREA_SIZE,
                    math.min(size, SkateGame.MAX_AREA_SIZE))
    if pendingArea == nil then
        if not IsPlayerSpawned() then
            print('[skate] not spawned yet')
            return
        end
        local x, y, z = GetEntityCoords(0)
        pendingArea = { x = x, y = y, z = z, size = size }
    else
        pendingArea.size = size
    end
    print(string.format('[skate] area preview size %.0f - "skateareaok" to' ..
        ' commit', size))
end)

RegisterCommand('skateareaok', function()
    if pendingArea == nil then
        print('[skate] nothing to commit - "skateareahere" first')
        return
    end
    TriggerServerEvent('skategame:setArea', pendingArea)
    pendingArea = nil
end)

RegisterCommand('skateareacancel', function()
    pendingArea = nil
    print('[skate] area preview discarded')
end)

-- A client that starts after a game is already set up would otherwise show
-- an empty scoreboard until the next state change.
Skate.CreateThread(function()
    Skate.Wait(500)
    TriggerServerEvent('skategame:sync')
end)

print('[skate] SkateGame ready - "skategame" to host')
