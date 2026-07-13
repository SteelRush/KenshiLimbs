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
// rejected approaches. Two hooks: robots never actually die (declareDead), and their medical
// simulation is completely frozen - health can't change in either direction, and declareDead()
// can't re-trigger - for as long as they're Deactivated (MedicalSystem::medicalUpdate skipped
// entirely). Healing only needs to happen after TryReactivate() releases the lock, not before.
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

// Real reactivation trigger - not yet wired to any in-game action (item use, dialogue option,
// etc.). Unconditional release, not a readiness gate: health can't change at all while Deactivated
// (medicalUpdate is frozen below), so a "was it repaired?" check here would be a permanent
// deadlock. Healing happens normally after this releases the lock, not before.
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

	ScreenLabel* label = gui->createScreenLabel(self->_NV_getName() + " was revived!", MyGUI::Colour::Green, ScreenLabel::LS_MEDIUM, ScreenLabel::RS_SLOW);
	if (label)
		label->setTracking(self->getHandle(), Ogre::Vector3(0, 1, 0));

	return true;
}

// --- Hook: MedicalSystem::applyFirstAid() - real reactivation trigger ------------------------
// Confirmed via diagnostic logging: applyDoctoring() always co-fires with applyFirstAid() for the
// same treatment (identical timestamp, same item/state every time), so only one needs hooking.
// itemFunction == ITEM_ROBOTREPAIR is also the only value ever seen for robot patients - not a
// very selective filter on its own, but still the correct check to make.
bool (*MedicalSystem_applyFirstAid_orig)(MedicalSystem*, float, Item*, float, Character*);
bool MedicalSystem_applyFirstAid_hook(MedicalSystem* self, float skill, Item* equipment, float frameTime, Character* who)
{
	MedicalSystem::HealthPartStatus* head = self->getPart(MedicalSystem::HealthPartStatus::PART_HEAD, SIDE_NEITHER);
	Character* patient = head ? head->me : nullptr;

	if (patient && g_deactivated.count(patient) && equipment && equipment->itemFunction == ITEM_ROBOTREPAIR && isInSkeletonBed(patient))
	{
		if (TryReactivate(patient))
			DebugLog("SkeletonRebirth: repair kit used in Skeleton Bed -> " + describe(patient));
	}

	return MedicalSystem_applyFirstAid_orig(self, skill, equipment, frameTime, who);
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
}
