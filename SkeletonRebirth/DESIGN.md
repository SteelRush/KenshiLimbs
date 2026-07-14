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
