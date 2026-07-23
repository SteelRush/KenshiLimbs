#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/GameSaveState.h>
#include <kenshi/util/hand.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Building/Building.h>
#include <kenshi/CharStats.h>
#include <kenshi/Faction.h>
#include <kenshi/Inventory.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Platoon.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MainBarGUI.h>
#include <kenshi/gui/MessageBoxManager.h>
#include <kenshi/gui/ScreenLabel.h>

#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Window.h>

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
#include <set>
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

// MessageBoxManager.h's declaration compiles but fails to link - see DESIGN.md §3/reference index.
extern "C" MyGUI::Window* MessageBoxManager_createMessageBox_PLACEHOLDER(
	const std::string& title,
	const std::string& message,
	const Ogre::vector<std::pair<std::string, int> >::type& buttons,
	bool modal,
	MyGUI::delegates::IDelegate1<int>* callback);

// SkeletonRebirth plugin - see DESIGN.md for full architecture, history, and rejected approaches.

// Gates DebugLog() (not ErrorLog()) behind RE_Kenshi.json's "Debug" setting - see DESIGN.md.
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

// FCS's Human/Animal Character category - not Character::isAnimal(), see DESIGN.md reference index.
static bool isAnimalCharacterType(Character* c)
{
	GameData* data = c->getGameData();
	return data && data->type == ANIMAL_CHARACTER;
}

// Not enough alone to decide AI Core vs Power Core - callers combine with isAnimalCharacterType()/
// isHumanoidState() too, see DESIGN.md §2.
static bool hasHeadPart(Character* c)
{
	MedicalSystem* med = c->getMedical();
	return med && med->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER) != nullptr;
}

// Character*-keyed and session-only - no persistence file needed, see DESIGN.md §6.
static std::map<Character*, bool> g_deactivated;

// Level-triggered, keyed by (character, trigger index) - see DESIGN.md §5.
static std::map<std::pair<Character*, size_t>, bool> g_triggerShown;

// patient -> caregiver, refreshed each frame - see DESIGN.md §5.
static std::map<Character*, Character*> g_activeFirstAid;

// Called after an action that could invalidate more than one trigger at once - see DESIGN.md §5.
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

// Re-derives g_deactivated membership from native data instead of a persisted ID - see DESIGN.md §6.
static bool hasDeactivatedSignature(Character* self)
{
	MedicalSystem* med = self->getMedical();
	if (!med || med->dead)
		return false;

	for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
	{
		MedicalSystem::HealthPartStatus* part = *it;
		if (part && part->isDead())
			return true;
	}

	return false;
}

// Do not set MedicalSystem::dead=true - see DESIGN.md §1.

// MedicalSystem::anatomy is the complete part list - see DESIGN.md §3.
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
		std::string name = part->data ? part->data->name : "?";
		ss << " " << label << "(name=\"" << name << "\" type=" << (int)part->whatAmI << " side=" << (int)part->side
		   << " flesh=" << part->flesh << " fleshStun=" << part->fleshStun << " wearDamage=" << part->wearDamage
		   << " bandaging=" << part->bandaging << " juryRigging=" << part->juryRigging
		   << " maxHealth=" << part->maxHealth() << " derivedFleshHealthPercent=" << part->derivedFleshHealthPercent
		   << " fatal=" << part->fatal << " isDead=" << part->isDead() << ")";
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

// Item-triggered instant death doesn't deplete flesh like combat does - see DESIGN.md §1.
bool (*Inventory_deathCheck_orig)(Inventory*, Item*);
bool Inventory_deathCheck_hook(Inventory* self, Item* item)
{
	bool result = Inventory_deathCheck_orig(self, item);

	Character* owner = self->getCallbackCharacter();
	if (result && owner && isRobotRace(owner))
	{
		MedicalSystem* med = owner->getMedical();
		if (med)
		{
			// Head/Chest/Stomach only, whichever is already weakest - see DESIGN.md §1.
			MedicalSystem::HealthPartStatus* weakest = nullptr;
			for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
			{
				MedicalSystem::HealthPartStatus* part = *it;
				if (!part)
					continue;

				bool isVital = part->whatAmI == MedicalSystem::HealthPartStatus::PART_HEAD
					|| part->whatAmI == MedicalSystem::HealthPartStatus::PART_TORSO;
				if (isVital && (!weakest || (part->flesh - part->fleshStun) < (weakest->flesh - weakest->fleshStun)))
					weakest = part;
			}

			if (weakest)
			{
				// Solves for flesh given the part's existing fleshStun - see DESIGN.md §1.
				weakest->flesh = -weakest->maxHealth() * 0.965f + weakest->fleshStun;
				weakest->updateDerivedHealths();
			}
		}
		verboseLog("SkeletonRebirth: forced fatal flesh for item-death signature -> " + describe(owner));
	}

	return result;
}

// --- Hook 1: declareDead() -----------------------------------------------------------------
// Blocks the real death transition for robots - see DESIGN.md §1.
void (*Character_declareDead_orig)(Character*);
void Character_declareDead_hook(Character* self)
{
	if (isRobotRace(self))
	{
		// Must be reset every call, not just left alone - see DESIGN.md §1.
		MedicalSystem* med = self->getMedical();
		if (med)
			med->dead = false;

		// Fires repeatedly per character - gated here to stay a cheap hot path, see DESIGN.md §1.
		if (g_deactivated.count(self))
			return;

		g_deactivated[self] = true;

		// Deactivation is otherwise silent (dead never becomes true) - see DESIGN.md §1.
		Faction* faction = self->getFaction();
		if (faction && faction->isThePlayer() && ou)
			ou->showPlayerAMessage_withLog(self->getName() + " has died.", true);

		verboseLog("SkeletonRebirth: declareDead() BLOCKED for robot -> " + describe(self));
		return;
	}

	Character_declareDead_orig(self);
}

static bool isCriticalPart(MedicalSystem::HealthPartStatus* part)
{
	return part && (part->whatAmI == MedicalSystem::HealthPartStatus::PART_TORSO
		|| part->whatAmI == MedicalSystem::HealthPartStatus::PART_HEAD);
}

// Floors flesh at -95% of maxHealth rather than a relative nudge - see DESIGN.md §3.
static void nudgeFleshTowardSurvivable(MedicalSystem::HealthPartStatus* part)
{
	if (!part)
		return;

	// Floor accounts for fleshStun the same way Inventory_deathCheck_hook does - see DESIGN.md §1.
	float floor = -part->maxHealth() * 0.95f + part->fleshStun;
	if (part->flesh < floor)
		part->flesh = floor;

	float cap = part->maxHealth();
	if (part->flesh > cap)
		part->flesh = cap;

	part->updateDerivedHealths();
}

// Only critical parts (torso/head) are nudged, via anatomy iteration not getPart() - see DESIGN.md §3.
bool TryReactivate(Character* self)
{
	auto deactivatedIt = g_deactivated.find(self);
	if (deactivatedIt == g_deactivated.end() || !deactivatedIt->second)
		return false; // not actually Deactivated - nothing to do

	MedicalSystem* med = self->getMedical();
	if (med)
	{
		for (auto it = med->anatomy.begin(); it != med->anatomy.end(); ++it)
			if (isCriticalPart(*it))
				nudgeFleshTowardSurvivable(*it);
	}

	g_deactivated.erase(self);
	verboseLog("SkeletonRebirth: TryReactivate() SUCCEEDED -> " + describe(self));

	return true;
}

// Confirmation UI built via MessageBoxManager::createMessageBox() - see DESIGN.md §3.
//
// --- JSON-driven dialogue boxes -----------------------------------------------------------------
// Box text and button behavior are data-driven from RE_Kenshi.json's "DialogueBoxes" object - see
// DESIGN.md §4 for the step types, nested-menu semantics, and button-gating fields below.
struct DialogueBoxItemRequirementDef
{
	std::string item; // FCS/GameData item String ID
	int count;         // how many of it are required - hidden (or, for "take_item", refused) unless initiator has at least this many

	DialogueBoxItemRequirementDef() : count(1) {}
};

struct DialogueBoxStepDef
{
	std::string type; // "action" | "take_item" | "show_text" | "notify" | "delay" | "open_menu" | "await_repair"
	std::string action; // for "action" - looked up in g_dialogueActions
	std::string item;   // for "show_text"/"notify" - resolves "{item}" in text, no consuming
	std::vector<DialogueBoxItemRequirementDef> items; // for "take_item" - same {item, count} shape as a button's requiresItems; re-checked at click time and consumed on success
	std::string text;   // for "show_text"/"notify" - "{name}"/"{item}" replaced with the patient's name / item's display name
	std::string color;  // for "show_text", optional - "#RRGGBB" hex, defaults to white
	float seconds;       // for "delay", pauses this many seconds (wall clock); for "await_repair", timeout - see DESIGN.md §3
	std::string menu;    // for "open_menu" - a "DialogueBoxes" key to open, e.g. a submenu or "back" target

	DialogueBoxStepDef() : seconds(0.0f) {}
};

struct DialogueBoxButtonDef
{
	std::string caption; // ~10 character limit before clipping - see DESIGN.md §3
	std::vector<DialogueBoxStepDef> steps; // run in order when clicked - see dispatchDialogueSteps()
	std::vector<DialogueBoxItemRequirementDef> requiresItems; // hidden unless initiator has every one, at its required count
	std::string requiresSkill; // lowercase CharStats field name (see g_skillFields) - hidden unless initiator's skill is in range
	bool hasMinSkill;
	float minSkill;
	bool hasMaxSkill;
	float maxSkill;
	std::string requiresSkill2; // second, independently-gated skill - see DESIGN.md §4, "Button-level gating"
	bool hasMinSkill2;
	float minSkill2;
	bool hasMaxSkill2;
	float maxSkill2;
	bool excludePlayerFaction; // hidden if the patient belongs to the player's faction
	bool requiresPlayerFaction; // hidden unless the patient belongs to the player's faction - the counterpart to excludePlayerFaction
	bool requiresDeactivated; // hidden unless the patient is in g_deactivated - see DESIGN.md §2
	bool excludeDeactivated; // hidden while the patient is in g_deactivated - the counterpart to requiresDeactivated
	bool requiresAnimal; // hidden unless isAnimalCharacterType() - lets an animal-only button require a different item than the humanoid one
	bool excludeAnimal; // hidden if isAnimalCharacterType() - the humanoid-only counterpart to requiresAnimal
	std::string requiresRace; // RaceData's display name (Character::getRace()->data->name) - hidden unless it matches exactly
	std::string excludeRace; // same match, inverted - the counterpart to requiresRace

	DialogueBoxButtonDef() : hasMinSkill(false), minSkill(0.0f), hasMaxSkill(false), maxSkill(0.0f), hasMinSkill2(false), minSkill2(0.0f), hasMaxSkill2(false), maxSkill2(0.0f), excludePlayerFaction(false), requiresPlayerFaction(false), requiresDeactivated(false), excludeDeactivated(false), requiresAnimal(false), excludeAnimal(false) {}
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
// "When does a dialogue box open" is data-driven the same way button behavior is - see DESIGN.md §5.
struct DialogueTriggerDef
{
	std::vector<std::string> requiredStates; // all looked up in g_triggerStateChecks, all must pass
	std::vector<std::string> buildings; // FCS building String IDs - any one match is enough; empty = no building requirement
	std::string menu; // a "DialogueBoxes" key to open once requiredStates and buildings both match
	std::string requiresQualifiedFor; // menu name - hidden unless initiatorQualifiesForMenu() passes for it - see DESIGN.md §5
	std::string excludeQualifiedFor; // counterpart - hidden if it passes
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

// See DESIGN.md §5.
static bool isInBedState(Character* c)
{
	return c->inSomething == IN_BED;
}

// See DESIGN.md §3.
static bool isBeingRepairedState(Character* c)
{
	return g_activeFirstAid.count(c) > 0;
}

// Caregiver's skill, not the patient's - see DESIGN.md §3/§5.
static bool hasRepairerScience1(Character* c)
{
	auto it = g_activeFirstAid.find(c);
	Character* repairer = (it != g_activeFirstAid.end()) ? it->second : nullptr;
	CharStats* stats = repairer ? repairer->getStats() : nullptr;
	return stats && stats->science >= 1.0f;
}

// See DESIGN.md §3, "await_repair".
static const float AWAIT_REPAIR_ARRIVAL_DISTANCE = 250.0f;
static bool hasArrivedForRepair(Character* patient, Character* caregiver)
{
	if (!caregiver)
		return true;

	float distance = patient->getPosition().distance(caregiver->getPosition());
	std::ostringstream ss;
	ss << "SkeletonRebirth: DIAG await_repair distance -> caregiver=\"" << caregiver->getName() << "\" patient=\"" << patient->getName() << "\" distance=" << distance;
	verboseLog(ss.str());
	return distance <= AWAIT_REPAIR_ARRIVAL_DISTANCE;
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

// See DESIGN.md §4 for why requiresRace/excludeRace need this.
static std::string trimCopy(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
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

// Checks one character, not the squad - see DESIGN.md §5, "requiresQualifiedFor".
static bool characterHasSkill(Character* c, const std::string& skillField, bool hasMin, float minValue, bool hasMax, float maxValue)
{
	auto fieldIt = g_skillFields.find(toLowerCopy(skillField));
	if (fieldIt == g_skillFields.end())
		return true; // unrecognized skill name - treated as no requirement, same as squadHasSkill

	CharStats* stats = c ? c->getStats() : nullptr;
	if (!stats)
		return false;

	float value = stats->*(fieldIt->second);
	return (!hasMin || value >= minValue) && (!hasMax || value <= maxValue);
}

// Prefers the initiator themselves over the rest of the squad - see DESIGN.md §4, "Button-level gating".
static Character* findSquadMemberWithSkill(Character* initiator, const std::string& skillField, bool hasMin, float minValue, bool hasMax, float maxValue)
{
	auto fieldIt = g_skillFields.find(toLowerCopy(skillField));
	if (fieldIt == g_skillFields.end())
		return initiator; // unrecognized skill name - logged once at JSON-load time, treated as no requirement

	if (characterHasSkill(initiator, skillField, hasMin, minValue, hasMax, maxValue))
		return initiator;

	ActivePlatoon* platoon = initiator ? initiator->getPlatoon() : nullptr;
	if (!platoon)
		return nullptr;

	for (auto it = platoon->things.begin(); it != platoon->things.end(); ++it)
	{
		Character* member = hand(*it).getCharacter();
		if (member == initiator)
			continue; // already checked above

		CharStats* stats = member ? member->getStats() : nullptr;
		if (!stats)
			continue;
		float value = stats->*(fieldIt->second);
		if ((!hasMin || value >= minValue) && (!hasMax || value <= maxValue))
			return member;
	}
	return nullptr;
}

static bool squadHasSkill(Character* initiator, const std::string& skillField, bool hasMin, float minValue, bool hasMax, float maxValue)
{
	return findSquadMemberWithSkill(initiator, skillField, hasMin, minValue, hasMax, maxValue) != nullptr;
}

static bool initiatorHasEitherSkill(Character* initiator, const DialogueBoxButtonDef& btn)
{
	bool hasFirst = !btn.requiresSkill.empty() && characterHasSkill(initiator, btn.requiresSkill, btn.hasMinSkill, btn.minSkill, btn.hasMaxSkill, btn.maxSkill);
	bool hasSecond = !btn.requiresSkill2.empty() && characterHasSkill(initiator, btn.requiresSkill2, btn.hasMinSkill2, btn.minSkill2, btn.hasMaxSkill2, btn.maxSkill2);
	return hasFirst || hasSecond;
}

// Patient-only subset of isDialogueButtonEligible's checks - see DESIGN.md §4, "Button-level gating".
static bool isDialogueButtonForPatient(const DialogueBoxButtonDef& btn, Character* patient)
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

	if (btn.excludeDeactivated && g_deactivated.count(patient))
		return false;

	if (!btn.requiresRace.empty())
	{
		RaceData* race = patient->getRace();
		if (!race || !race->data || trimCopy(race->data->name) != trimCopy(btn.requiresRace))
			return false;
	}

	if (!btn.excludeRace.empty())
	{
		RaceData* race = patient->getRace();
		if (race && race->data && trimCopy(race->data->name) == trimCopy(btn.excludeRace))
			return false;
	}

	return true;
}

// See DESIGN.md §5, "requiresQualifiedFor".
static bool initiatorQualifiesForMenu(const std::string& menuId, Character* patient, Character* initiator)
{
	auto defIt = g_dialogueBoxes.find(menuId);
	if (defIt == g_dialogueBoxes.end())
		return true; // unknown menu - a JSON config error, not the initiator's fault; don't block on it

	bool anySkillGatedButton = false;
	for (size_t i = 0; i < defIt->second.buttons.size(); ++i)
	{
		const DialogueBoxButtonDef& btn = defIt->second.buttons[i];
		if (btn.requiresSkill.empty() && btn.requiresSkill2.empty())
			continue;
		if (!isDialogueButtonForPatient(btn, patient))
			continue;

		anySkillGatedButton = true;
		if (initiatorHasEitherSkill(initiator, btn))
			return true;
	}

	return !anySkillGatedButton;
}

// Gates whether a button is shown, not whether its steps can still run once clicked - the "take_item"
// step re-checks defensively (the item could be lost between show and click).
static bool isDialogueButtonEligible(const DialogueBoxButtonDef& btn, Character* patient, Character* initiator)
{
	if (!isDialogueButtonForPatient(btn, patient))
		return false;

	if (!btn.requiresSkill.empty() && !squadHasSkill(initiator, btn.requiresSkill, btn.hasMinSkill, btn.minSkill, btn.hasMaxSkill, btn.maxSkill))
		return false;

	if (!btn.requiresSkill2.empty() && !squadHasSkill(initiator, btn.requiresSkill2, btn.hasMinSkill2, btn.minSkill2, btn.hasMaxSkill2, btn.maxSkill2))
		return false;

	for (size_t i = 0; i < btn.requiresItems.size(); ++i)
	{
		const DialogueBoxItemRequirementDef& req = btn.requiresItems[i];
		Inventory* inv = initiator ? initiator->getInventory() : nullptr;
		GameData* itemData = inv ? getGameDataGuarded(req.item, ITEM) : nullptr;
		if (!inv || !itemData || !inv->hasItem(itemData, req.count))
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

// Shared by the dialogue box message, "show_text", and "notify" - see DESIGN.md §4.
static std::string resolvePlaceholders(const std::string& text, Character* patient, const std::string& itemId, bool hasItem, const std::string& skillCharacterName, const std::string& skillCharacterName2, const std::string& callerLabel)
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

	size_t itemStatusPos = resolvedText.find("{itemStatus}");
	if (itemStatusPos != std::string::npos)
		resolvedText.replace(itemStatusPos, 12, hasItem ? "You have one." : "You don't have one yet.");

	// Falls back to "No one" rather than erroring - see DESIGN.md §4.
	size_t skillPos = resolvedText.find("{skillCharacter}");
	if (skillPos != std::string::npos)
		resolvedText.replace(skillPos, 16, skillCharacterName.empty() ? "No one" : skillCharacterName);

	size_t skillPos2 = resolvedText.find("{skillCharacter2}");
	if (skillPos2 != std::string::npos)
		resolvedText.replace(skillPos2, 17, skillCharacterName2.empty() ? "No one" : skillCharacterName2);

	return resolvedText;
}

static void showFloatingText(Character* patient, const std::string& text, const std::string& colorHex, const std::string& itemId)
{
	if (!gui || text.empty())
		return;

	std::string resolvedText = resolvePlaceholders(text, patient, itemId, false, "", "", "dialogue step \"show_text\"");

	MyGUI::Colour color = MyGUI::Colour::White;
	if (!colorHex.empty() && !tryParseHexColor(colorHex, color))
		ErrorLog("SkeletonRebirth: dialogue step \"show_text\" has unrecognized color \"" + colorHex + "\" (expected \"#RRGGBB\") - defaulting to white");

	ScreenLabel* label = gui->createScreenLabel(resolvedText, color, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
	if (label)
		label->setTracking(patient->getHandle(), Ogre::Vector3(0, 1, 0));
}

// The "notify" step type - see DESIGN.md §4.
static void showGameNotification(Character* patient, const std::string& text, const std::string& itemId)
{
	if (!ou || text.empty())
		return;

	std::string resolvedText = resolvePlaceholders(text, patient, itemId, false, "", "", "dialogue step \"notify\"");
	ou->showPlayerAMessage_withLog(resolvedText, true);
}

// Resumed from Character_updateOnScreenCheck_hook, which already polls every frame per character.
// GetTickCount64() wall-clock rather than a per-frame delta since that hook receives no frameTime.
struct PendingDialogueSequence
{
	std::vector<DialogueBoxStepDef> steps; // copied, not referenced - see dispatchDialogueSteps()
	size_t nextIndex;
	Character* initiator;
	Character* specialist1; // resolved once at button-click time, carried through every suspension - see DESIGN.md §3
	Character* specialist2;
	ULONGLONG fireAtTick;
	bool awaitingRepair; // "await_repair" step - see DESIGN.md §3
	ULONGLONG repairTimeoutAtTick;
	bool showNamePrompt; // set by "join_squad" - see showNamePrompt() below

	PendingDialogueSequence() : nextIndex(0), initiator(nullptr), specialist1(nullptr), specialist2(nullptr), fireAtTick(0), awaitingRepair(false), repairTimeoutAtTick(0), showNamePrompt(false) {}
};
static std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences;

// Forward-declared - showDialogueBox() is defined further down (it needs isDialogueButtonEligible()
// and the layout-loading machinery below), but "open_menu" steps here need to call back into it.
static void showDialogueBox(const std::string& dialogueId, Character* patient, Character* initiator);

// A "delay"/"await_repair" step suspends the rest of `steps` in g_pendingDialogueSequences and returns
// rather than blocking; resumed later from Character_updateOnScreenCheck_hook.
static void dispatchDialogueSteps(Character* patient, Character* initiator, Character* specialist1, Character* specialist2, const std::vector<DialogueBoxStepDef>& steps, size_t startIndex)
{
	for (size_t i = startIndex; i < steps.size(); ++i)
	{
		const DialogueBoxStepDef& step = steps[i];

		verboseLog("SkeletonRebirth: DIAG dispatchDialogueSteps step " + std::to_string(i) + " type=\"" + step.type
			+ "\" action=\"" + step.action + "\" patient=\"" + patient->getName() + "\"");

		if (step.type == "delay")
		{
			if (step.seconds > 0.0f)
			{
				PendingDialogueSequence pending;
				pending.steps = steps;
				pending.nextIndex = i + 1;
				pending.initiator = initiator;
				pending.specialist1 = specialist1;
				pending.specialist2 = specialist2;
				pending.fireAtTick = GetTickCount64() + (ULONGLONG)(step.seconds * 1000.0f);
				g_pendingDialogueSequences[patient] = pending;
				return; // remaining steps resume later
			}
			continue;
		}

		if (step.type == "await_repair")
		{
			// Orders issued once here, not on every poll tick - see DESIGN.md §3.
			if (specialist1)
			{
				OrdersReceiver* orders = specialist1->getOrdersReciever();
				if (orders)
					orders->addOrder(FIRST_AID_ROBOT, hand(patient), patient->getPosition(), true, false);
			}
			if (specialist2 && specialist2 != specialist1)
			{
				OrdersReceiver* orders = specialist2->getOrdersReciever();
				if (orders)
					orders->addOrder(FIRST_AID_ROBOT, hand(patient), patient->getPosition(), true, false);
			}

			PendingDialogueSequence pending;
			pending.steps = steps;
			pending.nextIndex = i + 1;
			pending.initiator = initiator;
			pending.specialist1 = specialist1;
			pending.specialist2 = specialist2;
			pending.awaitingRepair = true;
			float timeoutSeconds = step.seconds > 0.0f ? step.seconds : 45.0f;
			pending.repairTimeoutAtTick = GetTickCount64() + (ULONGLONG)(timeoutSeconds * 1000.0f);
			g_pendingDialogueSequences[patient] = pending;
			return;
		}

		if (step.type == "take_item")
		{
			Inventory* inv = initiator ? initiator->getInventory() : nullptr;

			// Defensive re-check - "requiresItems" only gated the button when the box was first shown;
			// the initiator could have dropped/traded/consumed an item in the gap since then, including
			// during an earlier "delay"/"await_repair" suspension. Verify every requirement before
			// consuming any of them, so a shortfall on a later item doesn't leave an earlier one already
			// spent.
			const DialogueBoxItemRequirementDef* missing = nullptr;
			for (size_t itemIdx = 0; itemIdx < step.items.size() && !missing; ++itemIdx)
			{
				const DialogueBoxItemRequirementDef& req = step.items[itemIdx];
				GameData* itemData = inv ? getGameDataGuarded(req.item, ITEM) : nullptr;
				if (!inv || !itemData || !inv->hasItem(itemData, req.count))
					missing = &req;
			}

			if (missing)
			{
				// Not an error - the initiator simply doesn't have enough of this item anymore (could
				// have dropped/traded it away since the button was shown). Player-facing notification
				// above already communicates this; verboseLog() here is dev-facing and debug-gated,
				// not ErrorLog(), since nothing about the config or the JSON is actually wrong.
				showGameNotification(patient, "I don't have enough {item}.", missing->item);
				verboseLog("SkeletonRebirth: dialogue step \"take_item\" (\"" + missing->item + "\" x" + std::to_string((long long)missing->count) + ") short for " + patient->getName() + " - stopping the rest of this sequence");
				return;
			}

			for (size_t itemIdx = 0; itemIdx < step.items.size(); ++itemIdx)
			{
				const DialogueBoxItemRequirementDef& req = step.items[itemIdx];
				GameData* itemData = getGameDataGuarded(req.item, ITEM);
				for (int n = 0; n < req.count; ++n)
					inv->takeOneItemOnly(itemData);
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
			// Clears the specialists' order before recruit() runs - see DESIGN.md §3.
			if (step.action == "join_squad")
			{
				OrdersReceiver* orders1 = specialist1 ? specialist1->getOrdersReciever() : nullptr;
				if (orders1)
					orders1->clearOrders();
				if (specialist2 && specialist2 != specialist1)
				{
					OrdersReceiver* orders2 = specialist2->getOrdersReciever();
					if (orders2)
						orders2->clearOrders();
				}
			}

			auto actionIt = g_dialogueActions.find(step.action);
			if (actionIt != g_dialogueActions.end())
				actionIt->second(patient);
			else
				ErrorLog("SkeletonRebirth: dialogue step action \"" + step.action + "\" has no registered handler");

			// join_squad always defers the rest of the sequence, unconditionally - see DESIGN.md §4.
			if (step.action == "join_squad")
			{
				PendingDialogueSequence pending;
				pending.steps = steps;
				pending.nextIndex = i + 1;
				pending.initiator = initiator;
				pending.specialist1 = specialist1;
				pending.specialist2 = specialist2;
				pending.fireAtTick = GetTickCount64() + 1000;
				pending.showNamePrompt = true;
				g_pendingDialogueSequences[patient] = pending;
				return;
			}

			continue;
		}

		if (step.type == "open_menu")
		{
			// Terminal - see DESIGN.md §4.
			showDialogueBox(step.menu, patient, initiator);
			return;
		}

		ErrorLog("SkeletonRebirth: unknown dialogue step type \"" + step.type + "\" for " + patient->getName());
	}
}

// Name prompt shown after fast-mode recruit - see DESIGN.md §4, "Fast-mode recruit and the name prompt".
struct PendingNamePrompt
{
	Character* patient;
	Character* initiator;
	Character* specialist1;
	Character* specialist2;
	std::vector<DialogueBoxStepDef> steps;
	size_t nextIndex;
	MyGUI::Window* window;
	MyGUI::EditBox* nameBox;
};
static std::map<MyGUI::Widget*, PendingNamePrompt> g_pendingNamePrompts;

static void OnNamePromptConfirmed(MyGUI::Widget* sender)
{
	auto it = g_pendingNamePrompts.find(sender);
	if (it == g_pendingNamePrompts.end())
		return;

	PendingNamePrompt pending = it->second;
	g_pendingNamePrompts.erase(it);

	std::string newName = pending.nameBox ? pending.nameBox->getOnlyText() : "";
	if (!newName.empty())
		pending.patient->setName(newName);

	MyGUI::Gui::getInstance().destroyWidget(pending.window);

	dispatchDialogueSteps(pending.patient, pending.initiator, pending.specialist1, pending.specialist2, pending.steps, pending.nextIndex);
}

static void showNamePrompt(Character* patient, Character* initiator, Character* specialist1, Character* specialist2, const std::vector<DialogueBoxStepDef>& steps, size_t nextIndex)
{
	MyGUI::Window* window = gui ? gui->createPanel("SkeletonRebirthNamePrompt", 0.38f, 0.41f, 0.18f, 0.15f, "Info", "Kenshi_WindowC") : nullptr;
	if (!window)
	{
		ErrorLog("SkeletonRebirth: could not create the name prompt window - skipping rename for " + patient->getName());
		dispatchDialogueSteps(patient, initiator, specialist1, specialist2, steps, nextIndex);
		return;
	}
	window->setCaption("Name this character");
	window->setMovable(false);

	MyGUI::EditBox* nameBox = gui->createEditBox(window, 0.10f, 0.125f, 0.75f, 0.15f, "SkeletonRebirthNameBox", false);
	if (nameBox)
		nameBox->setCaption(patient->getName());

	MyGUI::Button* confirmButton = gui->createButton(window, 0.45f, 0.3f, 0.4f, 0.18f, "SkeletonRebirthNameConfirm", "Confirm", "Kenshi_Button2");
	if (!confirmButton)
	{
		ErrorLog("SkeletonRebirth: could not create the name prompt's Confirm button - skipping rename for " + patient->getName());
		MyGUI::Gui::getInstance().destroyWidget(window);
		dispatchDialogueSteps(patient, initiator, specialist1, specialist2, steps, nextIndex);
		return;
	}

	PendingNamePrompt pending;
	pending.patient = patient;
	pending.initiator = initiator;
	pending.specialist1 = specialist1;
	pending.specialist2 = specialist2;
	pending.steps = steps;
	pending.nextIndex = nextIndex;
	pending.window = window;
	pending.nameBox = nameBox;
	g_pendingNamePrompts[confirmButton] = pending;

	confirmButton->eventMouseButtonClick += MyGUI::newDelegate(OnNamePromptConfirmed);

	verboseLog("SkeletonRebirth: name prompt shown for " + patient->getName());
}

// IDelegate1<int> callback for MessageBoxManager::createMessageBox() - see DESIGN.md §4.
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

	// Re-resolved at click time rather than reusing the message's specialists - see DESIGN.md §3.
	Character* specialist1 = btn.requiresSkill.empty() ? nullptr
		: findSquadMemberWithSkill(initiator, btn.requiresSkill, btn.hasMinSkill, btn.minSkill, btn.hasMaxSkill, btn.maxSkill);
	Character* specialist2 = btn.requiresSkill2.empty() ? nullptr
		: findSquadMemberWithSkill(initiator, btn.requiresSkill2, btn.hasMinSkill2, btn.minSkill2, btn.hasMaxSkill2, btn.maxSkill2);

	dispatchDialogueSteps(patient, initiator, specialist1, specialist2, btn.steps, 0);
}

// `initiator` is who "requiresItems"/"requiresSkill"/"take_item" are checked against - null is fine
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

	// Capped at 3 - see DESIGN.md §4.
	std::vector<DialogueBoxButtonDef> eligibleButtons;
	for (size_t i = 0; i < def.buttons.size() && eligibleButtons.size() < 3; ++i)
	{
		const DialogueBoxButtonDef& btn = def.buttons[i];
		bool eligible = isDialogueButtonEligible(btn, patient, initiator);
		{
			Faction* patientFaction = patient->getFaction();
			std::ostringstream ss;
			ss << "SkeletonRebirth: dialogue button \"" << btn.caption << "\" eligible=" << eligible
			   << " excludePlayerFaction=" << btn.excludePlayerFaction
			   << " requiresPlayerFaction=" << btn.requiresPlayerFaction
			   << " patientFactionPtr=" << (void*)patientFaction
			   << " patientFactionIsPlayer=" << (patientFaction ? patientFaction->isThePlayer() : false)
			   << " requiresDeactivated=" << btn.requiresDeactivated
			   << " excludeDeactivated=" << btn.excludeDeactivated
			   << " patientDeactivated=" << g_deactivated.count(patient);

			if (!btn.requiresRace.empty() || !btn.excludeRace.empty())
			{
				RaceData* patientRace = patient->getRace();
				ss << " requiresRace=\"" << btn.requiresRace << "\" excludeRace=\"" << btn.excludeRace << "\""
				   << " patientRaceNull=" << (patientRace == nullptr)
				   << " patientRaceDataNull=" << (patientRace && !patientRace->data)
				   << " patientRaceName=\"" << (patientRace && patientRace->data ? patientRace->data->name : "") << "\""
				   << " patientRaceStringID=\"" << (patientRace && patientRace->data ? patientRace->data->stringID : "") << "\"";
			}

			if (!btn.requiresSkill.empty())
			{
				ss << " requiresSkill=\"" << btn.requiresSkill << "\""
				   << " minSkill=" << (btn.hasMinSkill ? btn.minSkill : -1.0f)
				   << " squadSatisfies=" << squadHasSkill(initiator, btn.requiresSkill, btn.hasMinSkill, btn.minSkill, btn.hasMaxSkill, btn.maxSkill);
			}

			if (!btn.requiresSkill2.empty())
			{
				ss << " requiresSkill2=\"" << btn.requiresSkill2 << "\""
				   << " minSkill2=" << (btn.hasMinSkill2 ? btn.minSkill2 : -1.0f)
				   << " squadSatisfies2=" << squadHasSkill(initiator, btn.requiresSkill2, btn.hasMinSkill2, btn.minSkill2, btn.hasMaxSkill2, btn.maxSkill2);
			}

			for (size_t reqIdx = 0; reqIdx < btn.requiresItems.size(); ++reqIdx)
			{
				const DialogueBoxItemRequirementDef& req = btn.requiresItems[reqIdx];
				Inventory* inv = initiator ? initiator->getInventory() : nullptr;
				GameData* itemData = inv ? getGameDataGuarded(req.item, ITEM) : nullptr;
				ss << " requiresItems[" << reqIdx << "]=\"" << req.item << "\" count=" << req.count
				   << " initiatorInvNull=" << (inv == nullptr)
				   << " itemDataResolved=" << (itemData != nullptr)
				   << " itemDataName=\"" << (itemData ? itemData->name : "") << "\""
				   << " hasItem=" << (inv && itemData ? inv->hasItem(itemData, req.count) : false);
			}

			verboseLog(ss.str());
		}
		if (eligible)
			eligibleButtons.push_back(btn);
	}
	if (eligibleButtons.empty())
	{
		ErrorLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" has no eligible buttons for this initiator - not shown");
		return;
	}

	// Resolved via isDialogueButtonForPatient(), not eligibleButtons - see DESIGN.md §4.
	std::string effectiveItem = def.item;
	int effectiveItemCount = 1;
	bool foundItem = false;
	std::string skillCharacterName;
	bool foundSkill = false;
	std::string skillCharacterName2;
	bool foundSkill2 = false;
	for (size_t i = 0; i < def.buttons.size() && (!foundItem || !foundSkill || !foundSkill2); ++i)
	{
		const DialogueBoxButtonDef& btn = def.buttons[i];
		if (!isDialogueButtonForPatient(btn, patient))
			continue;

		if (!foundItem && !btn.requiresItems.empty())
		{
			effectiveItem = btn.requiresItems[0].item;
			effectiveItemCount = btn.requiresItems[0].count;
			foundItem = true;
		}

		if (!foundSkill && !btn.requiresSkill.empty())
		{
			foundSkill = true;
			Character* c = findSquadMemberWithSkill(initiator, btn.requiresSkill, btn.hasMinSkill, btn.minSkill, btn.hasMaxSkill, btn.maxSkill);
			if (c)
				skillCharacterName = c->getName();
		}

		if (!foundSkill2 && !btn.requiresSkill2.empty())
		{
			foundSkill2 = true;
			Character* c = findSquadMemberWithSkill(initiator, btn.requiresSkill2, btn.hasMinSkill2, btn.minSkill2, btn.hasMaxSkill2, btn.maxSkill2);
			if (c)
				skillCharacterName2 = c->getName();
		}
	}

	Inventory* initiatorInv = initiator ? initiator->getInventory() : nullptr;
	GameData* effectiveItemData = effectiveItem.empty() ? nullptr : getGameDataGuarded(effectiveItem, ITEM);
	bool hasItem = initiatorInv && effectiveItemData && initiatorInv->hasItem(effectiveItemData, effectiveItemCount);

	std::string message = resolvePlaceholders(def.message, patient, effectiveItem, hasItem, skillCharacterName, skillCharacterName2, "dialogue box \"" + dialogueId + "\"");

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

// "Fast" mode, not "with edit" - see DESIGN.md §4, "Fast-mode recruit and the name prompt".
static void DialogueAction_JoinSquad(Character* patient)
{
	if (ou && ou->player)
	{
		verboseLog("SkeletonRebirth: DIAG before recruit(fast) -> " + describe(patient));
		ou->player->recruit(patient, false);
		verboseLog("SkeletonRebirth: DIAG after recruit(fast) -> " + describe(patient));
	}
}

// recruit()'s "fast" mode - see DESIGN.md §4.
static void DialogueAction_JoinSquadFast(Character* patient)
{
	if (ou && ou->player)
		ou->player->recruit(patient, false);
}

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

// Must run before the other loaders below - see DESIGN.md.
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

// Shared by a button's "requiresItems" and a "take_item" step's "items" - both are arrays of either
// plain item-id strings (shorthand for count 1) or { "item", "count" } objects.
static void parseItemRequirements(const rapidjson::Value& arr, const std::string& dialogueBoxName, const std::string& buttonCaption, const char* fieldName, std::vector<DialogueBoxItemRequirementDef>& out)
{
	for (auto rit = arr.Begin(); rit != arr.End(); ++rit)
	{
		DialogueBoxItemRequirementDef req;
		if (rit->IsString())
			req.item = rit->GetString();
		else if (rit->IsObject())
		{
			if (rit->HasMember("item") && (*rit)["item"].IsString())
				req.item = (*rit)["item"].GetString();
			if (rit->HasMember("count") && (*rit)["count"].IsNumber())
				req.count = (*rit)["count"].GetInt();
		}

		if (!req.item.empty())
			out.push_back(req);
		else
			ErrorLog("SkeletonRebirth: dialogue box \"" + dialogueBoxName + "\" button \"" + buttonCaption + "\" has a \"" + fieldName + "\" entry with no \"item\" - skipped");
	}
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
				if (bit->HasMember("requiresItems") && (*bit)["requiresItems"].IsArray())
					parseItemRequirements((*bit)["requiresItems"], it->name.GetString(), btn.caption, "requiresItems", btn.requiresItems);
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
				if (bit->HasMember("requiresSkill2") && (*bit)["requiresSkill2"].IsString())
				{
					btn.requiresSkill2 = (*bit)["requiresSkill2"].GetString();
					if (!g_skillFields.count(toLowerCopy(btn.requiresSkill2)))
						ErrorLog("SkeletonRebirth: dialogue box \"" + std::string(it->name.GetString()) + "\" button \"" + btn.caption + "\" has unrecognized requiresSkill2 \"" + btn.requiresSkill2 + "\" - this button will show unconditionally (skill part of the check is skipped)");
				}
				if (bit->HasMember("minSkill2") && (*bit)["minSkill2"].IsNumber())
				{
					btn.hasMinSkill2 = true;
					btn.minSkill2 = (*bit)["minSkill2"].GetFloat();
				}
				if (bit->HasMember("maxSkill2") && (*bit)["maxSkill2"].IsNumber())
				{
					btn.hasMaxSkill2 = true;
					btn.maxSkill2 = (*bit)["maxSkill2"].GetFloat();
				}
				if (bit->HasMember("excludePlayerFaction") && (*bit)["excludePlayerFaction"].IsBool())
					btn.excludePlayerFaction = (*bit)["excludePlayerFaction"].GetBool();
				if (bit->HasMember("requiresPlayerFaction") && (*bit)["requiresPlayerFaction"].IsBool())
					btn.requiresPlayerFaction = (*bit)["requiresPlayerFaction"].GetBool();
				if (bit->HasMember("requiresDeactivated") && (*bit)["requiresDeactivated"].IsBool())
					btn.requiresDeactivated = (*bit)["requiresDeactivated"].GetBool();
				if (bit->HasMember("excludeDeactivated") && (*bit)["excludeDeactivated"].IsBool())
					btn.excludeDeactivated = (*bit)["excludeDeactivated"].GetBool();
				if (bit->HasMember("requiresAnimal") && (*bit)["requiresAnimal"].IsBool())
					btn.requiresAnimal = (*bit)["requiresAnimal"].GetBool();
				if (bit->HasMember("excludeAnimal") && (*bit)["excludeAnimal"].IsBool())
					btn.excludeAnimal = (*bit)["excludeAnimal"].GetBool();
				if (bit->HasMember("requiresRace") && (*bit)["requiresRace"].IsString())
					btn.requiresRace = (*bit)["requiresRace"].GetString();
				if (bit->HasMember("excludeRace") && (*bit)["excludeRace"].IsString())
					btn.excludeRace = (*bit)["excludeRace"].GetString();
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
						if (sit->HasMember("items") && (*sit)["items"].IsArray())
							parseItemRequirements((*sit)["items"], it->name.GetString(), btn.caption, "items", step.items);
						if (sit->HasMember("text") && (*sit)["text"].IsString())
							step.text = (*sit)["text"].GetString();
						if (sit->HasMember("color") && (*sit)["color"].IsString())
							step.color = (*sit)["color"].GetString();
						if (sit->HasMember("seconds") && (*sit)["seconds"].IsNumber())
							step.seconds = (*sit)["seconds"].GetFloat();
						if (sit->HasMember("menu") && (*sit)["menu"].IsString())
							step.menu = (*sit)["menu"].GetString();
						if (step.type == "take_item" && step.items.empty())
							ErrorLog("SkeletonRebirth: dialogue box \"" + std::string(it->name.GetString()) + "\" button \"" + btn.caption + "\" has a \"take_item\" step with no \"items\" - it will do nothing");
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
		trigger.requiresQualifiedFor = (t.HasMember("requiresQualifiedFor") && t["requiresQualifiedFor"].IsString()) ? t["requiresQualifiedFor"].GetString() : "";
		trigger.excludeQualifiedFor = (t.HasMember("excludeQualifiedFor") && t["excludeQualifiedFor"].IsString()) ? t["excludeQualifiedFor"].GetString() : "";

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
		// Empty "buildings" is intentional - see DESIGN.md §5.

		g_dialogueTriggers.push_back(trigger);
	}

	std::ostringstream summary;
	summary << "SkeletonRebirth: loaded " << g_dialogueTriggers.size() << " dialogue trigger(s) from \"" << jsonPath << "\"";
	verboseLog(summary.str());
}

// Character::update() is deliberately NOT hooked - see DESIGN.md §1.

// --- Hook 2: MedicalSystem::medicalUpdate() ---------------------------------------------------
// Freezes a Deactivated character's medical simulation - see DESIGN.md §1. No owning-Character* field
// on MedicalSystem, so reach it via getPart(...)->me.
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

// --- Hook 3: MedicalSystem::applyFirstAid() -----------------------------------------------------
// Feeds g_activeFirstAid for the item-based reactivation trigger - see DESIGN.md §3/§5.
bool (*MedicalSystem_applyFirstAid_orig)(MedicalSystem*, float, Item*, float, Character*);
bool MedicalSystem_applyFirstAid_hook(MedicalSystem* self, float skill, Item* equipment, float frameTIME, Character* who)
{
	MedicalSystem::HealthPartStatus* head = self->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER);
	Character* patient = head ? head->me : nullptr;

	if (patient)
		g_activeFirstAid[patient] = who;

	return MedicalSystem_applyFirstAid_orig(self, skill, equipment, frameTIME, who);
}

// Level-triggered, per DESIGN.md §5.
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

	// Empty buildings = no requirement - see DESIGN.md §5.
	bool buildingMatches = trigger.buildings.empty();
	if (!buildingMatches)
	{
		RootObject* obj = self->inWhat.getRootObject();
		Building* building = obj ? static_cast<Building*>(obj) : nullptr;
		GameData* buildingData = building ? building->getGameData() : nullptr;

		for (size_t i = 0; !buildingMatches && buildingData && i < trigger.buildings.size(); ++i)
			buildingMatches = (trigger.buildings[i] == buildingData->stringID);
	}

	if (!buildingMatches)
	{
		g_triggerShown.erase(shownKey);
		return;
	}

	if (g_triggerShown.count(shownKey))
		return; // already shown, waiting on the player to act or leave

	// Prefers g_activeFirstAid's caregiver, else player's current selection - see DESIGN.md §4/§5.
	auto repairerIt = g_activeFirstAid.find(self);
	Character* initiator = (repairerIt != g_activeFirstAid.end() && repairerIt->second)
		? repairerIt->second
		: ((ou && ou->player) ? ou->player->selectedCharacter.getCharacter() : nullptr);

	// Not marked shown on failure, so this re-checks next tick - see DESIGN.md §5.
	if (!trigger.requiresQualifiedFor.empty() && !initiatorQualifiesForMenu(trigger.requiresQualifiedFor, self, initiator))
		return;

	if (!trigger.excludeQualifiedFor.empty() && initiatorQualifiesForMenu(trigger.excludeQualifiedFor, self, initiator))
		return;

	showDialogueBox(trigger.menu, self, initiator);
	g_triggerShown[shownKey] = true;
}

// --- Persistent HUD health-status text - see DESIGN.md §2's "Second override" ---------------------
static const char* DEACTIVATED_COLOR = "#59231a"; // vanilla's own dark red, same as its "Dead" text
static MyGUI::TextBox* g_healthTextWidget = nullptr;
static bool g_healthTextWidgetSearched = false;
static MyGUI::TextBox* findHealthTextWidget(MyGUI::Widget* widget)
{
	if (!widget)
		return nullptr;

	static const std::string suffix = "_HealthText";
	const std::string& name = widget->getName();
	if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
	{
		MyGUI::TextBox* textBox = widget->castType<MyGUI::TextBox>(false);
		if (textBox)
			return textBox;
	}

	size_t count = widget->getChildCount();
	for (size_t i = 0; i < count; ++i)
	{
		MyGUI::TextBox* found = findHealthTextWidget(widget->getChildAt(i));
		if (found)
			return found;
	}

	return nullptr;
}

// Tracks "which character's panel is currently shown", set by DatapanelGUI_setLine_KeyLastVisible_hook -
// deliberately not PlayerInterface::selectedCharacter, see DESIGN.md §2 and "A second pitfall".
static Character* g_lastInspectedCharacter = nullptr;

static void updateHealthTextOverride(Character* self)
{
	if (g_lastInspectedCharacter != self)
		return;

	if (!g_healthTextWidgetSearched && gui && gui->mainbar)
	{
		g_healthTextWidgetSearched = true;
		g_healthTextWidget = findHealthTextWidget(gui->mainbar->getWidget());
		if (!g_healthTextWidget)
			ErrorLog("SkeletonRebirthDiagnostics: Could not find MainBarGUI's *_HealthText widget - persistent HUD health status will not be overridden for Deactivated robots.");
	}

	if (g_healthTextWidget)
		g_healthTextWidget->setCaption(std::string(DEACTIVATED_COLOR) + "Deactivated");
}

// --- Hook: Character::updateOnScreenCheck() - dialogue trigger polling point -------------------
// Fires every frame per character - also doubles as the tick source for resuming paused dialogue
// step sequences below, regardless of self's own trigger status.
bool (*Character_updateOnScreenCheck_orig)(Character*);

bool Character_updateOnScreenCheck_hook(Character* self)
{
	bool result = Character_updateOnScreenCheck_orig(self);

	// g_deactivated.count() checked first (cheap) so hasDeactivatedSignature's anatomy scan only runs
	// for not-yet-tracked characters - see DESIGN.md §6.
	if (!g_deactivated.count(self) && isRobotRace(self) && hasDeactivatedSignature(self))
	{
		g_deactivated[self] = true;
		verboseLog("SkeletonRebirth: re-derived Deactivated state from native signature -> " + describe(self));
	}

	if (g_deactivated.count(self))
		updateHealthTextOverride(self);

	auto delayIt = g_pendingDialogueSequences.find(self);
	if (delayIt != g_pendingDialogueSequences.end())
	{
		if (delayIt->second.awaitingRepair)
		{
			bool specialistsReady = hasArrivedForRepair(self, delayIt->second.specialist1)
				&& hasArrivedForRepair(self, delayIt->second.specialist2);

			if (specialistsReady)
			{
				PendingDialogueSequence pending = delayIt->second;
				g_pendingDialogueSequences.erase(delayIt);
				dispatchDialogueSteps(self, pending.initiator, pending.specialist1, pending.specialist2, pending.steps, pending.nextIndex);
			}
			else if (GetTickCount64() >= delayIt->second.repairTimeoutAtTick)
			{
				// Aborts the sequence rather than proceeding without both specialists - see DESIGN.md §3.
				verboseLog("SkeletonRebirth: await_repair timed out for " + self->getName());
				showGameNotification(self, "{name}'s specialists couldn't get into position in time - service mode aborted.", "");
				g_pendingDialogueSequences.erase(delayIt);
			}
		}
		else
		{
			if (GetTickCount64() >= delayIt->second.fireAtTick)
			{
				PendingDialogueSequence pending = delayIt->second;
				g_pendingDialogueSequences.erase(delayIt);
				if (pending.showNamePrompt)
					showNamePrompt(self, pending.initiator, pending.specialist1, pending.specialist2, pending.steps, pending.nextIndex);
				else
					dispatchDialogueSteps(self, pending.initiator, pending.specialist1, pending.specialist2, pending.steps, pending.nextIndex);
			}
		}
	}

	for (size_t i = 0; i < g_dialogueTriggers.size(); ++i)
		evaluateDialogueTrigger(self, i, g_dialogueTriggers[i]);

	// Cleared after evaluating so this frame's checks still see it - see DESIGN.md §5.
	g_activeFirstAid.erase(self);

	return result;
}

// --- Hook: DatapanelGUI::setLine(key,s1,s2,category,last,keyVisible) - status tag override -------
// The "State:" row is overridden unconditionally for any tracked Deactivated character - see DESIGN.md §2.
DataPanelLine* (*DatapanelGUI_setLine_KeyLastVisible_orig)(DatapanelGUI*, const std::string&, const std::string&, const std::string&, int, bool, bool);
DataPanelLine* DatapanelGUI_setLine_KeyLastVisible_hook(DatapanelGUI* self, const std::string& keyValue, const std::string& s1, const std::string& s2, int category, bool last, bool keyVisible)
{
	Character* target = self->getObject().getCharacter();

	// Feeds g_lastInspectedCharacter (DESIGN.md §2) - kept fresh for any open panel, not just Deactivated.
	g_lastInspectedCharacter = target;

	if (!target || !g_deactivated.count(target))
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);

	if (keyValue == "State:")
	{
		bool needsAiCore = isHumanoidState(target) && hasHeadPart(target);
		std::string overriddenS2 = std::string(DEACTIVATED_COLOR) + (needsAiCore ? "AI Failure" : "Power Failure");
		return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, overriddenS2, category, last, keyVisible);
	}

	return DatapanelGUI_setLine_KeyLastVisible_orig(self, keyValue, s1, s2, category, last, keyVisible);
}

// --- Hook: DatapanelGUI::setLineStatInfo(s1,s2,category) - robot age-tier relabeling, see DESIGN.md.
DataPanelLine* (*DatapanelGUI_setLineStatInfo_orig)(DatapanelGUI*, const std::string&, const std::string&, int);
DataPanelLine* DatapanelGUI_setLineStatInfo_hook(DatapanelGUI* self, const std::string& s1, const std::string& s2, int category)
{
	Character* target = self->getObject().getCharacter();
	if (target && s1 == "Age:" && isRobotRace(target))
	{
		float age0to1 = target->getAge0to1();
		std::string tier = age0to1 <= 0.39f ? "Learning"
			: age0to1 <= 0.59f ? "Operational"
			: age0to1 <= 1.10f ? "Adaptive"
			: "Elite";
		return DatapanelGUI_setLineStatInfo_orig(self, "Calibration:", tier, category);
	}

	return DatapanelGUI_setLineStatInfo_orig(self, s1, s2, category);
}

// --- Robot limb race-lock (merged in from the former "The Limbless (Type 2)" mod) --------------
// Unrelated to the Deactivated/reactivation system above - see DESIGN.md's "Robot limb race-lock".
static std::map<RootObject*, Character*> g_limbInterfaceOwners;

RootObject* (*RobotLimbs_getInventoryInterface_orig)(RobotLimbs*, bool);
RootObject* RobotLimbs_getInventoryInterface_hook(RobotLimbs* self, bool create)
{
	RootObject* result = RobotLimbs_getInventoryInterface_orig(self, create);
	if (result && self->character)
		g_limbInterfaceOwners[result] = self->character;
	return result;
}

// Primes the RaceLimiter cache, then substitutes "who" - see DESIGN.md's "Robot limb race-lock".
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
	g_triggerStateChecks["inBed"] = &isInBedState;
	g_triggerStateChecks["beingRepaired"] = &isBeingRepairedState;
	g_triggerStateChecks["repairerScience1"] = &hasRepairerScience1;
	loadDebugSettingFromJson();
	loadDialogueBoxesFromJson();
	loadDialogueTriggersFromJson();

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::declareDead), Character_declareDead_hook, &Character_declareDead_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add declareDead hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::medicalUpdate), MedicalSystem_medicalUpdate_hook, &MedicalSystem_medicalUpdate_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::medicalUpdate hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&MedicalSystem::applyFirstAid), MedicalSystem_applyFirstAid_hook, &MedicalSystem_applyFirstAid_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add MedicalSystem::applyFirstAid hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Character::updateOnScreenCheck), Character_updateOnScreenCheck_hook, &Character_updateOnScreenCheck_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Character::updateOnScreenCheck hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Inventory::deathCheck), Inventory_deathCheck_hook, &Inventory_deathCheck_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add Inventory::deathCheck hook!");

	typedef DataPanelLine* (DatapanelGUI::*SetLineKeyLastVisibleFn)(const std::string&, const std::string&, const std::string&, int, bool, bool);
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((SetLineKeyLastVisibleFn)&DatapanelGUI::setLine), DatapanelGUI_setLine_KeyLastVisible_hook, &DatapanelGUI_setLine_KeyLastVisible_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add DatapanelGUI::setLine(key,last,visible) hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DatapanelGUI::setLineStatInfo), DatapanelGUI_setLineStatInfo_hook, &DatapanelGUI_setLineStatInfo_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add DatapanelGUI::setLineStatInfo hook!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RobotLimbs::getInventoryInterface), RobotLimbs_getInventoryInterface_hook, &RobotLimbs_getInventoryInterface_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add RobotLimbs::getInventoryInterface hook!");

	// canEquip is virtual, so this goes through the exported _NV_ (non-virtual) stub - GetRealAddress
	// doesn't work on &Class::VirtualMethod directly (see DESIGN.md's hooking-virtual-functions
	// pitfall). It's overloaded, hence the cast.
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((bool(RaceLimiter::*)(GameData*, RootObject*))&RaceLimiter::_NV_canEquip), RaceLimiter_canEquip2_hook, &RaceLimiter_canEquip2_orig))
		ErrorLog("SkeletonRebirthDiagnostics: Could not add RaceLimiter::canEquip(item,who) hook!");
}
