# Animation

Status: Milestone 8 in progress. Skinned glTF models cook with their skeleton and clips, and the runtime Animation module samples, blends, and skins them. Rendering, the player character, the state machine graph, and the Sandbox station follow.

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

The Sandbox character is Quaternius's CC0 Mannequin from the Universal Animation Library; see `Games/Sandbox/Content/Characters/Mannequin/Source.md` for the exact source and the clips kept.
