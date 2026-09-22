# Rendering foundation

Milestone 2 adds an offscreen textured mesh to the editor's Viewport panel. Reset the editor layout from the hamburger menu to dock it beside Start and above Output Log. Existing saved layouts remain intact.

## Ownership

- `RHI` owns resource descriptors, shared GPU handles, the graphics device contract, and the versioned cooked shader format. Calls belong to one render thread; all resource handles must be released before the owning presentation device.
- `NvrhiVulkan` privately maps those operations to NVRHI. Three independent command contexts retain recorded resources and upload storage until their graphics-queue submission serial completes. Cancelling a recording releases CPU commands without submitting GPU work.
- `RenderGraph` validates declared reads/writes, derives resource hazards, orders passes deterministically, and scopes transient ownership to first and last use. RHI retention extends GPU lifetimes past graph execution. This first graph runs on the graphics queue; physical aliasing and asynchronous compute are deferred.
- `Renderer` owns the checkerboard cube, camera transform, offscreen color/depth targets, and clear/draw passes. Mesh and texture source import belong to Milestone 3.

Uploads use NVRHI staging with a 64 MiB per-recording limit. Frame reuse waits only for that context's submission, rather than waiting for the entire device every frame. Readback is an explicitly blocking diagnostic operation.

VSync is enabled by default and can be toggled from the application menu. The change is applied between frames by recreating the main and detached viewport swapchains with FIFO or the best available non-VSync present mode.

The scene uses column-major matrices, a +Z-forward camera, counter-clockwise front faces, infinite-far reversed-Z projection, depth clear `0`, and `GreaterOrEqual` depth testing. NVRHI supplies Vulkan's viewport Y inversion; shaders do not flip Y again.

Scene textures and offscreen color targets use sRGB formats, with linear shader sampling and output blending. ToolUI samples the completed scene through an UNORM view so its existing display-encoded UI composition does not decode the scene twice.

## Shader cooking

The pinned Vulkan SDK supplies Slang. Setup validates its headers and compiler libraries on Windows and Linux. `ShaderCompiler` is a Developer module; runtime rendering depends only on RHI's cooked reader.

Building `HertaEditor` first builds `HertaShaderWorker` and runs the `HertaShaders` target. It cooks `Engine/Shaders/TexturedMesh.slang` into `Shaders/TexturedMesh.vert.hshader` and `Shaders/TexturedMesh.frag.hshader` beside the executable. Builds do not download anything. Shipping strips shader debug information and never invokes a compiler at runtime.

Cooked files contain explicit little-endian versioned fields, stage and entry point, SPIR-V, reflected bindings and push-constant size, compiler version, permutation settings, source dependency hashes, and a content checksum. The reader bounds sizes and rejects corrupt or incompatible files. A successful cook atomically replaces the output; failure preserves the previous file.

Release cooks are byte-identical across checkout directories for identical sources and compiler settings. Slang debug information embeds absolute include paths, so Debug and Development artifacts can differ across checkout locations even when their code and dependency hashes match.

Manual invocation:

```text
HertaShaderWorker <source.slang> <vertex|fragment> <entry-point> <output.hshader> [--debug]
```

Existing files under `Engine/Shaders` participate in build dependencies. Regenerate project files when adding a shader or include file. Build-time cooking is implemented; interactive shader watching and editor hot reload remain a later extension of the worker boundary.

## Verification

`HertaTests` covers graph hazards and failure cleanup, renderer projection and winding, shader serialization and corruption, deterministic Slang compilation, dependency tracking, reflection, and failed-cook preservation.

`HertaEditor --renderer-test` checks GPU image coverage, near-over-far depth occlusion, recording cancellation, fence retirement, repeated offscreen resize/zero extents, and native resize/minimize/restore requests, then exits. Development renderer tests require Vulkan validation instead of silently running without it. Smoke runs store layout and appearance under `TestResults/Smoke`.

On Windows, expose the pinned SDK layer when launching from a normal terminal:

```powershell
$env:VK_ADD_LAYER_PATH = (Resolve-Path 'SDK/Windows/Vulkan/1.4.357.0/Bin').Path
./Binaries/windows/x86_64/Development/HertaEditor.exe --renderer-test
```

Linux CI runs the same renderer tests on X11 and headless Wayland using lavapipe and Khronos validation. Window-manager requests remain capability-dependent; zero-extent resource tests are deterministic on both backends.
