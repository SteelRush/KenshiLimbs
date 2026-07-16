# SkeletonRebirth — design

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state directly. Instead they collapse into a
**Deactivated** state - reversibly, since the real death transition (`Character::declareDead()`)
itself never runs, and `MedicalSystem::dead` is kept false throughout. Placing a Deactivated robot in
a Skeleton Repair Bed prompts reactivation via a confirmation dialogue box, whose text and button
behavior are data-driven from RE_Kenshi.json's `"DialogueBoxes"` object (see §4) rather than hardcoded C++. All of
this survives save/reload. Cleanup of unattended Deactivated robots (random-spawn or otherwise) is
**not implemented** - see §5 for why, and what was tried.

File:line references are to `/home/bryan/Git/RE_Kenshi/KenshiLib/Include/kenshi/`.

## A pitfall worth flagging up front: hooking virtual functions

Two different trigger mechanisms in this mod's history were hooked incorrectly by taking the address of
a **virtual** method (`InventoryGUI::show`, then almost `HandleManager::restore`) the same way this
codebase hooks everything else: `KenshiLib::AddHook(KenshiLib::GetRealAddress(&Class::Method), ...)`.
For a non-virtual method this works because KenshiLib generates a real, addressable stub for it. For a
virtual method, `&Class::Method` doesn't resolve to that stub at all — it resolves to an MSVC-generated
vtable-dispatch thunk that lives in *our own* module, which fails `GetRealAddress`'s stub-range check
and crashes with KenshiLib's own "enable whole program optimization" assert (confirmed via live testing
with `InventoryGUI::show`). The fix every time was the same: use the class's own non-virtual `_NV_`
prefixed wrapper instead (e.g. `HandleManager::_NV_restore`, `InventoryTraderGUI::_CONSTRUCTOR`) - same
RVA as the real implementation, safe to hook. This convention exists precisely for this reason; check
for a `_NV_` (or plain non-`virtual`-marked) sibling before hooking anything whose header comment says
`vtable offset`. Calling a virtual method *normally* on a live object (e.g.
`result->_NV_getCallbackCharacter()`) is unaffected by any of this - the problem is specific to taking
the address for hooking, not to virtual dispatch itself.

## Architecture

### 1. Deactivated state

`Character::declareDead()` is hooked. For robots, the original call is blocked entirely, and
`MedicalSystem::dead` is forced to **false** every time this fires - not just left alone.

This is the single most load-bearing line in the whole mod, and it isn't obvious why it's needed:
live testing found `medDead=1` already true *on entry* to this hook, before any of this file's own code
had touched it - some upstream native code (combat/damage resolution) sets `dead=true` directly, and
`declareDead()` is only the finalize step called afterward, not where the flag originates. Leaving
`dead` alone (assuming "we never set it true, so it stays false") does not work - it has to be actively
reset back to false here, every time, or every native system that reads it treats the character as a
real, permanent corpse regardless of anything else this file does.

An earlier version of this mod deliberately left `dead=true` after blocking `declareDead()`, on the
theory that GUI/party-membership/AI/looting logic all read that one flag directly, so leaving it true
would make a Deactivated robot "operationally dead" everywhere for free. Live testing found this isn't
a single choke point: `HandleManager::destroy()` destroys any `dead=true` character when its zone
unloads (reason `"corpse unloaded"`), and `PlayerInterface::update()` separately, independently prunes
`dead=true` characters from the player's own party roster - two unrelated native systems, discovered one
at a time, each requiring its own investigation. A reactive fix for the second one (re-`recruit()`ing a
character the roster had silently dropped) caused a real crash - `STATUS_ACCESS_VIOLATION` reading
address `-1`, confirmed by manually parsing the resulting `crashDump*.zip`'s minidump (no proper
minidump-parsing tool handled this Wine-generated dump correctly - it was walked by hand from the raw
`MINIDUMP_HEADER`/`MINIDUMP_DIRECTORY`/`MINIDUMP_EXCEPTION_STREAM` structures). Root cause: `recruit()`
was being called unconditionally every single frame while the character read as still missing from the
roster, rather than once per actual removal event - not a function meant to be hammered like that.
Fighting each native system as it surfaced this way, with no guarantee a third wouldn't turn up next,
was abandoned as unsustainable. `dead` is now never set true for a Deactivated robot at all, so none of
this native machinery has any reason to ever engage.

**Why a Deactivated robot doesn't just get up and walk away**: nothing explicit makes it inert.
Vanilla's own knockout state (`Character::ProneState::PS_KO`, entered naturally as part of the same
combat sequence that would otherwise have killed it) has no recovery timer at the catastrophic damage
level that triggers this hook - confirmed live, and by design: `MedicalSystem::knockoutTimer`/
`startKnockoutTimer()` only apply below some damage threshold. An explicit AI-pause
(`AITaskSytem::setJobsEnabled(false)`/`clearOrders()`/`clearJobs()`, reached via
`Character::getAI()->getTaskSystem()`) was tried as a safeguard and then removed once confirmed
redundant - permanent KO already does the job on its own.

`MedicalSystem::medicalUpdate(float)` is hooked and skipped entirely for a Deactivated character,
freezing health at whatever fatal value triggered Deactivation. Under the current `dead=false` design
this hook is genuinely load-bearing (medicalUpdate keeps firing normally for a merely-KO'd-but-alive
character) - unlike the old `dead=true` design, where the engine stopped calling it at all once a
character was genuinely dead, making the freeze redundant for different reasons.

`Character::update()` is deliberately **not** hooked/skipped - skipping it breaks position syncing while
a Deactivated character is being carried, and causes a rigid, non-ragdolling corpse.

`g_deactivated` stays `Character*`-keyed and session-only for hot per-tick lookups deliberately - see
§6 for how it survives save/reload without needing every lookup site to switch to a string key.

### 2. GUI status tag override

The character info panel's "State:" row (category 3) is overridden **unconditionally** for any tracked
Deactivated character - not by matching specific text. This had to change from an earlier, narrower
version that only rewrote the row when it detected the literal word "dead": that approach depended on
`dead=true` (see §1's history) so the native corpse-only text-setting call would actually fire; now that
`dead` stays false, that specific corpse-only call never fires at all, and whatever text *would* show for
a character frozen at fatal health while still alive needed to be overridden regardless of what it says.
Confirmed live: vanilla's real text here is `"Rebooting"` for robots (organic races reportedly show
`"Recovery coma"`), colour-tagged `#59231a` - the same dark red vanilla uses for its own "Dead" text
elsewhere, reused here for the override's colour too, on a plain `"POWER FAILURE"` string.

`DatapanelGUI::setLine(key, s1, s2, category, last, keyVisible)` is the hook point; `self->getObject().
getCharacter()` identifies which character the panel belongs to. This required checking - and ruling
out - essentially every other `setLine*` overload on `DatapanelGUI` (there are ~7 total signature
variants, all now confirmed logged live and excluded: `setLineStatInfo` only carries skill/stat values,
`setLineFaction` never fires at all for this row, none of the remaining `setLine(...)` skin/bar-value/
no-key variants carry it either).

**Known gap, not fixed, deprioritized**: there's a second, separate "overall health status" text (also
showing `"Dying"`/`"Recovery coma"`), visible without opening any panel - part of the persistent HUD,
directly right of the always-visible name+faction display. It does not go through any
`DatapanelGUI::setLine*` overload - exhaustively ruled out live, including specifically on
`MainBarGUI::getMedicalPanel()`'s own `MedicalDatapanel*` instance (compared by raw pointer address
against every `self` seen in the hooked overload; zero matches, ever). `MedicalDatapanel` only exists as
a forward declaration anywhere in RE_Kenshi's headers - none of its own methods have ever been reverse
engineered, so there's no RVA to hook directly, and the pointer-comparison result confirms it either
doesn't inherit `DatapanelGUI` at all, or never calls `setLine` on this content specifically. It's
updated via some other mechanism - most likely a raw MyGUI widget outside the `DataPanelLine`
abstraction entirely (`MainBarGUI` has several plain `MyGUI::TextBox*` members like `dayText`/
`moneyText`/`biomePanelText` set this way). Finding it from here would mean a live widget-tree walk
(`MyGUI::Widget::getChildCount()`/`getChildAt()`) with speculative type-casting to read captions -
`Widget`'s base class only exposes a generic *setter* (`setProperty`), not a getter, so reading an
arbitrary widget's caption means guessing its concrete type and casting to it, which risks a crash on a
wrong guess. Judged not worth that risk for a second cosmetic label once the panel-level "POWER FAILURE"
override was in place - closed out, not pursued further.

Finding the *original* status-tag logic (back when this was still a `dead=true` design) required static
analysis of the shipped `kenshi_x64.exe`, not just the headers: `MedicalSystem::getMedicalGUIData`'s RVA
as declared in `MedicalSystem.h` (`0x889140`) is simply wrong - it actually points at an unrelated
wind-speed tooltip formatter (confirmed by its `.rdata` string refs: `"{1,num} (+{2,num}) mph"` / `"The
current amount of wind."`). KenshiLib's RVA database (loaded at runtime from `RE_Kenshi/RVAs/*.br`, not
the header comment) has this entry wrong for this build/version. The real logic was found by
cross-referencing literal ASCII strings in the binary back to their call sites via `objdump`. Later,
unrelated attempts at *general* RVA-blind linear disassembly (trying to trace native callers of
`HandleManager::destroy()`) proved unreliable the same way for a different reason - MSVC padding/jump
tables misalign a naive linear sweep even when the RVA itself is a correct, working hook target - and
were abandoned in favour of dynamic (live) diagnostic hooking throughout the rest of this investigation.

### 3. Reactivation trigger and confirmation UI

**Trigger**: simply placing a Deactivated robot in the Skeleton Repair Bed (`isInSkeletonBed()` -
`Character::inSomething == IN_BED` and `Building::_NV_getSpecialFunction() == BF_SKELETON_BED`, value
`25`) is enough - no item requirement, checked continuously via `Character::updateOnScreenCheck()`
(fires roughly every frame per character). Three earlier approaches were tried and rejected:
- `MedicalSystem::applyFirstAid()` (the original design, gated on using a `ITEM_ROBOTREPAIR` item) broke
  once `dead=true` took effect under an earlier design - a genuinely dead character isn't a valid target
  for the normal AI-driven "treat with item" action at all, confirmed by direct testing (repair kits
  stopped working). Moot now that `dead` stays false, but not reverted back to, since the polling
  approach below is simpler regardless.
- `InventoryGUI::show()` (triggering off the loot/trade menu opening) crashed the game - see the virtual-
  function pitfall above.
- `InventoryTraderGUI::_CONSTRUCTOR()` (the non-virtual-safe fix for the above) worked, but
  `Character::updateOnScreenCheck()` was simpler still - it already fires every frame per character for
  vanilla reasons, so simply checking "is this Deactivated robot now in the bed" on it needs no new hook
  site at all, and was adopted instead.

**Confirmation UI**: a real dialogue box loaded from Kenshi's own `Kenshi_MessageBox.layout` via
`MyGUI::LayoutManager::getInstance().loadLayout(...)`, recentered on screen (the layout's own
`position_real` anchors it top-left by default - `Widget::setRealCoord()` with the same fixed
width/height from the layout file, just recentered: `left/top = (1 - width/height) / 2`; no real-size
getter exists in this SDK to compute that dynamically). Its content is data-driven, not hardcoded per
dialogue - see "JSON-driven dialogue boxes" below - with `MessageText`/`ButtonA`/`ButtonB`/`ButtonC`
widgets found by name and relabeled via the generic `Widget::setProperty("Caption", ...)`, any button
beyond however many the JSON entry defines hidden. Three earlier approaches to the box itself (as
opposed to what drives its content, which came later) were tried and rejected:
- `Dialogue::startPlayerConversation()` (the original design) stopped working once an earlier design's
  `dead=true` took effect - live testing showed the game suppresses dialogue entirely for a dead
  character. Moot now, for the same reason as `applyFirstAid()` above.
- `ForgottenGUI::messageBox()` actually rendered (confirming it isn't gated by aliveness), but its `btn`
  parameter is completely undocumented in RE_Kenshi's headers (just `int btn`, no enum), and a guessed
  value produced a single "No" button instead of Yes/No. No call sites were found anywhere in the
  binary's `.text` section to reverse-engineer the real values from.
- Hand-built widgets via `ForgottenGUI::createPanel()`/`createButton()` were tried next, with two
  different button skins - one rendered a visible but uncaptioned button, the other rendered nothing at
  all. Root cause, found by reading Kenshi's own UI files on disk (`data/gui/layout/*.layout`,
  `data/gui/skins/*.xml`, `data/gui/templates/kenshi_templates.xml`): real Kenshi buttons/windows are
  composed from *`ResourceLayout` templates*, not plain single-skin widgets - `createButton(...,
  "Kenshi_Button2")` was never going to work, since `"Kenshi_Button2"` is a template name, not a skin.
  This is what led to loading the real layout directly instead of reconstructing it from primitives.
  (Also: `createPanel`/`createLabel`/`createButton`'s float position/size parameters are normalized
  `0.0-1.0` screen fractions, matching `position_real` in the `.layout` XML - not pixels. An early
  attempt used pixel-ish values and the panel rendered far off-screen, invisible despite succeeding.)

**`TryReactivate()`** nudges every part in `MedicalSystem::anatomy` (the real, complete part list -
`lektor<HealthPartStatus*>`) that's still in fatal (negative) `flesh` territory 1% toward zero, clamped
to that part's own `maxHealth()`. This replaced an earlier version that only nudged two hardcoded parts
via `getPart(PART_HEAD/PART_TORSO, SIDE_NEITHER)`: `HealthPartStatus::PartType` only has four generic
buckets (`TORSO`/`LEG`/`ARM`/`HEAD`), but Kenshi's real damage model has separate "chest" and "stomach"
parts that both fall in the `TORSO` bucket - `getPart()` only ever reached one of them. A kill that
happened to land on the unreachable one caused an instant re-death after every reactivation (confirmed
live: a `describeAnatomy()` diagnostic showed two separate `type=0` (TORSO) entries, one healthy, one
still at its original fatal `flesh` value no matter how many times "Yes" was clicked). Iterating
`anatomy` instead can't miss a part regardless of how many distinct locations a race actually has. The
clamp to `maxHealth()` is necessary because `anatomy` includes already-healthy parts too (most of a
robot's body, on any kill) - nudging every part unconditionally (rather than only ones still negative)
pushed already-healthy parts past their normal maximum, also confirmed live before being narrowed to
fatal-only.

### 4. JSON-driven dialogue boxes

Requested explicitly: dialogue box *content and button behavior* shouldn't be hardcoded C++ - a
`"DialogueBoxes"` object nested inside `RE_Kenshi.json` itself (not a separate file - matching how the
old, removed `ConversationOverrides`/`DialogueSkillChecks` system nested its own config in that same
file) defines each dialogue box's title, message, and per-button gating + behavior; a generic
`showDialogueBox(dialogueId, patient, initiator)` loads the entry and wires it up, instead of a
dedicated `Show*`/`On*Clicked` function pair per dialogue.

A button's behavior is an ordered list of **steps**, not a single action - directly mirroring that old
system's per-reply override list (`reactivate_skeleton`/`take_item`/`show_text`/`delay` types), just
attached to a dialogue box button instead of an FCS dialogue reply. Reimplemented from scratch rather
than restored verbatim, since there's no `Dialogue`/`DialogLineData` object here to hook into - the
shipped `system_menu` entry exercises all four step types, plus a second registered action
(`system_reset` - see `DialogueAction_SystemReset()`, which wipes every skill and core attribute to 1
and fast-recruits the patient via `PlayerInterface::recruit()`):

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

`{name}` is substituted with the patient's name wherever it appears (`message`, and any `show_text`
step's `text`). Up to 3 buttons per entry (`Kenshi_MessageBox.layout` has exactly `ButtonA`/`ButtonB`/
`ButtonC`) - see the button-gating/visibility rules below for how "up to 3" is actually resolved from a
JSON array that might define more, or fewer be eligible.

**The four step types** (`DialogueBoxStepDef::type`), run in order by `dispatchDialogueSteps()`:
- `"action"` — dispatches `action` through `g_dialogueActions` (a `std::map<std::string,
  DialogueActionFn>`, populated once in `startPlugin()`). `DialogueAction_Reactivate` (which calls
  `TryReactivate()`) is the one action registered right now. A button with an empty `steps` array (e.g.
  "No") does nothing but close - there's no dedicated close-only step type, since closing already happens
  unconditionally on any click, before any step runs (see `OnDialogueButtonClicked()`).
- `"take_item"` — consumes one of `item` (an FCS/GameData String ID) from the *initiator's* inventory
  (`Inventory::takeOneItemOnly()`), stopping the rest of the sequence if it fails (no silent partial
  effects - a robot shouldn't get "successfully revived" without the cost actually being paid). Distinct
  from the button-level `requiresItem` gate (see below) - `requiresItem` only controls whether the button
  is shown at all; a `take_item` step is what actually removes it, and doesn't run until the button is
  actually clicked.
- `"show_text"` — a floating rising-text notification via `ForgottenGUI::createScreenLabel()`, tracking
  the patient (`ScreenLabel::setTracking()`), **not** a GUI panel and **not** tied to the dialogue box in
  any way (the box is already closed by the time any step runs). Optional `color` is `"#RRGGBB"` hex,
  hand-rolled parsing (`tryParseHexColor()`) rather than trusting `MyGUI::Colour`'s own undocumented
  string-constructor format - same reasoning as every other hand-rolled parser in this file. Defaults to
  white if omitted or unparseable.
- `"delay"` — pauses the *remaining* steps in the sequence for `seconds`, not the whole dialogue system.
  A `std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences`, keyed by patient, holds
  the not-yet-run steps (copied, not referenced, into the pending entry) plus a resume index; ticked from
  `Character_updateOnScreenCheck_hook` (already firing every frame per character for the bed-placement
  trigger, reused rather than adding a new hook) via `GetTickCount64()` wall-clock milliseconds, not a
  per-frame delta, since `updateOnScreenCheck()` doesn't receive one and this is cosmetic UI pacing, not
  gameplay-critical timing - the same relaxed precision the old `delay` override type used, just driven
  by wall clock instead of `Dialogue::update()`'s `frameTime`.

**Button-level gating** (`requiresSkill`/`minSkill`/`maxSkill`, `requiresItem`, `excludePlayerFaction`)
controls whether a button is shown at all, evaluated once when `showDialogueBox()` is called, separately
from anything its steps do:
- `requiresSkill` (a lowercase `CharStats` field name - see `g_skillFields`, the same skill-name table
  the old `DialogueSkillChecks` feature used) plus `minSkill`/`maxSkill` (at least one required if
  `requiresSkill` is set, Kenshi's native 0-100 scale), checked against `initiator`'s
  `Character::getStats()`. An unrecognized skill name is logged once at JSON-load time and then treated
  as "no skill requirement" every time the button would otherwise show, rather than hiding it - a typo
  shouldn't silently make a button impossible to see and impossible to know why.
- `requiresItem` (an FCS/GameData item String ID, looked up via `GameDataManager::getData(id, ITEM)`) -
  the button only shows if `initiator`'s `Inventory::hasItem()` finds at least one. This is deliberately
  separate from a `take_item` *step* consuming the same item ID - unlike the old system (where FCS's own
  native `hasItem` dialogue condition handled visibility and the `take_item` override only ever handled
  removal, two systems that happened to compose), there's no separate native condition system here to
  lean on, so this file's own gate has to do both jobs, just via two different fields an author sets to
  the same item ID.
- `excludePlayerFaction` (bool) - hides the button if the *patient* (not the initiator) belongs to the
  player's faction, checked via `RootObjectBase::getFaction()`/`Faction::isThePlayer()`. Used on "Reset"
  so it's only offered for a wild/unaffiliated robot, not a recruited squad member.
- Eligible buttons are **compacted** into `ButtonA`/`ButtonB`/`ButtonC` in JSON order, not left as gaps -
  an ineligible button in the middle of a 3-button JSON array doesn't leave an empty slot where it would
  have been. If gating leaves zero buttons eligible for the current initiator, the box isn't shown at all
  (`showDialogueBox()` bails before loading the layout) - an all-buttons-hidden box would otherwise be an
  unclosable dead end.
- Item IDs come from JSON (user-authored, unverified), so the lookup (`getGameDataGuarded()`) is
  SEH-guarded (`__try`/`__except`) the same way the old `take_item` override's lookup was - a bad ID
  fails closed (button hidden / step stops the sequence) instead of crashing. MSVC forbids local C++
  objects with destructors in a function using `__try`/`__except` (`C2712`), so that function stays free
  of them, same constraint as the original.

This deliberately does *not* revive the mod's older, unrelated `Dialogue`-hooking system
(`Dialogue::replyClicked()`/JSON `"ConversationOverrides"`/`DialogLineData::checkConditions()`, removed
earlier - see git history) - that was for driving real in-world FCS conversation trees and ran into
`dead=true` suppressing dialogue entirely (see §3's rejected-approaches list). This system is unrelated:
it only configures the mod's own custom MyGUI panels, which were never gated by aliveness at all - the
four step types and the gating fields just happen to cover the same *use cases* that system did.

`OnDialogueButtonClicked()` is the single shared click handler for every button on every dialogue box -
it resolves which of `ButtonA`/`ButtonB`/`ButtonC` fired by comparing the clicked widget's identity
against `root->findWidget(...)` for each name (looked up *before* `closeDialogueBox()` unloads the
layout, not after), maps that index into the button list captured when the box was shown, closes the box
unconditionally, then hands the button's `steps` to `dispatchDialogueSteps()`.

Only one dialogue box shows at a time (`g_pendingDialoguePatient` gates `showDialogueBox()`) - a second
trigger while one is already open is silently dropped rather than stacking a second box.

`getOwnModDirectory()` (Win32 `GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` +
`GetModuleFileNameA`) is how the mod locates its own folder to load `RE_Kenshi.json` from - a mod DLL
has no working directory of its own. Reused verbatim from the old (removed) `ConversationOverrides`
system, which needed the same thing to find the same file.

### 5. Cleanup - not implemented

An unattended Deactivated robot - random-spawn or otherwise - currently sits inert in the world forever.
No automatic disposal exists. This is deliberate, not an oversight: getting the core Deactivated
mechanism itself solid took most of this mod's development, and cleanup was explicitly deferred rather
than layering more complexity on top before that core was proven out live.

Two earlier approaches were tried and abandoned, for different reasons:

- A **reimplemented cleanup-sweep timer** on `Character::updateOnScreenCheck()`: track a per-character
  "last seen on screen" timestamp, and once a Deactivated robot had been off-screen past some threshold
  (3 in-game days), erase it from `g_deactivated` and call the original `Character::declareDead()` to let
  real death proceed. This turned out to be solving an already-solved problem, discovered by accident
  under the (then-current) `dead=true` design: live testing found Deactivated robots disappearing from
  the world within *seconds to minutes* of their zone unloading - far faster than any sweep threshold. A
  diagnostic hook on `HandleManager::destroy()` caught the real cause: it fires with
  `reason="corpse unloaded"` - a native mechanism that already treats any `dead == true` character as a
  disposable corpse once its zone unloads. Vanilla was already doing, for free, exactly what "unattended
  robots eventually get cleaned up" would have asked for. The reimplemented sweep was removed.

- **Gating `HandleManager::destroy()` directly**, to selectively let random-spawn robots be disposed of
  by that native mechanism while exempting Player/Unique ones. This is what first surfaced that
  `dead=true` isn't a single choke point (see §1) - blocking this one call was confirmed, repeatedly, to
  *not* result in the character actually staying present/functional afterward, because a second,
  independent native system (`PlayerInterface::update()`'s party-roster prune) was removing it from the
  player's roster regardless of whether the handle-level object itself survived. Chasing that second
  system's fix caused the crash described in §1. Once `dead` stopped being set true at all, this gate
  became meaningless (native disposal is entirely gated on `dead==true`, so it never engages) and was
  removed along with all the diagnostic hooks built to investigate it (`HandleList`/
  `PlatoonHandleContainerList`/`ZoneMapHandleContainerList::destroy()`, several `ActivePlatoon`-level
  squad-bookkeeping functions, `PlayerInterface::update()`).

Any future cleanup design needs to work *without* relying on `dead=true`, since that's the entire reason
the current architecture exists - likely something bespoke on top of `g_deactivated` (an explicit
timestamp + `updateOnScreenCheck`-based sweep, similar to the first abandoned attempt, but calling the
*real* `declareDead()` afterward rather than fighting native corpse-disposal machinery for a character
that was never marked dead to begin with).

### 6. Save/load persistence

`g_deactivated` (and `g_reactivateDialogueShown`) stay `Character*`-keyed and session-only for the hot
per-tick lookups - `updateOnScreenCheck` fires roughly every frame per character, and switching every
lookup site to a handle-string key (a string construction per check) isn't worth it for maps already
gated to near-zero cost for the overwhelming majority of characters. Save/reload survival is handled
additively instead, as a write-through JSON side-file keyed by
`RootObjectBase::getHandle().toString()` (confirmed to round-trip correctly across save/reload via
`HandleManager::serialise`/`restore`) - none of the already-stabilized per-tick hooks needed to change.

No native "attach custom data to this object's own save entry" hook was found in RE_Kenshi's headers
(`GameData`'s `bdata`/`idata` maps are a plausible but unverified candidate - not worth betting on), and
no save/load *event* exists either (`SaveManager` only exposes a polled internal state machine, nothing
subscribable). Practical alternative: `saveDeactivatedState()` writes `SkeletonRebirth_Deactivated.json`
(array of handle strings) next to the active save (`SaveManager::getSingleton()->getSavePath()` +
`getCurrentGame()`) on every `g_deactivated` mutation (Deactivation, reactivation) rather than hooking a
pre-save event, so it can't be missed by a save trigger that turns out not to fire the way expected.
`loadDeactivatedState()` reads it back and resolves each handle string to a live `Character*` via
`hand::fromString()`/`hand::getCharacter()`, called from a hook on `HandleManager::_NV_restore` (see the
virtual-function pitfall note above for why not the virtual `restore()` directly) - the handle table
itself being restored is the natural "a load just happened" signal.

## Reference index

- `Character::declareDead()` — Character.h:411, RVA `0x7A4FA0`, non-virtual
- `Character::updateOnScreenCheck()` — Character.h:220, RVA `0x5C97E0`, non-virtual, returns `bool` —
  reactivation-trigger polling point
- `Character::isOnScreen` / `isVisibleAndNear` — Character.h:218-219
- `Character::isPlayerCharacter() const` — Character.h:568, RVA `0x790470`
- `Character::isUnique()` — Character.h:202, RVA `0x5061E0`
- `Character::getAI() const` / `AI::getTaskSystem() const` — declared locally in the .cpp (not via
  `kenshi/AI/AI.h` - see that file's own comment for why) purely for `getCurrentGoalStringSafe()`'s
  diagnostic logging; `AITaskSytem::setJobsEnabled`/`clearOrders`/`clearJobs` (AI/AITaskSystem.h,
  `OrdersReceiver` base) were tried and removed - see §1
- `Character::ProneState::PS_KO` — Character.h:53-59; `MedicalSystem::knockoutTimer` /
  `startKnockoutTimer()` / `knockout(float)` / `knockoutForceTimer(float)` — MedicalSystem.h:208-211 —
  vanilla's own permanent-at-catastrophic-damage knockout, relied on instead of any explicit AI pause
- `RaceData::robot` — RaceData.h:48
- `MedicalSystem::dead` — MedicalSystem.h:291
- `MedicalSystem::anatomy` — MedicalSystem.h:296, `lektor<HealthPartStatus*>` — the real, complete part
  list; `HealthPartStatus::PartType` (MedicalSystem.h:134-140) is a simplified 4-value view that can't
  reach every part (e.g. "chest" vs "stomach", both `PART_TORSO`)
- `MedicalSystem::HealthPartStatus::isDead()` / `maxHealth()` — MedicalSystem.h:159/172
- `MedicalSystem::medicalUpdate(float)` — MedicalSystem.h:115, non-virtual
- `MedicalSystem::getMedicalGUIData(DatapanelGUI*)` — MedicalSystem.h:261 — **RVA as declared
  (`0x889140`) is wrong**, do not hook via `GetRealAddress` (see §2) - not used in the shipped version
- `Character::inSomething` (`UseStuffState`) / `inWhat` (`hand`) — Character.h:595-596
- `UseStuffState::IN_BED` — Character.h:129-134
- `Building::_NV_getSpecialFunction() const` — Building/Building.h:220-221, RVA `0xF6AF0`, virtual
- `BuildingFunction::BF_SKELETON_BED` — Enums.h:150, value `25`
- `DatapanelGUI::setLine(key, s1, s2, category, last, keyVisible)` — DatapanelGUI.h:70, RVA `0x6FD4B0`,
  non-virtual — status-tag override hook, the only `setLine*` overload actually used; the other ~6
  (`setLineStatInfo`, `setLineFaction`, three more `setLine(...)` signature variants, `setLineText`) were
  all hooked diagnostically, ruled out, and removed - see §2
- `DatapanelGUI::getObject() const` — DatapanelGUI.h:100-101, virtual — resolves which character a panel
  belongs to (called normally at runtime, not hooked - see the virtual-function pitfall note)
- `MainBarGUI::medicalPanel` (`MedicalDatapanel*`) / `getMedicalPanel()` — gui/MainBarGUI.h:91/131 —
  ruled out as the source of the still-unfixed persistent health-status text (see §2) via live pointer
  comparison; not hooked in the shipped version. `MedicalDatapanel` itself is forward-declared only, no
  full definition anywhere in RE_Kenshi's headers
- `hand::getCharacter() const` / `fromString(const std::string&)` / `toString() const` — util/hand.h:49/39/38
- `MyGUI::LayoutManager::loadLayout(file, prefix, parent)` / `unloadLayout(widgets)` —
  mygui/MyGUI_LayoutManager.h:38/41 — loads `data/gui/layout/Kenshi_MessageBox.layout`
- `MyGUI::Widget::setProperty(key, value)` — mygui/MyGUI_Widget.h:257 — generic property setter, what
  the layout XML's `<Property>` tags compile down to; used instead of type-specific `setCaption()` so no
  casting from the generic `Widget*` `findWidget()` returns is needed
- `MyGUI::Widget::findWidget(const std::string&)` — mygui/MyGUI_Widget.h:206
- `MyGUI::newDelegate(void(*)(Args...))` — mygui/MyGUI_DelegateImplement.h (via mygui/MyGUI_Delegate.h -
  `MyGUI_DelegateImplement.h` is a macro-driven template generator, not includable directly)
- `MyGUI::Widget::setRealCoord(float, float, float, float)` / `setRealPosition` —
  mygui/MyGUI_Widget.h:130/137 — normalized 0.0-1.0 screen-fraction positioning, matching `position_real`
  in `.layout` XML; used to recenter the dialogue box (§3/§4). No matching real-size *getter* exists in
  this SDK.
- `getOwnModDirectory()` / `loadDialogueBoxesFromJson()` — this file, not RE_Kenshi/KenshiLib - locates
  the mod's own folder (Win32 `GetModuleHandleExA`/`GetModuleFileNameA`, no working directory of its own
  for a DLL) and loads `RE_Kenshi.json`'s `"DialogueBoxes"` object from it (see §4)
- `Character::getStats()` — Character.h:532, RVA `0xDEE40`, non-virtual — used for `requiresSkill`/
  `minSkill`/`maxSkill` button gating (§4)
- `RootObjectBase::getFaction() const` / `Faction::isThePlayer() const` — RootObjectBase.h:52,
  Faction.h:112 — used for `excludePlayerFaction` button gating (§4)
- `CharStats` skill fields (e.g. `robotics`, `science`, `engineer`, ...) — CharStats.h:103-135, all plain
  `float` members - see `g_skillFields` for the full lowercase-name-to-member-pointer table (§4). Only
  the plain skill floats, not derived attributes like `strengthActual()`
- `Character::_NV_getInventory() const` — Character.h:386, RVA `0x5E1760` (real vtable-offset-0 slot;
  non-virtual wrapper still used per this file's own convention even though offset 0 rarely shifts)
- `Inventory::hasItem(GameData*, int) const` / `takeOneItemOnly(GameData*)` — Inventory.h:179/191 —
  visibility-gate and actually-consume-it, respectively, for `requiresItem` (§4)
- `GameDataManager::getData(const std::string&, itemType) const` (via `GameWorld::gamedata`) —
  GameDataManager.h:28 — resolves an FCS/GameData item String ID to a `GameData*`; SEH-guarded here
  (`getGameDataGuarded()`) since the ID is JSON-authored and unverified
- `GetTickCount64()` — Win32 (`<Windows.h>`), wall-clock milliseconds since boot - drives a `"delay"`
  step's countdown (§4), not a per-frame delta, since `Character::updateOnScreenCheck()` doesn't provide
  one
- `ForgottenGUI::createScreenLabel(text, colour, LabelSize, RisingSpeed)` — gui/ScreenLabel.h (class
  declared there; factory method declared on `ForgottenGUI`, gui/ForgottenGUI.h:206), RVA `0x73E920` —
  a `"show_text"` step's floating rising-text notification (§4); `ScreenLabel`'s own constructor is
  protected, this factory is the real public entry point
- `ScreenLabel::setTracking(const hand&, const Ogre::Vector3&)` — gui/ScreenLabel.h, RVA `0x6E1BB0`,
  virtual — anchors a floating text label to the patient so it follows them
- `HandleManager::_NV_restore(std::ifstream&)` — HandleManager.h:201, RVA `0x36ADF0`, non-virtual
  wrapper (the virtual `restore()` at the same RVA can't be hooked directly - see the pitfall note)
- `HandleManager::destroy(const hand&, const char* reason)` — HandleManager.h:208, RVA `0x2AC790`,
  non-virtual — the native "corpse unloaded" cleanup mechanism (see §5); not hooked in the shipped
  version - `dead` never becomes true, so this never fires for a Deactivated robot regardless
- `SaveManager::getSingleton()` / `getSavePath() const` / `getCurrentGame()` — SaveManager.h:26/47/44
- `RootObjectBase::getHandle()` — RootObjectBase.h:63/78

## A note on the RE_Kenshi SDK itself

`Platoon.h` and `Building/Building.h` independently redeclared `enum BuildingDesignation`
byte-for-byte, with neither deferring to the shared `Enums.h` both already include - including both
headers in the same translation unit was a hard compile error (`C2011: redefinition`). Fixed upstream
(not part of this mod) by moving the enum into `Enums.h` and deleting both duplicates, in both
`/home/bryan/Git/RE_Kenshi` (the tracked SDK repo) and `/home/bryan/Git/KenshiLib_Examples_deps` (the
copy actually compiled against, which was silently out of sync with the tracked repo before this).
