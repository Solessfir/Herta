# Rendering

See the [Milestone 4.5 completion report](Milestone45.md) for verified platforms, playground captures, measured budgets, and remaining limitations.

The editor renders authored levels through a native HDR PBR pipeline beneath the docked panels. The camera uses a 65-degree vertical perspective field of view. Viewport layout and camera input are resolved before rendering, so resizing uses the current frame's aspect ratio. Render-target size limits scale both dimensions together. The depth-tested meter grid uses subdued major lines and distance fading. Saved workspace layouts remain intact.

## Ownership

- `RHI` owns resource descriptors, shared GPU handles, the graphics device contract, and the versioned cooked shader format. Calls belong to one render thread; all resource handles must be released before the owning presentation device.
- `NvrhiVulkan` privately maps those operations to NVRHI. Three independent command contexts retain recorded resources and upload storage until their graphics-queue submission serial completes. Cancelling a recording releases CPU commands without submitting GPU work.
- `RenderGraph` validates declared reads/writes, derives resource hazards, orders passes deterministically, and scopes transient ownership to first and last use. RHI retention extends GPU lifetimes past graph execution. This first graph runs on the graphics queue; physical aliasing and asynchronous compute are deferred.
- `Renderer` owns GPU meshes/materials, HDR and display targets, shadows, environment lighting, fog, tone mapping, SMAA, and grid/debug passes. It owns no content: the editor supplies view/projection, model matrices, material-slot overrides, lights, environment settings, and overlays. Null meshes, such as ones still loading, are skipped.
- `FRenderMesh` is the GPU copy of a cooked model: vertex/index buffers, imported material defaults, mip-mapped textures, sections, and bounds. `FRenderMaterial` owns editable parameters and texture handles separately. Instances sharing one GPU mesh and material-slot overrides batch into indexed instanced draws. Recorded frames retain handles, so replacement never waits for the whole GPU.
- `EditorCore` owns the viewport camera math. `EditorFramework` owns input routing, the preview transform, and its private im3d context; it copies im3d output into Herta debug primitives before rendering.

The world grid is a two-triangle GPU procedural pass after opaque geometry, with reversed-Z depth testing and no depth writes. `WorldGrid.slang` evaluates anti-aliased 1 m minor and 5 m major lines, fading from 50 m to 200 m around the camera. Only four projected vertices are uploaded per frame; the grid does not enter the CPU debug-line expansion path.

Uploads use NVRHI staging with a 64 MiB per-recording limit, exposed as `MaximumUploadBytesPerRecording`. `FRenderMesh::Create` submits and begins a new recording whenever the next write would exceed it, so a 4096x4096 texture with its mips uploads across several recordings. Textures are uploaded one mip level at a time and cannot be sampled until every level has been written. Frame reuse waits only for that context's submission, rather than waiting for the entire device every frame. Readback is an explicitly blocking diagnostic operation.

VSync is enabled by default and can be toggled from the application menu. The change is applied between frames by recreating the main and detached viewport swapchains with FIFO or the best available non-VSync present mode.

The scene uses column-major matrices, a +Z-forward camera, counter-clockwise front faces, infinite-far reversed-Z projection, depth clear `0`, and `GreaterOrEqual` depth testing. NVRHI supplies Vulkan's viewport Y inversion; shaders do not flip Y again.

Meshes carry cooked normals and tangents with explicit handedness. Surface lighting uses metallic-roughness GGX PBR, authored lights, and filtered environment lighting. The instanced vertex stream carries object-to-clip and object-to-view matrices (128 bytes per object). Single draws retain the 128-byte push-constant ABI. Unlit new levels and asset thumbnails have a studio preview; authored lighting replaces it.

Color textures use sRGB sampling; numerical maps use Linear. Scene lighting and fog accumulate into RGBA16F HDR targets. Manual exposure and tone mapping produce the sRGB display target. ToolUI samples the completed image through an UNORM view so UI composition does not decode it twice.

## Viewport interaction

Use RMB with WASD and QE for fly navigation, Alt+LMB to orbit, MMB to pan, and the wheel to dolly. While flying, the wheel adjusts movement speed and Shift accelerates movement. F focuses the selection. The viewport toolbar exposes translation, rotation, scale, local/world axes, and snapping.

Camera and gizmo drags start only over the viewport image. They keep ownership through the drag and release it on focus loss or cancellation, so other editor widgets cannot drive the camera. Grid, axes, and bounds use level depth; transform handles draw as an overlay. Level owns authored entities; EditorFramework adapts their transforms to the viewport and records grouped authoring transactions for undo/redo.

Click a mesh or editor marker to select its entity; click empty viewport space to deselect. Ctrl/Shift-click toggles additional entities, and dragging from empty space box-selects projected bounds. Selection shows transform handles and a two-pixel outline of the rendered meshes, including alpha cutouts, dimmed where other geometry hides them. The outline rasterizes the selected meshes into a selection depth target with the depth-only shadow pipeline and composites after SMAA. Camera navigation and gizmo clicks preserve selection. Light and environment components have typed editor guides, including selected ranges and emitter shapes. G hides authoring overlays without hiding the optional camera coordinates or entering Play.

Details and gizmos edit world-space location in meters, XYZ Euler rotation in degrees, and positive per-axis scale; levels store parent-local transforms. Compact RGB-marked fields omit axis labels and trailing zeros. Changed properties expose revert arrows. Locking scale preserves all three starting proportions and rejects edits outside the positive supported range. Search filters properties and components. Multi-selection changes are grouped into undoable gestures and saved with the level. With no selection the panel displays a selection prompt.

Translation and scale use small colored plane handles. Scale plane handles change two local axes together while preserving their starting ratio. During dragging, translation displays meters, scale displays multipliers relative to the starting scale, and rotation displays the signed applied angle with a swept sector. Dotted guides replace full-length axis lines; releasing or cancelling hides the feedback.

Q hides the transform gizmo without deselecting; W/E/R select move/rotate/scale and show it again. L switches local/world axes while the viewport owns keyboard focus. These shortcuts do not override RMB flight controls. Escape cancels a transform drag and restores its starting transform. Alt+RMB also dollies. RMB navigation hides the pointer; flying displays the current speed beside FPS, including Shift acceleration. The toolbar's Grid (m) field sets the translation snap interval. Navigation currently uses ordinary pointer motion, so a continuous drag is bounded by the desktop edge; relative-pointer capture is not implemented.

im3d is pinned under `External/im3d` and is private to EditorFramework. The renderer accepts Herta-owned points, lines, and triangles. Pixel-width lines and points expand to triangles without requiring geometry shaders or native wide-line support.

## Visual authoring

[Materials](Materials.md) describes editable PBR assets, mesh slots, and project Slang iteration. Directional, Sky, Point, Spot, and Rect lights are independently optional level components, available through placement and Add Component. Details exposes linear color or temperature, physical intensity, range, shadow biases, spot cones, and rectangle dimensions. Directional range sets shadow distance, 60 m by default, and the last cascade fades out over its final 15%.

The renderer admits at most 32 enabled lights in stable entity order. Shadows use one 4096x5120 reversed-Z Depth32 atlas (80 MB) with at most 16 views. The first shadow-casting Directional light owns four 2048x2048 cascades in the top 4096x4096; further directional lights cast no shadows. Point (six faces), Spot, and Rect (one each) share sixteen 512x512 tiles in the strip below. Admission is whole-light, so insufficient space disables that light's shadows rather than rendering incomplete cube faces. Cascade normal offset scales with each cascade's world texel size. Viewport settings exposes Off, Hard, and Soft shadows and the budgets; both use bilinear-weighted PCF over 2x2 and 4x4 texels. Rect surface lighting integrates a 4x4 emitter sample grid; its initial shadow is a single center projection, not a soft area shadow. There is no light baking or dynamic GI.

Sky Light filters a cooked HDR environment or the procedural atmosphere into diffuse irradiance and roughness-dependent GGX specular mips. Filtering runs on background tasks and keeps the previous environment while updating. Visibility and ambient strength are independent. Sky Atmosphere links a directional sun and ray-marches single-scattering Rayleigh, Mie, and ozone from sea level, using the authored scattering scales, anisotropy, planet radius, and atmosphere height. Multiple scattering uses Hillaire's 2020 transfer table (Psi_ms): a 32x32 RGBA32F table over sun cosine and altitude, integrating second-order isotropic scattering over 64 directions with Earth's 0.3 ground albedo and amplifying it by 1 / (1 - f_ms). The CPU builds it only when the scattering scales, planet radius, or atmosphere height change; moving the sun reuses it. With an atmosphere, the directional sun is authored above the air (about 128,000 lx for the real sun). Surfaces and fog receive it multiplied by the transmittance along its direction, so sunsets redden, and once the sun sets direct light stops and the sky falls dark. The visible sun disc has the sun's real luminance, its illuminance over a 0.27 degree radius (about 600 million cd/m2), and clips at the half-float scene-color maximum. Image-based lighting leaves the disc out because the directional light already provides it. Covered interiors still receive unoccluded sky light, because there is no sky occlusion or global illumination, so realistic indoor lamps only read once the sky is dark. Below the horizon, each direction shows the sky's horizon colour for its azimuth, darkening towards the nadir, so an open level reads as distant haze instead of a flat ground plane without repeating the sky's gradient or sunrise glow upside down. Environment filtering bakes the same model into a 128x64 table once per change, then filters it like an authored environment map. Clouds, aerial perspective, and sky occlusion are not implemented.

Height Fog integrates density, albedo, and anisotropy along view rays to opaque depth. Density is constant below the fog entity's height and falls off exponentially above it, so a void beneath the level is not filled with opaque fog. Ambient in-scattering uses the sky's mean radiance, estimated from the zenith and nadir irradiance. Volumetric mode adds supported lights and shadow visibility with 16/32/64 steps. The fog target is quarter resolution, capped at 512 pixels per dimension, and composites with depth-aware upsampling. Disabling volumetric mode keeps height fog at eight integration steps. There is no temporal history, so cuts and resize need no history reset; low sample counts can show integration banding.

The pass order is shadow atlas, HDR sky and PBR geometry, fog and depth-aware composite, exposure/tone mapping, SMAA 1x, then grid/debug overlays and ToolUI. Exposure is photographic EV100: the tone mapper scales scene luminance by 1 / (1.2 * 2^EV100), the saturation-based convention from Lagarde and de Rousiers. The editor derives EV100 from the viewport's physical camera aperture, shutter, and ISO, or meters it automatically. Automatic exposure stays on the GPU: a pass samples the HDR scene into a 32x32 grid of centre-weighted log luminance, and a one-pixel pass builds a 64-bin histogram, averages its 50th to 95th percentile so a dark sky cannot overexpose a lit room, converts that to the saturation-based EV100 (log2(L) + 3), clamps it to EV100 1 to 18, and eases towards it with rates of 3/s brightening and 1.5/s darkening. Tone mapping reads the resulting one-pixel exposure, so nothing is read back to the CPU. Both passes together cost about 0.03 ms. Viewport settings provides the camera, SMAA Off/Low/Medium/High/Ultra, and shadow quality; the Performance panel reports GPU pass timings and render-target memory. Defaults are the "sunny 16" EV100 15 (f/16, 1/125 s, ISO 100), SMAA High, and Soft PCF shadows. SMAA uses the [unmodified reference implementation and lookup data](../External/SmaaUpstream.md). It does not resolve temporal subpixel shimmer, and requires no motion vectors or history.

## Shader cooking

The pinned Vulkan SDK supplies Slang. Setup validates its headers and compiler libraries on Windows and Linux. `ShaderCompiler` is a Developer module; runtime rendering depends only on RHI's cooked reader.

Building `HertaEditor` first builds `HertaShaderWorker` and runs the `HertaShaders` target. It cooks the mesh, shadow, sky, fog/composite, tone-map, SMAA, grid, and debug shaders into `Shaders/*.hshader` beside the executable, including instanced mesh and shadow variants. Builds do not download anything. Shipping strips shader debug information; the runtime renderer consumes cooked shaders without depending on Slang.

Cooked files contain explicit little-endian versioned fields, stage and entry point, SPIR-V, reflected bindings and push-constant size, compiler version, permutation settings, source dependency hashes, and a content checksum. The reader bounds sizes and rejects corrupt or incompatible files. A successful cook atomically replaces the output; failure preserves the previous file.

Release cooks are byte-identical across checkout directories for identical sources and compiler settings. Slang debug information embeds absolute include paths, so Debug and Development artifacts can differ across checkout locations even when their code and dependency hashes match.

Manual invocation:

```text
HertaShaderWorker <source.slang> <vertex|fragment> <entry-point> <output.hshader> [--debug] [--include-root <path> ...]
```

Existing files under `Engine/Shaders` participate in build dependencies. Regenerate project files when adding a shader or include file. The editor also watches material shader sources and literal includes/imports, compiling immutable snapshots asynchronously through the worker. Compatible vertex, instanced vertex, and fragment pipelines publish atomically; failure retains the last valid pipeline. See [Materials](Materials.md) for mounted paths and the exact shader ABI.

## Verification

`HertaTests` covers camera/picking and viewport input ownership, grouped authoring, graph/resource failure cleanup, renderer projection/winding, PBR material serialization and color-space/channel rules, normals/tangents, HDR environment filtering, light/shadow budgets, shader reflection and deterministic compilation, and shader snapshot coalescing with last-valid publication.

`HertaEditor --renderer-test` checks instanced versus single-draw GPU image equivalence, GPU image coverage, multi-section render meshes with per-section textures and uploaded mip chains, near-over-far depth occlusion, depth-tested and overlay debug primitives, point pixel size, recording cancellation, fence retirement, repeated offscreen resize/zero extents, and native resize/minimize/restore requests, then exits. The native resize sequence also switches panel blur between zero, normal, and maximum radius to exercise backdrop allocation and reuse. Development renderer tests require Vulkan validation instead of silently running without it. Smoke runs store layout and appearance under `TestResults/Smoke`.

`HertaEditor --visual-test --validation` renders the normal Sandbox playground and camera until 600 presented, asset-ready frames complete, then exits. It uses isolated layout/appearance settings rather than loading or writing personal preferences, and has a five-minute loading/presentation deadline. The log reports viewport size, total render-target bytes, and GPU timings for each visual pass. It then visits each of the level's camera bookmarks in slot order, lets each settle for 60 frames, logs its total GPU time and draw count, and saves the viewport, overlays included, to `Saved/VisualTest/00-Start.png` and `NN-<Bookmark name>.png`. This exercises the authored material/light/sky/fog/SMAA path; the images are for review, not a screenshot comparison, and do not replace checking appearance in the editor.

On Windows, expose the pinned SDK layer when launching from a normal terminal:

```powershell
$env:VK_ADD_LAYER_PATH = (Resolve-Path 'SDK/windows/Vulkan/1.4.363.0/Bin').Path
./Binaries/windows/x86_64/Development/HertaEditor.exe --renderer-test
./Binaries/windows/x86_64/Development/HertaEditor.exe --visual-test --validation
```

Linux CI runs the same renderer tests on X11 and headless Wayland using lavapipe and Khronos validation. Window-manager requests remain capability-dependent; zero-extent resource tests are deterministic on both backends.
