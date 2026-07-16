# SkeletonRebirth — design

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state directly. Instead they collapse into a
permanent KO/"Rebooting" **Deactivated** state, revivable via a repair kit used in a Skeleton Repair
Bed plus player confirmation. If a Deactivated robot sits in a zone the player hasn't been near for
long enough, it converts to real Dead state so it stops littering the world.

File:line references are to `/home/bryan/Git/RE_Kenshi/KenshiLib/Include/kenshi/`.

## Architecture

### 1. Deactivated state

`Character::declareDead()` is hooked. For robots, the original call is blocked entirely and
`MedicalSystem::dead` is cleared directly — the GUI/party-membership logic reads `dead`/`isDead()`
directly, not whether `declareDead()` ran, so both need to happen. The character is recorded in a
`std::map<Character*, bool> g_deactivated` side table.

`MedicalSystem::medicalUpdate(float)` is hooked and skipped entirely for a Deactivated character —
health is frozen completely in both directions while Deactivated, and healing only needs to resume
after reactivation, not before. This also prevents `declareDead()` from being able to re-fire while
Deactivated.

`Character::update()` is deliberately **not** hooked/skipped — skipping it breaks position syncing
while a Deactivated character is being carried, and causes a rigid, non-ragdolling corpse. Nothing
needs to gate on it: `MedicalSystem::medicalUpdate()` is hooked independently of caller, so it stays
frozen even when invoked from inside `Character::update()`.

`g_deactivated` is currently pointer-keyed and session-only. `Character*` does not survive across
game sessions (the same logical character gets a fresh pointer on reload) — a save-persistent version
of this mechanism needs to key on the handle string (`RootObjectBase::getHandle().toString()`)
instead.

### 2. Reactivation trigger

`MedicalSystem::applyFirstAid()` is hooked. When all three hold — the patient is in `g_deactivated`,
the item used has `itemFunction == ITEM_ROBOTREPAIR`, and the patient is in a bed with
`Building::_NV_getSpecialFunction() == BF_SKELETON_BED` (`25`) — instead of reactivating immediately,
it opens a real FCS dialogue confirmation via `Dialogue::startPlayerConversation(target, root)`
(`Dialogue.h:390`), where `root` is resolved from the hardcoded `SR_REACTIVATE_DIALOGUE_ID` FCS String
ID.

`applyFirstAid()` re-fires many times per second for one continuous repair-kit-use action (the
treater's AI calls it every tick the animation plays). The dialogue is only opened once per "ask and
await an answer" cycle, gated by `g_reactivateDialogueShown` (keyed by patient) — checked directly in
`MedicalSystem_applyFirstAid_hook` before ever calling `showReactivateDialogue()`, not by trying to stop
the underlying repair action at the right moment (that approach is racy: the action can re-fire again
before a stop signal takes effect, reopening the dialogue immediately). The flag is cleared once the
cycle genuinely concludes — reactivated, or declined — so a later, distinct repair-kit-use can prompt
again. `AITaskSytem::_notifyBodyTaskComplete()` is
called on the treater (not the patient) at that same conclusion point, ending the repair animation
cleanly instead of leaving it stuck or wiping the task queue - see `notifyTreaterActionComplete()`.

`TryReactivate()` does the actual state change: nudges each part's `flesh` 1% toward zero (enough to
clear whatever makes `HealthPartStatus::isDead()` stick, not a full heal — the character wakes up
critically hurt and needs treatment) and erases the patient from `g_deactivated`. It's an
unconditional release, not a readiness gate — health can't change at all while Deactivated, so a "was
it repaired?" precondition would deadlock permanently.

### 3. ConversationOverrides — JSON-driven dialogue effects

What happens when a given FCS dialogue reply fires is data-driven via `RE_Kenshi.json`'s
`"ConversationOverrides"` key, not hardcoded per-feature. Only *starting* the conversation
(`SR_REACTIVATE_DIALOGUE_ID`) stays a hardcoded constant, since that trigger is inherent to this
specific feature. Any FCS reply/line String ID — including stock/vanilla dialogue, not just this
mod's own — can be tagged with one or more named overrides:

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

One override type:

| `type` | Params | Purpose |
|---|---|---|
| `reactivate_skeleton` | *(none)* | Calls `TryReactivate()`; on success also stops the treater's repair animation and clears `g_reactivateDialogueShown` (see §2). |

There used to be `take_item`, `show_text`, and `delay` overrides and a `DialogueSkillChecks` JSON key
(skill-gated dialogue reply visibility, hooked via `DialogLineData::checkConditions()`). All were
removed in favor of
[BFrizz_Extra_Extensions](https://github.com/BFrizzleFoShizzle/BFrizz_Extra_Extensions/wiki), a
community-supported FCS extension that already provides native item/notification effects and
stat-level dialogue conditions — no need to maintain a second, narrower implementation of the same
thing in this plugin. Item consumption, notifications, delays, and skill-gating for this mod's own
dialogue content should now be authored directly in FCS using that extension's effects/conditions, not
through `RE_Kenshi.json`. The `ConversationOverrides` dispatch mechanism itself (`g_overrideHandlers`,
keyed by `type`) is kept, so a genuinely new override type specific to this mod's own mechanics can
still be added without disturbing this structure.

`Dialogue::replyClicked` is hooked (both the `int` and `const std::string&` overloads — the native
dialogue window calls both for a single click, but only the `string` overload's parameter is trusted
as the clicked reply's FCS String ID; the `int` overload's `replyIds[index]` resolution is logged for
diagnostics only). Both overloads fire for *every* dialogue reply click in the entire game, not just
this mod's, so `handleDialogueReplyClicked` checks `g_conversationOverrides.count(replyId)` as the
very first thing it does — a plain map lookup, no native calls, no string construction — before doing
anything else. This must stay a true, instant no-op for any reply nobody's configured; doing real work
unconditionally there is what caused a hard-to-diagnose crash on certain stock dialogue previously.

`Dialogue::replyClicked` can report both sides of a mutually-exclusive Yes/No choice for a single
click (a spurious early report of the side the player didn't pick). To avoid acting on the wrong
answer, replies aren't dispatched immediately — they're buffered per-patient
(`g_pendingReplyId`/`g_pendingInitiator`) and only committed once `Dialogue::update()` (also hooked)
detects `conversationHasEnded()` — i.e. trust the last reply reported before the conversation
genuinely ends, not the first one seen.

Parsed with rapidjson (vendored into `KenshiLib_Examples_deps/rapidjson`, added to `build_wine.bat`'s
`INCLUDE`), reusing the same `RE_Kenshi.json` RE_Kenshi's own loader already reads for `"Plugins"`.

### 4. Cleanup sweep

Not a background timer — `Character::updateOnScreenCheck()` (fires roughly every frame per character)
is edge-detected on `isOnScreen` transitioning, giving a lazy "check relevance only when it changes"
signal instead of a periodic scan. A debounce is needed before this is a real feature: a small number
of characters sitting exactly on a frustum/occlusion boundary can flicker `isOnScreen` on/off hundreds
of times within under a second, so acting on the first raw flip risks sweeping a character mid-flicker.
`Character::dailyUpdate()` and `Character::offscreenUpdate()` were both confirmed to never fire and
aren't usable triggers.

At whichever hook fires, for a Deactivated character whose on-screen-elapsed time
(`GameWorld::getTimeFromStamp_inGameHours`, not the plain real-time overload) has passed some
threshold: clear the Deactivated state and call the original `declareDead()` to let real death proceed
before the player next sees the character.

## Reference index

- `Character::declareDead()` — Character.h:411, RVA `0x7A4FA0`, non-virtual
- `Character::updateOnScreenCheck()` — Character.h:220, non-virtual — cleanup sweep trigger, needs debounce
- `Character::isOnScreen` / `isVisibleAndNear` — Character.h:218-219
- `RaceData::robot` — RaceData.h:48
- `MedicalSystem::dead` — MedicalSystem.h:291
- `MedicalSystem::HealthPartStatus::isDead()` — MedicalSystem.h:159
- `MedicalSystem::medicalUpdate(float)` — MedicalSystem.h:115, non-virtual
- `MedicalSystem::applyFirstAid(float, Item*, float, Character*)` — MedicalSystem.h:250, RVA `0x64D080`, non-virtual
- `Character::inSomething` (`UseStuffState`) / `inWhat` (`hand`) — Character.h:595-596
- `UseStuffState::IN_BED` — Character.h:129-134
- `Building::_NV_getSpecialFunction() const` — Building/Building.h:220-221, RVA `0xF6AF0`, virtual
- `BuildingFunction::BF_SKELETON_BED` — Enums.h:150, value `25`
- `GameWorld::getTimeStamp_inGameHours()` / `getTimeFromStamp_inGameHours(double)` — GameWorld.h:238/237
- `Dialogue* Character::dialogue` — Character.h:492 (`0x280` member) — every `Character` owns one
- `Dialogue::startPlayerConversation(Character*, DialogLineData*)` — Dialogue.h:390, RVA `0x683BA0`
- `Dialogue::replyClicked(int)` / `replyClicked(const std::string&)` — Dialogue.h:378-379
- `Dialogue::conversationHasEnded() const` — Dialogue.h:355, RVA `0x6714E0`
- `Dialogue::update(float)` — hooked to detect conversation end for the reply-buffering commit
- `Dialogue::me` — Dialogue.h:395 (`0x150` member) — the `Dialogue`'s owning `Character*`
- `DialogLineData::getStringID() const` — Dialogue.h:248, RVA `0x6A12E0`
- `DialogDataManager::getData(GameData*)` — Dialogue.h:443, RVA `0x6AD120`, static
- `GameDataContainer::getData(const std::string&, itemType)` — GameDataManager.h:28, RVA `0x6BF420` — called as `ou->gamedata.getData(sid, DIALOGUE_LINE)`
- `GameWorld::gamedata` — GameWorld.h:97 (`0x20` member), reached via global `ou`
- `PlayerInterface::getAnyPlayerCharacter() const` — PlayerInterface.h:177, RVA `0x7F19B0`, via `ou->player`
- `AITaskSytem::_notifyBodyTaskComplete()` — AI/AITaskSystem.h:219, RVA `0x50BFB0`

**Platform note (see §5 below): every RVA in this index is a GOG-build address.** They do not
apply directly to a Steam install and were never intended to — they're a snapshot of whichever
build KenshiLib's header-comment RVAs were captured against, not a live-updated value. Confirmed by
decoding `RE_Kenshi/RVAs/GOG_1.0.65.br` byte-for-byte against these numbers (exact match on every
function checked); `Steam_1.0.65.br` disagrees with all of them.

## 5. Version/platform RVA pitfalls (July 2026 investigation)

Investigated whether the Deactivated-state hack (§1) could be replaced with a design that lets
`Character::declareDead()` run for real — so the engine's own GUI/looting/AI/squad-removal/save
logic handles death correctly instead of being hand-duplicated in `g_deactivated` — while
specifically suppressing the corpse-cleanup-on-zone-unload step so a "dead" robot's Deactivated
corpse survives indefinitely instead of being purged like a normal NPC body. Candidate suppression
points identified from the headers: `Platoon::declareDead()`/`undeclareDead()` (a **separate**,
non-virtual `declareDead` on `Platoon` itself, distinct from `Character::declareDead()`),
`ActivePlatoon::destroyCharacters(bool justUnload)`, and `ActivePlatoon::unloadCheck()` — the
`justUnload` bool param name strongly suggests this is the "does the character get destroyed
outright or just the in-memory representation freed" fork.

Testing that redesign needs to know what these functions actually do and where they live — which
turned into a long detour, worth recording so it isn't repeated:

**The RVA numbers in every `KenshiLib` header comment are GOG-build addresses, not Steam.**
`RE_Kenshi/RVAs/` ships one `.br` offset table per platform+version
(`GOG_1.0.65.br`, `Steam_1.0.65.br`) because the two builds are **not** laid out identically —
diffing the two files byte-for-byte shows 13,959 of 29,636 bytes differ (~47%). Decoding either
file's raw binary content (`FULL_BUFF_LENGTH` `int`s, one per function slot, slot index computed
from `_ClassName_base` + the function's position in `Source/kenshi/functions/ClassName.inc`) and
comparing against the header comments confirms every comment matches `GOG_1.0.65.br` exactly and
disagrees with `Steam_1.0.65.br`. This makes sense once you look at how `KenshiLib::GetRealAddress()`
actually resolves an address at runtime (`Functions.cpp`): it's a slot index into a
`function_pointers[]` array populated from `RE_Kenshi/RVAs/<platform>_<version>.br` at startup — the
header comment is never consulted at runtime, it's purely a human-readable snapshot of whatever
build the comment was written against. There's no guarantee it tracks either "the latest KenshiLib
SDK release" or "the currently-shipped `.br` file" — released SDK versions can (and did, as of this
writing) still carry comments from years earlier, because regenerating them isn't part of cutting a
release.

**Even the correct platform doesn't transfer across versions.** The installed game is
`Steam 1.0.68 (Newland)`; the only RVA data available is for `1.0.65`. Decoding
`Steam_1.0.65.br`'s real values (not the GOG-matching header comments) for all seven target
functions and testing them against the 1.0.68 binary in Ghidra: zero hits. Every address landed on
unrelated code (a weather-description function, hash-table internals, an always-`return 1` stub, STL
container internals) — three point-release patches was enough to shift the whole layout. Steam's
depot history doesn't preserve a standalone `1.0.65` branch to diff against either (only `1.0.55`,
which itself reports as `1.0.46 (Dev Mode)` internally and *also* didn't match on any of the seven
functions) — so there's no available historical Steam binary at all to propagate addresses from,
labeled or not.

**What worked: pure static behavioral identification directly against the 1.0.68 binary, no
historical reference needed.** `KenshiLib`'s own `CopyRVAPlugin`/`CopySymbolPlugin` (Cutter/rizin
plugins) confirm there's no separate address-generation tool anywhere in this ecosystem — producing
RVA data has always meant a human in a disassembler identifying functions one at a time, for every
version, no shortcut. Given that, the fastest path was: use the documented struct offsets
(`MedicalSystem::dead` at `0x164`, `Platoon::isDead` at `0x1F0`) to scan the *entire* 1.0.68 binary's
disassembly (not decompiled — a raw instruction scan is fast enough to cover the whole ~36MB exe in
under a minute) for byte-sized writes at those exact displacements. That narrows ~36MB of code to a
handful of candidate functions, cheap enough to decompile and read individually. Confirmed two so
far by content, not just plausibility:

- **`Character::declareDead()` → RVA `0x7A6200`** (Steam 1.0.68) — the candidate at
  `MedicalSystem::medicalUpdate`'s (RVA `0x652000`, also newly reconfirmed for 1.0.68 by the same
  method — matches expected signature/field layout exactly) two `dead=1` call sites decompiles to a
  function containing the literal strings `"{1} is dead."`, `"{1} has died from starvation."`,
  `"{1} has died from blood loss."`, plus squad/faction-removal calls and object cleanup at the end.
  Unambiguous.
- **`Platoon::declareDead()` → RVA `0x797090`** (Steam 1.0.68) — found as a direct callee inside
  `Character::declareDead()` (called specifically when the dying character is the squad leader,
  `plVar3[0x14] == param_1`), and independently confirmed by a debug-log string **literally naming
  itself**: `"Platoon::declareDead: "`, written to `"death.log"`.

**Not yet found for 1.0.68 — parked, real open gap**: `Platoon::undeclareDead()`,
`ActivePlatoon::destroyCharacters()`, `ActivePlatoon::unloadCheck()`,
`ActivePlatoon::_checkForUniqueCharactersOnUnload()`, `Platoon::reCheckPersistenceOnUnload()` — these
are the functions the redesign is actually about (the corpse-cleanup-suppression half), so this is
unfinished, not a side detail. Five techniques were tried and each hit a real wall, worth recording
so they aren't retried blind:

1. **Binary string search** for the function names — turned up one orphaned, unreferenced
   `"@ActivePlatoon::destroyCharacters"` literal with no real code path loading it (a debug string
   the compiler didn't strip in this build but nothing calls — dead end).
2. **Cross-reference from `thunk_FUN_1407ebdd0(param_1,0)`**, called right alongside
   `Platoon::declareDead()` from both declareDead functions — turned out to be a 7-byte
   `Platoon::setPersistentSquad(bool)` (writes `_persistentSquad` at `0x115`), a real find but not
   one of the target functions. `ActivePlatoon::destroyCharacters`/`unloadCheck` are **not** called
   from the death path at all — they're unload-time functions on a separate call path (triggered by
   zone unloading, not character death), so tracing declareDead's callees can't reach them.
3. **Offset-scan for `ActivePlatoon::deactivationTimer`** (float, offset `0xB0`) compare
   instructions — too common a displacement value across the whole binary (436 hits), not
   discriminating enough to sift by hand.
4. **RTTI vtable lookup by class name** — Ghidra's RTTI analyzer recovered 6,120 vtable-related
   symbols but didn't attach class names to any of them (all generic `vftable`/`vftable_meta_ptr`),
   so `ActivePlatoon::periodicUpdate()`'s documented vtable slot (`0x40`) couldn't be looked up by
   name directly.
5. **Manual Complete Object Locator trace** — found the real, non-templated RTTI type descriptor
   string for the bare class (`.?AVActivePlatoon@@` @ `0x141dd7df0`) and wrote a script to check all
   3,055 `vftable_meta_ptr` locations Ghidra had already identified, reading each one's presumed
   Complete Object Locator and comparing its `pTypeDescriptor` field (offset `0xC`, x64 COL layout)
   against that address. Zero matches — either the offset assumption doesn't hold for this build, or
   `ActivePlatoon`'s vtable arrangement doesn't follow the plain single-inheritance COL layout
   assumed (plausible given its multiple base classes).

Next angles worth trying when this is picked back up: chain two offsets instead of one (e.g. find
functions reading `Platoon::isDead` at `0x1F0` *through* `ActivePlatoon::me` at `0x78`, since
"check if my platoon is dead, then unload" is exactly what these functions should do); or repeat the
Complete Object Locator trace with the offset-`0xC` assumption relaxed/verified against a
known-simpler single-inheritance class first (e.g. re-derive it against `Platoon`'s own type
descriptor, where the right vtable is already known from `Platoon::declareDead`, to check the
layout assumption before trusting it against an unverified class).

**Practical note for any future version bump**: since Kenshi is no longer receiving patches (last
`public` branch update 2024-04-01, per Steam depot metadata), whatever gets nailed down for 1.0.68
is a permanent, one-time result — not a treadmill that breaks on the next patch.
