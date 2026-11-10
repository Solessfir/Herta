# Sandbox playground

The Sandbox project has two levels. `Games/Sandbox/Levels/Sandbox.hlevel`, the starting level, is gameplay: a parkour course up the middle of the arena for the player, with calibration gauges and interactive physics beside it. `Games/Sandbox/Levels/Visuals.hlevel` is the rendering and physics test bench: the material, texture, lighting, sky, fog, shadow, and antialiasing stations, the physics comparisons, and the authoring examples, which the visual test measures. Open it from the Content Browser or pass it on the editor's command line. Cubes share the Engine mesh; editable `.hmat` assets provide their colors.

Press **F** to frame a selection, **G** for game view, and **F11** for an immersive viewport. **Alt+S** simulates the physics stations and **Alt+P** plays as the player; **Escape** stops either and restores authored poses. **Ctrl+S** saves the level, not simulation poses. **Ctrl+Space** reveals the bottom panel. Number keys in the focused viewport jump to the level's camera bookmarks, **Viewport settings > Bookmarks** lists them, and **Ctrl+number** saves the current view into a slot. Bookmarks are saved with the level, so update them when a station moves.

## Gameplay level

61 entities in 7 Outliner folders.

| Key | Bookmark |
| --- | --- |
| 1 | Course overview |
| 2 | Start, slide, and hurdles |
| 3 | Mantle block |
| 4 | Wall run lane |
| 5 | Chimney finish |
| 6 | Movement gauges |
| 7 | Moving platforms and destruction |
| 8 | Animation station |

**Parkour course.** **Course start** and **Course finish** triggers carry the Start and Finish roles; the timer starts on leaving the start zone at the arena's front edge and stops on the chimney ledge. The course runs up the middle along +Z, each section sized to the Mirror's Edge movement numbers in [Game runtime](Game.md):

- **Run-up and slide:** 7 m of open floor, then a coral bar 1.3 m above the floor that only a sliding or crouching player passes. Sprint and press crouch just before it.
- **Hurdles:** a 0.6 m and a 1.3 m hurdle 2.5 m apart across the course. The 1.2 m jump clears both, or lands on the higher one.
- **Mantle block:** a 1.7 m block, too high for a jump and its step-up and too far from the hurdles to jump onto, so jumping at it mantles onto its top. It drops onto the wall run lane's run-up.
- **Wall run lane:** two 22 m walls 6 m apart line a lane crossed by an 8 m pit, 1.8 m deep. Run alongside a wall and jump while strafing toward it, or run at it at an angle, to wall run across the pit; a good run lands just past the far edge, and jumping off to the other wall chains a second run. A fall into the pit mantles out.
- **Chimney finish:** at the lane's end, a 4.5 m chimney with 2.5 m between its walls leads to a mint ledge 3.5 m up, which wall kicks reach. The finish trigger sits on the ledge.

Play it with **Alt+P** in the editor or as the standalone game.

- **Movement gauges:** to the left of the course, stairs, a descent ramp, and gap platforms with 1 m and 2 m gaps; a 1.8 m coral capsule for scale; and mint step-up blocks of 0.18, 0.35, and 0.5 m. They encode the controller limits: walk up 0.18 m, step up 0.35 m, jump the 0.5 m steps, the hurdles, and the 2 m gap, as the jump lifts the feet about 1.2 m.
- **Moving platforms and triggers:** to the right of the start, a cyan shuttle travels 6 m and back every 6 s carrying a coral cube through a trigger at its far end, and a gold elevator lifts a mint cube 2.5 m every 5 s into a trigger at the top. Triggers draw as boxes that turn green while occupied and log each enter and exit; the platforms themselves do not set them off. Both platforms hum through spatial Audio Emitters that ride with them, the elevator lower; walk around them during Play to hear the hum pan and fade out by 16 m.
- **Destruction yard:** to the right of the mantle block, two anchored walls (16 cells at strength 40, and a harder 24 cells at 90), an anchored pillar, and a loose crate fracture when hit hard enough. During Play, throw balls with **F**, the left mouse button, or the gamepad's West button. Hits break the bonds around the impact; pieces still standing on the footing stay put, loose pieces fall, and single chunks become debris that fades after 12 s. Turn on **Collision** in the viewport settings to see the chunk hulls. Simulate keeps them intact. See [Physics](Physics.md).

- **Animation station:** right of the hurdles, three blocks of 1.4, 2.8, and 4.2 m climb toward the course, each a mantle above the last. Footsteps fire as the player runs to it, and the drop from the 4.2 m tower lands hard enough to play and fire Land. Jump off the tower toward the course and press **Z** in the air to ragdoll; the body lands on the open floor short of the course, and **Z** again gets back up where it lies. Edit the Mannequin's animation graph from the player character settings and replay the station to check the change. Three training dummies stand in front of it, facing the start: swing the sword at one (left mouse button) to slice it along the blade, or throw balls (F) at its limbs to sever them; cut pieces fall and stay until restart. See [Animation](Animation.md).

- **Sound:** the level's sounds play throughout Play. Footsteps follow the run's pace through the Footstep Audio Graph's `Speed` input, and jumps, landings, slides, sword swings, slices, and severs each have their own sound. Every impact, from thrown balls, falling crates, and breaking walls, plays the Impact Audio Graph, which picks one of four waves and maps the hit's `Impulse` onto its pitch, filter, and gain: tap a wall with a ball, then throw hard, to hear the difference. Leaving the start and reaching the finish chime through the Objective graph, and the **Wind ambience** emitter streams 30 s of stereo wind under everything. The sounds are generated by `Games/Sandbox/Tools/GenerateSounds.py`; open any `.haudiograph` in `Audio` to audition it. See [Audio](Audio.md).

Every meshed object is a static body the player can stand on, except the destructibles, which build their own fracture collision.

## Visuals level

195 entities in 14 Outliner folders. It keeps its own copy of the arena floor and the sun, sky, and fog, with their own object IDs.

| Key | Bookmark |
| --- | --- |
| 1 | Overview |
| 2 | Physics comparisons |
| 4 | Hierarchy workshop |
| 5 | East terrace physics |
| 6 | Shadow distance lane |
| 7 | PBR material gallery |
| 8 | Textures and antialiasing |
| 9 | Local lighting bays |

### Visual authoring checks

- **PBR material gallery:** the front terrace compares metallic 0 and 1 across roughness 0.06, 0.18, 0.35, 0.55, 0.75, and 0.95. The two roughest ceramics also set diffuse roughness to their roughness, so EON flattens their shading towards the silhouette; compare them with Lambert by resetting diffuse roughness in the material editor. A front row isolates the newer BRDF terms, ordered from left to right on screen: felt and concrete each as a Lambert and EON pair (diffuse roughness 0 and 1), a rough cast iron sphere that multiple-scattering compensation keeps from darkening, and copper whose tinted edge color uses F82-tint Fresnel. Beyond the copper, a striped sphere is shaded by `GraphStripes.hmatgraph`, a Material Graph whose stripe mask drives both base color and roughness; open it with **Edit graph** in its material's Shader section. A comment frames the stripe pattern and a reroute knot carries the mask into roughness. The graph is a master: its two colors, two roughnesses, stripe frequency, and metallic are parameters. The two spheres beyond it are instances, `GraphStripesFine` (fine green stripes) and `GraphStripesGilded` (wide metallic gold bands), which set those parameters under Graph parameters in their material editors. The graph's `Horizontal` static switch masks by V instead of U; the gilded instance turns it on, so its bands run horizontally and it compiles to its own shader permutation, while the other two share the default one. Editing the graph updates all three, live while it is unsaved. Double-click a material in the Content Browser to edit it, or use a mesh's material-slot editor. Changes preview before saving; material history is independent of level history.
- **Textures and alpha cutout:** the right terrace, viewed from the starting camera, combines sRGB base color, a linear normal map, and a packed linear ORM map (R: occlusion, G: roughness, B: metallic). The masked grille reveals the tiled wall through its openings. Adjust normal strength, UV scale, channels, and alpha cutoff to check their effects.
- **HDR emission:** cyan and amber strips use emissive intensity above ordinary surface brightness. They do not illuminate nearby objects; authored lights do that.
- **Sun and sky:** the directional sun is the real 128,000 lx above the atmosphere, which auto exposure meters to around the "sunny 16" EV100 15, and casts outdoor shadows. Drag the time of day towards sunset to see the atmosphere redden and dim it. The Sky component supplies atmosphere-derived diffuse lighting and roughness-dependent reflections. Rotate the sun or adjust atmosphere Rayleigh/Mie settings to compare the lighting and horizon.
- **Local lighting shadow bays:** the covered rear terrace isolates real-world lamps: a 1,600 lm, 2700 K Point; a 1,200 lm, 4000 K Spot; and a 2,500 lm, 3000 K Rect. In daylight they are invisible next to the sky, as real lamps would be, and the bays still receive most of the open-sky light because GTAO occludes only what is on screen within 75 cm. To inspect them, drag the time of day past sunset; auto exposure adapts to the lamps. With auto exposure off, use an indoor night exposure such as f/2.8, 1/30 s, ISO 800 (about EV100 5). Each bay has a pillar, a glossy sphere, and light-colored shadow receivers. Change range, intensity, temperature, cone angles, or emitter size in Details.
- **Volumetric height fog:** the environment folder owns the enabled fog component. Compare volumetric on/off, density, height falloff, anisotropy, and quality near the spot-light beam.
- **Shadow distance lane:** an 85 m strip east of the arena, viewed from bookmark 6. Gold pillars stand every 5 m and cast their shadows across the strip, with cyan distance stripes every 10 m and small coral contact cubes at 10, 30, 50, and 70 m. Use it to check cascade transitions, shadow detail close up, and the fade at the sun's 60 m shadow range.
- **Antialiasing:** the right terrace's narrow diagonal rails and grille provide high-contrast silhouettes for comparing SMAA 1x against no antialiasing. Reflections and texture detail are separate checks; SMAA is not temporal accumulation.

All five light types are authored entities. Atmosphere references the sun's stable object ID; a missing sun uses the renderer's directional-light fallback. The four shadow-casting lights use 12 of the 16 shadow views: four directional cascades, six point faces, one spot, and one rect projection. Sky does not cast a shadow.

### Physics and authoring checks

- **Bounce:** three gold cubes with restitution 0, 0.45, and 0.9 drop from the same height.
- **Gravity:** coral cubes compare gravity scales 0, 0.35, and 1.
- **Friction and damping:** paired ramps compare friction 0 and 0.8; tilted tumblers compare damping 0 and 1.
- **Stack and mass:** five dynamic blocks use a 10 kg base and 1 kg upper blocks. Mass affects contact response, not gravitational acceleration.
- **Hierarchy workshop:** a rotated arch, nested shelf, mesh-only child, and empty entity exercise parenting, duplication, component editing, and undo/redo.
- **Shared mesh gallery:** repeated checker and colored cubes exercise instancing and material overrides without duplicated geometry.
- **Soft bodies:** the east bay's cyan gantry carries a gold rope with a 2 kg slate box joined to its free end by a Constraint on the box, which starts 50 degrees off vertical and swings, and a cloth with the normal-mapped panel material, pinned at its top corners and released flat. The cloth checks UVs, tangents, and normal mapping on deforming geometry. A mint pressurized ball drops onto a 20 degree ramp and rolls off it. The rope, box, and cloth follow the gantry when its parent moves.
- **Collision shapes:** on the east terrace, a 15 degree ramp with friction 0.8 holds a box-collision cube in place while a sphere and a capsule lying on its side roll down to the arena rail. All three use the Engine meshes with matching collision.

Rigid collision is a box, sphere, or capsule fitted to mesh bounds, not arbitrary mesh collision. Soft bodies collide with those shapes through their own vertex radius. Every meshed object is a static body, except the hierarchy example's child mesh, which shows a mesh without one. The rope box and the three collision-shape bodies are the only dynamic bodies outside the 16 physics comparisons. Use [scaling fixtures](Scaling.md) for 1k/5k/10k stress tests.

## Content and validation

Project-owned materials and textures live in `Games/Sandbox/Content/Playground`. The four 128x128 PNG maps are original, procedurally generated test data: beveled panel color/normal/ORM patterns and an RGBA cutout grille. They require no external art tools or third-party license. The Engine sphere is a unit-diameter, 48-segment/24-ring glTF primitive with explicit normals and UVs. The Engine capsule has a 1 m diameter and 2 m height along +Y, with 48 segments and 12 rings per cap.

From the repository root, run `HertaEditorCmd level.validate Games/Sandbox/Levels/Sandbox.hlevel`, the same for `Visuals.hlevel`, and `HertaEditorCmd asset.validate --content-root Games/Sandbox/Content`. `HertaEditor --visual-test --validation` opens the Visuals level, renders 600 ready frames from its starting camera, logs GPU pass timings and render-target memory, then saves one image per camera bookmark to `Saved/VisualTest` without saving editor settings. Add `--quality=low`, `medium`, or `high` to capture and time one rendering quality tier at every bookmark, and `--resolution=1920x1080` or `3840x2160` to render at a fixed size regardless of the display. On GPUs with ray queries, `--path-tracing` renders every bookmark with the reference path tracer until 1024 samples per pixel and saves them to `Saved/VisualTest/PathTraced` for side-by-side comparison with the raster captures. `--exposure=<EV100>`, such as 15 for sunlight or 10 for Sponza's arcades, replaces auto exposure with a fixed exposure, so captures compare region by region without each image metering itself differently. `--shimmer` reads back each bookmark's last 16 frames, logs how much the still view changes from frame to frame (the mean change in 8-bit levels and the share of pixels averaging over one level), and saves a `-shimmer.png` heatmap beside each capture at 32 grey levels per level of change. `--walk` then flies 2.4 m forward from each bookmark, one 5 cm step a frame, and saves `-moving.png` as it stops and `-stopped.png` once the same view has settled. `--hold-bookmark=N` instead stays on bookmark N after the 600 frames until the window closes, so a profiler such as Nsight traces the same view every run. The High tier's captures include global illumination, so comparing them against those references checks it at every station; the lighting bays and the hierarchy workshop show its bounce light most clearly. `HertaEditor --scaling-test=Games/Sandbox/Levels/Visuals.hlevel --scaling-simulate --validation` also checks selection, simulation startup, and pose restoration. Inspect the viewport to verify visual quality.

Later engine features should add concrete checks to the level they exercise rather than create separate demo frameworks.
