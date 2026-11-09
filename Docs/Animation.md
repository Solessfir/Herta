# Animation

Status: Milestone 8 in progress. Skinned glTF models cook with their skeleton and clips, the runtime Animation module samples, blends, and skins them, and the player plays as an animated character in Play and the game with a first-person or third-person camera. The state machine graph, bloom, and the Sandbox station follow.

## Modules

| Module | Kind | Owns |
|---|---|---|
| `Animation` | Runtime | `FAnimationLibrary` (an immutable skeleton and its clips, shared between characters) and `FAnimationPose` (one character's sampling and blending memory). ozz-animation's runtime jobs stay private to it. |
| `AssetPipeline` | Developer | Building ozz skeletons, clips, and root-motion tracks from glTF inside `HertaAssetWorker`, with ozz's offline builders and optimizer. |

ozz-animation 0.17.0 is a pinned submodule under `External/ozz-animation`, built as `OzzBase`, `OzzAnimation`, and `OzzAnimationOffline`. Only the Animation module and the asset pipeline include its headers. Cooked assets hold ozz archives as opaque bytes, so the Assets module and everything downstream never see ozz types.

## Cooked data

A skinned model (cooked format 3) carries:

- one skin influence per vertex: four joint indices and four unorm16 weights summing to one;
- a skeleton: joint names, parents in parent-before-child order, column-major inverse bind matrices, and the ozz skeleton archive;
- named clips: duration, the ozz animation archive, and an optional root-motion track.

Each archive is sealed with its size and XXH3 hash. ozz trusts its input and reads past the end of a truncated archive, so `FAnimationLibrary::Create` rejects a seal that does not match before ozz reads anything, then checks that the archive's joints match the cooked names and parents.

The root joint's horizontal travel is extracted at cook time, so clips play in place. A clip whose root moves less than 1 mm keeps no root-motion track. `FAnimationLibrary::GetRootMotion` returns the travel between two clip times, wrapping once through the end for looping clips.

## Runtime

`FAnimationPose::Evaluate` samples up to eight weighted layers, blends them, and computes joint model transforms and skinning matrices (model transform times inverse bind matrix). Each layer slot keeps its own ozz sampling context, because a context caches the keyframes of the last clip it sampled. With no layers, or no positive weight, the pose is the bind pose. Evaluation allocates only the first time a new layer slot is used.

`FAnimationStateMachine` runs compiled state machine data: float, bool, and trigger parameters; states that play one clip or a 1D blend along a float parameter; and transitions with a duration, conditions that must all hold, and an optional wait for a non-looping state to finish. Transitions are tested in authored order, whether they leave the active state or any state, so the first one that can fire wins and the order is the priority. A blend advances at the weighted duration of the clips it mixes, so their phases stay in step. Interrupting a fade restarts it from the current state, so an interrupted fade can pop; blending from a frozen pose waits until a clip needs it. The Animation graph will compile to the same data.

## Player character

A level's `playerCharacter` (schema 11) names the skinned model the player plays as. `FGameSession` builds an `FPlayerCharacter` from the cooked model: the library, a pose, the default locomotion machine (an idle, walk, jog, and sprint blend by ground speed, crouching, slide start, loop, and exit, jump start, wall jump, air, and landing states, a mantle, and a wall run that plays the sprint cycle while the session leans the body, because the clips have none). It reads the mover's active mode, so it follows whichever modes the character uses, and its own copy of the mesh. Mantles and wall jumps are triggers from any state, listed first so they win. The mantle clip's root rises the 1 m it was authored to climb; the session draws the body lower by the pose's root height, and the first-person camera reads the head the same way, because the capsule already carries the body up the real ledge height. Every frame the pose is evaluated and skinned on the CPU (`SkinVertices`) into the mesh's vertex update, which shares the soft-body path for bounds, shadows, and ray-tracing refits. In first person the character draws two skinned copies through `FMeshRenderView::Visibility`: the intact body is hidden from the camera but casts shadows and appears to ray-traced reflections and global illumination, and a copy with the neck and head collapsed draws for the camera only. The camera sits 0.24 m ahead of and 0.07 m above the animated head joint, eased over about 50 ms, so it follows the body's lean, crouch, and slide without looking out through the neck, and looking down shows the chest and legs. The body follows the view yaw. In third person (V or the gamepad's North button) the body turns towards its movement, and a camera boom behind the shoulders sweeps a 0.2 m sphere with `FPhysicsWorld::CastSphere`, snapping in when something is in the way and easing back out. A model that cannot animate logs a warning and leaves the capsule playable.

Deferred: a first-person projection for arms and held items, with their own field of view and compressed depth so they never clip into walls, until the sword arrives in Milestone 8.5; GPU skinning, until character or vertex counts make CPU skinning measurable in a frame (the Mannequin evaluates in about 0.013 ms and skins its 7,538 vertices in about 0.16 ms on one core in Development, plus a 362 KB upload); per-vertex motion vectors for skinned and soft-body meshes, until temporal AA or another consumer reads them.

The Sandbox character is Quaternius's CC0 Mannequin from the Universal Animation Libraries 1 and 2, merged onto one 65-joint rig; see `Games/Sandbox/Content/Characters/Mannequin/Source.md` for the exact source and the clips kept.
