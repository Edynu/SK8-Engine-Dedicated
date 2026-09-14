# Metrics: bandwidth, hitches, Lua resmon

Three console commands and one HTTP route, on both `skate3.exe` and
`skate3_dedicated.exe` (each side reports what it can actually see - the
client has no visibility into the relay's own loop, and the relay does not
run inside a renderer with a frame to measure).

## Console commands

| Command   | Client                                    | Server                                          |
| --------- | ------------------------------------------ | ------------------------------------------------ |
| `resmon`  | every resource's CPU/memory, worst first   | same                                              |
| `netstat` | this client's own upload/download rate     | relay bandwidth, packet counts, loop hitches      |

Type either into the F7 console, the backtick console, or POST to
`/api/console/exec`.

### `resmon`

```
resource                        memory(KB)   ms/s   calls/s
SkateGame                            842.3   1.204        61
Markers                              128.0   0.031         2
Wardrobe                              96.5   0.000         0
2 resource(s), 1.235 ms/s total
```

Backed by `LuaScriptHost::ResourceMetricsSnapshot()`
([skate3_lua_script_host.h](../src/skate3_lua_script_host.h)). Every resource
already owns an independent `lua_State` (see the Lua host design), so memory
has no attribution problem - `memoryKb` IS that resource's own
`lua_gc(L, LUA_GCCOUNT)`, not a share of a shared pool.

CPU time is measured, not estimated, at every place a resource actually runs
script: its thread-scheduler tick (`Skate.CreateThread`/`Skate.Wait`), an
event handler, a console command, and a NUI callback -
`ScopedResourceTimer` wraps each `lua_pcall` and folds the elapsed time into
that resource's current-second accumulator, success or Lua error alike (a
script erroring every frame still costs the time it took to fail). The
accumulator rolls over into `msLastSecond`/`callsLastSecond` once a second,
inside `LuaScriptHost::Tick()` - so a reader never sees a partial number that
happens to catch it mid-second.

The same data is available from a resource itself:

```lua
local metrics = GetResourceMetrics()
print(metrics['SkateGame'].msLastSecond)
```

`GetResourceMetrics` is a **shared** native (client and server both), wired
as a built-in global in `CreateResourceLuaState` rather than a client-only
native in `skate3_lua_client_natives.cpp` - resmon is exactly as useful on
the dedicated server, and adding it once here means no per-side duplication.

### `netstat`

Client:

```
role 2   up 4.2 KB/s (38 pkt/s)   down 6.1 KB/s (41 pkt/s)   peers 3 known, 2 visible   total up 812 KB, down 1204 KB
```

Backed by `multiplayer::FormatNetworkTelemetryLine()`
([skate3_multiplayer.h](../src/skate3_multiplayer.h)) - the same rate
numbers `multiplayer-fanout`/`multiplayer-compression` log lines already
compute every 5 seconds internally (`Runtime::LogRates`), now cached instead
of only ever reaching a log file. "up"/"down" are from this client's own
point of view: up is what it sends, down what it receives - matching the
terms the feature was asked for in, not a networking convention that reads
backwards from a player's perspective.

Server:

```
down 12.4 KB/s   up 38.1 KB/s   hitches/s 0 (worst 1.2 ms)   hitches total 0   total in 4102 KB, out 12854 KB
```

Backed by `FormatRelayMetricsTable()`
([skate3_dedicated_main.cpp](../tools/relay/skate3_dedicated_main.cpp)).
"down"/"up" here are from the RELAY's point of view: down is what clients
send it, up is what it forwards back out - naturally larger, since one
client's packet is typically relayed to several others.

Byte counts on both sides are **offered load** - the size handed to
`sendto`/`recvfrom`, not a confirmation a datagram left the NIC or reached a
peer. UDP gives no such confirmation short of an application-level ack, and
this is a bandwidth gauge, not a delivery guarantee.

## Hitches (server only)

A "hitch" is one relay-loop iteration that took longer than 20ms
(`RelayMetrics::kHitchThresholdMicroseconds`). The loop is otherwise a tight
non-blocking poll - `recvfrom` returns immediately when nothing is waiting,
via `FIONBIO`/`O_NONBLOCK` - so an iteration that long means something inside
it stalled: a script's `__skate_tick`, the O(peers²) `RefreshCrowdCounts`
pass (once a second, see the sweep interval), or a burst of packets large
enough to process in one go. Every player on the relay feels a hitch as
added latency, which is why it is surfaced separately from plain bandwidth
rather than folded into it.

Measured around the WHOLE loop body, deliberately excluding the loop's own
idle `sleep_for(2ms)` (taken only when nothing was received) - that sleep is
throttling, not work, and including it would make every quiet iteration read
as a near-hitch and bury real ones under noise.

`hitches_total` is cumulative and never resets; `hitchesPerSec` (`netstat`'s
`hitches/s`) is the count from the last full second, alongside the worst
single iteration in that window.

## `/api/metrics`

`GET /api/metrics` on either process' dev-console port
(`skate3_lua_admin_http.cpp`). Client:

```json
{
  "network": { "enabled": true, "role": 2, "knownPeers": 3, "visiblePlayers": 2,
               "uploadKibPerSec": 4.2, "downloadKibPerSec": 6.1,
               "uploadPacketsPerSec": 38.0, "downloadPacketsPerSec": 41.0,
               "totalUploadBytes": 831488, "totalDownloadBytes": 1232896 },
  "resources": [ { "name": "SkateGame", "msLastSecond": 1.204,
                   "callsLastSecond": 61, "msLifetime": 4021.5,
                   "callsLifetime": 88213, "memoryKb": 842.3 }, ... ]
}
```

Server: the same `resources` array, plus `relay` in place of `network`
(`bytesInPerSec`, `bytesOutPerSec`, `hitchesPerSec`, `worstIterationMs`,
`hitchesTotal`, `bytesInTotal`, `bytesOutTotal`).

Both are wired through `AdminHttpServer::SetMetricsProvider` - the same
injection shape `SetSettingsProvider`/`SetPlayersProvider` already use, since
`skate3_lua_admin_http.cpp` is deliberately rex-free and shared with the
dedicated server, and must not link either side's actual counters directly.

Rendered by the React dashboard's Dashboard page
(`web-console/src/pages/DashboardPage.tsx`) - stat cards for down/upload,
hitches, and a sorted resmon table, polling every 2 seconds. It reads
whichever of `relay`/`network` the JSON actually has, so the same page works
unmodified against either process. See "The dashboard" below.

## The dashboard

`web-console/`'s React app, served at `/` by both processes' admin HTTP
server (built product checked in at `web-console/dist/`; source in
`web-console/src/`). A **Dashboard** page is now the default landing page,
alongside Console/Active Players/Scripts:

- Download/Upload stat cards, plus a small hand-rolled SVG sparkline of each
  (no charting library - one page needed one, and a polyline is a few
  lines).
- On the dedicated server: a hitch counter, coloured red while
  `hitchesPerSec > 0`, and total bytes relayed.
- On the client: peers known/visible and the session's role, in place of
  hitches (the client has no relay loop to report on).
- A **Resource Monitor** table - the same data `resmon` prints, sorted
  worst-first, each row's CPU time shown as a small orange bar scaled to
  the worst resource that second.

`DashboardPage.tsx` reads whichever of `relay`/`network` the
`/api/metrics` response actually carries, so the identical page renders
correctly against either process with no build-time branching.

The whole app was re-themed orange in this pass (was cyan) - `Sidebar.tsx`'s
accent classes and the one stray `text-cyan-400` in `ConsolePage.tsx`.

**After changing anything under `web-console/src/`**: `npm run build` inside
`web-console/` regenerates `dist/`, which is what actually ships - CMake has
no Node toolchain wired in, so `dist/` is a checked-in build product, not
rebuilt automatically (see the comment above `skate3_stage_web_console` in
`CMakeLists.txt`). A full `cmake --build` then re-stages it next to the exe;
to see a change against an already-built exe without a full rebuild, copy
`web-console/dist/*` into `<build dir>/web-console/` directly.

## Steam P2P has been removed

Not merely unused on the relay path - the capability itself is gone.
`src/skate3_steam_backend.cpp` is now a stub: every function it exports
(`Initialize`, `HostLobby`, `TransportActive`, ...) answers "unavailable"
unconditionally, and nothing in it can load `steam_api64.dll` - no
`LoadLibraryW`, no download, no lobby creation, no Steam Networking
Messages. The header (`skate3_steam_backend.h`) keeps its original shape on
purpose: `skate3_multiplayer.cpp`, `skate3_multiplayer_session.cpp` and
`skate3_app_common.cpp` already treated "Steam unavailable" as a normal,
handled state (it is what happened whenever a player had no Steam client
running), so answering that unconditionally removes the capability with the
change confined to one file - no surgery on `skate3_multiplayer.cpp`'s ~40
call sites in the core transport logic.

Two knock-on fixes were needed because the multiplayer menu used to gate
itself entirely on Steam being available:

- `SimpleMultiplayerControlsEnabled` (rexglue-sdk) used to return
  `steam_available` verbatim, which disabled the WHOLE Multiplayer menu
  category - including **Direct Connect**, the dedicated-server path this
  was explicitly kept for - whenever Steam wasn't running. It now always
  returns true; the parameter is kept, unused, for source compatibility.
- `simple_settings_overlay.cpp`'s Multiplayer tab dropped the
  "Start Steam to use multiplayer" early-exit and the wording that assumed
  Steam was the only backend. Server Browser and Host Game still work -
  they already had a non-Steam fallback (same-PC discovery via the Windows
  registry, `skate3_multiplayer_session.cpp`'s `PopulateBrowser`/
  `HostSession`/`JoinSession`), used for local multi-instance testing and
  unrelated to Steam - only the Steam-lobby half of each was removed.

`HostSession`/`JoinSession`'s Steam branches, `PopulateSteamBrowser`, and
`ParseSteamLobbyId` were deleted outright from
`skate3_multiplayer_session.cpp` rather than left dead (they could never run
again once the stub always reports uninitialized).
