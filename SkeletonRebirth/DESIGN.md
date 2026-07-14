# SkeletonRebirth — design plan

Goal: Robot-race characters (all races with `RaceData::robot == true` — Skeletons, Iron Spiders,
Soldierbot Guards, Security Spiders, etc., not just Skeletons specifically) never enter vanilla
Dead state directly. Instead they enter a custom **Deactivated** state (permanently incapacitated,
revivable via a high-effort/high-tier repair action). If a Deactivated robot sits in a zone the
player isn't near long enough, it converts to real Dead state so it stops littering the world.

This is a plan only — nothing below is built yet. File:line references are to
`/home/bryan/Git/RE_Kenshi/KenshiLib/Include/kenshi/`.

## Status (read this first)

**RESOLVED — `ConversationOverrides` now works reliably for any FCS dialogue reply, including
stock/vanilla content, not just this mod's own.** This supersedes every entry below about crashes,
`replyClicked` being unsafe to hook, `currentLine` polling, MyGUI events, `triggerNextLine`, or
`activateResponse` - all of that was a long detour chasing the wrong cause. The actual bug, found by
reverting to a known-good prior version of this file and diffing against it directly (rather than
continuing to re-derive an explanation from first principles): `handleDialogueReplyClicked`'s "nothing
configured for this reply" path was never a true no-op - it called `patient->_NV_getName()` and built a
log string for *every single dialogue reply click in the entire game*, every NPC, constantly. Separately,
during testing the crashing stock ID had been explicitly added to `RE_Kenshi.json`, so the plugin was
also doing real dispatch work against that content. Hooking `Dialogue::replyClicked` (both overloads,
including the int overload's unconditional `self->replyIds[index]` read) was never actually unsafe - a
prior, simpler version of this plugin hooked exactly the same functions the same way and ran completely
stably. Fixed by making `g_conversationOverrides.count(replyId)` the literal first thing
`handleDialogueReplyClicked` checks - a plain map lookup, no native calls, no string construction - a
true, instant, zero-cost return for any reply this plugin doesn't care about. Confirmed via a targeted
side-by-side test (a hardcoded stock ID handled with a true early-return, live-tested crash-free across
repeated clicks) before rebuilding the JSON dispatch path on top of that same proven-safe shape.
**Lesson for next time**: when a prior working version exists, diff against it before re-deriving the
answer from static analysis/first principles - a live crash-dump/disassembly/`.pdata` investigation
(documented in full below) produced a lot of true, real findings that were nonetheless not the actual
cause, and cost far more time than the eventual fix.

**SUPERSEDED — CONFIRMED ACTUAL ROOT CAUSE, FIXED — `DialogLineData::getName()`/`getText()` are not safe to call on
arbitrary (stock/vanilla) content; the earlier `insertWordSwaps()` theory below was disproven.** After
removing `insertWordSwaps()` entirely (see the superseded entry directly below), the user retested and
the crash recurred with the **identical** fault address — proof `insertWordSwaps()` was never the (sole)
cause. Root-caused properly this time via the actual crash dump: RE_Kenshi's own `MiniDumpWriteDump`
handler wrote a `.dmp`, hand-parsed with a Python script (no `minidump-stackwalk`/`cdb` available) to
pull `MINIDUMP_EXCEPTION_STREAM`. Two independent crashes both faulted at the exact same address
(`0xc0000005`, read access violation from a garbage pointer), which fell in none of our own DLLs'
PE ranges. Captured the *live* running Kenshi process's `/proc/<pid>/maps` and found that address
actually lands inside `msvcr100.dll` (the VS2010 CRT this project itself builds against, loaded into
the Proton prefix at `C:\windows\system32\msvcr100.dll`) — RVA `0x3c131`, which sits between the
exported `memcpy`/`memmove` (`0x3bf60`) and `memset` (`0x3c2b0`) symbols (`objdump -p` on the DLL's
export table). **The fault is inside `memcpy`/`memmove` itself, reading from a corrupted source
pointer** — consistent with a `std::string` returned *by value* across the native ABI (the
hidden-pointer-return calling-convention hazard already flagged elsewhere in this file) having a bad
internal buffer for this specific content, which then faults later, whenever something copies it - not
at the point of the original native call. The actual culprit was `describeDialogLine()`, this file's
own diagnostic helper, which called `line->getName()`/`line->getText(false)` (both by-value returns)
directly on a `DialogLineData*` resolved from unverified, arbitrary FCS IDs and concatenated the
results into a log line — exercised on **every** `replyClicked` for any reply with `ConversationOverrides`
configured, including stock dialogue this plugin never touched before. This also explains the "zero log
output before the crash" symptom seen in every earlier attempt: the crash happened *while constructing*
the `DebugLog()` argument, before `DebugLog()` itself was ever called. SEH around the earlier
`getFcsDialogLine()` lookup call couldn't catch this because the fault wasn't at that call — it surfaced
later, inside `describeDialogLine()`, arbitrarily far from any `__try`. **Fixed by removing DialogLineData
resolution from the hot dispatch/logging path entirely** (`describeDialogLine()` deleted,
`dispatchConversationOverrides()` and `handleDialogueReplyClicked()` no longer call `getFcsDialogLine()`)
— turned out to be safe to remove outright since, after `show_text` moved off native `insertWordSwaps()`
(see below), **none of the three override handlers actually read the resolved line anymore**. Dispatch
is now driven purely by the FCS String ID (plain `std::string` compare), never touching the underlying
native game-data object for content this plugin didn't author. `getFcsDialogLine()` itself is kept (still
used once, for `SR_REACTIVATE_DIALOGUE_ID` — this mod's own trusted content, exercised the same way by
every install of this mod). Compiles clean, deployed. **Not yet live-tested** — next step is the user
reattaching a `show_text` override to the stock `55804-Dialogue.mod` reply (already present in the
deployed `RE_Kenshi.json`) and confirming it no longer crashes.

**SUPERSEDED — CONFIRMED ROOT CAUSE, FIXED - `Dialogue::insertWordSwaps()` is not safe to call outside the native
engine's own controlled conversation flow.** Live-tested and reported directly, the decisive symptom:
once any `ConversationOverride` touched one stock dialogue line for a character, **all** stock
dialogue for that same character started crashing, not just the configured line - "like it's
corrupting an internal reference." `Character::dialogue` is a per-character singleton, reused for
every conversation that character ever has. `insertWordSwaps()` is native code that almost certainly
mutates internal `Dialogue` state as a side effect (plausible candidates already documented in this
file's own header: `currentLine`, `locked`, `_hasChanceLines`). `show_text` called it manually,
outside the live conversation the engine itself manages, on a `DialogLineData` that isn't the
character's actual current line - most likely leaving that per-character `Dialogue` object in an
inconsistent state rather than crashing immediately, which is exactly why it only broke *future*
native conversations with that same character rather than the moment of the call itself. The earlier
SEH guard around this call (see below) could only ever have caught an immediate hard fault, not
corrupted-but-still-"valid" state left behind afterward - explains why that fix compiled fine and
still didn't resolve the crash. **Fixed by removing the native call entirely**, not by trying to make
it safer: `show_text` now resolves `/MYNAME/` via a small, self-contained, pure-C++ string
substitution (`applySimpleWordSwaps()`) that touches nothing on the native `Dialogue` object at all.
Loses the full native wordswap tag vocabulary (this mod's own content never used anything beyond
`/MYNAME/` anyway) in exchange for no longer being able to corrupt a character's dialogue state. Kept
regardless of the disproof below (still strictly safer, and this mod's own content never needed more
than `/MYNAME/`), but **live-tested and confirmed NOT to have been the crash's root cause** — the crash
recurred with an identical fault address after this fix. See the actual root cause at the top of this
Status section.

**CRASH INVESTIGATION - key reframe: it was breaking *all* original dialogue, not just the
configured stock ID.** User-reported and decisive: attaching one override to one stock dialogue ID
broke vanilla dialogue broadly, not just that specific entry - which rules out "something wrong
resolving this one ID" as the root cause and points at the hooks touching far more shared state than
they need to for dialogue this plugin has no reason to care about.

Analyzed the actual crash dump (`crashDump1.0.65_x64.zip`, written by RE_Kenshi's own
`MiniDumpWriteDump` crash handler - `Bugs.cpp`) directly, no debugger needed: parsed the minidump's
stream directory and exception record with a small Python script (module list stream was
unfortunately empty in this dump, but the exception stream wasn't). Confirmed: genuine access
violation (`0xC0000005`), reading from a garbage-looking address (`0x10ef7415e`), at an instruction
address that falls in **none** of kenshi_x64.exe/KenshiLib.dll/RE_Kenshi.dll/
SkeletonRebirthDiagnostics.dll's own PE header address ranges (checked each directly) - consistent
with the fault surfacing deep inside a Wine/system DLL (ntdll/ucrtbase-style address range), the
classic signature of heap corruption whose *cause* happened earlier and whose *symptom* (the actual
crash) only surfaces later, elsewhere, when the corrupted memory finally gets touched - explaining
why SEH guards around the specific lookup calls didn't help: the fault wasn't happening at the
point being guarded.

**Confirmed root cause candidate, fixed: `g_pendingReplyId[patient] = replyId` ran unconditionally
for every trustworthy `replyClicked`, regardless of whether anything was configured for that
reply.** This meant `Dialogue::update()` (hooked globally, fires every frame per character) was
doing real work - map lookups, a native `conversationHasEnded()` call, potential dispatch - for
*every* character with any pending vanilla/other-mod dialogue reply, not just this mod's own. Given
Kenshi has NPCs constantly having ambient background conversations with each other, this could mean
our hooks were touching shared mutable state (`g_pendingReplyId`/`g_pendingInitiator`) far more
often, for far more characters, than intended - a much larger surface area for something to go wrong
than "one specific ID," matching the "breaks all dialogue" report. Fixed by returning early - before
touching `g_pendingReplyId` at all - for any reply nobody configured overrides for, mirroring the
narrowing already applied to the diagnostic FCS-line lookup. **Not yet live-tested against either
symptom** (the original 55811 crash, or the "breaks all dialogue" regression) - this is a strong,
well-evidenced candidate fix, not a confirmed resolution.

**CRASH, PARTIALLY MITIGATED - `ConversationOverrides` can be attached to any dialogue reply in the
whole game (not just this mod's, confirmed - a stock/vanilla dialogue entry, `55811-Dialogue.mod`,
was configured and crashed the game).** `Dialogue::replyClicked` is hooked globally (there's no
per-mod scoping on the native function itself), which is exactly what makes attaching overrides to
*any* conversation ID possible - but it also means the plugin was, until this fix, running a native
FCS lookup (`getFcsDialogLine()` -> `GameDataContainer::getData()` -> `DialogDataManager::getData()`)
on *every single dialogue reply click in the entire game*, purely to build a diagnostic log line,
regardless of whether that reply had anything to do with this mod. Live-tested: the crash happened
with **zero** SkeletonRebirth log output beforehand, and `getFcsDialogLine()` ran *before* the
`DebugLog()` call in `handleDialogueReplyClicked()` - consistent with the crash happening inside
that lookup itself, before there was anything to log. Two fixes applied:
1. The diagnostic lookup now only runs when the clicked reply actually has `ConversationOverrides`
   configured for it (`g_conversationOverrides.count(replyId)`) - the vast majority of dialogue in
   the game (everything nobody configured) no longer touches this lookup at all. This protects all
   *other* dialogue in the game from a risk this plugin was introducing for no reason.
2. `applyShowTextOverride()` no longer assumes `dialogueLine` is non-null before passing it to
   `insertWordSwaps()` - falls back to unswapped text instead of risking a null-pointer call into
   native code that likely doesn't check its own parameter.
**Confirmed the caveat was real - crash recurred even with fix (1), then fixed with SEH.**
`55811-Dialogue.mod` crashed again on the exact same test, live-confirmed by the user, proving the
native lookup crashes outright (an access violation) rather than gracefully returning null - no
null-check after the call could ever catch this. Fixed by wrapping the two native lookups that take
user/JSON-supplied IDs (`getFcsDialogLine()` and the new `getGameDataGuarded()`, used by
`applyTakeItemOverride()`) in `__try`/`__except (EXCEPTION_EXECUTE_HANDLER)`, converting a hard
crash into a logged failure + graceful `nullptr`.

**MSVC gotcha hit twice getting this to compile - `C2712: Cannot use __try in functions that require
object unwinding`.** This project builds with `/EHsc`, and MSVC rejects `__try`/`__except` in *any*
function that also needs C++ object unwinding *anywhere* in its body - not just inside the `__try`
block. First attempt put `ErrorLog(std::string(...))` inside the `__except` handler - rejected, even
though the handler is a separate block from the `__try`. Second attempt removed that but still took
`const char*` parameters, passed straight to `ou->gamedata.getData(stringID, ...)` (which takes
`const std::string&`) - the *implicit temporary* `std::string` created by that conversion, entirely
within the `__try` block, was *also* enough to trigger C2712. Fixed by changing both guarded
functions to take `const std::string&` directly (a reference needs no construction/destruction
inside the callee) and updating call sites to pass their already-existing `std::string`s directly
instead of round-tripping through `.c_str()`. End state: these two functions contain **zero** C++
objects requiring destruction anywhere in their bodies - only raw pointers and references. Callers
are responsible for any logging (the guarded functions themselves stay silent on failure) -
`dispatchConversationOverrides()`'s existing "doesn't resolve to a real FCS DIALOGUE_LINE" error and
the generic "conversation override... failed" error already cover a recovered-`nullptr` the same as
a legitimately-missing ID, so no diagnostic information was lost.
**Not yet re-tested live** - needs confirming the SEH guard actually catches this specific crash
now, not just that it compiles.

**CONFIRMED BUG, FIXED — `Dialogue::replyClicked` can report BOTH sides of a mutually-exclusive
Yes/No choice for a single click.** Live-tested and confirmed directly by the user: clicking only
"No" still logged "Yes" firing - and reactivating the character - about 1.5 seconds *before* "No"
itself fired. The improved diagnostic logging below (which resolves each reply ID to its actual FCS
`name`/`text`/`parent` fields) is what made this legible enough to catch: the log line for the
spurious "Yes" showed `text="Yes"` plainly, ruling out an ID-mismatch/mislabeling theory and
confirming the *wrong side of a binary choice* was genuinely reported as clicked. Root cause not
confirmed (candidates: the dialogue system pre-evaluating/scoring candidate lines before the real
click, a hover-triggered call, something else) - not knowable without a disassembler, and the fix
doesn't depend on knowing why. Fixed by no longer dispatching straight from `replyClicked`: it now
only buffers the most recently reported reply per-patient
(`g_pendingReplyId`/`g_pendingInitiator`), committed once the conversation is confirmed to have
genuinely ended. Trusts the final answer, not the first signal seen.

**First attempt at the "conversation genuinely ended" signal was wrong - `Dialogue::_endPlayerConversation`
never fires in this flow.** Hooked it first (`Dialogue.h:389`, RVA `0x674750`) since it's the direct
counterpart to `startPlayerConversation`; installed without error, but live-tested and confirmed it
never logs even once, including after a real Yes click that should have ended the conversation - not
knowable without a disassembler why (candidates: only reachable via a different closing path,
internal-only helper not actually invoked at the point we assumed). Replaced with edge-detecting
`Dialogue::conversationHasEnded()` (a query function already trusted elsewhere in this file, e.g.
`showReactivateDialogue()`'s guard) inside a `Dialogue::update(float)` hook instead - a real signal
we already know reflects true conversation state correctly. `update()` is a per-frame,
per-character function (same "hooking something this hot is risky" category as the deliberately-
avoided `Character::update()`), so the hook only does real work when `g_pendingReplyId` actually has
an entry for that patient (a cheap map lookup gates everything else - in practice zero characters
almost all the time, one at most while our own dialogue is active).

**CONFIRMED WORKING END TO END, both critical cases live-tested by the user separately:**
1. First run: "Yes" fired twice (both buffered, same value, harmless), conversation ended, exactly
   one commit, exactly one `TryReactivate()` - log showed
   `conversation ended for Sadneil - committing last-seen reply "12-Skeleton Rebirth.mod"` immediately
   followed by `TryReactivate() SUCCEEDED`, 9ms apart.
2. Second run, the actual regression test for the original bug: clicked only "No" - **nothing
   fired**, no reactivation, confirmed correct (this is the exact scenario that previously
   reactivated on the wrong answer). Went through the dialogue again afterward and chose "Yes" -
   worked correctly.
This closes out the "wrong side of Yes/No" bug - the buffer-and-commit-on-conversation-end design is
confirmed sound, not just theoretically reasoned through.

**NOT YET LIVE-TESTED — reactivation dialogue rearchitected to a JSON-driven "conversation
overrides" system.** Everything below about `SR_REACTIVATE_YES_REPLY_ID`,
`SR_REACTIVATE_ITEM_GATE_ID`, `takeReviveCostItem()`, and the FCS-linked/hardcoded revival message
is **historical** - superseded by this entry, kept for context on how the design got here. Only
`SR_REACTIVATE_DIALOGUE_ID` (which FCS dialogue to *start* on the repair-kit-in-bed trigger) is
still a hardcoded C++ constant, by design - starting the conversation is inherent to this specific
feature. Everything about **what happens when a given reply fires** is now data-driven:

- `RE_Kenshi.json` (the manifest already read by RE_Kenshi's own loader for `"Plugins"`) gets a new
  top-level `"ConversationOverrides"` key: a flat map from any FCS reply/line String ID to a list of
  named overrides. `Dialogue::replyClicked`'s hook (unchanged mechanism) looks up whichever list is
  registered for the clicked reply and dispatches each entry to a registered C++ handler by `"type"`.
  Adding a new FCS-authored choice that reuses an existing override type needs a JSON edit and a
  restart - no recompile.
- Three override types exist: `reactivate_skeleton` (the old `TryReactivate()`, narrowed to just the
  mechanical state change), `take_item` (takes one of an item identified by *its own* FCS ID
  directly - `itemType::ITEM`, not a dialogue-line lookup like the superseded
  `SR_REACTIVATE_ITEM_GATE_ID` approach), and `show_text` (floating `ScreenLabel` text, generic, not
  revival-specific - resolves `/PLACEHOLDER/` word-swap tokens via `Dialogue::insertWordSwaps()`
  against a `DialogLineData` resolved fresh via `getFcsDialogLine(replyId)`, not read from any live
  `Dialogue` state).
- New dependency: **rapidjson**, vendored (cloned, not authored) into
  `/home/bryan/Git/KenshiLib_Examples_deps/rapidjson` - RE_Kenshi itself already depends on it
  (`Plugins.cpp`) but that copy lives in RE_Kenshi's own build, not the separate
  `KenshiLib_Examples_deps` toolchain this plugin builds against. Confirmed compiles and runs
  correctly under this project's VS2010 (`v100`) toolset via an isolated smoke-test *before* touching
  the real refactor (a real, justified concern - this session already hit one C++11-incompatibility
  surprise, range-based `for`, under this exact toolchain). All new code uses explicit iterators and
  `ostringstream`, no range-`for`/`std::to_string` (caught one `std::to_string` slip during review,
  fixed before the first real build attempt).
- Locating the plugin's own directory (to find `RE_Kenshi.json` next to the DLL): no existing
  KenshiLib helper does this - `Debug.cpp`'s `GetModuleName()` deliberately strips to just the base
  filename (confirmed by reading its source). New `getOwnModDirectory()` uses the same underlying
  Win32 call (`GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` +
  `GetModuleFileNameA`), keeping the directory instead of stripping it.
- Current live config (`RE_Kenshi.json`): all three override types attached to
  `"12-Skeleton Rebirth.mod"` (the "Yes" reply) - `reactivate_skeleton`, `take_item` with
  `item: "43392-changes_otto.mod"` (the AI Core), `show_text` with `color: "Green"`,
  `string: "/MYNAME/ has been revived!"`.
- **Compiles clean, deployed, not yet live-tested against the real game.** In particular still
  needs confirming: the full repair-kit → dialogue → Yes → all three overrides fire in order →
  reactivation + item taken + green text shown, exactly as before this refactor; and that editing
  only `RE_Kenshi.json` (no rebuild) actually changes behavior, which is the entire point of this
  change.

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
- **CONFIRMED — revival success notification uses `ForgottenGUI::messageBox(title, message, btn,
  modal, callback)`** (global `gui` pointer, `kenshi/Globals.h`), a real titled dialog window -
  tried a `ScreenLabel` floating-text popup first, but it read identically to routine pickup/snack
  notifications, so switched to a real dialog for something categorically more prominent. The
  `btn` int is Kenshi's own undocumented convention (not stock MyGUI, not in the RE'd headers):
  **`0` crashes the game** (rendered broken placeholder "A"/"B"/"C" buttons - looks like an
  invalid-index fallback, not a bitmask); **`1` is confirmed safe and renders a single dismissible
  "OK" button** - used for this one-button notification only. `MyGUIEngine_x64.lib` needs to be on
  the link line (`MyGUI::Colour`'s constructor lives there, not in `KenshiLib.lib`).
- **SUPERSEDED — reactivation confirmation was a self-built MyGUI panel, not a Kenshi-native
  dialog.** Before reactivating, the player was asked "Attempt to reactivate `<name>`?" via a
  panel built directly from `ForgottenGUI::createPanel()`/`createButton()`/`createLabel()` and real
  MyGUI `Widget::eventMouseButtonClick` delegates - "Yes" triggered `TryReactivate()`, "No" just
  closed the panel. This was deliberately *not* the same mechanism as the notification above,
  because multi-button `messageBox()` proved unusable: extensive live testing found `btn` is an
  undocumented bitmask where every combination *without* bit `1` present rendered stuck,
  unclickable buttons (`2` alone = unclickable "Yes"; `2|4` = unclickable "Yes"+"No") and `0`
  alone crashed the game outright. A separate attempt to trigger an interactive reply through
  `Dialogue::runCustomDialog()`/`sendEvent()` was also abandoned: `runCustomDialog()` just narrates
  every line back-to-back with no pause for player choice (seemingly meant for scripted
  monologues), and `sendEvent()` returned `false` (rejected) for every `EventTriggerEnum` tried
  (`EV_ALMOST_WOKE_UP`, `EV_BEING_HEALED_START`), for reasons that couldn't be pinned down without
  a proper disassembler (plain `objdump` wasn't enough to trace the real branch logic). Building
  the panel from MyGUI's own standard, documented widget/event API sidestepped all of this at the
  time - it's genuine open-source MyGUI code, not a Kenshi-specific undocumented wrapper. Replaced
  below by a proper FCS dialogue hook once a better native entry point was found.
- **NOT YET LIVE-TESTED — replaced the MyGUI panel with a real FCS dialogue via
  `Dialogue::startPlayerConversation(Character* target, DialogLineData* _talk)`
  (`Dialogue.h:390`, RVA `0x683BA0`).** This is a different, previously-untried native entry point
  from the `runCustomDialog()`/`sendEvent()` dead ends above - it's the function vanilla itself
  uses to open an actual interactive player conversation (choices, not just narration). It's
  labelled `private` in the RE'd header, but that label is documentation-only: KenshiLib's headers
  never actually add a C++ `private:`/`protected:` keyword anywhere in `Dialogue.h` (confirmed by
  grepping the file - every member ends up compiled as public), and the linked `KenshiLib.lib`
  exports a real callable thunk for it regardless
  (`Source/kenshi/functions/Dialogue.inc:294`, mangled name `?startPlayerConversation@Dialogue@@QEAA_N...`
  - the `Q` in the mangling itself means the *original* Kenshi source really did declare this
  public, so the RE header's "private" tag looks like a tooling misclassification, not a real
  restriction). So it's directly callable with no hooking/raw-RVA tricks needed:
  `patient->dialogue->startPlayerConversation(playerCharacter, dialogLineDataRoot)`.
  `showReactivateDialogue()` in `SkeletonRebirthDiagnostics.cpp` resolves `dialogLineDataRoot` via
  `ou->gamedata.getData(SR_REACTIVATE_DIALOGUE_ID, DIALOGUE)` then `DialogDataManager::getData(...)`,
  and a new `Dialogue::replyClicked(int)` hook (fully public, RVA `0x683670`) reads back which
  reply was picked by checking `dialogue->currentLine->getStringID()` against a known "Yes" reply
  ID after letting the real `replyClicked()` run first. Points (1)-(3) below still need live
  confirmation: (1) `startPlayerConversation()` actually opens the native dialogue window when
  called from inside the `applyFirstAid()` hook's call context; (2) `currentLine` has already
  advanced to the clicked reply's own line by the time the hooked `replyClicked()` returns, not to
  some later auto-advanced line; (3) `conversationHasEnded()` is actually false while our dialogue
  is active (used as the "don't stack a second start" guard).
  **FCS content has been authored** under this mod: the "Attempt to reactivate `<name>`?" line with
  two player-reply children, and its "Yes" reply line. Both ended up with FCS's own auto-assigned
  String IDs rather than custom strings - `24-Skeleton Rebirth.mod` (dialogue, originally
  `11-Skeleton Rebirth.mod` until the initial-dialogue line was recreated in FCS and picked up a new
  auto-assigned ID) and `12-Skeleton Rebirth.mod` (Yes reply, unchanged), FCS's default
  `"<index>-<mod filename>.mod"` format - hardcoded as
  `SR_REACTIVATE_DIALOGUE_ID`/`SR_REACTIVATE_YES_REPLY_ID` in the .cpp. **Risk to flag**: since
  these weren't custom-named, deleting and recreating either node in FCS (or renaming the mod file)
  will likely assign yet another new ID and silently break the lookup - the constants would need
  updating to match, exactly as just happened with the dialogue ID.
  **`SR_REACTIVATE_DIALOGUE_ID = "24-Skeleton Rebirth.mod"` is intentional, confirmed by the user -
  not a bug.** The conversation's start point was deliberately moved to `24` in FCS. Investigating
  the earlier "revived text with no Yes click" report, grepping the compiled `.mod` file's string
  table directly showed node `24`'s own text as `"Reset CPU"` with an `effects` field pointing at
  `DialogLineData::DialogAction` node `27` (`"/MYNAME/ was revived!"`, the now-removed FCS-linked
  message text, see above) - that misread as "24 is the wrong root" at the time, but the user
  confirmed the FCS-side restructuring (moving the start point to `24`) was deliberate. The original
  root cause of the premature-text report is still not fully nailed down - removing the FCS-authored
  message text (see above) may have already resolved it as a side effect, or FCS-side changes since
  may have, but this hasn't been independently re-confirmed live. If the premature-text symptom
  resurfaces, revisit this rather than assuming the ID itself is the problem.
  **CONFIRMED BUG, FIXED — FCS's dialogue editor never creates a standalone `itemType::DIALOGUE`
  GameData object at all.** First live test logged `getReactivateDialogueRoot()` failing on every
  single call (`ou->gamedata.getData(id, DIALOGUE)` always returned null, spamming ~1300
  "not found" errors in under a second - `applyFirstAid()` re-fires continuously while the repair
  action animation plays, same as the earlier-documented ~75-137 calls/sec pattern, and nothing
  gated the retries because the lookup never got far enough to start a conversation and make
  `conversationHasEnded()` false). Root cause, confirmed by grepping the compiled `.mod` file's
  string table directly: every node FCS creates for a dialogue tree - the initial line and each
  reply alike - is tagged `DIALOGUE_LINE`, never `DIALOGUE`. Fixed by looking up with
  `itemType::DIALOGUE_LINE` instead.
  **CONFIRMED PROGRESS, CONFIRMED BUG #2, FIXED — `startPlayerConversation()` genuinely opens the
  native dialogue window.** After the `DIALOGUE_LINE` fix, live-tested end to end: the FCS dialogue
  window popped up correctly with the "Attempt to reactivate" prompt and a clickable "Yes" - this
  confirms open question (1) from above. But clicking "Yes" did **not** trigger `TryReactivate()`
  (no flesh nudge, no "was revived!" notification) - `Dialogue::replyClicked` is overloaded
  (`Dialogue.h:378-379`: `replyClicked(const std::string&)` and `replyClicked(int)`), and only the
  `int` overload was hooked. The native `DialogueWindow` almost certainly calls replies by their
  string reply ID (`Dialogue::replyIds` is a `vector<std::string>`), not a numeric index, so the
  hooked overload most likely never fired at all. Fixed by hooking **both** overloads
  (`handleDialogueReplyClicked()` shared by both) and logging unconditionally on every fire - not
  just on a Yes match - specifically to get hard evidence next test of which overload actually
  fires and what `currentLine->getStringID()` looks like, which will also settle open questions (2)
  and (3) from above.
  **CONFIRMED BUG #3, FIXED — both overloads do fire (string then int, same timestamp - `int` looks
  like an internal wrapper around `string`), but `currentLine` was already `null` by the time either
  hook resumed.** Our Yes/No replies are terminal (no further child lines), so the conversation
  ends and clears state (`_endPlayerConversation`) inside `replyClicked()` itself, before the hook
  gets to inspect `currentLine` - checking post-call state was the wrong approach entirely, not
  just the wrong overload. Fixed by using the string overload's own `index` parameter directly
  instead of reading any post-call state - on the hypothesis that it already *is* the clicked
  reply's FCS String ID, since it lines up with `Dialogue::replyIds` being a `vector<std::string>`.
  The `int` overload's `index` is resolved to a reply ID via `self->replyIds[index]` *before*
  calling the original, same reasoning - don't trust any Dialogue state read after the real
  `replyClicked()` runs.
  **CONFIRMED WORKING END TO END.** Live-tested and logged the full chain in order, same timestamp:
  `replyClicked(string) fired -> reply="12-Skeleton Rebirth.mod"` (hypothesis confirmed - the string
  overload's parameter genuinely is the clicked reply's FCS String ID, no `currentLine`/state
  inspection needed at all) -> `TryReactivate() SUCCEEDED` (torso `isDead` flipped `1` -> `0`,
  flesh nudged as expected) -> the repair-kit-in-bed log line. This settles all three open questions
  from the "NOT YET LIVE-TESTED" entry above: `startPlayerConversation()` opens the window correctly
  from inside the `applyFirstAid()` hook, the reply-detection mechanism works (once fixed to not
  depend on post-call `currentLine`), and `conversationHasEnded()` behaves as expected for the
  re-entrancy guard (no stuck/duplicate dialogues observed across repeated tests).
  **Notification changed twice**: `TryReactivate()`'s modal `gui->messageBox(...)` ("`<name>` was
  revived!") was first removed outright (reasoning: the dialogue exchange already got the player's
  attention, a second modal on top felt redundant), then replaced with the exact mechanism this
  plugin used *before* the messageBox existed at all - `ForgottenGUI::createScreenLabel(text,
  colour, size, risingSpeed)` (`gui/ForgottenGUI.h:206`, RVA `0x73E920`) plus
  `ScreenLabel::setTracking(handle, offset)` (`gui/ScreenLabel.h:49`), producing floating green text
  tracking the character - the same visual language as Kenshi's own stat-increase/pickup
  notifications. This was tried first, historically, then dropped in favour of the modal
  `messageBox` specifically because a modal read as more prominent (see the "revival success
  notification" entry above) - now that the FCS dialogue exchange itself is what gets the player's
  attention, that prominence isn't needed and the original, more understated `ScreenLabel` is the
  better fit again. `ScreenLabel`'s constructor is labelled `protected` in the RE'd header, same
  documentation-only situation as `Dialogue::startPlayerConversation` (no real `private:`/
  `protected:` in `ScreenLabel.h`) - moot anyway since `createScreenLabel()` itself is a genuinely
  public factory method.
  **Notification text was briefly FCS-authored, then reverted back to hardcoded.** Tried linking it
  via `SR_REACTIVATE_REVIVED_MESSAGE_ID` (`"18-Skeleton Rebirth.mod"`), resolved through the same
  `GameData` -> `DialogLineData` chain as the dialogue root (`getFcsDialogLine()`), with
  `DialogLineData::getText(false)` (`Dialogue.h:246`) plus
  `Dialogue::insertWordSwaps(text, target, swapMeYou, line)` (`Dialogue.h:333`, public) to resolve
  `/PLACEHOLDER/`-style tokens like `/MYNAME/`. Worked mechanically, but **reverted** - the FCS
  ID-churn problem that also broke the dialogue root (see below) made these auto-assigned IDs feel
  too fragile for something this minor, and the text isn't important enough to be FCS-editable.
  `getReactivateRevivedMessage()` is back to a plain hardcoded `"<name> was revived!"` string. The
  now-unreferenced FCS line (`18-Skeleton Rebirth.mod`) is safe to delete in FCS - nothing in the
  plugin looks it up anymore.
  Skin/layer name strings from the old panel
  (`"Kenshi_FloatingPanelSkin"`, `"Kenshi_Button2"`, `"Main"`) are no longer used now that the
  native dialogue window handles its own UI.
- **Tried and reverted — interrupting the treater's repair action so the item isn't wasted while
  the panel is open.** The panel blocks `applyFirstAid()`'s real effect via its return value
  (returns `true` instead of `false`, since `false` seemed to make the game keep retrying), but the
  visible "Repairing" action/animation itself kept running regardless. Tried
  `OrdersReceiver::removeJob(JOB_REPAIR_ROBOT)` (targeted, confirmed real `TaskType`) - no effect.
  Tried the broader `AITaskSytem::clearAllTasks()` - also no effect (confirmed via logging that the
  *target* character was correct; `goalString` stayed `"Repairing"` across every tick regardless of
  the interrupt call, meaning the AI just immediately re-selects the same action rather than the
  interrupt failing to reach it) - and on a later attempt this combination left the confirmation
  panel permanently stuck/unclickable. Both interrupt attempts were removed; the panel now only
  blocks the *effect*, not the *action* - the repair-kit animation may still play out while the
  panel is open, a known cosmetic limitation, not a correctness problem.
- **CONFIRMED WORKING — third attempt at interrupting the treater's repair action, this time
  because it's no longer just cosmetic.** The dialogue-based confirmation (see above) surfaced a
  real correctness bug the MyGUI panel's structure happened to avoid: `applyFirstAid()` re-fires
  continuously for the same physical action (same root cause as the two reverted attempts above -
  returning `true` without running the real effect means the underlying task never sees its normal
  completion signal), and while `TryReactivate()` (on "Yes") removes the patient from
  `g_deactivated` so later calls stop matching on their own, clicking "No" leaves the patient in
  `g_deactivated` - the very next `applyFirstAid()` tick sees the dialogue already ended (`No`
  answers are terminal too) and pops a fresh one right back open, over and over, for as long as the
  action keeps re-firing. `removeJob()` and `clearAllTasks()` were already ruled out (see above,
  and `clearAllTasks()`'s stuck-panel side effect specifically). This attempt is different in kind,
  not just a retry: `AITaskSytem::_notifyBodyTaskComplete()` (`AI/AITaskSystem.h:219`, public, RVA
  `0x50BFB0`), called on **`who` (the treater passed into `applyFirstAid()`), not the patient** -
  the treater is the one whose AI is actually looping the action, the patient is KO'd and has no
  driving goal of its own. The theory: this is the AI's own natural "this body action just finished"
  signal (what would fire on a real completed repair), so it should end the current goal cleanly
  and let the AI re-evaluate, rather than wiping the task queue (`clearAllTasks()`) or marking the
  goal a failure to be avoided (`taskImpossible()`, deliberately not used here for that reason -
  untested but risked either blacklisting future automatic repair attempts or producing the same
  kind of stuck state `clearAllTasks()` did). Implemented as `notifyTreaterActionComplete()`, called
  every time the trigger condition matches in `MedicalSystem_applyFirstAid_hook`.
  Live-tested end to end: clicking "No" (FCS reply ID `13-Skeleton Rebirth.mod`) produced exactly
  one `replyClicked` log line with no repeated re-opening afterward - confirming the fix. A later,
  separate deliberate repair-kit use 9 seconds after that opened a fresh dialogue normally and
  reactivated cleanly (torso `isDead` flipped `1` -> `0`, no stray re-`declareDead()`), confirming
  the interrupt doesn't block legitimate later attempts either.
  **Separately confirmed NOT a regression from this fix**: an earlier test in this same session saw
  `TryReactivate()` succeed and then `declareDead()` re-fire ~10ms later, looking like "revival
  doesn't work." Turned out to be the pre-existing, already-documented "third death path" mystery
  (see Confirmed-from-testing below) - that character's *original* deactivation already showed
  `torso flesh=200, isDead=0` (nowhere near the normal `-200` threshold), the exact signature of
  that unsolved bug, not something introduced today. `TryReactivate()` only nudges flesh, which does
  nothing for whatever hidden condition drives that path, so it's expected to immediately re-fire in
  that specific scenario regardless of any dialogue/AI-task changes. Confirmed unrelated once a
  normal low-flesh deactivation reactivated cleanly with the exact same code.
- **NOT YET LIVE-TESTED — "Yes" now takes an item from the initiator, driven by FCS's own item
  condition instead of a hardcoded item.** The item requirement isn't on the "Yes" reply line
  itself (`SR_REACTIVATE_YES_REPLY_ID`, `12-Skeleton Rebirth.mod`, still only used to detect which
  reply was clicked) - it's on a **separate** FCS line, `SR_REACTIVATE_ITEM_GATE_ID`
  (`26-Skeleton Rebirth.mod`, confirmed directly by the user, not assumed), whose `hasItem` field
  (`DialogLineData::hasItem` - `lektor<GameData*>`, the same field `checkConditions()`/`chooseAChild()`
  use natively) is what actually holds the required item type - "Use a Power Core to revive
  `/MYNAME/`?" is that item's name showing through in the line's own text. Rather than hardcoding
  the item's FCS ID separately (which would need to be kept in sync with whatever FCS's condition
  actually checks), `takeReviveCostItem()` reads `hasItem` directly off that gate line at click
  time and takes one of whichever required type the initiator actually has, via
  `Inventory::hasItem()`/`takeOneItemOnly()` (`Inventory.h`) on `initiator->_NV_getInventory()`.
  Re-checks `hasItem` defensively even though FCS's own condition already gated the reply once, in
  case the item was lost between the dialogue opening and "Yes" being clicked - if so, logs an
  error and does **not** reactivate (no free revive).
  **"initiator" needed a real fix, not just a new field**: `showReactivateDialogue()` previously
  always passed `ou->player->getAnyPlayerCharacter()` as `startPlayerConversation()`'s target,
  regardless of who actually used the repair kit - harmless before (nothing depended on exactly
  *which* character it was), but wrong now that a specific character's inventory is being charged.
  Fixed by threading `who` (the treater, from `applyFirstAid()`) through
  `showReactivateDialogue(patient, who)` instead, falling back to any player character only if
  `who` is somehow unavailable.
  **Resolving the initiator at reply time hit the same trap as `currentLine`**: `Dialogue::conversationTarget`
  is captured via `self->conversationTarget.getCharacter()` (`util/hand.h:49`) *before* calling the
  real `replyClicked()` in both hook overloads, not after - on the same reasoning already confirmed
  for `currentLine` and the int overload's `replyIds[index]`: don't trust any `Dialogue` state read
  after the real call returns, since `_endPlayerConversation()` may have already cleared it.
  **VS2010 toolset note**: a first draft used a C++11 range-based `for` over
  `DialogLineData::hasItem` (a `lektor<GameData*>`) and failed to compile (`C2143`) - this project's
  VS2010 (`v100`) toolset predates C++11 range-for entirely; needed a plain indexed loop instead
  (`lektor` supports `operator[]`/`size()`, see `util/lektor.h`).

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

### 4. Revival — DECIDED and wired: Skeleton Repair Bed + repair kit item + player confirmation

Item-triggered: the *trigger* is a repair kit used in the Skeleton Repair Bed, not a dialogue.
`TryReactivate()` is an unconditional release (see Status) called from a hook on
`MedicalSystem::applyFirstAid()`, gated on: character is in `g_deactivated`, item used has
`itemFunction == ITEM_ROBOTREPAIR`, and character is in a bed with
`Building::_NV_getSpecialFunction() == BF_SKELETON_BED` (confirmed `25`). No readiness/health
check - health is frozen while Deactivated, so nothing to gate on; healing happens normally via
the bed after reactivation releases the freeze.

Reactivation additionally requires an explicit player choice, and *that* confirmation step is now
dialogue-based: instead of calling `TryReactivate()` directly, the hook starts a real FCS dialogue
("Attempt to reactivate `<name>`?" with Yes/No replies) via `Dialogue::startPlayerConversation()`
and only reactivates if the player picks the reply tagged as "Yes" (read back via a
`Dialogue::replyClicked()` hook). See the Status section for the full story, including why an
earlier version used a hand-built MyGUI panel instead, and what FCS content and live testing this
still needs.

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
- `ForgottenGUI* gui` — global singleton, `Globals.h` (`__declspec(dllimport)`)
- `ForgottenGUI::messageBox(title, message, btn, modal, callback)` — gui/ForgottenGUI.h:70
  (RVA 0x740F60 - used only for the single-button success notification, see Status)
- **Superseded, no longer called — kept for historical reference only (the reactivation prompt is
  now the FCS dialogue system, see Status):**
  - `ForgottenGUI::createPanel(name, top, left, width, height, layer, skin)` — gui/ForgottenGUI.h:177
  - `ForgottenGUI::createButton(parent, top, left, width, height, name, text, skin)` — gui/ForgottenGUI.h:184
  - `ForgottenGUI::createLabel(parent, top, left, width, height, text, align)` — gui/ForgottenGUI.h:191
  - `ForgottenGUI::destroyWidget(MyGUI::Widget*)` — gui/ForgottenGUI.h:121
  - `MyGUI::Widget::eventMouseButtonClick` — mygui/MyGUI_WidgetInput.h:169
  - `MyGUI::Align::Center` — mygui/MyGUI_Align.h:25
  - Skin/layer name strings (`"Kenshi_FloatingPanelSkin"`, `"Kenshi_Button2"`,
    `"Kenshi_GenericTextBoxSkin"`, `"Main"`)
- `Dialogue* Character::dialogue` — Character.h:492 (0x280 Member) - every `Character` owns one
- `Dialogue::startPlayerConversation(Character* target, DialogLineData* _talk)` — Dialogue.h:390
  (RVA 0x683BA0 - opens the real interactive dialogue window; see Status for the "private" label
  being documentation-only, not a real access restriction)
- `Dialogue::startConversation(Character* target, DialogLineData* _talk, EventTriggerEnum ev, bool force)`
  — Dialogue.h:391 (RVA 0x683810 - lower-level generic starter, not currently used; `startPlayerConversation`
  covers our case)
- `Dialogue::replyClicked(int index)` — Dialogue.h:379 (RVA 0x683670, public - hooked to read back
  the player's reply)
- `Dialogue::conversationHasEnded() const` — Dialogue.h:355 (RVA 0x6714E0 - used as the "don't
  start a second conversation on top of one already running" guard)
- `Dialogue::me` / `Dialogue::currentLine` — Dialogue.h:395,401 (0x150/0x198 Members - `me` is the
  Dialogue's owning `Character*`, i.e. the patient, not the player)
- `DialogLineData::getStringID() const` — Dialogue.h:248 (RVA 0x6A12E0 - the FCS "String ID" field
  set per-line in FCS; used to detect the "Yes" reply)
- `DialogDataManager::getData(GameData* d)` — Dialogue.h:443 (RVA 0x6AD120, static - converts an
  FCS Dialogue `GameData*` into the `DialogLineData*` tree `startPlayerConversation` needs)
- `GameDataContainer::getData(const std::string& sid, itemType category)` — GameDataManager.h:28
  (RVA 0x6BF420 - resolves an FCS String ID + category to a `GameData*`; called as
  `ou->gamedata.getData(sid, DIALOGUE)`)
- `GameWorld::gamedata` — GameWorld.h:97 (0x20 Member, type `GameDataManager`) - reached via the
  global `ou` (`Globals.h`)
- `PlayerInterface::getAnyPlayerCharacter() const` — PlayerInterface.h:177 (RVA 0x7F19B0) - reached
  via the global `ou->player`
- `itemType::DIALOGUE` — Enums.h:24, one value of the `itemType` category enum passed to `getData()`
- `OrdersReceiver::removeJob(TaskType)` / `AITaskSytem::clearAllTasks()` — AI/AITaskSystem.h -
  **tried and reverted**, see Status; both are real, confirmed-linkable methods, but neither
  stopped the treater's repair action from continuing, and `clearAllTasks()` caused a
  confirmation-panel-stuck regression once - do not reintroduce without solving that first
- `TaskType::JOB_REPAIR_ROBOT` — Enums.h:336 (confirmed real, but removing this specific job had
  no observable effect - see Status)
- Existing hook pattern to follow — `/home/bryan/Git/KenshiLimbs/The Limbless (Type 2)/RobotLimbRaceLock.cpp`
