# What the game can actually give us

A discovery pass over the retail image, aimed at answering "what natives are
possible" with addresses instead of guesses. Read this before planning native
work; [LUA_NATIVES_ROADMAP.md](LUA_NATIVES_ROADMAP.md) has the prioritisation,
this has the evidence.

Everything below came out of the offline image
(`tools/guest_image.pl`, `tools/resolve_bindings.pl`) plus the existing coverage
captures. No game launches were needed.

---

## 1. The unlock

Three facts that together change what is worth attempting:

**Every retail function is already a callable C++ function.** `generated/skate3_init.h`
declares **47,652** of them (`DECLARE_REX_FUNC(sub_825BDAF8)` and friends). There
is no FFI to build and no address to resolve at runtime — the recompiler already
emitted them all as linkable symbols.

**853 of them have names.** `docs/frontend_bindings.md` is the full list of what
retail's own Flash UI can call into C++ with, resolved to this build's addresses.
That file is the menu. Anything the retail HUD or menus can display, one of these
returns.

**The call mechanism already exists and is proven.** `src/skate3_flash_bridge.cpp`
shows the pattern end to end: copy the `PPCContext`, set `r3..r10` per the PPC
ABI, call `sub_XXXXXXXX(ctx, base)`, read the result out of `ctx.r3`.

So the question stops being "can we reach this" and becomes "which of the 853,
and how do we call it safely".

### The one real constraint

Calls into retail must happen **on the guest thread at a safe point**. The flash
bridge does this by arming a request and applying it during a physics tick, and
that is not incidental — retail functions expect a live guest stack in `r1`.
A native that calls retail cannot simply run wherever Lua happened to call it.

**Which means, for simple getters, prefer reading the memory over calling the
function.** Worked example — `GetSequenceMultiplier` (`0x825C2950`), disassembled
from the image:

```
lis    r11, 0x8306        ; \
lis    r10, 0x8222        ;  |
addi   r9,  r11, 0x7060   ;  > global at 0x83067060
lfs    f13, 0x49B4(r10)   ; a float constant
lwz    r11, 0x10(r9)      ; object pointer at +0x10
lfs    f0,  0x8C(r11)     ; THE MULTIPLIER
fcmpu  cr6, f0, f13
```

The multiplier is `*(float*)(*(ptr*)(0x83067060 + 0x10) + 0x8C)`. Reading that is
thread-safe, allocation-free, cannot reenter retail, and is exactly what the
existing `ReadGuestU32` machinery already does. Reserve actual calls for things
with side effects.

---

## 2. Confirmed reachable, with addresses

Verified: each address below was decoded from the image and is real code.
`IsOverMarker` (`0x825AE5F0`) is a textbook no-argument bool — `mflr`, one call,
`cmpwi`, return 0 or 1 in `r3`.

### Score and multiplier

| binding | address | what it gives |
|---|---|---|
| `GetCurrentScore` | `0x825BDAF8` | score right now |
| `GetCurrentSequenceScore` | `0x825C2888` | the in-progress trick sequence — **readable mid-trick** |
| `GetSequenceMultiplier` | `0x825C2950` | the 1x–4x multiplier |
| `TrickDisplay_GetSequenceMultiplier` | `0x8265D9E8` | what the HUD shows |
| `GetMomentumScore` | `0x825C28E0` | |
| `GetLineScore` | `0x825C28F8` | |
| `GetPlayerScore` | `0x825AB5A0` | |

This covers the whole ask: current score at any moment, the in-flight sequence
score before it lands or fails, and the multiplier. Also present:
`GetFormattedCurrentSequenceScore`, `GetFormatLineScore`, `GetTeamScoreAsInt`,
`SetScoreBiasMultiplier`.

### Hall of Meat

Richer than expected — full per-bone damage, not just a total:

`HOM_GetScore` (`0x8265FE70`), `HOM_GetMultiplier` (`0x8265FED0`),
`HOM_GetBrokenBonesArray` (`0x82662B78`), `HOM_GetBodyDamageArray`
(`0x82661A10`), `HOM_GetBodyDamageMultArray`, `HOM_GetBonusScores`,
`HOM_GetBonusCollisionScore`, `HOM_GetBonusesArray`, `HOM_GetHOMRecord`,
`HOM_ResultsGetBoneScore`, `HOM_GetSecondsToAnimatedBones`.

21 `HOM_*` bindings in total.

### Markers — the real ones, including setters

This is the answer to faking markers. Note these are **setters**, not just
getters, which is unusual in this list:

| binding | address |
|---|---|
| `IsOverMarker` | `0x825AE5F0` |
| `SetSessionMarker` | `0x825AE748` |
| `RemoveSessionMarker` | `0x825AE788` |
| `IsActiveSessionMarker` | `0x825AE7C8` |
| `CanUseSessionMarker` | `0x825A5710` |
| `CanPlaceSessionMarker` | `0x825A5798` |

The existing `TeleportToSessionMarker` native already lives in this
neighbourhood, so the surrounding machinery is partly understood.

**Corroborated against the coverage captures.** Diffing
`coverage_marker.txt` against `coverage_nomarker.txt` (19,990 vs 17,683
functions) leaves 2,331 functions that run only while standing over a marker —
and exactly one is a named binding: `SignUp_GetChallengeTitle` (`0x825C1020`).
That is independent confirmation that the marker-over state drives the challenge
sign-up flow, which is the hold-D-pad-up path.

### Challenges and objectives

721 distinct challenge-related strings; 32 `Objective_*`/objective bindings.
Useful ones: `GetSignUpMissionType` (`0x825C0C78`), `GetSignUpMissionPath`
(`0x825C0D20`), **`SetSignupSelection` (`0x825C14F8`)** — a setter, so the
selection is drivable — `GetObjectiveStatus` (`0x825BC560`),
`Objective_GetObjectiveValue`, `Objective_GetScoringData`,
`GetCurrentChallengeObjectives`, `CanSignUpToChallenge`.

Also `GetCurrentPlayerPosition` (`0x825BDA68`).

### Menu actions (the start menu / crossbar)

`out/build/relwithdebinfo/dump_actions.json` is 4,000 entries of the crossbar
action table — 598 unique strings, 389 of them `ID_*` localisation keys. The
actionable half includes `ChallengeMap`, `ChallengeObjectives`,
`OnlineChallengeMap`, `OnlineStartChallenge`, `RestartChallenge`,
`QuitChallenge`, `SelectActivity`, `SoloFreeskate`, `EnterSkatePark`,
`LoadParkFromFile`, `SavePark`.

Each row is `{at, value, text}` — a table slot, a pointer, and the string — so
the table is directly editable in principle: **hiding or renaming a menu entry is
a write to a known address**, not a UI rewrite. Adding one is harder (the table
has a fixed size) but repurposing an existing entry is not.

Input actions are in the same table as `AptDUp`, `AptDDown`, `AptSelect`… — the
D-pad path into the Flash UI, which is what marker activation rides.

### Retail art: extracted, not captured

**Solved, and the answer was offline extraction.** The first attempt pointed
markers at the renderer's runtime texture capture (`g_r.tex_store`, keyed by
fetch words). It works, and it is useless in practice: front-end art is unloaded
with its screen, and retail challenge markers do not exist on most maps, so the
texture is absent exactly when a game mode wants it.

The format chain, each layer verified against this build's own data:

| Layer | What it is |
|---|---|
| `data/big/fetexture.big` | EB v3 archive - the same container `skate3_cac_archive.cpp` reads for clothing |
| entry method `1` | plain RefPack (`10 FB`, 3-byte big-endian size). The existing reader accepts only methods 0 and 2, so it would **reject this archive** as-is |
| `*.rx2` | RenderWare 4, Xbox 360 (`RW4xb2`): 48-byte header, section table at `+0x30`, payload at `+0x44` |
| payload | DXT5 (BC3), Xbox 360 tiled and 16-bit byte-swapped |

The detail that mattered: **each 16 KiB allocation holds a 32x32 icon.** The 360
pads a tiled texture up to whole 32x32-BLOCK tiles, so a 32x32 DXT5 image (8x8
blocks, 1 KiB) occupies 16 KiB. `GetTiledOffset2D` aligns the pitch to 32
internally, so passing the real block extent is enough and no crop is needed.

**The dictionary is the authority, not a pattern.** The first version of the
parser inferred spans from a repeating shape in the section table and found 18
textures. There are **26**. The real structure is `entry count` (at `+0x20`)
24-byte entries at `+0x30`, ending exactly at the payload - verified:
`0xD48 + 54*24 == 0x1258`. Entries alternate texture DATA (`0x00010031`) and
texture INFO (`0x000200E8`), plus one TOC (`0x00EB000B`).

Two things come out of doing it properly:

- **Names.** The TOC carries a name per texture
  (`challengesignup_icon_textures
.Texture`), so icons are identified by their
  Flash character id rather than by eyeballing a numbered list.
- **Real dimensions and format**, from the Xbox 360 fetch constant at
  `info->pointer + 28`: width `(w[2] & 0x1FFF) + 1`, height
  `((w[2] >> 13) & 0x1FFF) + 1`, format in the low 6 bits of `w[1]`
  (20 = DXT5). Previously these were assumed - which happened to be right here
  and would have silently produced garbage on any asset that differed.

Mind the two pointer bases: DATA pointers are relative to the payload, INFO and
TOC pointers are absolute within the resource header. Mixing them up reads zeros
and yields a plausible-looking 1x1 texture.

**Credit:** the dictionary layout, TOC walk and fetch-constant fields were
cross-checked against the MIT-licensed reader in
[SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine)
(`tools/vendor/skate3_ui/rw4.py`, `xenos.py`). That project is an unrelated
from-scratch Rust/Bevy reimplementation, but its format layer covers the same
archives. One deviation: its reader takes the texture format from bits 26-31 of
word 1 and bit-reverses it, which reads 0 for these assets, while the low six
bits give 20 (DXT5) and match what the payload actually decodes as - so the low
bits are used here, and the value is checked against the known format codes
rather than trusted.

`tools/extract_rw4_icons.py` runs the whole chain and emits
`src/generated/skate3_marker_icons.h`. 18 icons from
`data/fe/source/controls/challengesignup_icon.rx2`, the first being the camera
icon the challenge HUD shows. Committed output, so a build never depends on the
user's installed game.

Two things this unlocks beyond markers: the same tool points at any `.rx2` in
`fetexture.big` (`--list` prints the archive), and the untile + DXT5 decode is
now written down in a readable form, which is the part that would otherwise have
to be rediscovered.

### Teleport locations

`GetTeleportOverlayTitle`, `GetTeleportOverlayIconString`, `GetTeleportTitle`,
`GetTeleporterPopupTitle`, `GetTeleporterPopupDescription`, `IsWorldSpawner`,
`IsSpawnerPresent`. So the teleport overlay is a real system with a queryable
list, which is the natural home for server-supplied spawn points.

---

## 3. Reachable, but needs runtime probing first

These have no named binding, so the offline image cannot finish the job.

### Raycasting

Only three strings exist — `PhysicsLineTests` (`0x821760F4`), `VolumeLineTest`
(`0x82069204`), `ClusteredMeshLineTest` (`0x821BEF4C`) — and `xref` finds **no
references to any of them**. That is the known limitation in
`tools/guest_image.pl`: it matches `lis`+`addi` pairs and misses literals reached
through a long-lived base register, which is how these (memory-tag or RTTI
names) are referenced.

So raycasting needs the runtime route: `findref` from `resources/Probe`, or a
coverage diff. The physics engine is Havok-shaped, so the line-test entry points
will be virtual calls, not global functions — expect vtable work.

### Blips and the radar

`MiniMapManager`, `MiniMapComponent`, `MiniMapTexture`,
`Sk8ChallengeSystem::MiniMapManager`,
`Sk8ChallengeSystem::MiniMapEventServiceProvider`, plus real shaders
(`minimap_PixShader.fpo`, `minimap_VtxShader.vpo`) and `minimap_effect`.

So a minimap system genuinely exists — but it is C++ classes with **zero
bindings**, because retail's minimap is not driven by Flash. `AddBlip`/`RemoveBlip`
is therefore the most speculative item on the list, and the one most likely to
need real reverse engineering rather than a call. The
`MiniMapEventServiceProvider` name is the thread to pull: an event-driven
provider implies blips are *published* to the minimap rather than drawn by it.

Note this is now the only remaining "can we even reach it" item of the three:
raycasting needs a runtime hunt, but the art question was settled by offline
extraction (above), so a blip's *icon* is no longer a blocker - only the
publishing mechanism is.

### Freezing the player

No binding contains `freeze`, and nothing matches `Enable.*Control`/`SetControl`.
Every `Is*Enabled` binding is a UI-state getter.

Worth restating the requirement, because it rules out the easy options: movement
must stop while the **controls stay live**, and forcing position every tick is not
acceptable. So this is not `SendPadInput` spam and not a position clamp — it
means finding where the skater's velocity or its ActionGraph input is applied and
gating that. `src/skate3_trick_pipeline.h`'s
`ActionGraphInputFillObservationScope` is already sitting on exactly that
boundary and is the obvious place to look first.

---

## 4. Honest limits

- **These bindings are front-end getters: they read the LOCAL player's state.**
  They are client-side by nature. Score and multiplier natives make a client able
  to *report* its score; they do not make the server able to verify it. Server
  authority still means the client reporting and the server sanity-checking.
- **Nothing here has been called yet.** Decoding proves a function exists and
  what it reads. It does not prove it is safe to call at an arbitrary point, and
  the flash bridge's own comment calls a first run an experiment. Treat the first
  call into any of these the same way.
- **The 47,652 number is not 47,652 natives.** Most are internal. The 853 named
  bindings are the tractable set, and the useful subset of those is maybe 100.
- **`SetSessionMarker` is the session marker**, the one a player places for
  friends — not necessarily the same system as a challenge marker. They share the
  `IsOverMarker` predicate, which is promising but not proof. Verify before
  designing a game mode around it.

## 5. Method, for the next pass

In rough order of cost:

1. **`docs/frontend_bindings.md`** — grep it first, always. It answered score,
   multiplier, Hall of Meat, objectives and markers in one pass.
2. **`tools/guest_image.pl words <addr>`** — decode the function. Often shows the
   answer is a global read, which is cheaper and safer than a call.
3. **Coverage diffing** — arm, do the thing by hand, disarm, `comm -23`. This is
   how the marker/sign-up link was confirmed, and it works where `xref` cannot.
4. **`resources/Probe`'s `findref`** — for runtime-built tables the image cannot
   show.

And the standing lesson from this repo: instrument from inside rather than
driving from outside. `xref` returning zero for three real strings is a reminder
that a negative result from one tool is not evidence of absence.
