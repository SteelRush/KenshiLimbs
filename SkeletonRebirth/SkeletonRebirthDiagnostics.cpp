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
#include <kenshi/Faction.h>
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

// AI/AI.h forward-declares CharacterMessage as a class while Character.h defines it as an enum,
// so both can't be included together - declare the one method we need locally instead.
class AITaskSytem;
class AI
{
public:
	AITaskSytem* getTaskSystem() const;
};

// SkeletonRebirth plugin - see DESIGN.md for full history and rejected approaches. Robots never
// actually die (declareDead blocked, MedicalSystem::dead kept false); a Deactivated robot sits inert
// via vanilla's own knockout state, with health frozen (medicalUpdate skipped). Placing one in a
// Skeleton Repair Bed (polled via Character::updateOnScreenCheck) opens a confirmation dialogue box
// built from Kenshi's own Kenshi_MessageBox.layout, data-driven from RE_Kenshi.json's "DialogueBoxes"
// object (see showDialogueBox()/dispatchDialogueSteps()). Deactivated state survives save/reload via a
// JSON side-file keyed by handle string.
//
// DebugLog() output (see Debug.h) is prefixed "SkeletonRebirth:".

static bool isRobotRace(Character* c)
{
	RaceData* race = c->_NV_getRace();
	return race != nullptr && race->robot;
}

// Character*-keyed and session-only (doesn't survive reload - see saveDeactivatedState/
// loadDeactivatedState below for the handle-string-keyed side-file that does).
static std::map<Character*, bool> g_deactivated;

// Gates Character_updateOnScreenCheck_hook's trigger so it doesn't reopen the dialogue box every tick
// while still in the bed. Dismissing the box (e.g. "No") deliberately does NOT clear this - the trigger
// is level-triggered, so clearing on dismiss would reopen the box the very next tick, making "No"
// appear to do nothing. Only a real reactivation or leaving the bed clears it.
static std::map<Character*, bool> g_reactivateDialogueShown;

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
	DebugLog(ss.str());
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

// BF_SKELETON_BED == 25, confirmed via live testing (not the FCS-adjacent guess of 150).
static bool isInSkeletonBed(Character* c)
{
	if (c->inSomething != IN_BED)
		return false;

	RootObject* obj = c->inWhat.getRootObject();
	Building* bed = obj ? static_cast<Building*>(obj) : nullptr;
	return bed && bed->_NV_getSpecialFunction() == BF_SKELETON_BED;
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

		// No explicit AI pause needed - vanilla's own knockout state has no recovery timer at the
		// catastrophic damage level that triggers this hook, so the character is already inert.
		g_deactivated[self] = true;
		saveDeactivatedState();

		DebugLog("SkeletonRebirth: declareDead() BLOCKED for robot -> " + describe(self));
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
	DebugLog("SkeletonRebirth: TryReactivate() SUCCEEDED -> " + describe(self));

	return true;
}

// Custom MyGUI box rather than Dialogue::startPlayerConversation() (dialogue was suppressed while
// this mod was still using MedicalSystem::dead=true - see DESIGN.md) or hand-built widgets
// (ForgottenGUI::createPanel/createButton render nothing usable without a ResourceLayout template).
// Kenshi_MessageBox.layout is loaded via MyGUI::LayoutManager and its widgets repurposed by name -
// see DESIGN.md for the full rejected-approaches history.
//
// --- JSON-driven dialogue boxes -----------------------------------------------------------------
// Box text and button behavior are data-driven from RE_Kenshi.json's "DialogueBoxes" object rather
// than hardcoded per-dialogue C++. A button's behavior is an ordered list of "steps" - see
// DialogueBoxStepDef for the step types. A new dialogue box needs only a JSON entry; a new "action"
// step type needs registering in g_dialogueActions.
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
	bool excludePlayerFaction; // hidden if the patient belongs to the player's faction

	DialogueBoxButtonDef() : hasMinSkill(false), minSkill(0.0f), hasMaxSkill(false), maxSkill(0.0f), excludePlayerFaction(false) {}
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
		Faction* faction = patient->_NV_getFaction();
		if (faction && faction->isThePlayer())
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

// Resumed from Character_updateOnScreenCheck_hook, which already polls every frame per character.
// GetTickCount64() wall-clock rather than a per-frame delta since that hook receives no frameTime.
struct PendingDialogueSequence
{
	std::vector<DialogueBoxStepDef> steps; // copied, not referenced - see dispatchDialogueSteps()
	size_t nextIndex;
	Character* initiator;
	ULONGLONG fireAtTick;
};
static std::map<Character*, PendingDialogueSequence> g_pendingDialogueSequences;

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

	// Looked up by widget identity before closeDialogueBox() unloads the layout below.
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

	// Resolved before loading the layout - every button gated out would otherwise show an unclosable box.
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

	g_pendingDialoguePatient = patient;
	g_pendingDialogueInitiator = initiator;

	// Prefix avoids MyGUI widget-name collisions with any other load of the same layout.
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

	// Recenters Kenshi_MessageBox.layout's fixed width/height (0.182292 x 0.148148, top-left by default) -
	// no real-size getter exists in this SDK to compute this dynamically.
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
			button->setVisible(false); // fewer than 3 buttons eligible
		}
	}

	DebugLog("SkeletonRebirth: dialogue box \"" + dialogueId + "\" shown for " + patient->_NV_getName());
}

static void DialogueAction_Reactivate(Character* patient)
{
	if (!TryReactivate(patient))
		ErrorLog("SkeletonRebirth: TryReactivate failed for " + patient->_NV_getName() + " after confirm");

	g_reactivateDialogueShown.erase(patient);
}

// recruit() runs BEFORE the stat reset now, not after - confirmed live that recruit(editor=true)
// reinitializes the character's CharStats from its original template, which was silently clobbering
// this function's own reset back to the character's pre-reset (modified) values whenever recruit() ran
// second. CharStats* is re-fetched after recruit() rather than reusing a pointer captured beforehand,
// in case recruit() replaces the object rather than mutating it in place.
static void DialogueAction_SystemReset(Character* patient)
{
	if (ou && ou->player)
		ou->player->recruit(patient, true);

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

// --- Hook: Character::updateOnScreenCheck() - reactivation trigger polling point -------------------
// Fires every frame per character - also doubles as the tick source for resuming paused dialogue
// step sequences below, regardless of self's own g_deactivated status.
bool (*Character_updateOnScreenCheck_orig)(Character*);
bool Character_updateOnScreenCheck_hook(Character* self)
{
	bool result = Character_updateOnScreenCheck_orig(self);

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
			showDialogueBox("system_menu", self, initiator);
			g_reactivateDialogueShown[self] = true;
		}
	}
	else
	{
		g_reactivateDialogueShown.erase(self); // left the bed - a future bed-placement should prompt again
	}

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

__declspec(dllexport) void startPlugin()
{
	g_dialogueActions["reactivate"] = &DialogueAction_Reactivate;
	g_dialogueActions["system_reset"] = &DialogueAction_SystemReset;
	loadDialogueBoxesFromJson();

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
