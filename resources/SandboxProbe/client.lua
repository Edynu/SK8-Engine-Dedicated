-- Each entry names a way out of the sandbox and how to look for it. The
-- probes only ever inspect - none of them tries to actually read a file or
-- start a process, because a check that has to do damage to report success is
-- a bad check.
local escapes = {
  { 'io (file read/write)',          function() return io end },
  { 'io.open',                       function() return io and io.open end },
  { 'package (module loader)',       function() return package end },
  { 'package.loadlib (load a DLL)',  function() return package and package.loadlib end },
  { 'require',                       function() return require end },
  { 'load (bytecode)',               function() return load end },
  { 'loadstring',                    function() return loadstring end },
  { 'loadfile',                      function() return loadfile end },
  { 'dofile',                        function() return dofile end },
  { 'os.execute (spawn a process)',  function() return os and os.execute end },
  { 'os.remove',                     function() return os and os.remove end },
  { 'os.rename',                     function() return os and os.rename end },
  { 'os.getenv',                     function() return os and os.getenv end },
  { 'os.exit (kill the game)',       function() return os and os.exit end },
  { 'debug.getregistry',             function() return debug and debug.getregistry end },
  { 'debug.setupvalue',              function() return debug and debug.setupvalue end },
}

-- Things the sandbox is meant to KEEP. A sandbox that broke these would look
-- perfectly secure above while quietly breaking every resource, so they are
-- checked in the same pass rather than left to be discovered later.
local keep = {
  { 'os.time',        function() return os and os.time end },
  { 'os.clock',       function() return os and os.clock end },
  { 'os.date',        function() return os and os.date end },
  { 'debug.traceback',function() return debug and debug.traceback end },
  { 'string.format',  function() return string and string.format end },
  { 'table.insert',   function() return table and table.insert end },
  { 'math.floor',     function() return math and math.floor end },
  { 'coroutine',      function() return coroutine end },
  { 'pcall',          function() return pcall end },
  { 'tostring',       function() return tostring end },
}

RegisterCommand('sandboxcheck', function()
  local leaked, missing = 0, 0

  print('--- escapes (all must be blocked) ---')
  for _, entry in ipairs(escapes) do
    local ok, value = pcall(entry[2])
    -- An error here means the lookup itself failed, which is still a form of
    -- unreachable, so it counts as blocked rather than as an inconclusive.
    local reachable = ok and value ~= nil
    if reachable then leaked = leaked + 1 end
    print(string.format('  %-32s %s', entry[1], reachable and 'REACHABLE' or 'blocked'))
  end

  print('--- kept (all must be present) ---')
  for _, entry in ipairs(keep) do
    local ok, value = pcall(entry[2])
    local present = ok and value ~= nil
    if not present then missing = missing + 1 end
    print(string.format('  %-32s %s', entry[1], present and 'ok' or 'MISSING'))
  end

  if leaked == 0 and missing == 0 then
    print('sandbox: ok - nothing reachable, nothing broken')
  else
    print(string.format('sandbox: FAIL - %d escape(s) reachable, %d kept function(s) missing',
                        leaked, missing))
  end
end)
