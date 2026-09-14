-- Set here, read by second.lua at ITS top level. The manifest promises a
-- later script sees what an earlier one defined, and this file waits before
-- finishing - so if that promise still holds with waiting in the mix, the
-- boot chain is ordered correctly.
WaitTestOrder = { 'first.lua top level' }

local started = Skate.GetGameTimer()

print('[waittest] first.lua: top level, before the wait')

-- The case that used to be impossible: a wait during load.
Skate.Wait(250)

local elapsed = Skate.GetGameTimer() - started
if elapsed >= 200 then
  print(string.format('[waittest] top-level wait OK (%d ms)', elapsed))
else
  print(string.format('[waittest] top-level wait TOO SHORT (%d ms, wanted 250)',
                      elapsed))
end

WaitTestOrder[#WaitTestOrder + 1] = 'first.lua after wait'
