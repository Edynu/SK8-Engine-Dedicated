-- CreateMarker(type, x, y, z, text, fn) - a world marker you hold D-pad up
-- to activate.
--
-- `fn` works only for a marker created inside THIS resource. Another
-- resource's callback cannot survive the export boundary (arguments are
-- marshalled as JSON), so a game mode listens for the local event instead:
--
--   AddEventHandler('markers:activated', function(id, marker) ... end)
--
-- comparing `id` against what CreateMarker returned.
--
-- WHY THIS IS OURS RATHER THAN RETAIL'S
--
-- Skate 3 draws its mission sign-up markers through its own challenge
-- system: the icon and the "Sign Up" prompt are elements of a Flash movie
-- (converted to EA APT) that the front end positions from a projected world
-- point. Driving that would mean reverse-engineering the challenge manager
-- AND keeping it alive - which is exactly what we want gone, since
-- retail's missions write into the player's save and have no business in an
-- online session. A marker system built on retail's would therefore depend
-- on the thing it is meant to replace.
--
-- So markers are ours end to end: the engine projects a world point to the
-- screen (WorldToScreen), and this resource's NUI page draws the icon,
-- name, and hold prompt. That also means a marker behaves identically for
-- every player in a session, which retail's save-backed missions never
-- could.
--
-- ONE ACTIVE MARKER AT A TIME. If two overlap, the nearest wins: holding
-- D-pad up is a single input and cannot mean two things at once, and
-- picking arbitrarily would fire the wrong callback often enough to matter.

local markers = {}
local next_id = 1

-- The marker currently offering its prompt, and how long D-pad up has been
-- held on it. Reset when the active marker changes so a hold cannot carry
-- from one marker to the next.
local active_id = nil
local hold_started = nil

-- Wait(0) resumes on the NEXT tick, and the client's scheduler is driven
-- once per rendered frame - so markers are recomputed every frame they are
-- drawn. Anything slower shows as the icon lagging behind the world while
-- the camera pans, because the marker's screen position is a function of a
-- camera that moved since the last sample.

function CreateMarker(marker_type, x, y, z, text, fn)
    if Markers.TYPES[marker_type] == nil then
        error(string.format(
            "CreateMarker: unknown type '%s' (expected one of signup, info, start)",
            tostring(marker_type)), 2)
    end
    local id = next_id
    next_id = next_id + 1
    markers[id] = {
        id = id,
        type = marker_type,
        x = x, y = y, z = z,
        text = text or '',
        on_activate = fn,
    }
    return id
end

function RemoveMarker(id)
    markers[id] = nil
    if active_id == id then
        active_id = nil
        hold_started = nil
    end
end

function GetMarkers()
    return markers
end

local function distance_to(marker, px, py, pz)
    local dx, dy, dz = marker.x - px, marker.y - py, marker.z - pz
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function activate(marker)
    -- An EVENT first, because that is the only thing that crosses resources.
    --
    -- A callback passed to CreateMarker from another resource cannot arrive
    -- here as a function: exports marshal their arguments as JSON
    -- (LuaScriptHost::CallExport_Impl), and a function has no JSON form, so
    -- it lands as nil and pcall reports "attempt to call a nil value". The
    -- header comment used to claim the caller's closure survived the
    -- crossing; it does not, and could not.
    --
    -- A local event DOES reach every started resource (TriggerEvent
    -- dispatches with network_only = false), so this is how a game mode in
    -- its own lua_State hears about its own marker.
    TriggerEvent('markers:activated', marker.id, {
        id = marker.id,
        type = marker.type,
        text = marker.text,
        x = marker.x, y = marker.y, z = marker.z,
    })

    -- The direct callback still works for a marker created INSIDE this
    -- resource, where no state boundary is crossed. Type-checked rather
    -- than assumed so a cross-resource caller gets the event path and no
    -- spurious error.
    if type(marker.on_activate) ~= 'function' then
        return
    end
    -- Guarded: a marker callback is user code, and one that errors must not
    -- take the marker loop - and therefore every other marker - with it.
    local ok, err = pcall(marker.on_activate, marker)
    if not ok then
        print(string.format('[Markers] %s callback error: %s',
            marker.text ~= '' and marker.text or ('#' .. marker.id),
            tostring(err)))
    end
end

-- Blips for our own radar, in radar-local coordinates.
--
-- WHY OUR OWN RADAR. Retail's radar is part of the same Flash/APT HUD movie
-- as its mission markers, and nothing here has reverse-engineered it -
-- adding a blip to it would mean driving that movie. It would also be
-- pointless alongside skate3_no_retail_missions, which denies the mission
-- content the retail radar exists to point at.
--
-- Orientation comes from the CHASE CAMERA rather than a skater heading: the
-- horizontal vector from camera to skater is the direction the player is
-- looking, which is what a rotating radar has to agree with. It also stays
-- sane while the skater is mid-flip, where a board heading does not.
local function radar_blips()
    if not IsPlayerSpawned() then
        return nil
    end
    local px, py, pz = GetEntityCoords(0)
    local cx, cy, cz = GetCameraCoords()
    if cx == nil then
        return nil
    end

    -- Forward on the horizontal plane (Y is vertical in this engine).
    local fx, fz = px - cx, pz - cz
    local length = math.sqrt(fx * fx + fz * fz)
    if length < 0.01 then
        -- Camera sitting on top of the skater: no meaningful facing, so
        -- rotating by a normalised garbage vector would spin the radar.
        return nil
    end
    fx, fz = fx / length, fz / length
    -- Right-hand perpendicular, so positive lands on the radar's right.
    local rx, rz = -fz, fx

    local blips = {}
    for _, marker in pairs(markers) do
        local dx, dz = marker.x - px, marker.z - pz
        local distance = math.sqrt(dx * dx + dz * dz)
        if distance <= Markers.RADAR_RANGE then
            -- Project the world offset onto the camera's own axes, giving
            -- radar-local right/forward in the same units.
            blips[#blips + 1] = {
                id = marker.id,
                type = marker.type,
                -- -1..1 across the radar, forward being up.
                x = (dx * rx + dz * rz) / Markers.RADAR_RANGE,
                y = -(dx * fx + dz * fz) / Markers.RADAR_RANGE,
                near = distance <= Markers.ACTIVATE_RADIUS,
            }
        end
    end
    return blips
end

Skate.CreateThread(function()
    while true do
        local visible = {}
        local nearest, nearest_distance = nil, nil

        if IsPlayerSpawned() then
            local px, py, pz = GetEntityCoords(0)
            for _, marker in pairs(markers) do
                local distance = distance_to(marker, px, py, pz)
                if distance <= Markers.VISIBLE_RADIUS then
                    -- Markers float above the point they mark, the way
                    -- retail's sit over an NPC's head rather than at their
                    -- feet. Y is the vertical axis in this engine.
                    local sx, sy = WorldToScreen(marker.x, marker.y + 2.0, marker.z)
                    -- nil means behind the camera: skip it entirely rather
                    -- than draw it at 0,0 pinned to the corner.
                    if sx ~= nil then
                        visible[#visible + 1] = {
                            id = marker.id,
                            type = marker.type,
                            x = sx, y = sy,
                            text = marker.text,
                            distance = distance,
                            near = distance <= Markers.ACTIVATE_RADIUS,
                        }
                    end
                    if distance <= Markers.ACTIVATE_RADIUS
                        and (nearest_distance == nil or distance < nearest_distance) then
                        nearest, nearest_distance = marker, distance
                    end
                end
            end
        end

        local nearest_id = nearest and nearest.id or nil
        if nearest_id ~= active_id then
            active_id = nearest_id
            hold_started = nil
        end

        local progress = 0.0
        if active_id ~= nil then
            -- D-pad up specifically, never the left stick: the stick is
            -- steering the skater and would fire every marker skated past.
            if IsControlPressed('DPADUP') then
                hold_started = hold_started or Skate.GetGameTimer()
                local held = Skate.GetGameTimer() - hold_started
                progress = math.min(held / Markers.HOLD_MS, 1.0)
                if held >= Markers.HOLD_MS then
                    local marker = markers[active_id]
                    -- Cleared BEFORE the callback so a callback that removes
                    -- its own marker, or teleports the player, cannot come
                    -- back to a half-finished hold and fire twice.
                    hold_started = nil
                    active_id = nil
                    progress = 0.0
                    if marker then
                        activate(marker)
                    end
                end
            else
                hold_started = nil
            end
        end

        SendNUIMessage({
            action = 'markers',
            markers = visible,
            activeId = active_id,
            progress = progress,
            prompt = active_id and Markers.TYPES[markers[active_id].type].label or nil,
            radar = radar_blips(),
        })

        Skate.Wait(0)
    end
end)

-- Exported for other resources. Each resource gets its OWN lua_State, so
-- the globals above are private to Markers - a game mode has to come
-- through exports.
--
-- A CALLBACK CANNOT COME THROUGH. Export arguments are marshalled as JSON,
-- which has no representation for a function, so `fn` arrives nil from any
-- other resource. Cross-resource callers listen for the `markers:activated`
-- event instead (see activate) and compare the id CreateMarker returned.
exports('CreateMarker', CreateMarker)
exports('RemoveMarker', RemoveMarker)
exports('GetMarkers', GetMarkers)

RegisterCommand('markers', function()
    local px, py, pz = 0, 0, 0
    if IsPlayerSpawned() then
        px, py, pz = GetEntityCoords(0)
    end
    local count = 0
    for _, marker in pairs(markers) do
        count = count + 1
        print(string.format('  #%d %-8s %-24s (%.1f, %.1f, %.1f) %.1f away',
            marker.id, marker.type,
            marker.text ~= '' and marker.text or '-',
            marker.x, marker.y, marker.z, distance_to(marker, px, py, pz)))
    end
    if count == 0 then
        print('[Markers] no markers - try "markerhere Test"')
    end
end)

RegisterCommand('markerhere', function(source, args)
    if not IsPlayerSpawned() then
        print('[Markers] not spawned yet')
        return
    end
    local x, y, z = GetEntityCoords(0)
    local text = table.concat(args, ' ')
    local id = CreateMarker('signup', x, y, z,
        text ~= '' and text or 'Test Marker',
        function(marker)
            print(string.format('[Markers] activated %s (#%d)',
                marker.text, marker.id))
        end)
    print(string.format('[Markers] created #%d at (%.1f, %.1f, %.1f)', id, x, y, z))
end)

print('[Markers] ready - "markerhere Big Air" then skate up and hold D-pad up')
