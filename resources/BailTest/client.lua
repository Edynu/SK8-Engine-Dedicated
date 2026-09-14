local watching = false

AddEventHandler('skate3:bailed', function()
  print('[bailtest] BAILED - crashed off the board')
end)

AddEventHandler('skate3:steppedOff', function()
  print('[bailtest] stepped off - left the board deliberately')
end)

AddEventHandler('skate3:recovered', function(info)
  print(string.format('[bailtest] recovered after %.1fs off the board',
                      info.duration / 1000.0))
end)

-- Prints on change only. The two transitions are the whole story; a per-frame
-- readout would bury them.
RegisterCommand('bailwatch', function()
  if watching then
    watching = false
    print('[bailtest] watch off')
    return
  end
  watching = true
  print('[bailtest] watch on - crash, then try stepping off deliberately')
  Skate.CreateThread(function()
    local last = ''
    while watching do
      local bail = GetSkaterBail()
      local line
      if not bail.offBoard then
        line = 'on the board'
      else
        line = string.format('%s for %.1fs',
                             bail.bailed and 'BAILED' or 'stepped off',
                             bail.duration / 1000.0)
      end
      if line ~= last then
        print('[bailtest] ' .. line)
        last = line
      end
      Skate.Wait(100)
    end
  end)
end)

print('[bailtest] ready - bailwatch; events print automatically')
