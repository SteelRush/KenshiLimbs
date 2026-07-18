#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/HandleManager.h>
#include <kenshi/SaveManager.h>
#include <kenshi/util/hand.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Building/Building.h>
#include <kenshi/CharStats.h>
#include <kenshi/Faction.h>
#include <kenshi/Inventory.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MessageBoxManager.h>
#include <kenshi/gui/ScreenLabel.h>

#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Window.h>

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
#include <utility>
#include <vector>

// AI/AI.h forward-declares CharacterMessage as a class while Character.h defines it as an enum,
// so both can't be included together - declare the one method we need locally instead.
class AITaskSytem;
class AI
{
public:
	AITaskSytem* getTaskSystem() const;
};

// MessageBoxManager::createMessageBox() as declared in MessageBoxManager.h fails to link
// ("unresolved external symbol") - confirmed via the build error, then confirmed why via
// MessageBoxManager.inc: its real mangled name (five template-heavy parameters) is too long for
// MASM's identifier limit, so KenshiLib's .inc generator falls back to exporting it under an
// arbitrary placeholder name instead of its true mangled one - a documented, recurring pattern for
// ~24 functions across KenshiLib (e.g. AnimalInventoryLayout::setupSections and others hit the same
// limit), not something specific to this function. The placeholder is a plain `jmp` to the real
// function's address, so it's calling-convention-transparent - redeclaring it here with matching
// parameter types under its literal exported name (extern "C" to skip C++ name mangling on our side)
// calls the exact same native function the header's declaration was meant to reach.
extern "C" MyGUI::Window* MessageBoxManager_createMessageBox_PLACEHOLDER(
	const std::string& title,
	const std::string& message,
	const Ogre::vector<std::pair<std::string, int> >::type& buttons,
	bool modal,
	MyGUI::delegates::IDelegate1<int>* callback);

// SkeletonRebirth plugin - see DESIGN.md for full history and rejected approaches. Robots never
// actually die (declareDead blocked, MedicalSystem::dead kept false); a Deactivated robot sits inert
// via vanilla's own knockout state, with health frozen (medicalUpdate skipped). Placing one in a
// Skeleton Repair Bed (polled via Character::updateOnScreenCheck) opens a confirmation dialogue box
// built from Kenshi's own Kenshi_MessageBox.layout, data-driven from RE_Kenshi.json's "DialogueBoxes"
// object (see showDialogueBox()/dispatchDialogueSteps()). Deactivated state survives save/reload via a
// JSON side-file keyed by handle string.
//
// DebugLog() output (see Debug.h) is prefixed "SkeletonRebirth:" - gated behind verboseLog() below,
// off by default.

// Gates every DebugLog() call in this file behind RE_Kenshi.json's "Debug" (bool, default false, see
// loadDebugSettingFromJson()). Regular play still generates real ongoing log volume even with
// declareDead() debounced (dialogue boxes opening, reactivations succeeding, JSON reload summaries,
// ...) - keeping that off by default avoids growing RE_Kenshi_log.txt indefinitely for players who
// never need it, while leaving it available for troubleshooting without a rebuild. ErrorLog() is
// deliberately NOT gated anywhere in this file - those indicate real problems/misconfigurations and
// should always be visible regardless of this setting.
static bool g_debugLoggingEnabled = false;
static void verboseLog(const std::string& message)
{
	if (g_debugLoggingEnabled)
		DebugLog(message);
}

static bool isRobotRace(Character* c)
{
	RaceData* race = c->getRace();
	return race != nullptr && race->robot;
}

// Whether this character's own FCS "Character" template is categorized as ANIMAL_CHARACTER (vs
// HUMAN_CHARACTER) - itemType is the same category enum already used elsewhere in this file (ITEM,
// DIALOGUE_LINE, ...), just read off this character's own defining GameData. Not Character::isAnimal():
// that reflects the CharacterAnimal AI/movement component, not this FCS category, and returns false for
// e.g. an Iron Spider even though its FCS entry is authored as ANIMAL_CHARACTER.
static bool isAnimalCharacterType(Character* c)
{
	GameData* data = c->getGameData();
	return data && data->type == ANIMAL_CHARACTER;
}

// Character*-keyed and session-only (doesn't survive reload - see saveDeactivatedState/
// loadDeactivatedState below for the handle-string-keyed side-file that does).
static std::map<Character*, bool> g_deactivated;

// Gates Character_updateOnScreenCheck_hook's trigger loop so a matched trigger doesn't reopen its box
// every tick while conditions still hold. Dismissing a box (e.g. "No") deliberately does NOT clear
// this - the trigger is level-triggered, so clearing on dismiss would reopen the box the very next
// tick, making "No" appear to do nothing. Only a real state change (leaving the building, or an action
// like reactivation clearing g_deactivated) clears it. Keyed by (character, trigger index) rather than
// just character, since DialogueTriggers (see below) supports more than one trigger.
static std::map<std::pair<Character*, size_t>, bool> g_triggerShown;

// Drops every pending trigger-shown entry for one character, regardless of which trigger set it -
// called after an action (like reactivation) that could invalidate more than one trigger's
// requiredStates at once. A handful of linear scans over what's normally a tiny map costs nothing.
static void clearTriggerShownFor(Character* c)
{
	for (auto it = g_triggerShown.begin(); it != g_triggerShown.end(); )
	{
		if (it->first.first == c)
			it = g_triggerShown.erase(it);
		else
			++it;
	}
}

// --- Save/load persistence -----------------------------------------------------------------------
// g_deactivated is Character*-keyed and doesn't survive reload; RootObjectBase::getHandle().toString()
// does. No native "attach data to this object's save entry" hook exists, and SaveManager exposes no
// subscribable save/load event - so state is written to a small side-file next to the active save and
// re-read on load via HandleManager::_NV_restore (see that hook's comment for why not the virtual
// HandleManager::restore).
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

// Missing file is normal (no Deactivated robots yet this save); a per-handle resolution failure is only
// debug-logged, since a character destroyed between sessions is expected, not a bug.
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
	verboseLog(ss.str());
}

// --- Hook: HandleManager::_NV_restore() - post-load persistence trigger ---------------------------
// HandleManager::restore() is virtual; hooking it directly (taking &Class::Method) resolves to an
// MSVC vtable thunk, not a real stub, and crashes. _NV_restore is the non-virtual wrapper, same RVA,
// safe to hook - see DESIGN.md's "hooking virtual functions" pitfall section.
void (*HandleManager_restore_orig)(HandleManager*, std::ifstream&);
void HandleManager_restore_hook(HandleManager* self, std::ifstream& in)
{
	HandleManager_restore_orig(self, in);
	loadDeactivatedState();
}

// Do not set MedicalSystem::dead=true to get native corpse/roster cleanup "for free" - see
// Character_declareDead_hook's comment and DESIGN.md for why that's been tried twice and reverted
// twice (most recently 2026-07-16, a real recruited squad member permanently lost to the party-roster
// prune). Nothing here needs a HandleManager::destroy() hook because dead never becomes true.

// MedicalSystem::anatomy is the complete part list; getPart(PART_HEAD/PART_TORSO) alone misses a
// second torso-region part ("chest", distinct from "stomach") - see TryReactivate's nudge loop.
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
	ss << "name=\"" << c->getName() << "\""
	   << " ptr=" << (void*)c
	   << " handle=" << c->getHandle().toString()
	   << " race=" << (c->getRace() ? c->getRace()->data->stringID : "<none>")
	   << " robot=" << isRobotRace(c)
	   << " isDead=" << c->isDead()
	   << " medDead=" << (med ? med->dead : false)
	   << " proneState=" << (int)c->getProneState()
	   << " knockoutTimer=" << (med ? med->knockoutTimer : -1.0f)
	   << " blood=" << (med ? med->blood : -1.0f)
	   << " " << describeAnatomy(med)
	   << " isOnScreen=" << c->isOnScreen
	   << " isVisibleAndNear=" << c->isVisibleAndNear
	   << " goalString=\"" << getCurrentGoalStringSafe(c) << "\"";
	return ss.str();
}

// --- Hook 1: declareDead() -----------------------------------------------------------------
// Blocks the real death transition for robots. MedicalSystem::dead is kept false - do not set it true
// to get AI/looting/party-membership/GUI to treat the character as a corpse "for free": that's been
// tried twice (most recently 2026-07-16) and reverted twice after it caused real, permanent loss of a
// character via native systems that independently read dead (HandleManager::destroy's zone-unload
// cleanup, PlayerInterface::update's party-roster prune) - see DESIGN.md. "Operationally dead" is
// instead handled directly: AI is paused below, health frozen (MedicalSystem_medicalUpdate_hook), and
// the GUI status tag overridden (DatapanelGUI_setLine hook).
void (*Character_declareDead_orig)(Character*);
void Character_declareDead_hook(Character* self)
{
	if (isRobotRace(self))
	{
		// dead is already true on entry (some upstream combat/damage code sets it directly -
		// declareDead() only finalizes) - must be reset every time, not just left alone.
		MedicalSystem* med = self->getMedical();
		if (med)
			med->dead = false;

		// This fires repeatedly - confirmed live via a 250k+ line log spike (~550 calls/second
		// sustained) from a handful of robots taking continuous damage in combat - since some upstream
		// system keeps re-setting dead=true and re-invoking this for as long as a Deactivated robot
		// keeps getting hit, not just once at the moment of "death". Only the first call for a given
		// character needs to touch disk (saveDeactivatedState()) or build the describe() log string;
		// every repeat call skips straight past both once the character's already recorded, which is
		// what actually keeps this genuinely hot path cheap. The med->dead reset above still has to run
		// unconditionally every time regardless.
		if (g_deactivated.count(self))
			return;

		// No explicit AI pause needed - vanilla's own knockout state has no recovery timer at the
		// catastrophic damage level that triggers this hook, so the character is already inert.
		g_deactivated[self] = true;
		saveDeactivatedState();

		// Player-squad robots getting Deactivated is otherwise silent - no native "X is dead" message
		// fires, since dead never actually becomes true. Uses Kenshi's own player-notification queue
		// (see showGameNotification()'s comment further down) directly rather than through that
		// helper, since this is a fixed, hook-triggered message, not JSON-authored dialogue text - no
		// {name}/{item} substitution machinery needed for a string built right here.
		Faction* faction = self->getFaction();
		if (faction && faction->isThePlayer() && ou)
			ou->showPlayerAMessage_withLog(self->getName() + " has died.", true);

		verboseLog("SkeletonRebirth: declareDead() BLOCKED for robot -> " + describe(self));
		return;
	}

	Character_declareDead_orig(self);
}

// Reactivating with flesh still fatal would let medicalUpdate() immediately re-trigger declareDead()
// the moment it un-freezes. A 1% nudge toward zero clears isDead() without granting a full heal - the
// character wakes up critically hurt on purpose. Only applies below 0 (an already-healthy part pushed
// further positive overshoots its max instead), and clamped to maxHealth() as a backstop.
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

// Nudges every part in anatomy, not just PART_HEAD/PART_TORSO via getPart() - "chest" and "stomach"
// are separate torso-region parts in Kenshi's damage model but share one PartType bucket, so getPart()
// only ever reaches one of them, leaving the other fatal and re-triggering declareDead() on unfreeze.
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
	verboseLog("SkeletonRebirth: TryReactivate() SUCCEEDED -> " + describe(self));

	return true;
}

// Custom dialogue box rather than Dialogue::startPlayerConversation() (dialogue was suppressed while
// this mod was still using MedicalSystem::dead=true - see DESIGN.md) or hand-built widgets
// (ForgottenGUI::createPanel/createButton render nothing usable without a ResourceLayout template).
//
// Built via MessageBoxManager::createMessageBox() - the same native function the game itself uses for
// its own confirmation popups - rather than manually MyGUI::LayoutManager::loadLayout()-ing
// Kenshi_MessageBox.layout directly (an earlier version did this). The hand-loaded version rendered
// with only a plain dark background instead of the game's real window chrome, even though it
// referenced the identical skin ("Kenshi_WindowC", the same Kenshi_GenericWindowSkin/
// Kenshi_GenericWindowHeaderSkin body every other window in the game uses) - going through the native
// entry point instead guarantees the box looks and positions itself exactly like every other message
// box in the game, since it's the same code path rather than a lookalike. This also means
// MessageBoxManager owns the box's entire lifecycle - creation, positioning, and teardown on click -
// so there's no layout to manually unload the way there was before; this file only needs to track
// which patient/initiator/buttons a pending box belongs to for when its callback fires.
// See DESIGN.md for the full rejected-approaches history.
//
// --- JSON-driven dialogue boxes -----------------------------------------------------------------
// Box text and button behavior are data-driven from RE_Kenshi.json's "DialogueBoxes" object rather
// than hardcoded per-dialogue C++. A button's behavior is an ordered list of "steps" - see
// DialogueBoxStepDef for the step types. A new dialogue box needs only a JSON entry; a new "action"
// step type needs registering in g_dialogueActions.
//
// Nested/multi-level menus are just another dialogue box: an "open_menu" step opens a different
// "DialogueBoxes" entry by ID, the same way the reactivation trigger opens the first one (see
// showDialogueBox()). A "back" button is nothing special either - just a button in the submenu whose
// steps open the parent menu's ID. There's no dedicated menu-stack/breadcrumb tracking; each box only
// ever knows the one ID it was opened with, so "back" has to name its target explicitly rather than
// popping an implicit history.
struct DialogueBoxStepDef
{
	std::string type; // "action" | "take_item" | "show_text" | "notify" | "delay" | "open_menu"
	std::string action; // for "action" - looked up in g_dialogueActions
	std::string item;   // for "take_item" - FCS/GameData item String ID, consumed from the initiator;
	                    // also usable on "show_text"/"notify" purely to resolve "{item}" in text, no consuming
	std::string text;   // for "show_text"/"notify" - "{name}"/"{item}" replaced with the patient's name / item's display name
	std::string color;  // for "show_text", optional - "#RRGGBB" hex, defaults to white
	float seconds;       // for "delay" - pauses the remaining steps this many seconds (wall clock)
	std::string menu;    // for "open_menu" - a "DialogueBoxes" key to open, e.g. a submenu or "back" target

	DialogueBoxStepDef() : seconds(0.0f) {}
};

struct DialogueBoxButtonDef
{
	// Kenshi_MessageBox.layout's buttons (Kenshi_Button2 skin) have a fixed width sized for short
	// captions like the layout's own placeholder "A"/"B"/"C" - text doesn't wrap or shrink to fit, it
	// just clips on both sides once centered text overflows the button. Confirmed live: "Do nothing"
	// (10 characters) rendered fully; "Run Diagnostics" (15 characters) rendered as "un Diagnostic" -
	// both ends clipped. Keep captions at "Do nothing"'s length or shorter (~10 characters) to be safe.
	std::string caption;
	std::vector<DialogueBoxStepDef> steps; // run in order when clicked - see dispatchDialogueSteps()
	std::string requiresItem;  // FCS/GameData item String ID - hidden unless initiator has one
	std::string requiresSkill; // lowercase CharStats field name (see g_skillFields) - hidden unless initiator's skill is in range
	bool hasMinSkill;
	float minSkill;
	bool hasMaxSkill;
	float maxSkill;
	bool excludePlayerFaction; // hidden if the patient belongs to the player's faction
	bool requiresPlayerFaction; // hidden unless the patient belongs to the player's faction - the counterpart to excludePlayerFaction
	bool requiresDeactivated; // hidden unless the patient is in g_deactivated (i.e. "POWER FAILURE") - see DatapanelGUI_setLine_KeyLastVisible_hook
	bool requiresAnimal; // hidden unless isAnimalCharacterType() - lets an animal-only button require a different item than the humanoid one
	bool excludeAnimal; // hidden if isAnimalCharacterType() - the humanoid-only counterpart to requiresAnimal

	DialogueBoxButtonDef() : hasMinSkill(false), minSkill(0.0f), hasMaxSkill(false), maxSkill(0.0f), excludePlayerFaction(false), requiresPlayerFaction(false), requiresDeactivated(false), requiresAnimal(false), excludeAnimal(false) {}
};

struct DialogueBoxDef
{
	std::string title;
	std::string message; // "{name}"/"{item}" replaced with the patient's name / this box's item's display name
	std::string item;    // optional - FCS/GameData item String ID, resolved only for "{item}" in message
	std::vector<DialogueBoxButtonDef> buttons; // up to 3 - Kenshi_MessageBox.layout has ButtonA/B/C
};

static std::map<std::string, DialogueBoxDef> g_dialogueBoxes;

// --- JSON-driven dialogue triggers ---------------------------------------------------------------
// "When does a dialogue box open" is data-driven the same way "what does a button do" already is -
// RE_Kenshi.json's "DialogueTriggers" array, each entry an AND of "requiredStates" (named, registered
// bool(Character*) checks - see g_triggerStateChecks, all must pass) and "buildings" (the used
// building's own FCS String ID must be in this list), opening "menu" once both hold. Deliberately flat
// named checks AND'd together, not a general boolean expression language - covers "deactivated AND
// animal"-style combinations without the parsing/fragility risk a real expression syntax would add.
//
// Character_updateOnScreenCheck_hook evaluates every trigger for every on-screen character every
// frame, so requiredStates are checked before buildings, and short-circuit on the first failing check
// - the same "cheap gate first" discipline used everywhere else in this file (e.g.
// handleDialogueReplyClicked's g_conversationOverrides.count() in the old ConversationOverrides
// system). Individual state checks vary in cost (isDeactivatedState is a map lookup;
// isAnimalCharacterType calls the virtual getGameData() on the character itself) but all of them are
// still far cheaper than resolving and querying the RootObject/Building the character is standing in -
// requiredStates failing early means that never happens at all for an irrelevant character.
struct DialogueTriggerDef
{
	std::vector<std::string> requiredStates; // all looked up in g_triggerStateChecks, all must pass
	std::vector<std::string> buildings; // FCS building String IDs - any one match is enough
	std::string menu; // a "DialogueBoxes" key to open once requiredStates and buildings both match
};
static std::vector<DialogueTriggerDef> g_dialogueTriggers;

typedef bool (*TriggerStateCheckFn)(Character*);
static std::map<std::string, TriggerStateCheckFn> g_triggerStateChecks;

static bool isDeactivatedState(Character* c)
{
	auto it = g_deactivated.find(c);
	return it != g_deactivated.end() && it->second;
}

static bool isHumanoidState(Character* c)
{
	return !isAnimalCharacterType(c);
}

typedef void (*DialogueActionFn)(Character* patient);
static std::map<std::string, DialogueActionFn> g_dialogueActions;

// Skills tab only, not attributes (strength/dexterity/etc. - see g_attributeFields below). Keys are
// the CharStats member name lowercased, not the in-game display label.
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

// Subset of g_skillFields with a confident StatsEnumerated match, used by isDialogueButtonEligible()
// to read via CharStats::getStat(what, /*unmodified*/ true) instead of the raw member - the game's
// own explicit "base, ignore temporary modifiers" accessor, not an assumption that the raw field never
// gets modified in place. Deliberately not exhaustive: a few g_skillFields names (arrowdefence, bluff,
// tracking, climbing, doctor, assassin, unarmed, bows) have no StatsEnumerated entry that's an obvious,
// confident match, so those fall back to the raw member read unchanged - wrong is worse than skipped.
static std::map<std::string, StatsEnumerated> buildSkillStatEnumTable()
{
	std::map<std::string, StatsEnumerated> table;
	table["medic"] = STAT_MEDIC;
	table["masscombat"] = STAT_MASSCOMBAT;
	table["stealth"] = STAT_STEALTH;
	table["swimming"] = STAT_SWIMMING;
	table["thieving"] = STAT_THIEVING;
	table["lockpicking"] = STAT_LOCKPICKING;
	table["survival"] = STAT_SURVIVAL;
	table["engineer"] = STAT_ENGINEERING;
	table["weaponsmith"] = STAT_SMITHING_WEAPON;
	table["armoursmith"] = STAT_SMITHING_ARMOUR;
	table["bowsmith"] = STAT_SMITHING_BOW;
	table["robotics"] = STAT_ROBOTICS;
	table["science"] = STAT_SCIENCE;
	table["labouring"] = STAT_LABOURING;
	table["farming"] = STAT_FARMING;
	table["cooking"] = STAT_COOKING;
	table["dodging"] = STAT_DODGE;
	table["friendlyfire"] = STAT_FRIENDLY_FIRE;
	table["katanas"] = STAT_KATANAS;
	table["sabres"] = STAT_SABRES;
	table["hackers"] = STAT_HACKERS;
	table["blunt"] = STAT_BLUNT;
	table["heavyweapons"] = STAT_HEAVYWEAPONS;
	table["turrets"] = STAT_TURRETS;
	table["polearms"] = STAT_POLEARMS;
	return table;
}
static const std::map<std::string, StatsEnumerated> g_skillStatEnums = buildSkillStatEnumTable();

// Used by DialogueAction_SystemReset.
static std::map<std::string, float CharStats::*> buildAttributeFieldTable()
{
	std::map<std::string, float CharStats::*> table;
	table["strength"] = &CharStats::_strength;
	table["fitness"] = &CharStats::fitness;
	table["dexterity"] = &CharStats::_dexterity;
	table["perception"] = &CharStats::perception;
	table["toughness"] = &CharStats::_toughness;
	table["athletics"] = &CharStats::_athletics;
	table["meleeattack"] = &CharStats::__meleeAttack;
	table["meleedefence"] = &CharStats::_meleeDefence;
	return table;
}
static const std::map<std::string, float CharStats::*> g_attributeFields = buildAttributeFieldTable();

static std::string toLowerCopy(const std::string& s)
{
	std::string result = s;
	for (size_t i = 0; i < result.size(); ++i)
		result[i] = (char)tolower((unsigned char)result[i]);
	return result;
}

// Hand-rolled "#RRGGBB" parsing - MyGUI::Colour's own string constructor format isn't documented in
// RE_Kenshi's headers.
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

// SEH-guarded since item IDs come from JSON (unverified). MSVC forbids local objects with destructors
// in a function using __try/__except (C2712), so this stays free of them.
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

// Gates whether a button is shown, not whether its steps can still run once clicked - the "take_item"
// step re-checks defensively (the item could be lost between show and click).
static bool isDialogueButtonEligible(const DialogueBoxButtonDef& btn, Character* patient, Character* initiator)
{
	if (btn.excludePlayerFaction)
	{
		Faction* faction = patient->getFaction();
		if (faction && faction->isThePlayer())
			return false;
	}

	if (btn.requiresPlayerFaction)
	{
		Faction* faction = patient->getFaction();
		if (!faction || !faction->isThePlayer())
			return false;
	}

	if (btn.requiresAnimal && !isAnimalCharacterType(patient))
		return false;

	if (btn.excludeAnimal && isAnimalCharacterType(patient))
		return false;

	if (btn.requiresDeactivated)
	{
		auto deactivatedIt = g_deactivated.find(patient);
		if (deactivatedIt == g_deactivated.end() || !deactivatedIt->second)
			return false;
	}

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
		// unrecognized skill names are logged once at JSON-load time, not here
	}

	if (!btn.requiresItem.empty())
	{
		Inventory* inv = initiator ? initiator->getInventory() : nullptr;
		GameData* itemData = inv ? getGameDataGuarded(btn.requiresItem, ITEM) : nullptr;
		if (!inv || !itemData || !inv->hasItem(itemData, 1))
			return false;
	}

	return true;
}

static Character* g_pendingDialoguePatient = nullptr;
static Character* g_pendingDialogueInitiator = nullptr;
static std::vector<DialogueBoxButtonDef> g_currentDialogueButtons; // parallel to the button ids passed to createMessageBox

// MessageBoxManager owns the actual MyGUI::Window's lifecycle - this only clears our own bookkeeping
// of which patient/initiator/buttons a pending box belongs to, for OnMessageBoxButtonClicked() to use.
static void closeDialogueBox()
{
	g_pendingDialoguePatient = nullptr;
	g_pendingDialogueInitiator = nullptr;
	g_currentDialogueButtons.clear();
}

// itemId is optional - only resolved (via the same SEH-guarded lookup take_item uses) if the text
// actually references "{item}", so a plain "{name}"-only show_text step pays nothing extra.
// Shared by the dialogue box message, "show_text", and "notify" - all three support "{name}" (the
// patient's name) and, given a resolvable item String ID, "{item}" (that item's display name,
// GameData::name - not its FCS stringID). itemId is optional; {item} is only resolved (via the same
// SEH-guarded lookup take_item uses) if the text actually references it, so plain "{name}"-only text
// pays nothing extra. callerLabel is only used in the {item}-unresolvable error message, to say which
// of the three callers hit it.
static std::string resolvePlaceholders(const std::string& text, Character* patient, const std::string& itemId, const std::string& callerLabel)
{
	std::string resolvedText = text;

	size_t namePos = resolvedText.find("{name}");
	if (namePos != std::string::npos)
		resolvedText.replace(namePos, 6, patient->getName());

	size_t itemPos = resolvedText.find("{item}");
	if (itemPos != std::string::npos)
	{
		GameData* itemData = itemId.empty() ? nullptr : getGameDataGuarded(itemId, ITEM);
		if (itemData)
			resolvedText.replace(itemPos, 6, itemData->name);
		else
			ErrorLog("SkeletonRebirth: " + callerLabel + " has \"{item}\" in its text but no resolvable \"item\" (\"" + itemId + "\") - left as-is");
	}

	return resolvedText;
}

static void showFloatingText(Character* patient, const std::string& text, const std::string& colorHex, const std::string& itemId)
{
	if (!gui || text.empty())
		return;

	std::string resolvedText = resolvePlaceholders(text, patient, itemId, "dialogue step \"show_text\"");

	MyGUI::Colour color = MyGUI::Colour::White;
	if (!colorHex.empty() && !tryParseHexColor(colorHex, color))
		ErrorLog("SkeletonRebirth: dialogue step \"show_text\" has unrecognized color \"" + colorHex + "\" (expected \"#RRGGBB\") - defaulting to white");

	ScreenLabel* label = gui->createScreenLabel(resolvedText, color, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
	if (label)
		label->setTracking(patient->getHandle(), Ogre::Vector3(0, 1, 0));
}

// Kenshi's own native player-notification system - the corner text queue used for things like "<name>
// is dead" - distinct from show_text's ScreenLabel (floating over the character) and from the
// dialogue box itself. showPlayerAMessage_withLog() (vs. the plain non-"_withLog" variant) also
// records it into whatever history/log that notification queue keeps, matching how a native message
// like that would normally behave. queued=true so it takes its place behind any other pending
// notification instead of replacing one that's still showing.
static void showGameNotification(Character* patient, const std::string& text, const std::string& itemId)
{
	if (!ou || text.empty())
		return;

	std::string resolvedText = resolvePlaceholders(text, patient, itemId, "dialogue step \"notify\"");
	ou->showPlayerAMessage_withLog(resolvedText, true);
}

// Resumed from Character_updateOnScreenCheck_hook, which already polls every frame per character.
// GetTickCount64() wall-clock rather than a per-frame delta since that hook receives no frameTime.
struct PendingDialogueSequence
{
	std::vector<DialogueBoxStepDef> steps; // copied, not referenced - see dispatchDialogueSteps()
	size_t nextIndex;
	Character* initiator;
	bool waitForEditorClose; // if true, fireAtTick is unused - resumes once gui->isCharacterEditorMode() clears instead
	ULONGLONG fireAtTick;

	PendingDialogueSequence() : nextIndex(0), initiator(nullptr), waitForEditorClose(false), fireAtTick(0) {}
};
static std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences;

// Forward-declared - showDialogueBox() is defined further down (it needs isDialogueButtonEligible()
// and the layout-loading machinery below), but "open_menu" steps here need to call back into it.
static void showDialogueBox(const std::string& dialogueId, Character* patient, Character* initiator);

// A "delay" step suspends the rest of `steps` in g_pendingDialogueSequences and returns rather than
// blocking; resumed later from Character_updateOnScreenCheck_hook.
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
			Inventory* inv = initiator ? initiator->getInventory() : nullptr;
			GameData* itemData = inv ? getGameDataGuarded(step.item, ITEM) : nullptr;
			if (!inv || !itemData || !inv->takeOneItemOnly(itemData))
			{
				ErrorLog("SkeletonRebirth: dialogue step \"take_item\" (\"" + step.item + "\") failed for " + patient->getName() + " - stopping the rest of this sequence");
				return;
			}
			continue;
		}

		if (step.type == "show_text")
		{
			showFloatingText(patient, step.text, step.color, step.item);
			continue;
		}

		if (step.type == "notify")
		{
			showGameNotification(patient, step.text, step.item);
			continue;
		}

		if (step.type == "action")
		{
			auto actionIt = g_dialogueActions.find(step.action);
			if (actionIt != g_dialogueActions.end())
				actionIt->second(patient);
			else
				ErrorLog("SkeletonRebirth: dialogue step action \"" + step.action + "\" has no registered handler");

			// join_squad specifically (the "with edit" recruit path, as opposed to join_squad_fast)
			// opens the character editor - implicit, not a JSON-authored step, so a JSON author can't
			// forget it and have a later step (e.g. system_reset) run while the editor's still open and
			// possibly about to change the very things that step just set.
			if (step.action == "join_squad" && gui && gui->isCharacterEditorMode())
			{
				PendingDialogueSequence pending;
				pending.steps = steps;
				pending.nextIndex = i + 1;
				pending.initiator = initiator;
				pending.waitForEditorClose = true;
				g_pendingDialogueSequences[patient] = pending;
				return;
			}

			continue;
		}

		if (step.type == "open_menu")
		{
			// Terminal, like "delay" and a failed "take_item" - by the time a submenu is up and waiting
			// on the player's next click, any further steps from this sequence have no clear meaning.
			// The box that triggered this step is already gone (MessageBoxManager destroys it as part
			// of the click that led here, before OnMessageBoxButtonClicked ever dispatches), so
			// showDialogueBox()'s one-at-a-time guard doesn't block this.
			showDialogueBox(step.menu, patient, initiator);
			return;
		}

		ErrorLog("SkeletonRebirth: unknown dialogue step type \"" + step.type + "\" for " + patient->getName());
	}
}

// IDelegate1<int> callback for MessageBoxManager::createMessageBox() - buttonId is whichever int we
// paired each button's caption with in showDialogueBox() below (its index into g_currentDialogueButtons).
// MessageBoxManager has already destroyed the box's native Window by the time this fires, so there's
// no widget teardown to do here - just our own bookkeeping and dispatching the clicked button's steps.
static void OnMessageBoxButtonClicked(int buttonId)
{
	Character* patient = g_pendingDialoguePatient;
	Character* initiator = g_pendingDialogueInitiator;

	DialogueBoxButtonDef btn;
	bool hasBtn = (buttonId >= 0 && buttonId < (int)g_currentDialogueButtons.size());
	if (hasBtn)
		btn = g_currentDialogueButtons[buttonId];

	closeDialogueBox();

	if (!patient || !hasBtn)
		return;

	dispatchDialogueSteps(patient, initiator, btn.steps, 0);
}

// `initiator` is who "requiresItem"/"requiresSkill"/"take_item" are checked against - null is fine
// for a dialogue with no gated buttons or item steps.
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

	// Capped at 3 - Kenshi_MessageBox.layout (which createMessageBox() almost certainly still builds
	// on internally, going by its fixed-size ButtonA/B/C widgets) only ever had 3 button slots, and
	// createMessageBox()'s generic vector<pair<string,int>> signature doesn't confirm it supports more.
	std::vector<DialogueBoxButtonDef> eligibleButtons;
	for (size_t i = 0; i < def.buttons.size() && eligibleButtons.size() < 3; ++i)
	{
		if (isDialogueButtonEligible(def.buttons[i], patient, initiator))
			eligibleButtons.push_back(def.buttons[i]);
	}
	if (eligibleButtons.empty())
	{
		ErrorLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" has no eligible buttons for this initiator - not shown");
		return;
	}

	std::string message = resolvePlaceholders(def.message, patient, def.item, "dialogue box \"" + dialogueId + "\"");

	// Ogre::vector<T>::type, not plain std::vector<T> - createMessageBox()'s "buttons" parameter is
	// declared with Ogre's own custom-allocator vector, a distinct type from the plain-std one.
	Ogre::vector<std::pair<std::string, int> >::type buttons;
	for (size_t i = 0; i < eligibleButtons.size(); ++i)
		buttons.push_back(std::make_pair(eligibleButtons[i].caption, (int)i));

	g_pendingDialoguePatient = patient;
	g_pendingDialogueInitiator = initiator;
	g_currentDialogueButtons = eligibleButtons;

	MyGUI::Window* window = MessageBoxManager_createMessageBox_PLACEHOLDER(def.title, message, buttons, /*modal*/ true, MyGUI::newDelegate(OnMessageBoxButtonClicked));
	if (!window)
	{
		ErrorLog("SkeletonRebirth: MessageBoxManager::createMessageBox() returned null for \"" + dialogueId + "\"");
		closeDialogueBox();
		return;
	}

	verboseLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" shown for " + patient->getName());
}

static void DialogueAction_Reactivate(Character* patient)
{
	if (!TryReactivate(patient))
		ErrorLog("SkeletonRebirth: TryReactivate failed for " + patient->getName() + " after confirm");

	clearTriggerShownFor(patient);
}

// Split out from DialogueAction_SystemReset so a dialogue button can join the patient to the player's
// squad independently of resetting its stats (e.g. joining without a reset, or a reset that doesn't
// join). This is recruit()'s "with edit" mode (editor=true) - matches the native DA_JOIN_SQUAD_WITH_EDIT
// dialogue action - and it's this exact mode that reinitializes the character's CharStats from its
// original template, which is why it must run BEFORE system_reset whenever a button uses both (see that
// function's comment). Use DialogueAction_JoinSquadFast below instead if a button just wants the
// character in the squad without touching its stats. JSON authors are responsible for step order;
// "join_squad" needs to be listed ahead of "system_reset" in the steps array for the reset to stick.
static void DialogueAction_JoinSquad(Character* patient)
{
	if (ou && ou->player)
		ou->player->recruit(patient, true);
}

// recruit()'s "fast" mode (editor=false) - matches the native DA_JOIN_SQUAD_FAST dialogue action -
// skips the character-editing reinitialization DialogueAction_JoinSquad's editor=true triggers, so
// unlike that one, this has no known ordering interaction with system_reset.
static void DialogueAction_JoinSquadFast(Character* patient)
{
	if (ou && ou->player)
		ou->player->recruit(patient, false);
}

// Must run AFTER DialogueAction_JoinSquad if a button uses both (see that function's comment) -
// recruit(editor=true) reinitializes CharStats from the character's original template, which would
// silently clobber this reset back to the character's pre-reset (modified) values if it ran second.
// CharStats* is fetched fresh here rather than passed in from JoinSquad, in case recruit() replaces
// the object rather than mutating it in place.
//
// ANIMAL_CHARACTER-type robots (isAnimalCharacterType() - e.g. Iron Spiders) don't get their skills
// and attributes wiped to 1 like humanoid robots do - age is what governs an animal's growth stage, so
// "reset" means taking that to its minimum instead. setAge()/getAge() must be called through true
// virtual dispatch (patient->setAge(...), not patient->_NV_setAge(...)) - CharacterAnimal overrides
// both with its own backing fields at separate RVAs, and the _NV_ variants silently no-op by running
// Character's base implementation regardless of the object's real type - see DESIGN.md's virtual-
// function pitfall note.
static void DialogueAction_SystemReset(Character* patient)
{
	if (isAnimalCharacterType(patient))
	{
		patient->setAge(0.3f);
		return;
	}

	CharStats* stats = patient->getStats();
	if (stats)
	{
		for (auto it = g_skillFields.begin(); it != g_skillFields.end(); ++it)
			stats->*(it->second) = 1.0f;
		for (auto it = g_attributeFields.begin(); it != g_attributeFields.end(); ++it)
			stats->*(it->second) = 1.0f;
	}
}

// Mod DLLs don't get a working directory of their own - this is how the mod locates RE_Kenshi.json,
// which sits next to the DLL.
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

// Sets g_debugLoggingEnabled from RE_Kenshi.json's top-level "Debug" (bool, default false/absent).
// Must run before the other loaders below, so their own verboseLog() summary lines respect it. Silent
// on a missing file or parse error - the other loaders open the same file and already report those;
// no need to say it three times. A missing/absent "Debug" key just means logging stays off, the safe
// default, not an error worth reporting.
static void loadDebugSettingFromJson()
{
	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	std::ifstream file(jsonPath.c_str());
	if (!file.is_open())
		return;

	rapidjson::IStreamWrapper isw(file);
	rapidjson::Document doc;
	if (doc.ParseStream(isw).HasParseError())
		return;

	if (doc.IsObject() && doc.HasMember("Debug") && doc["Debug"].IsBool())
		g_debugLoggingEnabled = doc["Debug"].GetBool();
}

static void loadDialogueBoxesFromJson()
{
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
		def.item = (v.HasMember("item") && v["item"].IsString()) ? v["item"].GetString() : "";

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
				if (bit->HasMember("excludePlayerFaction") && (*bit)["excludePlayerFaction"].IsBool())
					btn.excludePlayerFaction = (*bit)["excludePlayerFaction"].GetBool();
				if (bit->HasMember("requiresPlayerFaction") && (*bit)["requiresPlayerFaction"].IsBool())
					btn.requiresPlayerFaction = (*bit)["requiresPlayerFaction"].GetBool();
				if (bit->HasMember("requiresDeactivated") && (*bit)["requiresDeactivated"].IsBool())
					btn.requiresDeactivated = (*bit)["requiresDeactivated"].GetBool();
				if (bit->HasMember("requiresAnimal") && (*bit)["requiresAnimal"].IsBool())
					btn.requiresAnimal = (*bit)["requiresAnimal"].GetBool();
				if (bit->HasMember("excludeAnimal") && (*bit)["excludeAnimal"].IsBool())
					btn.excludeAnimal = (*bit)["excludeAnimal"].GetBool();

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
						if (sit->HasMember("menu") && (*sit)["menu"].IsString())
							step.menu = (*sit)["menu"].GetString();
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
	verboseLog(summary.str());
}

static void loadDialogueTriggersFromJson()
{
	std::string jsonPath = getOwnModDirectory() + "RE_Kenshi.json";
	std::ifstream file(jsonPath.c_str());
	if (!file.is_open())
	{
		ErrorLog("SkeletonRebirth: could not open \"" + jsonPath + "\" - no dialogue triggers will fire");
		return;
	}

	rapidjson::IStreamWrapper isw(file);
	rapidjson::Document doc;
	if (doc.ParseStream(isw).HasParseError())
	{
		ErrorLog("SkeletonRebirth: JSON parse error in \"" + jsonPath + "\": " + rapidjson::GetParseError_En(doc.GetParseError()));
		return;
	}

	if (!doc.IsObject() || !doc.HasMember("DialogueTriggers") || !doc["DialogueTriggers"].IsArray())
	{
		ErrorLog("SkeletonRebirth: \"" + jsonPath + "\" has no \"DialogueTriggers\" array - no dialogue triggers will fire");
		return;
	}

	const rapidjson::Value& triggers = doc["DialogueTriggers"];
	for (rapidjson::SizeType i = 0; i < triggers.Size(); ++i)
	{
		const rapidjson::Value& t = triggers[i];
		std::ostringstream label;
		label << "DialogueTriggers[" << i << "]";

		if (!t.IsObject())
		{
			ErrorLog("SkeletonRebirth: " + label.str() + " is not an object - skipped");
			continue;
		}

		DialogueTriggerDef trigger;
		trigger.menu = (t.HasMember("menu") && t["menu"].IsString()) ? t["menu"].GetString() : "";

		if (t.HasMember("requiredStates") && t["requiredStates"].IsArray())
		{
			const rapidjson::Value& states = t["requiredStates"];
			for (rapidjson::SizeType j = 0; j < states.Size(); ++j)
			{
				if (states[j].IsString())
					trigger.requiredStates.push_back(states[j].GetString());
			}
		}

		if (trigger.requiredStates.empty() || trigger.menu.empty())
		{
			ErrorLog("SkeletonRebirth: " + label.str() + " missing \"requiredStates\" or \"menu\" - skipped");
			continue;
		}
		for (size_t j = 0; j < trigger.requiredStates.size(); ++j)
		{
			if (!g_triggerStateChecks.count(trigger.requiredStates[j]))
				ErrorLog("SkeletonRebirth: " + label.str() + " references unknown requiredState \"" + trigger.requiredStates[j] + "\" - will never fire");
		}

		if (t.HasMember("buildings") && t["buildings"].IsArray())
		{
			const rapidjson::Value& buildings = t["buildings"];
			for (rapidjson::SizeType j = 0; j < buildings.Size(); ++j)
			{
				if (buildings[j].IsString())
					trigger.buildings.push_back(buildings[j].GetString());
			}
		}
		if (trigger.buildings.empty())
			ErrorLog("SkeletonRebirth: " + label.str() + " has no \"buildings\" - will never fire");

		g_dialogueTriggers.push_back(trigger);
	}

	std::ostringstream summary;
	summary << "SkeletonRebirth: loaded " << g_dialogueTriggers.size() << " dialogue trigger(s) from \"" << jsonPath << "\"";
	verboseLog(summary.str());
}

// Character::update() is deliberately NOT hooked - skipping it breaks position syncing while carried
// and causes a rigid, non-ragdolling corpse. MedicalSystem::medicalUpdate() stays frozen regardless,
// since it's hooked independently of caller.

// --- Hook 2: MedicalSystem::medicalUpdate() ---------------------------------------------------
// Freezes a Deactivated character's medical simulation - no health change either direction, and stops
// declareDead() from re-triggering. MedicalSystem has no owning-Character* field, so reach it via
// getPart(...)->me. Reactivation is polled from Character_updateOnScreenCheck_hook instead of here -
// this hook doesn't fire for a genuinely dead character, which is what an earlier dead=true design hit.
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

// requiredStates are checked (in order, short-circuiting on the first failure) before touching
// inSomething/RootObject/Building at all - see the comment on DialogueTriggerDef for why that
// ordering is load-bearing, not stylistic. Clears this trigger's shown flag on any early-out, so a
// later re-match (re-entering the state(s), or re-entering the building) can prompt again - mirrors
// the old single-trigger "leaving the bed clears it" behavior, just per-trigger.
static void evaluateDialogueTrigger(Character* self, size_t triggerIndex, const DialogueTriggerDef& trigger)
{
	std::pair<Character*, size_t> shownKey(self, triggerIndex);

	for (size_t i = 0; i < trigger.requiredStates.size(); ++i)
	{
		TriggerStateCheckFn stateCheck = g_triggerStateChecks.count(trigger.requiredStates[i]) ? g_triggerStateChecks[trigger.requiredStates[i]] : nullptr;
		if (!stateCheck || !stateCheck(self))
		{
			g_triggerShown.erase(shownKey);
			return;
		}
	}

	if (self->inSomething != IN_BED)
	{
		g_triggerShown.erase(shownKey);
		return;
	}

	RootObject* obj = self->inWhat.getRootObject();
	Building* building = obj ? static_cast<Building*>(obj) : nullptr;
	GameData* buildingData = building ? building->getGameData() : nullptr;

	bool buildingMatches = false;
	for (size_t i = 0; !buildingMatches && buildingData && i < trigger.buildings.size(); ++i)
		buildingMatches = (trigger.buildings[i] == buildingData->stringID);

	if (!buildingMatches)
	{
		g_triggerShown.erase(shownKey);
		return;
	}

	if (g_triggerShown.count(shownKey))
		return; // already shown, waiting on the player to act or leave

	Character* initiator = (ou && ou->player) ? ou->player->getAnyPlayerCharacter() : nullptr;
	showDialogueBox(trigger.menu, self, initiator);
	g_triggerShown[shownKey] = true;
}

// --- Hook: Character::updateOnScreenCheck() - dialogue trigger polling point -------------------
// Fires every frame per character - also doubles as the tick source for resuming paused dialogue
// step sequences below, regardless of self's own trigger status.
bool (*Character_updateOnScreenCheck_orig)(Character*);

bool Character_updateOnScreenCheck_hook(Character* self)
{
	bool result = Character_updateOnScreenCheck_orig(self);

	auto delayIt = g_pendingDialogueSequences.find(self);
	if (delayIt != g_pendingDialogueSequences.end())
	{
		bool ready = delayIt->second.waitForEditorClose
			? !(gui && gui->isCharacterEditorMode())
			: GetTickCount64() >= delayIt->second.fireAtTick;

		if (ready)
		{
			PendingDialogueSequence pending = delayIt->second;
			g_pendingDialogueSequences.erase(delayIt);
			dispatchDialogueSteps(self, pending.initiator, pending.steps, pending.nextIndex);
		}
	}

	for (size_t i = 0; i < g_dialogueTriggers.size(); ++i)
		evaluateDialogueTrigger(self, i, g_dialogueTriggers[i]);

	return result;
}

// --- Hook: DatapanelGUI::setLine(key,s1,s2,category,last,keyVisible) - status tag override -------
// The "State:" row is overridden unconditionally for any tracked Deactivated character - since dead
// stays false, the corpse-only native "Dead" text never fires, so whatever text a fatally-frozen-but-
// alive character would show (likely "Dying"/"Critical") is replaced regardless of its actual content.
static const char* DEACTIVATED_COLOR = "#59231a"; // vanilla's own dark red, same as its "Dead" text
DataPanelLine* (*DatapanelGUI_setLine_KeyLastVisible_orig)(DatapanelGUI*, const std::string&, const std::string&, const std::string&, int, bool, bool);
DataPanelLine* DatapanelGUI_setLine_KeyLastVisible_hook(DatapanelGUI* self, const std::string& keyValue, const std::string& s1, const std::string& s2, int category, bool last, bool keyVisible)
{
	Character* target = self->getObject().getCharacter();
	if (!target || !g_deactivated.count(target))
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);

	if (keyValue == "State:")
	{
		std::string overriddenS2 = std::string(DEACTIVATED_COLOR) + "POWER FAILURE";
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, overriddenS2, category, last, keyVisible);
	}

	return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);
}

// --- Robot limb race-lock (merged in from the former "The Limbless (Type 2)" mod) --------------
// Unrelated to the Deactivated/reactivation system above - this is a standalone fix for vanilla
// robot limbs being equippable onto any race. Kept as its own self-contained section (own map, own
// two hooks) rather than woven into the reactivation code, since the two features share no state.
//
// Vanilla stores each item's FCS "Race Limiter" (racesInclude/racesExclude) in RaceLimiter's
// singleton, populated lazily via addLimit() the first time something looks the item up - nothing on
// the robot-limb equip path ever calls that, so the cache stays empty and canEquip() always sees
// "unrestricted" for limbs. RobotLimbs::inventory is a cached "interface" RootObject* representing a
// character's limb-slot section in the GUI - this is exactly the object vanilla passes as "who"
// during the drag/equip race-check, but its getName() is a generic "ROBOTICS" label, not tied to the
// owning character. getInventoryInterface() is hooked to record interface-pointer -> owning-character
// every time one is (re)built, so canEquip()'s hook can look up the true recipient from "who" later -
// correct in all cases, including multi-character trades, no guessing needed.
static std::map<RootObject*, Character*> g_limbInterfaceOwners;

RootObject* (*RobotLimbs_getInventoryInterface_orig)(RobotLimbs*, bool);
RootObject* RobotLimbs_getInventoryInterface_hook(RobotLimbs* self, bool create)
{
	RootObject* result = RobotLimbs_getInventoryInterface_orig(self, create);
	if (result && self->character)
		g_limbInterfaceOwners[result] = self->character;
	return result;
}

// Primes the RaceLimiter cache for this item (see comment above), then substitutes "who" for the
// item's true owning Character* before deferring to vanilla's own equip validation, which then
// enforces the race restriction correctly on its own.
bool (*RaceLimiter_canEquip2_orig)(RaceLimiter*, GameData*, RootObject*);
bool RaceLimiter_canEquip2_hook(RaceLimiter* self, GameData* item, RootObject* who)
{
	RaceLimiter::getSingleton()->addLimit(item);

	RootObject* correctedWho = who;
	auto it = g_limbInterfaceOwners.find(who);
	if (it != g_limbInterfaceOwners.end())
		correctedWho = (RootObject*)it->second;

	return RaceLimiter_canEquip2_orig(self, item, correctedWho);
}

__declspec(dllexport) void startPlugin()
{
	g_dialogueActions["reactivate"] = &DialogueAction_Reactivate;
	g_dialogueActions["join_squad"] = &DialogueAction_JoinSquad;
	g_dialogueActions["join_squad_fast"] = &DialogueAction_JoinSquadFast;
	g_dialogueActions["system_reset"] = &DialogueAction_SystemReset;
	g_triggerStateChecks["deactivated"] = &isDeactivatedState;
	g_triggerStateChecks["animal"] = &isAnimalCharacterType;
	g_triggerStateChecks["humanoid"] = &isHumanoidState;
	loadDebugSettingFromJson();
	loadDialogueBoxesFromJson();
	loadDialogueTriggersFromJson();

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

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RobotLimbs::getInventoryInterface), RobotLimbs_getInventoryInterface_hook, &RobotLimbs_getInventoryInterface_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add RobotLimbs::getInventoryInterface hook!");

	// canEquip is virtual, so this goes through the exported _NV_ (non-virtual) stub - GetRealAddress
	// doesn't work on &Class::VirtualMethod directly (see DESIGN.md's hooking-virtual-functions
	// pitfall). It's overloaded, hence the cast.
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((bool(RaceLimiter::*)(GameData*, RootObject*))&RaceLimiter::_NV_canEquip), RaceLimiter_canEquip2_hook, &RaceLimiter_canEquip2_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add RaceLimiter::canEquip(item,who) hook!");
}
