# Skate 3 Dev Console

A tiny, dev-only, no-auth web UI (React + TypeScript + Tailwind, via Vite)
for ensuring/restarting/stopping Lua resources loaded by `skate3.exe` /
`skate3_dedicated.exe`. Modeled loosely on FiveM's txAdmin, but far smaller
in scope - single page, no login, polls the game's own embedded admin HTTP
API every 2 seconds.

This is a fully separate build step from the main C++ project - it is not
wired into CMake, so Node/npm is never required to build `skate3` or
`skate3_dedicated`.

## Build

```
npm install
npm run build
```

This produces `dist/`. Copy the *contents* of `dist/` into a folder named
`web-console/` next to the built executable (e.g.
`out/build/relwithdebinfo/web-console/index.html`, not
`.../web-console/dist/index.html`) and the game's embedded admin HTTP
server will serve it directly at `http://127.0.0.1:27180/` (client) or
`http://127.0.0.1:27280/` (dedicated server, `--admin-port=` to override).
The API (`/api/resources`) works even without this step - only the UI
needs the built static files.

## Develop

```
npm run dev
```

Runs Vite's dev server with hot reload; `/api` calls are proxied to
`http://127.0.0.1:27180` (the running client's admin port) so you can edit
the UI without rebuilding `dist/` each time. Update the proxy target in
`vite.config.ts` if you're pointing at `skate3_dedicated` instead.
