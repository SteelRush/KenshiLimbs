#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/HandleManager.h>
#include <kenshi/SaveManager.h>
#include <kenshi/util/hand.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Building/Building.h>
#include <kenshi/CharStats.h>
#include <kenshi/Inventory.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/ScreenLabel.h>

#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_LayoutManager.h>

#include <core/Functions.h>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>
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

// SkeletonRebirth plugin - see SkeletonRebirth/DESIGN.md for the full design history and rejected
// approaches. Core mechanism: robots never actually die (declareDead blocked). MedicalSystem::dead is
// forced back to false every time (native code sets it true just before calling declareDead(), so this
// has to be reasserted, not just left alone), and stays false for as long as the robot is Deactivated.
// "Operationally treated as dead" is NOT implemented by leaning on the native dead flag - an earlier
// version of this mod did that and found dead=true is read by several independent native systems
// (zone-unload corpse disposal, party-roster pruning, more likely undiscovered) that each had to be
// fought individually, one of which caused a real crash - see DESIGN.md for the full history. Instead,
// a Deactivated robot simply sits inert: vanilla's own knockout state (entered naturally from the
// combat that would have killed it) has no recovery timer at catastrophic damage, so nothing further is
// needed to keep it motionless. Health is frozen via MedicalSystem::medicalUpdate being skipped while
// Deactivated. Reactivation trigger: placing a Deactivated robot in the Skeleton Repair Bed (checked
// continuously via Character::updateOnScreenCheck) opens a confirmation dialogue box built from Kenshi's
// own Kenshi_MessageBox.layout; TryReactivate() does the actual state change on confirmation. Dialogue
// box text and per-button step sequences are data-driven from RE_Kenshi.json's "DialogueBoxes" object,
// not hardcoded per-dialogue C++ - see showDialogueBox()/dispatchDialogueSteps(). A button can
// be gated on the initiator's skill level or an item they're carrying (isDialogueButtonEligible()), and
// its steps can take an item, show floating text, delay, and/or run a registered action
// (dispatchDialogueSteps()/g_dialogueActions) - JSON-configurable equivalents of features an older,
// removed version of this mod had via real FCS dialogue trees. Cleanup for unattended random-spawn
// robots is not implemented yet - deferred until the core mechanism above is fully proven out.
// Deactivated state survives save/reload via a JSON side-file keyed by save-stable handle strings.
//
// Output goes to KenshiLib's own debug log via DebugLog() - see Debug.h. Filter the log for the
// "SkeletonRebirth:" prefix.

static bool isRobotRace(Character* c)
{
	RaceData* race = c->_NV_getRace();
	return race != nullptr && race->robot;
}

// Marks characters blocked from dying (declareDead hook) so the medicalUpdate hook knows to keep
// them frozen. Still Character*-keyed and session-only for the hot per-tick lookups (updateOnScreenCheck
// fires roughly every frame per character, and switching every lookup site to a handle-string key -
// a string construction per check - isn't worth it for a map that's already gated to near-zero cost
// for the overwhelming majority of characters). Save/reload survival is handled separately, as a
// write-through JSON side-file keyed by handle string (see saveDeactivatedState/loadDeactivatedState
// below) - additive, not a replacement for this map, so none of the already-stabilized per-tick hooks
// needed to change.
static std::map<Character*, bool> g_deactivated;

// Set once the reactivation dialogue box has been opened for a patient, cleared once the character
// actually leaves the Skeleton Repair Bed (see Character_updateOnScreenCheck_hook). Gates that same
// hook's trigger check so it doesn't reopen the box on top of itself every tick while still in the bed.
// Dismissing the box (any button without a "reactivate" action, e.g. "No") deliberately does NOT clear
// this itself - the trigger is level-triggered (fires every tick still in the bed), so clearing it on
// dismiss would let the very next tick reopen the box immediately, making "No" appear to do nothing
// (live tested - it does exactly this). Only DialogueAction_Reactivate (a real accepted reactivation)
// or actually leaving the bed clears it.
static std::map<Character*, bool> g_reactivateDialogueShown;

// --- Save/load persistence -----------------------------------------------------------------------
// g_deactivated (and friends) are Character*-keyed and don't survive a reload - the same logical
// character gets a fresh pointer next session. RootObjectBase::getHandle().toString() does survive
// (round-trips through HandleManager::serialise/restore - see util/hand.h), so that's what gets
// written to disk instead. No native "attach custom data to this object's own save entry" hook was
// found in RE_Kenshi's headers, and no save/load *event* exists either - SaveManager only exposes a
// polled internal state machine (SaveManager.h's Signal enum), nothing subscribable. Practical
// alternative: a small side-file, next to the active save, written on every g_deactivated mutation
// and re-read via a hook on the load path (HandleManager::_NV_restore - see that hook's own comment
// for why the *virtual* HandleManager::restore can't be hooked directly).
static std::string getPersistenceFilePath()
{
	SaveManager* saveManager = SaveManager::getSingleton();
	if (!saveManager)
		return "";

	std::string currentGame = saveManager->getCurrentGame();
	if (currentGame.empty())
		return ""; // no save loaded yet (e.g. main menu, brand new unsaved game)

	std::string path = saveManager->getSavePath();
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += "/";
	path += currentGame;
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += "/";
	path += "SkeletonRebirth_Deactivated.json";
	return path;
}

// Called after every g_deactivated mutation (Deactivation, reactivation, cleanup-sweep-triggered real
// death) rather than hooking a pre-save event - simpler, and can't be missed by a save trigger that
// turns out not to fire the way expected. Small, infrequent writes - negligible cost.
static void saveDeactivatedState()
{
	std::string path = getPersistenceFilePath();
	if (path.empty())
		return;

	rapidjson::Document doc;
	doc.SetObject();
	rapidjson::Document::AllocatorType& alloc = doc.GetAllocator();
	rapidjson::Value handles(rapidjson::kArrayType);
	for (auto it = g_deactivated.begin(); it != g_deactivated.end(); ++it)
	{
		if (!it->second || !it->first)
			continue;

		std::string handleStr = it->first->getHandle().toString();
		handles.PushBack(rapidjson::Value(handleStr.c_str(), alloc), alloc);
	}
	doc.AddMember("DeactivatedRobots", handles, alloc);

	std::ofstream out(path.c_str());
	if (!out.is_open())
	{
		ErrorLog("SkeletonRebirth: could not open \"" + path + "\" to persist Deactivated state");
		return;
	}

	rapidjson::OStreamWrapper osw(out);
	rapidjson::Writer<rapidjson::OStreamWrapper> writer(osw);
	doc.Accept(writer);
}

// Called from the HandleManager::_NV_restore hook (see that hook's comment) - repopulates
// g_deactivated after a load by resolving each persisted handle string back to a live Character* via
// hand::fromString()/getCharacter(). Missing file is normal (no Deactivated robots existed yet for
// this save) and not logged as an error; a resolution failure per-handle is only a debug log, not an
// error, since a genuinely destroyed/cleaned-up character between sessions is an expected case, not a
// bug.
static void loadDeactivatedState()
{
	std::string path = getPersistenceFilePath();
	if (path.empty())
		return;

	std::ifstream file(path.c_str());
	if (!file.is_open())
		return;

	rapidjson::Document doc;
	rapidjson::IStreamWrapper isw(file);
	if (doc.ParseStream(isw).HasParseError())
	{
		ErrorLog("SkeletonRebirth: JSON parse error in \"" + path + "\": " + rapidjson::GetParseError_En(doc.GetParseError()));
		return;
	}

	if (!doc.HasMember("DeactivatedRobots") || !doc["DeactivatedRobots"].IsArray())
		return;

	int resolved = 0;
	int failed = 0;
	const rapidjson::Value& handles = doc["DeactivatedRobots"];
	for (rapidjson::SizeType i = 0; i < handles.Size(); ++i)
	{
		if (!handles[i].IsString())
			continue;

		hand h;
		h.fromString(handles[i].GetString());
		Character* c = h.getCharacter();
		if (c)
		{
			g_deactivated[c] = true;
			++resolved;
		}
		else
		{
			++failed;
		}
	}

	std::ostringstream ss;
	ss << "SkeletonRebirth: loaded persisted Deactivated state from \"" << path << "\" - resolved=" << resolved << " failed=" << failed;
	DebugLog(ss.str());
}

// --- Hook: HandleManager::_NV_restore() - post-load persistence trigger ---------------------------
// HandleManager::restore() is virtual - hooking it directly the same way InventoryGUI::show() was
// attempted earlier would hit the same "enable whole program optimization" crash (taking the address
// of a virtual method doesn't resolve to a real hookable stub). _NV_restore is the same non-virtual,
// directly-addressable wrapper convention already relied on for InventoryTraderGUI::_CONSTRUCTOR -
// same RVA as the real restore() implementation, safe to hook. Fires whenever a save is loaded (the
// handle table itself is what's being restored here), so it's the natural "a load just happened,
// re-resolve persisted state" trigger. Always calls orig() first - restoring the real handle table
// before we try to resolve any handles against it.
void (*HandleManager_restore_orig)(HandleManager*, std::ifstream&);
void HandleManager_restore_hook(HandleManager* self, std::ifstream& in)
{
	HandleManager_restore_orig(self, in);
	loadDeactivatedState();
}

// Native "corpse unloaded" cleanup (HandleManager::destroy()), the party-roster prune
// (PlayerInterface::update()), and everything downstream of them (ActivePlatoon-level squad
// bookkeeping, the various HandleList-family destroy() overrides) were all extensively investigated
// this session as part of an earlier "operationally dead" design that set MedicalSystem::dead=true -
// see DESIGN.md for the full history, including the crash that design change caused (confirmed via
// manual minidump parsing) and why it was abandoned. None of that native machinery can fire anymore:
// Character_declareDead_hook now leaves dead=false unconditionally, so nothing here ever sees these
// robots as corpses in the first place. No hooks needed for any of it.

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
// Fires right before a character would actually die. For robots, blocks the original call - the real
// death transition, and whatever undocumented native side effects it has, never runs - but
// MedicalSystem::dead is left false throughout, not flipped to true.
//
// An earlier version of this mod set dead=true here specifically to get AI/looting/party-membership/
// GUI to treat the character as a corpse "for free", on the theory that everything reads that one flag.
// Live testing found that's not a single choke point: HandleManager::destroy() destroys any dead=true
// character when its zone unloads ("corpse unloaded"), and PlayerInterface::update() separately prunes
// dead=true characters from the player's own party roster - two independent native systems, and there
// was no confidence a third and fourth wouldn't turn up next (AI corpse-disposal jobs were a live
// candidate). Fighting each one as it surfaced (blocking destroy(), then fighting to restore roster
// membership, which itself caused a real crash - see DESIGN.md) proved unsustainable. "Operationally
// treated as dead" is implemented ourselves instead, narrowly, so nothing native ever sees dead=true for
// these characters at all: AI is paused below (own task system disabled, so the character doesn't keep
// acting despite catastrophic health), health stays frozen (MedicalSystem_medicalUpdate_hook), and the
// GUI tag is overridden directly (DatapanelGUI_setLine hook, unconditionally now rather than only when
// it spots "dead" text - see that hook's comment for why that had to change too).
void (*Character_declareDead_orig)(Character*);
void Character_declareDead_hook(Character* self)
{
	if (isRobotRace(self))
	{
		// CONFIRMED (live testing, 2026-07-14): MedicalSystem::dead is already true by the time this
		// hook is reached (logged medDead=1 on entry, before this hook touches anything) - something
		// upstream of declareDead() (combat/damage resolution) sets it directly; declareDead() is only
		// the finalize step, not where dead=true originates. Blocking declareDead() alone does NOT stop
		// dead from becoming true - it has to be forced back to false explicitly here, every time,
		// otherwise every native system that reads dead (corpse-unload destroy, party-roster pruning,
		// the corpse-only GUI text) still fires exactly as if this were a real, permanent death.
		MedicalSystem* med = self->getMedical();
		if (med)
			med->dead = false;

		// No explicit AI pause needed here - confirmed live (2026-07-14) that vanilla's own knockout
		// state has no recovery timer at the catastrophic damage level that triggers this hook, so the
		// character is already permanently inert on its own. Tried setJobsEnabled(false)/clearOrders()/
		// clearJobs() first as a safeguard; removed once confirmed redundant.
		g_deactivated[self] = true;
		saveDeactivatedState();

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
		for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
			nudgeFleshTowardSurvivable(*it);
	}

	g_deactivated.erase(self);
	saveDeactivatedState();
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
// --- JSON-driven dialogue boxes -----------------------------------------------------------------
// Text, button gating, and button behavior for the mod's custom MyGUI dialogue boxes are data-driven
// from RE_Kenshi.json's "DialogueBoxes" object, not hardcoded per-dialogue C++ functions - nested in
// that file rather than a separate one, matching how the old (removed) ConversationOverrides/
// DialogueSkillChecks system did it too. A button's behavior is an ordered list of "steps" (mirroring
// that old system's per-reply override list, just attached to a button instead of an FCS dialogue
// reply) - see
// DialogueBoxStepDef for the four step types. Adding a new dialogue box means adding a JSON entry and,
// if it needs a new "action" step type, registering that action in g_dialogueActions below - not
// writing a new Show*/On*Clicked function pair.
struct DialogueBoxStepDef
{
	std::string type; // "action" | "take_item" | "show_text" | "delay"
	std::string action; // for "action" - looked up in g_dialogueActions
	std::string item;   // for "take_item" - FCS/GameData item String ID, consumed from the initiator
	std::string text;   // for "show_text" - "{name}" replaced with the patient's name
	std::string color;  // for "show_text", optional - "#RRGGBB" hex, defaults to white
	float seconds;       // for "delay" - pauses the remaining steps this many seconds (wall clock)

	DialogueBoxStepDef() : seconds(0.0f) {}
};

struct DialogueBoxButtonDef
{
	std::string caption;
	std::vector<DialogueBoxStepDef> steps; // run in order when clicked - see dispatchDialogueSteps()
	std::string requiresItem;  // FCS/GameData item String ID - hidden unless initiator has one
	std::string requiresSkill; // lowercase CharStats field name (see g_skillFields) - hidden unless initiator's skill is in range
	bool hasMinSkill;
	float minSkill;
	bool hasMaxSkill;
	float maxSkill;

	DialogueBoxButtonDef() : hasMinSkill(false), minSkill(0.0f), hasMaxSkill(false), maxSkill(0.0f) {}
};

struct DialogueBoxDef
{
	std::string title;
	std::string message; // "{name}" is replaced with the patient's name
	std::vector<DialogueBoxButtonDef> buttons; // up to 3 - Kenshi_MessageBox.layout has ButtonA/B/C
};

static std::map<std::string, DialogueBoxDef> g_dialogueBoxes;

typedef void (*DialogueActionFn)(Character* patient);
static std::map<std::string, DialogueActionFn> g_dialogueActions;

// Deliberately only the plain "skill" floats (the ones under the Skills tab in-game), not attributes
// like strength/dexterity/toughness - those are derived through accessor methods rather than plain
// settable members on some builds, and aren't what "skill check" means here. Keys are the CharStats
// member name lowercased, not Kenshi's in-game display label, to keep the mapping unambiguous against
// the header this was written from. Reused verbatim from the old (removed) DialogueSkillChecks feature,
// which gated real FCS dialogue replies the same way this gates dialogue box buttons.
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

static std::string toLowerCopy(const std::string& s)
{
	std::string result = s;
	for (size_t i = 0; i < result.size(); ++i)
		result[i] = (char)tolower((unsigned char)result[i]);
	return result;
}

// "#RRGGBB" hex, the common web/paint-tool convention - not MyGUI::Colour's own undocumented string
// constructor format. Hand-rolled for the same reason the old (removed) show_text override's color
// parsing was: not worth reverse-engineering a format RE_Kenshi's headers don't document.
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

	unsigned int r = (unsigned int)strtoul(hex.substr(0, 2).c_str(), nullptr, 16);
	unsigned int g = (unsigned int)strtoul(hex.substr(2, 2).c_str(), nullptr, 16);
	unsigned int b = (unsigned int)strtoul(hex.substr(4, 2).c_str(), nullptr, 16);
	outColor = MyGUI::Colour(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	return true;
}

// Item IDs come from JSON (user-supplied, unverified), so this is SEH-guarded the same way the old
// (removed) take_item override was - MSVC forbids local C++ objects with destructors (std::string
// included) in a function that also uses __try/__except (C2712), so this stays free of them.
static GameData* getGameDataGuarded(const std::string& id, itemType category)
{
	__try
	{
		return ou->gamedata.getData(id, category);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

// Gates whether a button is even shown - not whether its steps can still run once shown. The
// "take_item" step re-checks defensively anyway (the item could in principle be lost between show and
// click).
static bool isDialogueButtonEligible(const DialogueBoxButtonDef& btn, Character* initiator)
{
	if (!btn.requiresSkill.empty())
	{
		CharStats* stats = initiator ? initiator->getStats() : nullptr;
		if (!stats)
			return false;

		auto fieldIt = g_skillFields.find(toLowerCopy(btn.requiresSkill));
		if (fieldIt != g_skillFields.end())
		{
			float value = stats->*(fieldIt->second);
			if (btn.hasMinSkill && value < btn.minSkill)
				return false;
			if (btn.hasMaxSkill && value > btn.maxSkill)
				return false;
		}
		// An unrecognized skill name is logged once at JSON-load time, not here - a typo shouldn't
		// hide a button on every single show, just be visible once in the log.
	}

	if (!btn.requiresItem.empty())
	{
		Inventory* inv = initiator ? initiator->_NV_getInventory() : nullptr;
		GameData* itemData = inv ? getGameDataGuarded(btn.requiresItem, ITEM) : nullptr;
		if (!inv || !itemData || !inv->hasItem(itemData, 1))
			return false;
	}

	return true;
}

static Character* g_pendingDialoguePatient = nullptr;
static Character* g_pendingDialogueInitiator = nullptr;
static MyGUI::VectorWidgetPtr g_dialogueLayoutWidgets;
static std::vector<DialogueBoxButtonDef> g_currentDialogueButtons; // parallel to visible ButtonA/B/C, by index

static void closeDialogueBox()
{
	if (!g_dialogueLayoutWidgets.empty())
	{
		MyGUI::LayoutManager::getInstance().unloadLayout(g_dialogueLayoutWidgets);
		g_dialogueLayoutWidgets.clear();
	}
	g_pendingDialoguePatient = nullptr;
	g_pendingDialogueInitiator = nullptr;
	g_currentDialogueButtons.clear();
}

// "show_text" is a floating rising-text notification (ForgottenGUI::createScreenLabel(), tracking the
// patient) - independent of, and not tied to, the dialogue box itself (which is already closed by the
// time any step runs - see OnDialogueButtonClicked). Not a GUI panel/message box.
static void showFloatingText(Character* patient, const std::string& text, const std::string& colorHex)
{
	if (!gui || text.empty())
		return;

	std::string resolvedText = text;
	size_t namePos = resolvedText.find("{name}");
	if (namePos != std::string::npos)
		resolvedText.replace(namePos, 6, patient->_NV_getName());

	MyGUI::Colour color = MyGUI::Colour::White;
	if (!colorHex.empty() && !tryParseHexColor(colorHex, color))
		ErrorLog("SkeletonRebirth: dialogue step \"show_text\" has unrecognized color \"" + colorHex + "\" (expected \"#RRGGBB\") - defaulting to white");

	ScreenLabel* label = gui->createScreenLabel(resolvedText, color, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
	if (label)
		label->setTracking(patient->getHandle(), Ogre::Vector3(0, 1, 0));
}

// A step sequence paused on a "delay" step resumes here rather than at click time - picked up by
// Character_updateOnScreenCheck_hook, which already polls every frame per character (see that hook's
// resume check). GetTickCount64() (wall-clock milliseconds) is used rather than a per-frame delta,
// since updateOnScreenCheck() doesn't receive one and this is cosmetic UI pacing, not gameplay-critical
// timing - the same relaxed precision the old (removed) "delay" override type used, just driven by wall
// clock instead of Dialogue::update()'s frameTime.
struct PendingDialogueSequence
{
	std::vector<DialogueBoxStepDef> steps; // copied, not referenced - see dispatchDialogueSteps()
	size_t nextIndex;
	Character* initiator;
	ULONGLONG fireAtTick;
};
static std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences;

// Runs `steps` in order starting at `startIndex` for `patient`. A "delay" step suspends the rest of the
// sequence in g_pendingDialogueSequences and returns immediately, resuming later (see
// Character_updateOnScreenCheck_hook) rather than blocking. `steps` is copied into the pending entry
// (not referenced) since it's cheap and avoids any lifetime question about where the caller's vector
// lives relative to when the delay elapses.
static void dispatchDialogueSteps(Character* patient, Character* initiator, const std::vector<DialogueBoxStepDef>& steps, size_t startIndex)
{
	for (size_t i = startIndex; i < steps.size(); ++i)
	{
		const DialogueBoxStepDef& step = steps[i];

		if (step.type == "delay")
		{
			if (step.seconds > 0.0f)
			{
				PendingDialogueSequence pending;
				pending.steps = steps;
				pending.nextIndex = i + 1;
				pending.initiator = initiator;
				pending.fireAtTick = GetTickCount64() + (ULONGLONG)(step.seconds * 1000.0f);
				g_pendingDialogueSequences[patient] = pending;
				return; // remaining steps resume later
			}
			continue;
		}

		if (step.type == "take_item")
		{
			Inventory* inv = initiator ? initiator->_NV_getInventory() : nullptr;
			GameData* itemData = inv ? getGameDataGuarded(step.item, ITEM) : nullptr;
			if (!inv || !itemData || !inv->takeOneItemOnly(itemData))
			{
				ErrorLog("SkeletonRebirth: dialogue step \"take_item\" (\"" + step.item + "\") failed for " + patient->_NV_getName() + " - stopping the rest of this sequence");
				return;
			}
			continue;
		}

		if (step.type == "show_text")
		{
			showFloatingText(patient, step.text, step.color);
			continue;
		}

		if (step.type == "action")
		{
			auto actionIt = g_dialogueActions.find(step.action);
			if (actionIt != g_dialogueActions.end())
				actionIt->second(patient);
			else
				ErrorLog("SkeletonRebirth: dialogue step action \"" + step.action + "\" has no registered handler");
			continue;
		}

		ErrorLog("SkeletonRebirth: unknown dialogue step type \"" + step.type + "\" for " + patient->_NV_getName());
	}
}

static void OnDialogueButtonClicked(MyGUI::Widget* sender)
{
	Character* patient = g_pendingDialoguePatient;
	Character* initiator = g_pendingDialogueInitiator;

	// Which of ButtonA/B/C fired - looked up by widget identity while the layout is still loaded, since
	// closeDialogueBox() below unloads it.
	int index = -1;
	if (!g_dialogueLayoutWidgets.empty())
	{
		MyGUI::Widget* root = g_dialogueLayoutWidgets[0];
		const std::string prefix = "SkeletonRebirth_";
		if (sender == root->findWidget(prefix + "ButtonA"))
			index = 0;
		else if (sender == root->findWidget(prefix + "ButtonB"))
			index = 1;
		else if (sender == root->findWidget(prefix + "ButtonC"))
			index = 2;
	}

	DialogueBoxButtonDef btn;
	bool hasBtn = (index >= 0 && index < (int)g_currentDialogueButtons.size());
	if (hasBtn)
		btn = g_currentDialogueButtons[index];

	closeDialogueBox();

	if (!patient || !hasBtn)
		return;

	dispatchDialogueSteps(patient, initiator, btn.steps, 0);
}

// Shows the JSON-defined dialogue box `dialogueId` (see RE_Kenshi.json's "DialogueBoxes" object) for
// `patient`. `initiator` is who a "requiresItem"/"requiresSkill" button (and any "take_item" step) is
// checked against (e.g. the player character) - null is fine for a dialogue with no gated buttons or
// item steps.
static void showDialogueBox(const std::string& dialogueId, Character* patient, Character* initiator)
{
	if (g_pendingDialoguePatient)
		return; // one at a time - a second trigger shouldn't stack a second box

	auto defIt = g_dialogueBoxes.find(dialogueId);
	if (defIt == g_dialogueBoxes.end())
	{
		ErrorLog("SkeletonRebirth: no dialogue box definition for \"" + dialogueId + "\" - is RE_Kenshi.json missing a \"DialogueBoxes\" entry for it?");
		return;
	}
	const DialogueBoxDef& def = defIt->second;

	// Resolve eligible buttons first, before loading the layout - a dialogue with every button gated
	// out from under the current initiator would otherwise show as an unclosable empty box.
	std::vector<DialogueBoxButtonDef> eligibleButtons;
	for (size_t i = 0; i < def.buttons.size() && eligibleButtons.size() < 3; ++i)
	{
		if (isDialogueButtonEligible(def.buttons[i], initiator))
			eligibleButtons.push_back(def.buttons[i]);
	}
	if (eligibleButtons.empty())
	{
		ErrorLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" has no eligible buttons for this initiator - not shown");
		return;
	}

	g_pendingDialoguePatient = patient;
	g_pendingDialogueInitiator = initiator;

	// Prefixing every widget name avoids collisions with a second load of the same layout elsewhere
	// in the game (MyGUI names should be unique) - "Root"/"ButtonA" etc. become
	// "SkeletonRebirth_Root"/"SkeletonRebirth_ButtonA" etc. after loading.
	const std::string prefix = "SkeletonRebirth_";
	g_dialogueLayoutWidgets = MyGUI::LayoutManager::getInstance().loadLayout("Kenshi_MessageBox.layout", prefix, nullptr);
	if (g_dialogueLayoutWidgets.empty())
	{
		ErrorLog("SkeletonRebirth: loadLayout(Kenshi_MessageBox.layout) returned no widgets");
		g_pendingDialoguePatient = nullptr;
		g_pendingDialogueInitiator = nullptr;
		return;
	}

	MyGUI::Widget* root = g_dialogueLayoutWidgets[0];
	root->setProperty("Caption", def.title);

	// Kenshi_MessageBox.layout's own Root position_real is "0 0 0.182292 0.148148" - anchored at the
	// screen's top-left corner by default. No real-size getter exists in this SDK to compute this
	// dynamically, so the same fixed width/height from the layout file is reused here, just recentered:
	// left/top = (1 - width/height) / 2.
	root->setRealCoord(0.408854f, 0.425926f, 0.182292f, 0.148148f);

	std::string message = def.message;
	size_t namePos = message.find("{name}");
	if (namePos != std::string::npos)
		message.replace(namePos, 6, patient->_NV_getName());

	MyGUI::Widget* messageText = root->findWidget(prefix + "MessageText");
	if (messageText)
		messageText->setProperty("Caption", message);

	static const char* buttonWidgetNames[3] = { "ButtonA", "ButtonB", "ButtonC" };
	g_currentDialogueButtons = eligibleButtons;
	for (int i = 0; i < 3; ++i)
	{
		MyGUI::Widget* button = root->findWidget(prefix + buttonWidgetNames[i]);
		if (!button)
			continue;

		if (i < (int)eligibleButtons.size())
		{
			button->setProperty("Caption", eligibleButtons[i].caption);
			button->setVisible(true);
			button->eventMouseButtonClick += MyGUI::newDelegate(OnDialogueButtonClicked);
		}
		else
		{
			// Fewer than 3 buttons ended up eligible - Kenshi_MessageBox.layout always has all three.
			button->setVisible(false);
		}
	}

	DebugLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" shown for " + patient->_NV_getName());
}

// The one action a dialogue box "action" step can currently trigger. Registered into g_dialogueActions
// in startPlugin(). "No"/dismiss buttons just have an empty steps list - see closeDialogueBox()'s
// unconditional close in OnDialogueButtonClicked.
static void DialogueAction_Reactivate(Character* patient)
{
	if (!TryReactivate(patient))
		ErrorLog("SkeletonRebirth: TryReactivate failed for " + patient->_NV_getName() + " after confirm");

	// Tidiness only - TryReactivate() already erased patient from g_deactivated on success, so the
	// trigger check in Character_updateOnScreenCheck_hook short-circuits before ever reading this
	// regardless. On failure, deliberately left alone, for the same reason a "No" click leaves it
	// alone - see g_reactivateDialogueShown's own declaration comment for why.
	g_reactivateDialogueShown.erase(patient);
}

// Same Win32 "find our own module's path" technique the old (removed) ConversationOverrides system
// used - mod DLLs don't get a working directory of their own, so this is how a mod locates its own
// folder to load bundled config from (RE_Kenshi.json, this file, etc. all sit next to the DLL).
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

static void loadDialogueBoxesFromJson()
{
	// Nested under "DialogueBoxes" in RE_Kenshi.json itself, not a separate file - matching the old
	// (removed) ConversationOverrides/DialogueSkillChecks system, which both lived under their own keys
	// in this exact same file rather than each getting their own JSON.
	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	std::ifstream file(jsonPath.c_str());
	if (!file.is_open())
	{
		ErrorLog("SkeletonRebirth: could not open \"" + jsonPath + "\" - no dialogue boxes will show");
		return;
	}

	rapidjson::IStreamWrapper isw(file);
	rapidjson::Document doc;
	if (doc.ParseStream(isw).HasParseError())
	{
		ErrorLog("SkeletonRebirth: JSON parse error in \"" + jsonPath + "\": " + rapidjson::GetParseError_En(doc.GetParseError()));
		return;
	}

	if (!doc.IsObject() || !doc.HasMember("DialogueBoxes") || !doc["DialogueBoxes"].IsObject())
	{
		ErrorLog("SkeletonRebirth: \"" + jsonPath + "\" has no \"DialogueBoxes\" object - no dialogue boxes will show");
		return;
	}
	const rapidjson::Value& dialogueBoxes = doc["DialogueBoxes"];

	for (auto it = dialogueBoxes.MemberBegin(); it != dialogueBoxes.MemberEnd(); ++it)
	{
		if (!it->value.IsObject())
			continue;

		const rapidjson::Value& v = it->value;
		DialogueBoxDef def;
		def.title = (v.HasMember("title") && v["title"].IsString()) ? v["title"].GetString() : "";
		def.message = (v.HasMember("message") && v["message"].IsString()) ? v["message"].GetString() : "";

		if (v.HasMember("buttons") && v["buttons"].IsArray())
		{
			for (auto bit = v["buttons"].Begin(); bit != v["buttons"].End(); ++bit)
			{
				if (!bit->IsObject())
					continue;

				DialogueBoxButtonDef btn;
				btn.caption = (bit->HasMember("caption") && (*bit)["caption"].IsString()) ? (*bit)["caption"].GetString() : "";
				if (bit->HasMember("requiresItem") && (*bit)["requiresItem"].IsString())
					btn.requiresItem = (*bit)["requiresItem"].GetString();
				if (bit->HasMember("requiresSkill") && (*bit)["requiresSkill"].IsString())
				{
					btn.requiresSkill = (*bit)["requiresSkill"].GetString();
					if (!g_skillFields.count(toLowerCopy(btn.requiresSkill)))
						ErrorLog("SkeletonRebirth: dialogue box \"" + std::string(it->name.GetString()) + "\" button \"" + btn.caption + "\" has unrecognized requiresSkill \"" + btn.requiresSkill + "\" - this button will show unconditionally (skill part of the check is skipped)");
				}
				if (bit->HasMember("minSkill") && (*bit)["minSkill"].IsNumber())
				{
					btn.hasMinSkill = true;
					btn.minSkill = (*bit)["minSkill"].GetFloat();
				}
				if (bit->HasMember("maxSkill") && (*bit)["maxSkill"].IsNumber())
				{
					btn.hasMaxSkill = true;
					btn.maxSkill = (*bit)["maxSkill"].GetFloat();
				}

				if (bit->HasMember("steps") && (*bit)["steps"].IsArray())
				{
					for (auto sit = (*bit)["steps"].Begin(); sit != (*bit)["steps"].End(); ++sit)
					{
						if (!sit->IsObject())
							continue;

						DialogueBoxStepDef step;
						step.type = (sit->HasMember("type") && (*sit)["type"].IsString()) ? (*sit)["type"].GetString() : "";
						if (step.type.empty())
						{
							ErrorLog("SkeletonRebirth: dialogue box \"" + std::string(it->name.GetString()) + "\" button \"" + btn.caption + "\" has a step with no \"type\" - skipped");
							continue;
						}
						if (sit->HasMember("action") && (*sit)["action"].IsString())
							step.action = (*sit)["action"].GetString();
						if (sit->HasMember("item") && (*sit)["item"].IsString())
							step.item = (*sit)["item"].GetString();
						if (sit->HasMember("text") && (*sit)["text"].IsString())
							step.text = (*sit)["text"].GetString();
						if (sit->HasMember("color") && (*sit)["color"].IsString())
							step.color = (*sit)["color"].GetString();
						if (sit->HasMember("seconds") && (*sit)["seconds"].IsNumber())
							step.seconds = (*sit)["seconds"].GetFloat();
						btn.steps.push_back(step);
					}
				}

				def.buttons.push_back(btn);
			}
		}

		g_dialogueBoxes[it->name.GetString()] = def;
	}

	std::ostringstream summary;
	summary << "SkeletonRebirth: loaded " << g_dialogueBoxes.size() << " dialogue box definition(s) from \"" << jsonPath << "\"";
	DebugLog(summary.str());
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

// --- Hook: Character::updateOnScreenCheck() - reactivation trigger polling point -------------------
// Reused from an earlier cleanup-sweep design ("fires roughly every frame per character") as the
// reactivation-trigger polling point - a leftover from when medicalUpdate() (the original choice)
// stopped firing under the old dead=true design; see that hook's comment. No cleanup sweep of any kind
// lives here (or anywhere in this file currently - see the top-of-file summary comment).
// Always calls orig() - this only observes, never blocks/alters the real on-screen-check behavior.
bool (*Character_updateOnScreenCheck_orig)(Character*);
bool Character_updateOnScreenCheck_hook(Character* self)
{
	bool result = Character_updateOnScreenCheck_orig(self);

	// Paused dialogue step sequences (see PendingDialogueSequence) resume from here, regardless of
	// self's own g_deactivated status - this hook fires every frame for every character, not just
	// tracked ones, so it doubles as the tick source without needing a new hook.
	auto delayIt = g_pendingDialogueSequences.find(self);
	if (delayIt != g_pendingDialogueSequences.end() && GetTickCount64() >= delayIt->second.fireAtTick)
	{
		PendingDialogueSequence pending = delayIt->second;
		g_pendingDialogueSequences.erase(delayIt);
		dispatchDialogueSteps(self, pending.initiator, pending.steps, pending.nextIndex);
	}

	auto it = g_deactivated.find(self);
	if (it == g_deactivated.end() || !it->second)
		return result;

	if (isInSkeletonBed(self))
	{
		if (!g_reactivateDialogueShown.count(self))
		{
			Character* initiator = (ou && ou->player) ? ou->player->getAnyPlayerCharacter() : nullptr;
			showDialogueBox("reactivate_confirm", self, initiator);
			g_reactivateDialogueShown[self] = true;
		}
	}
	else
	{
		// Left the bed (or never entered it yet) - reset "already asked" so a future bed-placement
		// prompts again. This is also what makes a "No" answer's dismissal stick instead of an
		// instant re-prompt loop: see g_reactivateDialogueShown's own declaration comment for why
		// dismissing the box doesn't clear this flag itself, only leaving the bed does.
		g_reactivateDialogueShown.erase(self);
	}

	return result;
}

// --- Hook: DatapanelGUI::setLine(key,s1,s2,category,last,keyVisible) - status tag override -------
// The character info panel's "State:" row (category 3) is overridden unconditionally for any tracked
// Deactivated character, regardless of what vanilla text it was about to show (see the override's own
// comment below for why this had to move away from matching literal "Dead" text).
static const char* DEACTIVATED_COLOR = "#59231a"; // vanilla's own dark red, same as its "Dead" text

// Broadened from matching the word "dead" specifically to overriding unconditionally: now that
// declareDead_hook leaves MedicalSystem::dead false (see that hook's comment for why), the corpse-only
// native call that used to set literal "Dead" text never fires for these characters at all - whatever
// text WOULD show for a character frozen at fatal health while still alive (most likely "Dying" or
// "Critical") gets overridden unconditionally instead of pattern-matched, so it doesn't matter what the
// underlying vanilla text actually is. The old text-matching all-caps "DEAD" banner branch is gone -
// that banner is corpse-only UI and should never fire anymore now that dead stays false; if it somehow
// does, it'll just show vanilla's own text rather than silently going unhandled.
DataPanelLine* (*DatapanelGUI_setLine_KeyLastVisible_orig)(DatapanelGUI*, const std::string&, const std::string&, const std::string&, int, bool, bool);
DataPanelLine* DatapanelGUI_setLine_KeyLastVisible_hook(DatapanelGUI* self, const std::string& keyValue, const std::string& s1, const std::string& s2, int category, bool last, bool keyVisible)
{
	Character* target = self->getObject().getCharacter();
	if (!target || !g_deactivated.count(target))
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);

	// The "State:" row - value carries the color tag, key/s1 stay "State:". Confirmed live: vanilla's
	// native KO-state text here is "Rebooting" for robots (organic races apparently show "Recovery
	// coma") - color-tagged the same dark red (#59231a) vanilla uses for "Dead" elsewhere.
	if (keyValue == "State:")
	{
		std::string overriddenS2 = std::string(DEACTIVATED_COLOR) + "POWER FAILURE";
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, overriddenS2, category, last, keyVisible);
	}

	return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);
}

__declspec(dllexport) void startPlugin()
{
	g_dialogueActions["reactivate"] = &DialogueAction_Reactivate;
	loadDialogueBoxesFromJson();

	// CONFIRMED SAFE - each individually isolated via single-variable live testing (removed alone while
	// the rest of the plugin's hooks stayed active; crash still reproduced identically both times,
	// clearing both as suspects independent of one another).
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::declareDead), Character_declareDead_hook, &Character_declareDead_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add declareDead hook!");


	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate), MedicalSystem_medicalUpdate_hook, &MedicalSystem_medicalUpdate_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::medicalUpdate hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::updateOnScreenCheck), Character_updateOnScreenCheck_hook, &Character_updateOnScreenCheck_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Character::updateOnScreenCheck hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&HandleManager::_NV_restore), HandleManager_restore_hook, &HandleManager_restore_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add HandleManager::_NV_restore hook!");

	typedef DataPanelLine* (DatapanelGUI::*SetLineKeyLastVisibleFn)(const std::string&, const std::string&, const std::string&, int, bool, bool);
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetLineKeyLastVisibleFn)&DatapanelGUI::setLine), DatapanelGUI_setLine_KeyLastVisible_hook, &DatapanelGUI_setLine_KeyLastVisible_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add DatapanelGUI::setLine(key,last,visible) hook!");

}
