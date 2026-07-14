#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/util/hand.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Dialogue.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/gui/ForgottenGUI.h>

#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_LayoutManager.h>

#include <core/Functions.h>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/error/en.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

// AI/AI.h forward-declares CharacterMessage as a class while Character.h defines it as an enum -
// including both together is a compile error. We only need one method off AI, so declare it
// locally instead of pulling in the full header.
class AITaskSytem;
class AI
{
public:
	AITaskSytem* getTaskSystem() const;
};

// SkeletonRebirth plugin - see SkeletonRebirth/DESIGN.md for the full design history and
// rejected approaches. Core mechanism, two hooks: robots never actually die (declareDead), and
// their medical simulation is completely frozen - health can't change in either direction, and
// declareDead() can't re-trigger - for as long as they're Deactivated (MedicalSystem::medicalUpdate
// skipped entirely). Healing only needs to happen after TryReactivate() releases the lock, not
// before. Reactivation trigger: MedicalSystem::medicalUpdate() also starts the FCS reactivation
// dialogue once a Deactivated robot is placed in the Skeleton Repair Bed (see that hook's comment).
// Two more hooks handle the dialogue itself: Dialogue::replyClicked() reads
// back which reply the player picked and dispatches whatever JSON-configured "conversation
// overrides" (RE_Kenshi.json, "ConversationOverrides") are attached to that reply - see the comment
// above ConversationOverride further down for the full picture.
//
// Output goes to KenshiLib's own debug log via DebugLog() - see Debug.h. Filter the log for the
// "SkeletonRebirth:" prefix.

static bool isRobotRace(Character* c)
{
	RaceData* race = c->_NV_getRace();
	return race != nullptr && race->robot;
}

// Marks characters blocked from dying (declareDead hook) so the medicalUpdate hook knows to keep
// them frozen. Pointer-keyed and session-only - the real feature needs a handle-keyed, save-aware
// version (see DESIGN.md §2), since Character* doesn't survive across sessions.
static std::map<Character*, bool> g_deactivated;

// Set once the reactivation dialogue has been opened for a patient, cleared once that "ask and await
// an answer" cycle has genuinely concluded (see the clearing site in Dialogue_update_hook). Gates
// MedicalSystem_medicalUpdate_hook's trigger check so it doesn't reopen the dialogue on top of itself
// every tick while an answer is still pending.
static std::map<Character*, bool> g_reactivateDialogueShown;

// Dumps every entry in MedicalSystem::anatomy (the real, complete part list - see the comment on
// TryReactivate's nudge loop for why this replaced two hardcoded getPart(PART_HEAD/PART_TORSO) calls:
// those only ever reached one torso-region part, silently missing a second one ("chest", distinct
// from "stomach" in Kenshi's real damage model) that isn't reachable through the type/side accessor
// at all). Labelled by the part's own GameData stringID when available (same field already used for
// race identification elsewhere in this file) so distinct torso-region parts are told apart in logs.
static std::string describeAnatomy(MedicalSystem* med)
{
	if (!med)
		return "anatomy=<no medical>";

	std::ostringstream ss;
	ss << "anatomy=[";
	for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
	{
		MedicalSystem::HealthPartStatus* part = *it;
		if (!part)
			continue;

		std::string label = (part->data && !part->data->stringID.empty()) ? part->data->stringID : "?";
		ss << " " << label << "(type=" << (int)part->whatAmI << " side=" << (int)part->side
		   << " flesh=" << part->flesh << " fatal=" << part->fatal << " isDead=" << part->isDead() << ")";
	}
	ss << " ]";
	return ss.str();
}

// A std::string-by-value return uses a hidden-pointer x64 calling convention that a hook can get
// wrong and crash - only ever call this normally, never hook it directly.
static std::string getCurrentGoalStringSafe(Character* c)
{
	AI* ai = c->getAI();
	if (!ai)
		return "<no AI>";

	AITaskSytem* taskSystem = ai->getTaskSystem();
	if (!taskSystem)
		return "<no task system>";

	return taskSystem->getCurrentGoalString();
}

static std::string describe(Character* c)
{
	MedicalSystem* med = c->getMedical();

	std::ostringstream ss;
	ss << "name=\"" << c->_NV_getName() << "\""
	   << " ptr=" << (void*)c
	   << " handle=" << c->getHandle().toString()
	   << " race=" << (c->_NV_getRace() ? c->_NV_getRace()->data->stringID : "<none>")
	   << " robot=" << isRobotRace(c)
	   << " isDead=" << c->isDead()
	   << " medDead=" << (med ? med->dead : false)
	   << " proneState=" << (int)c->_NV_getProneState()
	   << " knockoutTimer=" << (med ? med->knockoutTimer : -1.0f)
	   << " blood=" << (med ? med->blood : -1.0f)
	   << " " << describeAnatomy(med)
	   << " isOnScreen=" << c->isOnScreen
	   << " isVisibleAndNear=" << c->isVisibleAndNear
	   << " goalString=\"" << getCurrentGoalStringSafe(c) << "\"";
	return ss.str();
}

// Revival requires the character to be in the Skeleton Repair Bed specifically (BF_SKELETON_BED,
// confirmed == 25 via live testing - not the FCS-adjacent guess of 150 originally assumed), not
// just any bed.
static bool isInSkeletonBed(Character* c)
{
	if (c->inSomething != IN_BED)
		return false;

	RootObject* obj = c->inWhat.getRootObject();
	Building* bed = obj ? static_cast<Building*>(obj) : nullptr;
	return bed && bed->_NV_getSpecialFunction() == BF_SKELETON_BED;
}

// --- Hook 1: declareDead() -----------------------------------------------------------------
// Fires right before a character would actually die. For robots, blocks the original call but lets
// MedicalSystem::dead read as true (matching a real death) - GUI/party-membership/AI/looting logic
// all read that flag directly, not whether declareDead() ran, so robots are operationally treated as
// dead (lootable, no longer a live threat/ally) while staying reversible, since declareDead() itself
// - and whatever undocumented cleanup it triggers - never runs. TryReactivate() below is the only
// thing that ever clears g_deactivated (and sets dead back to false).
void (*Character_declareDead_orig)(Character*);
void Character_declareDead_hook(Character* self)
{
	if (isRobotRace(self))
	{
		MedicalSystem* med = self->getMedical();
		if (med)
			med->dead = true;

		g_deactivated[self] = true;

		DebugLog("SkeletonRebirth: declareDead() BLOCKED for robot -> " + describe(self));
		return;
	}

	Character_declareDead_orig(self);
}

// Real reactivation trigger - wired to the reactivation dialogue below (shown when a repair kit is
// used on a Deactivated character in the Skeleton Bed). Unconditional release, not a readiness
// gate: health can't change at all while Deactivated (medicalUpdate is frozen below), so a "was it
// repaired?" check here would be a permanent deadlock. Healing happens normally after this
// releases the lock, not before.
//
// Reactivating with flesh still deep in fatal range would let medicalUpdate() immediately call
// declareDead() again the moment it un-freezes - our hook would re-block it, snapping the
// character straight back into Deactivated. A 1% nudge of a fatal part's flesh toward zero (tested
// and confirmed correct: enough to clear whatever makes isDead() stick, not a full heal - the
// character wakes up critically hurt and needs immediate treatment, which is the intended
// difficulty) avoids that without granting free healing.
//
// Only applies to parts still in fatal (negative) territory - now that TryReactivate calls this for
// every part in anatomy (not just the two known-fatal ones, see that loop's comment), most parts
// passed in are already fine. Unconditionally adding fabs(flesh)*0.01 to an already-healthy
// (positive) part pushes it further positive, not toward safety - live tested, it overshot several
// parts' normal maximum. Clamped to the part's own maxHealth() regardless, as a hard backstop.
static void nudgeFleshTowardSurvivable(MedicalSystem::HealthPartStatus* part)
{
	if (!part)
		return;

	if (part->flesh < 0.0f)
		part->flesh += std::fabs(part->flesh) * 0.01f;

	float cap = part->maxHealth();
	if (part->flesh > cap)
		part->flesh = cap;
}

// Narrowed to just the mechanical state change - no notification text of its own. Showing something
// on screen is left to FCS-authored content / other extensions, not this function.
// Root cause of the instant-re-death bug (live tested, confirmed by direct feedback): "chest" and
// "stomach" are separate body parts in Kenshi's real damage model, but MedicalSystem::HealthPartStatus
// ::PartType (MedicalSystem.h:134-140) only has four generic buckets (TORSO/LEG/ARM/HEAD) - getPart()
// only ever reached ONE torso-region part (nudged correctly each time - the one the player saw as
// "stomach" ticking up), while a second, separate torso-region part ("chest") sitting in the same
// TORSO bucket was never reachable through that API at all, stayed at its original fatal flesh value,
// and kept re-triggering declareDead() every time medicalUpdate() unfroze. MedicalSystem::anatomy
// (MedicalSystem.h:296, `lektor<HealthPartStatus*>`) is the real, complete list of every body part the
// character has - nudging every entry in it instead of manually enumerating type/side combinations
// covers chest and stomach both (and anything else the type/side accessor can't reach) without needing
// to know the exact part breakdown.
bool TryReactivate(Character* self)
{
	auto deactivatedIt = g_deactivated.find(self);
	if (deactivatedIt == g_deactivated.end() || !deactivatedIt->second)
		return false; // not actually Deactivated - nothing to do

	MedicalSystem* med = self->getMedical();
	if (med)
	{
		med->dead = false; // reverses the declareDead hook's dead=true - matches revival back to "alive"
		for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
			nudgeFleshTowardSurvivable(*it);
	}

	g_deactivated.erase(self);
	DebugLog("SkeletonRebirth: TryReactivate() SUCCEEDED -> " + describe(self));

	return true;
}

// Confirmation prompt shown instead of reactivating immediately - this is meant to be a deliberate
// choice, not an automatic side effect of placing a robot in the bed.
//
// NOT built on Dialogue::startPlayerConversation() - live testing showed the game suppresses
// dialogue entirely once a character has MedicalSystem::dead == true, which is now permanently the
// case for a Deactivated robot (see Character_declareDead_hook's comment).
//
// NOT built on ForgottenGUI::messageBox() either - that got as far as actually appearing (confirming
// it, unlike Dialogue, isn't gated by character aliveness), but its `btn` parameter is completely
// undocumented in RE_Kenshi's headers, and a live test with a guessed value produced a single "No"
// button instead of Yes/No.
//
// NOT hand-built from ForgottenGUI::createPanel/createButton either - two different button skins were
// live-tested, one rendering visible-but-uncaptioned buttons, the other rendering nothing at all.
// Root cause found by inspecting Kenshi's own UI files on disk
// (data/gui/layout/Kenshi_MessageBox.layout): real Kenshi buttons/windows are composed from
// *ResourceLayout* templates (data/gui/templates/kenshi_templates.xml - "Kenshi_Button2" etc.), not
// single-skin widgets - createButton(..., "Kenshi_Button2") was never going to render a real button,
// it was passing a template name where a plain skin was expected. Kenshi_MessageBox.layout is a
// genuine 3-button confirm/notify template already used throughout the live game (Kenshi_OverviewWindow,
// Kenshi_MainPanel, etc. all reference its component skins) - loading that exact layout via
// MyGUI::LayoutManager::loadLayout() and repurposing its widgets (found by name via
// Widget::findWidget(), captions set via the same generic Widget::setProperty("Caption", ...) the XML
// <Property> tags compile down to) reuses vanilla's own proven structure instead of reconstructing it
// from individually-guessed primitives.
static Character* g_pendingReactivationPatient = nullptr;
static MyGUI::VectorWidgetPtr g_reactivateLayoutWidgets;

static void closeReactivatePanel()
{
	if (!g_reactivateLayoutWidgets.empty())
	{
		MyGUI::LayoutManager::getInstance().unloadLayout(g_reactivateLayoutWidgets);
		g_reactivateLayoutWidgets.clear();
	}
	g_pendingReactivationPatient = nullptr;
}

static void OnReactivateYesClicked(MyGUI::Widget* sender)
{
	Character* patient = g_pendingReactivationPatient;
	closeReactivatePanel();
	if (!patient)
		return;

	DebugLog("SkeletonRebirth: reactivate panel - Yes clicked for " + patient->_NV_getName());
	if (!TryReactivate(patient))
		ErrorLog("SkeletonRebirth: TryReactivate failed for " + patient->_NV_getName() + " after confirm");

	// Tidiness only - TryReactivate() already erased patient from g_deactivated on success, so the
	// trigger check in Character_updateOnScreenCheck_hook short-circuits before ever reading this
	// regardless. On failure, deliberately NOT erased - see the comment on the "No" branch below for
	// why leaving it set is what prevents an instant re-prompt loop.
	g_reactivateDialogueShown.erase(patient);
}

static void OnReactivateNoClicked(MyGUI::Widget* sender)
{
	Character* patient = g_pendingReactivationPatient;
	closeReactivatePanel();
	if (patient)
		DebugLog("SkeletonRebirth: reactivate panel - No clicked for " + patient->_NV_getName());

	// Deliberately NOT erasing g_reactivateDialogueShown here (unlike the old, event-triggered
	// version of this flow). The trigger is now level-triggered - it fires every tick the character
	// is still in the bed (see Character_updateOnScreenCheck_hook) - so clearing the flag here would
	// let the very next tick re-open the panel immediately, making "No" appear to do nothing (live
	// tested - it does exactly this). Leaving the flag set means "already asked for this bed-visit";
	// it only gets cleared once the character actually leaves the bed, in the hook below.
}

// initiator is unused now (this panel has no per-character "who's asking" concept) - kept in the
// signature since callers already have it in hand and it costs nothing to ignore.
static void showReactivateDialogue(Character* patient, Character* initiator)
{
	if (g_pendingReactivationPatient)
		return; // one at a time - a second bed-placement shouldn't stack a second panel

	g_pendingReactivationPatient = patient;

	// Prefixing every widget name avoids collisions with a second load of the same layout elsewhere
	// in the game (MyGUI names should be unique) - "Root"/"ButtonA" etc. become
	// "SkeletonRebirth_Root"/"SkeletonRebirth_ButtonA" etc. after loading.
	const std::string prefix = "SkeletonRebirth_";
	g_reactivateLayoutWidgets = MyGUI::LayoutManager::getInstance().loadLayout("Kenshi_MessageBox.layout", prefix, nullptr);
	if (g_reactivateLayoutWidgets.empty())
	{
		ErrorLog("SkeletonRebirth: loadLayout(Kenshi_MessageBox.layout) returned no widgets");
		g_pendingReactivationPatient = nullptr;
		return;
	}

	MyGUI::Widget* root = g_reactivateLayoutWidgets[0];
	root->setProperty("Caption", "Reactivate?");

	MyGUI::Widget* messageText = root->findWidget(prefix + "MessageText");
	if (messageText)
	{
		std::string message = "Attempt to reactivate " + patient->_NV_getName() + "? It will wake up critically damaged and need immediate treatment.";
		messageText->setProperty("Caption", message);
	}

	MyGUI::Widget* buttonA = root->findWidget(prefix + "ButtonA");
	if (buttonA)
	{
		buttonA->setProperty("Caption", "Yes");
		buttonA->eventMouseButtonClick += MyGUI::newDelegate(OnReactivateYesClicked);
	}

	MyGUI::Widget* buttonB = root->findWidget(prefix + "ButtonB");
	if (buttonB)
	{
		buttonB->setProperty("Caption", "No");
		buttonB->eventMouseButtonClick += MyGUI::newDelegate(OnReactivateNoClicked);
	}

	// The layout has a third button (ButtonC) this feature has no use for - only Yes/No are needed.
	MyGUI::Widget* buttonC = root->findWidget(prefix + "ButtonC");
	if (buttonC)
		buttonC->setVisible(false);

	DebugLog("SkeletonRebirth: reactivate panel (Kenshi_MessageBox.layout) shown for " + patient->_NV_getName());
}

// --- JSON-driven conversation overrides ---------------------------------------------------------
// What happens when a given FCS reply fires is data-driven (RE_Kenshi.json, "ConversationOverrides"
// key), not hardcoded per-feature - only *starting* the conversation in the first place
// (SR_REACTIVATE_DIALOGUE_ID above) stays in the DLL. Any FCS reply/line ID can be tagged with one
// or more named overrides; the plugin dispatches to whichever handlers are registered for that
// override's "type" whenever Dialogue::replyClicked reports that ID. Adding a new FCS-authored
// choice that reuses an existing override type (reactivate_skeleton) needs zero new C++ - just a
// JSON edit and a game restart.
struct ConversationOverride
{
	std::string type;                          // e.g. "reactivate_skeleton"
	std::map<std::string, std::string> params;  // empty if none
};
static std::map<std::string, std::vector<ConversationOverride> > g_conversationOverrides; // keyed by FCS reply ID

// dialogueLine is currently always nullptr - no override handler needs the resolved DialogLineData
// object, so dispatch never resolves it (see dispatchConversationOverrides). Kept in the signature so
// a future override type that genuinely needs it can add SEH-guarded resolution scoped to just that
// handler, without forcing every dispatch back into the native lookup.
typedef bool (*OverrideHandler)(Character* patient, Character* initiator, DialogLineData* dialogueLine, const ConversationOverride&);
static std::map<std::string, OverrideHandler> g_overrideHandlers; // keyed by override type

static std::string getParam(const ConversationOverride& override, const std::string& key, const std::string& fallback)
{
	std::map<std::string, std::string>::const_iterator it = override.params.find(key);
	return it != override.params.end() ? it->second : fallback;
}

// The existing revival logic, unchanged - just relocated behind the generic dispatch instead of
// being called directly by name from handleDialogueReplyClicked.
static bool applyReactivateSkeletonOverride(Character* patient, Character* initiator, DialogLineData* dialogueLine, const ConversationOverride& override)
{
	bool success = TryReactivate(patient);
	if (success)
		g_reactivateDialogueShown.erase(patient); // cycle concluded - being placed in the bed again later may prompt again

	return success;
}

// Dialogue::replyClicked can report both sides of a mutually-exclusive Yes/No choice for what the
// player experienced as a single click (a spurious report of the unclicked side, ~1.5s before the
// real one). Dispatching immediately on the first replyClicked seen would act on the wrong answer, so
// replies are buffered per-patient instead and only committed once Dialogue::update() (see below)
// detects the conversation has ended - trust the last reply reported before the conversation
// genuinely ends, not the first one seen.
static std::map<Character*, std::string> g_pendingReplyId;
static std::map<Character*, Character*> g_pendingInitiator;

static void dispatchConversationOverrides(Character* patient, Character* initiator, const std::string& replyId)
{
	std::map<std::string, std::vector<ConversationOverride> >::iterator overridesIt = g_conversationOverrides.find(replyId);
	if (overridesIt == g_conversationOverrides.end())
	{
		DebugLog("SkeletonRebirth: no ConversationOverrides configured for reply \"" + replyId + "\" - nothing to do");
		return;
	}

	// dialogueLine isn't resolved here - no handler needs it (see the OverrideHandler typedef comment
	// above), so dispatch runs purely off the FCS String ID, never touching the underlying game data
	// object for content this plugin didn't author.
	const std::vector<ConversationOverride>& overrides = overridesIt->second;
	for (size_t i = 0; i < overrides.size(); ++i)
	{
		const ConversationOverride& override = overrides[i];

		std::map<std::string, OverrideHandler>::iterator handlerIt = g_overrideHandlers.find(override.type);
		if (handlerIt == g_overrideHandlers.end())
		{
			ErrorLog("SkeletonRebirth: no handler registered for conversation override type \"" + override.type + "\"");
			continue;
		}

		if (!handlerIt->second(patient, initiator, nullptr, override))
			ErrorLog("SkeletonRebirth: conversation override \"" + override.type + "\" failed for reply \"" + replyId + "\"");
	}
}

// --- Reply detection: Dialogue::replyClicked, both overloads ---------------------------------------
// The standard dialogue system handles the actual conversation UI end-to-end once
// startPlayerConversation() has started it (see showReactivateDialogue() above); this hook is only how
// the plugin finds out which reply the player picked.
//
// This fires for every dialogue reply click in the entire game, not just this mod's, so
// g_conversationOverrides.count(replyId) must stay the very first thing this function does - a plain
// map lookup, no native calls, no string construction, a true instant no-op for anything unconfigured.
// Both overloads are hooked because the native dialogue window calls both for a single click, but only
// the string overload's value is trusted (see the int overload's comment below).
static void handleDialogueReplyClicked(Dialogue* self, const std::string& replyId, Character* initiator, const char* fromOverload, bool trustworthy)
{
	if (!g_conversationOverrides.count(replyId))
		return;

	Character* patient = self->me;
	if (!patient)
		return;

	DebugLog("SkeletonRebirth: replyClicked(" + std::string(fromOverload) + ") fired -> patient=" + patient->_NV_getName() + " reply=\"" + replyId + "\"");

	// The int overload's resolved index->replyIds lookup is known unreliable (live-tested: can report
	// a different reply than the string overload for the same click) - logged for diagnostics only,
	// never buffered/dispatched.
	if (!trustworthy)
		return;

	g_pendingReplyId[patient] = replyId;
	g_pendingInitiator[patient] = initiator;
}

// index here is a position into Dialogue::replyIds, not itself a reply's String ID - resolved to a
// String ID for the diagnostic log line only. Its value is known unreliable (can report a different
// reply than the string overload for the same click), so it's never buffered/dispatched, only logged.
void (*Dialogue_replyClickedInt_orig)(Dialogue*, int);
void Dialogue_replyClickedInt_hook(Dialogue* self, int index)
{
	std::string replyId = (index >= 0 && (size_t)index < self->replyIds.size()) ? self->replyIds[index] : "<index out of range>";
	Dialogue_replyClickedInt_orig(self, index);
	handleDialogueReplyClicked(self, replyId, nullptr, "int", false);
}

void (*Dialogue_replyClickedStr_orig)(Dialogue*, const std::string&);
void Dialogue_replyClickedStr_hook(Dialogue* self, const std::string& index)
{
	Character* initiator = self->conversationTarget.getCharacter(); // capture before orig - same reasoning as currentLine elsewhere in this file
	Dialogue_replyClickedStr_orig(self, index);
	handleDialogueReplyClicked(self, index, initiator, "string", true);
}

// Dialogue::_endPlayerConversation does not fire in this flow, so conversation-end is detected by
// edge-triggering on Dialogue::conversationHasEnded() inside Dialogue::update() instead - a per-frame,
// per-character function (same "hot function, hook carefully" category as the deliberately-avoided
// Character::update()), so a cheap map lookup gates everything else: this only does real work for
// patients that actually have a pending reply buffered, in practice zero characters almost all the
// time, one at most while our own dialogue is active.
void (*Dialogue_update_orig)(Dialogue*, float);
void Dialogue_update_hook(Dialogue* self, float frameTime)
{
	Character* patient = self->me;
	bool hasPending = patient && g_pendingReplyId.count(patient) != 0;

	Dialogue_update_orig(self, frameTime);

	if (!hasPending || !self->conversationHasEnded())
		return;

	std::string replyId = g_pendingReplyId[patient];
	Character* initiator = g_pendingInitiator.count(patient) ? g_pendingInitiator[patient] : nullptr;
	g_pendingReplyId.erase(patient);
	g_pendingInitiator.erase(patient);

	DebugLog("SkeletonRebirth: conversation ended for " + patient->_NV_getName() + " - committing last-seen reply \"" + replyId + "\"");
	dispatchConversationOverrides(patient, initiator, replyId);

	// If dispatch didn't reactivate the patient (e.g. "No", or a reply with no reactivate_skeleton
	// override at all), this "ask and await an answer" cycle is over: clear g_reactivateDialogueShown
	// so leaving and re-entering the bed later can prompt again (MedicalSystem_medicalUpdate_hook is
	// what actually guards against the dialogue reopening every tick while still in the bed).
	if (g_deactivated.count(patient))
		g_reactivateDialogueShown.erase(patient);
}

// Character::update() is deliberately NOT hooked. Skipping it entirely (an earlier version did)
// stops vanilla's stand-up/recovery check, but also breaks position syncing while carried and
// causes a rigid, non-ragdolling corpse - both fixed by leaving it alone. Nothing needs to gate on
// it: MedicalSystem::medicalUpdate() below is hooked independently of caller, so it stays frozen
// even when called from inside Character::update().

// --- Hook 2: MedicalSystem::medicalUpdate() ---------------------------------------------------
// Freezes a Deactivated character's medical simulation entirely - no health change in either
// direction, and stops declareDead() from being able to re-trigger. MedicalSystem has no direct
// owning-Character* field, so reach it via getPart(...)->me.
//
// This is NOT also the reactivation-trigger polling point (an earlier version tried that) - once
// Character_declareDead_hook started leaving dead=true, this hook stopped firing at all for
// Deactivated robots. Per direct feedback ("I think the whole medical system shuts down, so no
// healing attempts would tick"): the engine itself appears to stop calling medicalUpdate() once a
// character is genuinely dead, not just skip its effects - there's nothing left for it to simulate.
// Harmless for freezing (an already-uncalled function needs no freezing), but useless as a trigger.
// See Character_updateOnScreenCheck_hook below for where the trigger moved instead.
void (*MedicalSystem_medicalUpdate_orig)(MedicalSystem*, float);
void MedicalSystem_medicalUpdate_hook(MedicalSystem* self, float frameTime)
{
	MedicalSystem::HealthPartStatus* head = self->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER);
	Character* owner = head ? head->me : nullptr;

	if (owner)
	{
		auto it = g_deactivated.find(owner);
		if (it != g_deactivated.end() && it->second)
			return;
	}

	MedicalSystem_medicalUpdate_orig(self, frameTime);
}

// Random-spawn robots left Deactivated and unattended eventually complete a real death, matching
// normal dead-NPC cleanup - Player/Unique robots are permanently exempt (per direct feedback:
// "Player and Unique should permanently remain"). Continuously tracking a "last seen on screen"
// timestamp (refreshed every tick while isOnScreen, checked against the threshold only while not)
// absorbs isOnScreen's frustum/occlusion-boundary flicker for free - DESIGN.md §4's original sketch
// needed a dedicated edge-detection debounce for this; polling every tick doesn't.
static std::map<Character*, double> g_lastSeenOnScreen;
static const float CLEANUP_THRESHOLD_HOURS = 3.0f * 24.0f; // 3 in-game days

// --- Hook: Character::updateOnScreenCheck() - reactivation trigger + cleanup sweep polling point ---
// Reused from DESIGN.md §4's cleanup-sweep proposal ("fires roughly every frame per character") as
// the reactivation-trigger polling point too, since medicalUpdate() (the original choice) stopped
// firing once dead=true - see that hook's comment. Screen-visibility tracking is a different
// subsystem from medical simulation and keeps running for a corpse (confirmed by live testing -
// rendering/culling still needs it). Always calls orig() - this only observes, never blocks/alters
// the real on-screen-check behavior.
bool (*Character_updateOnScreenCheck_orig)(Character*);
bool Character_updateOnScreenCheck_hook(Character* self)
{
	bool result = Character_updateOnScreenCheck_orig(self);

	auto it = g_deactivated.find(self);
	if (it == g_deactivated.end() || !it->second)
		return result;

	if (isInSkeletonBed(self))
	{
		if (!g_reactivateDialogueShown.count(self))
		{
			Character* initiator = ou->player ? ou->player->getAnyPlayerCharacter() : nullptr;
			showReactivateDialogue(self, initiator);
			g_reactivateDialogueShown[self] = true;
		}
	}
	else
	{
		// Left the bed (or never entered it yet) - reset "already asked" so a future bed-placement
		// prompts again. This is also what makes a "No" answer's dismissal stick instead of an
		// instant re-prompt loop: see OnReactivateNoClicked's comment - that handler deliberately
		// does NOT clear this flag itself, only leaving the bed does.
		g_reactivateDialogueShown.erase(self);
	}

	if (self->isOnScreen)
	{
		g_lastSeenOnScreen[self] = ou->getTimeStamp_inGameHours().time;
	}
	else
	{
		auto seenIt = g_lastSeenOnScreen.find(self);
		if (seenIt != g_lastSeenOnScreen.end() && ou->getTimeFromStamp_inGameHours(seenIt->second) >= CLEANUP_THRESHOLD_HOURS)
		{
			if (self->isPlayerCharacter() || self->isUnique())
			{
				// Permanently exempt - checked at sweep time (not at the moment of Deactivation), so
				// a robot that's later recruited/renamed unique is still covered correctly. Keep
				// tracking its on-screen timestamp regardless (harmless) rather than special-casing
				// it out of g_lastSeenOnScreen entirely.
			}
			else
			{
				DebugLog("SkeletonRebirth: cleanup sweep - letting real death proceed for " + describe(self));
				g_deactivated.erase(self);
				g_reactivateDialogueShown.erase(self);
				g_lastSeenOnScreen.erase(seenIt);
				Character_declareDead_orig(self);
			}
		}
	}

	return result;
}

// --- Hook: DatapanelGUI::setLine(key,s1,s2,category,last,keyVisible) - status tag override -------
// The character info panel's "State:" row (category 3) is corpse-only - confirmed via live testing
// that it never fires while a robot sits Deactivated with dead=false, but fires reliably once dead
// is left true (see Character_declareDead_hook). Vanilla sets it to an inline-color-tagged "Dead"
// (dark red, e.g. "#59231aDead"); for a Deactivated robot this overrides it to a distinct color +
// "Deactivated" instead, so it reads differently from a real death at a glance. A second, separate
// all-caps "DEAD" banner line goes through this same overload (the word is baked directly into
// keyValue/s1 with no separate s2) and is overridden the same way.
static std::string toLowerCopy(const std::string& s)
{
	std::string result = s;
	for (size_t i = 0; i < result.size(); ++i)
		result[i] = (char)tolower((unsigned char)result[i]);
	return result;
}

// MyGUI inline color tags are a literal '#' + 6 hex digits prefixed directly onto the text with no
// separator (e.g. "#59231aDead"). Splits that prefix off so the plain word can be compared/rebuilt.
static void splitColorTag(const std::string& s, std::string* colorOut, std::string* textOut)
{
	if (s.size() >= 7 && s[0] == '#')
	{
		*colorOut = s.substr(0, 7);
		*textOut = s.substr(7);
	}
	else
	{
		*colorOut = "";
		*textOut = s;
	}
}

static const char* DEACTIVATED_COLOR = "#4a6fa5"; // blue/grey - visually distinct from vanilla's dark red "Dead"

DataPanelLine* (*DatapanelGUI_setLine_KeyLastVisible_orig)(DatapanelGUI*, const std::string&, const std::string&, const std::string&, int, bool, bool);
DataPanelLine* DatapanelGUI_setLine_KeyLastVisible_hook(DatapanelGUI* self, const std::string& keyValue, const std::string& s1, const std::string& s2, int category, bool last, bool keyVisible)
{
	Character* target = self->getObject().getCharacter();
	if (!target || !g_deactivated.count(target))
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);

	// The "State:" row - value carries the color tag, key/s1 stay "State:".
	if (keyValue == "State:")
	{
		std::string color, text;
		splitColorTag(s2, &color, &text);
		if (toLowerCopy(text) == "dead")
		{
			std::string overriddenS2 = std::string(DEACTIVATED_COLOR) + "Deactivated";
			return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, overriddenS2, category, last, keyVisible);
		}
	}

	// The standalone all-caps banner - the word IS the key/s1, color-tagged, no separate s2.
	{
		std::string color, text;
		splitColorTag(keyValue, &color, &text);
		if (!color.empty() && s2.empty() && toLowerCopy(text) == "dead")
		{
			std::string overriddenKey = color + "DEACTIVATED";
			return DatapanelGUI_setLine_KeyLastVisible_orig(self, overriddenKey, overriddenKey, s2, category, last, keyVisible);
		}
	}

	return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);
}

// --- Loading ConversationOverrides from RE_Kenshi.json -----------------------------------------
// RE_Kenshi.json already sits next to this DLL in the mod folder and is already parsed with
// rapidjson by RE_Kenshi's own loader (Plugins.cpp, for "PreloadPlugins"/"Plugins") - reusing that
// same file/library here instead of a new config file or a new dependency. Extra top-level keys
// don't interfere with RE_Kenshi's own parsing (it only ever checks for specific expected keys), so
// adding "ConversationOverrides" alongside "Plugins" is safe.

// No existing KenshiLib helper resolves a plugin's own directory - Debug.cpp's GetModuleName()
// deliberately strips to just the base filename (confirmed by reading its implementation), which
// is useless here. Same underlying Win32 technique, keeping the directory instead of stripping it.
static std::string getOwnModDirectory()
{
	HMODULE module = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&getOwnModDirectory, &module))
		return "";

	char modulePath[MAX_PATH];
	if (GetModuleFileNameA(module, modulePath, sizeof(modulePath)) == 0)
		return "";

	std::string path(modulePath);
	size_t lastSlash = path.find_last_of("\\/");
	return lastSlash != std::string::npos ? path.substr(0, lastSlash + 1) : "";
}

// Shared by both loaders below - RE_Kenshi.json is only opened/parsed once per startPlugin() now,
// rather than once per JSON section consumed.
static bool loadOwnJsonDocument(rapidjson::Document& doc, const std::string& jsonPath)
{
	std::ifstream file(jsonPath.c_str());
	if (!file.is_open())
	{
		ErrorLog("SkeletonRebirth: could not open \"" + jsonPath + "\"");
		return false;
	}

	rapidjson::IStreamWrapper isw(file);
	if (doc.ParseStream(isw).HasParseError())
	{
		ErrorLog("SkeletonRebirth: JSON parse error in \"" + jsonPath + "\": " + rapidjson::GetParseError_En(doc.GetParseError()));
		return false;
	}

	return true;
}

static void loadConversationOverridesFromJson(const rapidjson::Document& doc, const std::string& jsonPath)
{
	if (!doc.HasMember("ConversationOverrides") || !doc["ConversationOverrides"].IsObject())
	{
		ErrorLog("SkeletonRebirth: \"" + jsonPath + "\" has no \"ConversationOverrides\" object - reactivation dialogue replies will do nothing");
		return;
	}

	const rapidjson::Value& conversationOverrides = doc["ConversationOverrides"];
	for (rapidjson::Value::ConstMemberIterator replyIt = conversationOverrides.MemberBegin(); replyIt != conversationOverrides.MemberEnd(); ++replyIt)
	{
		if (!replyIt->value.IsArray())
		{
			ErrorLog("SkeletonRebirth: ConversationOverrides[\"" + std::string(replyIt->name.GetString()) + "\"] is not an array - skipped");
			continue;
		}

		std::vector<ConversationOverride> overrides;
		const rapidjson::Value& overrideArray = replyIt->value;
		for (rapidjson::SizeType i = 0; i < overrideArray.Size(); ++i)
		{
			const rapidjson::Value& entry = overrideArray[i];
			if (!entry.HasMember("type") || !entry["type"].IsString())
				continue;

			ConversationOverride override;
			override.type = entry["type"].GetString();

			for (rapidjson::Value::ConstMemberIterator paramIt = entry.MemberBegin(); paramIt != entry.MemberEnd(); ++paramIt)
			{
				if (paramIt->value.IsString())
					override.params[paramIt->name.GetString()] = paramIt->value.GetString();
			}

			overrides.push_back(override);
		}

		g_conversationOverrides[replyIt->name.GetString()] = overrides;
	}

	std::ostringstream countMsg;
	countMsg << "SkeletonRebirth: loaded ConversationOverrides for " << g_conversationOverrides.size() << " repl(ies) from JSON";
	DebugLog(countMsg.str());
}

__declspec(dllexport) void startPlugin()
{
	g_overrideHandlers["reactivate_skeleton"] = &applyReactivateSkeletonOverride;

	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	rapidjson::Document doc;
	if (loadOwnJsonDocument(doc, jsonPath))
		loadConversationOverridesFromJson(doc, jsonPath);

	// CONFIRMED SAFE - each individually isolated via single-variable live testing (removed alone while
	// the rest of the plugin's hooks stayed active; crash still reproduced identically both times,
	// clearing both as suspects independent of one another).
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::declareDead), Character_declareDead_hook, &Character_declareDead_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add declareDead hook!");


	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate), MedicalSystem_medicalUpdate_hook, &MedicalSystem_medicalUpdate_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::medicalUpdate hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::updateOnScreenCheck), Character_updateOnScreenCheck_hook, &Character_updateOnScreenCheck_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Character::updateOnScreenCheck hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(int))&Dialogue::replyClicked), Dialogue_replyClickedInt_hook, &Dialogue_replyClickedInt_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(int) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(const std::string&))&Dialogue::replyClicked), Dialogue_replyClickedStr_hook, &Dialogue_replyClickedStr_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(string) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::update), Dialogue_update_hook, &Dialogue_update_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::update hook!");

	typedef DataPanelLine* (DatapanelGUI::*SetLineKeyLastVisibleFn)(const std::string&, const std::string&, const std::string&, int, bool, bool);
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetLineKeyLastVisibleFn)&DatapanelGUI::setLine), DatapanelGUI_setLine_KeyLastVisible_hook, &DatapanelGUI_setLine_KeyLastVisible_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add DatapanelGUI::setLine(key,last,visible) hook!");
}
