# Sandbox playground

The default Sandbox project opens `Games/Sandbox/Levels/Sandbox.hlevel`: a compact, editable feature playground rather than a separate demo framework. Colored glTF blocks live in `Games/Sandbox/Content/Playground`; Outliner folders organize the stations without adding ECS entities or transform parents. The arch and nested shelf remain real transform anchors.

Select a station in the Outliner and press **F** to frame it. Use **Alt+S** to simulate and **Escape** to restore the authored poses. Save edits with **Ctrl+S**; simulation never saves its transient poses.

## Current stations

- **Bounce comparison:** three gold cubes with restitution 0, 0.45, and 0.9, dropped from the same height.
- **Gravity comparison:** three coral cubes with gravity scales 0, 0.35, and 1. The zero-gravity cube remains suspended.
- **Friction and damping:** paired 18-degree ramps with friction 0 and 0.8, plus tilted tumblers with damping 0 and 1. Compare sliding and settling during simulation.
- **Stack and mass:** five dynamic blocks, with a 10 kg base and 1 kg upper blocks. Mass affects contact response, not gravitational acceleration.
- **Traversal course:** stairs, a landing, descent ramp, low/high hurdles, and spaced platforms. These are static collision geometry now, ready for the upcoming player controller.
- **Hierarchy workshop:** a rotated arch parent, nested shelf, mesh-only child, and empty entity. Exercise parent movement, reparenting, duplication, component editing, and undo/redo.
- **Shared mesh gallery:** repeated colored and checker meshes with varied scales and rotations. Exercise shared-mesh rendering, asset selection, and live reimport.

Collision currently uses mesh-bounds boxes, not arbitrary mesh collision. Physics comparisons are qualitative checks, not benchmarks. Use [scaling fixtures](Scaling.md) for 1k/5k/10k entity stress tests rather than making the startup level heavy.

Validate authored content from the repository root with `HertaEditorCmd level.validate Games/Sandbox/Levels/Sandbox.hlevel` and `HertaEditorCmd asset.validate --content-root Games/Sandbox/Content`. The editor's bounded capture mode, `HertaEditor --scaling-test=Games/Sandbox/Levels/Sandbox.hlevel --scaling-simulate --validation`, checks rendering, selection, simulation startup, and exact pose restoration without saving editor settings. It is not a long-running physics comparison.

## Growing the same playground

Follow the [milestone order](EngineDesign.md): Milestone 4.5 first adds editable PBR materials and shaders, directional/sky/point/spot/rect lights and shadows, sky/atmosphere, volumetric fog, and SMAA 1x. Add a material gallery, light/environment stations, and AA comparisons as those features become usable. Replace the separate colored glTF blocks with shared mesh geometry and authored material assignments; changing a color must not require re-exporting a model.

Milestone 5 then adds a controllable player, collision feedback, a simple course objective, and HUD using the established renderer. Later rendering, physics, animation, audio, and gameplay slices should add usable examples to these stations or expand the arena. Every new supported engine feature should have a repeatable playground check where practical; unsupported components and placeholder systems do not belong in the level.
