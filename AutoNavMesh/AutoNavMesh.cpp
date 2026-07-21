#include <Debug.h>

#include <kenshi/Building/Building.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/util/hand.h>

#include <core/Functions.h>

#include <rapidjson/istreamwrapper.h>
#include <rapidjson/document.h>

#include <fstream>
#include <set>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// kenshi/NavMesh.h fails to compile under the VS2010 toolchain (MessageQueue<T>::Node is a dependent
// name missing 'typename', unrelated to what we need from this class) - declare just the one method
// we call instead, the same local-declaration workaround SkeletonRebirth uses for its own
// doesn't-compile/link-cleanly headers (see its "AI" class and MessageBoxManager placeholder).
class hand;
class NavMesh
{
public:
	void generate(const hand& building);
};

// Gates DebugLog() (not ErrorLog()) behind RE_Kenshi.json's "Debug" setting, same as SkeletonRebirth.
static bool g_debugLoggingEnabled = false;
static void verboseLog(const std::string& message)
{
	if (g_debugLoggingEnabled)
		DebugLog(message);
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

// AutoNavMesh - regenerates the navmesh around a building the moment its construction completes,
// the same work Ctrl+Shift+F11 does manually. Without this, a player-built base's navmesh doesn't
// update until the player remembers to force a regen.
//
// notifyConstructionComplete() genuinely re-fires for the same building later on (confirmed live: same
// Building* logged again ~70s+ after its first firing), so this dedupes to regen each building once.
// Keyed by hand (the game's own object handle, index+serial) rather than the raw Building* - a
// destroyed building's memory can get reused for an unrelated new building, and keying on the pointer
// would wrongly skip that new building's regen since the address would already be in the set.
static std::set<hand> g_regeneratedBuildings;

void (*Building_notifyConstructionComplete_orig)(Building*);
void Building_notifyConstructionComplete_hook(Building* self)
{
	Building_notifyConstructionComplete_orig(self);

	hand buildingHandle = self->getHandle();
	if (!g_regeneratedBuildings.insert(buildingHandle).second)
	{
		verboseLog("AutoNavMesh: skipped repeat notifyConstructionComplete for \"" + self->getName() + "\" (" + buildingHandle.toString() + ")");
		return;
	}

	if (ou && ou->navmesh)
	{
		ou->navmesh->generate(buildingHandle);
		verboseLog("AutoNavMesh: regenerated navmesh for \"" + self->getName() + "\" (" + buildingHandle.toString() + ")");
	}
}

__declspec(dllexport) void startPlugin()
{
	loadDebugSettingFromJson();

	// notifyConstructionComplete is virtual but not overridden by any Building subclass (DoorStuff,
	// WallBuilding, GatewayBuilding, UseableStuff, etc. all inherit Building's own implementation), so
	// hooking Building's RVA - reached via the _NV_ address per the standard hooking-a-virtual-function
	// pattern, since &Class::VirtualMethod itself isn't a hookable address - catches every building type.
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Building::_NV_notifyConstructionComplete), Building_notifyConstructionComplete_hook, &Building_notifyConstructionComplete_orig))
		ErrorLog("AutoNavMesh: Could not add Building::notifyConstructionComplete hook!");
}
