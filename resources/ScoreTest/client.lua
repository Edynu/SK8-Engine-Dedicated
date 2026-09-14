local watching = false

local function describe(s)
  if s == nil then
    return 'no score block (front end, loading, or not spawned)'
  end
  return string.format('seq=%d  mult=%.2fx  momentum=%d  line=%d',
                       s.sequence, s.multiplier, s.momentum, s.line)
end

RegisterCommand('scoreonce', function()
  print('[scoretest] ' .. describe(GetSkaterScore()))
end)

-- Prints only on CHANGE rather than every tick. A polling loop that prints
-- every frame buries the one transition you are trying to see, and the whole
-- point of this is watching the multiplier step 1x -> 2x -> 3x and the sequence
-- score climb mid-trick before it lands.
RegisterCommand('scorewatch', function()
  if watching then
    watching = false
    print('[scoretest] watch off')
    return
  end
  watching = true
  print('[scoretest] watch on - skate and land a trick. "scorewatch" again to stop.')
  Skate.CreateThread(function()
    local last = ''
    while watching do
      local s = GetSkaterScore()
      local line = describe(s)
      if line ~= last then
        print('[scoretest] ' .. line)
        last = line
      end
      Skate.Wait(50)
    end
  end)
end)

RegisterCommand('scoreblock', function()
  print('[scoretest] score block  : ' .. tostring(GetSkaterScoreBlock()))
  print('[scoretest] marker block : ' .. tostring(GetSessionMarkerBlock()))
  print('[scoretest] marker active: ' .. tostring(IsSessionMarkerActive()))
  -- Both blocks hang off the same root (0x83067060) at different slots, so if
  -- one resolves and the other does not, the root is right and a slot is wrong -
  -- which is a much smaller thing to go and fix than "nothing works".
  print('[scoretest] dumping the first 64 words of the score block:')
  local block = GetSkaterScoreBlock()
  if block == nil then
    print('[scoretest]   unavailable')
    return
  end
  local base = tonumber(block, 16)
  for i = 0, 63 do
    local addr = string.format('0x%08X', base + i * 4)
    local value = ReadGuestU32(addr)
    if value ~= nil and value ~= '0x00000000' then
      print(string.format('[scoretest]   +%-4d %s', i * 4, value))
    end
  end
end)

print('[scoretest] ready - scoreonce, scorewatch, scoreblock')
