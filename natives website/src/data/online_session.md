# What an online session changes, and where

Single player is retail's game, untouched. Everything below applies only when a
client is in an online session, and is lifted when it ends. That split is
deliberate: a player's own career is theirs, and online is where everyone has to
be playing the same game.

Nothing here is a Lua resource. These are rules of fair play, so a script must
not be able to grant itself the whole game back by declining to call a native.

## How "online" is decided

Two different signals, because they answer different questions:

| Signal | Means | Used for |
| --- | --- | --- |
| `skate3_multiplayer_relay_active` | this process was LAUNCHED to play online | anything that must be true before the world loads |
| `lua_client::LocalPlayerId() != 0` | currently connected to a server | anything that can change while running |

The launcher (`scripts/Launch-Local-Dedicated*.bat`) passes the first. The second
is set from the relay's register acknowledgement and cleared on disconnect.

## The rules

All in `src/skate3_retail_ui_hooks.cpp` unless noted.

### Free Play is forced — `EnforceOnlineGameMode`

Writes the game-mode global (`0x830B7AE8`, Free Play is 3) from the FIRST FRAME
of an online launch.

The timing is the whole point. Setting it after connecting did not work: by then
the career challenges are loaded, and changing the mode afterwards does not
unload them. Telling the game what it is before anything loads means they are
never loaded at all - no missions, no challenge markers, no minimap blips.

Two earlier attempts are worth not repeating:

- Denying the challenge data (`challenge_local_data\*.vlt`) crashed the game with
  a call through a null function pointer. Removing content the game had already
  decided to load leaves references to it.
- Denying `challenge_local_data_framework.vlt` crashed it too, for the same
  reason `db.big` cannot be denied: it is schema, not content.

`skate3_no_retail_missions` and `skate3_no_retail_challenges` still exist and
both default to OFF. Neither is needed now.

### Retail's menus are trimmed — `SetMenuPolicy` / `RefreshOnlineMenuPolicy`

Applied while connected, cleared on disconnect. Free Play's tab numbering
(`0 Main, 1 Create, 2 Graduation, 3 Options` - there is no Xbox LIVE tab, which
is why Career's numbering left Extras on screen):

    0:1   Main     Challenge Map only
    1:1   Create   Replay Editor only
    3:2   Options  Game Settings and Music Player

Rows are kept from the START of a tab, so the entries that remain sit at their
original indices and selection still lands on the right thing - no index
remapping, nothing to get subtly wrong.

Game Settings is the exception: Difficulty is its row 0, so it is SKIPPED rather
than trimmed, by remapping the row index at the one conversion all five of that
menu's accessors share (filtered to calls from inside them).

Hidden rows are also reported disabled (`IsItemEnabled`, inner `sub_8261E208`),
because the row count only drives drawing - without this the cursor still walks
onto them.

### Difficulty is the server's — `ConfigureServerRules` / `EnforceServerRules`

`server.cfg`'s `game_difficulty` is served at `GET /api/settings` by the
dedicated server and fetched by the client on the register acknowledgement, then
retried each tick until it takes.

It writes the field the game actually reads:

    manager  = sub_824AD240()
    settings = [[manager + 8] + 156]
    index    = [settings + 88]
    name     = [0x83038280 + index * 4]   ("ID_GAMESETTINGS_DIFFICULTY_EASY")

The name table holds localisation keys; `DifficultyName` reports only the last
word, so "easy" matches. Lua can still call `SetGameDifficulty` for a scenario -
what it cannot do is decline the server's rule.

### Challenge sign-up is refused — `sub_825C1FE8`

`CanSignUpToChallenge` answers no while connected. Largely redundant now that
Free Play means no challenges exist, but harmless and correct if one ever does.

## Timing: use `IsPlayerInGame`, not `IsPlayerSpawned`

`IsPlayerSpawned` is backed by the skateboard transform, which exists while the
world is still LOADING - a script waiting on it runs during the load screen.
`IsPlayerInGame` also requires retail's gameplay presence context (`0x8001`), so
it is true only once the front end has handed over.

`IsPlayerSpawned` was left as it was: other resources depend on its current
meaning, and silently redefining it would break them.

## Not online-gated

- `skate3_block_pause_menu` - blunt START blocking, off, superseded by trimming
- `skate3_unlock_everything` - OFF. Governs offline play only, where a career
  is the player's own and keeps its progression. Online forces it regardless;
  see below.
- `skate3_trace_*` - diagnostics

## Online-gated, added with the unlock work

### Everything unlocked

An online session forces retail's progression unlock-everything flag on, from
the first frame, in the engine. No resource is needed and none can opt out:
`SetUnlockEverything(false)` returns `true` online rather than failing quietly,
and `IsEverythingUnlocked()` reports `forced` so a script can tell why.

This is a rule of the session, not a favour. Everyone joining has the same
wardrobe, boards and graphics, so nobody arrives with an advantage earned in a
career nobody else played - the same reason the menu is trimmed and the mode is
forced to Free Play. Offline it is off: `skate3_unlock_everything` decides, it
defaults to false, and a player's own career keeps its progression - unlocking
everything is a multiplayer rule, not a cheat switch left on by accident.

Runtime only in both cases: the flag lives in a heap object retail's own
constructor rebuilds each launch, so it can never reach a save file. See
docs/frontend_bridge.md for how it was found.

### The board-sales milestone strip

The board-sales milestone strip above the menu ("Board Sales Milestone 2/6" and
its progress bar) is career furniture, and online there is no career for it to
report on. It is not a screen - the registry has no entry for it, it is drawn
inside `options/options.swf` - so it is suppressed at its data source instead:
`GetMilestoneDotData` already skips its fill loop when the milestone vector
comes back empty, and the one call that fills it is declined. Gated on
`skate3_multiplayer_relay_active`, so a player's own career keeps its bar.
