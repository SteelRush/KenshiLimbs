# KenshiLimbs

C++ plugins built on RE_Kenshi/KenshiLib that fix and extend Kenshi gameplay systems, mostly around
robotic characters and their limbs.

## Mods

- [`SkeletonRebirth`](SkeletonRebirth/README.md) - lets robot-race characters (Skeletons, Iron
  Spiders, Soldierbot Guards, Security Spiders, etc.) collapse into a reversible **Deactivated** state
  instead of dying outright, reactivated via a data-driven confirmation dialogue (repair items, skill
  checks, squad-wide gating, item requirements with counts) rather than hardcoded per-dialogue C++. Also
  includes the robotic limb race-lock fix formerly shipped as the standalone "The Limbless (Type 2)"
  mod (vanilla's FCS-configured robotic limb race restrictions are silently ignored by the base game
  engine; see [`DESIGN.md`](SkeletonRebirth/DESIGN.md) for the full architecture).
- [`AutoNavMesh`](AutoNavMesh) - regenerates the navmesh around a building the moment its construction
  completes (the same work `Ctrl+Shift+F11` does manually), so a player-built base's pathing updates
  immediately instead of waiting for a manual regen.
