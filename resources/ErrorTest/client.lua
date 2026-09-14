RegisterCommand('errcmd', function()
  error('this command handler failed on purpose')
end)

-- Three frames deep, so the traceback has something to actually show. Reported
-- with only the innermost line, this would say "failed in deep()" and leave you
-- guessing which of deep's callers ran it.
local function deep()
  error('thrown three calls deep')
end

local function middle()
  deep()
end

RegisterCommand('errnested', function()
  middle()
end)

AddEventHandler('errortest:boom', function()
  error('this event handler failed on purpose')
end)

RegisterCommand('errevent', function()
  TriggerEvent('errortest:boom')
end)

-- The interesting case. This one throws AFTER a wait, so by then the command
-- that started it has long returned and there is no C++ call site left to
-- report against - the scheduler has to report it itself.
RegisterCommand('errthread', function()
  print('[errortest] waiting, then failing...')
  Skate.Wait(500)
  error('failed after a wait - reported by the scheduler')
end)

-- Not an explicit error() call: the kind of mistake people actually make. The
-- message should name the culprit ("attempt to index a nil value (global
-- 'thisTableDoesNotExist')") rather than just pointing at the line.
RegisterCommand('errnil', function()
  print(thisTableDoesNotExist.field)
end)

print('[errortest] ready - errcmd, errnested, errevent, errthread, errnil')
