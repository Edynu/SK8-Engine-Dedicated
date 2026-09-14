local watching = false

-- Fires once per completed grind, including one that started and ended between
-- two script frames - which a polling loop alone would miss.
AddEventHandler('skate3:grindEnded', function(grind)
  print(string.format('[grindtest] grind ended: %.2fm over %.1fs (reward %.0f)',
                      grind.distance, grind.duration / 1000.0, grind.reward))
end)

RegisterCommand('grindlog', function()
  print('[grindtest] grind events are always on - go grind a rail')
end)

-- Prints on change only. Per-frame output would bury the two transitions that
-- matter: the moment the grind starts, and the moment it ends.
RegisterCommand('grindwatch', function()
  if watching then
    watching = false
    print('[grindtest] watch off')
    return
  end
  watching = true
  print('[grindtest] watch on - grind a rail. "grindwatch" again to stop.')
  Skate.CreateThread(function()
    local last = ''
    while watching do
      local grind = GetSkaterGrind()
      local line
      if grind == nil then
        line = 'not grinding'
      else
        -- Rounded before comparing, or every frame differs and this prints
        -- continuously - defeating the point of printing on change.
        line = string.format('grinding  %.1fm  %.1fs  reward %.0f',
                             grind.distance, grind.duration / 1000.0,
                             grind.reward)
      end
      if line ~= last then
        print('[grindtest] ' .. line)
        last = line
      end
      Skate.Wait(100)
    end
  end)
end)

-- Cross-check: IsSkaterGrinding and GetSkaterGrind must never disagree. They
-- read the same published snapshot, so a mismatch means the publish is torn.
RegisterCommand('grindcheck', function()
  local flag = IsSkaterGrinding()
  local grind = GetSkaterGrind()
  local agree = (flag and grind ~= nil) or (not flag and grind == nil)
  print(string.format('[grindtest] IsSkaterGrinding=%s GetSkaterGrind=%s -> %s',
                      tostring(flag), grind ~= nil and 'table' or 'nil',
                      agree and 'consistent' or 'MISMATCH'))
  if grind ~= nil then
    print(string.format('[grindtest]   %.2fm  %dms  reward %.0f',
                        grind.distance, grind.duration, grind.reward))
  end
end)

print('[grindtest] ready - grindwatch, grindcheck; grind events print automatically')
