# Sandbox playground

The default project opens `Games/Sandbox/Levels/Sandbox.hlevel`: 193 editable entities in 16 Outliner folders. The original physics course and hierarchy examples remain intact. Cubes share the Engine mesh; editable `.hmat` assets provide their colors. A single shared sphere makes PBR reflections easier to compare.

Press **F** to frame a selection, **G** for game view, and **F11** for an immersive viewport. **Alt+S** simulates the physics stations; **Escape** restores authored poses. **Ctrl+S** saves the level, not simulation poses. **Ctrl+Space** reveals the bottom panel.

Number keys in the focused viewport jump to the level's camera bookmarks; **Viewport settings > Bookmarks** lists them:

| Key | Bookmark |
| --- | --- |
| 1 | Overview |
| 2 | Physics comparisons |
| 3 | Traversal and player scale |
| 4 | Hierarchy workshop |
| 5 | Soft bodies and collision shapes |
| 6 | Shadow distance lane |
| 7 | PBR material gallery |
| 8 | Textures and antialiasing |
| 9 | Local lighting bays |

**Ctrl+number** saves the current view into that slot. Bookmarks are saved with the level, so update them when a station moves.

## Visual authoring checks

- **PBR material gallery:** the front terrace compares metallic 0 and 1 across roughness 0.06, 0.18, 0.35, 0.55, 0.75, and 0.95. Double-click a material in the Content Browser to edit it, or use a mesh's material-slot editor. Changes preview before saving; material history is independent of level history.
- **Textures and alpha cutout:** the right terrace, viewed from the starting camera, combines sRGB base color, a linear normal map, and a packed linear ORM map (R: occlusion, G: roughness, B: metallic). The masked grille reveals the tiled wall through its openings. Adjust normal strength, UV scale, channels, and alpha cutoff to check their effects.
- **HDR emission:** cyan and amber strips use emissive intensity above ordinary surface brightness. They do not illuminate nearby objects; authored lights do that.
- **Sun and sky:** the directional sun casts outdoor shadows. The Sky component supplies atmosphere-derived diffuse lighting and roughness-dependent reflections. Rotate the sun or adjust atmosphere Rayleigh/Mie settings to compare the lighting and horizon.
- **Local lighting shadow bays:** the covered rear terrace isolates warm omnidirectional Point, cool cone-shaped Spot, and broad Rect lighting. Each bay has a pillar, a glossy sphere, and light-colored shadow receivers. Change range, intensity, temperature, cone angles, or emitter size in Details.
- **Volumetric height fog:** the environment folder owns the enabled fog component. Compare volumetric on/off, density, height falloff, anisotropy, and quality near the spot-light beam.
- **Shadow distance lane:** an 85 m strip east of the arena, viewed from bookmark 6. Gold pillars stand every 5 m and cast their shadows across the strip, with cyan distance stripes every 10 m and small coral contact cubes at 10, 30, 50, and 70 m. Use it to check cascade transitions, shadow detail close up, and the fade at the sun's 60 m shadow range.
- **Antialiasing:** the right terrace's narrow diagonal rails and grille provide high-contrast silhouettes for comparing SMAA 1x against no antialiasing. Reflections and texture detail are separate checks; SMAA is not temporal accumulation.

All five light types are authored entities. Atmosphere references the sun's stable object ID; a missing sun uses the renderer's directional-light fallback. The four shadow-casting lights use 12 of the 16 shadow views: four directional cascades, six point faces, one spot, and one rect projection. Sky does not cast a shadow.

## Physics and authoring checks

- **Bounce:** three gold cubes with restitution 0, 0.45, and 0.9 drop from the same height.
- **Gravity:** coral cubes compare gravity scales 0, 0.35, and 1.
- **Friction and damping:** paired ramps compare friction 0 and 0.8; tilted tumblers compare damping 0 and 1.
- **Stack and mass:** five dynamic blocks use a 10 kg base and 1 kg upper blocks. Mass affects contact response, not gravitational acceleration.
- **Traversal:** stairs, a descent ramp, hurdles, and spaced platforms are static collision geometry for the upcoming player controller.
- **Player scale gauges:** a 1.8 m coral capsule stands at the foot of the stairs for scale. Mint step-up blocks of 0.18, 0.35, and 0.5 m sit beside the gap platforms, and the platforms leave 1 m and 2 m gaps. They encode the proposed Milestone 5 controller limits: walk up 0.18 m, step up to 0.35 m, jump 0.5 m steps, the 0.6 m low hurdle, and a 2 m gap, but not the 1.3 m high hurdle.
- **Hierarchy workshop:** a rotated arch, nested shelf, mesh-only child, and empty entity exercise parenting, duplication, component editing, and undo/redo.
- **Shared mesh gallery:** repeated checker and colored cubes exercise instancing and material overrides without duplicated geometry.
- **Soft bodies:** the east bay's cyan gantry carries a gold rope with a 2 kg slate box, which starts 50 degrees off vertical and swings, and a cloth with the normal-mapped panel material, pinned at its top corners and released flat. The cloth checks UVs, tangents, and normal mapping on deforming geometry. A mint pressurized ball drops onto a 20 degree ramp and rolls off it. The rope, box, and cloth follow the gantry when its parent moves.
- **Collision shapes:** on the east terrace, a 15 degree ramp with friction 0.8 holds a box-collision cube in place while a sphere and a capsule lying on its side roll down to the arena rail. All three use the Engine meshes with matching collision.

Rigid collision is a box, sphere, or capsule fitted to mesh bounds, not arbitrary mesh collision. Soft bodies collide with those shapes through their own vertex radius. New visual-gallery objects have no rigid bodies; the rope box and the three collision-shape bodies are the only dynamic bodies outside the 16 physics comparisons. Use [scaling fixtures](Scaling.md) for 1k/5k/10k stress tests.

## Content and validation

Project-owned materials and textures live in `Games/Sandbox/Content/Playground`. The four 128x128 PNG maps are original, procedurally generated test data: beveled panel color/normal/ORM patterns and an RGBA cutout grille. They require no external art tools or third-party license. The Engine sphere is a unit-diameter, 48-segment/24-ring glTF primitive with explicit normals and UVs. The Engine capsule has a 1 m diameter and 2 m height along +Y, with 48 segments and 12 rings per cap.

From the repository root, run `HertaEditorCmd level.validate Games/Sandbox/Levels/Sandbox.hlevel` and `HertaEditorCmd asset.validate --content-root Games/Sandbox/Content`. `HertaEditor --visual-test --validation` renders 600 ready frames from the starting camera, logs GPU pass timings and render-target memory, then saves one image per camera bookmark to `Saved/VisualTest` without saving editor settings. `HertaEditor --scaling-test=Games/Sandbox/Levels/Sandbox.hlevel --scaling-simulate --validation` also checks selection, simulation startup, and pose restoration. Inspect the viewport to verify visual quality.

[Milestone 5](EngineDesign.md) adds a controllable player, course objective, collision feedback, and HUD to this same playground. Later engine features should add concrete checks here rather than create separate demo frameworks.
