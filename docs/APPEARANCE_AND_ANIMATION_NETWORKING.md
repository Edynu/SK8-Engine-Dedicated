# Appearance and animation networking

How a player's *look* and a player's *motion* reach other clients, and why
each one is built the way it is. Both were reworked after a long run of
"that player is just a dark box" bugs; the reasoning below is the part that
is easy to lose and expensive to rediscover.

The short version: **the server is authoritative and clients talk only to
it.** No client sends anything to another client, and nothing on the wire is
addressed to one specific recipient.

---

## 1. The rule both systems now follow

The dedicated server is a **broadcasting** server. When a client sends a
datagram, the server forwards it to every peer that should see it
(`VisualRelayRouter::RouteRaw`, filtered by map, routing bucket and
`sv_radius`). The envelope has no "intended recipient" field.

That single fact drives everything else:

> If a client encodes something differently for different recipients, every
> recipient still receives *all* the variants and cannot tell which one was
> meant for it.

Both subsystems used to violate this, and both produced the same visible
symptom — a peer rendering as a collapsed dark box — through completely
different mechanisms.

---

## 2. Animation: one broadcast stream

### What it does now

Each client encodes its skeleton **once per pose** and sends **one** copy to
the server, which fans it out. All receivers get identical bytes: one
keyframe/delta chain, one envelope sequence counter, one shared baseline.

State lives in `BroadcastAnimationStream` (`src/skate3_multiplayer.cpp`).

### Why — the bug this replaced

Previously `SendAnimation` built a **separate payload per recipient**: a full
keyframe for a peer that had just joined, deltas for peers already in sync.
Sensible for peer-to-peer, wrong here. Three things combined:

1. Per-recipient payloads (keyframe for the newcomer, deltas for the rest).
2. A **per-recipient envelope sequence counter**
   (`peer_control_[role].v12_animation_send_sequence`), which **starts at 0**
   when that peer is first seen.
3. The server broadcasting every variant to everyone, with no target field on
   the wire.

So when client 3 joined an established 1↔2 session, sender 1's counter for
peer 2 was already in the thousands while its counter for peer 3 started at
0 — and **both streams were delivered to client 3 on the same channel**.
`ReceiveHistory` is a 32-packet sliding window, so the established stream
pushed `latest_` into the thousands and client 3's own packets, arriving at
sequence 1, 2, 3…, were discarded as `kTooOld` **before reassembly, forever**.

Meanwhile the foreign variants it *did* accept referenced baselines it had
never been sent, so it requested a baseline endlessly and never rendered.

This is why **it was always the clients who joined later**. Two clients
worked perfectly (one recipient, one counter). The brief "everything looked
right when client 4 connected" was a burst of forced keyframes momentarily
landing.

Diagnostic signature in the logs: `v12_anim_rx` climbing into the thousands
while `v12_anim_complete` stays frozen at 1, `v12_anim_rejected` at 0 (the
"missing baseline" path returns success and silently discards), and
`v12_baseline_request_tx` climbing forever with no progress.

### The trade-off accepted

One shared chain means **one peer asking for a baseline gives everybody a
keyframe**. That is cheap at these player counts and self-correcting.
Per-recipient chains would require a target role on the wire, which the
40-byte envelope has no room for.

### Related fixes made along the way

Real bugs, kept, but none of them were the root cause:

- `PoseReceiverState::ScheduleBaselineRetry()` had **no callers anywhere**.
  Once a baseline request or its reply was lost, `recovery_active_` stayed
  set with no request in flight and `RequestBaseline()` is a no-op in that
  state — so the peer never recovered. `PrunePeers` now re-arms it after
  750 ms, but only while the reassembler has no slot in progress, because
  every `kRequestBaseline` makes the sender discard whatever it was mid-way
  offering.
- `PoseGroupReassembler::OldestSlotIndex()` was plain LRU. Under loss an
  in-progress **baseline** could become the oldest slot and be evicted by an
  ordinary delta — discarding the one group needed to unblock decoding. It
  now prefers evicting non-baseline slots.

---

## 3. Appearance: server-held, server-scoped

### What it does now

1. A client **uploads its own appearance once per identity** to the server
   (`POST /api/appearance/<hex>?role=N`). An identity is a content hash, so
   re-uploading the same look is free and two players wearing the same thing
   are one stored blob.
2. The server **saves it** (`skate3::appearance_store::AppearanceStore`).
3. Each client **long-polls** `GET /api/appearance?role=N&since=V&wait=1`.
   The server answers with *who that client can currently see and what they
   are wearing*, and parks the request until that answer actually changes.
4. The client downloads any blob it does not already hold and installs it.

No appearance bytes travel over UDP, and no client sends an appearance to
another client.

### Why the server decides visibility

The roster is computed **per requesting client** in `VisibleAppearances()`
(`tools/relay/skate3_dedicated_main.cpp`) using the *same* rules the packet
router applies: same map hash, same routing bucket, within `sv_radius`. A
peer the server would not forward packets from is a peer whose appearance
that client has no business downloading.

It reads the `PlayerRegistry` snapshot rather than the router directly,
because the relay loop mutates the router on its own thread while this runs
on an HTTP worker thread.

Previously the endpoint returned a **global** roster and each client filtered
it against its own peer list. That meant a client could only fetch an
appearance for a peer it had already received packets from — a race at join
time. Now the server offers it as soon as the peer is visible, whether or not
any packet has arrived yet.

### Why long-polling, and why the version is a hash

The version is an FNV-1a hash of the client's own answer
(`HashAppearanceRoster`), not a global counter.

A global counter would be bumped by every position update — which happens
every tick — so every parked request would wake immediately and the long
poll would degenerate into a busy loop. Hashing the answer means a wake
happens only when something *that client can see* actually changed: someone
joined, left, changed outfit, or crossed its visibility boundary.

Budget is 20 s server-side, re-evaluated every 200 ms; the client's read
timeout is 30 s so a parked request is never mistaken for a dead server.
The HTTP thread pool is 64, so parked requests do not starve other routes.

### Why the peer-to-peer UDP transfer was deleted

It was **still running in parallel and winning every race**, which made the
entire server store dead code. Measured in `skate3_327.log`:

```
appearance from server store  : 0
received appearance (UDP)     : 2
```

UDP delivered ~1.4 s after join. The roster poll did not fire until 1 s in
and then skipped anything the client already held — so the store path never
transferred a single byte, in any session, ever. Its upload had likely never
run end to end.

The UDP path was also the wrong shape for this server, for the same reason
animation was: `SendAppearance` built **per-recipient** reliability state
(`outbound_appearance_[target_role]`) and sent one copy per peer to a
broadcasting server. Its own source comments record the bugs that caused,
including *"observed as 'the second client always sees a box'"*.

Removed: the chunked fanout send, the chunk receive/assembly, the per-peer
retry/resend state, the appearance-request control message, and the assembly
timeout/recovery machinery.

Kept deliberately:

- `kAppearanceState` control messages ("I have installed your look") are
  still received and counted. They no longer stop a retry loop — there is no
  transfer to stop — but they remain useful telemetry.
- `kAppearanceRequest` from an older client is counted and ignored.
- Incoming appearance **chunks** are dropped rather than assembled. Accepting
  them would reintroduce exactly the race that kept the store unused.

---

## 3b. Player names ride the same roster

A remote player's **display name** now comes back in the same per-viewer
roster as their appearance:

```json
{"version":123,"peers":[{"role":3,"id":"6BEA...","name":"Edynu(2)"}]}
```

### Why it moved there

Names used to reach clients **only** as a one-shot `skate3:playerNamed`
script event, broadcast at the moment a client POSTed its name to
`/api/players/name`. Any client that was not listening at that instant —
still loading, or simply not yet connected — never learned the name and fell
back to `Player <role>` **for the rest of the session**, with nothing to
correct it. That is what the nameplates were showing.

The roster answer is re-sent whenever it changes and is scoped to peers the
viewer can actually see, so it has the same delivery guarantee appearances
now have. `lua_client::PlayerName(id)` checks the script-event cache first
and falls back to the server's roster, so both paths work and the roster
repairs anything the event missed.

The server remains the authority on identity, including the `(2)` suffix it
appends when two connections present the same name.

### Still required: the client must post a name

`SyncResourcesFromServer` skips the POST entirely when the name is empty, and
`skate3_multiplayer_player_name` defaults to empty. A client launched
**without** `--skate3_multiplayer_player_name=...` therefore has no name for
anyone to show, and will still read as `Player <role>`.

Of the launch scripts, only these set it:
`Launch-Local-Dedicated.bat`, `Launch-Local-Dedicated-TwoClients.bat`,
`Perf-Test-A-OnlineRulesOff.bat`, `Perf-Test-B-NoRelay.bat`.

## 4. What each side owns

| Concern | Owner |
|---|---|
| Who can see whom | Server (`VisibleAppearances`, `RouteRaw`) |
| Appearance bytes | Server (`AppearanceStore`, content-hash keyed) |
| When a client learns of a peer's look | Server (long-poll wakes on change) |
| Player display names | Server (dedup + per-viewer roster) |
| Pose / animation encoding | Client, but **one stream for everyone** |
| Fan-out | Server, always |

---

## 5. If a peer renders as a dark box again

Check in this order:

1. `multiplayer-role-health` — if `skipped` is at 12.00/frame with
   `missing_mesh=0` and `degenerate_bones=0`, bones are not resolving, so it
   is an **animation** problem, not appearance.
2. `v12_anim_complete` in `multiplayer-net`. Frozen while `v12_anim_rx`
   climbs means pose groups arrive but never decode — suspect the baseline
   chain.
3. `appearance-service:` lines. Absent entirely means the store is not being
   reached; a `server store unreachable` warning names which half failed.
4. Whether it is **only later joiners**. That asymmetry always points at
   per-recipient state meeting a broadcasting server.

## 6. Not yet verified by tests

The test suite could not be executed in the environment these changes were
made in — every test binary fails to launch with `[operation not permitted]`,
including ones unrelated to this work. These changes are build-verified and
reviewed, not test-verified. The meaningful check is a 4-5 client session.
