# Physics

Herta wraps Jolt Physics in the runtime Physics module. Jolt types stay private; `FPhysicsWorld` exposes bodies, shapes, a character, sensors, contacts, constraints, soft bodies, snapshots, and debug lines through Herta math types. GameFramework's `FLevelSimulation` builds a world from authored level entities for editor Simulate, editor Play, and the standalone game.

## Determinism

Jolt is built with `JPH_CROSS_PLATFORM_DETERMINISTIC`, and the Physics module defines it too, because Jolt's headers change layout with it. Linux builds Jolt, Physics, and GameFramework with `-ffp-contract=off`; the x86-64 baseline has no FMA, so MSVC and ClangCL do not contract either. The same inputs then produce bit-identical results on Windows, Linux, and any thread count.

`SaveState` and `RestoreState` snapshot the whole world through Jolt's state recorder. `ComputeStateHash` hashes that snapshot with XXH3. Tests pin a world hash after fixed steps and compare it across one and several worker threads; CI runs them on both platforms, so a hash change on one platform fails there.

## Jobs and capacities

`FPhysicsWorldSettings::Tasks` runs Jolt's jobs as Herta tasks. Without it, or when a submit fails, the step runs its jobs on the calling thread. Every step waits for its submitted jobs before returning, so no job outlives the step. `MaxBodies`, `MaxBodyPairs`, `MaxContactConstraints`, `TempMemoryBytes`, and `MaxJobs` bound the world; exhausting one is reported as a step failure or a rejected body, never as silent loss of authored state. See `Docs/Scaling.md`.

## Events and origin

Bodies opt into contact reporting with `bReportContacts`. Jolt's callbacks only append to a locked buffer; after the step, `GetContactEvents` returns Began and Ended events sorted by body ID and phase, one per pair with the strongest impulse. Sensor events are sorted the same way. Gameplay reacts after the step, so it never mutates the world inside a callback or depends on Jolt's callback order.

Positions cross the API as double-precision `FWorldPosition`. `ShiftOrigin` moves every body and character so the simulation stays near zero, then rebuilds the broad phase.

## Collision

Layers are Static, Dynamic, Debris, and Character, of eight. `LayerCollisions` is a symmetric bitmask table: debris collides with static and dynamic bodies, but not with other debris or the player. Shapes are box, sphere, capsule, convex hull, and static compounds of hulls. `bContinuousCollision` uses linear casts for fast bodies such as thrown balls. `DestroyBody` refuses bodies a constraint still uses. The viewport's **Collision** toggle draws every shape as wireframe during Simulate and Play: static grey, kinematic blue, dynamic green, sensors amber, and sleeping bodies dimmed.

## Destruction

A Destructible component (level schema 10) fractures its mesh bounds into Voronoi cells when the simulation starts. Cell count and seed fix the pattern; the generator clips in double precision, so the same component always produces the same chunks. Fracturing happens at simulation start rather than in the asset pipeline, since a 64-cell box takes about 7 ms (Development build); cook it once fracture meshes come from imported art. Chunks sharing a face are bonded; chunks touching the bottom face anchor the object to the ground.

The intact object is one box body, static when anchored. A contact whose impulse reaches Strength breaks the bonds near the hit, within a radius that grows with the square root of the excess impulse. Connected islands are found again: an island on the footing stays static, larger loose islands become dynamic compounds, and single chunks become debris. Debris is limited by `FLevelDestructionSettings::MaxDebris` and removed after `DebrisLifetimeSeconds`. Editor Simulate keeps destructibles intact as plain bodies; Play and the game break them. Thrown balls (F, left mouse, or the gamepad's West button) are the Sandbox's damage source.
