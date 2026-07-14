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
#include <kenshi/Inventory.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/ScreenLabel.h>
#include <kenshi/Dialogue.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/CharStats.h>
#include <kenshi/Globals.h>

#include <core/Functions.h>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/error/en.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
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
// before. Reactivation trigger, two more hooks: MedicalSystem::applyFirstAid() starts the FCS
// reactivation dialogue instead of reactivating immediately, and Dialogue::replyClicked() reads
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
// showReactivateDialogue()'s caller directly, rather than relying on stopping the treater's repair
// task at the right moment - applyFirstAid() re-fires many times per second for one continuous
// repair-kit-use action, and stopping the underlying AI task is inherently racy (it can re-fire again
// before the stop signal takes effect). This flag makes "only ask once per cycle" independent of that
// timing entirely.
static std::map<Character*, bool> g_reactivateDialogueShown;

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

// Resolves an FCS dialogue node's String ID to the DialogLineData tree startPlayerConversation()
// needs. FCS's dialogue editor tags every node DIALOGUE_LINE, never a standalone DIALOGUE - notification
// lines with no replies included. Only ever called with SR_REACTIVATE_DIALOGUE_ID (this mod's own
// trusted content); wrapped in SEH as defense-in-depth against a missing/renamed FCS node rather than
// a null-check, since a failed native lookup can fault outright instead of returning null. Kept free of
// local C++ objects requiring destruction (no std::string/std::vector locals) - MSVC C2712 (under this
// project's /EHsc build) disallows __try in a function that also needs C++ object unwinding, including
// inside the __except handler itself.
static DialogLineData* getFcsDialogLine(const std::string& stringID)
{
	__try
	{
		GameData* data = ou->gamedata.getData(stringID, DIALOGUE_LINE);
		if (!data)
			return nullptr;

		return DialogDataManager::getData(data);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// Callers are responsible for logging - see C2712 note above for why nothing can happen here.
		return nullptr;
	}
}

// Narrowed to just the mechanical state change - no notification text of its own anymore. Showing
// something on screen is now the generic, JSON-configured "show_text" conversation override (see
// below), decoupled from revival specifically so it's reusable by any future override-bearing reply.
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
// Yes/No). This is the one FCS ID that deliberately stays hardcoded rather than moving to JSON
// (see ConversationOverrides below) - starting the conversation in the first place, from the
// repair-kit-in-bed trigger, is inherent to this specific feature, unlike what happens on each
// reply afterward. FCS's own auto-assigned String ID (the "<index>-<mod filename>.mod" format, not
// a custom string) - if this dialogue node is ever deleted and recreated in FCS, FCS will likely
// assign a different ID and this constant will need updating to match (already happened once).
static const char* SR_REACTIVATE_DIALOGUE_ID = "24-Skeleton Rebirth.mod";

static DialogLineData* getReactivateDialogueRoot()
{
	DialogLineData* line = getFcsDialogLine(SR_REACTIVATE_DIALOGUE_ID);
	if (!line)
		ErrorLog(std::string("SkeletonRebirth: FCS dialogue '") + SR_REACTIVATE_DIALOGUE_ID + "' not found - has it been authored in FCS yet?");

	return line;
}

// initiator is the treater from applyFirstAid's "who" param - the one who actually used the repair
// kit, not just any player character (matters now that Yes takes an item from their inventory
// specifically - see handleDialogueReplyClicked()). Falls back to any player character only if
// initiator is somehow unavailable.
static void showReactivateDialogue(Character* patient, Character* initiator)
{
	if (!patient->dialogue)
		return;

	if (!patient->dialogue->conversationHasEnded())
		return; // already mid-conversation - don't stack a second start on top of it

	DialogLineData* root = getReactivateDialogueRoot();
	if (!root)
		return;

	Character* target = initiator ? initiator : (ou->player ? ou->player->getAnyPlayerCharacter() : nullptr);
	if (!target)
		return;

	patient->dialogue->startPlayerConversation(target, root);
}

// --- Hook: MedicalSystem::applyFirstAid() - real reactivation trigger ------------------------
// Confirmed via diagnostic logging: applyDoctoring() always co-fires with applyFirstAid() for the
// same treatment (identical timestamp, same item/state every time), so only one needs hooking.
// itemFunction == ITEM_ROBOTREPAIR is also the only value ever seen for robot patients - not a
// very selective filter on its own, but still the correct check to make.
//
// applyFirstAid() re-fires many times per second for the same continuous repair-kit-use action (the
// treater's AI keeps calling it every tick, since returning true here without running the real effect
// means the underlying task never sees its normal completion signal). Deliberately NOT called here at
// dialogue-open time - the repair animation stays playing while the confirmation is pending, so the
// treater visibly keeps working on the patient until reactivation actually happens (or the conversation
// ends without it - see the cleanup call in Dialogue_update_hook, which also guards against the
// dialogue immediately reopening after "No" the same way this used to). Notifies "who" (the treater,
// not the patient - the one whose AI is actually looping the action) via the AI task system's own
// natural completion signal, which ends the "Repairing" goal cleanly instead of wiping the task queue
// or marking it a failure.
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
		if (!g_reactivateDialogueShown.count(patient))
		{
			showReactivateDialogue(patient, who);
			g_reactivateDialogueShown[patient] = true;
		}
		return true; // report handled (not failed) so the repair-kit use doesn't keep retrying while the dialogue is up - see Dialogue_replyClicked_hook()
	}

	return MedicalSystem_applyFirstAid_orig(self, skill, equipment, frameTime, who);
}

// --- JSON-driven conversation overrides ---------------------------------------------------------
// What happens when a given FCS reply fires is data-driven (RE_Kenshi.json, "ConversationOverrides"
// key), not hardcoded per-feature - only *starting* the conversation in the first place
// (SR_REACTIVATE_DIALOGUE_ID above) stays in the DLL. Any FCS reply/line ID can be tagged with one
// or more named overrides; the plugin dispatches to whichever handlers are registered for that
// override's "type" whenever Dialogue::replyClicked reports that ID. Adding a new FCS-authored
// choice that reuses an existing override type (reactivate_skeleton/take_item/show_text) needs zero
// new C++ - just a JSON edit and a game restart.
struct ConversationOverride
{
	std::string type;                          // e.g. "reactivate_skeleton", "take_item", "show_text"
	std::map<std::string, std::string> params;  // e.g. {"item": "43392-changes_otto.mod"} - empty if none
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
// being called directly by name from handleDialogueReplyClicked. Stops the treater's repair animation
// exactly here, at the moment reactivation actually happens (see notifyTreaterActionComplete's
// comment) - not before, so a "delay" override placed ahead of this one in the JSON keeps the treater
// visibly working for the full delay instead of stopping early.
static bool applyReactivateSkeletonOverride(Character* patient, Character* initiator, DialogLineData* dialogueLine, const ConversationOverride& override)
{
	bool success = TryReactivate(patient);
	if (success)
	{
		notifyTreaterActionComplete(initiator);
		g_reactivateDialogueShown.erase(patient); // cycle concluded - a future fresh repair-kit-use may prompt again
	}
	return success;
}

// The item is identified by *its own* FCS ID directly (itemType::ITEM, not DIALOGUE_LINE, unlike
// every other lookup in this file) - not an indirect lookup through some dialogue line's hasItem
// field. FCS's own native hasItem dialogue condition already gates whether a reply offering this
// override is even clickable; this only handles actually removing the item, which FCS's condition
// doesn't do on its own.
// Item IDs come from JSON (user-supplied, unverified), so this is SEH-guarded the same way as
// getFcsDialogLine above - same C2712 reasoning applies, kept free of local C++ objects requiring
// destruction.
static GameData* getGameDataGuarded(const std::string& id, itemType category)
{
	__try
	{
		return ou->gamedata.getData(id, category);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// See getFcsDialogLine's comment - no std::string anywhere in this function, including here.
		return nullptr;
	}
}

static bool applyTakeItemOverride(Character* patient, Character* initiator, DialogLineData* dialogueLine, const ConversationOverride& override)
{
	std::string itemId = getParam(override, "item", "");
	if (itemId.empty() || !initiator)
		return false;

	GameData* itemData = getGameDataGuarded(itemId, ITEM);
	if (!itemData)
		return false;

	Inventory* inv = initiator->_NV_getInventory();
	if (!inv)
		return false;

	return inv->takeOneItemOnly(itemData) != nullptr;
}

// "#RRGGBB" or "RRGGBB" hex, the common web/paint-tool convention - not MyGUI's own native 0-1
// float range, which show_text's JSON authors shouldn't need to know about. Deliberately hand-rolled
// instead of MyGUI::Colour::parse()/the string constructor: their expected format isn't documented
// in the RE'd header and isn't worth reverse-engineering blind when a few lines of straightforward
// parsing here says exactly what format is accepted.
static bool tryParseHexColor(const std::string& value, MyGUI::Colour& outColor)
{
	std::string hex = (!value.empty() && value[0] == '#') ? value.substr(1) : value;
	if (hex.size() != 6)
		return false;

	for (size_t i = 0; i < hex.size(); ++i)
	{
		if (!isxdigit((unsigned char)hex[i]))
			return false;
	}

	int r = (int)strtol(hex.substr(0, 2).c_str(), NULL, 16);
	int g = (int)strtol(hex.substr(2, 2).c_str(), NULL, 16);
	int b = (int)strtol(hex.substr(4, 2).c_str(), NULL, 16);

	outColor = MyGUI::Colour(r / 255.0f, g / 255.0f, b / 255.0f);
	return true;
}

// "R,G,B" with each component 0-255 - kept alongside hex above as an alternative raw-RGB format.
static bool tryParseRgb(const std::string& value, MyGUI::Colour& outColor)
{
	size_t firstComma = value.find(',');
	size_t secondComma = firstComma != std::string::npos ? value.find(',', firstComma + 1) : std::string::npos;
	if (firstComma == std::string::npos || secondComma == std::string::npos)
		return false;

	int r = atoi(value.substr(0, firstComma).c_str());
	int g = atoi(value.substr(firstComma + 1, secondComma - firstComma - 1).c_str());
	int b = atoi(value.substr(secondComma + 1).c_str());

	if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
		return false;

	outColor = MyGUI::Colour(r / 255.0f, g / 255.0f, b / 255.0f);
	return true;
}

static MyGUI::Colour resolveNamedColor(const std::string& name)
{
	if (name == "Red")
		return MyGUI::Colour::Red;
	if (name == "Green")
		return MyGUI::Colour::Green;
	if (name == "Blue")
		return MyGUI::Colour::Blue;
	if (name == "Black")
		return MyGUI::Colour::Black;
	if (name == "White")
		return MyGUI::Colour::White;

	MyGUI::Colour parsed;
	if (tryParseHexColor(name, parsed) || tryParseRgb(name, parsed))
		return parsed;

	ErrorLog("SkeletonRebirth: unrecognized show_text color \"" + name + "\" - expected Red/Green/Blue/Black/White, \"#RRGGBB\" hex, or \"R,G,B\" (0-255) - defaulting to White");
	return MyGUI::Colour::White;
}

// Deliberately not Dialogue::insertWordSwaps() - that's native code meant to run only inside the
// engine's own live conversation flow (Character::dialogue is a per-character singleton, and calling
// it manually on a DialogLineData that isn't the character's actual current line risks leaving that
// singleton in a bad state). This is a small, self-contained substitute that touches nothing on the
// native Dialogue object - only handles /MYNAME/ (all this mod's content needs), not the full native
// wordswap tag vocabulary.
static void applySimpleWordSwaps(std::string& text, Character* patient)
{
	std::string name = patient->_NV_getName();
	size_t pos = 0;
	while ((pos = text.find("/MYNAME/", pos)) != std::string::npos)
	{
		text.replace(pos, 8, name);
		pos += name.size();
	}
}

// Floating text tracking the patient, same visual language as Kenshi's own stat-increase/pickup
// notifications - generic, not aware of revival at all. "string" supports /MYNAME/ via
// applySimpleWordSwaps() above (see its comment for why this isn't the native FCS word-swap
// mechanism anymore).
static bool applyShowTextOverride(Character* patient, Character* initiator, DialogLineData* dialogueLine, const ConversationOverride& override)
{
	std::string text = getParam(override, "string", "");
	if (text.empty())
		return false;

	applySimpleWordSwaps(text, patient);

	MyGUI::Colour color = resolveNamedColor(getParam(override, "color", "White"));

	ScreenLabel* label = gui->createScreenLabel(text, color, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
	if (label)
		label->setTracking(patient->getHandle(), Ogre::Vector3(0, 1, 0));

	return true;
}

// Dialogue::replyClicked can report both sides of a mutually-exclusive Yes/No choice for what the
// player experienced as a single click (a spurious report of the unclicked side, ~1.5s before the
// real one). Dispatching immediately on the first replyClicked seen would act on the wrong answer, so
// replies are buffered per-patient instead and only committed once Dialogue::update() (see below)
// detects the conversation has ended - trust the last reply reported before the conversation
// genuinely ends, not the first one seen.
static std::map<Character*, std::string> g_pendingReplyId;
static std::map<Character*, Character*> g_pendingInitiator;

// "delay" pauses the dispatch sequence itself rather than performing an action, so it's special-cased
// here instead of going through g_overrideHandlers - the handler interface has no way to say "stop
// and resume the rest of this list later." A paused sequence is resumed from Dialogue::update() (see
// below), which already runs every frame per-character regardless of whether a conversation is
// currently active, decrementing remainingSeconds until it reaches zero.
struct PendingOverrideSequence
{
	Character* initiator;
	std::string replyId;
	size_t nextIndex;      // index into g_conversationOverrides[replyId] to resume at
	float remainingSeconds;
};
static std::map<Character*, PendingOverrideSequence> g_pendingDelayedOverrides; // keyed by patient

static void dispatchConversationOverridesFrom(Character* patient, Character* initiator, const std::string& replyId, size_t startIndex)
{
	std::map<std::string, std::vector<ConversationOverride> >::iterator overridesIt = g_conversationOverrides.find(replyId);
	if (overridesIt == g_conversationOverrides.end())
		return;

	// dialogueLine isn't resolved here - no handler needs it (see the OverrideHandler typedef comment
	// above), so dispatch runs purely off the FCS String ID, never touching the underlying game data
	// object for content this plugin didn't author.
	const std::vector<ConversationOverride>& overrides = overridesIt->second;
	for (size_t i = startIndex; i < overrides.size(); ++i)
	{
		const ConversationOverride& override = overrides[i];

		if (override.type == "delay")
		{
			float seconds = (float)atof(getParam(override, "seconds", "0").c_str());
			if (seconds > 0.0f)
			{
				PendingOverrideSequence pending;
				pending.initiator = initiator;
				pending.replyId = replyId;
				pending.nextIndex = i + 1;
				pending.remainingSeconds = seconds;
				g_pendingDelayedOverrides[patient] = pending;
				return; // remaining overrides resume later, see Dialogue_update_hook
			}
			continue;
		}

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

static void dispatchConversationOverrides(Character* patient, Character* initiator, const std::string& replyId)
{
	if (!g_conversationOverrides.count(replyId))
	{
		DebugLog("SkeletonRebirth: no ConversationOverrides configured for reply \"" + replyId + "\" - nothing to do");
		return;
	}

	dispatchConversationOverridesFrom(patient, initiator, replyId, 0);
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
//
// Also drives resuming a "delay"-paused override sequence (see dispatchConversationOverridesFrom) -
// this fires regardless of conversation state, so a delay keeps ticking down even after the dialogue
// that triggered it has closed. Same cheap-gate shape: a map lookup for patients with nothing pending
// costs nothing further.
void (*Dialogue_update_orig)(Dialogue*, float);
void Dialogue_update_hook(Dialogue* self, float frameTime)
{
	Character* patient = self->me;
	bool hasPending = patient && g_pendingReplyId.count(patient) != 0;

	Dialogue_update_orig(self, frameTime);

	if (patient)
	{
		std::map<Character*, PendingOverrideSequence>::iterator delayIt = g_pendingDelayedOverrides.find(patient);
		if (delayIt != g_pendingDelayedOverrides.end())
		{
			delayIt->second.remainingSeconds -= frameTime;
			if (delayIt->second.remainingSeconds <= 0.0f)
			{
				PendingOverrideSequence pending = delayIt->second;
				g_pendingDelayedOverrides.erase(delayIt);
				dispatchConversationOverridesFrom(patient, pending.initiator, pending.replyId, pending.nextIndex);
			}
		}
	}

	if (!hasPending || !self->conversationHasEnded())
		return;

	std::string replyId = g_pendingReplyId[patient];
	Character* initiator = g_pendingInitiator.count(patient) ? g_pendingInitiator[patient] : nullptr;
	g_pendingReplyId.erase(patient);
	g_pendingInitiator.erase(patient);

	DebugLog("SkeletonRebirth: conversation ended for " + patient->_NV_getName() + " - committing last-seen reply \"" + replyId + "\"");
	dispatchConversationOverrides(patient, initiator, replyId);

	// If dispatch didn't reactivate the patient (e.g. "No", or a reply with no reactivate_skeleton
	// override at all) and isn't mid-delay toward doing so, this "ask and await an answer" cycle is
	// over: stop the treater's repair animation, and clear g_reactivateDialogueShown so a genuinely
	// new repair-kit-use later can prompt again (MedicalSystem_applyFirstAid_hook is what actually
	// guards against the dialogue reopening immediately - this is no longer relying on the AI task's
	// stop timing for that).
	if (g_deactivated.count(patient) && !g_pendingDelayedOverrides.count(patient))
	{
		notifyTreaterActionComplete(initiator);
		g_reactivateDialogueShown.erase(patient);
	}
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

// --- Hook: DialogLineData::checkConditions() - JSON-driven skill-gated dialogue choices --------
// FCS's own dialogue conditions (DialogConditionEnum, Enums.h:796) have no skill-check type at
// all - no way to author "only show this reply if Science is 80+" in FCS itself. checkConditions()
// is the general eligibility gate FCS calls for every dialogue line, including player reply choices
// (DialogLineData::getPlayerReplies() relies on it the same way NPC line selection does), so hooking
// it here extends that same native gate instead of reimplementing reply-list filtering separately.
//
// Only ever turns a true into false: the original result is checked first and returned as-is
// whenever it's already false or nothing is configured for this line, so lines nobody's tagged -
// which is almost every dialogue line in the game, since this hook fires for all of them - see zero
// added behavior. Config lives in RE_Kenshi.json under "DialogueSkillChecks", keyed by FCS reply/line
// String ID the same way "ConversationOverrides" is, e.g.:
//   "DialogueSkillChecks": { "15-Some Mod.mod": [ { "skill": "science", "min": 80 } ] }
// Every check for a line must pass (AND) for the line to stay visible. "skill" is a lowercase
// CharStats field name (see g_skillFields below) - not necessarily identical to the skill's in-game
// display label. "min"/"max" are both optional (at least one required); values are Kenshi's native
// 0-100 skill scale.
struct DialogueSkillCheck
{
	std::string skill; // lowercase CharStats field name, e.g. "science" - see g_skillFields
	bool hasMin;
	float min;
	bool hasMax;
	float max;
};
static std::map<std::string, std::vector<DialogueSkillCheck> > g_dialogueSkillChecks; // keyed by FCS reply/line String ID

// Deliberately only the plain "skill" floats (the ones under the Skills tab in-game), not attributes
// like strength/dexterity/toughness - those are derived through accessor methods rather than plain
// settable members on some builds, and aren't what "skill check" means in the requested feature.
// Keys are the CharStats member name lowercased, not Kenshi's in-game display label, to keep the
// mapping unambiguous against the header this was written from (CharStats.h).
static std::map<std::string, float CharStats::*> buildSkillFieldTable()
{
	std::map<std::string, float CharStats::*> table;
	table["medic"] = &CharStats::medic;
	table["masscombat"] = &CharStats::massCombat;
	table["arrowdefence"] = &CharStats::arrowDefence;
	table["stealth"] = &CharStats::stealth;
	table["swimming"] = &CharStats::swimming;
	table["thieving"] = &CharStats::thieving;
	table["lockpicking"] = &CharStats::lockpicking;
	table["bluff"] = &CharStats::bluff;
	table["assassin"] = &CharStats::assassin;
	table["survival"] = &CharStats::survival;
	table["tracking"] = &CharStats::tracking;
	table["climbing"] = &CharStats::climbing;
	table["doctor"] = &CharStats::doctor;
	table["engineer"] = &CharStats::engineer;
	table["weaponsmith"] = &CharStats::weaponSmith;
	table["armoursmith"] = &CharStats::armourSmith;
	table["bowsmith"] = &CharStats::bowSmith;
	table["robotics"] = &CharStats::robotics;
	table["science"] = &CharStats::science;
	table["labouring"] = &CharStats::labouring;
	table["farming"] = &CharStats::farming;
	table["cooking"] = &CharStats::cooking;
	table["dodging"] = &CharStats::dodging;
	table["friendlyfire"] = &CharStats::friendlyFire;
	table["katanas"] = &CharStats::katanas;
	table["sabres"] = &CharStats::sabres;
	table["hackers"] = &CharStats::hackers;
	table["blunt"] = &CharStats::blunt;
	table["heavyweapons"] = &CharStats::heavyWeapons;
	table["unarmed"] = &CharStats::unarmed;
	table["bows"] = &CharStats::bows;
	table["turrets"] = &CharStats::turrets;
	table["polearms"] = &CharStats::polearms;
	return table;
}
static const std::map<std::string, float CharStats::*> g_skillFields = buildSkillFieldTable();

static std::string toLower(const std::string& s)
{
	std::string out = s;
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

// Unknown skill names are logged once at JSON-load time (see loadDialogueSkillChecksFromJson) and
// simply skipped here rather than failing the whole line closed - a typo in one check out of several
// on the same reply shouldn't hide a reply whose other checks are all fine.
static bool evaluateSkillChecks(const std::vector<DialogueSkillCheck>& checks, CharStats* stats)
{
	for (size_t i = 0; i < checks.size(); ++i)
	{
		const DialogueSkillCheck& check = checks[i];

		std::map<std::string, float CharStats::*>::const_iterator fieldIt = g_skillFields.find(check.skill);
		if (fieldIt == g_skillFields.end())
			continue;

		float value = stats->*(fieldIt->second);
		if (check.hasMin && value < check.min)
			return false;
		if (check.hasMax && value > check.max)
			return false;
	}
	return true;
}

bool (*DialogLineData_checkConditions_orig)(DialogLineData*, Dialogue*, Character*, bool);
bool DialogLineData_checkConditions_hook(DialogLineData* self, Dialogue* dialog, Character* target, bool isWordswap)
{
	bool result = DialogLineData_checkConditions_orig(self, dialog, target, isWordswap);

	// Cheapest possible no-op path for the overwhelming majority of calls (every dialogue line in the
	// entire game, not just tagged ones): skip getStringID() and the map lookup entirely whenever the
	// original already said no, or nobody's configured any DialogueSkillChecks at all.
	if (!result || g_dialogueSkillChecks.empty())
		return result;

	std::string stringId = self->getStringID();
	std::map<std::string, std::vector<DialogueSkillCheck> >::iterator it = g_dialogueSkillChecks.find(stringId);
	if (it == g_dialogueSkillChecks.end())
		return true;

	// Skill-gated choices are always evaluated against the player, regardless of which Character
	// object dialog/target resolve to for this particular line - a reply choice is something only the
	// player ever picks, so it's the player's skill that's meant to gate it.
	Character* player = ou->player ? ou->player->getAnyPlayerCharacter() : nullptr;
	if (!player || !player->stats)
	{
		DebugLog("SkeletonRebirth: DialogueSkillChecks - no player CharStats available to evaluate \"" + stringId + "\" - hiding");
		return false; // can't verify eligibility for a gated choice - fail closed, don't show it
	}

	return evaluateSkillChecks(it->second, player->stats);
}

// --- Loading ConversationOverrides and DialogueSkillChecks from RE_Kenshi.json -----------------
// RE_Kenshi.json already sits next to this DLL in the mod folder and is already parsed with
// rapidjson by RE_Kenshi's own loader (Plugins.cpp, for "PreloadPlugins"/"Plugins") - reusing that
// same file/library here instead of a new config file or a new dependency. Extra top-level keys
// don't interfere with RE_Kenshi's own parsing (it only ever checks for specific expected keys), so
// adding "ConversationOverrides"/"DialogueSkillChecks" alongside "Plugins" is safe.

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

static void loadDialogueSkillChecksFromJson(const rapidjson::Document& doc, const std::string& jsonPath)
{
	if (!doc.HasMember("DialogueSkillChecks") || !doc["DialogueSkillChecks"].IsObject())
	{
		DebugLog("SkeletonRebirth: \"" + jsonPath + "\" has no \"DialogueSkillChecks\" object - dialogue skill gating disabled");
		return;
	}

	const rapidjson::Value& skillChecks = doc["DialogueSkillChecks"];
	for (rapidjson::Value::ConstMemberIterator lineIt = skillChecks.MemberBegin(); lineIt != skillChecks.MemberEnd(); ++lineIt)
	{
		std::string lineId = lineIt->name.GetString();
		if (!lineIt->value.IsArray())
		{
			ErrorLog("SkeletonRebirth: DialogueSkillChecks[\"" + lineId + "\"] is not an array - skipped");
			continue;
		}

		std::vector<DialogueSkillCheck> checks;
		const rapidjson::Value& checkArray = lineIt->value;
		for (rapidjson::SizeType i = 0; i < checkArray.Size(); ++i)
		{
			const rapidjson::Value& entry = checkArray[i];
			if (!entry.HasMember("skill") || !entry["skill"].IsString())
			{
				ErrorLog("SkeletonRebirth: DialogueSkillChecks[\"" + lineId + "\"] has an entry with no \"skill\" string - skipped");
				continue;
			}

			DialogueSkillCheck check;
			check.skill = toLower(entry["skill"].GetString());
			check.hasMin = entry.HasMember("min") && entry["min"].IsNumber();
			check.min = check.hasMin ? entry["min"].GetFloat() : 0.0f;
			check.hasMax = entry.HasMember("max") && entry["max"].IsNumber();
			check.max = check.hasMax ? entry["max"].GetFloat() : 0.0f;

			if (!check.hasMin && !check.hasMax)
			{
				ErrorLog("SkeletonRebirth: DialogueSkillChecks[\"" + lineId + "\"] entry for skill \"" + check.skill + "\" has neither \"min\" nor \"max\" - skipped");
				continue;
			}

			if (!g_skillFields.count(check.skill))
				ErrorLog("SkeletonRebirth: DialogueSkillChecks[\"" + lineId + "\"] references unknown skill \"" + check.skill + "\" - that check will never gate anything");

			checks.push_back(check);
		}

		if (!checks.empty())
			g_dialogueSkillChecks[lineId] = checks;
	}

	std::ostringstream countMsg;
	countMsg << "SkeletonRebirth: loaded DialogueSkillChecks for " << g_dialogueSkillChecks.size() << " line(s) from JSON";
	DebugLog(countMsg.str());
}

__declspec(dllexport) void startPlugin()
{
	g_overrideHandlers["reactivate_skeleton"] = &applyReactivateSkeletonOverride;
	g_overrideHandlers["take_item"] = &applyTakeItemOverride;
	g_overrideHandlers["show_text"] = &applyShowTextOverride;

	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	rapidjson::Document doc;
	if (loadOwnJsonDocument(doc, jsonPath))
	{
		loadConversationOverridesFromJson(doc, jsonPath);
		loadDialogueSkillChecksFromJson(doc, jsonPath);
	}

	// CONFIRMED SAFE - each individually isolated via single-variable live testing (removed alone while
	// the rest of the plugin's hooks stayed active; crash still reproduced identically both times,
	// clearing both as suspects independent of one another).
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::declareDead), Character_declareDead_hook, &Character_declareDead_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add declareDead hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyFirstAid), MedicalSystem_applyFirstAid_hook, &MedicalSystem_applyFirstAid_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::applyFirstAid hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate), MedicalSystem_medicalUpdate_hook, &MedicalSystem_medicalUpdate_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::medicalUpdate hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(int))&Dialogue::replyClicked), Dialogue_replyClickedInt_hook, &Dialogue_replyClickedInt_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(int) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(Dialogue::*)(const std::string&))&Dialogue::replyClicked), Dialogue_replyClickedStr_hook, &Dialogue_replyClickedStr_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::replyClicked(string) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::update), Dialogue_update_hook, &Dialogue_update_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Dialogue::update hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::checkConditions), DialogLineData_checkConditions_hook, &DialogLineData_checkConditions_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add DialogLineData::checkConditions hook!");
}
