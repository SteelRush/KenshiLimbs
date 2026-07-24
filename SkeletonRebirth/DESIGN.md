# SkeletonRebirth — design

Robot-race characters (`RaceData::robot == true` — Skeletons, Iron Spiders, Soldierbot Guards,
Security Spiders, etc.) never enter vanilla Dead state directly. Instead they collapse into a
**Deactivated** state - reversibly, since the real death transition (`Character::declareDead()`)
itself never runs, and `MedicalSystem::dead` is kept false throughout. Placing a Deactivated robot in
a Skeleton Repair Bed prompts reactivation via a confirmation dialogue box, whose text and button
behavior are data-driven from RE_Kenshi.json's `"DialogueBoxes"` object (see §4) rather than hardcoded C++. All of
this survives save/reload. Cleanup of unattended Deactivated robots (random-spawn or otherwise) is
**not implemented** - see §6 for why, and what was tried.

File:line references are to `/home/bryan/Git/RE_Kenshi/KenshiLib/Include/kenshi/`.

`DebugLog()` calls throughout this file are gated behind `verboseLog()`, on only when RE_Kenshi.json's
top-level `"Debug"` is `true` (default `false`) - regular play still generates real log volume (dialogue
boxes opening, reactivations, JSON reload summaries) even with `declareDead()` debounced, so this stays
off by default to avoid growing `RE_Kenshi_log.txt` indefinitely for players who never need it.
`ErrorLog()` is never gated - those indicate real problems and should always be visible.
`loadDebugSettingFromJson()` must run before the other JSON loaders so their own summary log lines
respect it, and is silent on a missing file/parse error (the other loaders open the same file and
already report those) or a missing `"Debug"` key (logging just stays off, the safe default).

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
`vtable offset`.

**A second, easy-to-miss pitfall in the opposite direction**: `_NV_` wrappers are *not* generally safe
for normal (non-hooking) calls either, if the method is actually overridden somewhere relevant. An
`_NV_` wrapper is a non-virtual, RVA-direct thunk for one specific class's own implementation - calling
`someObject->_NV_setAge(...)` always runs *that exact class's* code, regardless of `someObject`'s real
dynamic type, because there's no vtable indirection involved at all. This bit the animal-reset feature
(§4): `Character::_NV_setAge()`/`_NV_getAge()` were called on patients that were actually
`CharacterAnimal` instances, which override `setAge`/`getAge`/`getAge0to1` with real backing fields at
their own separate RVAs (`CharacterAnimal.h`) - the `_NV_` calls silently ran `Character`'s own base
implementation instead, with zero effect, confirmed live across three independent read-backs (raw age,
0-1 fraction, growth-stage string) and multiple species. The fix was to call the plain virtual method
(`patient->setAge(...)`, not `patient->_NV_setAge(...)`) so real dynamic dispatch picks the correct
override. Rule of thumb: `_NV_` is for taking an address (hooking); a normal call on a live object should
use the plain virtual name unless there's a specific reason to pin it to one exact class's
implementation regardless of the object's real type.

## A second pitfall: `hand`/selection resolution silently breaks for animals

Two independent things stopped working correctly for `ANIMAL_CHARACTER`-type robots (Iron Spiders etc. -
this is FCS's Human/Animal Character category, not the `CharacterAnimal` C++ class; see the `isAnimal()`
reference-index note) while working fine for humanoid robots, and both trace back to the same root cause:
`hand`-based identity resolution isn't reliable for this class of object.

**Persistence** (see §7 for the full history): the original JSON side-file persistence serialised
`RootObjectBase::getHandle().toString()` and resolved it back via `hand::fromString()` +
`hand::getCharacter()` on load. Confirmed live this fails completely for these characters - a real
save/reload logged `resolved=0 failed=8`. Separately confirmed `hand` values for this class of character
aren't even stable *within a single session*, no reload involved: the same live `Character*` reported two
different handle strings 36 seconds apart, three of five fields changed. `hand` isn't a durable identifier
for these characters regardless of whether a reload happens in between.

**Selection**: `PlayerInterface::selectedCharacter` (a `hand`) was used to check "is this my current
selection" for the persistent HUD text override (§2). Confirmed live that clicking directly on a wild
`ANIMAL_CHARACTER` left `selectedCharacter` resolving to a different, unrelated character the whole
time - not stale data, verified two independent ways (`hand::getCharacter()` and
`hand::getRootObjectBase()` agreed, and the resolved object's own live name was logged). Also confirmed
this isn't about squad membership either way - a wild, never-recruited *humanoid* robot updates
`selectedCharacter` correctly on click, so the split really is animal-vs-humanoid. Best working theory:
Kenshi's core selection model (giving orders, combat targeting) is built around humanoid interaction, and
clicking an animal may only open its inspect panel without ever making it a real `PlayerInterface`
selection.

**Lesson**: don't trust `hand`-based resolution (`getCharacter()`, `PlayerInterface::selectedCharacter`,
anything serialised through `hand::toString()`) for `ANIMAL_CHARACTER`-tagged objects, for either
persistence or live identity checks. Where a `hand`-free alternative already exists and is proven working
for both types - e.g. `DatapanelGUI::getObject().getCharacter()` - prefer reusing it over continuing to
debug the `hand`-based path.

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

**The same "hammered every call" pattern bit this hook too, later, in a different way.**
`declareDead()` doesn't fire once per character death - it fires *repeatedly*, for as long as an
already-Deactivated robot keeps taking damage (some upstream system keeps re-setting `dead=true` and
re-invoking it, same as the `medDead=1`-on-entry finding above). The hook originally did its full work
- `g_deactivated[self] = true`, `saveDeactivatedState()` (a real disk write), and a `DebugLog(describe(self))`
call - unconditionally on every single call, with no guard for "this character is already recorded".
Confirmed live via a real crash: the debug log showed 250k+ lines (82% of the entire session's log,
~550 calls/second sustained) from a handful of robots ("Weak Thrall") under continuous combat damage,
immediately preceding an ~67-minute timestamp gap and then `Unhandled Exception Filter called`/emergency
save - consistent with the sustained disk I/O and string-building load destabilizing the game over a
long session. Fixed by gating everything except the `med->dead` reset (still genuinely needed every
call) behind `if (g_deactivated.count(self)) return;` - the same "cheap gate first" discipline used
throughout this file's other hot-path hooks, applied here only after it had already caused a real
crash instead of before.

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
§7 for how it survives save/reload without needing every lookup site to switch to a string key.

Player-squad robots getting Deactivated is otherwise silent - no native "X is dead" message fires, since
`dead` never actually becomes true. `Character_declareDead_hook` calls Kenshi's own player-notification
queue (`showPlayerAMessage_withLog`) directly for this fixed message, rather than through
`showGameNotification()` (§4) - that helper exists for JSON-authored `{name}`/`{item}` dialogue text, and
this message needs neither.

**A part's real effective health is `flesh - fleshStun`, not `flesh` alone** - confirmed live via
`HealthPartStatus::derivedFleshHealthPercent`, which matches `(flesh - fleshStun) / maxHealth()` exactly
against logged values (and `flesh - fleshStun` alone matches the in-game UI's displayed number). A part
can carry real `fleshStun` from ordinary combat independent of `flesh`, so any code that sets `flesh` to
hit a target effective-health value must solve for `flesh` given the part's *current* `fleshStun`
(`targetFlesh = targetPercent * maxHealth() + fleshStun`), not treat `flesh` as the whole picture - writing
`flesh` alone once left a part at a far more negative effective health than intended, and separately meant
a reactivation floor that looked correct in `flesh` terms was still fatal in practice.

**Item-triggered instant death** (`Item::deathItem`, checked by `Inventory::deathCheck(Item*)` when a
death-tagged item is looted off a character) already goes through `declareDead()` like any other kill, so
blocking is unaffected - but unlike combat, it doesn't deplete `flesh`, so a Deactivated robot killed this
way had no fatal part for §7's persistence signature to detect after reload. `Inventory_deathCheck_hook`
finds whichever vital part (`PART_HEAD`, or either `PART_TORSO` part - chest and stomach are separate
parts that both bucket to `PART_TORSO`, `part->data->name` distinguishes them, not `stringID`, which is a
generic asset name shared across parts) has the lowest effective health (`flesh - fleshStun`), and writes
`-96.5%` of that part's own `maxHealth()` to `flesh`, offset by its `fleshStun` per the note above, then
calls `HealthPartStatus::updateDerivedHealths()` - setting `flesh` directly doesn't refresh `isDead()`'s
own cached result, confirmed live (two parts written to the identical `flesh` value disagreed on
`isDead()` until this was added). This makes the item-death path produce the same
native signature a combat death does, on whichever vital part narratively make sense as already weakest.

### 2. GUI status tag override

The character info panel's "State:" row (category 3) is overridden **unconditionally** for any tracked
Deactivated character - not by matching specific text. This had to change from an earlier, narrower
version that only rewrote the row when it detected the literal word "dead": that approach depended on
`dead=true` (see §1's history) so the native corpse-only text-setting call would actually fire; now that
`dead` stays false, that specific corpse-only call never fires at all, and whatever text *would* show for
a character frozen at fatal health while still alive needed to be overridden regardless of what it says.
Confirmed live: vanilla's real text here is `"Rebooting"` for robots (organic races reportedly show
`"Recovery coma"`), colour-tagged `#59231a` - the same dark red vanilla uses for its own "Dead" text
elsewhere, reused here for the override's colour too, on a plain `"POWER FAILURE"` / `"AI FAILURE"`
string. Which one is picked mirrors which core the FCS system-menu buttons require (RE_Kenshi.json's
`system_menu`/`system_menu_animal`, gated by `requiresRace`/`excludeRace`/animal trigger state - see
"FCS item requirements" below): `isHumanoidState()` (i.e. not `isAnimalCharacterType()`) combined with
`hasHeadPart()` (a direct `MedicalSystem::getPart(PART_HEAD, SIDE_NEITHER)` lookup, not a race-name
match) shows `"AI FAILURE"`; anything else - headless (e.g. "Skeleton No-Head MkII"), or animal-type even
though those still have a head part (e.g. Iron Spider) - shows `"POWER FAILURE"`.

`DatapanelGUI::setLine(key, s1, s2, category, last, keyVisible)` is the hook point; `self->getObject().
getCharacter()` identifies which character the panel belongs to. This required checking - and ruling
out - essentially every other `setLine*` overload on `DatapanelGUI` (there are ~7 total signature
variants, all now confirmed logged live and excluded: `setLineStatInfo` only carries skill/stat values,
`setLineFaction` never fires at all for this row, none of the remaining `setLine(...)` skin/bar-value/
no-key variants carry it either).

**Second override - persistent HUD health-status text**: a separate, always-visible label
(`MainBarGUI`'s `*_HealthText` widget, located once via a name-suffix widget-tree walk -
`findHealthTextWidget()`/`updateHealthTextOverride()`) shows vanilla text like `"Recovery coma"`/`"Dying"`
without any panel open, and does not go through `DatapanelGUI::setLine` at all (ruled out live, including
`MainBarGUI::getMedicalPanel()`'s `MedicalDatapanel*` - forward-declared only in RE_Kenshi's headers, no
reversed methods, so no RVA to hook directly there). Forcing its caption (`"Deactivated"`, same
`DEACTIVATED_COLOR`) requires knowing which character it's currently showing - originally checked via
`PlayerInterface::selectedCharacter.getCharacter()`, which does not reliably resolve for animals (see "A
second pitfall" above); fixed by reusing `g_lastInspectedCharacter`, tracked from the
`DatapanelGUI::getObject().getCharacter()` resolution already proven correct for the row above, instead
of depending on `selectedCharacter` at all. That tracking assignment must be unconditional, including when
the currently-open panel resolves to no character at all (a building, an item): earlier code only wrote
`g_lastInspectedCharacter` when a character was found, so inspecting a non-character object after a
Deactivated robot left the stale robot reference in place, and the health-text override kept stomping
"Deactivated" onto whatever was currently on screen and inspected next - e.g. a Research Bench.

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

**Trigger, humanoid robots**: either placing a Deactivated humanoid robot in a Skeleton Repair Bed
(`Character::inSomething == IN_BED`, checked via the `"inBed"` named state, on a building whose
`Building::_NV_getSpecialFunction() == BF_SKELETON_BED`, FCS String ID in the trigger's `buildings`
list - no item needed) or the item-based trigger below - both open `system_menu`.

**Trigger, animal-type robots (e.g. Iron Spider)**: item-based only - furniture-based reactivation never
worked reliably for animal skeletons, and the item-based trigger sidesteps the furniture-pose question
entirely.

`"await_repair"`'s completion check was originally `MedicalSystem::applyFirstAid()` activity (below), on
the assumption a specialist visibly arriving and animating meant that hook had fired. Live testing proved
that assumption wrong in two different ways: a specialist ordered to a bed-confined patient visibly
arrives and plays a repair animation without `applyFirstAid()` ever firing, and separately a humanoid
patient placed on open ground (`"Weak Thrall"`, race `95870-Newwworld.mod`) treated with a Skeleton
Repair Kit *also* never fired it despite the caregiver visibly performing the treatment action - so this
isn't specific to bed-confinement, `applyFirstAid()` just isn't a reliable signal for "is someone here
working on this patient" across every race/item/placement combination. Replaced with plain proximity
(`hasArrivedForRepair()`, `Ogre::Vector3::distance()` between patient and specialist positions) -
sidesteps the question of which native code path handles "care" entirely, since it only asks where the
specialist is standing, not what native function last touched the patient. A held-steady distance is
itself a reasonable proxy for "the animation is playing", since the specialist stops moving only once
they've actually engaged the patient. `AWAIT_REPAIR_ARRIVAL_DISTANCE` was first guessed at `3.0f` with no
idea what scale Kenshi's coordinates use - wrong by two orders of magnitude, confirmed live via the
`DIAG await_repair distance` log: a specialist visibly in position and animating held a constant
`182.528` for the full timeout window, never satisfying `3.0f`. Raised to `250.0f`, comfortably above
that confirmed real value.

**The (now sole) trigger** fires the same menu on using either of the game's two robot-repair items on a
Deactivated robot, while the caregiver has Science skill ≥ 1. This is a revival of the mod's *original*
design (see the rejected-approaches list just below) - `MedicalSystem::applyFirstAid()`
(`MedicalSystem.h:250`, RVA `0x64D080`, non-virtual) is hooked purely to populate `g_activeFirstAid` (a
`Character* patient -> Character* caregiver` map, cleared and re-set every frame - see §5), which backs
two named states: `"beingRepaired"` (just checks the map has an entry - only two items in the game can
make `applyFirstAid()` fire on a robot at all, so no separate item-identity check is needed) and
`"repairerScience1"` (reads the caregiver's `CharStats::science` directly, not the `skill` parameter
`applyFirstAid()` receives - live logging showed that value drifts frame-to-frame during a single repair
session, roughly 24.54 → 25.45 over ~1,460 calls, so it's some per-tick computed effective value rather
than a stable stat snapshot, and unsuitable for a one-time gate). Confirmed live (not just from the
header) that `who` is the caregiver, not the patient - reached the same way as the `medicalUpdate()` hook
reaches its owning `Character*`, via `getPart(PART_HEAD)->me`.

**`"await_repair"` step**: makes the "Diagnostics" button's specialists (the squad members
`requiresSkill`/`requiresSkill2` resolved for that button, re-resolved fresh at click time rather than
reusing the message's copy - squad composition could theoretically change between the two) physically
walk to the patient and perform the repair animation before the item is consumed or "Service mode
loading..." shows, rather than that text appearing the instant the button is clicked regardless of
where anyone actually is. Confirmed live via a one-off diagnostic that `OrdersReceiver::addOrder`
(`Character::getOrdersReciever()`, the same order-issuing entry point vanilla right-click orders use)
with `FIRST_AID_ROBOT` genuinely walks a character to the target and plays the repair animation - not
just a state flag. Issued once per specialist when the step first runs (`clearOld=true`, so it
pre-empts whatever they were doing), not on every poll tick, which would otherwise restart their
approach path repeatedly. Completion is detected via `hasArrivedForRepair()` - see the note above on why
this is proximity-based rather than tied to `applyFirstAid()`. If both specialists (or
the one required, or neither if the button needs no skill) haven't started within the step's `seconds`
(default 45 if omitted/zero) the sequence aborts with a notification instead of silently proceeding
without them - same "no silent partial effects" principle as `take_item`'s failure path. This is also
what stops a squad member from reviving from across the map purely by satisfying the squad-wide skill
check: passing the gate only gets you the option to attempt it, the specialists still have to physically
get there.

**Clearing the order once repair is confirmed matters, not just issuing it.** Crashed live on "Reset":
`join_squad`'s `recruit(editor=true)` reassigns the patient's handle (confirmed via logged
before/after handles differing), and a specialist's `FIRST_AID_ROBOT` order was still live, targeting
the patient by its old identity, when that reassignment happened - a handle-vs-stale-order race. The
same `join_squad`→`system_reset` sequence had run crash-free earlier, before `await_repair` existed,
which pointed at the outstanding order rather than the already-documented `recruit()` flakiness above.
Fixed by calling `OrdersReceiver::clearOrders()` (the counterpart to `addOrder`, not `removeGoal`/
`removeJob` - those pair with `addGoal`/`addJob`, a different sub-system) on both specialists, but not
immediately once `specialistsReady` goes true - deferred specifically to right before the `"join_squad"`
action step fires (`dispatchDialogueSteps()`'s `"action"` handling, before `recruit()` runs), so the
specialists keep visibly working through `take_item`/the delays/`diagnostic_menu` instead of going idle
the moment the animation is first confirmed, only stopping at the one step that's actually unsafe to
run while their order is still live.

Earlier approaches tried and rejected on the way to the item-based trigger above (which, per the note
above, is now the only trigger, having also replaced the furniture-based one it originally coexisted
with):
- `MedicalSystem::applyFirstAid()` (the original design, gated on using a `ITEM_ROBOTREPAIR` item) broke
  once `dead=true` took effect under an earlier design - a genuinely dead character isn't a valid target
  for the normal AI-driven "treat with item" action at all, confirmed by direct testing (repair kits
  stopped working). This is exactly why it was dropped at the time - not a flaw in the idea itself, only
  in the surrounding `dead=true` design (§1). Once `dead` stayed false permanently, the idea was revisited.
- `InventoryGUI::show()` (triggering off the loot/trade menu opening) crashed the game - see the virtual-
  function pitfall above.
- `InventoryTraderGUI::_CONSTRUCTOR()` (the non-virtual-safe fix for the above) worked, but
  `Character::updateOnScreenCheck()` was simpler still - it already fires every frame per character for
  vanilla reasons, so simply checking "is this Deactivated robot now in the bed" on it needs no new hook
  site at all, and was adopted instead.

**Confirmation UI**: a real dialogue box built via `MessageBoxManager::createMessageBox()` - the same
native function the game itself uses for its own confirmation popups - rather than manually
`MyGUI::LayoutManager::getInstance().loadLayout("Kenshi_MessageBox.layout", ...)`-ing the layout
directly (an earlier version did this, and worked, but there was no way to confirm from inside this
mod alone whether its window chrome genuinely matched the rest of the game's UI or merely looked
plausible - going through the native entry point removes that doubt, since it's the same code path
rather than a lookalike). `createMessageBox()`'s real mangled name is too long for MASM's identifier
limit, so KenshiLib's `.inc` generator exports it under an arbitrary placeholder symbol
(`MessageBoxManager_createMessageBox_PLACEHOLDER`) instead of its true name - a recurring pattern
across ~24 functions in KenshiLib, not specific to this one - so it's called via an `extern "C"`
redeclaration under that literal exported name rather than the header's own declaration (which compiles
but fails to link). `MessageBoxManager` owns the resulting `MyGUI::Window`'s entire lifecycle -
creation, positioning, and teardown on click - so there's no layout to manually unload the way the
`loadLayout()` version needed; this file only tracks which patient/initiator/buttons a pending box
belongs to, for its `IDelegate1<int>` callback to use once a button's clicked (the `int` is the
button's index into the eligible-buttons list, not a widget lookup).

**Button captions have a real, confirmed character limit.** `Kenshi_MessageBox.layout`'s buttons
(`Kenshi_Button2` skin) have a fixed width, sized for short captions like the layout's own placeholder
"A"/"B"/"C" - text doesn't wrap or shrink to fit, it just clips on both sides once centered text
overflows. Confirmed live via screenshot: "Do nothing" (10 characters) rendered fully; "Run
Diagnostics" (15 characters) rendered as "un Diagnostic" - both ends clipped. `RE_Kenshi.json`'s
button captions were shortened accordingly ("Diagnose"/"Repair"/"Reset"/"Cancel"/"Do nothing"). Keep
future captions at roughly "Do nothing"'s length (~10 characters) or shorter.

**Message text renders center-aligned and this can't be fixed from outside the layout.** Tried: walking
`createMessageBox()`'s returned `MyGUI::Window*` to find the message `TextBox` (by exact caption match,
since the layout isn't reversed - no child name to hook by) and calling `setTextAlign(Align::Left)` on
it directly. Confirmed live via a full widget-tree dump that there's exactly one `_MessageText` widget
(no duplicate/shadow copy to miss), and its `getTextAlign()` already reports `Left Top` both before and
after the call - yet the text still renders centered. So `TextAlign` isn't what's actually driving this
widget's wrapped-line layout; Kenshi's skin for this widget does its own line-centering independent of
that property, and there's no other reversed hook into it. Left as vanilla center-aligned - not worth
chasing further for a cosmetic issue.

Its content is data-driven, not hardcoded per dialogue - see "JSON-driven dialogue boxes" below - with
each eligible button's caption and an integer id passed straight to `createMessageBox()`, any button
beyond however many the JSON entry defines simply not included in that list. Three earlier approaches
to the box itself (as opposed to what drives its content, which came later) were tried and rejected:
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

**`TryReactivate()`** iterates every part in `MedicalSystem::anatomy` (the real, complete part list -
`lektor<HealthPartStatus*>`, not `getPart(PART_HEAD/PART_TORSO, SIDE_NEITHER)`, which only reaches one of
Kenshi's two separate TORSO-bucket parts - "chest" and "stomach" - and can leave the other fatal) and
floors any critical part (`isCriticalPart()` - `PART_TORSO`/`PART_HEAD` only, limbs left untouched) still
below `-95%` of its own `maxHealth()` up to exactly that floor, clamped to `maxHealth()` as a backstop.
Floors rather than a relative nudge because a fixed percentage of the *current* value doesn't reliably
clear `isDead()` from deep overkill `flesh` - `-95%` reliably does, in one call, confirmed against the
same `-96.5%` value `Inventory_deathCheck_hook` (§1) writes for the item-death path. The floor is offset by
the part's own `fleshStun` the same way (§1's note on effective health) - a part can carry real stun
damage from combat independent of `flesh`, and ignoring it once left a reactivated character with an
effective health still fatal enough to immediately re-trigger `declareDead()`, confirmed live. Also calls
`updateDerivedHealths()` per part, same reason as §1 - writing `flesh` alone doesn't refresh `isDead()`'s
own cached result.

### 4. JSON-driven dialogue boxes

Requested explicitly: dialogue box *content and button behavior* shouldn't be hardcoded C++ - a
`"DialogueBoxes"` object nested inside `RE_Kenshi.json` itself (not a separate file - matching how the
old, removed `ConversationOverrides`/`DialogueSkillChecks` system nested its own config in that same
file) defines each dialogue box's title, message, and per-button gating + behavior; a generic
`showDialogueBox(dialogueId, patient, initiator)` loads the entry and wires it up, instead of a
dedicated `Show*`/`On*Clicked` function pair per dialogue. `initiator` (who `requiresSkill`/`requiresItems`/
`take_item` check against) is `PlayerInterface::selectedCharacter` - not `getAnyPlayerCharacter()`, which
returns an arbitrary squad member unrelated to who's actually carrying the required item. Safe to use here
specifically because the initiator is always the player's own humanoid character, unlike the patient - see
"A second pitfall" for why `selectedCharacter` is unreliable when the *target* is an `ANIMAL_CHARACTER`.

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
                    "requiresItems": [ { "item": "43392-changes_otto.mod", "count": 1 } ],
                    "steps": [
                        { "type": "take_item", "items": [ { "item": "43392-changes_otto.mod", "count": 1 } ] },
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
step's `text`). `{skillCharacter}`/`{skillCharacter2}` (box-level `message` only, not step text) name
the squad member `findSquadSkillStatus()` found qualifying for the first patient-applicable button's
(per `isDialogueButtonForPatient()`, not `eligibleButtons`) `requiresSkill`/`requiresSkill2` - see
"Button-level gating" below - falling back to "No one" if nobody currently qualifies, since that's an
expected outcome, not a data error. `{itemStatus}` (same scope as the two above) reads
`initiator->getInventory()->countItems()` against the same patient-applicable button's first
`requiresItems` entry (and that entry's own `count`) and resolves to "You have one."/"You don't have
one yet." when `count` is 1, or "You have X of the Y needed." when `count` is greater than 1 - added
so the message explains *why* a button like "Diagnostics" might be missing (missing item, and how many
short, vs. nobody in the squad qualifying) instead of the player having to guess.

`{skillStatus}`/`{skillStatus2}` (same scope, counterparts to `{skillCharacter}`/`{skillCharacter2}`)
close the equivalent gap on the skill side: blank once `{skillCharacter}`/`{skillCharacter2}` already
names a qualifying member (nothing more to say), otherwise `findSquadSkillStatus()` - the same
squad-scan as `findSquadMemberWithSkill()` (initiator preferred, then platoon), but tracking the
highest-value candidate seen even when nobody qualifies, not just a qualifying one - resolves to
"<bestMember> is closest at <skillField> <value> (needs <threshold>)." or, if nobody in the squad has
`CharStats` at all, "Nobody in the squad has trained <skillField> yet (needs <threshold>)." `<skillField>`
is the raw `requiresSkill`/`requiresSkill2` JSON string (e.g. "science"), not a display-name lookup -
matches how the message's own hardcoded "old world science"/"robotics" wording already names them
manually. `<threshold>` comes from `describeSkillThreshold()` - "at least X" / "at most X" / "between X
and Y" depending on which of `minSkill`/`maxSkill` the button set - resolving to `""` (no threshold to
report) if the button set neither, in which case `{skillStatus}` is left blank same as the qualified case.

Up to 3 buttons per entry (`Kenshi_MessageBox.layout` has exactly `ButtonA`/`ButtonB`/`ButtonC`) - see
the button-gating/visibility rules below for how "up to 3" is actually resolved from a JSON array that
might define more, or fewer be eligible.

**The step types** (`DialogueBoxStepDef::type`), run in order by `dispatchDialogueSteps()`:
- `"action"` — dispatches `action` through `g_dialogueActions` (a `std::map<std::string,
  DialogueActionFn>`, populated once in `startPlugin()`): `"reactivate"` (`TryReactivate()`, see §3),
  `"join_squad"`/`"join_squad_fast"` (`PlayerInterface::recruit()`, both now "fast" mode - see "Fast-mode
  recruit and the name prompt" below for why `"join_squad"` no longer uses "with edit"), and
  `"system_reset"` (`DialogueAction_SystemReset()`, wipes every skill/attribute to 1 for a humanoid
  patient, or takes an `ANIMAL_CHARACTER` patient's age to its minimum instead - real growth-stage
  governor for that type, called through true virtual dispatch, not `_NV_setAge()`, so `CharacterAnimal`'s
  override actually runs; see the virtual-function pitfall note at the top of this document). A button
  with an empty `steps` array (e.g. "No") does nothing but close - there's no dedicated close-only step
  type, since closing already happens unconditionally on any click, before any step runs (see
  `OnMessageBoxButtonClicked()`).
- `"take_item"` — consumes `items`, the same array-of-`{ "item", "count" }` (or plain-string, for
  `count: 1`) shape as a button's `requiresItems` - each entry's `count` units are removed from the
  *initiator's* inventory via repeated `Inventory::takeOneItemOnly()` calls. Distinct from the
  button-level `requiresItems` gate (see below) - `requiresItems` only controls whether the button is
  shown at all; a `take_item` step is what actually removes the item(s), and doesn't run until the
  button is actually clicked, so it re-checks `Inventory::hasItem()` for every entry before consuming
  any of them (see "Re-checking at click time" below) - no silent partial effects, a robot shouldn't get
  "successfully revived" without the full cost actually being paid.
- `"show_text"` — a floating rising-text notification via `ForgottenGUI::createScreenLabel()`, tracking
  the patient (`ScreenLabel::setTracking()`), **not** a GUI panel and **not** tied to the dialogue box in
  any way (the box is already closed by the time any step runs). Optional `color` is `"#RRGGBB"` hex,
  hand-rolled parsing (`tryParseHexColor()`) rather than trusting `MyGUI::Colour`'s own undocumented
  string-constructor format - same reasoning as every other hand-rolled parser in this file. Defaults to
  white if omitted or unparseable.
- `"notify"` — like `"show_text"`, but through Kenshi's own native player-notification queue
  (`showPlayerAMessage_withLog()`, the corner-text log used for things like "X is dead") instead of a
  floating `ScreenLabel` - `queued=true` so it takes its place behind any other pending notification
  rather than replacing one still showing.
- `"open_menu"` — opens a different `"DialogueBoxes"` entry by `menu` ID, the same way a reactivation
  trigger (§5) opens the first one. This is how nested/multi-level menus and "back" buttons work: a
  submenu is just another dialogue box, and "back" is just a button whose steps `open_menu` the parent's
  ID - there's no menu-stack/breadcrumb tracking, so each box only ever knows the one ID it was opened
  with and a "back" button has to name its target explicitly. Terminal, like `"delay"` and a failed
  `"take_item"` - the box that triggered this step is already gone by the time it runs (`MessageBoxManager`
  destroys it as part of the click), so `showDialogueBox()`'s one-at-a-time guard doesn't block this.
- `"delay"` — pauses the *remaining* steps in the sequence for `seconds`, not the whole dialogue system.
  A `std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences`, keyed by patient, holds
  the not-yet-run steps (copied, not referenced, into the pending entry) plus a resume index; ticked from
  `Character_updateOnScreenCheck_hook` (already firing every frame per character for dialogue-trigger
  evaluation, reused rather than adding a new hook) via `GetTickCount64()` wall-clock milliseconds, not a
  per-frame delta, since `updateOnScreenCheck()` doesn't receive one and this is cosmetic UI pacing, not
  gameplay-critical timing - the same relaxed precision the old `delay` override type used, just driven
  by wall clock instead of `Dialogue::update()`'s `frameTime`.
- `"await_repair"` — suspends the same way `"delay"` does, but resumes on a condition
  (`hasArrivedForRepair()` for both resolved specialists) instead of a fixed time, with `seconds` as a
  timeout rather than a wait length. See §3, "`"await_repair"` step", for the full mechanism.

`join_squad` (not `join_squad_fast`) always defers the rest of its sequence through the same
`g_pendingDialogueSequences` mechanism, implicitly - not something a JSON author writes. Originally this
existed because `recruit(editor=true)`'s character-editor path needed real wall-clock time to settle -
confirmed live that a later step (`system_reset`) running immediately after `recruit()`, in the same call,
crashed (`STATUS_ACCESS_VIOLATION` reading `-1`) even when the editor never visibly opened. A 1-tick defer
(`GetTickCount64() + 1`) still crashed intermittently (same exception, confirmed live via two identical
minidumps - same address, code, and thread ID, on two different characters); comparing traces showed the
successful case happened to take a real multi-second gap (waiting for the editor to close), while the
crashes matched the near-instant path. Raised to 5000ms on the theory that whatever `recruit()` does
internally needs that much real time to settle, not just a different call stack.

**`join_squad` no longer uses "with edit" mode at all** - see "Fast-mode recruit and the name prompt"
below for why - so the defer's original `isCharacterEditorMode()` branch is gone; always the fixed
5000ms wait now, kept at the value already confirmed crash-free above.

### Fast-mode recruit and the name prompt

The `STATUS_ACCESS_VIOLATION reading -1` crash above recurred live on the "Reset" button even with the
5000ms defer in place - same signature, confirmed via manually parsing the resulting Wine minidump (same
approach as before; standard tools don't parse Wine's minidumps correctly here either). Since
`system_reset`'s `ANIMAL_CHARACTER` branch only calls `setAge()` (no `CharStats` dependency on `join_squad`
having run "with edit" first), `DialogueAction_JoinSquad` was switched to `recruit(patient, false)` ("fast"
mode) for both branches, removing the character-editor code path entirely rather than continuing to defer
around it.

Trade-off: fast mode has no in-game renaming path of its own (that's only available via a plastic
surgeon, which animal-type robots can't interact with at all). `showNamePrompt()` covers this instead -
shown once the post-recruit defer elapses, in place of resuming the rest of the sequence directly. Built
from `ForgottenGUI::createPanel`/`createEditBox`/`createButton` using the same `Kenshi_WindowC`/
`Kenshi_Button2` skins the native message box layout already uses (confirmed compatible by reading
`Kenshi_MessageBox.layout` directly - its own "MessageText" widget is itself an `EditBox`), rather than
guessing at an untested skin. Confirm-only, no Cancel - the field is pre-filled with the current name, so
confirming without editing already is the "don't rename" path; there's nothing for a separate Cancel to do
differently. Pending state (`PendingNamePrompt`) is keyed by the Confirm button widget, not by patient -
nothing stops two robots from being mid-sequence at once.

**The defer and the name prompt are both intentionally baked into `"join_squad"` itself, not left as
separate JSON-composable steps** - the defer exists specifically to dodge the crash above, so it can't be
optional or something a JSON author might forget to add. `"join_squad"` (recruit + mandatory defer +
rename prompt) and `"join_squad_fast"` (recruit only, no defer, no prompt) are each already independently
reusable by name from any future button's `steps` array via `g_dialogueActions` - pick whichever fits
without needing to hand-assemble delay/prompt steps around either one.

**Button-level gating** (`requiresSkill`/`minSkill`/`maxSkill`, `requiresItems`, `excludePlayerFaction`)
controls whether a button is shown at all, evaluated once when `showDialogueBox()` is called, separately
from anything its steps do:
- `requiresSkill`/`requiresSkill2` (independent, lowercase `CharStats` field names - see `g_skillFields`,
  the same skill-name table the old `DialogueSkillChecks` feature used) plus their own `minSkill`/
  `maxSkill` and `minSkill2`/`maxSkill2` (at least one required per skill that's set, Kenshi's native
  0-100 scale). Checked squad-wide (`squadHasSkill()`/`findSquadMemberWithSkill()`, over
  `initiator->getPlatoon()->things`, each member resolved via `hand(RootObjectBase*).getCharacter()` -
  the same safe cast-by-handle pattern `getObject().getCharacter()` uses elsewhere in this file), not
  against `initiator` alone: each skill just needs *some* squad member in range, not the same member for
  both, so "Diagnostics" (Reactivate/Reset's entry point, requiring both Science and Robotics -
  representing Old World dual-discipline knowledge) can be satisfied by two specialists working together
  instead of demanding one character who's trained in both. `findSquadMemberWithSkill()` checks the
  initiator first (`characterHasSkill()`), before scanning the rest of the squad - live testing showed
  scanning `platoon->things` in storage order without this could pick a different, uninvolved squad
  member for `{skillCharacter}`/`{skillCharacter2}` and `await_repair`'s orders even when the initiator
  already personally covered that skill, sending someone else across the map to do a job the person
  already standing there could have done. An unrecognized skill name is logged once at JSON-load time
  and then treated as "no skill requirement" every time the button would otherwise show, rather than
  hiding it - a typo shouldn't silently make a button impossible to see and impossible to know why.
- `requiresItems` (an array of `{ "item": <FCS/GameData item String ID>, "count": <int, defaults to 1> }`
  objects, or plain strings as shorthand for `count: 1`, looked up via `GameDataManager::getData(id,
  ITEM)`) - the button only shows if `initiator`'s `Inventory::hasItem()` finds at least `count` of
  *every* entry (an empty or absent array means no item requirement at all). This is deliberately
  separate from a `take_item` *step* consuming an item ID - unlike the old system (where FCS's own
  native `hasItem` dialogue condition handled visibility and the `take_item` override only ever handled
  removal, two systems that happened to compose), there's no separate native condition system here to
  lean on, so this file's own gate has to do both jobs, just via two different fields an author sets to
  the same item ID(s).
- **Re-checking at click time**: `requiresItems` is only evaluated once, when the box is shown -
  between then and the player actually clicking a button (an ordinary reaction-time gap, or a much
  longer one if the button's `steps` include a `delay`/`await_repair` that suspends the sequence), the
  initiator could drop, trade away, or otherwise lose the item(s) that made the button eligible in the
  first place. A `take_item` step therefore re-verifies `Inventory::hasItem()` for every one of its
  `items` entries before consuming any of them (see "take_item" above) - on a shortfall it shows the
  player a game notification, `"I don't have enough {item}."` (the same `{item}`-name substitution as
  everywhere else), naming whichever entry came up short, and stops the rest of that button's `steps`
  without partially consuming the others.
- `excludePlayerFaction` (bool) - hides the button if the *patient* (not the initiator) belongs to the
  player's faction, checked via `RootObjectBase::getFaction()`/`Faction::isThePlayer()`. Used on "Reset"
  so it's only offered for a wild/unaffiliated robot, not a recruited squad member.
- `requiresDeactivated`/`excludeDeactivated` (bool, counterparts) - gate on `g_deactivated` membership.
  "Reset" requires `excludeDeactivated` - `join_squad`'s `recruit(editor=true)` crashed
  (`STATUS_ACCESS_VIOLATION` reading `-1`, confirmed live via minidump) when run on a still-fatally-wounded
  character; "Revive" no longer also requires `requiresPlayerFaction` (removed - it otherwise never showed
  for a wild robot at all, the exact case that needs reactivating before Reset can safely run).
- `requiresRace`/`excludeRace` (string or array of strings, counterparts) - matches
  `Character::getRace()->data->stringID`, the FCS String ID, not the display `name` - `name` comparison
  was tried first and dropped: this race's real `name` carries a trailing space invisible in-game, which
  silently broke an untrimmed exact match, and `name` is translatable/re-orderable in a way a `stringID`
  isn't. Accepts either a single String ID or a JSON array of them (any one match is enough), same
  string-or-array shorthand as a trigger's `buildings`. Used to give "Skeleton No-Head MkII"
  (`95870-Newwworld.mod`) its own "Diagnostics" button variant (requiring a Power Core, same item as the
  animal path) alongside `system_menu`'s generic humanoid one (which excludes this race so the two don't
  both show at once).
- `requiresCharacter`/`excludeCharacter` (string or array of strings, counterparts) - same shape and
  "any one match" semantics as `requiresRace`/`excludeRace`, but matches the patient's own FCS String ID
  (`Character::getGameData()->stringID`, the same accessor `isAnimalCharacterType()` already uses) rather
  than its race - for gating on specific unique characters instead of every character of a race.
  `system_menu_animal` uses this to split two specific unique robot animals (`56111-Newwworld.mod`,
  `96147-__Southern hive.mod`) into their own "Diagnostics" variant requiring 3 AI Cores instead of the
  generic animal path's 1.
- The box-level `message`'s `{item}`/`{skillCharacter}`/`{skillCharacter2}` substitutions are resolved
  from `def.buttons` filtered by `isDialogueButtonForPatient()` (the patient-only subset of gating -
  race/animal/faction/deactivated), not from `eligibleButtons` (the full, initiator-inclusive gate).
  Originally scoped to `eligibleButtons`: this under-resolved live - a patient with a qualifying squad
  but no Power Core yet had its "Diagnostics" button hidden for the item alone, so the message fell
  through to `{item}`'s box-level default and (once `{skillCharacter}`/`{skillCharacter2}` existed)
  had no button left to resolve those from at all, showing "No one" despite a qualifying squad member.
  Message text should describe what a button *would* need/who *would* qualify for this patient
  regardless of what the initiator currently has in hand.
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

`OnMessageBoxButtonClicked(int buttonId)` is the single shared click handler for every button on every
dialogue box, registered as the `MyGUI::delegates::IDelegate1<int>*` callback `createMessageBox()`
invokes - `buttonId` is whichever int `showDialogueBox()` paired each button's caption with (its index
into `g_currentDialogueButtons`), no widget lookup needed since `MessageBoxManager` already resolved the
click itself and has destroyed the box's native `Window` by the time this fires. Maps that index into the
button list captured when the box was shown, closes the box unconditionally, then hands the button's
`steps` to `dispatchDialogueSteps()`.

Only one dialogue box shows at a time (`g_pendingDialoguePatient` gates `showDialogueBox()`) - a second
trigger while one is already open is silently dropped rather than stacking a second box.

`getOwnModDirectory()` (Win32 `GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` +
`GetModuleFileNameA`) is how the mod locates its own folder to load `RE_Kenshi.json` from - a mod DLL
has no working directory of its own. Reused verbatim from the old (removed) `ConversationOverrides`
system, which needed the same thing to find the same file.

### 5. JSON-driven dialogue triggers

"When does a dialogue box open" is data-driven the same way button behavior is (§4): `RE_Kenshi.json`'s
`"DialogueTriggers"` array, each entry an AND of `requiredStates` (named `bool(Character*)` checks
registered in `g_triggerStateChecks` - `"deactivated"`/`"animal"`/`"humanoid"`/`"inBed"`/
`"beingRepaired"`/`"repairerScience1"`, all must pass) and `buildings` (the used building's own FCS String
ID must be in this list; **empty means no building requirement at all**, not "never fires" - see below),
opening `menu` once both hold. This lets more than one trigger definition exist - e.g. `system_menu` for
humanoid robots vs `system_menu_animal` for animal-type ones, and a pair of `requiresQualifiedFor`/
`excludeQualifiedFor` triggers per case (§4) both able to open a menu for the same `requiredStates` -
rather than the single hardcoded trigger `Character::updateOnScreenCheck()` originally polled (§3).
Deliberately flat named checks AND'd together, not a general boolean expression language, since every
combination needed so far (e.g. "deactivated AND animal AND repairerScience1") fits that shape without
the parsing/fragility risk a real expression syntax would add.

`"inBed"` (`isInBedState`) was split out of `evaluateDialogueTrigger()` into a named `requiredStates`
check rather than staying a hardcoded `self->inSomething != IN_BED` early-out, back when a Skeleton
Repair Bed furniture trigger existed alongside the item-based one - unused by any shipped trigger now
(§3), but left registered in `g_triggerStateChecks` since it costs nothing to keep and a JSON author
could still reach for it.

`"beingRepaired"`/`"repairerScience1"` don't fit a `self->inWhat` building lookup at all - a robot being
repair-kitted isn't necessarily standing in any building - so `trigger.buildings` being empty had to
become a real, intentional "no requirement" case (`buildingMatches = trigger.buildings.empty()`) rather
than the JSON loader's original assumption that an empty list was an authoring mistake ("will never
fire"). The initiator passed to `showDialogueBox()` is resolved from `g_activeFirstAid`'s caregiver for
`self`, falling back to the player's current selection (§4) only if there isn't one.

`Character_updateOnScreenCheck_hook` evaluates every trigger for every on-screen character every frame,
so `requiredStates` are checked before `buildings` and short-circuit on the first failing check - the
same cheap-gate-first discipline used throughout this file - since resolving/querying the standing
building costs more than any individual state check.

Each trigger's "already shown, waiting on the player" flag (`g_triggerShown`) is keyed by `(Character*,
trigger index)`, not just `Character*`, so more than one trigger can be pending independently for the
same character. It's deliberately level-triggered: dismissing a box (e.g. clicking "No") does not clear
the flag, since the trigger's conditions still hold - clearing it would reopen the box the very next
frame, making "No" appear to do nothing. Only a real state change (leaving the building, or an action
like reactivation clearing `g_deactivated`) clears it, via the same requiredStates/buildings check
re-run every frame regardless of the flag.

**`requiresQualifiedFor`/`excludeQualifiedFor`** (string, counterparts, a named `DialogueBoxes` key) gate
a trigger on whether the initiator *personally* qualifies for a skill-gated button in that box -
`initiatorQualifiesForMenu()` walks the named box's buttons, filters to the ones
`isDialogueButtonForPatient()` says apply to this patient, and returns true if the initiator meets
`requiresSkill` or `requiresSkill2` (OR, via `initiatorHasEitherSkill()`) on any of them; true
unconditionally if none of them are skill-gated at all. No skill names or thresholds are hardcoded here -
it reads whatever's already on the target box's own buttons, the same values `squadHasSkill()` uses for
the squad-wide check. This is what stops an unrelated squad member from opening "Diagnostics" just
because *someone else* in the squad qualifies and the item happens to be in their pocket: passing the
squad-wide check only means specialists exist somewhere, not that the person standing here is one of
them. `system_menu`/`system_menu_animal` each get a paired dead-end trigger, listed *first* in
`DialogueTriggers` and sharing the same `requiredStates`/`buildings`, with `excludeQualifiedFor` pointing
at the real menu and `menu` pointing at `no_specialist_menu` instead; the real trigger immediately after
adds the matching `requiresQualifiedFor`. Since the two conditions are exact complements for a given
initiator, at most one of the pair ever fires, and because triggers are evaluated in array order with
`showDialogueBox()` a no-op once one is already pending, whichever is listed first effectively wins -
"first eligible trigger" gives if/else routing to a genuinely distinct initial box, entirely through
existing trigger evaluation, with no bespoke redirect concept.

`no_specialist_menu` has no `requiresItems`/`requiresSkill` buttons of its own (just "Exit"), so without
help its message could never say *what* is missing - only the message's own box's buttons feed
`{item}`/`{itemStatus}`/`{skillCharacter}`/`{skillStatus}` (see above), and this box's buttons are empty
of that data by design (it's the dead end, not the real menu). `evaluateDialogueTrigger()` closes this
gap for free: the dead-end trigger's own `excludeQualifiedFor` already names the real menu it stands in
for (e.g. `"system_menu_animal"`), so it's passed straight through as `showDialogueBox()`'s
`requirementsMenuId` argument. Inside, `reqDef` points at that real menu's `DialogueBoxDef` instead of
`no_specialist_menu`'s own for the item/skill-resolution loop only - `eligibleButtons`, `title`, and the
literal `message` template still come from `no_specialist_menu` itself, so it still only ever offers
"Exit". This is JSON-driven with zero new fields: the same `excludeQualifiedFor` value already had to name
the real menu for the trigger gate to work at all, so the message-fallback wiring is entirely a
consequence of data already in `RE_Kenshi.json`, not a hardcoded C++ special case.

Rejected first: the same OR-check as a *button*-level `requiresInitiatorSpecialist`/
`excludeInitiatorSpecialist` pair, with a mutually-exclusive dead-end sibling button per "Diagnostics"
variant whose only step was `open_menu` to the dead-end box. It worked but only after an extra click
through an intentionally-identical "Diagnostics" caption - live testing showed a player has no way to
tell, before clicking, that this particular menu already knows they don't qualify, so it read as "nothing
happened" rather than a warning. Moving the same check to trigger time, so the dead-end is the box that
opens initially, fixed that without discarding the JSON-driven check itself - only where it's evaluated
changed.

### 6. Cleanup - not implemented

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

### 7. Save/load persistence

`g_deactivated` (and `g_reactivateDialogueShown`) stay `Character*`-keyed and session-only for the hot
per-tick lookups - `updateOnScreenCheck` fires roughly every frame per character, and switching every
lookup site to a handle-string key (a string construction per check) isn't worth it for maps already
gated to near-zero cost for the overwhelming majority of characters.

**No custom persistence file exists** (an earlier design's write-through JSON side-file, keyed by
`RootObjectBase::getHandle().toString()`, was removed - see "A second pitfall" above for why: `hand`
round-trip resolution failed completely, `resolved=0 failed=8` on a real save/reload, and separately
`hand` values for this class of character aren't even stable within one session). Instead,
`hasDeactivatedSignature()` re-derives `g_deactivated` membership every time from data Kenshi's own save
system already persists reliably: `MedicalSystem::dead` and `HealthPartStatus::flesh`/`isDead()`, the
same fields any character's health depends on. `Character_declareDead_hook`'s block leaves a specific,
otherwise near-impossible combination on a Deactivated robot - at least one body part at fatal flesh while
`dead` stays false (in real vanilla play `dead` becomes true essentially the instant damage is fatal, so
that combination doesn't normally persist; it only does here because this mod's own hook forces `dead`
back to false). Checked from `Character_updateOnScreenCheck_hook` (already firing every frame per
character) for any not-yet-tracked robot, gated behind a cheap `g_deactivated.count()` check first so the
anatomy scan only runs until a character is found and recorded. No load-event hook, no serialised ID,
nothing that can go stale.

## Robot age-tier relabeling ("Calibration")

Vanilla shows an "Age:" row (Pup/Teen/Adult/Elder) in the Attributes panel for animal-type characters, in
place of "Defense:" (native behavior, not this mod's). Doesn't fit robots narratively.
`DatapanelGUI_setLineStatInfo_hook` relabels it to "Calibration:" with Learning/Operational/Adaptive/Elite
tiers for `isRobotRace` characters specifically, leaving real animals untouched.

Boundaries confirmed live via a temporary sweep (`setAge()` across candidate values, reading back
`getAge0to1()`/`getAgeString()`): Pup ends at `0.39` (Teen starts `0.40`), Teen ends at `0.59` (Adult
starts `0.60`), Adult ends at `1.10` (Elder starts `1.11`) - the last gap isn't evenly spaced like the
other two and wasn't guessable from the label text alone.

## Robot limb race-lock

Unrelated to the Deactivated/reactivation system above - merged in from the former standalone "The
Limbless (Type 2)" mod, kept as its own self-contained section (own map, own two hooks) since the two
features share no state. Fixes vanilla robot limbs being equippable onto any race regardless of their
FCS "Race Limiter" (racesInclude/racesExclude).

Vanilla stores each item's Race Limiter in `RaceLimiter`'s singleton, populated lazily via `addLimit()`
the first time something looks the item up. Nothing on the robot-limb equip path ever calls that, so the
cache stays empty and `canEquip()` always sees "unrestricted" for limbs - `RaceLimiter_canEquip2_hook`
primes the cache for the item on every call before deferring to vanilla's own check.

Separately, `canEquip()`'s `who` parameter is `RobotLimbs::inventory` - a cached "interface" `RootObject*`
representing a character's limb-slot section in the GUI - whose `getName()` is a generic `"ROBOTICS"`
label, not tied to the owning character, so vanilla's race check can't resolve the real recipient from
it. `RobotLimbs_getInventoryInterface_hook` records interface-pointer -> owning-character
(`g_limbInterfaceOwners`) every time one is (re)built; `RaceLimiter_canEquip2_hook` substitutes the
item's true owning `Character*` for `who` before calling through, so vanilla's own validation then
enforces the race restriction correctly, including in multi-character trades.

`canEquip` is virtual, so it's hooked via its `_NV_` non-virtual export (`RaceLimiter::_NV_canEquip`) -
see the virtual-function hooking pitfall at the top of this document.

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
- `MedicalSystem::applyFirstAid(float skill, Item* equipment, float frameTIME, Character* who)` —
  MedicalSystem.h:250, RVA `0x64D080`, non-virtual (the class has no vtable of its own, only a virtual
  dtor) - `who` confirmed live to be the caregiver, not the patient; `skill` is some per-tick computed
  value, not a stable stat (see §3) - reached the patient the same way `medicalUpdate()`'s hook does,
  via `getPart(PART_HEAD)->me`
- `CharStats::science` — CharStats.h:121, plain `float` member, offset `0xE0` - already reusable via
  `g_skillFields["science"]` (used elsewhere for dialogue button `requiresSkill` checks), but the item
  trigger's `"repairerScience1"` reads it directly off the caregiver instead, since `requiredStates`
  checks only take the patient (`self`), not an arbitrary other character
- `DatapanelGUI::setLine(key, s1, s2, category, last, keyVisible)` — DatapanelGUI.h:70, RVA `0x6FD4B0`,
  non-virtual — status-tag override hook, the only `setLine*` overload actually used; the other ~6
  (`setLineStatInfo`, `setLineFaction`, three more `setLine(...)` signature variants, `setLineText`) were
  all hooked diagnostically, ruled out, and removed - see §2
- `DatapanelGUI::getObject() const` — DatapanelGUI.h:100-101, virtual — resolves which character a panel
  belongs to (called normally at runtime, not hooked - see the virtual-function pitfall note)
- `MainBarGUI::medicalPanel` (`MedicalDatapanel*`) / `getMedicalPanel()` — gui/MainBarGUI.h:91/131 —
  ruled out as the source of the persistent health-status text via live pointer comparison.
  `MedicalDatapanel` itself is forward-declared only, no full definition anywhere in RE_Kenshi's headers -
  the real widget was found by name-suffix tree walk instead, see the next entry
- `findHealthTextWidget()` / `updateHealthTextOverride()` — this file, not RE_Kenshi/KenshiLib - locates
  `MainBarGUI`'s `*_HealthText` `MyGUI::TextBox` by walking its widget tree once (`MyGUI::Widget::
  castType<T>(false)` is RTTI-checked, safe against a type mismatch) and matching the stable name suffix
  (the full name has a per-session numeric prefix); cached thereafter since `MainBarGUI` is a persistent
  singleton. See §2 and "A second pitfall" for why this needed `g_lastInspectedCharacter` rather than
  `PlayerInterface::selectedCharacter`
- `hand::getCharacter() const` / `getRootObjectBase() const` / `operator==(const hand&) const` /
  `fromString(const std::string&)` / `toString() const` — util/hand.h - see "A second pitfall" for why
  none of these reliably identify an `ANIMAL_CHARACTER`
- `MessageBoxManager::createMessageBox(title, message, buttons, modal, callback)` —
  gui/MessageBoxManager.h:30 — real mangled name too long for MASM, exported as
  `MessageBoxManager_createMessageBox_PLACEHOLDER` instead (see MessageBoxManager.inc); redeclared
  `extern "C"` under that literal name since the header's own declaration compiles but doesn't link
- `MyGUI::newDelegate(void(*)(Args...))` — mygui/MyGUI_DelegateImplement.h (via mygui/MyGUI_Delegate.h -
  `MyGUI_DelegateImplement.h` is a macro-driven template generator, not includable directly) — used here
  for the `MyGUI::delegates::IDelegate1<int>*` callback `createMessageBox()` invokes with the clicked
  button's id
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
  visibility-gate/re-check (per-entry `count`) and actually-consume-it (called `count` times per entry,
  since there's no batch-count overload), respectively, for `requiresItems`/`take_item`'s `items` (§4)
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
- `HandleManager::destroy(const hand&, const char* reason)` — HandleManager.h:208, RVA `0x2AC790`,
  non-virtual — the native "corpse unloaded" cleanup mechanism (see §6); not hooked in the shipped
  version - `dead` never becomes true, so this never fires for a Deactivated robot regardless
- `hasDeactivatedSignature()` — this file, not RE_Kenshi/KenshiLib - re-derives `g_deactivated`
  membership from native `MedicalSystem::dead`/`HealthPartStatus::isDead()` instead of any persisted ID
  (see §7 and "A second pitfall")
- `PlayerInterface::selectedCharacter` (`hand`) — PlayerInterface.h:191 - safe for identifying a humanoid
  *initiator* (§4). Not reliable when the object being checked could be an `ANIMAL_CHARACTER` - superseded
  by `g_lastInspectedCharacter` (this file) for that case - see "A second pitfall"
- `PlayerInterface::getAnyPlayerCharacter() const` — PlayerInterface.h:177, RVA `0x7F19B0` - returns an
  arbitrary squad member; not used for dialogue `initiator` (§4) since that must be predictable
- `RootObjectBase::getHandle()` — RootObjectBase.h:63/78
- `RootObjectBase::getGameData() const` — RootObjectBase.h:36, virtual, `Character` inherits this via
  `RootObject` - `isAnimalCharacterType()` reads `->type == ANIMAL_CHARACTER` off the result
- `GameData::type` (`itemType`) — GameData.h:91 - same category enum already used for `ITEM`/
  `DIALOGUE_LINE` elsewhere in this file; `HUMAN_CHARACTER`/`ANIMAL_CHARACTER` are FCS's own Character-template
  categories, unrelated to `Character::isAnimal()` (see below) and unrelated to `RaceData`/`RACE_GROUP`
- `Character::isAnimal()` / `_NV_isAnimal()` — Character.h:204-205, virtual, RVA `0x5E51D0` (base) -
  reflects the `CharacterAnimal` AI/movement component, **not** FCS's Human/Animal Character category;
  returned `false` live for an Iron Spider authored as `ANIMAL_CHARACTER` in FCS. Not used by this mod;
  documented here so a future reader doesn't reach for it expecting FCS's category
- `CharacterAnimal : public Character` — CharacterAnimal.h:25 - overrides `setAge`/`getAge`/
  `getAge0to1`/`getAgeInverse`/`isAnimal` at its own RVAs, with real backing fields (`age`,
  `lastUpdatedAge`, `ageSizeMin`/`Max`, `lifespanInDays`). `getAgeString()` is *not* overridden, so
  `_NV_getAgeString()` is fine to call directly - unlike the others, see the pitfall note above
- `Character::setAge(float zeroToOne)` / `getAge() const` / `getAge0to1() const` — Character.h:516-524,
  all virtual - **call these normally, not via `_NV_`**, so `CharacterAnimal`'s override actually runs
  (see the pitfall note above). `getAge0to1()` is the reliable unit (confirmed via `getAgeString()`
  live: `0.3` → `"Pup"`, matching Kenshi's own Pup/Teen/Adult/Elder growth-stage milestones)

## Unresolved: generic "unconscious" status tooltip

Hovering a Deactivated robot's portrait (both the main character-info panel and squad-list thumbnails)
shows a fixed native tooltip: `"This guy is unconscious and probably needs medical attention and rest."`
Wanted: override this per-robot with something like `"This unit's {AI/power} core has failed and will
need to be replaced."`, the same way §2's "State:" row already is. Not done - the real source function
couldn't be identified with the tools available (no disassembler in this environment), only ruled out
via live hooking:

- Confirmed via a temporary hook that the text is delivered through
  `ToolTip::setup(MyGUI::Widget* widget, const std::string& text)` (ToolTip.h, RVA `0x91F830`,
  non-virtual) - `widget` here is the tooltip's own fixed display widget, not the hovered portrait, so it
  carries no path back to which `Character` the tooltip belongs to.
- That call's immediate caller has return-address RVA **`0x920c0f`** (relative to the exe's own base,
  captured via `_ReturnAddress()`). This does **not** correspond to any function documented anywhere in
  RE_Kenshi's current headers - it sits in a numeric gap with no exact match.
- Initial theory: `0x920c0f` falls inside `ToolTip::setup(MyGUI::Widget*, const lektor<StringPair>&
  lines)` (RVA `0x920AB0`, the next-closest preceding documented RVA). **Disproven by direct testing** -
  hooking that overload showed it fires constantly for a completely different purpose (the Attributes
  panel's per-stat breakdown tooltips - Strength/Dexterity/Athletics/etc.), always from one single caller
  RVA `0x9216d7`, which matches `ToolTip::showGameData(const hand&)` (RVA `0x9216B0`, non-virtual) almost
  exactly. Never once fired for "unconscious" text. So RVA-proximity-to-a-header-entry is not a reliable
  way to identify an unknown caller here - confirmed, not just suspected.
- Next step, if picked back up: needs an actual disassembler against `kenshi_x64.exe` pointed at RVA
  `0x920c0f` to identify the real function and check whether it's virtual before attempting any hook -
  not something to guess at again from header proximity alone.

## A note on the RE_Kenshi SDK itself

`Platoon.h` and `Building/Building.h` independently redeclared `enum BuildingDesignation`
byte-for-byte, with neither deferring to the shared `Enums.h` both already include - including both
headers in the same translation unit was a hard compile error (`C2011: redefinition`). Fixed upstream
(not part of this mod) by moving the enum into `Enums.h` and deleting both duplicates, in both
`/home/bryan/Git/RE_Kenshi` (the tracked SDK repo) and `/home/bryan/Git/KenshiLib_Examples_deps` (the
copy actually compiled against, which was silently out of sync with the tracked repo before this).
