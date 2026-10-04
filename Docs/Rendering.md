# Rendering foundation

Milestone 2 adds an offscreen textured mesh to the editor workspace, behind the docked Details and Output Log overlays. The editor camera uses a 65-degree vertical perspective field of view. Viewport layout and camera input are resolved before rendering the scene and recording its image, so resizing uses the current frame's aspect ratio. Render-target size limits scale both dimensions together. Its depth-tested meter grid uses subdued major lines and distance fading. Start is closed by default and can be toggled from the application menu. Existing saved layouts remain intact; a new Details panel inherits Start's saved dock when available.

## Ownership

- `RHI` owns resource descriptors, shared GPU handles, the graphics device contract, and the versioned cooked shader format. Calls belong to one render thread; all resource handles must be released before the owning presentation device.
- `NvrhiVulkan` privately maps those operations to NVRHI. Three independent command contexts retain recorded resources and upload storage until their graphics-queue submission serial completes. Cancelling a recording releases CPU commands without submitting GPU work.
- `RenderGraph` validates declared reads/writes, derives resource hazards, orders passes deterministically, and scopes transient ownership to first and last use. RHI retention extends GPU lifetimes past graph execution. This first graph runs on the graphics queue; physical aliasing and asynchronous compute are deferred.
- `Renderer` owns render meshes, the procedural world grid, offscreen color/depth targets, and clear/draw passes. It owns no content: the editor supplies view, projection, model matrices, one render mesh per model, grid visibility, and camera position. Null meshes, such as ones still loading, are skipped.
- `FRenderMesh` is the GPU copy of a cooked model: one vertex buffer, one index buffer, a mip-mapped texture per cooked texture, and its sections and bounds. `CreateTexturedCubeModel` builds the 1 m cube used by texture previews and renderer tests; the editor's default cube is the cooked `Engine/Content/Shapes/Cube.gltf`. Opaque instances sharing one GPU mesh batch into one indexed instanced draw per section and base color texture. Recorded frames share the mesh's handles, so replacing a mesh never waits on the GPU.
- `EditorCore` owns the viewport camera math. `EditorFramework` owns input routing, the preview transform, and its private im3d context; it copies im3d output into Herta debug primitives before rendering.

The world grid is a two-triangle GPU procedural pass after opaque geometry, with reversed-Z depth testing and no depth writes. `WorldGrid.slang` evaluates anti-aliased 1 m minor and 5 m major lines, fading from 50 m to 200 m around the camera. Only four projected vertices are uploaded per frame; the grid does not enter the CPU debug-line expansion path.

Uploads use NVRHI staging with a 64 MiB per-recording limit, exposed as `MaximumUploadBytesPerRecording`. `FRenderMesh::Create` submits and begins a new recording whenever the next write would exceed it, so a 4096x4096 texture with its mips uploads across several recordings. Textures are uploaded one mip level at a time and cannot be sampled until every level has been written. Frame reuse waits only for that context's submission, rather than waiting for the entire device every frame. Readback is an explicitly blocking diagnostic operation.

VSync is enabled by default and can be toggled from the application menu. The change is applied between frames by recreating the main and detached viewport swapchains with FIFO or the best available non-VSync present mode.

The scene uses column-major matrices, a +Z-forward camera, counter-clockwise front faces, infinite-far reversed-Z projection, depth clear `0`, and `GreaterOrEqual` depth testing. NVRHI supplies Vulkan's viewport Y inversion; shaders do not flip Y again.

Meshes use a camera-relative studio light: one directional light slightly above the camera plus ambient. Until cooked meshes carry normals, the fragment shader derives flat face normals from screen-space derivatives of the view-space position and orients them toward the camera, so every face reads clearly but smooth models appear faceted. The instanced vertex stream carries object-to-clip and object-to-view matrices (128 bytes per object). The single-draw fallback and common reflected shader layout retain the existing 128-byte push-constant ABI.

Scene textures and offscreen color targets use sRGB formats, with linear shader sampling and output blending. ToolUI samples the completed scene through an UNORM view so its existing display-encoded UI composition does not decode the scene twice.

## Viewport interaction

Milestone 2.5 uses RMB with WASD and QE for fly navigation, Alt+LMB to orbit, MMB to pan, and the wheel to dolly. While flying, the wheel adjusts movement speed and Shift accelerates movement. F focuses the preview object. The viewport toolbar exposes translation, rotation, scale, local/world axes, and snapping.

Camera and gizmo drags start only over the viewport image. They keep ownership through the drag and release it on focus loss or cancellation, so other editor widgets cannot drive the camera. Grid, axes, and bounds use scene depth; transform handles draw as an overlay. Scene owns authored entities; EditorFramework adapts their transforms to the viewport and records grouped authoring transactions for undo/redo.

Click the preview cube to select it; click empty viewport space to deselect it. Selection shows an orange silhouette outline and transform handles. Camera navigation and gizmo clicks preserve selection. This picker handles only the transformed preview cube; general scene selection and mesh outlines belong to the scene editor milestone.

Details edits the same preview transform as the gizmos: location in meters, XYZ Euler rotation in degrees, and positive per-axis scale. Compact RGB-marked fields omit axis labels. Each property has a reset button and a Local/World selector; the parentless preview has identical local and world values. Locking scale preserves all three starting proportions and rejects edits outside the positive supported range. Search filters transform properties. With no selection the panel displays a selection prompt. These edits are session-only.

Translation and scale use small colored plane handles. Scale plane handles change two local axes together while preserving their starting ratio. During dragging, translation displays meters, scale displays multipliers relative to the starting scale, and rotation displays the signed applied angle with a swept sector. Dotted guides replace full-length axis lines; releasing or cancelling hides the feedback.

Q hides the transform gizmo without deselecting; W/E/R select move/rotate/scale and show it again. L switches local/world axes while the viewport owns keyboard focus. These shortcuts do not override RMB flight controls. Escape cancels a transform drag and restores its starting transform. Alt+RMB also dollies. RMB navigation hides the pointer; flying displays the current speed beside FPS, including Shift acceleration. The toolbar's Grid (m) field sets the translation snap interval. Navigation currently uses ordinary pointer motion, so a continuous drag is bounded by the desktop edge; relative-pointer capture is not implemented.

im3d is pinned under `External/im3d` and is private to EditorFramework. The renderer accepts Herta-owned points, lines, and triangles. Pixel-width lines and points expand to triangles without requiring geometry shaders or native wide-line support.

## Shader cooking

The pinned Vulkan SDK supplies Slang. Setup validates its headers and compiler libraries on Windows and Linux. `ShaderCompiler` is a Developer module; runtime rendering depends only on RHI's cooked reader.

Building `HertaEditor` first builds `HertaShaderWorker` and runs the `HertaShaders` target. It cooks `Engine/Shaders/TexturedMesh.slang` and `Engine/Shaders/DebugDraw.slang` into matching `Shaders/<name>.vert.hshader` and `Shaders/<name>.frag.hshader` files beside the executable, plus `Shaders/TexturedMesh.instanced.vert.hshader` for the shared-mesh path. Builds do not download anything. Shipping strips shader debug information and never invokes a compiler at runtime.

Cooked files contain explicit little-endian versioned fields, stage and entry point, SPIR-V, reflected bindings and push-constant size, compiler version, permutation settings, source dependency hashes, and a content checksum. The reader bounds sizes and rejects corrupt or incompatible files. A successful cook atomically replaces the output; failure preserves the previous file.

Release cooks are byte-identical across checkout directories for identical sources and compiler settings. Slang debug information embeds absolute include paths, so Debug and Development artifacts can differ across checkout locations even when their code and dependency hashes match.

Manual invocation:

```text
HertaShaderWorker <source.slang> <vertex|fragment> <entry-point> <output.hshader> [--debug]
```

Existing files under `Engine/Shaders` participate in build dependencies. Regenerate project files when adding a shader or include file. Build-time cooking is implemented; interactive shader watching and editor hot reload remain a later extension of the worker boundary.

## Verification

`HertaTests` covers camera navigation and picking rays, viewport input ownership, gizmo dragging and snapping, debug primitive validation, graph hazards and failure cleanup, renderer projection and winding, shader serialization and corruption, deterministic Slang compilation, dependency tracking, reflection, and failed-cook preservation.

`HertaEditor --renderer-test` checks instanced versus single-draw GPU image equivalence, GPU image coverage, multi-section render meshes with per-section textures and uploaded mip chains, near-over-far depth occlusion, depth-tested and overlay debug primitives, point pixel size, recording cancellation, fence retirement, repeated offscreen resize/zero extents, and native resize/minimize/restore requests, then exits. The native resize sequence also switches panel blur between zero, normal, and maximum radius to exercise backdrop allocation and reuse. Development renderer tests require Vulkan validation instead of silently running without it. Smoke runs store layout and appearance under `TestResults/Smoke`.

On Windows, expose the pinned SDK layer when launching from a normal terminal:

```powershell
$env:VK_ADD_LAYER_PATH = (Resolve-Path 'SDK/Windows/Vulkan/1.4.357.0/Bin').Path
./Binaries/windows/x86_64/Development/HertaEditor.exe --renderer-test
```

Linux CI runs the same renderer tests on X11 and headless Wayland using lavapipe and Khronos validation. Window-manager requests remain capability-dependent; zero-extent resource tests are deterministic on both backends.
