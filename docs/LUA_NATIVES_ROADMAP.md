# Lua natives: where to go next

What the scripting surface looks like today, what is missing, and how to decide
what to add — in priority order, with the reasoning attached so the ordering can
be argued with rather than just followed.

## Where things stand

| Surface | Count |
|---|---|
| Client natives | ~70 |
| **Server natives** | **6** (`GetConvar`, `GetPlayerName`, `Get`/`SetPlayerRoutingBucket`, `GetSkater`, `GetSkaters`) |
| Events raised into Lua | 4 (`playerNamed`, `playerRoster`, `trickLanded`, `trickCancelled`) |

That table *is* the finding. The client surface is in decent shape; the server
has almost nothing. A server developer writing a game mode has to put the rules
on the client — where every player's machine is free to disagree — because the
server cannot see or change enough to arbitrate. Closing that gap is worth more
than any individual new native.

The second finding: **the game already knows things Lua cannot ask for.** Score,
for instance, is fully reverse-engineered in `src/skate3_trick_types.h`
(`ScoreHolderLayout` — cumulative air reward, pending components, multiplier) and
exposed to nothing. Natives like that are plumbing, not research, and they are
where to start.

---

## How to decide what is worth adding

Two questions, in this order.

**1. Does a resource currently fake it?** The best evidence is a workaround
already in the tree. `resources/Markers` computes `WorldToScreen` per frame and
draws icons in an HTML overlay, because there is no world-space drawing native.
That is the honest signal that something is missing — and also proof the
workaround is good enough, which demotes it.

**2. Can it be cheated if the client owns it?** Anything a game mode's *rules*
depend on — did that trick land, what did it score, where is the player —
belongs on the server, or at minimum needs a server-side check. Anything
cosmetic can live on the client forever. Player movement stays
client-authoritative on purpose; that is a deliberate trade, not an oversight.

A native that fails both questions is a wishlist item. Plenty of useful-sounding
natives fail both.

---

> **Status update.** Tier 1a (score) and the marker work are **done**:
> `GetSkaterScore` reads retail's live score block directly (sequence score
> mid-trick, the 1x-4x multiplier, momentum, line), and `CreateMarker` /
> `DestroyMarker` / `SetMarkerText` / `SetMarkerPosition` / `GetActiveMarker` /
> `GetMarkerTextures` give native in-world markers drawn as depth-tested
> billboards, with retail's own icons extracted offline and embedded. Player
> nameplates moved onto the same billboard system, which took ImGui out of the
> shipped rendering path. What remains in Tier 1 is bail/respawn events, the
> control lock, and the server-side authority group.

> **Superseded in part by [NATIVE_DISCOVERY.md](NATIVE_DISCOVERY.md).** A pass
> over the retail image found named, addressed functions for score, the 1x-4x
> multiplier, Hall of Meat, objectives and real in-world markers — so several
> items below are now "call a known address" rather than "find out whether this
> is possible". The tiering here still holds; the effort estimates were
> pessimistic.

## Tier 1 — unblocks the S.K.A.T.E. mode

These are the ones where offsets are already known or the data already exists in
a struct on the right side of the wire. Mostly plumbing.

### 1a. Score — `GetSkaterScore()`, and a score event

`ScoreHolderLayout` and `ScoreModuleLayout` are mapped already. A skate game mode
cannot adjudicate anything without a number, and right now `trickLanded` gives a
name with no value, so a mode can tell *that* something landed but not whether it
beat the other player.

Start with the simplest thing that works: read the cumulative reward at the point
`trickLanded` already fires, and add it to that event's payload. That is one
field on an event that already exists, versus a new native plus a polling loop in
every mode.

**Risk, stated up front:** the roadmap already flags trick identification as the
single riskiest assumption in this whole plan, and score attribution sits right
next to it. Verify that two identical tricks produce the same number, and that a
combo does not smear its score across the tricks inside it, *before* building
rules on top.

### 1b. Bail, land and respawn events

There is no bail hook anywhere in the tree (`grep -i bail` finds nothing). For a
turn-based mode a bail is not an edge case — it *is* the fail condition. Without
it a mode has to infer failure from `IsOffBoard` flapping, which will misfire.

Wanted: `skate3:bailed`, `skate3:respawned`. `IsOffBoard`/`IsAirOffBoard` already
exist, so the state is observable; this is about getting an edge-triggered event
instead of a polled predicate.

### 1c. Control lock — `SetPlayerControlEnabled(bool)`

Also absent. A turn-based mode needs the waiting player to stop skating, and
today the only lever is `SendPadInput`, which fights the player rather than
stopping them.

Worth doing properly rather than as a neutral-input spam loop, because "the
controls went weird for a second" is exactly the kind of bug nobody can
reproduce.

### 1d. Server-side authority

The registry already holds position, bucket and name per player
(`PlayerRegistry::Player`), and `GetSkater(id)` already returns them — so the
server can *read* fine. What it cannot do is act:

- `SetPlayerCoords(id, x, y, z)` — put a player at the marker. Movement stays
  client-authoritative, so treat this as a request the client honours, not a
  teleport the server can guarantee.
- `DropPlayer(id, reason)` — no way to remove anyone today. Needed the first time
  a server is public.
- `SetPlayerControlEnabled(id, bool)` — 1c, addressed from the server, which is
  where a turn order actually lives.

This is the highest-value group for "server developers" specifically, because it
is the difference between a mode that runs on the server and a mode that merely
reports to it.

---

## Tier 2 — general server-dev quality of life

Worth doing, but no mode is blocked on them.

- **Chat / notification natives.** Every message today goes through a resource's
  own NUI page, so two resources that both want to say something have no shared
  place to say it.
- **`GetPlayers()` server-side** returning the full table in one call, rather
  than `GetSkaters()` ids plus a `GetSkater()` per id. Trivial, and it removes a
  loop from every mode.
- ~~**World-space drawing.**~~ **Done.** The native billboard pass
  (`skate3_world_markers.h`, `marker.hlsl`) draws world-space quads in the scene
  pass, in two depth variants: depth-tested for markers, depth-disabled for
  nameplates. The `WorldToScreen`-plus-NUI pattern in `resources/Markers` is now
  the legacy approach.

## Tier 3 — research first, natives second

Do not budget these as implementation work. The unknown is whether the game
exposes the concept cleanly at all, and the answer may be no.

- **Per-trick score attribution** inside a combo (see 1a's risk).
- **Emote / animation triggering** — the custom-trick and motion-graph work
  (`skate3_custom_trick.h`, `SelectMotionGraphPath`) is the closest existing
  thread to pull.
- **Server-directed camera** for spectating a turn. `CreateCam`/`MoveCam`/
  `LookAtCam` already exist client-side, so this may turn out to be Tier 1
  plumbing once someone checks.

---

## How to research one of these

The tooling for this already exists and is the fastest path in:

1. **`resources/Probe`** — `findstr`, `findref`, `readu32`, `readstr` against
   live guest memory, plus `cacnames`. Retail registers script bindings under
   readable names, so the names are in memory and the tables pointing at them can
   be found from there.
2. **`src/skate3_trick_types.h`** — the layouts already recovered. Check here
   before probing; a surprising amount is already mapped and simply unexposed.
3. **`StartCoverage`/`StopCoverage`** — narrows down which generated functions
   actually run during a given action, which turns "where is bail handled" from a
   search into a diff.
4. **Instrument from inside, don't drive from outside.** Synthetic input into
   `skate3.exe` has misled us before; add an observation scope and log what
   retail actually did.

When a native lands, extend a resource in `resources/` in the same change. Every
native in the current 70 that has a caller is one we know works; the ones without
are the ones that quietly rot.
