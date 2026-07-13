# SkeletonRebirth

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state. Instead they collapse into a permanent
KO/"Rebooting" **Deactivated** state, and can only be brought back by using a Skeleton Repair Kit
on them while they're in a Skeleton Repair Bed and confirming a prompt. See [`DESIGN.md`](DESIGN.md)
for the full design history, rejected approaches, and confirmed test results.

## What it does

Three hooks in `SkeletonRebirthDiagnostics.cpp`:

| Hook | Purpose |
|---|---|
| `Character::declareDead()` | Blocked entirely for robots — clears `MedicalSystem::dead` instead of letting real death proceed, and marks the character Deactivated. |
| `MedicalSystem::medicalUpdate(float)` | Skipped entirely while Deactivated — freezes health completely (no healing, no further deterioration, and stops `declareDead()` from re-firing). |
| `MedicalSystem::applyFirstAid(...)` | The reactivation trigger. When a `ITEM_ROBOTREPAIR` item is used on a Deactivated character in a bed with `Building::_NV_getSpecialFunction() == BF_SKELETON_BED`, shows a confirmation panel ("Attempt to reactivate `<name>`?") instead of reactivating immediately. Clicking "Yes" calls `TryReactivate()`, which applies a small (1%) nudge to head/torso `flesh` toward zero — just enough to stop an instant re-death, not a full heal — and shows a one-button "was revived!" notification. |

The confirmation panel is built directly from MyGUI's own widget/event API (`ForgottenGUI::createPanel`/`createButton`/`createLabel`, real `eventMouseButtonClick` delegates) rather than any of Kenshi's own dialog mechanisms — both `ForgottenGUI::messageBox()` (undocumented button-flags bitmask, several combinations crashed or produced stuck unclickable dialogs) and `Dialogue::runCustomDialog()`/`sendEvent()` (narrated instead of pausing for choice, or was silently rejected) were tried and abandoned first. See DESIGN.md Status section for the full story.

`Character::update()` is deliberately **not** hooked — see DESIGN.md Status section for why.

Debug output goes through KenshiLib's `DebugLog()` (see `Debug.h`) with a `SkeletonRebirth:` prefix.

## Build

Same toolchain as `The Limbless (Type 2)`:

- **Native Windows**: VS2019+ with the VS2010 (`v100`) toolset and the
  [KenshiLib_Examples dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  (`KENSHILIB_DIR` env var). Open `SkeletonRebirthDiagnostics.vcxproj`, build Release|x64.
  - or -
- **Linux via Wine**: set `DEPS` to your `KenshiLib_Examples_deps` checkout, then run
  `build_wine.bat` via `wine cmd /c build_wine.bat` (see `The Limbless (Type 2)/README.md` for the
  one-time Wine/SDK setup this assumes).

## Install

RE_Kenshi loads a plugin DLL by reading a mod's `RE_Kenshi.json`, so it needs an actual (even if
content-empty) Kenshi mod folder to ride along in — that means a binary `.mod` file, which needs
FCS to produce (not something to hand-author blind).

Carrier mod: **`Skeleton Rebirth`** (an empty mod created via FCS's "New Mod" button). Drop the
built `SkeletonRebirthDiagnostics.dll` and this `RE_Kenshi.json` into `mods/Skeleton Rebirth/` next
to its `.mod` file, then enable **Skeleton Rebirth** in Kenshi's Mods tab.
