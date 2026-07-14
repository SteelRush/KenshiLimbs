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
//
// CONFIRMED CRASH, wrapped in SEH: ConversationOverrides can reference IDs from anywhere in the
// game (stock dialogue included, not just this mod's own FCS content - confirmed live), and this
// plugin has no way to validate an ID is safe before looking it up. Live-tested: a specific stock
// dialogue ID (55811-Dialogue.mod) crashed the game inside this lookup outright - a real access
// violation, not a graceful null return - even after narrowing which reply IDs trigger this call at
// all (see handleDialogueReplyClicked). No null-check on our side after the call can help when the
// fault happens inside it, so __try/__except converts the crash into a logged failure instead.
// Kept deliberately free of local C++ objects requiring destruction (no std::string/std::vector
// locals) - MSVC (C2712, under this project's /EHsc build) won't allow __try in a function that
// also needs C++ object unwinding.
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
		// No std::string (or any C++ object requiring destruction) anywhere in this function, not
		// even here in the handler - MSVC's C2712 rejects __try/__except in a function that needs
		// C++ object unwinding *anywhere* in its body under /EHsc, not just inside the __try block
		// itself (confirmed by hitting this exact error with the logging call that used to be
		// here). Callers are responsible for logging - see getFcsDialogLine's callers.
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
		showReactivateDialogue(patient, who);
		notifyTreaterActionComplete(who);
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

// dialogueLine is currently always nullptr - see the CONFIRMED CRASH comment in
// dispatchConversationOverrides for why this plugin stopped resolving DialogLineData objects for
// arbitrary (potentially stock/vanilla) reply IDs at all. Kept in the signature so a future override
// type that genuinely needs it can add SEH-guarded resolution scoped to just that handler, without
// forcing every dispatch (including ones that never touch it) back into the native lookup.
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
	return TryReactivate(patient);
}

// The item is identified by *its own* FCS ID directly (itemType::ITEM, not DIALOGUE_LINE, unlike
// every other lookup in this file) - not an indirect lookup through some dialogue line's hasItem
// field. FCS's own native hasItem dialogue condition already gates whether a reply offering this
// override is even clickable; this only handles actually removing the item, which FCS's condition
// doesn't do on its own.
// Same SEH reasoning as getFcsDialogLine above (see its comment) - item IDs in ConversationOverrides
// are equally user/JSON-supplied and unverified, so equally capable of crashing the game outright
// inside the native lookup. Kept free of local C++ objects requiring destruction for the same
// MSVC C2712 reason - __try cannot share a function with locals needing unwinding under /EHsc.
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

// CONFIRMED ROOT CAUSE - Dialogue::insertWordSwaps() is not safe to call outside the native engine's
// own controlled conversation flow. Live-tested and reported directly: once any ConversationOverride
// touched one stock dialogue line for a character, *all* stock dialogue for that same character
// started crashing, not just the configured line - "like it's corrupting an internal reference".
// Character::dialogue is a per-character singleton, reused for every conversation that character
// ever has; insertWordSwaps() is native code that almost certainly mutates internal Dialogue state
// as a side effect (this project's own header lists fields like currentLine, locked, _hasChanceLines
// that plausibly get touched). Calling it manually, outside the live conversation the engine itself
// is managing, with a DialogLineData that isn't the character's actual current line, most likely
// left that per-character Dialogue object in an inconsistent state rather than crashing immediately
// - explaining why it only broke *future* native conversations with that same character, not the
// moment of the call itself. The earlier SEH guard around this call could only catch an immediate
// hard fault, not corrupted-but-still-"valid" state left behind afterward - the right fix is to
// never call it this way at all, not to make the call safer. Replaced with a small, self-contained,
// pure-C++ substitution below that touches nothing on the native Dialogue object - only handles
// /MYNAME/ (all this mod's own content ever needed), not the full native wordswap tag vocabulary,
// but that tradeoff is clearly worth it against corrupting a character's dialogue permanently.
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

// CONFIRMED BUG, FIXED: Dialogue::replyClicked can report BOTH sides of a mutually-exclusive
// Yes/No choice for what the player experienced as a single click - live-tested and confirmed
// directly by the user: clicking only "No" still logged "Yes" firing (and reactivating!) about 1.5
// seconds before "No" itself fired. Dispatching immediately on the first replyClicked seen was
// therefore acting on the wrong answer. Fixed by buffering the most recent reply per-patient instead
// of dispatching straight away, and only committing it once Dialogue::update() (see below) detects
// the conversation has ended - i.e. trust the LAST reply reported before the conversation genuinely
// ends, not the first one seen. Not yet confirmed whether the earlier "Yes" report is the dialogue system
// pre-evaluating/scoring candidate lines, a hover-triggered call, or something else - the fix here
// doesn't depend on knowing why, only on waiting for a definitive end-of-conversation signal.
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

	// CONFIRMED CRASH, FIXED (round 3): dialogueLine used to be resolved here via getFcsDialogLine()
	// and passed to every handler "just in case" - but none of the three handlers actually read it
	// (show_text stopped needing it once it moved off the native insertWordSwaps() path - see
	// applySimpleWordSwaps's comment). Live-tested and confirmed: attaching a show_text override to a
	// *stock* dialogue reply crashed the game hard (access violation) every time that reply fired,
	// even with the resolution itself SEH-guarded - crash dump analysis (two independent captures,
	// identical fault address both times) placed the actual fault inside msvcr100.dll's memcpy/memmove,
	// not in our own guarded lookup call. Root cause: DialogLineData::getName()/getText() return
	// std::string *by value* across the native ABI (the same hidden-pointer-return hazard already
	// flagged elsewhere in this file) - safe for this mod's own dialogue tree (exercised the same way
	// by the vanilla game every time it runs), but for arbitrary stock content the returned buffer was
	// getting corrupted somewhere inside that native call, and the corruption only surfaced later, once
	// something (originally: word-swaps; still, after removing those: this file's own diagnostic
	// describeDialogLine() concatenating the returned strings into a log line) copied it. SEH around
	// the resolution *call* couldn't catch this because the fault didn't happen there - it happened
	// later, during string construction/copying, arbitrarily far from any __try. Since nothing here
	// actually needs the DialogLineData object anymore, the fix is to stop resolving it at all: dispatch
	// purely off the FCS String ID (plain std::string compare, no native calls), never touching the
	// underlying game data object for content this plugin didn't author.
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
// CONFIRMED ROOT CAUSE (finally, after a long detour): hooking replyClicked itself was never the
// problem. Five alternative approaches were tried and abandoned chasing a wrong theory - triggerNextLine
// (MinHook won't hook it), DialogueWindow::activateResponse (installs, never fires), MyGUI widget click
// events (never fire, widgets churn constantly), Dialogue::currentLine polling (only ever reflects
// NPC-spoken lines, structurally can't see a player's reply), and even the player character's own
// Dialogue object (its update() never fires at all). The actual, decisive test: this exact hook shape
// (both overloads, unconditional self->replyIds[index] read in the int overload) already exists and has
// run stably end-to-end in this file the whole time - the crash only ever showed up in the JSON-refactored
// version, which is what changed. Live-tested side by side: a hardcoded stock-dialogue reply
// ("55804-Dialogue.mod", the Skeleton Barman) handled with a true, instant, no-op early return for
// every other reply never crashed, across multiple repeated clicks. The real bug was that the JSON
// version's "nothing configured" path *wasn't* actually a no-op - it still called patient->_NV_getName()
// and built a log string for every single dialogue reply click in the entire game, and separately, the
// Barman's own ID was explicitly configured during testing, so the plugin was doing real dispatch work
// against stock content on top of that. Fixed by gating on g_conversationOverrides.count(replyId) as the
// very first thing handleDialogueReplyClicked does - a plain map lookup, no native calls, no string
// construction - matching the true early-return shape that's been safe in this file all along.
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

// index here is a position into Dialogue::replyIds, not itself a reply's String ID. Reading
// self->replyIds[index] unconditionally (for every dialogue reply click in the whole game) looks risky
// on paper - a struct-offset std::vector<std::string> read on a native object - but this exact line has
// run stably through extensive live testing, including the decisive side-by-side test that finally
// isolated the real crash cause (see handleDialogueReplyClicked's comment). Kept as-is rather than
// "optimized away" - matching a proven-safe shape exactly is worth more here than a theoretical
// efficiency gain.
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

// CONFIRMED, Dialogue::_endPlayerConversation does NOT fire in this flow - live-tested: the hook
// installed without error, but never once logged, even after a genuine Yes click that should have
// ended the conversation. Removed. Replaced with edge-detecting Dialogue::conversationHasEnded()
// inside Dialogue::update() instead - update() is a per-frame, per-character function (same
// "hook something this hot is risky" category as the deliberately-avoided Character::update()), so
// this only ever does real work for patients that actually have a pending reply buffered (a cheap
// map lookup gates everything else) - in practice that's zero characters almost all the time, one
// at most while our own dialogue is active. CONFIRMED working via live testing.
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

// --- Loading ConversationOverrides from RE_Kenshi.json -------------------------------------------
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

static void loadConversationOverridesFromJson()
{
	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	std::ifstream file(jsonPath.c_str());
	if (!file.is_open())
	{
		ErrorLog("SkeletonRebirth: could not open \"" + jsonPath + "\" to load ConversationOverrides");
		return;
	}

	rapidjson::IStreamWrapper isw(file);
	rapidjson::Document doc;
	if (doc.ParseStream(isw).HasParseError())
	{
		ErrorLog("SkeletonRebirth: JSON parse error in \"" + jsonPath + "\": " + rapidjson::GetParseError_En(doc.GetParseError()));
		return;
	}

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
	g_overrideHandlers["take_item"] = &applyTakeItemOverride;
	g_overrideHandlers["show_text"] = &applyShowTextOverride;
	loadConversationOverridesFromJson();

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
}
