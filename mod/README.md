# Playwright — v0.7

A scene-control / NPC-puppeteering toolkit for **SkyrimNet**. Direct who's "in" a
scene, narrate it, and put words (and thoughts, and sleep) into your cast — like a
playwright running a live table read.

> Formerly "SkyrimNet Director Mode" (the `SND_` prefix). Renamed to Playwright
> (`PW_` prefix, plugin `SNPlaywright.esp`) once it grew past the original
> leave-the-scene feature.

**v0.6** adds a **PrismaUI control panel** (`SNPlaywright.dll`): an on-screen panel
with a live nearby-NPC list and the full action set as buttons + a roomy text box —
no crosshair aiming, no cramped UIExtensions input, multi-word names just work. The
wheel + MCM hotkeys still work exactly as before; the panel is an alternative
surface, not a replacement.

## Actions

| Action | What it does |
|---|---|
| **Director Mode** | Toggle yourself out of the scene (SkyrimNet `ActorBlacklistFaction`). NPCs talk among themselves; you're unseen/unheard and narrate from outside. |
| **Narrate** | Type a scene event; a nearby NPC voices it as a general remark to everyone present. Works in or out of Director Mode. |
| **Prompt** | No-text nudge — prompts the crosshair NPC (or a nearby one) to speak/continue the scene. Player stays in the audience but isn't force-addressed. |
| **Say** | The crosshair NPC's line, **verbatim** (text/subtitle + memory). *Not voiced* — SkyrimNet can't TTS an arbitrary literal line. |
| **Transform** | The crosshair NPC speaks an **LLM-phrased** line from your gist, in their voice (voiced). The spoken sibling of Say. |
| **Think** | Inject a private, unvoiced **thought** into the crosshair NPC (LLM-mediated; colors their behavior). |
| **Deep Sleep** | Crosshair NPC (or self) goes deeply unconscious — deaf, unselectable, others see them out cold. Optional walk-to-bed. |
| **Sleep-talk** | Like Deep Sleep but they may murmur dream fragments aloud (ambient channel; never pulls others into conversation). Optional walk-to-bed. |

Deep Sleep / Sleep-talk also work **on the player** (Self), and a woken actor
permanently "forgets" what was said while they were out (a per-actor deaf-window
stored in the co-save, exposed to prompts via the `pw_deaf_start`/`pw_deaf_end`
decorators).

## The PrismaUI panel (v0.6)

Press the panel key (**F11** by default; rebind in
`SKSE/Plugins/SNPlaywright.ini` → `[Controls] ToggleKey`, a DXScanCode) to open a
left-side panel. It lists the nearby cast (distance-sorted, with **asleep** /
**murmuring** badges) plus a **(you)** row at the top. Click a name to target it,
type into the text box, and hit an action: **Say / Transform / Think / Prompt /
Narrate / Deep Sleep / Sleep-talk / Wake**, plus a **Director** toggle in the
header. Buttons grey out until their needs are met (a target and/or text). Escape or
the ✕ closes it.

How it works: `SNPlaywright.dll` (a thin CommonLibSSE-NG SKSE plugin) owns the
PrismaUI view, enumerates the cast itself, and forwards button clicks to
`PW_Controller` as the `PW_PrismaCommand` SKSE ModEvent — every action runs through
the *same* Papyrus cores the wheel/hotkeys use, so behaviour is identical. **Needs
the Prisma UI framework**; without it the DLL no-ops and the wheel/hotkeys still
work.

## Conversation log (v0.7)

The panel header has a **Log** toggle that opens a side column showing SkyrimNet's
live conversation/event history, with inline **Edit** and **Delete** per entry —
the same editable memory as SkyrimNet's F12 chat, but as a side-panel.

It works by talking to SkyrimNet's **local HTTP API**: `SNPlaywright.dll` reads the
port from SkyrimNet's `config/WebServer.yaml` (default `127.0.0.1:8080`, honours a
custom port) and does the requests itself via WinHTTP — `GET /events?api=list` to
list, `PUT /events?api=update` to edit (the API validates the whole event, so the
DLL sends `{id, type, data}` with `data` as a plain string), and
`DELETE /events?api=delete&id=` to delete.
Doing it in the DLL (not the view's `fetch`) sidesteps browser CORS entirely.

Requires SkyrimNet's web server enabled (`WebServer.yaml` → `enabled: true`, which
is the default). If it's off/unreachable the log shows a notice and the rest of the
panel still works.

## Keyboard controls (v0.7)

The panel is fully keyboard-drivable — a director's console. While the panel is open
and you're *not* typing, these keys are captured by `SNPlaywright.dll` and routed to
the view (so they never leak to other mods' hotkeys, e.g. Modex):

| Key | Action |
|---|---|
| **↑ / ↓** | Walk the active column (cast list or conversation log). |
| **→** | Jump to the newest log entry (clears the cast cursor). |
| **←** | Select the player (PC) as target (clears log focus). |
| **Enter** *(on a cast member)* | 1st press = **Speaker** (badge), 2nd = **Target** (badge); re-press clears. **Say** honours this Speaker→Target pairing; other actions ignore it. |
| **Enter** *(on a log entry)* | Open it for inline edit with all text selected — edits keep their original attribution. |
| **Enter** *(otherwise)* | Sends the box if an action is armed; else arms **Say**. On a focused action button, presses it. |
| **Delete** | Deletes the focused log entry (no confirm) and moves up — press repeatedly to chain-delete. In the cast column it clears the Speaker/Target pairing. |
| **Tab** | Open the action menu (then walkable with ↑/↓). |
| **1 – 4** | Arm **Say / Think / Transform / System** (no text focus, the digit isn't printed). |
| **0** | Toggle **Transform** (LLM-phrased/voiced) vs verbatim mode. |
| **+** *(numpad)* | Pause / unpause the game. |
| **? / `/`** | Open the controls (help) window. |
| **any letter** | Start typing into the message box. |

The Speaker/Target pairing and the panel's + controls-window position/size persist
across sessions (localStorage). The panel and controls window are both draggable, and
the panel is resizable. The conversation log keeps polling for new entries even while
the game is paused.

## The wheel (UIWheelMenu radial, 8 slots)

```
              Director (top)
   Prompt                      Narrate
   Say                         Transform
   Think                       Sleep-talk
            Deep Sleep (bottom)
```
Left column = Director / Prompt / Say / Think; right column = Narrate / Transform /
Sleep-talk / Deep Sleep. Crosshair-dependent slots (Say/Think/Transform/Sleep-talk/
Deep Sleep) grey out unless you're looking at an NPC and show their name + current
state.

## Controls

All keys are **unbound by default** — set them in the MCM (SkyUI → **Playwright** →
Controls). Every Playwright key is gated behind a **modifier** (default Left Shift,
SeverActions-style): nothing fires unless the modifier is held while *Require
modifier* is on. The MCM also exposes **Send to bed (while sleeping)** and
**Sleep-talk murmuring** (with interval/chance sliders).

## Contents

- **Plugin** `SNPlaywright.esp` (ESL-flagged): quest `PW_DirectorQuest` (player alias →
  `PW_Controller`), quest `PW_MCMConfig` (→ `PW_MCM`), factions `PW_AsleepFaction`
  (0x801) and `PW_SleeptalkFaction` (0x803).
- **Scripts** `PW_Controller` (hotkeys + wheel + actions), `PW_MCM` (SkyUI MCM).
- **Loose prompt overrides** under `SKSE/Plugins/SkyrimNet/prompts/` — stock SkyrimNet
  prompts with gated branches added (byte-identical to stock when the player is not in
  any Playwright faction). They reference `PW_AsleepFaction`/`PW_SleeptalkFaction` by
  EditorID and the `pw_deaf_*` decorators.

## Dependencies

SkyrimNet, SKSE, UIExtensions (wheel + text entry), PapyrusUtil (StorageUtil), and
SeverActions (walk-to-bed + JSON helper). The v0.6 panel additionally needs the
**Prisma UI** framework — optional; skip it and the wheel/hotkeys cover everything.

## Install

1. In **MO2**: enable **Playwright**, sort it **below/after SkyrimNet** so its prompt
   overrides win. Enable **`SNPlaywright.esp`** in the Plugins pane.
2. **Fully restart the game** — SkyrimNet caches prompt templates at load, so the
   overrides only apply on a fresh launch.
3. Bind keys in the MCM (SkyUI → Playwright → Controls).

## Notes / limitations

- Director Mode needs **2+ SkyrimNet NPCs** near each other to produce NPC-to-NPC
  dialogue.
- **Say** is text-only (verbatim, not voiced) by design — use **Transform** for a
  voiced line.
- The prompt overrides are copies of SkyrimNet's prompts; if SkyrimNet updates those
  files, re-sync the gated branches.
- The `~55s` SkyrimNet decorator cache means faction-driven states (asleep, Director
  absence) can lag up to a minute after toggling.
