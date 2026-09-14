-- Reads out every trick the game records, so the NAMES can be checked
-- against what the HUD says before anything is built on top of them.
--
-- Name fidelity is the open question: the engine looks each name up in the
-- fixed EScorable metadata table by the id the Scorable carries, and marks
-- the result untrusted when that table entry does not corroborate the
-- lookup. An untrusted name must be treated as "unknown trick", never as
-- the trick it appears to name.

local PATTERN_CLASS = {
  [0] = 'none', 'air', 'flip', 'fingerflip', 'grab', 'grind', 'handplant',
  'nocomply', 'manual', 'slide', 'hippyjump', 'revert', 'boneless',
  'footplant',
}

local counts = {}
local total = 0

local function describe(trick)
  local class = PATTERN_CLASS[trick.patternClass] or ('class' .. tostring(trick.patternClass))
  local name = trick.name
  if name == nil or name == '' then
    name = '<no name>'
  end
  if not trick.nameTrusted then
    name = name .. ' (UNTRUSTED)'
  end
  return string.format('%-34s id=%-4d %-11s value=%s', name, trick.id, class,
                       tostring(trick.value))
end

AddEventHandler('skate3:trickLanded', function(trick)
  total = total + 1
  counts[trick.name or ''] = (counts[trick.name or ''] or 0) + 1
  print(string.format('[trick] LANDED    %s', describe(trick)))
end)

AddEventHandler('skate3:trickCancelled', function(trick)
  print(string.format('[trick] cancelled %s', describe(trick)))
end)

RegisterCommand('tricks', function()
  print(string.format('[trick] %d landed trick(s) seen so far:', total))
  local names = {}
  for name in pairs(counts) do names[#names + 1] = name end
  table.sort(names)
  for _, name in ipairs(names) do
    print(string.format('  %-34s x%d', name == '' and '<no name>' or name,
                        counts[name]))
  end
end)

print('[trick] TrickTest ready - skate around, then run "tricks"')

-- Dumps retail's whole trick-metadata table. This is the check that the
-- table layout is right BEFORE any trick is landed: if the names read out
-- as recognisable Skate 3 tricks and each entry corroborates its own id,
-- the id -> name path the events depend on is sound.
RegisterCommand('trickdump', function(source, args)
  local first = tonumber(args[1]) or 0
  local last = tonumber(args[2]) or 331
  local trusted, untrusted = 0, 0
  for id = first, last do
    local name, ok = GetTrickName(id)
    local class = GetTrickPatternClass(id)
    if ok then
      trusted = trusted + 1
    else
      untrusted = untrusted + 1
    end
    print(string.format('%3d %-36s class=%-2d %s', id,
                        (name ~= '' and name) or '<empty>', class,
                        ok and '' or 'UNTRUSTED'))
  end
  print(string.format('[trick] %d trusted, %d untrusted of %d entries',
                      trusted, untrusted, last - first + 1))
end)
