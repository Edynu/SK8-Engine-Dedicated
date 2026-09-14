# Client security model

What a server you connect to is allowed to do to your machine, and why the
boundaries sit where they do.

The premise is FiveM's: **joining a server means downloading and running that
server's code.** Resources are fetched over HTTP and executed
(`SyncResourcesFromServer`), their NUI pages are server-supplied HTML and
JavaScript rendered in a real Chromium, and their Lua runs in-process. None of
that can be made safe by trusting the server, because the whole point is that
anyone can host one. So the client has to assume the server is hostile and
restrict what the code it just downloaded can reach.

---

## 1. Lua: the standard library is cut down

`RestrictStandardLibrary` (`src/skate3_lua_script_host.cpp`) runs immediately
after `luaL_openlibs` on every resource state.

Removed outright:

| Removed | Why |
|---|---|
| `io` | Arbitrary file read and write. |
| `package` | `require`, and `package.loadlib` — which maps any native DLL into the process. That one function is remote code execution by itself. |
| `require` | Same. |
| `load`, `loadstring` | Accept **precompiled bytecode**. The Lua bytecode verifier is not a security boundary: hand-crafted bytecode is memory corruption, not a Lua error, so it walks straight past everything else on this list. |
| `loadfile`, `dofile` | Execute a file from any path. |
| `os.execute` | Spawn a process. |
| `os.remove`, `os.rename` | Destroy or move files. |
| `os.getenv` | Reads the environment — usernames, paths, tokens. |
| `os.exit` | Kills the game from a script. |
| `os.tmpname`, `os.setlocale` | No legitimate use here; both are process-wide side effects. |
| `debug.*` except `traceback` | Defeats everything above. `debug.getregistry()` alone returns the loaded-module table that removing `package` was meant to hide, and `debug.setupvalue` can rewrite the upvalues the natives are bound with. |

Kept: `string`, `table`, `math`, `coroutine`, `pcall`/`error`, `os.time`,
`os.clock`, `os.date`, `os.difftime`, `debug.traceback` (the thread scheduler
uses it to report a coroutine that died).

Applied on the **server** side too. Those scripts are the owner's own, so the
threat model is different, but nothing shipped here needs the removed
functions and a resource that behaves differently depending on which side
loaded it is worse than one that fails on both.

**Verifying it:** the `SandboxProbe` resource registers `sandboxcheck`, which
prints every escape and every kept function with a verdict. Run it after any
Lua version bump or any change to state setup — this boundary is weakened
silently, not loudly.

Two things were already correct and were left alone:

- **Manifests** are loaded with *no* libraries opened at all. A manifest is
  data; running it with the standard library available would let it touch the
  filesystem just by being parsed.
- **`ReadResourceFile`** resolves with `weakly_canonical` and then proves the
  result is still inside the resource directory. A textual check would be
  fooled by `a/../../secret`; this is not.

## 2. The client's HTTP server is loopback-only

Each client runs its own `AdminHttpServer` so the in-game CEF console and NUI
pages have something to talk to. It exposes `POST /api/console/exec`, which
runs console commands.

It used to `bind_to_port("0.0.0.0")` — while logging that it was listening on
`127.0.0.1`, which is what it was always meant to do. That made it remote
command execution offered to anyone who could reach the player's machine.
`Start` now takes an explicit `Bind`, with no default, so both call sites have
to state the decision:

- **Client** → `kLoopbackOnly`. Nothing remote ever needs it; every user is on
  `127.0.0.1` by construction.
- **Dedicated server** → `kAllInterfaces`. It must be reachable: clients fetch
  resources, appearances and script events from it.

## 3. Loopback is not a trust boundary on a client

Binding to loopback is not sufficient on its own, because **a resource's NUI
page is server-supplied JavaScript served from that same
`http://127.0.0.1:<port>` origin.** A hostile page could simply
`fetch('/api/console/exec', {method:'POST', body:...})` — same-origin,
no preflight, indistinguishable from the player typing it.

So the admin-only routes (`/api/console/exec`, `/api/console/log`,
`/api/resources/<name>/ensure|restart|stop`) are gated by
`IsAdminAuthorized`, in one of two modes:

- **Client — `kTokenOnly`.** A random 128-bit token is generated per run and
  handed to the dev-console page in its URL, and to nothing else. Separate CEF
  browser instances cannot read each other's URLs, so a NUI page has no way to
  learn it. Loopback earns no exemption.
- **Dedicated server — `kLoopbackOrToken`.** Loopback is trusted so the
  owner's own dashboard works with no configuration; a remote admin sets
  `sv_token` and sends it as `X-Skate3-Token`. With no token configured,
  remote access is refused rather than silently open — which is what closed
  the other half of this: the server's admin port is necessarily on
  `0.0.0.0`, so `/api/console/exec` was reachable by **every player who
  joined**.

Routes a connecting client legitimately calls — appearances, script events,
its own name, fetching resources — are deliberately **not** gated. Leaving
`sv_token` unset never stops anyone from playing.

**Known consequence:** the React dashboard in `web-console/src` calls those
three admin routes without a token. It still works against a dedicated server
opened on the server's own machine (loopback). Against a *client* it will now
get `403` on those three calls; the F7 console is the supported path there.

## 4. Not closed — accepted risks

Recorded because an undocumented known gap is worse than a documented one.

- **CEF runs with `no_sandbox = true`** (`src/skate3_cef_browser.cpp`). The
  renderer that parses server-supplied HTML, CSS and JavaScript has no
  Chromium sandbox, so any renderer exploit is straight to full process
  privileges. Enabling it means linking `cef_sandbox.lib`, which pins a
  specific MSVC runtime and has to be reconciled with this project's own
  runtime settings — a real build change, not a flag flip. This is the largest
  remaining hole.
- **No navigation allowlist.** Nothing restricts where a NUI page may navigate
  or fetch, so a page can reach the public internet (exfiltration) and other
  services listening on the player's own loopback. `CefRequestHandler::
  OnBeforeBrowse` plus a resource-request filter is the fix.
- **Site isolation is off** (`disable-site-isolation-trials`,
  `--disable-features=IsolateOrigins,site-per-process`) so all NUI pages share
  one renderer. FiveM does the same for the same reason, but it does mean one
  resource's page shares a process with another's.
- **No cap on what a server may push.** The resource count, script size and
  NUI asset size are all unbounded, so a server can make a client allocate
  until it dies. A disk-space attack is not possible — client resources are
  held in memory and never written out.
- **Lua CPU is not bounded.** A `while true do end` in a downloaded client
  script hangs the game: the scheduler is cooperative and there is no
  instruction-count hook. FiveM has the same property.
