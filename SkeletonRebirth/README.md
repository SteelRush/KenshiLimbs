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
            { "type": "reactivate_skeleton" }
        ]
    }
}
```

One override type exists:

| `type` | Params | Purpose |
|---|---|---|
| `reactivate_skeleton` | *(none)* | The core revival logic — flesh nudge, un-Deactivate. |

Attaching an existing override type to a new or changed FCS reply only needs a JSON edit and a
restart — no rebuild. Adding a genuinely new override *type* still needs new C++ (a handler function
registered in `startPlugin()`), but from then on it's reusable by any conversation, not just this one.

**Works on any FCS dialogue reply, including stock/vanilla content** — not limited to this mod's own
dialogue tree.

Item-consumption effects, floating notification text, delayed sequencing, and skill-gated dialogue
conditions used to be implemented here too (`take_item`/`show_text`/`delay` overrides,
`DialogueSkillChecks`). All were removed in favor of
[BFrizz_Extra_Extensions](https://github.com/BFrizzleFoShizzle/BFrizz_Extra_Extensions/wiki), a
community-supported FCS extension that already provides native item, notification, and stat-level
dialogue effects/conditions — author those directly in FCS using that extension instead of through
`RE_Kenshi.json`.

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
