## Robot Limb Race Lock plugin

Bundled with **The Limbless (Type 2)**. Enforces the race restriction ("Race Limiter" /
`racesInclude` / `racesExclude`) set on robotic limb items in FCS, which vanilla Kenshi silently
ignores for limbs (it works correctly for normal Gear/Armour).

### Root cause

Two separate vanilla bugs had to be fixed for this to work:

1. `RaceLimiter::limits` (the parsed FCS restriction data, keyed per item) is populated lazily via
   `RaceLimiter::addLimit()` the first time something looks an item up. Nothing on the robot-limb
   equip path ever calls this, so the cache stays empty and every limb looks unrestricted.
2. The drag/equip validation (`RaceLimiter::canEquip(item, who)`) is called with a `who` that
   doesn't resolve to the equipping character at all — it's a generic per-character "limb slot
   interface" proxy object (`RobotLimbs::inventory`, name always `"ROBOTICS"`), not the character.
   This made every race check evaluate against a null race, which either blocked everyone
   (`racesInclude` set) or blocked no one (`racesExclude` set), depending on the item.

### The fix

- Hooks `RobotLimbs::getInventoryInterface` to record, for every such proxy object it returns,
  which `Character` actually owns it (`RobotLimbs::character`).
- Hooks `RaceLimiter::canEquip(GameData*, RootObject*)` to prime the cache via `addLimit()`, then
  look up the real owning character for the given `who` (via the map above) before calling through
  to the original check. This is correct even in multi-character trade/medical screens, where
  `who`/the "currently selected character" is not necessarily the actual recipient.

No tooltip text is added — vanilla doesn't show race-restriction info on *any* item type's tooltip
(confirmed by testing normal Armour too), it just silently refuses the equip, and RobotLimbItem's
tooltip panel (`ToolTipInventory`) turned out to have a fixed-size layout that garbled any extra
line we tried to inject (Data1, Data2, and appending via the real `ToolTip::addLine` API all hit
the same problem — the box never grows to fit an extra line). If you want players to know an item
is race-restricted, put that in the item's FCS description field instead.

## Setup

Two ways to build:

- **Native Windows**: Visual Studio 2019+ with the VS2010 (`v100`) C++ toolset, and the
  [KenshiLib_Examples dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  (set `KENSHILIB_DIR` to point at that checkout, as described in the
  [KenshiLib README](https://github.com/KenshiReclaimer/KenshiLib/)). Open `RobotLimbRaceLock.vcxproj`,
  compile in Release|x64.
- **Linux via Wine**: confirmed working. Install the Windows SDK 7.1 (x64) MSIs into a dedicated
  64-bit Wine prefix via `wine msiexec /i ... /qn ADDLOCAL=ALL` (`WinSDK_amd64`, `WinSDKBuild_amd64`,
  `WinSDKWin32Tools_amd64`, `WinSDKTools_amd64`, `vc_stdx86`, `vc_stdamd64` - the GUI `SDKSetup.exe`
  bootstrapper isn't reliable under Wine, install the individual MSIs directly instead). Pull
  `KenshiLib_Examples_deps` (needs `git lfs pull` for the `.lib`/`.zip` files, and also
  `OgreMain_x64.lib` if you extend this with anything touching Ogre allocator templates). Then run
  `build_wine.bat` in this folder via `wine cmd /c build_wine.bat` with `WINEPREFIX` pointed at that
  prefix - it sets `INCLUDE`/`LIB`/`UNICODE` defines and calls `cl.exe`/`link.exe` directly (skips
  MSBuild/`.vcxproj` entirely, avoiding the need for the full VS IDE under Wine).

## Install steps

Copy this whole folder to `[Kenshi install dir]/mods/The Limbless (Type 2)/` (mod data + DLL +
`RE_Kenshi.json` all together), or just copy the built `RobotLimbRaceLock.dll` over the existing one
there after a rebuild.

Run RE_Kenshi and enable **The Limbless (Type 2)** via Kenshi's `Mods` tab - the plugin loads
automatically with it (see `RE_Kenshi.json`).

## Notes

- If nothing in FCS has a robot limb's race restriction actually set, this plugin has no visible
  effect - it only enforces restrictions that already exist in the item data.
- Confirmed working in a multi-character trade/medical scenario (one character initiating, a
  different character as the actual recipient) - the fix resolves the true recipient correctly in
  that case, not just simple single-character equipping.
