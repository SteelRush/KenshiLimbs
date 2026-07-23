# SkeletonRebirth

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state directly. Instead they collapse into a
**Deactivated** state - reversibly, since the real death transition never runs, and
`MedicalSystem::dead` is kept false throughout (deliberately - see DESIGN.md §1 for why an earlier
version that left `dead=true` for "free" AI/looting/GUI integration was abandoned after it caused a real
crash). A Deactivated robot goes inert on its own, via vanilla's own permanent-at-catastrophic-damage
knockout state - no explicit AI pause is needed. Placing a Deactivated humanoid robot in a Skeleton
Repair Bed, or using a robot-repair item on any Deactivated robot, prompts reactivation via a
confirmation dialogue box, whose text and button behavior are data-driven
from a `"DialogueBoxes"` object nested in [`RE_Kenshi.json`](RE_Kenshi.json) rather than hardcoded C++.
Survives save/reload. Cleanup of unattended Deactivated robots is **not implemented** - see DESIGN.md
§5. See [`DESIGN.md`](DESIGN.md) for the full architecture, including three separate rejected
approaches for both the reactivation trigger and the confirmation box itself, a note on a
virtual-function hooking pitfall worth knowing about before touching this code, and the two abandoned
cleanup designs (one of which caused the crash mentioned above).

## What it does

Hooks in `SkeletonRebirthDiagnostics.cpp`:

| Hook | Purpose |
|---|---|
| `Character::declareDead()` | Blocked entirely for robots — forces `MedicalSystem::dead` back to false (native code sets it true just before calling this, so it has to be reasserted, not just left alone) instead of letting the real transition proceed, and marks the character Deactivated. |
| `MedicalSystem::medicalUpdate(float)` | Skipped entirely while Deactivated — freezes health completely. |
| `Character::updateOnScreenCheck()` | Reactivation trigger only: prompts when a Deactivated humanoid robot is in a Skeleton Repair Bed, or any Deactivated robot is being treated with a robot-repair item. |
| `DatapanelGUI::setLine(key, s1, s2, category, last, keyVisible)` | Overrides the "State:" GUI tag - vanilla has no concept of this in-between state at all (a robot either reads its normal alive status, or "Dead"; nothing shows for a robot frozen at fatal health while still technically alive) - to "AI FAILURE" (humanoid with a head - needs an AI Core) or "POWER FAILURE" (headless, or animal-type - needs a Power Core) in vanilla's own dead-red, unconditionally, for tracked robots. |
| `HandleManager::_NV_restore(std::ifstream&)` | Post-load trigger: re-resolves the persisted Deactivated-robot list after a save loads. |

The confirmation box is a real instance of Kenshi's own `Kenshi_MessageBox.layout`, loaded via
`MyGUI::LayoutManager` and recentered on screen - not a hand-built widget tree, and not the FCS dialogue
system (both were tried under an earlier `dead=true` design; see DESIGN.md for why they didn't work once
a robot was genuinely `dead=true`).

`Character::update()` is deliberately **not** hooked — see DESIGN.md for why.

Debug output goes through KenshiLib's `DebugLog()` (see `Debug.h`) with a `SkeletonRebirth:` prefix.

## Dialogue boxes

A `"DialogueBoxes"` object nested inside `RE_Kenshi.json` itself (not a separate file) defines every
custom dialogue box's title, message, button gating, and button behavior - not hardcoded per-dialogue
C++. `{name}` is replaced with the patient character's name wherever it appears. A button's behavior is
an ordered list of `"steps"`, run in sequence when clicked:

```json
{
    "Plugins" : [ "SkeletonRebirthDiagnostics.dll" ],
    "DialogueBoxes" : {
        "system_menu": {
            "title": "System Menu",
            "message": "{name} is in a catastrophic shutdown loop. What do you want to do?",
            "buttons": [
                {
                    "caption": "Repair",
                    "requiresSkill": "science",
                    "minSkill": 1,
                    "requiresItem": "43392-changes_otto.mod",
                    "steps": [
                        { "type": "take_item", "item": "43392-changes_otto.mod" },
                        { "type": "show_text", "text": "Used AI Core" },
                        { "type": "delay", "seconds": 5 },
                        { "type": "show_text", "text": "{name} has been successfully revived!" },
                        { "type": "action", "action": "reactivate" }
                    ]
                },
                {
                    "caption": "Reset",
                    "excludePlayerFaction": true,
                    "steps": [ { "type": "action", "action": "system_reset" } ]
                },
                { "caption": "Do nothing", "steps": [] }
            ]
        }
    }
}
```

Four step types:
- `"action"` — dispatches a named action from a small `std::map<std::string, DialogueActionFn>`
  registry (populated in `startPlugin()`). `"reactivate"` releases the patient from Deactivated state;
  `"system_reset"` wipes every skill and core attribute to 1 and fast-recruits the patient into the
  player faction. A button with no steps at all (e.g. "Do nothing") just closes the box - there's no
  separate close-only step type, since closing already happens on any click before its steps run.
- `"take_item"` — consumes one of `item` from whoever triggered the dialogue's inventory; stops the rest
  of that button's steps if it fails.
- `"show_text"` — a floating rising-text notification tracking the patient, **not** a GUI panel and not
  tied to the dialogue box (which is already closed by the time steps run). Optional `"color"`:
  `"#RRGGBB"` hex, defaults to white.
- `"delay"` — pauses the *remaining* steps for `seconds`, then resumes them automatically. The box itself
  doesn't wait - it's already closed.

Separately, `requiresItem` and/or `requiresSkill` + `minSkill`/`maxSkill` (a `CharStats` skill name,
0-100 scale) on a button gate whether it's even *shown* - checked once, against whoever triggered the
dialogue, before the box appears. This is deliberately independent of a `take_item` step actually
consuming the same item - one controls visibility, the other controls what happens on click - so a
button that both requires and consumes an item lists that item ID in both places.

`excludePlayerFaction: true` hides a button if the *patient* (not the initiator) belongs to the
player's faction - used on "Reset" so a recruited squad member can't have their skills wiped and be
re-recruited, only a wild/unaffiliated robot can.

**Keep button `caption`s short (~10 characters or less).** `Kenshi_MessageBox.layout`'s buttons have a
fixed width, sized for short captions like the layout's own placeholder "A"/"B"/"C" - text doesn't wrap
or shrink to fit, it just clips on both sides once centered text overflows. Confirmed live: "Do nothing"
(10 characters) rendered fully; "Run Diagnostics" (15 characters) rendered as "un Diagnostic".

See DESIGN.md §4 for the full design, including why this is a different (and much simpler) system than
the mod's older, removed FCS `Dialogue`-hooking approach, even though it now covers the same
item/skill/delay/text-notification features that older system had.

## Debug logging

`RE_Kenshi.json`'s top-level `"Debug"` (bool, default `false`) gates every `DebugLog()` call this mod
makes (dialogue boxes opening, reactivations succeeding, JSON-load summaries, etc.) - off by default so
`RE_Kenshi_log.txt` doesn't grow indefinitely during normal play, since even routine events add up over
a long session. Set it to `true` and restart to get that logging back for troubleshooting. `ErrorLog()`
calls are never gated - real problems are always logged regardless of this setting.

## Persistence

Which robots are Deactivated survives save/reload via a small JSON side-file
(`SkeletonRebirth_Deactivated.json`, an array of handle strings) written next to the active save on
every state change, and re-read on load. No `RE_Kenshi.json` configuration is needed for this - it's
automatic. See DESIGN.md §6 for why a side-file rather than a native save-data hook.

## Build

Same toolchain as `The Limbless (Type 2)`:

- **Native Windows**: VS2019+ with the VS2010 (`v100`) toolset and the
  [KenshiLib_Examples dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  (`KENSHILIB_DIR` env var). Open `SkeletonRebirthDiagnostics.vcxproj`, build Release|x64.
  - or -
- **Linux via Wine**: set `DEPS` to your `KenshiLib_Examples_deps` checkout, then run
  `build_wine.bat` via `wine cmd /c build_wine.bat` (see `The Limbless (Type 2)/README.md` for the
  one-time Wine/SDK setup this assumes).

Requires a `rapidjson` checkout (header-only) at `$DEPS/rapidjson` — clone
[Tencent/rapidjson](https://github.com/Tencent/rapidjson) there if it's not already present;
`build_wine.bat` adds `$DEPS/rapidjson/include` to `INCLUDE` automatically.

## Install

RE_Kenshi loads a plugin DLL by reading a mod's `RE_Kenshi.json`, so it needs an actual (even if
content-empty) Kenshi mod folder to ride along in — that means a binary `.mod` file, which needs
FCS to produce (not something to hand-author blind).

Carrier mod: **`Skeleton Rebirth`** (an empty mod created via FCS's "New Mod" button). Drop the
built `SkeletonRebirthDiagnostics.dll` and this `RE_Kenshi.json` into `mods/Skeleton Rebirth/` next
to its `.mod` file, then enable **Skeleton Rebirth** in Kenshi's Mods tab. `RE_Kenshi.json`'s
`"DialogueBoxes"` object is read at startup (see "Dialogue boxes" above) - if it's missing or empty,
dialogue boxes silently fail to show (logged, not a crash).
