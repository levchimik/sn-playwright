# Playwright — v0.5

A scene-control / NPC-puppeteering toolkit for **SkyrimNet**. Direct who's "in" a
scene, narrate it, and put words (and thoughts, and sleep) into your cast — like a
playwright running a live table read.

> Formerly "SkyrimNet Director Mode" (the `SND_` prefix). Renamed to Playwright
> (`PW_` prefix, plugin `SNPlaywright.esp`) once it grew past the original
> leave-the-scene feature.

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
SeverActions (walk-to-bed + JSON helper).

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
