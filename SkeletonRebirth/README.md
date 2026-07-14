# SkeletonRebirth

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state. Instead they collapse into a permanent
KO/"Rebooting" **Deactivated** state, and can only be brought back by using a Skeleton Repair Kit
on them while they're in a Skeleton Repair Bed, confirming a prompt, and spending an item (a Power
Core, per the FCS dialogue's own text and item requirement) from whoever used the kit. See
[`DESIGN.md`](DESIGN.md) for the full architecture and implementation reference.

## What it does

Six hooks in `SkeletonRebirthDiagnostics.cpp`:

| Hook | Purpose |
|---|---|
| `Character::declareDead()` | Blocked entirely for robots — clears `MedicalSystem::dead` instead of letting real death proceed, and marks the character Deactivated. |
| `MedicalSystem::medicalUpdate(float)` | Skipped entirely while Deactivated — freezes health completely (no healing, no further deterioration, and stops `declareDead()` from re-firing). |
| `MedicalSystem::applyFirstAid(...)` | The reactivation trigger. When a `ITEM_ROBOTREPAIR` item is used on a Deactivated character in a bed with `Building::_NV_getSpecialFunction() == BF_SKELETON_BED`, starts a real FCS dialogue instead of reactivating immediately. |
| `Dialogue::replyClicked(int)` / `Dialogue::replyClicked(const std::string&)` | Reads back which reply the player picked (both overloads are hooked - the native dialogue window calls both for a single click; only the `string` overload's value is trusted), then dispatches whatever **conversation overrides** are configured for that reply in `RE_Kenshi.json` — see below. |
| `Dialogue::update(float)` | Detects when a conversation has genuinely ended, to commit the last reply reported (see DESIGN.md — `Dialogue::replyClicked` can report both sides of a Yes/No choice for one click). |

The confirmation prompt is a real interactive conversation opened via `Dialogue::startPlayerConversation()` — the actual native "start a player conversation with choices" entry point.

`Character::update()` is deliberately **not** hooked — see DESIGN.md for why.

Debug output goes through KenshiLib's `DebugLog()` (see `Debug.h`) with a `SkeletonRebirth:` prefix.

## Configuring reactivation via `RE_Kenshi.json`

Only *which* FCS dialogue to start (on the repair-kit-in-bed trigger) is hardcoded
(`SR_REACTIVATE_DIALOGUE_ID`). What happens when a given reply fires is data-driven — any FCS
reply/line String ID can be tagged in `RE_Kenshi.json` with one or more named **conversation
overrides**, dispatched generically whenever that reply is clicked:

```json
{
    "Plugins": ["SkeletonRebirthDiagnostics.dll"],
    "ConversationOverrides": {
        "12-Skeleton Rebirth.mod": [
            { "type": "reactivate_skeleton" },
            { "type": "take_item", "item": "43392-changes_otto.mod" },
            { "type": "show_text", "color": "Green", "string": "/MYNAME/ has been revived!" }
        ]
    }
}
```

Three override types exist:

| `type` | Params | Purpose |
|---|---|---|
| `reactivate_skeleton` | *(none)* | The core revival logic — flesh nudge, un-Deactivate. |
| `take_item` | `item` — the item's own FCS String ID | Takes one of that item from whoever clicked the reply. |
| `show_text` | `string` — the text (`/MYNAME/` is replaced with the character's name); `color` — see below | Floating colored text tracking the character, same visual language as Kenshi's own stat-increase/pickup notifications. |

`show_text`'s `color` accepts a named constant (case-sensitive: `Red`, `Green`, `Blue`, `Black`,
`White` — the full set `MyGUI::Colour` exposes), `"#RRGGBB"` hex (the `#` is optional, e.g.
`"#FF8C00"` or `"FF8C00"` for orange), or raw RGB as `"R,G,B"` with each component 0-255 (e.g.
`"255,140,0"`) — `resolveNamedColor()`/`tryParseHexColor()`/`tryParseRgb()` in the .cpp. Omitted or
unrecognized values fall back to `White` and log an error, rather than failing silently.

Attaching an existing override type to a new or changed FCS reply only needs a JSON edit and a
restart — no rebuild. Adding a genuinely new override *type* still needs new C++ (a handler function
registered in `startPlugin()`), but from then on it's reusable by any conversation, not just this one.

**Works on any FCS dialogue reply, including stock/vanilla content** — not limited to this mod's own
dialogue tree. Confirmed via live testing: attaching a `show_text` override directly to a stock NPC's
own reply fires correctly with zero side effects on other dialogue.

Parsed with **rapidjson** (vendored into `KenshiLib_Examples_deps/rapidjson` — see Build below),
reusing the same file RE_Kenshi's own loader already reads for `"Plugins"`.

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
to its `.mod` file, then enable **Skeleton Rebirth** in Kenshi's Mods tab.
