# Herta Engine Design

Status: Proposed foundation design
Last updated: 2026-07-27

## 1. Purpose

Herta is a C++23 game engine and editor for Windows and Linux. It is a learning engine with the long-term goal of becoming a production-ready engine capable of shipping real games. Learning determines the order in which systems are built, not the quality bar applied to them.

The design takes Unreal's strict runtime, editor, developer, and program boundaries, then starts with Godot-sized modules. It does not reproduce Unreal Engine's accumulated size before Herta needs those features. A module is split only when ownership, reuse, build time, or platform isolation justifies it.

The priorities are:

1. Correct lifetime and dependency boundaries.
2. A fast edit, build, import, and debug loop.
3. A renderer that teaches Vulkan without leaking Vulkan through the engine.
4. Deterministic asset builds and versioned serialized data.
5. A capable editor built as a client of the runtime.
6. Dependencies added only when their milestone needs them.
7. Production concerns such as recovery, profiling, security, compatibility, packaging, and migration designed in before release pressure arrives.

Herta does not claim production readiness during its foundation milestones. Production readiness is earned through shipped projects, measured performance, stable data formats, upgrade paths, broad hardware testing, and reliable tooling.

Herta is not required to become:

- A feature-for-feature copy of Unreal Engine.
- A multi-backend renderer.
- A dynamically reloadable plugin ecosystem.
- A custom standard library, allocator, scripting VM, and ECS built at the same time.

## 2. Non-negotiable decisions

### 2.1 Coordinate system

Herta uses one coordinate system everywhere. It is not configurable per project.

| Property | Convention |
|---|---|
| Handedness | Right-handed |
| Axes | Left-Up-Forward |
| Left | +X |
| Up | +Y |
| Forward | +Z |
| Distance | Meters |
| Mass | Kilograms |
| Time | Seconds |
| Angles | Radians internally, degrees in editor UI |
| Positive rotation | Counter-clockwise |
| Vectors | Column vectors |
| Matrix storage | Column-major |
| Transform order | `Translation * Rotation * Scale` |
| Quaternion serialization | XYZW |
| Clip-space depth | 0 to 1 |
| Front face | Counter-clockwise |
| Depth buffer | Reversed-Z, infinite far plane where practical |

The axes and units match the [glTF 2.0 coordinate convention](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html): glTF defines +Y as up, +Z as forward, -X as right, meters for linear distance, and radians for angles. Standard glTF transforms therefore need no axis or unit conversion. USD and other DCC unit conversions remain importer concerns, never runtime policy.

World-space positions use a dedicated `FWorldPosition` backed by double-precision meters. Render and Jolt data use origin-relative floats in the same meter scale. The wrapper preserves an origin-rebasing path without forcing every vector operation to use doubles.

Meters are the serialized and API convention. Editor fields may display centimeters or other user-selected units, but convert at the UI boundary and never change stored values or simulation scale.

Every third-party boundary has named adapter functions and tests. Unit conversion occurs only when source metadata uses a different scale. No reinterpret casts between Herta, Jolt, ozz, glTF, or NVRHI math types.

### 2.2 Math

Herta owns its public math templates and common aliases:

```cpp
template <std::floating_point T> struct TVector2;
template <std::floating_point T> struct TVector3;
template <std::floating_point T> struct TVector4;
template <std::floating_point T> struct TQuaternion;
template <std::floating_point T> struct TMatrix3;
template <std::floating_point T> struct TMatrix4;

using FVector2 = TVector2<float>;
using FVector3 = TVector3<float>;
using FVector4 = TVector4<float>;
using FQuaternion = TQuaternion<float>;
using FMatrix3 = TMatrix3<float>;
using FMatrix4 = TMatrix4<float>;

using FVector3d = TVector3<double>;
```

The normal engine type does not need an `f` suffix everywhere. Float is the local/rendering default, so `FVector3` is concise. Precision suffixes are used when code deliberately differs from that default, such as `FVector3d`. These aliases are permanent contracts and must not silently change scalar type later.

Additional semantic types are:

- `FTransform`
- `FBox`, `FSphere`, `FFrustum`, `FPlane`
- `FWorldPosition`

Dimension-specific templates are preferred over a public `Vector<float>` because dimension is part of the operation contract. Cross products, homogeneous coordinates, alignment, and specialized storage do not apply equally to every dimension.

The first implementation should be small, scalar, and heavily tested. SIMD is added behind the same API after profiling. GLM may be used as a private bootstrap or test oracle, but GLM types must never cross a public Herta API. Prefer no GLM dependency until it solves a measured problem.

The math tests must lock down handedness, transform order, quaternion composition, projection matrices, winding, glTF round trips, and Jolt/ozz conversions. Coordinate mistakes become asset corruption when left ambiguous.

### 2.3 Ownership and errors

- RAII owns resources and shutdown occurs in reverse construction order.
- Unique ownership is the default. Use `std::unique_ptr` unless ownership is genuinely shared.
- Non-owning parameters use references, pointers, `std::span`, or stable handles.
- Fallible construction returns `std::expected<T, FError>`.
- Herta code does not throw across module boundaries. Do not globally disable exceptions until every dependency has been audited.
- Owning systems are non-copyable and normally non-movable.
- Serialized references use stable IDs. Raw pointers and process-local handles are never serialized.
- Global constructors and service-locator access are forbidden. Executables explicitly compose services.

### 2.4 Containers and strings

Use standard C++ containers and views first. Do not create Unreal-shaped aliases such as `TArray` or `FString` without different, proven semantics.

- UTF-8 is the engine string encoding.
- Paths use `std::filesystem::path` at OS boundaries and normalized UTF-8 asset paths internally.
- APIs prefer `std::string_view`, `std::span`, and ranges.
- Frame and importer scratch allocations may use `std::pmr` arenas.
- A custom allocator is added only after profiling shows a reason.

### 2.5 C++ conventions

Use Unreal-style naming where it communicates engine semantics:

- `F` for value types and non-reflected engine objects.
- `I` for pure interfaces and `E` for enums.
- `U` and `A` are reserved for future reflected object and actor families. Do not use them before those semantics exist.
- Prefix booleans with `b`.
- Use PascalCase for types, functions, members, and local variables, matching the selected Unreal convention consistently.
- Put code in the `Herta` namespace. Platform and third-party names do not enter it through broad `using` directives.

Prefer C++23 library features when they are implemented consistently by the supported MSVC, Clang, and GCC toolchains. Use RAII, concepts, ranges, `std::span`, `std::expected`, and scoped enums where they simplify contracts. Do not use a new feature only to make ordinary code less recognizable.

Comments explain constraints, engine quirks, or why a decision is non-obvious. They do not narrate the code. Public headers remain self-contained and include only what their declarations require.

## 3. Repository structure

The planned repository layout is:

```text
Herta/
|-- .github/
|   |-- workflows/
|   |   |-- ci.yml
|   |   |-- codeql.yml
|   |   |-- dependency-review.yml
|   |   |-- nightly.yml
|   |   |-- quality.yml
|   |   `-- release.yml
|   `-- dependabot.yml
|-- Build/
|   `-- Premake/
|       |-- Modules.lua
|       |-- Toolchains.lua
|       `-- ThirdParty/
|-- Config/
|   `-- Dependencies.lock
|-- Docs/
|   `-- EngineDesign.md
|-- Engine/
|   |-- Content/
|   |   `-- Editor/
|   |       |-- Fonts/
|   |       `-- Icons/
|   `-- Source/
|       |-- Runtime/
|       |   |-- Core/
|       |   |-- Math/
|       |   |-- Platform/
|       |   |-- Application/
|       |   |-- Reflection/
|       |   |-- Assets/
|       |   |-- Scene/
|       |   |-- GraphCore/
|       |   |-- ToolUI/
|       |   |-- Audio/
|       |   |-- Navigation/
|       |   |-- AI/
|       |   |-- RHI/
|       |   |-- RenderGraph/
|       |   |-- Renderer/
|       |   |-- Physics/
|       |   `-- Animation/
|       |-- Editor/
|       |   |-- EditorCore/
|       |   |-- EditorFramework/
|       |   |-- AssetEditor/
|       |   `-- GraphEditor/
|       |-- Developer/
|       |   |-- AssetPipeline/
|       |   |-- NavigationBuilder/
|       |   |-- PackageBuilder/
|       |   |-- StateTreeCompiler/
|       |   `-- ShaderCompiler/
|       `-- Programs/
|           |-- HertaEditor/
|           |-- HertaEditorCmd/
|           |-- HertaGame/
|           |-- HertaAssetWorker/
|           |-- HertaShaderWorker/
|           |-- HertaCooker/
|           `-- HertaTests/
|-- External/
|   |-- doctest/
|   |-- entt/
|   |-- fastgltf/
|   |-- glfw/
|   |-- imgui/
|   |-- imgui-node-editor/
|   |-- jolt/
|   |-- lunasvg/
|   |-- miniaudio/
|   |-- nvrhi/
|   |-- ozz-animation/
|   |-- recastnavigation/
|   `-- zstd/
|-- Games/
|   `-- Sandbox/
|       |-- Config/
|       |-- Content/
|       `-- Source/
|-- Scripts/
|-- Tools/
|   `-- Blender/                  # Herta exporter scripts, not a Blender installation
|-- .editorconfig
|-- .gitattributes
|-- .gitignore
|-- .gitmodules
|-- Cleanup.bat
|-- Cleanup.sh
|-- GenerateProjectFiles.bat
|-- GenerateProjectFiles.sh
|-- LICENSE
|-- premake5.lua
|-- README.md
|-- Setup.bat
`-- Setup.sh
```

Generated and local data never enter source control:

```text
Binaries/<platform>/x86_64/<configuration>/
DerivedDataCache/<platform>/
Intermediate/Build/<platform>/x86_64/<configuration>/<target>/
Intermediate/ProjectFiles/<premake-action>/
SDK/<platform>/<tool>/<version>/
Saved/Logs/
Saved/Crashes/
Saved/Editor/
```

Each engine module follows one shape:

```text
Renderer/
|-- Public/Herta/Renderer/
|-- Private/
|-- Tests/
`-- Renderer.lua
```

Only `Public` is exported as an include directory. `Private` may include `Public`, never the reverse. Tests live with their owner and are linked into `HertaTests`.

The module Lua file declares public dependencies, private dependencies, platform sources, definitions, and third-party usage. The root `premake5.lua` remains the single workspace ownership point and loads these declarations.

Milestone 0 replaced the preserved GLFW recipe with the real Herta workspace. The root `premake5.lua` owns workspace policy, while `Build/Premake` owns shared toolchain and module declarations. Per-module Lua files should be introduced when modules need meaningful independent build policy, not merely to split a short list across files.

## 4. Module architecture

In the following graph, `A -> B` means A depends on B:

```text
Core
|-- Math -> Core
|-- Platform -> Core
|-- Reflection -> Core
|-- Assets -> Core + Reflection
|-- Application -> Core + Math + Platform
|-- Scene -> Core + Math + Reflection + Assets
|-- GraphCore -> Core + Reflection + Assets
|-- ToolUI -> Core + Application + RHI
|-- RHI -> Core + Math
|   `-- NvrhiVulkan -> RHI + Platform
|-- RenderGraph -> Core + RHI
|-- Renderer -> Core + Math + Assets + RHI + RenderGraph
|-- Physics -> Core + Math
|-- Animation -> Core + Math + Assets
|-- Audio -> Core + Math + Assets
|-- Navigation -> Core + Math + Assets
`-- AI -> Core + Math + Assets + Scene + GraphCore + Physics + Navigation

Full game composition -> Application + Scene + Renderer + Physics + Animation + Audio + Navigation + AI
EditorCore -> Core + Platform, then adds Reflection + Assets + Scene as those milestones arrive
EditorFramework -> EditorCore + ToolUI
AssetEditor -> EditorFramework + AssetPipeline
GraphEditor -> EditorFramework + GraphCore
NavigationBuilder -> Core + Math + Assets + AssetPipeline
PackageBuilder -> Core + Assets
StateTreeCompiler -> Core + Reflection + Assets + GraphCore + AI
HertaEditor -> EditorFramework + runtime modules available in the current milestone
HertaEditorCmd -> EditorCore + selected Runtime and Developer command modules
HertaAssetWorker -> AssetPipeline
HertaCooker -> AssetPipeline + PackageBuilder
GUI programs -> Application + ToolUI + selected modules
Programs -> only the modules each program composes
```

Runtime modules must never depend on `Editor`, `Developer`, or `Programs`. Editor modules may use runtime modules. Developer modules are build-time systems and do not ship in `HertaGame`.

The graph above shows the eventual dependency direction, not a requirement that Milestone 1 link modules that do not exist yet. Composition roots grow with the roadmap. The initial `EditorCore` uses only Core and Platform services; project assets, reflection, scenes, graph compilers, and other services are added when their owning milestones arrive.

Start with these modules. Do not reproduce Unreal's current module count. Split a module when at least one is true:

- It has a reusable public contract with multiple implementations.
- It must be excluded from a target or platform.
- Its dependency footprint harms unrelated builds.
- Its ownership or lifetime differs from the current module.
- It has become large enough to obscure dependency direction.

### 4.1 Core

Core contains types that do not know about windows, rendering, scenes, or editor UI:

- Fixed-width types, IDs, flags, hashes, and version numbers.
- Assertions: `HCHECK`, `HVERIFY`, and recoverable `HENSURE` equivalents.
- `FError` and `std::expected` helpers.
- Logging categories, levels, and sinks.
- Delegates/events with explicit connection lifetimes.
- Time primitives, byte buffers, UUIDs, and lightweight profiling markers.
- Module registration contracts.

Formatting may use `std::format` initially. Add `fmt` only if compiler support, compile time, or diagnostics justify it. The logger is Herta-owned even if formatting is delegated.

### 4.2 Platform

Platform wraps OS primitives through compile-time selected implementations:

- Files, directories, memory mapping, and file watching.
- Processes, pipes, dynamic libraries, and environment access.
- Threads, synchronization, high-resolution clocks, and CPU information.
- Crash context and debugger detection.

Windows and Linux headers stay in `Private/Windows` and `Private/Linux`. Runtime interfaces are appropriate for replaceable services such as rendering, physics, audio, and editor hosts. Virtual dispatch is unnecessary for basic OS calls selected at compile time.

### 4.3 Application and custom title bar

Application owns windows, displays, input devices, cursors, clipboard, and event pumping. GLFW is private to this module.

The custom title bar design is:

1. Create a GLFW window with `GLFW_TITLEBAR` disabled while preserving the native resize frame and platform behavior.
2. Render the title bar and window controls in the editor UI.
3. Register Herta's generic GLFW hit-test callback for draggable and resize regions.
4. Let the GLFW platform backends translate hit-test results into Win32, X11, and Wayland behavior.
5. Route minimize, maximize, restore, close, double-click, and system menu actions through `FWindow`.

Editor code must not handle `WM_NCHITTEST`, X11 client messages, or Wayland serials. Those details belong in the GLFW fork. Each ImGui platform viewport installs the same callback because every viewport is a native window.

Wayland does not expose every global-window operation available on Win32/X11. Window placement and custom-decoration behavior must degrade honestly instead of faking unsupported state. The public API reports capabilities.

The GLFW fork should contain only a generic, upstreamable custom-titlebar API. Herta-specific colors, buttons, ImGui state, and engine events stay in Herta. The fork remains pinned as a submodule and its upstream copyright remains intact.

### 4.4 Reflection and serialization

Reflection will support serialization, editor property inspection, asset references, and graph pins. Herta does not need a reflection system in Milestone 0.

Phase 0 uses explicit serializers for the few types that exist. Phase 1 introduces Herta-owned runtime descriptors with explicit registration and stable type/property IDs when scene serialization and inspectors need them.

[C++26 static reflection](https://www.open-std.org/jtc1/SC22/wg21/docs/papers/2025/p2996r13.html) is the preferred future discovery mechanism, but it does not replace Herta's runtime metadata contract. A standard compiler can enumerate C++ declarations, but Herta must still define stable serialized names and IDs, versions, migrations, editor attributes, object construction, and which members participate.

As of 2026-07-27, [GCC 16 implements reflection behind `-freflection`](https://gcc.gnu.org/projects/cxx-status.html), while [upstream Clang lists P2996 as unavailable](https://clang.llvm.org/cxx_status) and MSVC does not provide a portable public implementation. Herta will not drop MSVC or make one experimental compiler the production ABI solely for reflection.

Phase 2 uses standard reflection to produce the same Herta descriptors after all baseline compilers support the required subset. A non-required GCC 16 research target may validate the design earlier. Until then, keep registration explicit. Do not add Unreal-like annotation macros, a header parser, or generated glue preemptively. If compiler convergence takes too long and manual registration becomes the measured bottleneck, reconsider Clang tooling then. Never parse C++ with regular expressions.

Serialization rules:

- Every format has a magic value, format version, and engine schema version.
- Never dump a C++ object ABI directly.
- Unknown fields can be skipped where forward compatibility matters.
- Migrations are explicit and tested.
- Asset and object references serialize stable IDs, not paths alone.
- Source/editor formats may be readable. Cooked runtime formats prioritize deterministic loading.

Generated files use `.gen.h` and `.gen.cpp`, live under `Intermediate/Generated`, and are never manually edited or committed.

### 4.5 Scene and world

ECS is Herta's canonical runtime world representation, not its universal object model. World entities and gameplay components use ECS storage. Assets, editor documents, windows, devices, allocators, registries, and other engine services remain ordinary Herta-owned C++ objects.

- `FWorld` owns entities and component storage.
- `FEntityId` is a generational runtime handle.
- `FObjectId` and `FAssetId` are stable serialized identifiers.
- The scene hierarchy describes transforms and authoring relationships, not C++ ownership.
- Creation and destruction are deferred to well-defined frame barriers.
- Queries declare component read and write access so ordering and parallel execution can be validated.
- Component pointers and views cannot survive a structural-change barrier.
- Cross-system events are buffered and consumed at explicit phases.
- Runtime systems produce narrow bridge data for physics, animation, audio, AI, and rendering.

Actor-like authoring objects may be added later, but the storage model must not require one heap allocation and virtual tick per object. Physics, animation, audio, navigation, and rendering do not own Scene entities. The program composition root extracts or submits the data each system needs, preventing dependency cycles.

EnTT is the preferred storage implementation candidate for the world milestone, not a foundation dependency. It uses sparse-set component pools and has different scheduling and locality tradeoffs from an archetype-chunk ECS such as Unreal Mass. Before selection, a focused spike must cover fragmented and homogeneous component mixes, hierarchy operations, deferred mutation, parallel read/write queries, component relocation, cloning, editor inspection, and iteration at 100,000 to 1,000,000 entities. If adopted, EnTT remains private behind Herta entity, world, and query contracts. No EnTT type is serialized or exposed by a public Herta API.

### 4.6 RHI, RenderGraph, and Renderer

Herta targets Vulkan 1.3 on Windows and Linux. Vulkan 1.4 features may be used only after capability checks and a deliberate baseline update.

The layers are distinct:

- `RHI` defines Herta resource handles, descriptors, queues, command lists, synchronization, and debug names.
- `NvrhiVulkan` creates and owns Vulkan instance/device policy, then adapts Herta RHI operations to NVRHI.
- `RenderGraph` owns pass scheduling, transient lifetimes, dependency analysis, and queue synchronization intent.
- `Renderer` owns scene extraction, views, materials, lighting, culling, and render features.

NVRHI types must not leave `NvrhiVulkan/Private`. Herta's API should be thin and intentional, not a one-to-one copy of NVRHI. Direct Vulkan escape hatches are private, rare, and labeled.

NVRHI does not create Herta's whole platform policy. The composition root obtains required instance extensions and a surface from `FWindow`, selects a physical device, creates queues and the logical device, then constructs the NVRHI device. No ImGui Vulkan helper may participate in device or swapchain selection.

NVRHI can track resource states and deferred destruction, but RenderGraph remains Herta's authority for pass dependencies and transient reuse. Destruction is tied to submitted frame serials or fence completion, never only a swapchain image index.

Do not add VMA beside NVRHI by default. NVRHI's current Vulkan allocator owns `VkDeviceMemory` allocation, so VMA is not a drop-in layer. Adopt VMA only after measurement and an explicit NVRHI allocator integration or fork.

Renderer progression:

1. Device, swapchain, clear, and validation-clean triangle.
2. Shader cooking, vertex/index buffers, textures, and material parameters.
3. Depth prepass or depth-only path, reversed-Z, camera-relative transforms.
4. PBR mesh rendering, image-based lighting, and shadows.
5. GPU-driven culling, indirect draws, meshlets, and async work after profiling.

Debug and Development enable Vulkan validation, object names, RenderDoc markers, and NVRHI validation. Shipping disables validation and runtime shader compilation.

### 4.7 Shader pipeline

Slang is the preferred shader compiler when the first real renderer shader pipeline is built. It provides HLSL-like source, SPIR-V output, modular compilation, and reflection on Windows and Linux.

`HertaShaderWorker` owns compilation. Its output is a cooked shader asset containing:

- SPIR-V bytecode per entry point and stage.
- Reflected bindings and push-constant layout.
- Permutation key and compiler version.
- Source dependency hashes.
- Debug symbols for non-Shipping builds.

Runtime code consumes cooked bytecode and metadata. Editor hot reload starts the worker process and atomically swaps a successful result. A failed compile keeps the last valid shader.

Pipeline caches are keyed by shader identities, attachment formats, vertex layout, and fixed-function state. Cache files include device and driver identity and may always be discarded.

### 4.8 Assets and derived data

Runtime never loads `.blend`, source PNGs, or arbitrary editor formats directly.

```text
Source Content
    -> importer worker
    -> canonical Herta asset representation
    -> platform cooker
    -> DerivedDataCache or packaged cooked data
    -> runtime asset loader
```

An asset build key hashes:

- Source bytes and normalized source path.
- Import settings.
- Importer and cooker versions.
- Dependency hashes.
- Target platform and renderer feature level.
- Relevant engine format versions.

Workers write temporary output and rename atomically after success. Failed imports preserve the last valid cooked asset and return structured diagnostics.

`fastgltf` handles editor/offline glTF 2.0 ingestion. Imported data is converted immediately to Herta coordinates, types, naming, and canonical vertex formats. glTF library types do not enter runtime modules.

Meshoptimizer is added when real mesh cooking exists. KTX2/Basis Universal is added when the texture cooker exists. Neither belongs in bootstrap code.

### 4.9 Native `.blend` import

"Native `.blend` support" means the editor can accept `.blend` as a source asset with no manual export step when Blender is installed system-wide. Blender is an optional external authoring tool, not an engine, build, runtime, Setup, or SDK dependency. HertaEditor, HertaGame, normal glTF import, builds, tests, and cooked content must work without it.

HertaEditor discovers Blender through an explicit editor setting first, then `PATH` and platform-standard installation locations. It probes the executable and version before enabling `.blend` import. An unsupported or missing installation disables only `.blend` import and live reimport, with an actionable editor message.

When available, `HertaAssetWorker` launches the discovered system Blender executable as an external process:

```text
blender --background --factory-startup --disable-autoexec <file.blend> \
    --python-exit-code 1 --python <herta_export.py> -- <arguments>
```

Blender processes arguments in order. Factory settings and script auto-execution restrictions are applied before the `.blend` file is loaded, and a Python exception becomes a failing worker exit code.

The Herta-controlled script exports glTF/GLB or a small Herta Scene IR into a temporary directory. The normal glTF pipeline handles the result. The cache key includes Blender version, exporter version, source hash, and import settings.

[Blender embeds and starts its own Python interpreter](https://docs.blender.org/api/current/info_overview.html#python-in-blender). The `--python` option runs inside Blender, so Herta does not require a system Python installation, Python environment, or Python package manager. `herta_export.py` is versioned importer code owned by Herta and should use only `bpy` plus Python's standard library where practical. Do not pass `--python-use-system-env`, because machine-specific packages would make import output non-reproducible. Removing embedded-Python use would mean giving up seamless import, implementing Blender's private file format, or linking Blender, all of which are worse boundaries.

The worker process boundary is required for crash isolation, cancellation, timeouts, and Blender's GPL license boundary. Untrusted `.blend` files run with automatic embedded-script execution disabled. Blender logs and structured exporter diagnostics are attached to the asset import result.

Previously cooked assets imported from `.blend` continue to load when Blender is absent. Only importing or rebuilding stale `.blend` source requires Blender. If a cooker encounters a required `.blend` source with no valid cooked generation and Blender is unavailable, it fails that asset with a precise diagnostic instead of failing engine initialization.

An optional persistent Blender worker process may be explored after the simple process is correct and measured to be too slow.

#### Blender live reimport

When a valid system Blender installation is available, saving a tracked `.blend` file must update every dependent asset and visible instance in HertaEditor without a manual export or editor restart.

```text
Blender save
    -> Platform file watcher detects the source change
    -> AssetPipeline debounces the event and hashes stable file contents
    -> HertaAssetWorker exports through headless Blender
    -> normal glTF import and cooking run in the background
    -> AssetRegistry validates the complete replacement dependency set
    -> successful assets are published atomically at a frame boundary
    -> editor viewports and inspectors receive an asset-reloaded event
```

The implementation must handle Blender's temporary-file and rename-based save patterns. File events are hints, not proof that a file is ready. Reimport begins only after the path is readable and its size and modification state have remained stable for a short debounce window. Content hashes suppress duplicate work.

Only the newest request for a source asset matters. An in-flight import is cancelled when possible or allowed to finish without publishing if a newer source generation exists. Blender work never blocks the main or render thread.

Runtime references use stable `FAssetId` handles. A successful reimport creates a new immutable asset generation, then swaps the registry entry atomically. Renderer resources change at a frame boundary and the old GPU generation is retired by frame serial or fence completion. Scene instances retain their identity, transforms, and explicit per-instance overrides while their imported mesh, skeleton, animation, material defaults, and hierarchy data refresh according to the asset's reimport policy.

Import and GPU creation must both succeed before the new generation becomes visible. On failure, Herta keeps the last valid asset, reports Blender output and structured importer diagnostics in the editor, and continues watching for the next save.

If Blender becomes unavailable while the editor is running, Herta keeps the last valid generation, marks live reimport as unavailable, and resumes after a valid executable is configured or discovered.

Live reimport is one-way from Blender to Herta. Herta must not write changes back into `.blend` files unless a separate, explicit round-trip workflow is designed later.

### 4.10 Physics

Jolt is private to Physics. Herta supplies allocator, job-system, logging, assertion, layer-filter, and debug-draw adapters.

- Physics uses a fixed timestep, initially 60 Hz.
- Game/render interpolation is separate from simulation state.
- Collision callbacks are buffered into Herta events.
- World mutation is forbidden while Jolt is invoking callbacks.
- Public APIs use `FPhysicsBodyId`, Herta collision shapes, and Herta math.
- Jolt SIMD and precision definitions are identical in every translation unit.
- Float physics space is relative to a physics origin derived from `FWorldPosition`.
- Herta and Jolt both use meters, kilograms, seconds, and radians. Adapters convert math representation and origin, not physical units.
- Default gravity is `-9.80665 m/s^2` along the down axis.

Thread count, temporary allocator size, broad-phase layers, sleeping, and determinism are configuration, not scattered constants.

### 4.11 Animation

ozz-animation is private to Animation and AssetPipeline.

- Offline ozz builders, optimizers, and serializers run only in the asset worker.
- Runtime ozz sampling and local-to-model jobs operate on immutable cooked data.
- Herta owns animation state machines, blend trees, events, root motion, retargeting policy, and graph assets.
- Skinning output is converted to Herta renderer buffers at a narrow bridge.
- ozz types are not serialized as Herta public scene types.

Sampling and blending are natural job-system workloads, but a custom task graph should not be built before this parallel work exists.

### 4.12 Editor and graph system

The interactive editor hosts the runtime. Runtime code never includes ImGui or editor headers. `EditorCore` owns project loading, transactions, validation, automation contracts, and other editor services that must also run headless. It does not depend on Application, ImGui, RHI, Renderer, or an audio device.

Dear ImGui is used for editor, tool, and optional GUI-application interfaces, not Herta game UI. `ToolUI` owns context lifetime and platform/render integration so a GUI program can use ImGui without linking Scene, Physics, Animation, AI, or EditorCore. Use the maintained docking branch with multi-viewports, pinned to an exact commit or release tag.

Herta does not adopt Stack Layout PR 846. Docking, tables, groups, child regions, `SameLine`, and explicit sizing cover the initial editor. `imgui-node-editor` itself works with standard ImGui. If repeated editor code later proves a missing layout abstraction, add small Herta-owned helpers using public ImGui APIs before considering an internal ImGui patch.

When the first domain requires a free-form node canvas, Herta uses the maintained [Solessfir imgui-node-editor fork](https://github.com/Solessfir/imgui-node-editor) as a pinned submodule. It remains attributed third-party code. Upstream history is preserved, Herta-specific changes are documented separately, and upstream synchronization remains possible. Themes, node contents, icons, menus, and domain behavior stay in `GraphEditor`; the fork changes only when its public API or rendering internals prevent required interaction, DPI, navigation, hit-testing, or draw-order behavior.

`imgui-node-editor` provides node drawing and interaction only. It is not Blueprints. `GraphCore` owns schema-neutral graph identities, versioning, validation contracts, compile diagnostics, and headless compiler interfaces. Each graph domain owns its semantics, compiler, IR, migration, and executor. The system separates:

```text
Graph asset data
    -> schema and typed-pin validation
    -> compiler to Herta graph IR
    -> bytecode or specialized output
    -> runtime executor

GraphEditor
    -> transient mapping from stable graph IDs to node-editor IDs
    -> selection, layout, transactions, diagnostics, and debugging views
```

The first graph domain is Herta StateTree for NPC decision-making. Its hierarchy may use a nested state-oriented editor and does not force the node-canvas dependency into that milestone. Shader Graph, animation graphs, Audio Graph, and a general gameplay Blueprint VM are separate domain compilers built on the same editing and diagnostic foundations. They must not share one weakly typed universal executor.

Source graph validation and compilation run in `HertaEditorCmd`, compiler modules, workers, and tests without ImGui or imgui-node-editor linked. Game and server targets do not ship authoring compilers; they load and execute immutable cooked domain programs. StateTree compilation belongs to `StateTreeCompiler`, while AI owns the cooked StateTree format and executor.

Undo/redo uses transaction objects with stable object/property paths. Inspector edits, graph edits, asset renames, and scene changes must enter the same transaction system.

### 4.13 Editor icons and fonts

Source icons remain `.svg` files under `Engine/Content/Editor/Icons`; fonts remain `.ttf` or `.otf` files under `Engine/Content/Editor/Fonts`. Each asset keeps its license metadata beside the source. Editor assets are content, not generated C++ byte arrays. A cooked editor package may be embedded through the normal package system when producing a monolithic executable.

`EditorFramework` owns a Herta SVG adapter with LunaSVG private behind it. The adapter rasterizes an icon to RGBA at the requested physical pixel size, uploads it as an ImGui texture, and caches by source hash, pixel dimensions, and scale. Rasterization may run off the UI thread, but GPU upload and cache publication occur at an explicit frame boundary.

Herta editor icons use a deterministic static SVG subset: paths and basic shapes, `viewBox`, solid fills, strokes, clipping, and gradients when needed. Scripts, animation, filters, external resources, embedded raster images, and SVG text are rejected. This keeps rendering portable and avoids hidden font, network, and timing dependencies.

Milestone 1 title-bar controls use ImGui draw primitives and do not require a production icon set. LunaSVG and the real editor icon library enter only when the first SVG-backed editor tool requires them. The visual language and source icon set are selected deliberately before production assets are generated.

### 4.14 Audio

Audio is a Herta-owned runtime module with miniaudio private behind it. miniaudio supplies Windows and Linux device backends, decoding, resampling, mixing primitives, spatialization, and a null backend. Herta owns sound assets, handles, buses, voices, concurrency limits, streaming policy, scene integration, profiling, and the public API.

- The audio callback never allocates, blocks, acquires engine locks, or performs normal logging.
- Game-thread changes cross through a bounded command queue and audio-thread events return through a bounded result queue.
- Headless programs use no device or the null backend without changing gameplay logic.
- Source WAV and FLAC are sufficient initially. Add Opus when measured dialogue or music streaming requirements justify it.
- Static, streaming, and already compressed audio payloads carry explicit cook policy and are not blindly compressed again with zstd.
- Audio Graph later compiles to immutable runtime data and has no runtime dependency on ImGui or imgui-node-editor.

Spatial audio consumes Herta transforms in meters. Backend coordinate conversion occurs in one tested adapter. Advanced HRTF or acoustic simulation remains an optional extension selected after the basic mixer and spatial path are measured.

### 4.15 Navigation and AI

The Navigation subsystem splits offline and runtime ownership. `NavigationBuilder` uses Recast to build deterministic tiled navmeshes from canonical Herta collision geometry. Runtime `Navigation` links Detour to load cooked tiles and perform queries. DetourCrowd is the initial local avoidance and crowd implementation; add another avoidance library only after measured limitations.

- Recast and Detour types remain private to the Navigation subsystem.
- Navmesh settings, agent profiles, source geometry hashes, and Recast version participate in the cook key.
- Runtime path requests are asynchronous, cancellable, and return Herta path handles and points.
- Large worlds stream independently validated navmesh tiles.
- Dynamic obstacles use Detour tile-cache updates for bounded runtime changes. Static geometry changes schedule an editor or cooker rebuild through `NavigationBuilder`; runtime never invokes the Recast builder.
- Debug draw, query traces, and failed-path diagnostics are available in the editor and headless tests.

Herta AI owns perception, working memory, StateTree execution, scheduling, tick-rate scaling, gameplay tasks, and navigation requests. Perception uses Herta physics queries and gameplay events. ECS stores agent state, but AI scheduling remains an explicit system with deterministic phase ordering.

Herta StateTree is a first-party compiled hierarchical decision runtime, not a wrapper over BehaviorTree.CPP or HFSM2. The authoring graph compiles into one immutable, versioned program containing aligned contiguous sections for states, transitions, nodes, constants, property bindings, and debug mappings. States reference child, task, condition, and transition ranges by index. Transitions use direct state indices or relative jumps rather than runtime pointer traversal.

Compilation must:

- Validate types, hierarchy, transitions, bindings, unreachable states, and illegal cycles.
- Flatten authoring data in deterministic order and produce a stable cooked hash.
- Pack immutable configuration separately from aligned per-instance mutable data.
- Retain stable node IDs for diagnostics and hot-reload migration, with optional Shipping stripping.
- Reject programs that exceed format limits instead of truncating indices.

One compiled program is shared by all agents using that tree. Each agent stores only an active-state stack, queued events, execution status, and a pooled instance-data handle. Native tasks and conditions dispatch through registered Herta function tables. The runtime bounds transitions per update to prevent infinite immediate-jump loops, buffers events, supports utility-based child selection, and emits deterministic traces for debugging and replay.

### 4.16 Packages, embedded applications, and mods

All runtime content is read through a Herta virtual filesystem and package interface. Loose cooked files, external packages, executable-embedded packages, and mod packages differ only in how their byte ranges are mounted.

Assets owns runtime package reading, VFS mounts, precedence, decompression, and asset lookup. `AssetPipeline` owns source import and canonical asset cooking. `PackageBuilder` owns package indexes, chunk layout, compression, executable embedding, and deterministic archive output. `HertaAssetWorker` isolates individual imports and cooks, `HertaCooker` orchestrates AssetPipeline and PackageBuilder for a target, and `HertaEditorCmd` only exposes those implementations as headless commands.

```text
Embedded base package
External base packages  -> ordered mount table -> VFS -> asset loader
External mod packages
```

A package contains a versioned index and independently readable chunks. Each chunk records asset identity, type, offset, alignment, compression codec, compressed size, uncompressed size, and content hash. Zstandard is the default general-purpose codec. BC/KTX textures, Opus audio, and other already compressed payloads normally use `None`. Decompression validates all sizes and configured limits before allocation.

The cooker supports three output policies:

- Loose cooked files for development.
- Executable plus external packages for normal games and large applications.
- A monolithic executable with the base package added as a platform linker resource or read-only section before signing.

Executable embedding never generates C++ byte arrays and uses the same package reader as external content. Project targets compose only the modules they need, so a GUI application may use Application and ToolUI without Scene, Physics, Animation, or game systems. A monolithic executable still relies on the operating system, GPU driver, Vulkan loader availability, and required platform libraries.

Initial mod support is data-only. Mods remain external even when the base package is embedded. Each mod manifest declares a stable namespace, version, engine compatibility, dependencies, load order constraints, and explicit asset-override intent. Mount resolution is deterministic, asset-ID collisions are diagnosed, and missing or incompatible dependencies produce actionable errors. Mod packages are untrusted input and use the same bounds, recursion, decompression, and path validation as imported assets.

Native C++ binary mods are deferred because they would expose an unstable ABI and execute unrestricted code. A future behavior-mod boundary should use a versioned scripting or sandboxed runtime contract. Source mods built together with an exact Herta revision remain possible without promising binary compatibility.

## 5. Executables

| Program | Responsibility | Ships to players |
|---|---|---:|
| `HertaEditor` | Editor host, runtime preview, asset and graph tools | No |
| `HertaEditorCmd` | Orchestrates headless editor validation, migration, import, cooking, and graph-compilation commands | No |
| `HertaGame` | Standalone Sandbox game target | Yes |
| `HertaAssetWorker` | Isolated source import and canonical asset-cooking tasks | No |
| `HertaShaderWorker` | Shader compilation and reflection | No |
| `HertaCooker` | Orchestrates target asset cooking and deterministic platform package construction | No |
| `HertaTests` | Unit and fast integration test runner | No |

Static module composition is the default. Dynamic game modules and hot reload are deferred until ABI, global state, and object reinstancing requirements are understood. Shader and asset hot reload use worker processes and stable asset handles first.

`HertaEditorCmd` is a separate composition target, not `HertaEditor` with an invisible window. Its baseline composition does not link or initialize GLFW, ImGui, presentation RHI, Renderer, or an audio device. Commands use stable names, machine-readable diagnostics, deterministic exit codes, and cancellation. Work that explicitly requires offscreen rendering uses an RHI-enabled command composition or worker rather than changing the baseline headless contract.

Game and application projects may define additional programs that compose only selected Herta modules. Packaging those programs as a single executable is an output policy, not a separate runtime architecture.

## 6. Main loop and threading

Initial execution is intentionally understandable:

```text
Poll platform events
    -> update input state
    -> run zero or more fixed simulation steps
    -> update variable-rate world and editor state
    -> build immutable render packet
    -> execute RenderGraph
    -> submit and present
    -> retire completed frame resources
    -> apply deferred world changes
```

Headless programs have a separate composition and loop:

```text
Parse command
    -> initialize only requested services
    -> execute bounded work and report progress
    -> flush outputs and diagnostics
    -> shut down in reverse construction order
    -> return deterministic exit status
```

Do not add a separate render thread before frame ownership and profiling justify it. The eventual thread model is:

- Platform/main thread owns event pumping and native windows.
- Game thread owns mutable world state.
- Render thread owns render submission state.
- The miniaudio backend owns the operating-system audio callback thread. It performs only real-time-safe work and communicates through bounded queues.
- Worker threads run bounded jobs.
- IO workers perform async file reads and decompression.

Data crosses thread boundaries as immutable frame packets, commands, or owned jobs. Systems do not create arbitrary private threads. Long-lived threads use `std::jthread` and cooperative cancellation.

## 7. Dependency policy

Source dependencies are pinned Git submodules under `External`. Optional system integrations such as Blender are capabilities, not Herta dependencies. A normal project-generation or build command performs no network access. `Setup.bat` and `Setup.sh` are the only scripts allowed to initialize submodules and acquire non-vendored tools.

| Dependency | Decision | Owner | Purpose and boundary |
|---|---|---|---|
| [Vulkan SDK](https://vulkan.lunarg.com/) | Adopt at renderer start | Setup, NvrhiVulkan | Headers, validation, tools, and SPIR-V environment. Runtime uses the system loader. |
| [Herta GLFW fork](https://github.com/Solessfir/glfw) | Adopt at MS1 | Application | Windows, X11, Wayland, input, surfaces, and generic custom-titlebar support. Begin from commit `d6e3eee4`. Private API. |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Adopt at MS1 presentation bootstrap | NvrhiVulkan | Vulkan 1.3 device, swapchain, resource, and command implementation behind Herta RHI. MS1 uses the minimum UI path; MS2 expands the renderer-facing contract. |
| [Dear ImGui](https://github.com/ocornut/imgui) | Adopt at editor milestone | ToolUI | Docking and multi-viewport editor and tool UI. GUI programs may compose ToolUI without the game or editor runtime. |
| [LunaSVG](https://github.com/sammycage/lunasvg) | Adopt with first SVG-backed editor tool | EditorFramework | Private CPU rasterizer for static SVG editor assets. Cache RGBA results as ImGui textures; LunaSVG types never enter Herta APIs. |
| [Stack Layout PR 846](https://github.com/ocornut/imgui/pull/846) | Do not adopt | None | The initial editor does not justify an unmerged patch to ImGui internals. |
| [Herta imgui-node-editor fork](https://github.com/Solessfir/imgui-node-editor) | Adopt with first node-canvas graph | GraphEditor | Maintained third-party fork for graph visualization and interaction, not graph semantics or execution. Preserve upstream history and document Herta patches. |
| [doctest](https://github.com/doctest/doctest) | Adopt with Core | HertaTests | Unit tests beside modules, one test runner. |
| [EnTT](https://github.com/skypjack/entt) | Preferred at world milestone after spike | Scene | Private ECS storage candidate. Herta owns entity, world, query, serialization, scheduling, and mutation-barrier contracts. |
| [fastgltf](https://github.com/spnda/fastgltf) | Adopt with asset import | AssetPipeline | Offline glTF 2.0 ingestion only. |
| [Blender](https://www.blender.org/) | Optional system tool | AssetPipeline | Used only for `.blend` import and live reimport. Never downloaded by Setup or required to build or run Herta. |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Adopt at physics milestone | Physics | Collision and rigid-body simulation behind Herta types. |
| [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) | Adopt at animation milestone | Animation, AssetPipeline | Offline optimization plus runtime sampling and blending primitives. |
| [miniaudio](https://github.com/mackron/miniaudio) | Adopt at audio milestone | Audio | Private device, decoder, mixer, resampler, spatialization, and null-backend implementation behind Herta Audio. |
| [Recast Navigation](https://github.com/recastnavigation/recastnavigation) | Adopt at navigation milestone | Navigation subsystem | NavigationBuilder owns offline Recast use; runtime Navigation owns Detour queries, streaming, tile-cache updates, and DetourCrowd behind Herta APIs. |
| [Zstandard](https://github.com/facebook/zstd) | Adopt at package milestone | Assets | Default general-purpose package chunk compression behind Herta stream and package APIs. Use the BSD license option. |
| [Slang](https://github.com/shader-slang/slang) | Adopt at shader milestone | ShaderCompiler | HLSL-like source to SPIR-V plus reflection. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Add when mesh cooking exists | AssetPipeline | Mesh optimization, simplification, and later meshlets. |
| [KTX-Software/Basis Universal](https://github.com/KhronosGroup/KTX-Software) | Add when texture cooking exists | AssetPipeline | KTX2 texture cooking and runtime transcode targets. Audit per-file licenses. |
| [Tracy](https://github.com/wolfpld/tracy) | Add when frame systems exist | Core, Renderer | CPU, allocation, lock, and Vulkan profiling. Compile out in Shipping. |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Defer | NvrhiVulkan | Requires deliberate NVRHI allocation integration. Do not create two allocation authorities. |
| [volk](https://github.com/zeux/volk) | Defer | NvrhiVulkan | Add only if its dispatch model is proven compatible with the selected NVRHI/Vulkan-Hpp integration. |
| [GLM](https://github.com/g-truc/glm) | Optional private oracle | Math tests | Never a public Herta type. Prefer no dependency initially. |
| [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) | Do not adopt | None | Its runtime, XML, blackboard, and plugin model overlap Herta StateTree, GraphCore, reflection, and serialization. Use only as a design reference. |
| [HFSM2](https://github.com/andrew-gresyk/HFSM2) | Do not adopt | None | Its compile-time static structure does not fit Herta's editor-authored, cooked StateTree assets. Use only as a design reference. |
| [RVO2](https://github.com/snape/RVO2) | Defer | Navigation | Evaluate only if DetourCrowd fails measured avoidance or crowd requirements. |
| [nob.h](https://github.com/tsoding/nob.h) | Do not adopt as primary build | None | Experimental C build-recipe library. Herta would have to own project generation, dependency scanning, incremental scheduling, and IDE integration. |
| [BGFX](https://github.com/bkaradzic/bgfx) | Reject | None | Overlaps NVRHI, Vulkan ownership, shader abstraction, and renderer learning goals. |

Likely later systems, with no dependency selected yet:

- Crash reporting: start with platform crash dumps, add a service only when distribution needs it.
- Networking, localization, voice chat, and advanced acoustics: project-driven, not foundation dependencies.

Every dependency must have:

- An exact revision and documented update procedure.
- Its license and notices recorded.
- Tests, examples, and tools disabled unless Herta uses them.
- Warnings isolated from Herta warnings.
- A single owning subsystem, explicit build/runtime integration modules where needed, and no accidental public includes.
- A reason tied to a current milestone.

Fonts and icon sets have independent licenses even when embedded. Generate a `ThirdPartyNotices` manifest from dependency and content metadata. Preserve upstream copyrights in forked source and document Herta modifications separately.

## 8. Build and setup

Premake owns the workspace. CMake files inside dependencies are not Herta's top-level build system.

### 8.1 Premake versus nob.h

Keep Premake as the primary build and IDE project generator. [`nob.h`](https://github.com/tsoding/nob.h) deliberately provides something closer to shell scripts written in C and describes itself as experimental. It does not supply Herta with a mature dependency graph, C/C++ header dependency scanning, Visual Studio solution generation, compilation database generation, configuration model, or persistent build cache.

Replacing Premake with nob.h would mean starting a second engine-sized learning project: HertaBuild. That can be worthwhile later, but it does not advance the first playable game or renderer today.

nob.h may be evaluated later for a self-contained setup, packaging, or task-runner executable. It must not create two competing build descriptions. If Herta eventually replaces Premake, the migration requires explicit exit criteria:

- Correct incremental rebuilds after source, header, flags, toolchain, generated file, and submodule changes.
- Parallel scheduling with reliable failure cancellation and diagnostics.
- MSVC, Clang, and GCC support on Windows and Linux.
- Visual Studio solutions or an equally good editor/debugging workflow.
- `compile_commands.json` for clang-tidy and tooling.
- All configurations, programs, dependency boundaries, generated code, and CI use cases represented once.
- Clean-build and incremental-build parity tests against the existing build.

### 8.2 Project-local SDKs

Downloaded development SDKs and tools live under ignored `SDK/<platform>/<tool>/<version>` directories. `Config/Dependencies.lock` is the canonical binary-tool manifest consumed by Setup, Premake, and CI. It uses a strict line format so the bootstrap layer does not require Python, `jq`, a JSON library, or an already-built Herta tool.

```text
HERTA_DEPENDENCIES_V1
# name|kind|platform|version|license|url|sha256|installed-entry
premake|tool|windows-x64|5.0.0-beta8|BSD-3-Clause|https://...|<sha256>|premake5.exe
premake|tool|linux-x64|5.0.0-beta8|BSD-3-Clause|https://...|<sha256>|premake5
```

The parser accepts UTF-8, blank lines, and full-line `#` comments. Every data line has exactly eight pipe-delimited fields. Fields may not contain a pipe or newline, duplicate name/platform entries are errors, SHA-256 is mandatory, and unknown schema versions or kinds fail closed. Values are data and are never evaluated or sourced as shell code. Setup never downloads an unversioned `latest` artifact.

Setup removes each downloaded archive or installer after the installed tree passes validation. `SDK` contains usable tools and SDKs only, not a second download cache.

`Dependencies.lock` covers downloaded binary tools and SDKs only. Git submodule revisions remain locked by Git's recorded gitlinks instead of being duplicated in this file.

The Vulkan SDK can be local to the Herta checkout:

- [Windows Setup](https://vulkan.lunarg.com/doc/view/latest/windows/getting_started.html) runs the LunarG installer with `--root <repo>/SDK/Windows/Vulkan/<version>` and `copy_only=1`. This copies files without registry changes, shortcuts, administrator rights, or a system `PATH` update.
- [Linux Setup](https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html) verifies and extracts the official tarball under `SDK/Linux/Vulkan/<version>`, then supplies `VULKAN_SDK`, `PATH`, `LD_LIBRARY_PATH`, and `VK_ADD_LAYER_PATH` only to Herta build and debug child processes.
- Premake receives the resolved SDK root explicitly. It does not read a required global `VULKAN_SDK` variable.

The development SDK is local, but the Vulkan-capable GPU driver and production loader remain operating-system or driver responsibilities. Linux X11, Wayland, and compiler development packages may also require system package-manager installation.

Premake binaries and other pinned command-line tools may use the same local SDK policy. Blender is explicitly excluded because it is a user-managed system installation. `External` remains for source dependencies tracked by Git submodules. `SDK` is disposable downloaded tooling and is never committed.

### 8.3 Configurations

Configurations:

| Configuration | Intent |
|---|---|
| Debug | Full symbols, assertions, validation, low optimization |
| Development | Symbols, assertions, useful validation, optimized engine code |
| Shipping | Optimization, no editor or test code in shipped products, no validation, no runtime shader compiler |
| Debug-ASan | Dedicated supported compiler target, not assumed available everywhere |

Rules:

- Use C++23 for Herta C++ targets and the language level required by each C dependency.
- Treat Herta warnings as errors in CI. Never force that policy on third-party code.
- Centralize shared compiler, output, and platform configuration in Premake helpers.
- Use `Binaries/<platform>/x86_64/<configuration>` and matching `Intermediate/Build` paths.
- Keep generated Wayland protocol files under `Intermediate/Generated/Wayland`, not inside the GLFW submodule.
- Build GLFW's library only. Its CMake tests, examples, and docs are unnecessary because Herta uses Premake and owns integration tests.
- Do not copy an arbitrary partial Vulkan SDK into source control. Setup obtains a complete pinned development SDK locally while deployment relies on the platform Vulkan loader and GPU driver.
- Named C++ modules are deferred until Premake, MSVC, Clang, debugging, and dependency interoperability are consistently reliable.

### 8.4 Setup responsibilities

1. Validate Git, a supported compiler, and platform build tools.
2. Initialize and update pinned submodules.
3. Download a pinned Premake binary into `SDK` if missing and verify its checksum.
4. Download or validate project-local SDKs required by the selected targets. Vulkan is added only when Vulkan work begins.
5. Probe the optional system Blender installation and report its version and `.blend` integration status. Absence is not a Setup failure, and Setup never installs Blender. No separate Python check is required.
6. When selected targets need them, detect missing Linux X11, Wayland, xkbcommon, and Vulkan development packages and provide or run the appropriate supported package-manager command.
7. Print actionable diagnostics and remain safe to run repeatedly.

`GenerateProjectFiles` validates the already-installed pinned Premake executable, then invokes it for the selected generator. It performs no downloads, submodule updates, or dependency mutations. Run Setup explicitly when bootstrap state must change.

### 8.5 Cleanup

`Cleanup.bat` and `Cleanup.sh` restore a clean Herta-managed workspace without requiring Git. They remove `Binaries`, `Intermediate`, `DerivedDataCache`, `Saved`, `SDK`, `TestResults`, generated root project files, and known IDE state. The scripts validate the repository root and every destructive target before removal.

This is the generated-state equivalent of `git clean -fdx`, not an imitation of Git's tracked-file database. Arbitrary untracked files inside source directories are preserved because a Git-independent script cannot distinguish scratch work from source safely.

## 9. GitHub workflows and repository policy

GitHub Actions must call the same checked-in entry points used locally. Workflow YAML selects triggers, permissions, runners, and matrices. Build logic belongs in `Scripts/CI` or normal Herta build commands, not duplicated shell fragments hidden in YAML.

### 9.1 Workflow layout

| Workflow | Triggers | Responsibility |
|---|---|---|
| `ci.yml` | Pull request, push to `main`, `merge_group`, manual | Required builds, unit tests, platform integration, and small asset smoke tests |
| `quality.yml` | Pull request, push to `main`, nightly | Formatting, warnings, clang-tidy, generated-file checks, and sanitizers |
| `codeql.yml` | Pull request, push to `main`, weekly | C/C++ CodeQL analysis using the real build |
| `dependency-review.yml` | Pull request | Vulnerability, license, submodule, SDK-lock, and workflow-action review |
| `nightly.yml` | Scheduled and manual | Full asset corpus, optional Blender integration, render regression, stress, TSan, and recovery tests |
| `release.yml` | Protected version tag or manual | Clean reproducible packages, checksums, notices, SBOM, attestations, and draft release |

All required workflows listen for `merge_group` from the start so they remain compatible with GitHub's [merge queue](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/configuring-pull-request-merges/managing-a-merge-queue). Use workflow concurrency to cancel superseded pull-request runs while never cancelling `main`, nightly, or release work.

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}
```

Every workflow also supports `workflow_dispatch` where a manual diagnostic run is useful. Scheduled jobs record the tested engine revision and do not silently test a moving branch after checkout.

### 9.2 Required build matrix

Use explicit [GitHub-hosted runner labels](https://github.com/actions/runner-images), not `windows-latest` or `ubuntu-latest`. Runner images still receive updates, so Setup pins every tool that affects Herta's output and records the runner image version.

The initial required matrix is intentionally explicit rather than a full Cartesian product:

| Runner | Compiler | Configuration | Required coverage |
|---|---|---|---|
| `windows-2025-vs2026` | MSVC x64 | Development | Win32, unit tests, Vulkan validation smoke, interactive and headless editor startup, custom title bar |
| `windows-2025-vs2026` | MSVC x64 | Shipping | Shipping compile, cooker, package, and launch smoke |
| `ubuntu-24.04` | Clang x64 | Debug-ASan | Core tests, ASan/UBSan, X11, Wayland, null, headless editor, Vulkan validation |
| `ubuntu-24.04` | GCC x64 | Shipping | Compiler portability, Shipping compile, cooker, and launch smoke |

Set matrix `fail-fast: false` so one failure does not hide results from other platforms. Add architecture or configuration entries only when Herta supports and tests them locally. A GCC 16 C++26 reflection experiment may run as non-required CI until the production toolchain decision changes.

Linux platform coverage is explicit:

- X11 integration runs under `xvfb-run`.
- Wayland integration runs under a headless Weston compositor with a private `XDG_RUNTIME_DIR`.
- Null integration selects `GLFW_PLATFORM_NULL` without a display server.
- X11 and Wayland surface tests create a window, create a Vulkan surface, submit and present a frame, exercise custom-titlebar hit tests, resize, and close.

Use one pinned software Vulkan implementation for deterministic offscreen and golden-image tests. Do not compare reference images across arbitrary hosted GPU drivers. Record the ICD, device information, validation-layer version, shader compiler, resolution, fixed timestep, and deterministic seed with every render result.

### 9.3 Test tiers and diagnostics

Pull-request required tests grow with the engine:

- Core, Math, serialization, scene, graph, StateTree, navigation, animation, audio, physics, package, and VFS unit tests.
- Win32, X11, Wayland, and null application lifecycle tests.
- Vulkan instance, device, surface, swapchain, and offscreen-render smoke tests.
- Small shader, texture, and glTF fixtures, plus Blender-unavailable behavior.
- Interactive editor startup, project load, one frame, and clean shutdown.
- Headless editor command startup, validation, structured diagnostics, exit status, and clean shutdown without a display or audio device.
- Package compression, corrupted-chunk rejection, external and embedded mounts, deterministic mod precedence, and compatibility diagnostics.
- Generated-output verification followed by a clean-worktree check.

Nightly tests contain expensive or statistically useful coverage:

- Full asset fixture corpus and `.blend` import on a designated runner with Blender installed system-wide.
- Import determinism, live reimport idempotence, and worker crash recovery.
- ThreadSanitizer and longer concurrency stress tests.
- Golden-image render regression.
- Repeated editor startup, project reload, swapchain recreation, and shutdown.
- Longer physics, animation, memory, and GPU-lifetime stress tests.
- Large tiled-navigation, crowd, StateTree scheduling, audio-streaming, and package-decompression stress tests.

doctest emits JUnit-compatible results. CI publishes summaries and retains structured results. On failure, upload logs, crash dumps, sanitizer output, Vulkan validation output, GPU information, expected/actual/diff images, and the minimum failed asset fixture. Successful pull-request builds do not upload large binaries by default.

### 9.4 Static analysis and sanitizers

- Run Clang ASan and UBSan together in required Linux CI.
- Run TSan separately at night because it cannot be combined with ASan and has a higher cost.
- Run clang-tidy against a generated `compile_commands.json` database.
- Treat Herta warnings as errors, but suppress third-party warnings at their module boundary.
- Run MSVC `/analyze` on `main` or nightly after its runtime is measured.
- Run Vulkan validation and synchronization validation in Debug integration jobs.
- Use CodeQL advanced setup for C/C++ with an explicit manual build so generated sources and real Herta targets are analyzed. GitHub documents the choices for [compiled-language CodeQL builds](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/manage-your-configuration/codeql-for-compiled-languages).

Static-analysis baselines must be explicit and temporary. New warnings cannot be hidden by expanding a blanket suppression list.

### 9.5 Caches and artifacts

Cache only disposable accelerators:

- Pinned installed SDK and tool trees when restoring them is measurably expensive.
- Compiler object caches after measurements show a benefit.
- Shader compiler intermediates keyed by compiler and source versions.
- Derived asset data only after its content-addressing and validation are proven.

Cache keys include OS, architecture, compiler identity, configuration, `Dependencies.lock`, build scripts, shader compiler version, and submodule gitlinks. A build must succeed from an empty cache. Pull requests may restore trusted base-branch caches but do not publish replacement caches. This limits cache-poisoning risk described by GitHub's [dependency caching guidance](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching).

Artifacts are used for job outputs and diagnostics, never as an undeclared build dependency. Artifact names include the workflow, platform, compiler, configuration, and commit. Suggested retention:

- Failure diagnostics: 14 days.
- Successful `main` packages: 30 days.
- Nightly packages and diagnostics: 14 days.
- Releases: permanent GitHub Release assets, not temporary workflow artifacts.

### 9.6 Reproducibility

- Clone submodules recursively at exact gitlinks.
- Pin Premake, Vulkan SDK, Slang, and downloaded tools with SHA-256 in `Dependencies.lock`.
- Record the discovered system Blender path and exact version in `.blend` import metadata whenever that optional integration runs.
- Use explicit runner, compiler, and SDK versions.
- Enable deterministic compiler and linker options, including MSVC `/Brepro` where supported.
- Normalize archive member order, permissions, paths, and timestamps.
- Set `SOURCE_DATE_EPOCH` from the release commit.
- Embed a machine-readable build manifest containing the Git SHA, dirty state, dependency gitlinks, SDK hashes, compiler identity, and build options.
- Build releases in empty directories without restored object or DDC caches.
- Build each release package twice in the same pinned environment and compare hashes before publishing.

### 9.7 Workflow security

Every workflow begins read-only:

```yaml
permissions:
  contents: read
```

Raise permissions only on the individual job that needs them. GitHub recommends [least-privilege `GITHUB_TOKEN` permissions](https://docs.github.com/en/actions/security-for-github-actions/security-guides/automatic-token-authentication).

Security rules:

- Pin every third-party action to a full 40-character commit SHA, with its release tag in a comment for readability.
- Configure Dependabot for `github-actions` and `gitsubmodule` ecosystems to propose reviewed updates.
- Set `persist-credentials: false` on checkout unless a job has a documented need to push.
- Allow GitHub-owned actions plus an explicit audited third-party allowlist.
- Never interpolate branch names, commit messages, issue text, or pull-request titles directly into a shell program. Pass untrusted values through environment variables or action inputs.
- Never place credentials in caches, artifacts, command lines, generated projects, or logs.
- Keep publishing permissions in a protected GitHub environment.
- Use GitHub-hosted ephemeral runners for untrusted changes. Never execute fork pull requests on persistent self-hosted or GPU runners.

GitHub identifies a full commit SHA as the immutable action reference in its [secure-use guidance](https://docs.github.com/en/actions/reference/security/secure-use). Dependabot keeps those pins reviewable rather than replacing them with mutable tags.

### 9.8 Fork pull requests

Build untrusted forks only through `pull_request` with a read-only token, no secrets, no publishing, no cache writes, and no privileged runners.

Do not use `pull_request_target` to compile, test, import, or otherwise execute pull-request code. It may run with the trusted base repository's context. Reserve it for non-executing metadata tasks such as labeling, and never check out fork-controlled content in that workflow.

Require maintainer approval before the first workflow run from a new external contributor. Changes to `.github/workflows`, Setup, Premake, `Dependencies.lock`, `.gitmodules`, and importer scripts receive additional review because they execute code or change the supply chain.

### 9.9 Dependency and source security

Enable GitHub's dependency graph, Dependabot alerts, security updates, secret scanning, and push protection where the repository plan permits them. Make Dependency Review required and fail newly introduced vulnerabilities at an agreed severity. GitHub supports both [`github-actions` and `gitsubmodule` Dependabot ecosystems](https://docs.github.com/en/code-security/reference/supply-chain-security/supported-ecosystems-and-repositories).

GitHub metadata does not replace Herta's dependency controls. `Dependencies.lock` records downloaded binary tools and SDKs. Source dependencies are pinned by submodule gitlinks, with licenses, Herta patches, owning modules, and update notes recorded beside the dependency policy. CI verifies the binary lock against downloaded SDK contents and verifies that submodules match their committed gitlinks.

### 9.10 Branch and release policy

After repository bootstrap, protect `main` with a GitHub ruleset:

- Require pull requests and the stable aggregate check `CI / required`.
- Require conversation resolution and block force pushes and deletion.
- Require CodeQL and Dependency Review once they contain meaningful coverage.
- Require CODEOWNERS review for workflows, build/setup code, dependencies, importers, platform code, and release policy when a second maintainer exists.
- Add an approval requirement when it does not make a single-maintainer repository impossible to operate.
- Prefer linear history and signed commits if they fit the contributor workflow.
- Do not grant routine administrator bypasses.

Use one always-created aggregate gate job as the required CI check. Do not require individual matrix job names and do not apply top-level path filters to required workflows, because renamed or skipped checks can block or accidentally weaken merging.

Enable merge queue when Herta receives enough concurrent pull requests to benefit. The workflows already support its `merge_group` event.

Protect `v*` tags from update and deletion. A release job:

1. Verifies the tag points to a tested `main` commit.
2. Builds clean Windows and Linux Shipping packages.
3. Repeats the build and compares package hashes.
4. Generates SHA-256 checksums, third-party notices, build manifest, and SPDX SBOM.
5. Generates GitHub [artifact attestations](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations) for packages and the SBOM.
6. Uploads everything to a draft release.
7. Publishes only after all platforms and provenance steps succeed.

Only the release-attestation job receives `id-token: write` and `attestations: write`. Enable immutable releases when available so published tags and assets cannot be silently replaced.

## 10. Testing and diagnostics

Use doctest. Keep tests next to the module and link them into `HertaTests`.

Initial required suites:

- Math conventions and third-party conversion round trips.
- Stable IDs, generational handles, hashing, and path normalization.
- Error propagation and staged initialization teardown.
- Serialization versioning and corrupted-input rejection.
- RenderGraph dependency, lifetime, and barrier planning.
- Asset build-key determinism and atomic import behavior.
- Graph validation and compiler IR.
- ECS structural barriers, query access declarations, component relocation, and deterministic system ordering.
- StateTree compilation, direct transitions, bounded jump loops, instance-data layout, tracing, and cooked-data rejection.
- Navmesh cook determinism, path queries, tile streaming, cancellation, and coordinate adapters.
- Audio null-backend behavior, command-queue limits, asset streaming, and coordinate adapters.
- Package index validation, zstd bounds, VFS mount precedence, embedded-package discovery, and mod compatibility.
- Fixed-step accumulation and physics-event buffering.

Add slower categories as their systems arrive:

- Golden glTF fixtures, Blender-unavailable tests, and conditional `.blend` fixtures on machines with Blender installed.
- Headless Vulkan/NVRHI smoke tests where CI provides a supported driver.
- Swapchain resize, minimize, restore, and device-loss tests.
- Windows custom-titlebar hit-test tests.
- Linux X11 and Wayland build and smoke coverage.

Diagnostics are foundation work, not polish:

- Structured logs with category, level, thread, and timestamp.
- Checked Vulkan results and staged RAII initialization.
- Debug names on every GPU object.
- Frame serials for GPU lifetime logging.
- Crash dumps and the active build/version ID.
- Tracy markers when real frame workloads exist.

When diagnosing engine failures, add logs, validation, captures, and assertions before changing behavior. Use RenderDoc or Nsight for GPU ground truth and sanitizers or debugger watchpoints for CPU memory issues.

## 11. Security and robustness

- Treat imported assets as untrusted input.
- Treat packages and mod manifests as untrusted input, including paths, dependency graphs, sizes, hashes, and compression metadata.
- Run complex importers in worker processes with timeouts and cancellation.
- Disable embedded Blender script auto-execution.
- Validate sizes, counts, offsets, recursion depth, and decompression limits before allocation.
- Keep native binary mods disabled until Herta defines and deliberately enables a versioned execution boundary.
- Use atomic output replacement so failed cooks do not destroy valid data.
- Check every Vulkan and OS result that can fail.
- Make partial initialization safe to destroy.
- Keep editor recovery state under `Saved`, separate from source assets.

## 12. Milestones

### Milestone 0 - Foundation

- Replace the preserved GLFW-only Lua recipe with the Herta Premake workspace.
- Add `Core`, `Math`, `Platform`, `HertaTests`, and doctest.
- Add setup and project-generation scripts for Windows and Linux.
- Add `Dependencies.lock`, its strict parsers and tests, project-local tool download support, formatting, warnings, Git attributes, and ignore rules.
- Add the required Windows/Linux CI gate, quality checks, CodeQL, Dependabot, and the initial `main` ruleset.
- Implement and test the fixed coordinate and math conventions.

Exit condition: a fresh clone can run Setup, generate, build, and execute tests on Windows and Linux.

### Milestone 1 - Application shell

- Add the pinned GLFW fork as a submodule.
- Create windows, event pumping, input state, and capability reporting.
- Integrate native-titlebar-disabled custom title bars on Win32, X11, and Wayland while retaining native resize and window-manager behavior.
- Add the pinned NVRHI dependency, create the Vulkan instance, device, surface, and swapchain, and expose the minimum Herta RHI presentation path required by ToolUI. Do not add a scene renderer.
- Add ToolUI and the Dear ImGui docking and multi-viewport editor shell.
- Add the `EditorCore`, `HertaEditor`, and `HertaEditorCmd` composition boundaries. The headless target starts and stops without GLFW, ImGui, presentation RHI, Renderer, or an audio device.
- Draw title-bar controls with ImGui primitives. Defer LunaSVG and the production icon set until an SVG-backed editor tool needs them.

Exit condition: the interactive editor opens, docks, creates platform viewports, resizes correctly, and closes cleanly on both platforms. The headless editor host starts, reports structured diagnostics, and exits cleanly on both platforms without a display server.

### Milestone 2 - RHI and renderer

- Expand the private NVRHI Vulkan backend and Herta RHI handles and descriptors beyond the MS1 presentation subset.
- Add RenderGraph, frame contexts, upload staging, and fence-based retirement.
- Add Slang worker compilation and cooked shader assets.
- Render a validation-clean textured mesh with reversed-Z.

Exit condition: render tests and resize/minimize stress runs produce no validation errors or leaked GPU objects.

### Milestone 3 - Asset pipeline

- Add stable asset IDs, registry, build keys, and DerivedDataCache.
- Add fastgltf, mesh cooking, and texture cooking foundations.
- Add optional isolated `.blend` import through a discovered system Blender installation.
- Add file watching, background Blender live reimport, and atomic editor asset generation swaps.
- Expose validation, import, and reimport commands through `HertaEditorCmd` without initializing interactive editor systems.

Exit condition: Herta operates normally without Blender. When Blender is installed, saving a tracked `.blend` updates dependent editor instances without blocking the UI, the same source, Blender version, and settings produce identical cooked hashes, and failed reimport preserves the previous asset.

### Milestone 4 - World and editor authoring

- Add the Herta ECS contracts, entities, components, hierarchy, scene save/load, and version migration.
- Run the EnTT storage and scheduling spike, record results, and either adopt its pinned revision privately or document why another implementation is required.
- Add deferred structural barriers, explicit query read/write access, buffered events, and deterministic system ordering.
- Add explicit Herta runtime descriptors needed by inspectors and serialization, without requiring C++26 reflection.
- Add transactions, property editing, selection, gizmos, and play-in-editor lifecycle.

Exit condition: scenes round-trip, undo/redo is reliable, ECS mutation and query rules pass focused and scale tests, and runtime remains independent of editor modules.

### Milestone 5 - Physics

- Add Jolt adapters, collision layers, shapes, bodies, fixed-step simulation, and debug draw.
- Add origin-relative physics transforms and buffered events.

Exit condition: physics tests are repeatable for the supported configuration and world mutation never occurs inside callbacks.

### Milestone 6 - Navigation and AI

- Add Recast offline tiled-navmesh cooking, Detour runtime queries and tile streaming, and initial DetourCrowd integration.
- Add asynchronous path requests, cancellation, dynamic-obstacle handling, debug draw, and deterministic fixtures.
- Add AI perception, working memory, scheduling, gameplay-task contracts, and ECS integration.
- Add GraphCore, StateTreeCompiler, and a nested StateTree authoring view for the first narrow graph domain without requiring a free-form node canvas.
- Add the first-party contiguous StateTree runtime program with direct index jumps, pooled instance data, utility selection, tracing, and bounded transitions.

Exit condition: agents navigate a streamed test level, avoidance behaves consistently, and authored StateTrees compile and produce matching headless execution traces after reload.

### Milestone 7 - Animation

- Add skeleton and clip import through ozz offline tools.
- Add sampling, blending, skinning, root motion, and animation events.
- Add an initial Herta-owned animation state machine using explicit cooked data. It is not the NPC StateTree runtime and does not become a GraphCore authoring domain yet.

Exit condition: a cooked skinned asset animates identically after reload and has no runtime dependency on offline ozz tools.

### Milestone 8 - Audio

- Add miniaudio behind Herta Audio with real and null device paths.
- Add cooked sound assets, voices, buses, streaming, spatial playback, concurrency policy, and real-time-safe command queues.
- Add audio diagnostics and deterministic headless tests without requiring an audio device.

Exit condition: static and streaming sounds play spatially on Windows and Linux, the callback remains real-time safe under stress, and headless gameplay produces the same audio commands through the null path.

### Milestone 9 - Graph domains

- Add the pinned Herta imgui-node-editor fork behind GraphEditor for the first domain that requires a free-form node canvas.
- Promote the animation state machine into a typed animation-graph domain and compiler.
- Extract only the graph authoring, typed-pin, transaction, validation, diagnostic, and migration infrastructure proven by StateTree and the animation graph.
- Keep Shader Graph, Audio Graph, and gameplay Blueprints as later distinct compiler targets rather than one universal executor.
- Extend debugging and cooked-format migration without linking editor UI into runtime executors.

Exit condition: StateTree and the animation graph compile headless and execute from cooked data without ImGui or imgui-node-editor linked, while sharing only proven GraphCore infrastructure.

### Milestone 10 - Cooking and distribution

- Add HertaCooker, versioned package indexes, dependency closure, zstd chunk compression, and platform deployment.
- Add loose, external-package, and executable-embedded output modes through the same VFS and package reader.
- Add deterministic data-mod discovery, manifests, dependency resolution, namespaces, override policy, and compatibility diagnostics.
- Add third-party notices, crash build IDs, and reproducible Shipping configuration.
- Add protected release tags, clean double-build verification, SBOM generation, artifact attestations, and immutable GitHub releases where available.

Exit condition: HertaGame runs from external or embedded cooked data without editor or developer modules, packages reproduce byte-for-byte, and compatible data mods mount deterministically without changing the base package.

## 13. Definition of done for an engine module

A module is not complete because its happy path works. It is complete when:

- Public and private boundaries are explicit.
- Dependency direction is acyclic.
- Ownership and shutdown order are unambiguous.
- Important logic has unit tests.
- Failures return useful context and preserve valid state.
- Debug names, logging, and profiling hooks exist where relevant.
- Platform behavior is tested or documented.
- Generated data is reproducible and versioned.
- The build and setup documentation remains correct.
- Required GitHub workflows exercise the module on every supported platform and preserve useful failure diagnostics.
- No dependency is exposed beyond its owning module without a deliberate reason.

## 14. Immediate next implementation slice

Do not add the full dependency table at once. The first code slice should contain only:

1. The real Herta Premake workspace.
2. Core, Math, and HertaTests.
3. doctest.
4. `Dependencies.lock`, its parser tests, project-local Setup, and project-generation scripts.
5. Coordinate-system, unit-convention, and world-to-origin-relative transform tests.
6. Required Windows/Linux CI plus quality and dependency checks.
7. The GLFW submodule only when Application work begins.

That slice establishes the rules every later system relies on while keeping build and debugging surface small.
