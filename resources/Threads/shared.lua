-- Runs on both sides. Reports how far each wake actually landed from the
-- time it asked for: a wait is never SHORTER than requested, but it can
-- overshoot by up to one tick, so this is the honest measure of resolution.
function RunThreadDemo(side)
    print(string.format('%s: scheduler demo starting at t=%d', side, Skate.GetGameTimer()))

    -- A thread that wakes on an interval and then finishes on its own.
    Skate.CreateThread(function()
        for i = 1, 3 do
            local before = Skate.GetGameTimer()
            Skate.Wait(500)
            local drift = Skate.GetGameTimer() - before - 500
            print(string.format('%s: interval tick %d (asked 500ms, drift %+dms)',
                side, i, drift))
        end
        print(side .. ': interval thread finished')
    end)

    -- Wait(0) yields to the next tick. Counting them measures tick rate.
    Skate.CreateThread(function()
        local ticks, started = 0, Skate.GetGameTimer()
        while Skate.GetGameTimer() - started < 1000 do
            ticks = ticks + 1
            Skate.Wait(0)
        end
        print(string.format('%s: %d ticks in ~1s', side, ticks))
    end)

    -- One-shot, and it spawns another thread from inside a thread.
    Skate.TimeOut(1500, function()
        print(side .. ': TimeOut fired at t=' .. Skate.GetGameTimer())
        Skate.CreateThread(function()
            Skate.Wait(250)
            print(side .. ': nested thread ran')
        end)
    end)

    -- A thread that dies must not take the scheduler (or the resource)
    -- with it: everything above still runs after this.
    Skate.CreateThread(function()
        Skate.Wait(750)
        error('deliberate error from a thread - the rest must keep running')
    end)
end
