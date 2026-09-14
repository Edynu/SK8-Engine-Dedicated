-- Rules and shapes shared by both sides.
--
-- Kept free of engine calls so the server (which has no renderer) and the
-- client run the same rule code - the scoreboard a player sees and the
-- letters the server assigns must never be two different implementations.

SkateGame = SkateGame or {}

SkateGame.WORD = 'SKATE'
SkateGame.MAX_LETTERS = #SkateGame.WORD

-- How long a player gets to land the trick that is on them. Generous: a
-- game of S.K.A.T.E. is played at conversation pace, and a timer that
-- expires while someone is lining up a run is worse than one that is too
-- slow.
SkateGame.SET_SECONDS = 45
SkateGame.MATCH_SECONDS = 45

-- Default half-extents of the play area, in world units.
SkateGame.DEFAULT_AREA_SIZE = 40.0
SkateGame.MIN_AREA_SIZE = 8.0
SkateGame.MAX_AREA_SIZE = 200.0
-- The area is a box, and its vertical extent is deliberately generous: a
-- trick can peak well above the ground it started on, and a player should
-- never lose a set because the box clipped their airtime.
SkateGame.AREA_HEIGHT = 30.0

-- Trick pattern classes a player can deliberately CALL.
--
-- This is the one list that decides two things at once, and they have to be
-- the same list: which tricks the menu offers, and which landings count as
-- a player's one attempt. Anything outside it - an ollie, an air, the
-- unclassified entries - is something that HAPPENS while skating rather
-- than a trick you chose, so it must never consume an attempt. Hopping onto
-- the board, in particular, cannot be allowed to fail somebody.
--
-- Shared rather than client-side because the server is the one assigning
-- letters; if the two sides disagreed about what counts, the scoreboard
-- would punish a trick the menu never offered.
SkateGame.CALLABLE_CLASSES = {
    [2] = 'Flip', [3] = 'Finger flip', [4] = 'Grab', [5] = 'Grind',
    [6] = 'Handplant', [7] = 'No comply', [8] = 'Manual', [9] = 'Slide',
    [11] = 'Revert', [12] = 'Boneless', [13] = 'Footplant',
}

function SkateGame.IsCallable(class)
    return type(class) == 'number' and SkateGame.CALLABLE_CLASSES[class] ~= nil
end

SkateGame.PHASE_LOBBY = 'lobby'
SkateGame.PHASE_SET = 'set'
SkateGame.PHASE_MATCH = 'match'
SkateGame.PHASE_OVER = 'over'

-- Letters earned, as the word so far: 2 -> "SK".
function SkateGame.LettersOf(count)
    return SkateGame.WORD:sub(1, math.max(0, math.min(count, SkateGame.MAX_LETTERS)))
end

function SkateGame.IsOut(letters)
    return letters >= SkateGame.MAX_LETTERS
end

-- Box containment. Y is VERTICAL in this engine, so the horizontal extent
-- is x/z and y is treated separately - see the axis note in Zones/shared.lua.
function SkateGame.InArea(area, x, y, z)
    if area == nil then
        return true  -- no area set means the whole world counts
    end
    return math.abs(x - area.x) <= area.size
        and math.abs(z - area.z) <= area.size
        and math.abs(y - area.y) <= SkateGame.AREA_HEIGHT
end

-- The eight corners of the area box, for drawing it. Ordered so that
-- CORNER_EDGES below indexes pairs that are actually adjacent.
function SkateGame.AreaCorners(area)
    local s, h = area.size, SkateGame.AREA_HEIGHT
    local corners = {}
    for i = 0, 7 do
        local sx = (i % 2 == 0) and -1 or 1
        local sy = (math.floor(i / 2) % 2 == 0) and -1 or 1
        local sz = (i < 4) and -1 or 1
        corners[i + 1] = {
            x = area.x + sx * s,
            y = area.y + sy * h,
            z = area.z + sz * s,
        }
    end
    return corners
end

-- The 12 edges of the box as index pairs into AreaCorners' output.
SkateGame.CORNER_EDGES = {
    {1, 2}, {3, 4}, {1, 3}, {2, 4},          -- near face
    {5, 6}, {7, 8}, {5, 7}, {6, 8},          -- far face
    {1, 5}, {2, 6}, {3, 7}, {4, 8},          -- connecting
}
