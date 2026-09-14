-- Load order: first.lua waited at its top level, so if the boot chain ran the
-- scripts as separate threads this file would have run during that wait and
-- would see an incomplete table - or none at all.
if WaitTestOrder == nil then
  print('[waittest] ORDER BROKEN - second.lua ran before first.lua defined anything')
elseif #WaitTestOrder ~= 2 then
  print(string.format(
      '[waittest] ORDER BROKEN - second.lua ran mid-wait, saw %d of 2 entries',
      #WaitTestOrder))
else
  print('[waittest] load order OK - second.lua saw all of first.lua')
end

WaitTestOrder[#WaitTestOrder + 1] = 'second.lua top level'

-- A wait inside a command handler. This is the shape people actually write and
-- the one that used to fail.
RegisterCommand('waittest', function()
  local started = Skate.GetGameTimer()
  print('[waittest] command handler: waiting 1000ms...')
  Skate.Wait(1000)
  local elapsed = Skate.GetGameTimer() - started
  if elapsed >= 900 then
    print(string.format('[waittest] command handler wait OK (%d ms)', elapsed))
  else
    print(string.format('[waittest] command handler wait TOO SHORT (%d ms)',
                        elapsed))
  end

  -- Waiting twice in a row: the handler has to be re-parked after each one,
  -- not just resumed once and then run to completion.
  Skate.Wait(500)
  print(string.format('[waittest] second wait OK (%d ms total)',
                      Skate.GetGameTimer() - started))
end)

-- And inside an event handler, which reaches Lua by a different path.
AddEventHandler('waittest:ping', function()
  print('[waittest] event handler: waiting 300ms...')
  Skate.Wait(300)
  print('[waittest] event handler wait OK')
end)

RegisterCommand('waittestevent', function()
  TriggerEvent('waittest:ping')
  -- Prints BEFORE the handler finishes: the handler is suspended, and
  -- TriggerEvent does not block waiting for it. Worth seeing explicitly,
  -- because it is the one behaviour change a script author can notice.
  print('[waittest] TriggerEvent returned while the handler is still waiting')
end)

print('[waittest] ready - "waittest" and "waittestevent"')
