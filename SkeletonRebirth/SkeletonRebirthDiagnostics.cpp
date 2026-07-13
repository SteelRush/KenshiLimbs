#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/util/hand.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Item.h>
#include <kenshi/Building/Building.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/ScreenLabel.h>
#include <kenshi/Dialogue.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Globals.h>

#include <core/Functions.h>

#include <cmath>
#include <sstream>
#include <map>
#include <string>

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
// before. Reactivation trigger, two more hooks: MedicalSystem::applyFirstAid() starts the FCS
// reactivation dialogue instead of reactivating immediately, and Dialogue::replyClicked() reads
// back which reply the player picked.
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

// Edge-detects the AI goal transitioning into "Repairing" (see getCurrentGoalStringSafe() below),
// so MedicalSystem_applyFirstAid_hook only opens the reactivation dialogue once per continuous
// repair-kit-use action rather than every single tick it re-fires for - see that hook for why.
static std::map<Character*, bool> g_wasRepairingLastTick;

static std::string describePart(MedicalSystem* med, MedicalSystem::HealthPartStatus::PartType type, const char* label)
{
	if (!med)
		return std::string(label) + "=<no medical>";

	MedicalSystem::HealthPartStatus* part = med->getPart(type, SIDE_NEITHER);
	if (!part)
		return std::string(label) + "=<none>";

	std::ostringstream ss;
	ss << label << "(flesh=" << part->flesh << " fatal=" << part->fatal << " isDead=" << part->isDead() << ")";
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
	   << " " << describePart(med, MedicalSystem::HealthPartStatus::PART_HEAD, "head")
	   << " " << describePart(med, MedicalSystem::HealthPartStatus::PART_TORSO, "torso")
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
// Fires right before a character would actually die. For robots, blocks the original call and
// clears MedicalSystem::dead - the GUI/party-membership logic reads that flag directly, not
// whether declareDead() ran. Doesn't touch flesh/part health at all. TryReactivate() below is the
// only thing that ever clears g_deactivated.

void (*Character_declareDead_orig)(Character*);
void Character_declareDead_hook(Character* self)
{
	if (isRobotRace(self))
	{
		MedicalSystem* med = self->getMedical();
		if (med)
			med->dead = false;

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
// character straight back into Deactivated. A 1% nudge of each part's flesh toward zero (tested
// and confirmed correct: enough to clear whatever makes isDead() stick, not a full heal - the
// character wakes up critically hurt and needs immediate treatment, which is the intended
// difficulty) avoids that without granting free healing.
static void nudgeFleshTowardSurvivable(MedicalSystem::HealthPartStatus* part)
{
	if (part)
		part->flesh += std::fabs(part->flesh) * 0.01f;
}

// Shared by every lookup of an FCS-authored dialogue node in this mod - confirmed via live testing
// (see the comment above SR_REACTIVATE_DIALOGUE_ID below) that FCS's dialogue editor never creates
// a standalone itemType::DIALOGUE object; every node, including plain notification-text lines with
// no replies of their own, is tagged DIALOGUE_LINE.
static DialogLineData* getFcsDialogLine(const char* stringID)
{
	GameData* data = ou->gamedata.getData(stringID, DIALOGUE_LINE);
	if (!data)
		return nullptr;

	return DialogDataManager::getData(data);
}

// FCS String ID for the revival notification's text (FCS's own auto-assigned
// "<index>-<mod filename>.mod" format, same situation as the constants below).
static const char* SR_REACTIVATE_REVIVED_MESSAGE_ID = "18-Skeleton Rebirth.mod";

// Not yet live-tested: insertWordSwaps() resolves FCS placeholders like /MYNAME/ (seen in this
// mod's own dialogue text) against a target Character - using self as both speaker and target
// here, since this notification isn't part of an active conversation with a real player-facing
// speaker, just a system message about self. No fallback text - if the FCS line isn't authored
// yet, the notification is simply skipped (see TryReactivate()), same as
// getReactivateDialogueRoot()'s handling of a missing dialogue.
static std::string getReactivateRevivedMessage(Character* self)
{
	DialogLineData* line = getFcsDialogLine(SR_REACTIVATE_REVIVED_MESSAGE_ID);
	if (!line)
	{
		ErrorLog(std::string("SkeletonRebirth: FCS dialogue line '") + SR_REACTIVATE_REVIVED_MESSAGE_ID + "' not found - has it been authored in FCS yet?");
		return "";
	}

	std::string text = line->getText(false);
	if (self->dialogue)
		self->dialogue->insertWordSwaps(text, self, true, line);

	return text;
}

bool TryReactivate(Character* self)
{
	auto deactivatedIt = g_deactivated.find(self);
	if (deactivatedIt == g_deactivated.end() || !deactivatedIt->second)
		return false; // not actually Deactivated - nothing to do

	MedicalSystem* med = self->getMedical();
	if (med)
	{
		nudgeFleshTowardSurvivable(med->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER));
		nudgeFleshTowardSurvivable(med->getPart(MedicalSystem::HealthPartStatus::PART_TORSO, SIDE_NEITHER));
	}

	g_deactivated.erase(self);
	DebugLog("SkeletonRebirth: TryReactivate() SUCCEEDED -> " + describe(self));

	// Floating green text tracking the character, same visual language as Kenshi's own
	// stat-increase/pickup notifications - deliberately not a modal popup, since the dialogue
	// exchange that led here already got the player's attention. Text is FCS-authored (linked via
	// SR_REACTIVATE_REVIVED_MESSAGE_ID) rather than hardcoded, so it stays consistent with the rest
	// of the dialogue's wording and can be edited/translated in FCS without a rebuild.
	std::string message = getReactivateRevivedMessage(self);
	if (!message.empty())
	{
		ScreenLabel* label = gui->createScreenLabel(message, MyGUI::Colour::Green, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
		if (label)
			label->setTracking(self->getHandle(), Ogre::Vector3(0, 1, 0));
	}

	return true;
}

// Confirmation prompt shown instead of reactivating immediately - this is meant to be a deliberate
// choice, not an automatic side effect of using a repair kit. An earlier version built this as a
// hand-rolled MyGUI panel (ForgottenGUI::createPanel()/createButton()/createLabel()) because a
// first attempt at the FCS dialogue system - Dialogue::runCustomDialog()/sendEvent() - only ever
// narrated straight through with no pause for choice, or was silently rejected. Neither of those is
// the real "open an interactive conversation" entry point, though: Dialogue::startPlayerConversation()
// is - it's labelled "private" in the RE'd header purely as a documentation note about the original
// game code (KenshiLib deliberately leaves every member public in real C++ terms, and the linked
// KenshiLib.lib exports a real thunk for it - see Source/kenshi/functions/Dialogue.inc), so it's
// directly callable like any other method here. Using it is the actual "let the standard dialogue
// system take it from here" mechanism, instead of hand-building UI.
//
// Requires matching content authored in FCS under this mod - already done: a Dialogue-type
// GameData object (the "Attempt to reactivate <character>?" line with two player-reply children,
// Yes/No) and its "Yes" reply line. Both IDs below are FCS's own auto-assigned String IDs (FCS's
// default "<index>-<mod filename>.mod" format, not custom strings we picked) - if either dialogue
// node is ever deleted and recreated in FCS, or the mod file is renamed, FCS will likely assign a
// different ID and these constants will need updating to match. The "No" reply needs no special ID
// - simply not matching Yes is enough to do nothing.
static const char* SR_REACTIVATE_DIALOGUE_ID = "11-Skeleton Rebirth.mod";
static const char* SR_REACTIVATE_YES_REPLY_ID = "12-Skeleton Rebirth.mod";

static DialogLineData* getReactivateDialogueRoot()
{
	DialogLineData* line = getFcsDialogLine(SR_REACTIVATE_DIALOGUE_ID);
	if (!line)
		ErrorLog(std::string("SkeletonRebirth: FCS dialogue '") + SR_REACTIVATE_DIALOGUE_ID + "' not found - has it been authored in FCS yet?");

	return line;
}

static void showReactivateDialogue(Character* patient)
{
	if (!patient->dialogue)
		return;

	if (!patient->dialogue->conversationHasEnded())
		return; // already mid-conversation - don't stack a second start on top of it

	DialogLineData* root = getReactivateDialogueRoot();
	if (!root)
		return;

	Character* playerChar = ou->player ? ou->player->getAnyPlayerCharacter() : nullptr;
	if (!playerChar)
		return;

	patient->dialogue->startPlayerConversation(playerChar, root);
}

// --- Hook: MedicalSystem::applyFirstAid() - real reactivation trigger ------------------------
// Confirmed via diagnostic logging: applyDoctoring() always co-fires with applyFirstAid() for the
// same treatment (identical timestamp, same item/state every time), so only one needs hooking.
// itemFunction == ITEM_ROBOTREPAIR is also the only value ever seen for robot patients - not a
// very selective filter on its own, but still the correct check to make.
//
// applyFirstAid() re-fires many times per second for the same continuous repair-kit-use action
// (the treater's AI keeps calling it every tick, since returning true here without ever running
// the real effect means the underlying task never sees its normal completion signal). Live testing
// confirmed this actually breaks "No": TryReactivate() (on "Yes") removes the patient from
// g_deactivated, so later calls stop matching the trigger condition on their own - but "No" leaves
// the patient in g_deactivated, so the very next applyFirstAid() tick sees the dialogue already
// ended and pops a fresh one right back open. Two earlier, different attempts at stopping the
// repair *action* itself (OrdersReceiver::removeJob(JOB_REPAIR_ROBOT), AITaskSytem::clearAllTasks())
// were tried and reverted (see DESIGN.md) - neither stopped goalString from staying "Repairing",
// and clearAllTasks() once left the (then-MyGUI) panel stuck. Not yet live-tested: notifying "who"
// (the treater, not the patient - the one whose AI is actually looping this action) that its
// current body task is complete, via the AI task system's own natural completion signal rather
// than wiping its task queue, on the theory that this ends the "Repairing" goal cleanly instead of
// leaving it stuck or blacklisting it as failed (unlike taskImpossible(), not used here for that
// reason).
static void notifyTreaterActionComplete(Character* who)
{
	if (!who)
		return;

	AI* ai = who->getAI();
	AITaskSytem* taskSystem = ai ? ai->getTaskSystem() : nullptr;
	if (taskSystem)
		taskSystem->_notifyBodyTaskComplete();
}

bool (*MedicalSystem_applyFirstAid_orig)(MedicalSystem*, float, Item*, float, Character*);
bool MedicalSystem_applyFirstAid_hook(MedicalSystem* self, float skill, Item* equipment, float frameTime, Character* who)
{
	MedicalSystem::HealthPartStatus* head = self->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER);
	Character* patient = head ? head->me : nullptr;

	if (patient && g_deactivated.count(patient) && equipment && equipment->itemFunction == ITEM_ROBOTREPAIR && isInSkeletonBed(patient))
	{
		showReactivateDialogue(patient);
		notifyTreaterActionComplete(who);
		return true; // report handled (not failed) so the repair-kit use doesn't keep retrying while the dialogue is up - see Dialogue_replyClicked_hook()
	}

	return MedicalSystem_applyFirstAid_orig(self, skill, equipment, frameTime, who);
}

// --- Hook: Dialogue::replyClicked(...) - reads the reactivation dialogue's outcome -------------
// The standard dialogue system handles the actual conversation UI end-to-end once
// startPlayerConversation() has started it (see showReactivateDialogue() above); this hook is only
// how the plugin finds out which reply the player picked. Dialogue::replyClicked is overloaded
// (int and const std::string&, Dialogue.h:378-379) - live testing confirmed both fire for a single
// click (string first, then int at the same timestamp - the int overload looks like an internal
// wrapper around the string one), so both are hooked, but only the string one is used for
// detection. Originally checked self->currentLine->getStringID() *after* calling the real
// replyClicked(), but live testing showed currentLine is already null by the time the hook
// resumes - our Yes/No replies are terminal (no further lines), so the conversation ends and
// clears state inside replyClicked() itself before the hook gets to look at it. Switched to using
// the string overload's own "index" parameter directly instead, on the hypothesis that it already
// *is* the clicked reply's FCS String ID (matching Dialogue::replyIds, a vector<std::string>) -
// not yet confirmed live; logs the raw value unconditionally so the next test either confirms this
// or shows what it actually contains.
static void handleDialogueReplyClicked(Dialogue* self, const std::string& replyId, const char* fromOverload)
{
	Character* patient = self->me;
	if (!patient || !g_deactivated.count(patient))
		return;

	DebugLog("SkeletonRebirth: replyClicked(" + std::string(fromOverload) + ") fired -> patient=" + patient->_NV_getName() + " reply=\"" + replyId + "\"");

	if (replyId == SR_REACTIVATE_YES_REPLY_ID)
	{
		if (TryReactivate(patient))
			DebugLog("SkeletonRebirth: repair kit used in Skeleton Bed -> " + describe(patient));
	}
}

// index here is a position into Dialogue::replyIds, not itself a reply's String ID - resolved
// before calling the original in case state gets cleared the same way currentLine did.
void (*Dialogue_replyClickedInt_orig)(Dialogue*, int);
void Dialogue_replyClickedInt_hook(Dialogue* self, int index)
{
	std::string replyId = (index >= 0 && (size_t)index < self->replyIds.size()) ? self->replyIds[index] : "<index out of range>";
	Dialogue_replyClickedInt_orig(self, index);
	handleDialogueReplyClicked(self, replyId, "int");
}

void (*Dialogue_replyClickedStr_orig)(Dialogue*, const std::string&);
void Dialogue_replyClickedStr_hook(Dialogue* self, const std::string& index)
{
	Dialogue_replyClickedStr_orig(self, index);
	handleDialogueReplyClicked(self, index, "string");
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

__declspec(dllexport) void startPlugin()
{
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::declareDead), Character_declareDead_hook, &Character_declareDead_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add declareDead hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate), MedicalSystem_medicalUpdate_hook, &MedicalSystem_medicalUpdate_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::medicalUpdate hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyFirstAid), MedicalSystem_applyFirstAid_hook, &MedicalSystem_applyFirstAid_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::applyFirstAid hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(int))&Dialogue::replyClicked), Dialogue_replyClickedInt_hook, &Dialogue_replyClickedInt_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(int) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(const std::string&))&Dialogue::replyClicked), Dialogue_replyClickedStr_hook, &Dialogue_replyClickedStr_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(string) hook!");
}
