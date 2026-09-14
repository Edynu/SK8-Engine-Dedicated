name = "ErrorTest"

-- Deliberately broken handlers, one per path that can fail, so the error
-- reporting can be checked rather than assumed.
--
-- Each command below raises an error on purpose. Every one should print a red
-- SCRIPT ERROR in the console naming the resource, the entry point, the file
-- and line, and the call chain that got there - not vanish into stderr, which
-- is invisible in a WIN32 app, and not report a line number with no path to it.
--
--   errcmd        a command handler that throws
--   errnested     a throw three calls deep (checks the traceback)
--   errevent      an event handler that throws
--   errthread     a throw AFTER a wait (reported by the scheduler, not a call site)
--   errnil        a real mistake rather than an explicit error() call
client_scripts = { "client.lua" }
server_scripts = { "client.lua" }
