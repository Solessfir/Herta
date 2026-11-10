# World streaming

A level places other levels as **regions** that stream in and out around the player. A region is a placement of a whole level at an offset, so the same level placed several times is also the reusable piece of repeated scenery that a prefab would be.

## Authoring

Level schema 15 requires a top-level `regions` array after `cameraBookmarks`. Each region has an `id`, a `name`, a `level` path relative to the placing level's file with `/` separators, an `offset` in meters, and a `loadRadius` and larger `unloadRadius`, measured horizontally from the offset. Validation rejects duplicate IDs, absolute or drive paths, backslashes, non-`.hlevel` paths, nonfinite offsets, and an unload radius that is not larger than the load radius. A level places at most 256 regions, and regions do not nest: a region level's own regions are ignored.

In the editor, **Viewport settings > Regions** lists them with their radii. Click one to edit its offset and radii; dragging previews the change and releasing commits it as one undoable edit. Double-click or right-click **Frame** to look at it, and right-click to rename or delete. **Add region...** lists the other levels beside the current one and places the chosen one 30 m in front of the camera. The viewport draws each region's scenery where it is placed and its load ring, with the unload ring fainter. Region contents are edited by opening the region level itself; opening the placing level again rereads them.

A region streams only static scenery: Static Mesh entities without a Dynamic body, Static rigid bodies, and point, spot, and rect lights. Movers, triggers, dynamic and soft bodies, destructibles, characters, constraints, emitters, listeners, directional and sky lights, atmosphere, and fog stay with the main level; a region that has them warns once how many it left out. `IsLevelRegionScenery` in Level decides, so the editor preview, staging, and the session agree. Each placement mixes its identity into its lights' IDs, so repeated placements keep separate shadow caches.

## Runtime

`FGameSession::SetRegions` takes the level's regions and an `FRegionLoader`. `FWorldStreamer` decides each update from the player's feet:

- A region within its load radius starts loading on a BlockingIo task, nearest first. At most 2 load at once and 8 stay loaded; when a ninth is wanted, the farthest loaded region outside its load radius unloads first, or the request waits and counts as deferred.
- The loader reads the region's level on the worker. Leaving beyond the unload radius before it finishes sets its cancellation flag and drops the result, and the worker's later completion is ignored.
- A finished load hands its document to the game thread. The loaded region's `Prepare` hook runs each update until its assets are ready, then the session places the scenery at the offset, composed in double precision before rounding, adds its static bodies to the running physics world as one `FLevelStaticGroup`, and draws its meshes and lights after the level's own.
- Beyond the unload radius, the session removes the bodies, stops drawing the region, and calls its `Release` hook. A failed load is retried only after the player leaves and returns.
- Restarting keeps loaded regions and re-adds their bodies to the new physics world, and destroying the session cancels and waits for loads in flight.

`GetRegionStats` reports loaded, loading, and preparing regions, and total loads, unloads, cancellations, failures, deferrals, and the last error; editor `play_state` includes them.

The standalone game uses `MakeStagedRegionLoader`: the worker reads the staged region level and decodes the cooked assets of its scenery, `Prepare` uploads them into `FStagedGameAssets`, and `Release` drops their references. Assets the starting level loaded stay for the session; a streamed asset is freed when the last region using it unloads. Editor Play reads the level on a worker too, while the viewport preview already keeps every region's meshes and materials loaded through the preview cache, so `Prepare` only waits for them. `game.stage` stages each region level beside the starting level at its relative path, with its scenery's assets.

Positions stay in single precision relative to the world origin; the Sandbox's farthest region is 84 m out. The physics world's shiftable origin is available when a level reaches distances where that matters.

## Sandbox

`Games/Sandbox/Levels/Regions/Causeway.hlevel` is a 24 m walkway segment with curbs, a gate, and a lamp. `Sandbox.hlevel` places it three times end to end west of the arena, from the descent ramp out to 84 m, with 40 m load and 50 m unload radii, so walking out streams the segments ahead in and the ones behind out.

## Checks

- `HertaTests` cover region serialization and validation, streaming in and out with hysteresis, collision on a region's deck before and after a restart, the load budgets, failed loads and their retry, cancellation of a worker load, and staged loading that frees an asset only when its last region releases it.
- `HertaGame --smoke-test=300` crosses to the farthest region and back, and fails unless regions loaded and unloaded without failures and the run ends with the assets it held before leaving.
