-- Console front end for the wardrobe catalogue. No state of its own: the
-- engine owns the index (src/skate3_clothing.cpp) because it is built from
-- the extracted retail archive, not from anything a script knows.

local DEFAULT_LIMIT = 40

local function printCategories()
    local categories = GetClothingCategories()
    if #categories == 0 then
        -- Not an error: createacharacter.big is extracted on a background
        -- thread at startup, so "nothing yet" is the honest answer early on.
        print('[wardrobe] catalogue not ready yet - try again in a moment')
        return
    end
    local total = 0
    for _, category in ipairs(categories) do
        local suffix = ''
        if category.custom > 0 then
            suffix = string.format('  (%d custom)', category.custom)
        end
        print(string.format('  %-14s %4d%s', category.name, category.count,
            suffix))
        total = total + category.count
    end
    print(string.format('[wardrobe] %d categories, %d items',
        #categories, total))
end

local function printCategory(name, filter, limit)
    local count = GetClothingCount(name)
    if count == 0 then
        print(string.format('[wardrobe] no category "%s" (run "clothing" ' ..
            'for the list)', name))
        return
    end
    local ids = ListClothing(name, filter or '', limit)
    for index, id in ipairs(ids) do
        print(string.format('  %3d  %s', index, id))
    end
    if filter and filter ~= '' then
        print(string.format('[wardrobe] %s: %d shown of %d matching filter ' ..
            '"%s" (%d total)', name, #ids, #ids, filter, count))
    else
        print(string.format('[wardrobe] %s: %d shown of %d', name, #ids,
            count))
    end
    if #ids >= limit and limit < count then
        print(string.format('[wardrobe] limit %d reached - "clothing %s ' ..
            '<filter> <limit>" for more', limit, name))
    end
end

RegisterCommand('clothing', function(source, args)
    if args[1] == nil then
        printCategories()
        return
    end
    printCategory(args[1], args[2], tonumber(args[3]) or DEFAULT_LIMIT)
end)

-- Is this id one the catalogue lists? The filter is a substring match on the
-- stored id, so a hit means the catalogue has that exact entry.
local function inCatalogue(category, id)
    if id == nil or id == '' then
        return false
    end
    return #ListClothing(category, id, 1) > 0
end

RegisterCommand('outfit', function()
    local pieces = GetSkaterOutfit()
    if pieces == nil then
        print('[wardrobe] no outfit read yet - the recipe is published once ' ..
              'the skater is in the world')
        return
    end
    -- Prints the ASSET id, the MODEL id, and which of the two the catalogue
    -- recognises.
    --
    -- This is the question that decides whether an outfit can be written
    -- later: the catalogue is keyed by the filenames in the extracted
    -- archive, and a worn piece names both an asset and a model. Whichever
    -- one matches is the id a setter would have to write - and if NEITHER
    -- matches, the two live in different hash spaces and need a lookup we do
    -- not have yet. Better to see that here than to guess at it.
    local assetHits, modelHits = 0, 0
    for _, piece in ipairs(pieces) do
        local assetHit = inCatalogue(piece.category, piece.id)
        local modelHit = inCatalogue(piece.category, piece.model)
        assetHits = assetHits + (assetHit and 1 or 0)
        modelHits = modelHits + (modelHit and 1 or 0)
        local mark = '-'
        if assetHit and modelHit then
            mark = 'both'
        elseif assetHit then
            mark = 'asset'
        elseif modelHit then
            mark = 'model'
        end
        print(string.format('  %-14s asset=%s model=%s  catalogue=%s',
            piece.category, piece.id, piece.model, mark))
    end
    print(string.format('[wardrobe] %d piece(s) worn - %d asset id(s) and ' ..
        '%d model id(s) found in the catalogue', #pieces, assetHits,
        modelHits))
end)

-- Proves the id is usable as an index, which is what a menu would do:
-- GetClothingCount to size the list, GetClothingId to walk it.
RegisterCommand('clothingat', function(source, args)
    local name = args[1]
    local index = tonumber(args[2])
    if name == nil or index == nil then
        print('[wardrobe] clothingat <category> <index>')
        return
    end
    local id = GetClothingId(name, index)
    if id == nil then
        print(string.format('[wardrobe] %s has no item %d (count %d)', name,
            index, GetClothingCount(name)))
        return
    end
    print(string.format('[wardrobe] %s #%d = %s', name, index, id))
end)



-- ------------------------------------------------------------ changing

-- wear <category> <index|0x id>
--
-- Index form so you can pick straight out of a "clothing <category>" listing;
-- id form so a script can name an exact item without depending on position.
RegisterCommand('wear', function(source, args)
    local category, choice = args[1], args[2]
    if category == nil or choice == nil then
        print('[wardrobe] wear <category> <index|0x id>')
        return
    end
    local id = choice
    local index = tonumber(choice)
    -- A bare number is an index; anything with 0x or hex letters is an id.
    -- Checked this way round because an id is never a plain decimal.
    if index ~= nil and not string.find(choice, '0x', 1, true) then
        id = GetClothingId(category, index)
        if id == nil then
            print(string.format('[wardrobe] %s has no item %d (count %d)',
                category, index, GetClothingCount(category)))
            return
        end
    end
    local ok, message = SetClothingModel(category, id)
    if not ok then
        print('[wardrobe] ' .. tostring(message))
        return
    end
    print(string.format('[wardrobe] %s -> %s', category, tostring(message)))
    print('[wardrobe] peers see this immediately; your own skater may need a ' ..
          'respawn - retail rebuilds the character on its own schedule')
end)

RegisterCommand('wearclear', function(source, args)
    ClearClothing(args[1])
    if args[1] then
        print('[wardrobe] cleared ' .. args[1])
    else
        print('[wardrobe] cleared every override')
    end
end)

RegisterCommand('wearlist', function()
    local overrides = GetClothingOverrides()
    for _, entry in ipairs(overrides) do
        print(string.format('  %-14s %s', entry.category, entry.id))
    end
    print(string.format('[wardrobe] %d override(s) active', #overrides))
end)

-- Tells us WHY the local skater is not changing, which the symptom alone
-- cannot: retail overwriting the recipe every frame and retail ignoring it
-- both look identical from the outside.
RegisterCommand('wearstatus', function()
    local stats = GetClothingStats()
    print(string.format('[wardrobe] examined=%d patched=%d rejected=%d',
        stats.examined, stats.patched, stats.rejected))
    if stats.examined == 0 then
        print('[wardrobe] no override active, or no recipe read yet')
        return
    end
    if stats.rejected > 0 then
        print('[wardrobe] some edits were thrown away by the verify - the ' ..
              'offsets did not hold')
    end
    local ratio = stats.patched / stats.examined
    if ratio > 0.5 then
        print('[wardrobe] retail is REWRITING the recipe every read: that ' ..
              'buffer is its output, not what it builds the skater from')
    else
        print('[wardrobe] the write STICKS (patched once, then stable): the ' ..
              'recipe is not being rebuilt from - what is missing is the rebuild')
    end
end)

-- ------------------------------------------------- retail's own editor
--
-- The reason this exists: retail's Edit Skater screen already rebuilds the
-- character correctly when you pick an item. Reaching it from a script is
-- worth more than reimplementing it, and more than the recipe-write path,
-- which changes what peers see but not your own skater.

-- Screens are numbered and the binary names none of them, so the id has to be
-- observed. Walk to Edit Skater by hand, then run this.
RegisterCommand('festate', function()
    local info = GetFrontEndInfo()
    print(string.format('[wardrobe] state=%d manager=%s calls=%d',
        info.state, info.manager, info.calls))
    for _, event in ipairs(info.history) do
        print(string.format('    state=%-4d mode=%-3d from lr=%s',
            event.state, event.mode, event.lr))
    end
    -- `calls` is the whole diagnosis. If it does not move while you walk into
    -- Edit Skater, that screen is not reached through the state machine and
    -- no id will open it.
    print('[wardrobe] note the call count, open Edit Skater, run this again:')
    print('[wardrobe]   count moved -> the new state id is the editor')
    print('[wardrobe]   count same  -> the editor is not a front-end state')
end)

-- editskater [sequence]
--
-- Walks to retail's Edit Skater screen by replaying pad presses, because that
-- screen is not a front-end state - the state machine is not used after boot,
-- so there is no id to ask for. Find your sequence once by noting the buttons
-- you press, then put it in skate3_character_editor_inputs and call this with
-- no argument.
RegisterCommand('editskater', function(source, args)
    local sequence = args[1]
    if sequence == nil and #args > 0 then
        sequence = table.concat(args, ',')
    end
    local ok, message = OpenCharacterEditor(sequence)
    print('[wardrobe] ' .. tostring(message))
end)

-- Raw replay, for working the sequence out a step at a time.
RegisterCommand('padinput', function(source, args)
    if args[1] == nil then
        print('[wardrobe] padinput start,down,down,a   (tokens: a b x y ' ..
              'start back lb rb lt rt up down left right l3 r3, each ' ..
              'optionally :ms)')
        return
    end
    local ok, message = SendPadInput(table.concat(args, ','))
    print('[wardrobe] ' .. tostring(message))
end)

-- unlockall [on|off]
--
-- Retail's own switch, not a display hook: the progression manager carries a
-- flag that makes every unlock query answer yes, so the padlock, the blurb and
-- the code that decides whether you may WEAR an item all agree at once.
--
-- ONLINE DOES NOT NEED THIS. A session forces the unlock on in the engine,
-- from the first frame, so every player has the same wardrobe whether or not a
-- resource ever runs. The command is here for offline play and for reading the
-- state back.
--
-- Three answers, because they can differ. "wanted" is whether it will be on;
-- "applied" is whether the flag is actually on the manager, which it cannot be
-- until the manager exists; "forced" is whether the session decided. If
-- "applied" is false, the game has not built the manager yet - wait for
-- IsPlayerInGame() and ask again.
RegisterCommand('unlockall', function(source, args)
    if args[1] ~= nil then
        SetUnlockEverything(args[1] == 'on' or args[1] == 'true' or
                            args[1] == '1')
    end
    local wanted, applied, forced = IsEverythingUnlocked()
    print(string.format(
        '[wardrobe] unlock everything: wanted=%s applied=%s forced=%s',
        tostring(wanted), tostring(applied), tostring(forced)))
    if forced then
        print('[wardrobe] forced by the online session - this cannot be ' ..
              'turned off here, and does not need turning on')
    end
end)

print('[wardrobe] ready - "clothing", "outfit", "wear <cat> <n>", ' ..
      '"festate", "editskater", "padinput <tokens>", "unlockall [on|off]"')
