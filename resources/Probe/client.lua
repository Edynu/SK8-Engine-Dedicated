-- Guest memory exploration commands. Read only: searching and reading cannot
-- corrupt a running game, and nothing here needs to write.

RegisterCommand('findstr', function(source, args)
    local text = table.concat(args, ' ')
    if text == '' then
        print('[probe] findstr <text>')
        return
    end
    local found = FindGuestString(text, 16)
    for _, address in ipairs(found) do
        print('  ' .. address)
    end
    print(string.format('[probe] %d hit(s) for "%s"', #found, text))
end)

RegisterCommand('findref', function(source, args)
    if args[1] == nil then
        print('[probe] findref <0xADDRESS>')
        return
    end
    local found = FindGuestRef(args[1], 24)
    if found == nil then
        print('[probe] not a hex address')
        return
    end
    for _, address in ipairs(found) do
        print('  ' .. address)
    end
    print(string.format('[probe] %d reference(s) to %s', #found, args[1]))
end)

RegisterCommand('readstr', function(source, args)
    print('[probe] ' .. tostring(ReadGuestString(args[1] or '', 128)))
end)

RegisterCommand('readu32', function(source, args)
    print('[probe] ' .. tostring(ReadGuestU32(args[1] or '')))
end)

-- The names worth locating first. Each is a front-end script binding the Flash
-- UI calls by name, so finding the name finds the table that dispatches it -
-- and that table is the route to calling the editor without pad input.
local CAC_NAMES = {
    'GetEditSkaterOptions',
    'CAC_GetNumItems',
    'CAC_GetItemUnlockHALID',
    'CAC_OnThumbnailSelect',
    'CASSimManager_ReloadOp',
}

RegisterCommand('cacnames', function()
    for _, name in ipairs(CAC_NAMES) do
        local found = FindGuestString(name, 4)
        if #found == 0 then
            print(string.format('  %-26s not found', name))
        else
            print(string.format('  %-26s %s', name, table.concat(found, ' ')))
        end
    end
    print('[probe] next: "findref <address>" on a hit to find what points at it')
end)

-- Given a name string's address, show what points at it and what sits beside
-- that pointer. A binding table is {name_ptr, function_ptr} pairs, so the
-- neighbouring words are where the function to call will be.
RegisterCommand('bindprobe', function(source, args)
    local target = args[1]
    if target == nil then
        print('[probe] bindprobe <0xADDRESS of a name string>')
        return
    end
    local refs = FindGuestRef(target, 8)
    if refs == nil or #refs == 0 then
        print('[probe] nothing points at ' .. target ..
              ' - the address is built in code, not stored')
        return
    end
    for _, ref in ipairs(refs) do
        print('  ref at ' .. ref)
        -- Two words either side: enough to see a {name, fn} pair whichever
        -- order the struct uses, without burying the answer in output.
        local base = tonumber(ref, 16)
        for offset = -8, 12, 4 do
            local address = string.format('0x%08X', base + offset)
            local value = ReadGuestU32(address)
            local text = ReadGuestString(value or '', 40)
            print(string.format('    %+3d %s = %s%s', offset, address,
                tostring(value),
                text and ('  "' .. text .. '"') or ''))
        end
    end
end)

-- ------------------------------------------------------- what ran when
--
-- Static search for a name-string reference failed: the compiler reaches many
-- strings from one base register by displacement, so there is no instruction
-- pair to find. Watching which functions actually execute avoids that
-- entirely.
--
-- Capture a BASELINE first (arm, do nothing, disarm), then capture the action.
-- Whatever is in the action and not the baseline is that action's code.
--
--   covstart
--   ...stand still for a few seconds...
--   covstop baseline
--   covstart
--   ...open Edit Skater by hand...
--   covstop editor
RegisterCommand('covstart', function()
    StartCoverage()
    print('[probe] coverage armed - now do the thing, then "covstop <label>"')
end)

RegisterCommand('covstop', function(source, args)
    local count, path = StopCoverage(args[1] or 'capture')
    print(string.format('[probe] %d address(es) -> %s', count, tostring(path)))
end)

-- Dumps the table sub_825D3B38 indexes by its first argument.
--
-- Found by diffing coverage: opening Edit Skater runs sub_825DF470, which
-- calls sub_825D3B38 with 50 and then 52. That function does
-- `lwzx r3, id*4, 0x8302C608` - a pointer table indexed by the id. Reading the
-- strings out of it says what those ids ARE, which decides whether this is the
-- screen opener we want or something else wearing the same shape.
RegisterCommand('screentable', function(source, args)
    local base = 0x8302C608
    local last = tonumber(args[1]) or 60
    local named = 0
    for id = 0, last do
        local slot = string.format('0x%08X', base + id * 4)
        local pointer = ReadGuestU32(slot)
        if pointer ~= nil and pointer ~= '0x00000000' then
            local text = ReadGuestString(pointer, 64)
            if text ~= nil then
                print(string.format('  %3d  %s  "%s"', id, pointer, text))
                named = named + 1
            end
        end
    end
    print(string.format('[probe] %d named entries in the table at 0x%08X',
        named, base))
end)

-- Walks consecutive NUL-terminated strings from an address. String tables are
-- laid out end to end, so reading one reveals its neighbours - which is how a
-- name compared against in code turns into the whole list of valid names.
RegisterCommand('strings', function(source, args)
    local address = tonumber((args[1] or ''):gsub('^0[xX]', ''), 16)
    if address == nil then
        print('[probe] strings <0xADDRESS> [count]')
        return
    end
    local count = tonumber(args[2]) or 24
    local shown = 0
    for _ = 1, count * 4 do
        if shown >= count then break end
        local text = ReadGuestString(string.format('0x%08X', address), 64)
        if text ~= nil and #text > 0 then
            print(string.format('  0x%08X  "%s"', address, text))
            address = address + #text + 1
            shown = shown + 1
        else
            -- Not text (padding or a gap): step a byte and keep looking.
            address = address + 1
        end
    end
end)

-- Dumps consecutive 32-bit words, resolving any that point at a string.
-- Reading a table this way shows both its contents and its STRIDE, which is
-- what turns one known entry into the whole table.
RegisterCommand('words', function(source, args)
    local address = tonumber((args[1] or ''):gsub('^0[xX]', ''), 16)
    if address == nil then
        print('[probe] words <0xADDRESS> [count]')
        return
    end
    local count = tonumber(args[2]) or 16
    for index = 0, count - 1 do
        local slot = string.format('0x%08X', address + index * 4)
        local value = ReadGuestU32(slot)
        local text = value and ReadGuestString(value, 48) or nil
        print(string.format('  %s = %s%s', slot, tostring(value),
            text and ('  "' .. text .. '"') or ''))
    end
end)

-- ------------------------------------------------- calling the front end
--
-- The front end is a Flash movie C++ drives by invoking named ActionScript
-- methods. "screentable" lists them; 6 is _global.ScreenManager.OpenScreen.
-- Arguments are plain guest addresses - a string argument is a pointer to
-- bytes already in memory, so a screen can be named using the game's OWN
-- string rather than one we have to write.
RegisterCommand('flashcall', function(source, args)
    local id = tonumber(args[1])
    if id == nil then
        print('[probe] flashcall <methodId> [0xarg ...]')
        return
    end
    local ok, message = CallFlashMethod(id, args[2], args[3], args[4], args[5])
    print('[probe] ' .. tostring(message))
end)

-- The screen registry gives "CreateASkater" at a fixed address, so opening the
-- editor is OpenScreen(thatName). Whether the Flash side alone is ENOUGH is
-- exactly what this tests: the C++ side also loads CAS assets when the editor
-- is entered normally, and this does not do that.
RegisterCommand('openeditor', function()
    local ok, message = OpenCharacterEditor()
    print('[probe] ' .. tostring(message))
end)

print('[probe] ready - "dumpwords <addr> <n>", "fenatives", "words", "strings"')
