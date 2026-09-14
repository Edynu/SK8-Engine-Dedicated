# Load testing with virtual players

Replays one recorded client as many virtual players, so a dedicated server and
a single real client can be put under the load of a full lobby without running
a full lobby.

Two pieces: a recorder in the client (`skate3_multiplayer_capture_outbound`)
and a replayer (`skate3_loadtest`).

## Why replay rather than synthesise

A believable animation stream needs the game's skeleton and its encoder, which
live in the client. Recording one real client doing a manoeuvre gives exactly
realistic packet sizes, keyframe/delta structure and cadence for nothing, and
cannot drift from what the real client actually sends.

Two facts make it work, both load-bearing:

- **The relay derives a player's role from the CONNECTION, not the datagram** -
  *"a client may claim any role it likes in bytes it wrote itself"*
  (`skate3_dedicated_main.cpp`). So a fresh socket is a fresh player, and the
  captured bytes need no rewriting to belong to somebody else.
- **Bone rows go on the wire RELATIVE to the root.** The encoder subtracts
  `root_position` and the decoder adds it back (`skate3_multiplayer.cpp`:
  `rows[axis * 4 + 3] - root_position[axis]`, and its mirror), and the pose
  group header carries no root of its own. So moving a replayed player is a
  matter of rewriting the twelve bytes of position in its root snapshots - the
  whole skater follows - and every other packet is sent verbatim.

---

## 1. Record a capture

In the client, connected to a server, with the F7 console:

```
skate3_multiplayer_capture_outbound capture.sk8cap
```

Now **do the thing you want repeated** - roll forward, kickflip, repeat for as
long as you want the loop to be. Then:

```
skate3_multiplayer_capture_outbound ""
```

The empty string closes the file. The cvar is hot-reload, so no restart either
way, and the file lands next to the exe unless you give an absolute path.

Every outbound datagram is written verbatim with its send time, behind an
`SK8CAP01` magic so the replayer can refuse a file that is not one of these
rather than spraying arbitrary bytes at a server.

**Three things worth getting right:**

- **Record long enough.** 30-60 seconds is plenty. The replay loop reconnects
  at the end (see below), so a very short capture turns into a join/leave storm
  rather than a movement test.
- **Record a representative manoeuvre.** Animation cost is not flat: a trick
  moves far more bones than rolling in a straight line, so a capture of
  someone standing still will under-state the load considerably.
- **Turn it off.** The file grows at roughly the client's uplink rate.

The capture includes the client's own registration and any script/appearance
traffic it happened to send. That is harmless - the replayer registers for
itself and those extra packets are simply part of the load.

## 2. Replay it

```
skate3_loadtest --server=127.0.0.1:27200 --capture=capture.sk8cap ^
                --clients=32 --spacing=8 --axis=x
```

| Flag | Meaning |
|---|---|
| `--server=host:port` | The dedicated server (default `127.0.0.1:27200`) |
| `--capture=FILE` | The file recorded above |
| `--token=TEXT` | `sv_token`, if the server sets one |
| `--clients=N` | How many virtual players (default 8) |
| `--spacing=UNITS` | World units between them (default 8) |
| `--axis=x` or `z` | Which horizontal axis to spread along |
| `--once` | Play the capture once instead of looping |

Players are spread **centred on the captured position**, so the spot you
recorded at stays in the middle of the crowd rather than at one end of it.

**`--axis=y` is accepted and mapped to Z**, with a warning. Y is the vertical
axis in this engine, so spreading along it would stack the players in the air
instead of across the ground - and silently doing that would look like a bug.

### Spacing is the interesting dial

It is what decides which code paths the test actually exercises:

- **Small spacing** (everyone inside everyone's `sv_radius`) is the worst case
  for fan-out: every player's stream is forwarded to every other. This is what
  finds the bandwidth ceiling.
- **Large spacing** spreads players across the fidelity bands, so the medium
  and low tiers and the crowd thinning get used. This is what tells you whether
  those are doing their job.

Test both. A run at only one spacing will look fine for the wrong reason.

### Why the loop reconnects

Each loop **re-registers** rather than rewinding. The animation stream is a
keyframe/delta chain: replaying from the top mid-stream sends a delta
referencing a baseline the receiver never got, and that peer sits there
requesting a baseline and rendering as a dark box - exactly the bug documented
in `APPEARANCE_AND_ANIMATION_NETWORKING.md`.

Reconnecting starts a clean stream, and has a side benefit: it exercises the
join path continuously under load, which is where that bug lived.

## 3. What to watch

On the **server**: `netstat` in its console, or `/api/metrics` - send/receive
rates and loop hitches. The hitch counter is the one that matters; bandwidth
climbing is expected, a relay loop that starts stalling is not.

On the **real client**: `multiplayer-net` for `v12_anim_complete` climbing
alongside `v12_anim_rx` (frozen means pose groups arrive but never decode), and
`multiplayer-role-health` for `skipped`. Frame time with 32 peers in view is
the other half of the answer - that is a render cost, not a network one, and
the two are easy to confuse.

## Limits, stated plainly

- **Every virtual player performs the same manoeuvre in lockstep.** Real
  players do not, so keyframes from all of them land on the same frames. That
  makes the peak burst worse than reality and the average about right - useful,
  but do not read the peak as a real-world number.
- **They do not receive.** Inbound traffic is drained and discarded, so this
  tests the server's fan-out and the real client's ability to render a crowd -
  not whether a virtual player could keep up.
- **No appearance upload.** They will render with whatever the server has for
  them, or nothing. Appearance bandwidth is a join-time cost and is not part of
  this test.
