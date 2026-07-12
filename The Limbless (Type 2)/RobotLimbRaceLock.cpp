#include <Debug.h>

#include <kenshi/Item.h>
#include <kenshi/Gear.h>
#include <kenshi/Character.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/RootObject.h>
#include <kenshi/MedicalSystem.h>

#include <core/Functions.h>

#include <map>

// RobotLimbs::inventory is a cached "interface" RootObject* used to represent
// a specific character's limb-slot section in the GUI. This is exactly the
// object vanilla passes as "who" during the drag/equip race-check below (its
// getName() is a generic "ROBOTICS" label, not tied to the character's own
// name) - RobotLimbs::character is the TRUE owner. Record interface-pointer ->
// owning-character every time this is (re)built, so we can look up the exact
// correct recipient from "who" later - correct in all cases, including
// multi-character trades, no guessing needed.
static std::map<RootObject*, Character*> g_limbInterfaceOwners;

RootObject* (*RobotLimbs_getInventoryInterface_orig)(RobotLimbs*, bool);
RootObject* RobotLimbs_getInventoryInterface_hook(RobotLimbs* self, bool create)
{
	RootObject* result = RobotLimbs_getInventoryInterface_orig(self, create);
	if (result && self->character)
		g_limbInterfaceOwners[result] = self->character;
	return result;
}

// Vanilla Kenshi stores each item's FCS "Race Limiter" (racesInclude/racesExclude)
// in the RaceLimiter singleton, populated lazily via addLimit() the first time
// something looks an item up - nothing on the robot-limb path ever calls that,
// so the cache stays empty and this check always sees "unrestricted" for limbs.
// Priming it here is enough; the "who" substitution above then lets vanilla's
// own equip validation enforce the restriction correctly on its own.
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
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&RobotLimbs::getInventoryInterface), RobotLimbs_getInventoryInterface_hook, &RobotLimbs_getInventoryInterface_orig))
		ErrorLog("RobotLimbRaceLock: Could not add getInventoryInterface hook!");

	// Note: canEquip is virtual, so we go through the exported _NV_ (non-virtual)
	// stub - GetRealAddress doesn't work on &Class::VirtualMethod directly (see
	// KenshiLib's own doc comment on GetRealAddress). It's overloaded, hence the cast.
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((bool(RaceLimiter::*)(GameData*, RootObject*))&RaceLimiter::_NV_canEquip), RaceLimiter_canEquip2_hook, &RaceLimiter_canEquip2_orig))
		ErrorLog("RobotLimbRaceLock: Could not add RaceLimiter::canEquip(item,who) hook!");
}
