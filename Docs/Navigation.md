# Navigation

Navigation is tiled navmesh built offline with Recast and loaded, queried, and steered at runtime with Detour, DetourTileCache, and DetourCrowd, all pinned as the `External/recastnavigation` submodule. Recast and Detour types stay private to two modules:

- **Navigation** (runtime): the `.hnav` format, `FNavigationWorld` with tile streaming, temporary obstacles, path queries and requests, and the crowd. Games link only Detour.
- **NavigationBuilder** (developer): `BuildNavigationData`, which runs Recast, and `BuildLevelNavigation`, which turns a level and its regions into a navigation file. The editor, `HertaEditorCmd`, staging, and tests use it; games never run Recast.

Polygon references are 64 bits (`DT_POLYREF64`), so the tile and polygon counts of large streamed worlds fit.

## One agent type

Navigation is built for one humanoid the size of the player: 0.3 m radius, 1.8 m tall, climbing 0.4 m ledges and 45 degree slopes, from 0.15 m by 0.1 m voxels. Tiles are 64 cells (9.6 m) square on a grid anchored at the world origin, so navigation built for different levels lines up. More agent types wait until a game needs a second size of agent.

## Building

`BuildLevelNavigation` collects the collision of the level's static bodies, fitted to their meshes' bounds exactly as physics fits them, as triangles. Boxes stay boxes, spheres and capsules become low-poly shapes. Movers, triggers, soft bodies, destructibles, and dynamic bodies move or break, so they are left to runtime obstacles. Only tiles overlapping a **Navigation Bounds** component's box are built, and a level without one has no navigation. Tiles build in parallel on the task system when one is given; the bytes are the same either way.

The navigation of a level and its regions is built in one piece: the level's static bodies and every region's scenery at its offset together, within every Navigation Bounds of either. Ground that continues from the level onto a region therefore meets across the seam, where navigation built separately would erode back from each edge. A tile then belongs to whichever bounds cover most of it: a region's tiles become that region's *patch* in the file and stream in and out with the region, and the rest stay with the level. This is World Partition's approach; a tile may hold a little ground of a neighbouring region that is not loaded, which agents near the player never reach because streaming loads what surrounds the player.

The source hash covers the geometry, bounds, settings, and Recast's revision, so navigation built by another Recast or from other geometry is told apart.

## Where navigation comes from

- **Editor:** while the viewport's **Navigation** overlay is on or Play runs, the editor rebuilds the open level's navigation on a worker 300 ms after an edit settles, from the meshes it has loaded. The Sandbox builds in about 16 ms. Play gets each new build.
- **Staging:** `game.stage` cooks the starting level's navigation with its region patches and writes `<level>.hnav` beside the staged level. Navigation is derived data; it is not kept in source.
- **Command line:** `nav.build <project.hertaproject> <output.hnav> [<level.hlevel>]` builds a file for inspection or tests.

## Runtime

`FNavigationWorld` loads sources with `AddData`. Where sources share a tile the newest covers older ones until it is removed, which keeps streamed patches correct even when they overlap. `GameFramework`'s session adds a region's patch when the region activates and removes it when the region unloads.

Obstacles are boxes turned about the vertical axis that cut walkable area out of the tiles they touch, rebuilt through DetourTileCache a few tiles per update. A moved obstacle re-cuts only once it moves 0.1 m or turns 0.05 rad, so a settling body does not rebuild tiles every step, and obstacles cut tiles that stream in after them. Unbroken destructibles, dynamic bodies at least 0.3 m tall, and **Navigation Obstacle** components become obstacles; a destructible's obstacle lifts when it breaks, which opens the way it blocked. Full runtime navmesh rebuilds are not needed: static geometry is cooked, and everything that changes at runtime is an obstacle.

`FindPath` searches at once; `RequestPath` searches over the following updates within `PathIterationsPerUpdate` node expansions and can be cancelled. A path to an unreachable goal comes back partial, ending at the closest point.

The crowd steers agents around each other with DetourCrowd's obstacle avoidance. The crowd moves its own agents; an owner that moves an agent itself, such as a physics character, moves it by its desired velocity and reports where it ended up, and an external agent such as the player is only avoided. An agent whose ground streams out waits off the navigation and rejoins when navigation under it loads.

Everything advances in `Update` in a fixed order, so the same calls give the same paths and steering on every run.

## Debug views

The viewport's **Overlays > Navigation** draws the built navigation, or Play's own with its obstacles cut as they move. Selected Navigation Bounds and Navigation Obstacle boxes draw as outlines. Automation's `viewport_view` switches the overlay and reports the latest build; `play_state` reports the loaded tiles, polygons, obstacles, and crowd agents.

## Limits

4096 tiles and 8192 layers loaded at once, 1024 obstacles, 128 crowd agents, and 64 waiting path requests by default. Tile cache layers are stored with run-length coding; a general compressor waits until navigation files grow large enough to matter.
