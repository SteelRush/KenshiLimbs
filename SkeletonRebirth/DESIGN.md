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
cycle genuinely concludes — reactivated, or declined with no `delay` still pending toward reactivating
— so a later, distinct repair-kit-use can prompt again. `AITaskSytem::_notifyBodyTaskComplete()` is
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
            { "type": "reactivate_skeleton" },
            { "type": "take_item", "item": "43392-changes_otto.mod" },
            { "type": "show_text", "color": "Green", "string": "/MYNAME/ has been revived!" }
        ]
    }
}
```

Four override types:

| `type` | Params | Purpose |
|---|---|---|
| `reactivate_skeleton` | *(none)* | Calls `TryReactivate()`; on success also stops the treater's repair animation and clears `g_reactivateDialogueShown` (see §2) — placing a `delay` before this in the JSON keeps the treater visibly working for the full delay instead of stopping early. |
| `take_item` | `item` — the item's own FCS String ID | Takes one of that item from whoever clicked the reply. |
| `show_text` | `string` (`/MYNAME/` replaced with the character's name); `color` | Floating colored text tracking the character, same visual language as Kenshi's own stat-increase/pickup notifications. |
| `delay` | `seconds` (fractional allowed) | Pauses this reply's remaining overrides for that long before continuing. |

`show_text`'s `color` accepts a named constant (`Red`/`Green`/`Blue`/`Black`/`White`), `"#RRGGBB"` hex
(`#` optional), or raw `"R,G,B"` (each 0-255). Unrecognized values fall back to `White` and log an
error.

`delay` is special-cased in `dispatchConversationOverridesFrom()` rather than registered as a normal
`OverrideHandler` - handlers perform one action and return, but a delay needs to suspend the whole
remaining sequence and resume it later, which the handler interface has no way to express. When the
dispatch loop hits a `delay`, it records a `PendingOverrideSequence` (patient, initiator, replyId, and
the index to resume at) keyed by patient, and returns without processing the rest of the list.
`Dialogue::update()` (already hooked for the Yes/No buffering commit below) decrements
`remainingSeconds` every frame it fires for that patient - which happens regardless of whether a
conversation is currently active - and resumes dispatch from the saved index once it reaches zero.

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

### 4. Skill-gated dialogue choices

FCS's own dialogue conditions (`DialogConditionEnum`, `Enums.h:796`) have no skill-check type at all
- there's no way to author "only show this reply if Science is 80+" in FCS itself. `RE_Kenshi.json`'s
`"DialogueSkillChecks"` key extends the same native eligibility gate FCS uses for every dialogue line
- `DialogLineData::checkConditions(Dialogue*, Character*, bool)` - to add one, keyed by FCS reply/line
String ID the same way `"ConversationOverrides"` is:

```json
{
    "DialogueSkillChecks": {
        "15-Skeleton Rebirth.mod": [
            { "skill": "science", "min": 80 }
        ]
    }
}
```

`checkConditions()` is the gate FCS itself calls for every dialogue line, including player reply
choices (`DialogLineData::getPlayerReplies()` relies on it the same way NPC line selection does) - so
hooking it here extends that native gate rather than reimplementing reply-list filtering separately.
The hook only ever turns a `true` into `false`: the original result is checked first and returned
as-is whenever it's already `false` or nothing is configured for that line's String ID, so the
overwhelming majority of calls (every dialogue line in the game, not just tagged ones) take a cheap
no-op path - a plain `g_dialogueSkillChecks.empty()` check before even calling `getStringID()`, the
same "cheap gate first" shape as `handleDialogueReplyClicked`'s `g_conversationOverrides.count()`
check in §3.

Skill checks are always evaluated against the player (`PlayerInterface::getAnyPlayerCharacter()`),
regardless of what `dialog`/`target` resolve to for that call - a reply choice is something only the
player ever picks, so it's the player's skill that's meant to gate it. If a player `CharStats` can't be
resolved at all, the line is hidden rather than shown - failing closed on a gated choice rather than
risking exposing content behind a check that couldn't be verified.

`skill` is matched against a fixed lookup table (`g_skillFields`, a `std::map<std::string, float
CharStats::*>`) of lowercase `CharStats` field names - `science`, `lockpicking`, `weaponsmith`, etc. -
not Kenshi's in-game display labels, to keep the mapping unambiguous against the header this was
written from. Only the plain "skill" floats are included (the ones under the Skills tab in-game), not
attributes like strength/dexterity/toughness, which aren't what "skill check" means in the requested
feature. An entry needs at least one of `min`/`max` (Kenshi's native 0-100 scale); an unknown `skill`
name is logged once at JSON-load time and simply skipped when evaluating (a typo in one check
shouldn't hide a reply whose other checks are all fine).

RE_Kenshi.json is only opened/parsed once now (`loadOwnJsonDocument()`), shared by both
`loadConversationOverridesFromJson()` and `loadDialogueSkillChecksFromJson()`, rather than each section
opening the file separately.

### 5. Cleanup sweep

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
- `ForgottenGUI* gui` — global singleton, `Globals.h` (`__declspec(dllimport)`)
- `ForgottenGUI::createScreenLabel(text, colour, size, risingSpeed)` — gui/ForgottenGUI.h:206, RVA `0x73E920`
- `ScreenLabel::setTracking(handle, offset)` — gui/ScreenLabel.h:49
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
- `Inventory::hasItem()` / `takeOneItemOnly()` — Inventory.h, via `Character::_NV_getInventory()`
- `DialogLineData::checkConditions(Dialogue*, Character*, bool)` — Dialogue.h:244, RVA `0x6787B0`, non-virtual — hooked for skill-gated dialogue choices (§4)
- `DialogLineData::getPlayerReplies(lektor<DialogLineData*>&, Dialogue*, Character*)` — Dialogue.h:251, RVA `0x679380` — relies on `checkConditions()` to filter player reply choices, not called directly
- `Character::stats` (`CharStats*`) — Character.h:710 (`0x450` member)
- `CharStats` skill fields (e.g. `science`, `lockpicking`) — CharStats.h:106-135, plain public `float` members
