# How Skate 3's C++ drives its Flash front end

Found by measurement in this build, not from the sk3o decompile — that is a
different binary whose addresses do not carry over (`sub_82D0AFA0` and
`sub_82590B50` are real here and absent there).

## The bridge

The front end is a Flash/APT movie. C++ drives it by **calling named
ActionScript methods**, looked up in a pointer table of method-path strings:

    table base   0x8302C608      (61 entries, indexed by method id)

    id  5  _global.HUDHooks.UpdateStance
    id  6  _global.ScreenManager.OpenScreen
    id  8  _global.ScreenManager.CloseScreen
    id 10  _global.ScreenManager.SetScreenState
    id 12  _global.ScreenManager.ShowScreen
    id 16  _global.ScreenManager.RegisterTemplate
    id 17  _global.ScreenManager.ShowStall
    id 30  _level0._root.SetScreenSize
    id 50  _global.Load_EnableTag
    id 52  _global.Load_HideTag

Dump the live table with the `Probe` resource's `screentable` command.

## The dispatcher

    sub_825D3B38(method_id, 0, 0, argc, arg1, arg2, ...)

`argc` is in r6 and the arguments follow in r7, r8, ... as **plain pointers**.
Verified against five call sites with 0, 1 and 2 arguments; a string argument
is an ordinary NUL-terminated char* (the `Load_EnableTag` call passes a
pointer to the literal string "false").

A second dispatcher, `sub_8263F320`, indexes the same table but takes the id in
r4 and has no callers anywhere in the recompiled sources.

226 call sites use `sub_825D3B38` with an immediate id. Note that **6, 8 and 12
are never among them** — screen opening is driven some other way, so calling
`OpenScreen` directly is untested territory rather than a copy of what retail
does.

## Why the front-end STATE machine is a dead end

`sub_82D0AFA0` (SetFrontEndState) is called **once per session**, at
press-start, and never again — measured with `GetFrontEndInfo`'s call counter
while navigating into Edit Skater. Screens are not reached through it.

## Finding code by behaviour

Static search for references to these name strings fails: the compiler sets up
one base register and reaches many strings by displacement, so there is no
`lis`/`addi` or `lis`/`ori` pair to match.

What works is the coverage instrumentation already in the recompiled sources.
`covstart` / `covstop <label>` (Probe) capture the basic-block labels that
executed; diffing two captures gives the functions unique to an action. The
labels are not function entries — map them to their enclosing function by
scanning `generated/skate3_recomp.*.cpp` for `MaybeRecordAddress(0x...)` under
each `DEFINE_REX_FUNC`.

Opening Edit Skater this way: 1141 functions unique to the action, narrowed to
187 that are called from already-running code. `sub_825DF470` is one of them
and calls ids 50/52 — the loading tags that accompany the transition, not the
transition itself.

## The menus are data-driven — and every binding is now addressable

Retail's Flash only RENDERS. The content comes from C++ bindings it calls by
name (`GetMenuItems`, `GetNumOptions`, `GetDifficulty`, `FreeSkateOptions`,
`CAC_GetItemUnlockHALID`, ...). So a menu entry is removed by controlling what
C++ returns, not by editing the movie — and a hook on a retail function is
something this project already does routinely, unlike calling INTO retail.

Bindings are registered as:

    sub_82E62B40(object, name_ptr, callback)      callback built by sub_82E62F00

853 registrations were extracted from the recompiled sources by walking each
site with light register tracking (the game sets up one base register and
reaches each name by a different displacement, which is why searching for an
address built as a `lis`/`addi` pair finds nothing). The result is in
`resources/Probe/bindings.lua` as (name address, implementation address); the
names live in the encrypted image, so the running game resolves them — the
Probe resource's `fenatives [filter]` command prints name -> address.

Known addresses in this build:

    GetMenuItems               0x825A52A8
    OnMenuSelect               0x825A4F00
    OnMenuGetItemEnabled       0x825A5078
    GetCICMenuOptions          0x825C1E88
    GetEditSkaterOptions       0x825C39E8
    CAC_GetNumItems            0x825A7308
    CAC_GetItemUnlockHALID     0x825A74A0
    CAC_OnThumbnailSelect      0x825A70B0

`CAC_GetItemUnlockHALID` reads its item index from argument 0, asks the CAC
manager for that item's unlock id, and wraps the result as a script string:

    li r3,0 ; bl 0x82E62E00     get argument 0
    bl 0x82E5F2A8               to integer
    bl 0x824AD240               manager
    bl 0x825FF3E0               unlock id for that item
    bl 0x82E864F8               wrap as the script return value

## Opening a retail screen from code

SOLVED, after two wrong attempts. Screens ARE reached through the front-end
state machine - the earlier conclusion that they were not was drawn from a
gameplay session in which no screen was ever opened.

### How a menu row becomes a screen

Activating a row runs `sub_8261BC30(menu)`:

    tab    = [menu + 368]
    row    = [menu + 372]
    sub_8261E208(menu, tab, row)          is the row enabled
    action = menuObject->vtable[8](tab, row)
    switch (action)                        jump table at 0x8261BD24, 0..48

`action` indexes a table of 20-byte entries at **0x83027558**:

    +0  action name      "MyCareerTeam", "ChallengeMap", "GameSettings", ...
    +4  title string id
    +8  icon name
    +12 special-case id, or -1
    +16 helper text id

49 actions. Dump it with the Probe resource's
`dumpwords 0x83027558 4000 actions`.

THE SKATER EDITOR IS ACTION 25, "MyCareerTeam" - not "EditSkaters", which is
why searching for that name never found anything. Its case is three
instructions:

    r4 = 63 ; r5 = 0 ; r3 = menu ; bl sub_8261AF30

and `sub_8261AF30(menu, state, mode)`:

    [menu + 394] = 1
    feManager = [0x830CFE1C]
    if (sub_82D09A40(feManager)) sub_82D0ABE0(feManager)
    else                         sub_82D0AFA0(feManager, state, mode)

`sub_82D0AFA0` is SetFrontEndState.

### Calling it

    menu = [[sub_824AD240() + 8] + 224]
    sub_8261AF30(menu, 63, 0)

on a guest thread. `retail_ui::RequestSkaterEditor()` queues exactly this;
`OpenCharacterEditor()` exposes it to script.

This needs NO menu context, which is the point: online the game is held in Free
Play, whose menu has no Edit Skaters row to select.

### Two mistakes worth not repeating

- Calling `ScreenManager.OpenScreen` through the ActionScript dispatcher
  (method 6) runs cleanly and does nothing. C++ owns screen transitions; the
  movie only renders. Note that 6, 8 and 12 never appear among the 226 call
  sites of the dispatcher, which was the clue.
- Calling `SetFrontEndState` directly instead of `sub_8261AF30` opens the
  editor but leaves the pause menu unopenable afterwards: the `[menu + 394]`
  store is what tells the menu manager it has handed over. Call the whole
  transition, not its last step - a wrapper usually exists for the bookkeeping
  it does.

### Finding things in guest memory

`dumpwords <addr> <count> <name>` (Probe) writes a run of words to JSON next to
the executable, resolving any that point at a string. Reading a table through
the console loses entries and wastes time; this is how the action table, the
screen registry and the difficulty names were all read.

Coverage diffing found none of this. It records basic-block labels only, so
small functions never appear at all, and the FE plumbing it does surface is the
same handful of functions for every action. What worked was reading the code
that the data pointed at.

## Reading the image offline — and what that changed

The decrypted image is on disk: `sk3o/decompiled/patched_dump/`, mapped at
`0x82000000`. It is **this build**, not the neighbouring Hex-Rays `.c` export
(whose function boundaries disagree — that is what the older warning in
`skate3_guest_probe.h` was about, and it was applied too broadly). Verified
before trusting it:

- the bytes at `0x825FF4C8` decode to exactly the instructions the recompiled
  source shows for `sub_825FF4C8`;
- `"Receipt"`, its NUL, and `"cas/createskater.swf"` sit precisely where the
  constants already in `skate3_retail_ui_hooks.cpp` assumed.

`tools/guest_image.pl` reads it: `words`, `str`, `find`, `xref`, `screens`.

This is a different way of working. Everything before this was measured by
launching the game and probing from Lua — a launch per question, and coverage
captures that go blind on small functions. The whole screen registry, the whole
ActionScript method table and all 853 binding names now come out in a second,
offline. `tools/resolve_bindings.pl` writes them to `docs/frontend_bindings.md`.

One gap worth knowing about, since it wasted a pass: `xref` matches `lis`+`addi`
and `lis`+`ori` pairs, and the binding NAME strings match neither — they are
reached from a base register set up elsewhere. So xref finds globals reliably
and string literals unreliably; for names, join `bindings.lua` against the image
instead.

## Unlocking everything — retail's own switch

Three attempts at the wardrobe failed the same way: the padlock came off and the
item still would not go on. Each was a lie told to the display while the code
that applies an item kept asking someone else.

The someone else is one predicate.

```
sub_825F46C8                 "is this item locked"
  reached as a functor: the 24-byte record at 0x823046E0 holds it at +8,
  which is why no call site names it and searching for callers found nothing

  asked by sub_825FF5E8    -> chooses "locked" over "owned"   (the padlock)
  asked by sub_825FD9F0    -> at 0x825FDB40, before it applies  (SelectItem)

  reads two 16-bit unlock ids off the item (+240, +220)
  0xFFFF in both      -> returns false immediately, nothing to unlock
  otherwise           -> FNV-1 hashes the unlock's name
                         (basis 0x811C9DC5 and prime 0x01000193 are literal
                          in the function) and asks sub_82503AE0

sub_82503AE0                 the unlock query
  first instruction group:  lbz r11,36(r3) ... if nonzero, return true
```

`u8[progressionManager + 36]` short-circuits **every** unlock query, in every
category. `sub_82503B60` next door opens the same way. The constructor
(`sub_825039F0`) zeroes the byte and never writes it again, and nothing else in
the image writes it either — a switch with no owner, which is what a leftover
development flag looks like.

The manager is `[0x83067074]` (`[0x83067060]` is the service table, slot +20),
taken from `sub_825F46C8` itself, which builds `0x83067060` at `0x825F46E8`.

`EnforceUnlockEverything` sets that byte. One byte, in the game's own words, and
the padlock, the blurb and SelectItem all agree — not because three hooks were
talked into agreeing, but because there was only ever one question.

**Runtime only.** The flag lives in a heap object the constructor rebuilds each
launch, so nothing here can reach a save file. That is also why the other route
to the same place — writing career counters until the milestones are genuinely
met — was rejected: it would edit the player's career for real.

One display hook survives, and has to. With the flag set, `sub_825F46C8` answers
"not locked", so an item that used to report `"locked"` now reaches
`sub_825FF5E8`'s fallthrough instead — and a grid of rows reporting `"none"`
renders as a grid of nothing. Both answers map to `"owned"`. The mapping is by
POINTER, not by text: that fallthrough returns `0x82202434` while a genuinely
empty row returns `0x82202418`, two identical strings at two addresses, which is
the only reason they can be told apart.

### Online forces it

`skate3_unlock_everything` governs offline play and is OFF - a career is the
player's own and keeps its progression. An online session sets the flag
regardless, from the first frame, and no script is consulted — the same shape as
the forced Free Play, and for the same reason: a resource must not be able to
hand itself a different game by calling, or not calling, a native. Everyone
joining gets the same wardrobe, which is the fair state rather than a favour.

Launch-scoped (`skate3_multiplayer_relay_active`), not connection-scoped, so
there is no window in which a menu could be drawn with padlocks still on it.
`SetUnlockEverything(false)` online returns `true` rather than failing quietly,
and `IsEverythingUnlocked()` reports `forced` so a script can tell why.
