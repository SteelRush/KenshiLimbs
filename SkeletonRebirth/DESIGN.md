# SkeletonRebirth — design plan

Goal: Robot-race characters (all races with `RaceData::robot == true` — Skeletons, Iron Spiders,
Soldierbot Guards, Security Spiders, etc., not just Skeletons specifically) never enter vanilla
Dead state directly. Instead they enter a custom **Deactivated** state (permanently incapacitated,
revivable via a high-effort/high-tier repair action). If a Deactivated robot sits in a zone the
player isn't near long enough, it converts to real Dead state so it stops littering the world.

This is a plan only — nothing below is built yet. File:line references are to
`/home/bryan/Git/RE_Kenshi/KenshiLib/Include/kenshi/`.

## Status (read this first)

**CONFIRMED WORKING, CLEAN, MINIMAL — core Deactivated mechanism (§1, §2):** built and live-tested
in `SkeletonRebirthDiagnostics.cpp`, not just planned. Down to **two** hooks:
1. `Character::declareDead()` — blocked for robots, plus `MedicalSystem::dead = false`. Confirmed
   necessary: blocking `declareDead()` alone left the GUI/party-membership logic still treating the
   character as dead (they read `dead`/`isDead()` directly, not whether `declareDead()` ran). Does
   **not** touch flesh/part health at all - confirmed this hook alone safely absorbs a
   `declareDead()` call no matter how often it fires or what health currently is (tested up to
   ~3,000 repeated calls in one session, no crash, no real death, no visible glitch).
2. `MedicalSystem::medicalUpdate(float frameTime)` — skipped **entirely** for a Deactivated
   character's medical system. **DECISION: health is frozen completely while Deactivated - no
   change in either direction, healing included.** Reasoning: healing only needs to matter *after*
   reactivation, not before, so there's no downside to freezing it completely, and doing so
   eliminates a confirmed ~75-137 calls/sec CPU cost from `declareDead()` re-firing continuously
   even at full health (root cause never identified, but irrelevant now - freezing this function
   entirely stops it regardless of cause). `MedicalSystem` has no direct owning-`Character*`
   field; reached the same way as elsewhere, via `getPart(...)->me`.
- **`Character::update()` is deliberately NOT hooked** (was skipped entirely in an earlier version).
  Confirmed that fixed the `setProneState` stand-up problem (blocked-attempt count went to 0), but
  also broke position syncing while being carried - visually follows the carrier, but the logical
  position never updates, so dropping a carried Deactivated character registered it at its
  pre-pickup position. Un-skipped to fix that; relies on `MedicalSystem::medicalUpdate()` being
  hooked independently of caller (hooking redirects the real function address, so it's still
  intercepted even when called from inside the now-unblocked `Character::update()`) to keep
  preventing the stand-up and re-death problems. **CONFIRMED working**: after un-hooking
  `Character::update()`, `declareDead()` still only fired once per real death (not the
  ~75-137/sec cascade), `proneState` stayed at `PS_KO`, and - unexpectedly - **both the position
  bug and the earlier "rigid, non-ragdolling corpse" issue were fixed by this same change.** See
  below - the ragdoll issue was walked back from "savegame artifact" to "actually caused by
  skipping `Character::update()`."
- **Removed the `setProneState()` lock entirely** (was a hook in earlier versions). Once
  `Character::update()` was (previously) confirmed to drive the stand-up check, this hook's block
  count dropped to 0 - it was doing nothing.
- Also removed as dead weight, confirmed to contribute nothing: `Character::dailyUpdate()` hook
  (confirmed the function never fires at all), `Character::offscreenUpdate()` hook (also confirmed
  never fires), `Character::updateOnScreenCheck()` hook (was only diagnostic/test-scaffold
  support), `Character::loadFromSerialisePostCreationStage()` hook (was diagnostic-only, no core
  logic depends on it currently - will likely be needed again once the handle-keyed persistent
  store, see below, is built).
- **CORRECTED — rigid/non-ragdolling corpse WAS caused by skipping `Character::update()`, not a
  savegame artifact as first concluded.** Original chase: tried `Character::ragdollMode(true,
  RagdollPart::WHOLE)`, `Character::ragdollUpdatesUT()` inside the `Character::update()` skip,
  removing the `setProneState` lock, and `MedicalSystem::knockout()` both before and after clearing
  `dead` - none of it made a difference, and testing on a fresh save/death appeared to show the
  character ragdolling normally, which was (incorrectly) taken as proof the issue was save-specific.
  It resurfaced later as a related but distinct symptom (carried character's position never
  updating), which led back to `Character::update()` being skipped as the actual common cause of
  both. Once `Character::update()` was un-hooked (see above), **both the position bug and the
  ragdoll/rigidity issue were confirmed fixed together** - no ragdoll-specific code needed at all,
  it was purely a consequence of skipping the wrong function.
- **SUPERSEDED — healing-while-Deactivated was proven possible, then deliberately dropped.**
  Repair kits were a dead end (three rejected approaches, all removed from the codebase: (a) freeze
  flesh at a fixed value - blocks all healing entirely; (b) one-time nudge of the real flesh value
  just past the death threshold in `declareDead()` - worked mechanically but caused a visible,
  unwanted jump in the health number before any repair happened; (c) hook `HealthPartStatus::isDead()`
  to report `false` without touching `flesh` - no visible jump, but repair kits still refused to do
  anything). But the **Skeleton Repair Bed** was live-tested and confirmed to heal a Deactivated
  character correctly and smoothly (~5 flesh/second, -190 to +200 over ~80 seconds) with zero extra
  hooks - proving it *could* work. Decided not to keep it: healing doesn't need to happen while
  Deactivated at all, only after reactivation, so `MedicalSystem::medicalUpdate()` is now frozen
  completely (see hook 3 above), which also stops the bed from healing a still-Deactivated
  character. This was a deliberate tradeoff for the CPU-cost fix, not a discovered limitation.
- **Reactivation design: unconditional release, not a readiness gate.** `TryReactivate()` used to
  require every part to have already cleared `HealthPartStatus::isDead()` before releasing the
  lock - now that health is frozen completely while Deactivated (see hook 3 above), that check
  would never pass and the gate could never open. Simplified to an unconditional release: clears
  `g_deactivated` and lets `Character::update()`/`MedicalSystem::medicalUpdate()` resume normally,
  with no health check at all. Healing/repair (bed or otherwise) is expected to happen normally
  *after* reactivation, not as a precondition for it.
- Displayed status is vanilla's own **"Rebooting"** label (the existing robot-specific KO status
  text) - confirmed this is what robots normally show while KO'd, not something we need to build.
  The earlier "Dying" flicker was a symptom of the `setProneState` fight (Hook 2 vs vanilla trying
  to stand the character up every tick), not a separate problem needing its own fix - once Hook 3
  stopped that fight, it went away on its own.
- **CONFIRMED — clean revival cycle works end-to-end.** Added a test-only trigger
  (`testOnlyRevive()` in `SkeletonRebirthDiagnostics.cpp`, called from Hook 4 after a short delay -
  not the real revival mechanic, just a way to test the reversal) that fully heals both parts and
  erases the character from `g_deactivated`. Verified directly against the logged `proneState`
  over time: held steady at `PS_KO` (4) for the entire Deactivated period, switched to `PS_NORMAL`
  (0) the instant revival fired, and stayed there with no flicker or reversion afterward. Confirms
  §1/§2's core mechanism and the "un-freeze" direction both work correctly - clearing
  `g_deactivated` and restoring health is sufficient for the character to resume completely normal
  behavior, no additional cleanup needed anywhere else.
- Race filter confirmed working: `RaceData::robot` correctly tags robot characters at the moment
  of death.
- Side-table must key on the **handle string** (`RootObjectBase::getHandle().toString()`), never
  the raw `Character*` — confirmed pointers don't survive across sessions, same handle comes back
  with a different pointer. **Not yet done**: the diagnostic's `g_deactivated`/`g_lastOnScreen` maps
  are pointer-keyed and session-only (fine for testing hook behavior live, not fine for the real
  feature) - switching to a handle-keyed, save-aware store is the remaining work before this is a
  real feature rather than a validated prototype.
- **CONFIRMED — real reactivation trigger wired and live: Skeleton Repair Bed + repair kit item.**
  `MedicalSystem::applyFirstAid()` is hooked; it reactivates a Deactivated character when all three
  hold: the character is in `g_deactivated`, the item used has `itemFunction == ITEM_ROBOTREPAIR`,
  and the character is in a bed whose `Building::_NV_getSpecialFunction() == BF_SKELETON_BED`.
  Diagnostic logging (since removed) confirmed: `applyDoctoring()` always co-fires with
  `applyFirstAid()` for the same treatment (identical timestamp, same item/state every time), so
  only `applyFirstAid()` needs hooking. It also confirmed `itemFunction == ITEM_ROBOTREPAIR` is the
  *only* value ever seen for robot patients (even routine background self-repair by non-Deactivated
  robots uses it) - not a very selective filter alone, but combined with the bed-type and
  `g_deactivated` checks it's sufficient. **`BF_SKELETON_BED` is confirmed `25`** via live
  `bedSpecialFunction`/`isSkeletonBed` logging - an earlier note in this doc had guessed `150`
  from FCS inspection; that guess was wrong, this value is the actual compiled constant.

**RESOLVED — cleanup (§3, secondary feature):** `Character::updateOnScreenCheck()` is a confirmed
viable trigger. Over one ~260s test session it logged exactly one clean on/off transition each for
8 different characters. `Character::offscreenUpdate()` never fired even once - dropped as a
candidate. One caveat: a couple of characters (sitting exactly on a frustum/occlusion boundary)
flickered on/off dozens to hundreds of times within under a second - the real hook needs a simple
debounce (act only once a state has held for N consecutive calls) rather than reacting to the
first flip. See §3 for the updated plan. Nothing left blocking either the core redirect or the
cleanup sweep now.

**Deferred / not needed to move forward:**
- Exact meaning of `HealthPartStatus::fatal`, the in-game `<`/`<<`/`<<!!` wound-severity chevrons,
  and the precise mechanism of the second ("left KO'd too long") death path. None of these change
  what gets built - `declareDead()` fires either way. Only worth resurrecting if the revival
  mechanic (§4, still undesigned) turns out to need per-part injury severity specifically.

**DECIDED:** `robot` (broad) is the intended filter, not a narrower skeleton-specific race ID.
Product decision: the revival mechanic is planned to expand to all robots (Iron Spiders,
Soldierbot Guards, Security Spiders, etc.), not just Skeletons, so catching every `robot=true`
race is correct as-is - no filter change needed.

## Why this shape

Vanilla `Character::isDead()` is read from AI, GUI, loot, save, and faction code we can't see
decompiled — patching all of it to recognize a truly new engine state isn't practical. So
"Deactivated" is not a new engine-level state; it's vanilla's existing permanent-KO/collapse
machinery, intercepted and relabeled, plus a small side-table of our own bookkeeping. Dead state
itself is untouched and still fires normally once we let it through (the 24h fallback, or the
"someone finishes them off"/never-revived case).

## Pieces

### 1. Intercept the death trigger

- Hook `Character::declareDead()` (`Character.h:411`, RVA `0x7A4FA0`, non-virtual — hookable
  directly via `KenshiLib::GetRealAddress(&Character::declareDead)`, same pattern as
  `RobotLimbRaceLock.cpp`).
- In the hook: check `self->getRace()->robot` (`Character.h:562` → `RaceData.h:48`) — refine to a
  specific race string ID if "robot" is too broad and other robot races shouldn't get this.
  - If skeleton and not already flagged Deactivated: **don't call the original.** Instead:
    - Set the character to a permanent collapsed/incapacitated state using the existing
      `ProneState` enum (`Character.h:53-60`, `PS_KO` or `PS_CRIPPLED`) and
      `MedicalSystem::CollapseStage` (`MedicalSystem.h:231-238`), whichever combination actually
      blocks self-recovery (needs in-game verification — vanilla KO normally auto-recovers, so we
      likely need to also suppress whatever periodic check would clear it).
    - Record our own side-table entry: `Character* -> deactivatedAtTime (TimeOfDay)`, stamped via
      `ou->getTimeStamp_inGameHours()` (`GameWorld.h:238`).
  - If not skeleton, or skeleton already past the 24h fallback threshold: call the original
    `declareDead()` unmodified.

### 2. Side-table for Deactivated bookkeeping

Same idiom as `RobotLimbRaceLock.cpp`'s `g_limbInterfaceOwners` map — a
`std::map<Character*, DeactivatedRecord>` (or `unordered_map`) living in the plugin, not in game
memory, keyed by character pointer. Holds at minimum:
- `TimeOfDay deactivatedAt`
- whatever "cost to revive" bookkeeping the revival-item mechanic ends up needing (separate design
  question, not covered here)

Risk to flag: character pointers can be invalidated if the engine ever frees/recreates a
`Character*` (e.g. across save/load). Needs verification — if save/load recreates characters, the
map needs to key on something stable (a persistent character ID) instead of raw pointer, or be
rebuilt from a save-persisted flag instead of an in-memory map. This is the single biggest
open risk in the whole plan and should be checked first before writing the revival mechanic.

### 3. Cleanup — not a literal 24h background sweep, a lazy on-relevance check

Original plan was a background timer (`dailyUpdate()`) checking every Deactivated character once
per in-game day. Dropped: `dailyUpdate()` never fires in testing (see confirmed-findings), and
a literal timer isn't actually required — nobody is watching an abandoned Deactivated skeleton in
a zone the player has left, so it doesn't matter *when* the conversion-to-real-Dead happens, only
that it happens **before the player next encounters that character again**.

Revised approach: check "has this Deactivated character been abandoned long enough to convert to
real Dead" lazily, inside a hook that fires when the character becomes relevant again, instead of
on a continuous background schedule.

**Confirmed hook: `Character::updateOnScreenCheck()`** (`Character.h:220`, non-virtual, RVA
present). Fires very frequently (~every frame per character - 267k calls in one test session), but
edge-detecting on `isOnScreen` actually changing gives a clean one-shot transition signal for most
characters. Caveat: a couple of characters flickered on/off dozens-to-hundreds of times within a
fraction of a second (likely sitting exactly on a frustum/occlusion boundary) - **the real hook
must debounce** (e.g. only act once `isOnScreen` has held a value for N consecutive calls, or for
some minimum real-time interval) rather than triggering on the first raw flip, or a flickering
character could get swept prematurely mid-flicker.

`Character::offscreenUpdate()` (`Character.h:221`) was the other candidate - **confirmed dead end,
never fired once** across multiple test sessions. Dropped.

`Character::loadFromSerialisePostCreationStage()` remains a good fallback/additional trigger point
(already confirmed reliable, fires on any character construction from data including ambient
respawns) for the case where a Deactivated character gets reconstructed from a save before ever
passing through `updateOnScreenCheck()` again.

Logic at whichever hook fires, for Deactivated skeletons only:
- Compare elapsed time via `ou->getTimeFromStamp_inGameHours(record.deactivatedAt.getTotalHours())`
  (`GameWorld.h:237`) — **use the `_inGameHours` variant specifically**, not the plain real-time
  `getTimeFromStamp` overload, which exists alongside it and is easy to grab by mistake.
- If past whatever threshold is chosen (no longer required to be literally 24h): clear the
  Deactivated flag/side-table entry, then call the *original* `declareDead()` (the saved function
  pointer from the hook in step 1) to let real death proceed normally, before the player actually
  sees/interacts with the character again.
- No scheduler or timer thread needs to be built - this piggybacks entirely on hooks that already
  fire for other reasons.

### 4. Revival — DECIDED and wired: Skeleton Repair Bed + repair kit item

Item-based, not dialogue-based (dialogue was only ever used historically because it was the only
option available - an item trigger is cleaner). `TryReactivate()` is an unconditional release (see
Status) called from a hook on `MedicalSystem::applyFirstAid()`, gated on: character is in
`g_deactivated`, item used has `itemFunction == ITEM_ROBOTREPAIR`, and character is in a bed with
`Building::_NV_getSpecialFunction() == BF_SKELETON_BED` (confirmed `25`). No readiness/health
check - health is frozen while Deactivated, so nothing to gate on; healing happens normally via
the bed after reactivation releases the freeze.

## Confirmed from testing (SkeletonRebirthDiagnostics)

- **Death is not purely a `blood`-loss check.** A live test (KO'd character left to deteriorate)
  died with `blood=78/100` — nowhere near zero. Per the user's in-game knowledge, a **head or
  torso injury reaching `flesh <= -200`** is a confirmed fatal threshold for those parts
  regardless of overall blood level. This lines up with `HealthPartStatus::isDead()`
  (MedicalSystem.h:159) being the real per-part death check, not whole-body blood. The diagnostic
  now logs `flesh`/`fatal`/`isDead()` for both `PART_HEAD` and `PART_TORSO` per character to
  confirm this directly next time (`SkeletonRebirthDiagnostics.cpp`).
- **`MedicalSystem::dead` / `Character::isDead()` are already `true` *before* `declareDead()`
  runs.** Logged at the moment `declareDead()` was about to fire, both flags were already set.
  This means whatever decides "this character is dead" happens upstream of `declareDead()` -
  `declareDead()` is the commit/cleanup step (corpse conversion, loot drop, etc.), not the
  decision point. **New risk to check**: does anything else in the engine react to `dead` flipping
  true independently of `declareDead()` actually executing? If so, a "block death before
  `declareDead()` commits" hook may be intercepting one tick too late for some systems.
- `proneState` was `PS_KO` (4) at time of death, consistent with "died while knocked out," not a
  separate death animation state.
- `Character::loadFromSerialisePostCreationStage()` fires on **any character construction from
  data**, not just a real player save/load - it fired repeatedly for the same NPC name with a
  fresh handle each time during ambient world spawns/encounters within a single session.
- **RESOLVED — Character* does NOT survive across sessions.** Same NPC ("Steel"), same
  `handle=1-2313720576-1-1776489216-1`, logged in two separate game sessions: `ptr=8B464BF0` in
  the first, `ptr=8BAA2128` in a later one. Same logical character, freshly reconstructed object.
  **Any Deactivated-bookkeeping side-table must key on the handle string, never the raw pointer.**
- **New: there appears to be a third death path, independent of both blood loss and the -200
  injury threshold.** A second live death (`Steel`) showed `blood=100` (full) and
  `head(flesh=76.27 isDead=0) torso(flesh=200 isDead=0)` — neither part anywhere near -200, both
  individually still `isDead=false` — yet the character died overall. Likely something like
  "left KO'd/unattended too long," distinct from the direct-injury path. Doesn't change the hook
  plan (still funnels through `declareDead()`), but means the Deactivated feature will need to
  handle at least two different fatal triggers, not one.
- **`fatal` on `HealthPartStatus` is unresolved — two competing explanations, neither confirmed.**
  Expected meaning (per game knowledge): a dynamic flag meaning "a fatal-type wound has been
  struck here, and it'll kill if it deteriorates to a large negative number." But logged data
  contradicts that: `fatal=1` was already true on `Steel` at full, uninjured health
  (`flesh=200/200`) right at load, before any damage happened. So either the meaning is more like
  "this part-type is capable of a fatal wound" (static, not injury-dependent), or the RE'd field at
  this offset isn't actually the same bit the in-game UI calls "fatal" - reverse-engineered field
  names are best-guess labels, not confirmed. Don't build logic on this field's meaning yet; the
  confirmed dynamic check is `HealthPartStatus::isDead()`.
  Likely explanation: the in-game UI shows escalating severity on a wounded part via `<`, `<<`,
  `<<!!` chevrons as `flesh` drops toward the fatal threshold (~-200) - that's probably just a
  *display* bucketing of the numeric `flesh` value, not a separately stored flag at all, which
  would mean the RE'd `fatal` bool is unrelated to it. `flesh` itself (already logged) is the
  real source of truth here. Not blocking - only worth confirming later by watching `flesh` values
  against the UI chevrons on one untreated injury if the exact thresholds ever matter.
- **Problem: `dailyUpdate()` has never fired, even across a full in-game day**, across multiple
  test sessions. Either it isn't the per-day tick assumed, or it's gated on something (active
  player squad membership?) that idle/KO'd NPCs don't satisfy. **The 24-hour-sweep hook plan in
  §3 needs a different mechanism** until this fires at least once - don't build on `dailyUpdate()`
  yet.

## Open questions / must-verify-before-building

1. ~~Does Character* survive save/load~~ — **RESOLVED: no**, see confirmed-findings above. Side
   table must key on the handle string.
2. ~~Which `ProneState`/`CollapseStage` combination prevents vanilla auto-recovery~~ — superseded:
   the fatal mechanism for a single-injury death is per-part `flesh` reaching a threshold around
   `-200` on `PART_HEAD`/`PART_TORSO` — but a second, still-unidentified death path also exists
   (see confirmed-findings above) that doesn't involve blood or part thresholds. What drives
   KO -> full recovery (vs. KO -> either death path) is still unconfirmed - blocked on
   `dailyUpdate()` not firing (see below).
3. ~~Whether `robot` (broad) or a specific skeleton race string ID (narrow) is the right filter~~ —
   **DECIDED: broad.** Revival mechanic is planned to expand to all robots, not just Skeletons.
4. ~~Whether zone-level or per-character is the more reliable "player isn't nearby" signal~~ —
   **RESOLVED: per-character.** `Character::isOnScreen`, edge-detected via `updateOnScreenCheck()`
   with a debounce, is confirmed to give a clean transition signal. Zone-level enumeration wasn't
   needed.
5. ~~Find a periodic hook that actually fires~~ — **RESOLVED: not needed.** §3 no longer uses a
   periodic hook at all (see revised §3, lazy on-relevance check triggered by
   `updateOnScreenCheck()`). `Character::offscreenUpdate()` confirmed dead end (never fired).
6. **New, only remaining open item: tune the debounce** for the `updateOnScreenCheck()` trigger -
   how many consecutive same-value calls (or how much real time) should be required before acting,
   to avoid a flickering character (observed: hundreds of flips within under a second) getting
   swept mid-flicker. Not blocking - a reasonable default (e.g. a few seconds of sustained
   `isOnScreen == false`) can be picked and adjusted later if needed.

## Reference index

- `Character::declareDead()` — Character.h:411 (RVA 0x7A4FA0, non-virtual)
- `Character::getRace()` — Character.h:562 (virtual)
- `RaceData::robot` — RaceData.h:48
- `Character::dailyUpdate()` — Character.h:216 (non-virtual - confirmed never fires, don't use)
- `Character::updateOnScreenCheck()` — Character.h:220 (non-virtual - confirmed the §3 cleanup
  trigger, needs debounce)
- `Character::offscreenUpdate()` — Character.h:221 (non-virtual - confirmed never fires, don't use)
- `Character::isOnScreen` / `isVisibleAndNear` — Character.h:218-219
- `ProneState` enum — Character.h:53-60
- `MedicalSystem::CollapseStage` — MedicalSystem.h:231-238
- `MedicalSystem::dead` — MedicalSystem.h:291
- `MedicalSystem::HealthPartStatus::isDead()` — MedicalSystem.h:159
- `MedicalSystem::HealthPartStatus::update()` — MedicalSystem.h:151 (non-virtual - NOT hooked in
  the current design; an earlier version skipped this entirely to freeze health, retired because it
  blocked repair-kit healing - see Status section)
- `Character::update()` — Character.h:235 (virtual, needs `_NV_` thunk - confirmed source of the
  `setProneState()` recovery-attempt loop; still hooked, skipped entirely for Deactivated
  characters)
- `MedicalSystem::medicalUpdate(float frameTime)` — MedicalSystem.h:115 (non-virtual - **hooked,
  confirmed necessary**: skipped entirely for a Deactivated character's medical system, both to stop
  `declareDead()` re-firing continuously and as a deliberate choice to freeze health completely -
  see Status section)
- `MedicalSystem::applyFirstAid(float skill, Item* equipment, float frameTime, Character* who)` —
  MedicalSystem.h:250 (RVA 0x64D080, non-virtual - **hooked, confirmed necessary**: real
  reactivation trigger)
- `MedicalSystem::applyDoctoring(...)` — MedicalSystem.h:251 (RVA 0x649280, non-virtual - confirmed
  always co-fires with `applyFirstAid()`, not hooked, redundant)
- `InventoryItemBase::itemFunction` — Item.h:76, type `ItemFunction`
- `ItemFunction::ITEM_ROBOTREPAIR` — Enums.h:238
- `Character::inSomething` (type `UseStuffState`) — Character.h:595
- `Character::inWhat` (type `hand`) — Character.h:596
- `UseStuffState::IN_BED` — Character.h:129-134
- `hand::getRootObject()` — util/hand.h:54 (RVA 0x79C1B0, non-virtual)
- `Building::_NV_getSpecialFunction() const` — Building/Building.h:220-221 (RVA 0xF6AF0, virtual,
  vtable offset 0x2F0)
- `BuildingFunction::BF_SKELETON_BED` — Enums.h:150 (**confirmed value `25`** via live
  `bedSpecialFunction` logging - do not trust the raw FCS enum-position guess)
- `TimeOfDay::timeOfDayHasPassed` — util/TimeOfDay.h:33
- `GameWorld::getTimeStamp_inGameHours()` — GameWorld.h:238
- `GameWorld::getTimeFromStamp_inGameHours(double)` — GameWorld.h:237
- `ZoneMap::isActive()` — ZoneManager.h:228
- `ZoneManager::getAllActiveZones()` — ZoneManager.h:336
- Existing hook pattern to follow — `/home/bryan/Git/KenshiLimbs/The Limbless (Type 2)/RobotLimbRaceLock.cpp`
