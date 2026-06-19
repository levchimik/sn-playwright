# Playwright — custom action buttons

Playwright's per-character **⋯ menu** (the actions you get by clicking the ⋯ on a cast
row, or pressing **Tab** on it) is built from JSON **button fragments** in this folder:

```
Data/SKSE/Plugins/SNPlaywright/buttons/*.json
```

`SNPlaywright.dll` reads **every** `*.json` here at game launch, in **filename order**,
and merges them. Because this is a normal `Data/...` path, any mod can ship its own
fragment (MO2/Vortex just overlays it into the same folder) — so you add buttons without
touching Playwright's files. If the folder is missing/empty, the panel falls back to a
built-in default menu.

> Add a button = ship **one JSON fragment** here + **one Papyrus script** that handles its
> action id. Nothing in Playwright needs to change.

## Fragment format

Each fragment is a **JSON array of button objects**:

```json
[
  { "id": "mymod_pet",    "label": "Pat on the head" },
  { "id": "mymod_scold",  "label": "Scold", "fg": "#e8a0a0", "showWhen": "follower" }
]
```

Load order is by filename, then array order within each file, so prefix numerically
(`10_`, `20_`, `50_yourmod.json`, …) to control where your buttons land.

### Fields

| Field | Required | Meaning |
|---|---|---|
| `id` | **yes** | Action token. Firing the item sends `id\|targetId\|` as the `PW_PrismaCommand` ModEvent payload. Use a unique, namespaced id (e.g. `diary_punish`) so it can't collide with another mod's. |
| `label` | no (falls back to `id`) | The text shown on the menu row. |
| `labelSelf` | no | Alternate label used **only on the player's own row** (the `(you)` entry). Falls back to `label`. |
| `bg` | no | Background color: `#RRGGBB` or `#RRGGBBAA` (the `AA` is alpha/transparency, e.g. `#3a7afe40` = translucent blue). |
| `fg` | no | Text color (`#RRGGBB`). |
| `showWhen` | no (default: always) | Visibility condition — see below. |

`bg`/`fg` let you make red / blue / green buttons; omit both for the default look. Hover
and keyboard-highlight feedback is automatic (a brightness shift) regardless of your colors.

## `showWhen` — when the button appears

A **comma-separated AND-list** of tokens. **All** must hold for the button to show. An
empty/absent `showWhen` means *always show*. Prefix any token with `!` to negate it.

**Tokens:**

| Token | True when the actor… |
|---|---|
| `awake` | is not asleep (neither deep sleep nor sleep-talk) |
| `asleep` | is asleep — **deep sleep OR sleep-talk** |
| `sleeptalk` | is specifically sleep-talking |
| `player` | is the player ( the `(you)` row ) |
| `npc` | is not the player |
| `follower` | is the player's current follower/teammate |
| `pinned` | is in SkyrimNet's "Pinned" group |
| `faction:0xFORMID~Plugin.esp` | is in that faction (see below) |

**Examples:**

- `"awake"` — only while awake (this is what Deep Sleep / Sleep-talk use).
- `"asleep"` — only while asleep (what Wake uses).
- `"follower,awake"` — followers who are awake.
- `"npc,!follower"` — NPCs who are not your follower.
- `"faction:0x1234~DiaryOfMine.esp"` — only for actors in that faction.

### Faction tokens (the important one)

`faction:0xFORMID~Plugin.esp` shows the button only when the target is in that faction —
e.g. a slave faction, a guild, a custom state your mod tracks via a faction.

- `0xFORMID` is the faction's **plugin-local** Form ID (the value xEdit shows, with the
  load-order byte ignored — SPID/KID style). `Plugin.esp` is the defining file.
- **No registration needed.** The DLL scans every installed fragment, collects the faction
  tokens, resolves them, and reports per-actor membership to the panel automatically. Drop
  in your fragment and your faction is tracked — the user maintains no list.
- If the faction can't be resolved (wrong id/plugin not present), the token simply never
  matches, so the button stays hidden (fail-safe). Unknown non-faction tokens behave the
  same way; negating an unknown token (`!whatever`) passes.

> Note: comma separates tokens, so a plugin filename containing a literal comma isn't
> supported in a faction token (spaces are fine).

## Handling your button in Papyrus

Firing a ⋯ button fires the SKSE ModEvent **`PW_PrismaCommand`** with:

```
strArg = "id|targetId|transform|speaker|text"
```

For a ⋯ menu button only the first two fields are set: `id` (your action) and `targetId`
(the row it was fired on — `"player"`, or a `"0x........"` runtime Form ID). The same event
also carries Playwright's own actions, and **multiple scripts can register for it** — each
just matches the ids it owns and ignores the rest.

Minimal handler:

```papyrus
Scriptname MyMod_PlaywrightHook extends ReferenceAlias

Event OnInit()
    RegisterForModEvent("PW_PrismaCommand", "OnPWCommand")
EndEvent
Event OnPlayerLoadGame()
    RegisterForModEvent("PW_PrismaCommand", "OnPWCommand")   ; re-register every load
EndEvent

Event OnPWCommand(String eventName, String strArg, Float numArg, Form sender)
    String action = StringField(strArg, 0)
    If action == "diary_punish"
        Actor target = ResolveTarget(StringField(strArg, 1))
        If target
            ; ... your logic ...
        EndIf
    EndIf
EndFunction

; "player" -> the player; "0x...." -> the runtime form; "" -> None.
Actor Function ResolveTarget(String token)
    If token == "" 
        Return None
    EndIf
    If token == "player"
        Return Game.GetPlayer()
    EndIf
    Return Game.GetFormEx(HexToInt(token)) as Actor
EndFunction
```

(`StringField` = the Nth `|`-delimited field; `HexToInt` = parse a `0x` string. Copy the
helpers from `PW_Controller.psc` if you need them.)

## Full example — a Diary of Mine integration

`50_diary.json`:

```json
[
  { "id": "diary_command", "label": "Give an order",
    "showWhen": "npc,faction:0x000D62~DiaryOfMine.esp" },
  { "id": "diary_punish",  "label": "Punish", "fg": "#e8a0a0",
    "showWhen": "npc,awake,faction:0x000D62~DiaryOfMine.esp" }
]
```

These two buttons appear **only** on awake NPCs in the Diary slave faction; your script
handles `diary_command` / `diary_punish` on `PW_PrismaCommand`. The Playwright user installs
your mod and the buttons appear — they edit nothing.

## Notes

- Fragments are read **once at launch**; restart to pick up changes.
- A malformed fragment makes the panel fall back to its **built-in default** menu (so a typo
  doesn't leave you with no menu) — validate your JSON.
- After any ⋯ action the panel re-pulls the cast list, so state changes (and thus
  `showWhen`) refresh on the next open — even while the game is paused.
