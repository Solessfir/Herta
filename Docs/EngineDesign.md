# Herta Engine Design

Status: Active design - Milestone 2 complete
Last updated: 2026-09-22

## 1. Purpose

Herta is a C++23 game engine and editor for Windows and Linux. It is a learning engine with the long-term goal of becoming a production-ready engine capable of shipping real games. Learning determines the order in which systems are built, not the quality bar applied to them.

The design takes Unreal's strict runtime, editor, developer, and program boundaries, then starts with Godot-sized modules. It does not reproduce Unreal Engine's accumulated size before Herta needs those features. A module is split only when ownership, reuse, build time, or platform isolation justifies it.

The priorities are:

1. Correct lifetime and dependency boundaries.
2. A responsive editor and a fast edit, build, import, and debug loop.
3. A renderer that teaches Vulkan without leaking Vulkan through the engine.
4. Deterministic asset builds and versioned serialized data.
5. A capable editor built as a client of the runtime.
6. Dependencies added only when their milestone needs them.
7. Production concerns such as recovery, profiling, security, compatibility, packaging, and migration designed in before release pressure arrives.

Editor responsiveness is an architectural requirement. Work is not parallelized merely because threads exist, but interactive threads must not wait on import, cooking, shader compilation, source control, search indexing, package construction, external tools, or other latency-bound work that can run asynchronously.

Herta does not claim production readiness during its foundation milestones. Production readiness is earned through shipped projects, measured performance, stable data formats, upgrade paths, broad hardware testing, and reliable tooling.

Herta is not required to become:

- A feature-for-feature copy of Unreal Engine.
- A multi-backend renderer.
- A stable third-party binary plugin ABI across engine versions.
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

Canonical mesh triangles are counter-clockwise when viewed from their front side. Importing through a transform with a negative determinant reverses triangle indices once while producing canonical Herta data. Renderer backends preserve the counter-clockwise semantic explicitly; Vulkan viewport-Y handling must not leak into asset winding or cause importers to flip geometry conditionally.

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

Prefer `constexpr` for pure operations and values that naturally support compile-time use. Use `consteval` when compile-time evaluation is a semantic requirement and a runtime call must be rejected. Do not force expensive work into constant evaluation when it only increases build cost or worsens diagnostics.

Comments explain constraints, engine quirks, or why a decision is non-obvious. They do not narrate the code. Public headers remain self-contained and include only what their declarations require.

### 2.6 Product architecture policies

- Editor responsiveness is a release criterion. Latency-heavy work is asynchronous, cancellable, observable, and publishes results atomically.
- Herta has no lightmap authoring or baking pipeline. Dynamic lighting provides capability-based fallbacks, and full GI is not required on every supported GPU.
- Multiplayer is server-authoritative. Herta supports dedicated servers and player-hosted listen servers through the same replication and permission model.
- Herta-authored source content is deterministic mergeable UTF-8 text where practical. External media and cooked runtime data remain binary.
- Networking, localization, scripting, editor tooling, and other optional systems remain composable modules instead of mandatory dependencies for every application.

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
|   |-- EditorStyle.md
|   `-- EngineDesign.md
|-- Engine/
|   |-- Content/
|   |   `-- Editor/
|   |       |-- Fonts/
|   |       `-- Icons/
|   |-- Plugins/
|   |   `-- Editor/
|   `-- Source/
|       |-- Runtime/
|       |   |-- Core/
|       |   |-- Tasks/
|       |   |-- Math/
|       |   |-- Platform/
|       |   |-- Application/
|       |   |-- Reflection/
|       |   |-- Assets/
|       |   |-- Scene/
|       |   |-- GraphCore/
|       |   |-- Scripting/
|       |   |-- Localization/
|       |   |-- TextLayout/
|       |   |-- Networking/
|       |   |-- Replication/
|       |   |-- ToolUI/
|       |   |-- Audio/
|       |   |-- SteamAudio/
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
|       |   |-- LocalizationEditor/
|       |   `-- GraphEditor/
|       |-- Developer/
|       |   |-- AssetPipeline/
|       |   |-- LocalizationPipeline/
|       |   |-- NavigationBuilder/
|       |   |-- PackageBuilder/
|       |   |-- StateTreeCompiler/
|       |   `-- ShaderCompiler/
|       `-- Programs/
|           |-- HertaEditor/
|           |-- HertaEditorCmd/
|           |-- HertaGame/
|           |-- HertaServer/
|           |-- HertaEditorMcp/
|           |-- HertaAssetWorker/
|           |-- HertaShaderWorker/
|           |-- HertaCooker/
|           `-- HertaTests/
|-- External/
|   |-- Premake/                    # ignored host binaries installed by Setup
|   |-- doctest/
|   |-- enkiTS/
|   |-- entt/
|   |-- fastgltf/
|   |-- glfw/
|   |-- imgui/
|   |-- imgui-node-editor/
|   |-- icu/
|   |-- harfbuzz/
|   |-- freetype/
|   |-- jolt/
|   |-- lunasvg/
|   |-- miniaudio/
|   |-- nvrhi/
|   |-- ozz-animation/
|   |-- recastnavigation/
|   |-- spdlog/
|   |-- steam-audio/
|   |-- GameNetworkingSockets/
|   |-- libsodium/
|   |-- umka-lang/
|   `-- zstd/
|-- Games/
|   `-- Sandbox/
|       |-- Config/
|       |-- Content/
|       |-- Plugins/
|       `-- Source/
|-- Scripts/
|-- Templates/
|   `-- Projects/
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
External/Premake/<platform>/<version>/
SDK/<platform>/<sdk>/<version>/
Saved/Logs/
Saved/HotReload/
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
|-- Tasks -> Core + Platform
|-- Math -> Core
|-- Platform -> Core
|-- Reflection -> Core
|-- Assets -> Core + Reflection
|-- Application -> Core + Math + Platform
|-- Scene -> Core + Math + Reflection + Assets
|-- GraphCore -> Core + Reflection + Assets
|-- Scripting -> Core + Reflection + Assets
|-- Localization -> Core + Assets
|-- TextLayout -> Core + Math + Assets + Localization
|-- Networking -> Core + Platform + Tasks
|-- Replication -> Networking + Math + Reflection + Scene
|-- ToolUI -> Core + Application + RHI + TextLayout
|-- RHI -> Core + Math
|   `-- NvrhiVulkan -> RHI + Platform
|-- RenderGraph -> Core + RHI
|-- Renderer -> Core + Math + Assets + RHI + RenderGraph
|-- Physics -> Core + Math
|-- Animation -> Core + Math + Assets
|-- Audio -> Core + Math + Assets
|-- SteamAudio -> Core + Math + Tasks + Audio + Scene
|-- Navigation -> Core + Math + Assets
`-- AI -> Core + Math + Assets + Scene + GraphCore + Physics + Navigation

Full game composition -> Application + Scene + Renderer + Physics + Animation + Audio + Navigation + AI + optional SteamAudio + optional Localization + optional Scripting + optional (Networking + Replication)
Dedicated server composition -> Scene + Physics + Networking + Replication + selected gameplay modules
EditorCore -> Core + Platform, then adds Reflection + Assets + Scene as those milestones arrive
EditorFramework -> EditorCore + ToolUI
AssetEditor -> EditorFramework + AssetPipeline
LocalizationEditor -> EditorFramework + LocalizationPipeline
GraphEditor -> EditorFramework + GraphCore
LocalizationPipeline -> Core + Assets + Localization
NavigationBuilder -> Core + Math + Assets + AssetPipeline
PackageBuilder -> Core + Assets
StateTreeCompiler -> Core + Reflection + Assets + GraphCore + AI
HertaEditor -> EditorFramework + runtime modules available in the current milestone
HertaEditorCmd -> EditorCore + selected Runtime and Developer command modules
HertaEditorMcp -> EditorCore automation through authenticated local IPC
HertaAssetWorker -> AssetPipeline
HertaCooker -> AssetPipeline + PackageBuilder
First-party editor plugins -> public editor extension and automation contracts
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

Core defines the stable `FTextId` namespace-and-key value used by descriptors and serialized data, but it does not resolve cultures or load catalogs. Those behaviors belong to Localization so Core and headless applications remain independent of ICU.

Herta uses `spdlog` as a compiled private backend when Milestone 1 introduces application and headless-editor diagnostics. It provides console, debugger, rotating-file, and editor-buffer sink plumbing. Herta owns log categories, record fields, source locations, filtering, lifecycle, failure policy, and public macros. No `spdlog` or `fmt` type appears in a Herta public header or crosses a game-module boundary.

Public formatting uses `std::format_string` and checks category and level before formatting. Begin with synchronous dispatch for deterministic startup, shutdown, tests, and crash diagnostics. Add bounded asynchronous dispatch only after profiling demonstrates a need. An asynchronous policy may drop low-level records with an explicit counter, but warnings and errors must not be silently discarded. Normal logs go to the console or debugger, rotating files under `Saved/Logs`, and a bounded editor-record buffer when an editor composition requests that sink. Audio callbacks and other real-time threads never use the normal logger.

Each editor record carries a monotonically increasing sequence, elapsed timestamp, category, level, thread identity, UTF-8 message, and optional source location. The bounded buffer exposes cursor-based incremental reads that copy records into caller-owned snapshots, so ToolUI never holds the producer lock while measuring, filtering, selecting, or drawing text. Reads explicitly report generation reset and history truncation. Clearing advances the generation and invalidates old cursors; capacity eviction reports lost records instead of silently presenting an apparently complete history. The buffer never exposes pointers or views into mutable sink storage.

### 4.2 Tasks

`Tasks` is a Herta-owned contract introduced before the first latency-heavy editor workflows. Its implementation provides bounded CPU-worker and blocking-IO lanes plus main-thread continuations. The public API includes owned task scopes, handles, cancellation, priorities, continuations, `WhenAll`, `ParallelFor`, progress reporting, profiling names, and deterministic test execution.

- Interactive threads never wait for asset import, cooking, shader compilation, source-control queries, thumbnails, fuzzy indexing, package construction, or external processes.
- Blocking file and process IO cannot consume every CPU worker.
- Game-facing tasks belong to an `FTaskScope` tied to a world, subsystem, plugin, or game-module generation. Unowned fire-and-forget tasks are discouraged.
- Reloadable modules stop dispatch, cancel or join owned tasks, and remove main-thread continuations before their code is unloaded.
- Worker waits help execute runnable work. Main-thread waits produce diagnostics after a small threshold and are forbidden in normal interactive workflows.
- Long-lived service loops use `std::jthread` and cooperative cancellation instead of occupying worker tasks indefinitely.

The task system does not grant arbitrary concurrent access to engine state. Mutable world state remains phase-owned, and parallel systems publish buffered results at explicit barriers. Do not create one private thread pool per subsystem or build a general automatic task-graph framework before measured workloads require it.

[enkiTS](https://github.com/dougbinks/enkiTS) is the preferred private implementation candidate after a focused spike. Herta retains ownership of cancellation, scopes, reload quiescence, diagnostics, and the game-facing API regardless of backend choice.

### 4.3 Platform

Platform wraps OS primitives through compile-time selected implementations:

- Files, directories, memory mapping, and file watching.
- Processes, pipes, dynamic libraries, and environment access.
- Threads, synchronization, high-resolution clocks, and CPU information.
- Crash context and debugger detection.

Windows and Linux headers stay in `Private/Windows` and `Private/Linux`. Runtime interfaces are appropriate for replaceable services such as rendering, physics, audio, and editor hosts. Virtual dispatch is unnecessary for basic OS calls selected at compile time.

### 4.4 Application and custom title bar

Application owns windows, displays, input devices, cursors, clipboard, and event pumping. GLFW is private to this module.

The custom title bar design is:

1. Create a GLFW window with `GLFW_TITLEBAR` disabled while preserving the native resize frame and platform behavior.
2. Render the title bar and window controls in the editor UI.
3. Register Herta's generic GLFW hit-test callback for draggable and resize regions.
4. Let the GLFW platform backends translate hit-test results into Win32, X11, and Wayland behavior.
5. Route minimize, maximize, restore, close, double-click, and system menu actions through `FWindow`.

Editor code must not handle `WM_NCHITTEST`, X11 client messages, or Wayland serials. Those details belong in the GLFW fork. Each ImGui platform viewport installs the same callback because every viewport is a native window.

Wayland does not expose every global-window operation available on Win32/X11. Window placement and custom-decoration behavior must degrade honestly instead of faking unsupported state. Application caches the per-window action capabilities advertised through GLFW and combines them with Herta's per-window action policy. ToolUI disables every native window-action control across platforms. The application glyph remains visible as decorative caption content. This policy does not detect a compositor or layout.

The GLFW fork should contain only a generic, upstreamable custom-titlebar API. Herta-specific colors, buttons, ImGui state, and engine events stay in Herta. The fork remains pinned as a submodule and its upstream copyright remains intact.

The Milestone 1 title-bar geometry is one pure Herta layout contract shared by rendering, hover feedback, and native hit testing. The baseline uses a 36 logical-pixel title bar, 46 logical-pixel window buttons, and a 6 logical-pixel resize border. Win32 and X11 apply viewport content scale. Wayland coordinates are already logical and are not scaled twice. Unit tests cover every region and fractional-scale rounding.

When another application consumer enables window controls, those pixels are not ImGui buttons. Returning a minimize, maximize, restore, close, or system-menu hit-test role lets the native GLFW backend own the click and window-manager action. Rendering code must not perform the same action again.

ImGui capture has priority over caption dragging. Each native viewport caches the most recent ToolUI mouse-capture state outside the GLFW callback. If a floating panel, popup, modal, or active item overlaps the title bar, the callback returns client space so ImGui receives the interaction. The callback is allocation-free and never calls ImGui directly. Multi-viewport integration therefore owns one `FWindow`, title-bar layout, scale, focus state, cursor state, and capture state per ImGui platform viewport.

Windows GUI programs compile a multi-resolution application icon as the named `GLFW_ICON` resource through Premake. Linux desktop packaging installs the corresponding SVG or PNG sizes with the program's `.desktop` entry. Application exposes runtime icon capability without pretending that ELF executables embed desktop icons.

On first launch, the main editor window uses 80 percent of the primary monitor work area and is centered where the platform permits. Later launches restore validated per-user placement without allowing a monitor-layout change to leave the title bar unreachable. Wayland placement remains compositor-owned.

Minimized windows wait for events and do not render continuously. Native move and resize may enter a platform modal loop, so window refresh callbacks render through the same guarded frame path used by the main loop. That path checks renderer readiness, rejects recursive entry, queries current framebuffer dimensions, and recreates the swapchain safely. This prevents exposed or newly enlarged regions from remaining black until the user releases the pointer.

### 4.5 Reflection and serialization

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

### 4.6 Scene and world

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

### 4.7 RHI, RenderGraph, and Renderer

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
4. PBR mesh rendering, image-based lighting, direct lights, shadows, motion vectors, depth hierarchy, exposure, and tone mapping.
5. GTAO and a crisp native-resolution TAA path with correct history invalidation.
6. GPU-driven culling, indirect draws, meshlets, and async work after profiling.
7. Optional temporal upscalers, screen-space GI, hardware ray tracing, and experimental dynamic-GI backends after their prerequisites and quality gates exist.

Herta has no lightmap authoring, baking, or lightmap-UV cooking pipeline. Dynamic lighting must still degrade by capability instead of requiring full GI on every supported GPU:

- Low uses direct lighting, image-based lighting, shadow maps, and GTAO.
- Medium adds screen-space GI when enabled.
- High may add a validated Radiance Cascades or hardware ray-traced GI backend.
- Headless and unsupported rendering paths retain deterministic unlit or direct-light fallbacks.

Radiance Cascades is a research candidate, not Herta's sole shipping GI foundation. Its acceptance spike must cover free-camera indoor and outdoor scenes, off-screen emitters and occluders, thin walls, foliage, skinned and moving geometry, large worlds, camera cuts, and multiple editor views. Results are compared against path-traced references with recorded GPU time, memory, instability, light leaks, and scene-update cost. Until those gates pass, it remains an internal experimental method.

Hardware ray tracing is capability-driven and optional. Add Vulkan KHR acceleration structures, ray queries, and ray-tracing pipelines only after the raster path, scene extraction, motion vectors, RenderGraph lifetime rules, and GPU profiling are stable. Raster rendering remains a complete supported path.

Native TAA is Herta's vendor-neutral temporal baseline, not permission to ship a soft image. The default quality contract requires:

- Motion vectors for rigid, skinned, procedurally displaced, and camera motion.
- Pre-tonemap HDR accumulation with jitter-aware reprojection.
- Depth, normal, velocity, and disocclusion-based history rejection.
- Neighborhood or variance clipping to prevent stale history from bleeding across edges.
- Reactive and transparency masks for emissive, translucent, particle, and rapidly changing content.
- Motion-adaptive history weighting, deterministic camera-cut resets, and stable low-discrepancy jitter.
- High-quality reconstruction and a modest configurable post-sharpen. Sharpening must not hide ghosting or incorrect rejection.

Game and editor UI is composited after TAA and temporal upscaling. Render regression scenes cover still detail, motion, foliage, thin geometry, specular highlights, particles, camera cuts, and disocclusion. `Off` and a lightweight non-temporal fallback remain available for accessibility, debugging, and content that deliberately rejects temporal accumulation. SMAA is deferred until a project demonstrates a quality need beyond that fallback.

The renderer owns one temporal-upscaler input contract containing color, depth, motion vectors, exposure, jitter, reactive and transparency masks, reset state, and input/output resolutions. FSR is the first optional Vulkan adapter. DLSS and XeSS may follow as vendor plugins without becoming renderer foundations. Frame generation is deferred until frame pacing, latency markers, UI separation, and swapchain integration are mature, and it is not enabled in the editor.

Atmosphere and volumetric fog precede volumetric clouds. Clouds use reduced-resolution ray marching, temporal reconstruction, cloud shadows, and measured async compute rather than introducing a renderer dependency prematurely.

Debug and Development enable Vulkan validation, object names, RenderDoc markers, and NVRHI validation. Shipping disables validation and runtime shader compilation.

### 4.8 Shader pipeline

Slang is the preferred shader compiler when the first real renderer shader pipeline is built. It provides HLSL-like source, SPIR-V output, modular compilation, and reflection on Windows and Linux.

`HertaShaderWorker` owns compilation. Its output is a cooked shader asset containing:

- SPIR-V bytecode per entry point and stage.
- Reflected bindings and push-constant layout.
- Permutation key and compiler version.
- Source dependency hashes.
- Debug symbols for non-Shipping builds.

Runtime code consumes cooked bytecode and metadata. Editor hot reload starts the worker process and atomically swaps a successful result. A failed compile keeps the last valid shader.

Pipeline caches are keyed by shader identities, attachment formats, vertex layout, and fixed-function state. Cache files include device and driver identity and may always be discarded.

### 4.9 Assets and derived data

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

Herta-authored source assets such as scenes, prefabs, materials, graphs, descriptors, import metadata, and configuration use deterministic versioned UTF-8 text. Files have stable IDs, canonical field ordering, locale-independent floating-point formatting, no timestamps or absolute machine paths, and atomic saves. Large worlds are partitionable so unrelated edits do not rewrite one monolithic scene file. Imported models, textures, audio, fonts, and `.blend` sources remain binary; they are never base64-encoded merely to make the container textual. Cooked data and DerivedDataCache entries remain binary and do not enter source control.

Application reports dropped paths without interpreting asset formats. `AssetEditor` validates and copies external sources into project content by default, then invokes the same asynchronous AssetPipeline command used by `HertaEditorCmd`. Batch imports support cancellation, conflict resolution, directory and symlink limits, progress, and atomic publication.

Fast asset search consumes immutable AssetRegistry snapshots. Names, normalized paths, types, tags, and stable IDs are indexed incrementally off the UI thread. Stale query generations are cancelled, result counts are bounded, and equal scores use deterministic path and ID tie-breaking. Start with a small Herta-owned fuzzy scorer and add a dependency only if measurement justifies it.

`fastgltf` handles editor/offline glTF 2.0 ingestion. Imported data is converted immediately to Herta coordinates, types, naming, and canonical vertex formats. glTF library types do not enter runtime modules.

Meshoptimizer is added when real mesh cooking exists. KTX2/Basis Universal is added when the texture cooker exists. Neither belongs in bootstrap code.

### 4.10 Native `.blend` import

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

### 4.11 Physics

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

### 4.12 Animation

ozz-animation is private to Animation and AssetPipeline.

- Offline ozz builders, optimizers, and serializers run only in the asset worker.
- Runtime ozz sampling and local-to-model jobs operate on immutable cooked data.
- Herta owns animation state machines, blend trees, events, root motion, retargeting policy, and graph assets.
- Skinning output is converted to Herta renderer buffers at a narrow bridge.
- ozz types are not serialized as Herta public scene types.

Sampling and blending are natural job-system workloads, but a custom task graph should not be built before this parallel work exists.

### 4.13 Editor and graph system

The interactive editor hosts the runtime. Runtime code never includes ImGui or editor headers. `EditorCore` owns project loading, transactions, validation, automation contracts, and other editor services that must also run headless. It does not depend on Application, ImGui, RHI, Renderer, or an audio device.

Dear ImGui is used for editor, tool, and optional GUI-application interfaces, not Herta game UI. `ToolUI` owns context lifetime and platform/render integration so a GUI program can use ImGui without linking Scene, Physics, Animation, AI, or EditorCore. Use the maintained docking branch with multi-viewports, pinned to an exact commit or release tag.

Milestone 1 adopts the centralized [Herta Editor Style](EditorStyle.md) proven by the native ProjectTemplate: Roboto at a 15 logical-pixel base, desaturated non-black surfaces, one configurable low-intensity background gradient, additive interaction tinting, white docking previews, transparent-panel modes, and a 36 logical-pixel custom title bar. ToolUI owns named palette, metric, rounding, interaction, and panel-presentation tokens. Feature panels do not scatter local ImGui style literals. Transparency ships in the initial shell; backdrop blur remains a later RenderGraph-backed option.

The title bar, application toolbar, and dock canvas form one continuous visual workspace. ToolUI draws one full-viewport gradient first, then uses transparent or low-alpha chrome instead of unrelated solid bands and separator lines. Cobalt is the initial hue at 15 percent intensity and 50 percent height. Appearance settings expose presets, a custom HSV color, saturation, intensity, gradient height, and panel transparency under `Saved/Editor`. Intensity remains a true interpolation factor so 100 percent reaches the chosen color.

The default dock layout is created only when no compatible saved layout exists. Saved user docking and intentional floating windows take precedence after first launch. Layout format changes are versioned instead of silently rebuilding the default every run.

`EditorFramework` owns the Output Log panel while Core owns records, sinks, and bounded storage. Core never includes ImGui, and ToolUI never writes directly to `spdlog`. The Output Log is docked across the bottom on first launch and presents simple colored text lines rather than a table. Search covers message, category, and verbosity; level filters, pause, clear, copy, category colorization, and auto-scroll remain panel-local state. Stable category colors apply to the whole line for normal records. Warnings are always yellow and errors are always red so severity cannot be hidden by a category hue.

The log text surface behaves like a read-only text editor. LMB drag selects continuously across lines, Shift extends the range, Ctrl+A selects all visible text, and Ctrl+C copies the selected UTF-8 range. The toolbar Copy action copies the selection or all visible records when no range exists. Per-record `InputText` widgets are not used because selection cannot cross widget boundaries. ToolUI provides a small public-API renderer with text hit testing, UTF-8 boundary-safe carets, selection rectangles, clipping, and horizontal and vertical scrolling while preserving per-line colors.

Auto-scroll follows appended records only while the view already owns the tail, or after an explicit request such as command submission. Scrolling upward relinquishes the tail so background logs do not pull the user away. A command request remains pending through the echoed command and its result batch, preventing the result from appearing one line below the visible region.

The command input queries an `EditorCore` command registry shared by interactive UI and `HertaEditorCmd`. Prefix matches open above the bottom input; Up and Down choose a match, Tab completes it, and the same keys navigate history when no suggestions are open. Commands publish their echo and structured results through normal logging. UI code does not own command semantics or duplicate headless command implementations.

Global ImGui metrics are shared behavior, not isolated widget decoration. In particular, checkbox size follows frame height. ToolUI must not shrink global `FramePadding` to resize checkboxes because that also changes buttons and inputs. A genuinely compact control uses a Herta helper or locally scoped style. Primary white pill buttons suppress the normal frame border to avoid a dark aliased outline.

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

Cross-scene copy and paste uses a versioned UTF-8 clipboard fragment containing selected entities, components, hierarchy, stable source IDs, and asset references. Paste generates new entity IDs, remaps references within the fragment, preserves valid external asset IDs, and commits atomically as one undoable transaction. It does not duplicate referenced assets. Cross-project dependency migration is a separate explicit operation because it may copy or remap content.

Numeric property fields accept deterministic expressions with `+`, `-`, `*`, `/`, unary signs, and parentheses. The parser lives in EditorCore and is shared by interactive and headless property-editing commands. It evaluates with `double`, rejects division by zero, non-finite values, trailing input, and destination overflow, then performs unit conversion and type conversion at the property boundary. It never invokes the scripting runtime or a general code evaluator.

The optional translucent-panel style renders the scene beneath the main editor panel region, builds one downsampled blurred scene-color pyramid per viewport after tone mapping, and lets all visible panels sample that shared result. Text, icons, controls, selection outlines, and gizmos are drawn afterward at native resolution. The pass is skipped when no visible panel requests blur and never recursively blurs previously drawn UI. Opacity, tint, radius, quality, and enable state are per-user editor settings under `Saved/Editor`.

Detached native ImGui viewports cannot portably blur another swapchain or the desktop, especially on Wayland. They use an opaque or tinted fallback unless Herta renders a meaningful backdrop for that viewport. The feature uses ToolUI draw integration and a Herta RenderGraph shader, not an ImGui fork.

### 4.14 Editor icons and fonts

Source icons remain `.svg` files under `Engine/Content/Editor/Icons`; fonts remain `.ttf` or `.otf` files under `Engine/Content/Editor/Fonts`. Each asset keeps its license metadata beside the source. Editor assets are content, not generated C++ byte arrays. A cooked editor package may be embedded through the normal package system when producing a monolithic executable.

The Milestone 1 editor font is Roboto Regular and Medium, rasterized through Dear ImGui's FreeType builder with normal hinting. Its SIL Open Font License and the required FreeType attribution are stored with dependency and content metadata, included in generated third-party notices, and available through the editor About surface. Packaging fails when redistributed font or library bytes lack required license metadata. Tests validate semantic markers instead of exact license byte counts. ToolUI keeps the font content generation alive as long as Dear ImGui may reference its source bytes for atlas rebuilding or dynamic glyph generation.

Development reads editor resources loosely for immediate iteration. Packaged editor builds read external or embedded cooked resources through the same Herta-owned provider. Milestone 1 keeps this provider narrow; the package milestone later replaces its storage backend with VFS mounts without changing ToolUI call sites. Executable embedding uses one cooked editor package rather than generating a public C++ array API per asset.

`EditorFramework` owns a Herta SVG adapter with LunaSVG private behind it. The adapter rasterizes an icon to RGBA at the requested physical pixel size, uploads it as an ImGui texture, and caches by source hash, pixel dimensions, and scale. Rasterization may run off the UI thread, but GPU upload and cache publication occur at an explicit frame boundary.

Herta editor icons use a deterministic static SVG subset: paths and basic shapes, `viewBox`, solid fills, strokes, clipping, and gradients when needed. Scripts, animation, filters, external resources, embedded raster images, and SVG text are rejected. This keeps rendering portable and avoids hidden font, network, and timing dependencies.

Milestone 1 title-bar controls use ImGui draw primitives and do not require a production icon set. LunaSVG and the real editor icon library enter only when the first SVG-backed editor tool requires them. Normal Dear ImGui font rasterization is the initial native-resolution UI path; msdfgen is deferred until a measured world-space, zoomable-canvas, or vector-text requirement exists.

### 4.15 Audio

Audio is a Herta-owned runtime module with miniaudio private behind it. miniaudio supplies Windows and Linux device backends, decoding, resampling, mixing primitives, spatialization, and a null backend. Herta owns sound assets, handles, buses, voices, concurrency limits, streaming policy, scene integration, profiling, and the public API.

- The audio callback never allocates, blocks, acquires engine locks, or performs normal logging.
- Game-thread changes cross through a bounded command queue and audio-thread events return through a bounded result queue.
- Headless programs use no device or the null backend without changing gameplay logic.
- Source WAV and FLAC are sufficient initially. Add Opus when measured dialogue or music streaming requirements justify it.
- Static, streaming, and already compressed audio payloads carry explicit cook policy and are not blindly compressed again with zstd.
- Audio Graph later compiles to immutable runtime data and has no runtime dependency on ImGui or imgui-node-editor.

Spatial audio consumes Herta transforms in meters. Backend coordinate conversion occurs in one tested adapter. Advanced HRTF or acoustic simulation remains an optional extension selected after the basic mixer and spatial path are measured.

[Steam Audio](https://github.com/ValveSoftware/steam-audio) is the preferred optional acoustics candidate after the miniaudio mixer and device path are stable. It complements rather than replaces miniaudio: Herta and miniaudio retain voice, decoding, streaming, bus, mix, and device ownership, while the private SteamAudio integration may provide HRTF binaural rendering, Ambisonics, directivity, air absorption, occlusion, transmission, reflections, convolution reverb, and pathing. A Steam-processed voice bypasses miniaudio's spatializer so attenuation and panning are not applied twice.

Steam Audio simulation never runs on the main or audio-processing thread. A bounded acoustics service performs direct, reflection, and pathing updates at configured rates, coordinates Steam Audio's internal thread count with Herta worker budgets, and publishes immutable results to preallocated audio-thread effects. The audio callback performs no simulation, allocation, blocking, or normal logging. Headless and unsupported targets retain the miniaudio null or basic-spatialization path without Steam Audio linked.

Herta and Steam Audio both use meters, but their axes differ. Steam Audio uses +X right, +Y up, and -Z forward, so the tested adapter maps a Herta vector to `{-X, +Y, -Z}`. This is a proper 180-degree rotation around +Y and preserves counter-clockwise winding. Acoustic geometry and material data are generated from canonical Herta assets; Steam Audio handles never enter scenes or serialized Herta source data.

Begin with Steam Audio's cross-platform built-in CPU ray tracer. Embree and custom Herta ray-tracing callbacks are later measured options. Radeon Rays and TrueAudio Next are not baseline dependencies because their OpenCL and Windows-focused paths conflict with Herta's Vulkan-first Windows/Linux portability. Real-time acoustics are the initial path; optional acoustic probes remain derived cooked data and do not weaken the no-light-baking policy or enter source control.

### 4.16 Navigation and AI

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

### 4.17 Packages, embedded applications, and mods

All runtime content is read through a Herta virtual filesystem and package interface. Loose cooked files, external packages, executable-embedded packages, and mod packages differ only in how their byte ranges are mounted.

Assets owns runtime package reading, VFS mounts, precedence, decompression, and asset lookup. `AssetPipeline` owns source import and canonical asset cooking. `PackageBuilder` owns package indexes, chunk layout, compression, executable embedding, and deterministic archive output. `HertaAssetWorker` isolates individual imports and cooks, `HertaCooker` orchestrates AssetPipeline and PackageBuilder for a target, and `HertaEditorCmd` only exposes those implementations as headless commands.

```text
Embedded base package
External base packages  -> ordered mount table -> VFS -> asset loader
External mod packages
```

A package contains a versioned index and independently readable chunks. Each chunk records asset identity, type, offset, alignment, compression codec, compressed size, uncompressed size, and content hash. Zstandard is the default general-purpose codec. BC/KTX textures, Opus audio, and other already compressed payloads normally use `None`. Decompression validates all sizes and configured limits before allocation.

Content hashes detect corruption but do not establish publisher authenticity when an attacker can replace both data and hashes. Publisher Shipping packages carry an Ed25519 signature over the canonical package header, index, and ordered chunk hashes. The private signing key exists only in protected release infrastructure; player builds contain versioned public verification keys. The VFS verifies the index before mount and verifies chunk hashes as data is read. Mod trust policy distinguishes publisher-signed, project-trusted, user-approved unsigned, and rejected packages without presenting them as equivalent.

Package encryption is deferred until a project defines a concrete threat model and key-provisioning policy. Client-side authenticated encryption can deter casual extraction, but it cannot make assets secret from the owner of the executing machine. If adopted, Herta uses a reviewed primitive such as XChaCha20-Poly1305 through libsodium, compresses before encryption, authenticates metadata, defines nonce and key rotation policy, and resolves the conflict between random nonces and byte-for-byte reproducible packages. Herta never invents cryptographic primitives or describes obfuscation as DRM.

The cooker supports three output policies:

- Loose cooked files for development.
- Executable plus external packages for normal games and large applications.
- A monolithic executable with the base package added as a platform linker resource or read-only section before signing.

Executable embedding never generates C++ byte arrays and uses the same package reader as external content. Project targets compose only the modules they need, so a GUI application may use Application and ToolUI without Scene, Physics, Animation, or game systems. A monolithic executable still relies on the operating system, GPU driver, Vulkan loader availability, and required platform libraries.

Initial mod support is data-only. Mods remain external even when the base package is embedded. Each mod manifest declares a stable namespace, version, engine compatibility, dependencies, load order constraints, and explicit asset-override intent. Mount resolution is deterministic, asset-ID collisions are diagnosed, and missing or incompatible dependencies produce actionable errors. Mod packages are untrusted input and use the same bounds, recursion, decompression, and path validation as imported assets.

Native C++ binary mods are deferred because they would expose an unstable ABI and execute unrestricted code. A future behavior-mod boundary should use a versioned scripting or sandboxed runtime contract. Source mods built together with an exact Herta revision remain possible without promising binary compatibility.

### 4.18 Projects and templates

Game and GUI application projects are first-class source trees described by `<ProjectName>.hertaproject`. The versioned descriptor contains a stable project ID, display name, engine association, modules, program targets, content roots, and enabled features. It never stores an absolute engine path or user-specific directory. Project generation receives an explicit descriptor path and resolves the associated engine through command-line selection or user-local installation metadata.

Built-in templates live under `Templates/Projects/<TemplateId>` and are versioned with the engine. Each template has a data-only manifest declaring its stable ID, version, compatible project-descriptor version, files, and allowed substitutions. Templates never execute scripts, download dependencies, or duplicate engine build logic.

Project creation is available through both the project browser and `HertaEditorCmd`. It validates the project name, C++ module identifier, destination, and path lengths, then expands only declared UTF-8 text files into a sibling staging directory. Binary files are copied without token replacement. The completed project is validated and atomically renamed into an absent destination, so cancellation or failure never leaves a half-created project and existing files are never overwritten.

Start with one minimal Game template. Add Blank, GUI Application, or specialized templates only when their owning modules exist. Generated projects may use a source checkout or an installed engine. Generated build products, rather than the source descriptor, record the exact engine build identity used to compile native modules.

### 4.19 Scripting

Scripting is optional. Herta, native C++ games, GUI applications, headless tools, and data-only mods must build and run without a scripting runtime. Scripting exists for fast gameplay iteration, controlled behavior mods, and selected editor automation, not as a foundation dependency or replacement for C++ engine systems.

[Umka](https://github.com/vtereshkov/umka-lang) is the preferred candidate after a focused spike. It is statically typed, compiles to a bytecode VM, is implemented in C99, exposes a C embedding API, uses reference-counted garbage collection, supports cooperative fibers, and uses the BSD-2-Clause license. [UEmka](https://github.com/Solessfir/UEmka) is useful design evidence for typed function-signature and graph-pin integration, but Unreal-specific code and assumptions do not enter Herta unchanged.

`Scripting` owns the public Herta contract and keeps Umka private. Public APIs use stable script module and function handles, Herta values, diagnostics, and explicitly owned buffers. `Umka`, `UmkaStackSlot`, VM pointers, garbage-collected pointers, and raw engine pointers never cross the module boundary. Do not create a generic multi-language backend abstraction until a second runtime is genuinely required.

Source `.um` files are editor assets. The asset pipeline validates imports and cooks them into a deterministic, versioned script program containing dependency hashes, binding-schema versions, VM revision, and debug mappings. A normal Shipping build loads cooked programs and does not compile source. Runtime source compilation is included only when a project explicitly enables a trusted development or mod-authoring mode.

The documented [Umka embedding API](https://github.com/vtereshkov/umka-lang/blob/master/doc/api.md) can compile source, call typed functions, report stack frames and memory usage, disable filesystem access, and disable implementation libraries. It does not currently document a stable bytecode save/load contract, instruction interruption, enforced heap limits, or a full line debugger. These are adoption gates. A narrow, maintained Herta fork is acceptable if required changes remain reviewable and upstream synchronization stays practical. Otherwise reject the candidate rather than shipping internal VM memory dumps or an incomplete sandbox.

Initial runtime rules are:

- Use one VM context per world or isolated script domain, not one VM per entity.
- Own and call a VM from the game thread unless upstream explicitly guarantees a different threading contract. Worker compilation uses isolated contexts.
- Register a capability allowlist generated from Herta runtime descriptors. Scripts receive entity, asset, object, and task handles instead of pointers.
- Integrate fibers with Herta update phases. Fibers do not create engine threads or bypass world mutation barriers.
- Enforce instruction, stack, heap, recursion, and host-call budgets with cancellation and actionable diagnostics.
- Disable filesystem access and native implementation libraries by default. Untrusted scripts receive no OS, process, network, dynamic-library, or unrestricted file API.
- Keep script-owned memory inside the VM. Herta-owned resources use explicit handles and remain valid independently of garbage-collection timing.

Script hot reload is transactional. Compile and validate a candidate program or VM beside the active generation, serialize explicitly persistent state through Herta descriptors, restore compatible state, then publish the candidate at a safe point. A failed compile, binding mismatch, budget violation, or restore keeps the last valid generation. Raw VM heap state is never copied or serialized.

StateTree, animation graphs, and gameplay Blueprints remain separate typed compiler domains. They may invoke scripted functions through validated Herta handles, and GraphEditor may offer an Umka Script node similar to UEmka, but graph assets do not compile to Umka source or depend on Umka bytecode internals.

Script behavior mods remain disabled until the same cooked-format validation, capability restrictions, resource budgets, cancellation, and hostile-input tests used by the engine pass on Windows and Linux. Native binary mods remain a separate, deferred security and ABI decision.

### 4.20 Localization and international text

Localization is an optional runtime composition, but its data identity is designed before scene, graph, and project formats become public. Internal identifiers, paths, logs, console commands, and diagnostics remain non-localized UTF-8 strings. User-visible localizable content uses `FText`; user-authored names and chat remain plain text.

`FTextId` is a stable namespace and key, never an English string or asset path. A source entry also carries source text, source hash, developer context, message-syntax version, and a named argument schema. Source wording changes mark translations stale without changing identity. Renames require explicit redirects or migrations. Canonical source catalogs generate `constexpr` C++ text-ID constants, so Herta does not scrape macros or depend on runtime code coverage to gather text.

`FText` has no implicit conversion to `std::string`. A resolved string deliberately loses localization identity. Formatted text retains an immutable recipe and typed named arguments so a culture change can resolve it again. Do not concatenate sentence fragments; translators receive complete messages whose arguments can be reordered and selected by grammar.

ICU4C is private to Localization and supplies BCP 47 locale handling, Unicode normalization and properties, plural and select rules, number/date formatting, collation, bidirectional analysis, and grapheme, word, and line boundaries. Public and serialized `FCultureId` values use canonical BCP 47 spelling. Herta never makes the process-global C locale, Windows NLS, a Linux system ICU installation, or an implicit machine locale authoritative. The first message syntax is the stable ICU MessageFormat with named arguments and an explicit Herta syntax version. ICU MessageFormat 2 for C++ remains a migration candidate only after it leaves technology preview. ICU types and binary resource objects never enter Herta assets or public APIs.

Repository localization data remains deterministic versioned UTF-8 text with stable ordering, LF endings, atomic writes, and no timestamps or machine paths. XLIFF 2.1 is a translation-tool interchange format, not the runtime or canonical repository format. Herta exports a documented subset and imports it with DTDs, entities, external resources, and unknown executable extensions disabled. Import validates IDs, source hashes, selectors, placeholders, types, and required plural branches before publishing changes.

Cooked catalogs are immutable binary assets containing sorted compact IDs, UTF-8 strings or versioned message programs, fallback metadata, schema versions, and hashes. Each target declares shipped cultures explicitly. The base culture always ships, optional language packs mount through the normal VFS, and fallback is deterministic, such as `pl-PL -> pl -> project default`. Servers and cookers never inherit the machine's implicit locale. ICU code and CLDR data revisions participate in cook keys, and filtered ICU data contains only declared features and cultures.

Localized display also requires typography rather than byte-oriented glyph lookup:

```text
UTF-8 logical text
    -> ICU paragraph direction, script, and break runs
    -> project font fallback
    -> HarfBuzz shaping
    -> line layout, hit testing, and grapheme-aware editing
    -> FreeType rasterization on workers
    -> paged GPU-atlas publication at a frame boundary
```

`TextLayout` owns this pipeline and keeps HarfBuzz and FreeType private. Project fonts and fallback chains are cooked assets; system fonts are not authoritative because their availability differs across machines. Shape and raster work runs off the UI thread, while GPU publication occurs at a frame boundary. Cursor movement, selection, deletion, wrapping, and hit testing operate on grapheme and shaped clusters, not bytes or isolated code points. ToolUI uses a Herta shaped-text draw and input path for complex scripts without requiring an ImGui fork.

`LocalizationPipeline` validates, pseudolocalizes, imports, exports, and cooks without ToolUI or a GPU. `HertaEditorCmd` exposes the same operations used by LocalizationEditor. Editor culture switching and catalog hot reload publish a new immutable generation; failed catalogs leave the previous generation active. Required tests cover locale fallback, plural and select behavior, argument parity, stale translations, deterministic cooking, hostile XLIFF, expanded pseudolocalization, forced RTL, mixed Arabic and Latin text, Indic shaping, combining marks, CJK fallback and line breaking, emoji sequences, and grapheme-aware editing.

Network gameplay sends semantic event or text IDs plus validated arguments when clients should localize a message. It does not send server-rendered language strings as authoritative gameplay UI. Localized voice, subtitle, and lip-sync variants use stable asset indirection and culture fallback rather than hard-coded paths.

### 4.21 Networking and replication

Multiplayer is an optional first-class composition. The default model is a server-authoritative fixed network tick with dedicated-server and player-hosted listen-server modes. A listen server composes the same authoritative simulation inside `HertaGame`; its local client communicates through the same command, replication, permission, and validation boundaries as remote clients. Host status does not grant gameplay code a shortcut into replicated client state.

```text
Transport
    -> versioned message protocol
    -> replication, relevancy, and prediction
    -> world and gameplay systems
```

`Networking` owns transport sessions, connections, packet validation, message lanes, time synchronization, and bounded inbound and outbound queues. Transport callbacks never mutate world state. `Replication` owns explicit versioned wire schemas, stable `FNetworkEntityId` values, field quantization, snapshots and deltas, input commands, interpolation, prediction, reconciliation, interest management, bandwidth budgets, and reliable or unreliable event policy. ECS handles and component memory are never transmitted directly.

The connection handshake includes protocol, schema, content, and required plugin identities. Every packet, argument, count, offset, compressed payload, and client request is untrusted input with size, rate, authority, and lifetime limits. Disconnect and timeout paths release world and task ownership deterministically.

[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) is the preferred transport candidate after a focused Windows and Linux spike. It may provide reliable and unreliable messages, fragmentation, encryption, statistics, lanes, and network-condition simulation, but it does not own Herta replication or serialization. Initial scope is direct-IP dedicated and listen servers. P2P discovery, ICE/NAT traversal, matchmaking, host migration, rollback netcode, voice chat, accounts, and online services are later capabilities with separate product requirements.

Headless integration tests run multiple clients and servers as isolated processes or loopback compositions under simulated latency, jitter, loss, reordering, duplication, bandwidth limits, reconnects, incompatible schemas, and hostile input. A deterministic simulation harness verifies logical results without claiming that internet delivery or cross-platform floating-point physics is deterministic.

### 4.22 Plugins, source control, terminal, and editor automation

User plugins are source modules built against an exact Herta revision, toolchain, architecture, and configuration. Herta does not promise a stable cross-version C++ ABI. Shipping uses static composition initially; editor dynamic loading may reuse the versioned game-module boundary only after task, delegate, type-lifetime, and unload quiescence rules are proven.

Plugins live under `Engine/Plugins` or a project's `Plugins` directory. A versioned UTF-8 `.hertaplugin` descriptor declares stable ID, version, engine compatibility, dependencies, modules, content roots, supported targets, load phase, and editor, headless, runtime, or developer capabilities. Dependency order is deterministic, cycles fail generation, discovery does not execute arbitrary build scripts, and headless targets load only explicitly compatible modules. Native plugins are trusted unrestricted code; data and script plugins retain their separate trust policies. The editor supports restart-required enable or disable first and records enough startup state to offer a safe mode after a plugin crash.

Editor extension registries use owned RAII registrations for menus, panels, commands, inspectors, asset actions, and settings. A plugin unload or failed activation removes every registration and owned task before code unload. Git source control, the terminal, and MCP are first-party plugins that exercise the same public extension contracts offered to projects.

The Git plugin invokes the user's installed Git executable through Platform process APIs with argument arrays, never a shell-built command. Background status uses porcelain v2 with NUL-delimited records and avoids optional locks. Operations are asynchronous, cancellable, credential handling remains with Git credential helpers, and polling never mutates the repository. The UI exposes precise Fetch, Pull with an explicit strategy, Commit, Push, Diff, Stage, and Revert operations. It never silently resets, overwrites unsaved documents, or treats ambiguous `Submit` and `Get Latest` labels as universal source-control semantics.

The terminal plugin uses ConPTY on Windows and a PTY on Linux with independent bounded input, output, and scrollback handling, UTF-8 and ANSI terminal parsing, resize support, cancellation, and Job Object or process-group cleanup. It sanitizes dangerous control sequences and does not include terminal contents in logs or crash reports by default. Terminal execution is a user tool, not Herta's structured Build, Cook, Test, or SourceControl API.

EditorCore exposes semantic, schema-described automation commands with stable object IDs, query and mutation separation, expected document revisions, progress, cancellation, safe-point execution, and named undo transactions. Interactive UI, plugins, headless commands, tests, and MCP call these operations instead of duplicating behavior or simulating input.

`HertaEditorMcp` is an out-of-process adapter connected to an active editor through authenticated local IPC. It exposes no raw ImGui clicking and no unrestricted terminal tool. Sessions are local-only and read-only by default; mutation, import, build, source control, deletion, packaging, and process capabilities require explicit grants and confirmation appropriate to their risk. Commands enforce path scopes, input schemas, rate and resource limits, timeouts, output sanitization, audit records, and revocation. Long operations return job IDs and never block the editor or protocol loop.

## 5. Executables

| Program | Responsibility | Ships to players |
|---|---|---:|
| `HertaEditor` | Editor host, runtime preview, asset and graph tools | No |
| `HertaEditorCmd` | Orchestrates headless editor validation, migration, import, cooking, and graph-compilation commands | No |
| `HertaGame` | Standalone Sandbox game target | Yes |
| `HertaServer` | Dedicated authoritative server without windows, rendering, ImGui, or audio devices | Optional |
| `HertaAssetWorker` | Isolated source import and canonical asset-cooking tasks | No |
| `HertaShaderWorker` | Shader compilation and reflection | No |
| `HertaCooker` | Orchestrates target asset cooking and deterministic platform package construction | No |
| `HertaEditorMcp` | Out-of-process MCP adapter over authenticated EditorCore automation | No |
| `HertaTests` | Unit and fast integration test runner | No |

Static module composition is the Shipping default. Development editor builds may compile project gameplay code as one reloadable game module. This is a fast iteration facility for source projects built against the exact Herta revision, toolchain, architecture, and configuration. It is not a stable binary-plugin or native-mod ABI.

### 5.1 Game C++ hot reload

Platform loads game DLLs on Windows and shared objects on Linux through Herta's dynamic-library wrapper. The module exports one `extern "C"` entry point that returns a versioned, sized POD function table. The boundary carries Herta handles and explicitly owned buffers, not STL objects, exceptions, RTTI objects, allocators, virtual interfaces, `spdlog` state, or ownership that could be destroyed by the other binary.

Each successful build has a unique build ID. The editor copies the candidate library and debug symbols to `Saved/HotReload/<BuildId>` before loading it. Unique shadow copies avoid the loaded-file replacement restriction on Windows and give Linux the same deterministic lifecycle. Old generations are removed only after they are unloaded and no diagnostic still references their symbols.

Reload is transactional at an editor safe point:

1. Load the candidate beside the active module and validate its API version, build identity, required engine services, and descriptors without mutating the world.
2. Stop dispatching new work to the old module, cancel or join its jobs, disconnect delegates, and reject any remaining thread or callback into its code.
3. Snapshot reloadable state into engine-owned ECS, reflected, or explicitly serialized storage.
4. Activate the candidate and restore compatible state, then atomically publish its systems and callbacks.
5. Unload the old module only after all code pointers and type lifecycle operations have been replaced.

If validation or activation fails, the candidate is unloaded and the old module remains active. The first implementation reloads gameplay systems whose mutable state already lives in engine-owned storage. A changed C++ component layout, live game-owned object with a module vtable, static lifetime, or incompatible descriptor requires a deliberate migration or editor restart. Herta must not claim successful reload while stale code pointers remain.

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

Data crosses thread boundaries as immutable frame packets, commands, or owned jobs. Systems do not create arbitrary private threads. Long-lived threads use `std::jthread` and cooperative cancellation. Blocking IO has a bounded lane separate from CPU work so slow disks and child processes cannot starve simulation or editor tasks.

User experience takes priority over maximizing worker occupancy. An editor operation expected to exceed one interactive frame must expose progress and cancellation, preserve the last valid result until atomic publication, and avoid synchronous waits on the main thread. Cancellation is cooperative but stale generations are never published. Queue depth, execution time, wait time, cancellation latency, and worker utilization are observable through profiling and diagnostics.

The game API exposes structured task scopes rather than raw worker threads. A world may run independent systems or partitioned queries concurrently when declared read/write sets do not conflict. Structural ECS mutation, unsafe third-party callbacks, and final publication remain explicit phase boundaries. A deterministic single-worker test mode validates ordering and failure behavior without claiming that normal parallel execution has deterministic wall-clock scheduling.

## 7. Dependency policy

Source dependencies are pinned Git submodules under `External`. Optional system integrations such as Blender are capabilities, not Herta dependencies. A normal project-generation or build command performs no network access. `Setup.bat` and `Setup.sh` are the only scripts allowed to initialize submodules and acquire non-vendored tools.

| Dependency | Decision | Owner | Purpose and boundary |
|---|---|---|---|
| [Vulkan SDK](https://vulkan.lunarg.com/) | Adopt at renderer start | Setup, NvrhiVulkan | Headers, validation, tools, and SPIR-V environment. Runtime uses the system loader. |
| [Herta GLFW fork](https://github.com/Solessfir/glfw) | Adopted at MS1 | Application | Windows, X11, Wayland, input, surfaces, and generic custom-titlebar support. Begin from commit `f2e6bb9b`. Private API. |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Adopted at MS1 presentation bootstrap | NvrhiVulkan | Vulkan 1.3 device, swapchain, resource, and command implementation behind Herta RHI. MS1 uses the minimum UI path; MS2 expands the renderer-facing contract. |
| [Dear ImGui](https://github.com/ocornut/imgui) | Adopted at MS1 | ToolUI | Docking and multi-viewport editor and tool UI. GUI programs may compose ToolUI without the game or editor runtime. Apply the centralized EditorStyle baseline rather than feature-local styling. |
| [LunaSVG](https://github.com/sammycage/lunasvg) | Adopt with first SVG-backed editor tool | EditorFramework | Private CPU rasterizer for static SVG editor assets. Cache RGBA results as ImGui textures; LunaSVG types never enter Herta APIs. |
| [Stack Layout PR 846](https://github.com/ocornut/imgui/pull/846) | Do not adopt | None | The initial editor does not justify an unmerged patch to ImGui internals. |
| [Herta imgui-node-editor fork](https://github.com/Solessfir/imgui-node-editor) | Adopt with first node-canvas graph | GraphEditor | Maintained third-party fork for graph visualization and interaction, not graph semantics or execution. Preserve upstream history and document Herta patches. |
| [doctest](https://github.com/doctest/doctest) | Adopt with Core | HertaTests | Unit tests beside modules, one test runner. |
| [spdlog](https://github.com/gabime/spdlog) | Adopted at MS1 | Core | Compiled private backend for console, debugger, rotating-file, and editor-buffer sinks. Herta owns the public logging contract and record schema. |
| [enkiTS](https://github.com/dougbinks/enkiTS) | Adopted at MS1 | Tasks | Private worker scheduling implementation. Herta owns task scopes, cancellation, reload quiescence, IO lanes, diagnostics, and public APIs. |
| [Umka](https://github.com/vtereshkov/umka-lang) | Preferred candidate at scripting milestone | Scripting | Optional statically typed gameplay VM behind Herta handles, bindings, cooking, budgets, diagnostics, and sandbox policy. Adopt only if production gates pass. |
| [EnTT](https://github.com/skypjack/entt) | Preferred at world milestone after spike | Scene | Private ECS storage candidate. Herta owns entity, world, query, serialization, scheduling, and mutation-barrier contracts. |
| [fastgltf](https://github.com/spnda/fastgltf) | Adopt with asset import | AssetPipeline | Offline glTF 2.0 ingestion only. |
| [Blender](https://www.blender.org/) | Optional system tool | AssetPipeline | Used only for `.blend` import and live reimport. Never downloaded by Setup or required to build or run Herta. |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Adopt at physics milestone | Physics | Collision and rigid-body simulation behind Herta types. |
| [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) | Adopt at animation milestone | Animation, AssetPipeline | Offline optimization plus runtime sampling and blending primitives. |
| [miniaudio](https://github.com/mackron/miniaudio) | Adopt at audio milestone | Audio | Private device, decoder, mixer, resampler, spatialization, and null-backend implementation behind Herta Audio. |
| [Steam Audio](https://github.com/ValveSoftware/steam-audio) | Preferred optional candidate after base audio spike | SteamAudio | Private HRTF and environmental-acoustics backend behind Herta Audio. Start with the built-in CPU ray tracer; do not make OpenCL, Radeon Rays, TrueAudio Next, or acoustic baking mandatory. Preserve Apache-2.0 notices and Valve trademark boundaries. |
| [Recast Navigation](https://github.com/recastnavigation/recastnavigation) | Adopt at navigation milestone | Navigation subsystem | NavigationBuilder owns offline Recast use; runtime Navigation owns Detour queries, streaming, tile-cache updates, and DetourCrowd behind Herta APIs. |
| [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) | Preferred transport candidate after spike | Networking | Message transport, encryption, lanes, statistics, and network simulation behind Herta sessions. It does not own replication or serialization. Defer optional P2P and ICE dependencies. |
| [ICU4C](https://github.com/unicode-org/icu) | Adopt at localization milestone | Localization | Private Unicode, BCP 47 locale, MessageFormat, plural, formatting, collation, BiDi, and boundary services with pinned CLDR data. |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | Adopt at localization milestone | TextLayout | Private shaping backend. Herta owns font fallback, layout, hit testing, caches, and public text types. |
| [FreeType](https://gitlab.freedesktop.org/freetype/freetype) | Adopted at MS1, expand at localization milestone | ToolUI, TextLayout | Private hinted rasterization for the editor, later expanded to project text parsing and glyph rasterization. Font data is untrusted and worker lifetime and face concurrency are explicit. |
| [Zstandard](https://github.com/facebook/zstd) | Adopt at package milestone | Assets | Default general-purpose package chunk compression behind Herta stream and package APIs. Use the BSD license option. |
| [libsodium](https://github.com/jedisct1/libsodium) | Adopt when package signing is implemented | PackageBuilder | Private Ed25519 signing and verification; optional authenticated encryption only after a reviewed threat and reproducibility policy. |
| [Slang](https://github.com/shader-slang/slang) | Adopt at shader milestone | ShaderCompiler | HLSL-like source to SPIR-V plus reflection. |
| [AMD FidelityFX FSR](https://gpuopen.com/fidelityfx-super-resolution-3/) | Adopt after native TAA and upscaler contracts | Renderer adapter | First optional Vulkan temporal-upscaling integration. Vendor types and lifecycle remain private. Frame generation is later. |
| [NVIDIA Streamline/DLSS](https://github.com/NVIDIA-RTX/Streamline) | Optional later plugin | Renderer plugin | Vulkan DLSS integration without making NVIDIA binaries or device capabilities a renderer foundation. Validate Windows and Linux deployment independently. |
| [Intel XeSS](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html) | Optional later plugin | Renderer plugin | Vulkan temporal upscaling through the same Herta input contract after native TAA and FSR are stable. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Add when mesh cooking exists | AssetPipeline | Mesh optimization, simplification, and later meshlets. |
| [KTX-Software/Basis Universal](https://github.com/KhronosGroup/KTX-Software) | Add when texture cooking exists | AssetPipeline | KTX2 texture cooking and runtime transcode targets. Audit per-file licenses. |
| [Tracy](https://github.com/wolfpld/tracy) | Add when frame systems exist | Core, Renderer | CPU, allocation, lock, and Vulkan profiling. Compile out in Shipping. |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Defer | NvrhiVulkan | Requires deliberate NVRHI allocation integration. Do not create two allocation authorities. |
| [volk](https://github.com/zeux/volk) | Defer | NvrhiVulkan | Add only if its dispatch model is proven compatible with the selected NVRHI/Vulkan-Hpp integration. |
| [GLM](https://github.com/g-truc/glm) | Optional private oracle | Math tests | Never a public Herta type. Prefer no dependency initially. |
| [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) | Do not adopt | None | Its runtime, XML, blackboard, and plugin model overlap Herta StateTree, GraphCore, reflection, and serialization. Use only as a design reference. |
| [HFSM2](https://github.com/andrew-gresyk/HFSM2) | Do not adopt | None | Its compile-time static structure does not fit Herta's editor-authored, cooked StateTree assets. Use only as a design reference. |
| [RVO2](https://github.com/snape/RVO2) | Defer | Navigation | Evaluate only if DetourCrowd fails measured avoidance or crowd requirements. |
| [Radiance Cascades](https://radiance-cascades.com/) | Research only until acceptance gates pass | Renderer | Experimental dynamic-GI reference. Do not make it the sole shipping GI path or add a runtime dependency before the 3D spike succeeds. |
| [nob.h](https://github.com/tsoding/nob.h) | Do not adopt as primary build | None | Experimental C build-recipe library. Herta would have to own project generation, dependency scanning, incremental scheduling, and IDE integration. |
| [BGFX](https://github.com/bkaradzic/bgfx) | Reject | None | Overlaps NVRHI, Vulkan ownership, shader abstraction, and renderer learning goals. |

Likely later systems, with no dependency selected yet:

- Crash reporting: start with platform crash dumps, add a service only when distribution needs it.
- Voice chat: project-driven, with no dependency selected yet.

[Wicked Engine](https://github.com/turanszkij/WickedEngine) is an MIT-licensed implementation reference for job scheduling, Vulkan rendering, temporal effects, dynamic GI, ray tracing, volumetric clouds, and shared GUI-backdrop blur. [Doriax](https://github.com/doriaxengine/doriax) is a reference for ECS/editor workflow, asynchronous resource handling, scripting integration, and agent-facing tooling. Herta studies their tradeoffs but retains its own ownership model, RenderGraph, task API, scripting decision, and data formats. Reused code must remain attributable and pass Herta's dependency and architecture review rather than entering by copy-and-paste convenience.

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

### 8.2 Project-local third-party tools and SDKs

Downloaded host tools live under ignored `External/<tool>/<platform>/<version>` directories. Downloaded development SDKs live under ignored `SDK/<platform>/<sdk>/<version>` directories. `Config/Dependencies.lock` is the canonical binary dependency manifest consumed by Setup, Premake, and CI. It uses a strict line format so the bootstrap layer does not require Python, `jq`, a JSON library, or an already-built Herta tool.

```text
HERTA_DEPENDENCIES_V1
# name|kind|platform|version|license|url|sha256|installed-entry
premake|tool|windows-x64|5.0.0-beta8|BSD-3-Clause|https://...|<sha256>|premake5.exe
premake|tool|linux-x64|5.0.0-beta8|BSD-3-Clause|https://...|<sha256>|premake5
```

The parser accepts UTF-8, blank lines, and full-line `#` comments. Every data line has exactly eight pipe-delimited fields. Fields may not contain a pipe or newline, duplicate name/platform entries are errors, SHA-256 is mandatory, and unknown schema versions or kinds fail closed. Values are data and are never evaluated or sourced as shell code. Setup never downloads an unversioned `latest` artifact.

Setup removes each downloaded archive or installer after the installed tree passes validation. `External/Premake` and `SDK` contain usable installations only, not a second download cache.

`Dependencies.lock` covers downloaded binary tools and SDKs only. Git submodule revisions remain locked by Git's recorded gitlinks instead of being duplicated in this file.

The Vulkan SDK can be local to the Herta checkout:

- [Windows Setup](https://vulkan.lunarg.com/doc/view/latest/windows/getting_started.html) runs the LunarG installer with `--root <repo>/SDK/Windows/Vulkan/<version>` and `copy_only=1`. This copies files without registry changes, shortcuts, administrator rights, or a system `PATH` update.
- [Linux Setup](https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html) verifies and extracts the official tarball under `SDK/Linux/Vulkan/<version>`.
- Generated Debug and Development launch settings supply `VULKAN_SDK`, SDK tools, runtime libraries, and `VK_ADD_LAYER_PATH` to Herta child processes on both platforms. If a platform loader still cannot discover the project-local validation layer, startup reports the downgrade and continues without Vulkan or NVRHI validation instead of making a fresh clone unusable.
- Premake receives the resolved SDK root explicitly. It does not read a required global `VULKAN_SDK` variable.

The development SDK is local, but the Vulkan-capable GPU driver and production loader remain operating-system or driver responsibilities. Linux X11, Wayland, and compiler development packages may also require system package-manager installation.

Premake binaries live under `External/Premake/<platform>/<version>`. Source dependencies also live under `External`, but remain pinned Git submodules rather than downloaded binary trees. Actual SDKs such as Vulkan remain under `SDK`. Blender is explicitly excluded because it is a user-managed system installation. Downloaded tool and SDK directories are disposable and never committed.

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
3. Download a pinned Premake binary into `External/Premake` if missing and verify its checksum.
4. Download or validate project-local SDKs required by the selected targets. Vulkan is added only when Vulkan work begins.
5. Probe the optional system Blender installation and report its version and `.blend` integration status. Absence is not a Setup failure, and Setup never installs Blender. No separate Python check is required.
6. When selected targets need them, detect missing Linux X11, Wayland, xkbcommon, and Vulkan development packages and provide or run the appropriate supported package-manager command.
7. Print actionable diagnostics and remain safe to run repeatedly.

Windows Setup prefers Visual Studio 2026 with `v145`, then automatically falls back to Visual Studio 2022 with `v143`. An explicit `-VisualStudioVersion` remains strict. Setup compiles a C++23 probe with each candidate toolset. Linux Setup compiles, links, and runs the same capability probe with the selected compiler.

The probe covers the C++23 standard-library facilities Herta uses immediately, including `std::expected`, `std::move_only_function`, and `std::print`. Checking only the language mode is insufficient because a compiler may accept `-std=c++23` while its paired standard library lacks required types. GCC with libstdc++ requires version 14 or newer for Herta's baseline. Ubuntu 24.04 CI installs GCC 14 explicitly. Clang 19 is the minimum supported Clang because it exposes the Concepts feature level required for libstdc++ to enable `std::expected`.

`GenerateProjectFiles` resolves the same supported Visual Studio action when none is specified, validates the already-installed pinned Premake executable, then invokes it for the selected generator. Explicit generator arguments remain strict. Project generation performs no downloads, submodule updates, or dependency mutations. Run Setup explicitly when bootstrap state must change.

### 8.5 Cleanup

`Cleanup.bat` and `Cleanup.sh` restore a clean Herta-managed workspace without requiring Git. They remove `Binaries`, `Intermediate`, `DerivedDataCache`, `Saved`, `External/Premake`, `SDK`, `TestResults`, generated root project files, and known IDE state. The scripts validate the repository root and every destructive target before removal.

This is the generated-state equivalent of `git clean -fdx`, not an imitation of Git's tracked-file database. Arbitrary untracked files inside source directories are preserved because a Git-independent script cannot distinguish scratch work from source safely.

## 9. GitHub workflows and repository policy

GitHub Actions must call the same checked-in entry points used locally. Workflow YAML selects triggers, permissions, runners, and matrices. Build logic belongs in `Scripts/CI` or normal Herta build commands, not duplicated shell fragments hidden in YAML.

### 9.1 Workflow layout

| Workflow | Triggers | Responsibility |
|---|---|---|
| `ci.yml` | Pull request, changed daily `main`, `merge_group`, optional manual | Required builds, unit tests, platform integration, and small asset smoke tests |
| `quality.yml` | Pull request, changed daily `main`, `merge_group`, optional manual | Formatting, warnings, clang-tidy, generated-file checks, and sanitizers |
| `codeql.yml` | Weekly and optional manual | C/C++ CodeQL analysis using the real build |
| `dependency-review.yml` | Pull request | Vulnerability, license, submodule, binary-lock, and workflow-action review |
| `nightly.yml` | Scheduled and manual | Full asset corpus, optional Blender integration, render regression, stress, TSan, and recovery tests |
| `release.yml` | Protected version tag or manual | Clean reproducible packages, checksums, notices, SBOM, attestations, and draft release |

All required workflows listen for `merge_group` from the start so they remain compatible with GitHub's [merge queue](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/configuring-pull-request-merges/managing-a-merge-queue). CI and Quality batch direct `main` development into daily validation and compare the scheduled revision with the previous completed scheduled run. They skip their expensive jobs when the revision is unchanged. Manual dispatch remains available for diagnostics and always runs.

Use workflow concurrency to cancel superseded work on the same ref. A newer revision is the authoritative validation target, including scheduled and direct diagnostic runs.

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

CI, Quality, and CodeQL support `workflow_dispatch` for optional diagnostic runs. Manual execution is never part of the normal validation contract. Scheduled jobs record the tested engine revision and do not silently test a moving branch after checkout. CodeQL still runs weekly without a source-change guard because analyzer updates can find new issues in unchanged code.

### 9.2 Required build matrix

Use explicit [GitHub-hosted runner labels](https://github.com/actions/runner-images), not `windows-latest` or `ubuntu-latest`. Runner images still receive updates, so Setup pins every tool that affects Herta's output and records the runner image version.

The initial required matrix is intentionally explicit rather than a full Cartesian product:

| Runner | Compiler | Configuration | Required coverage |
|---|---|---|---|
| `windows-2025-vs2026` | MSVC x64 | Development | Win32, unit tests, Vulkan validation smoke, interactive and headless editor startup, custom title bar |
| `windows-2025-vs2026` | MSVC x64 | Shipping | Shipping compile, cooker, package, and launch smoke |
| `ubuntu-24.04` | Clang 19 x64 with libstdc++ 14 | Debug-ASan | Core tests, ASan/UBSan, X11, Wayland, null, headless editor, Vulkan validation |
| `ubuntu-24.04` | GCC 14 x64 | Shipping | Compiler portability, Shipping compile, cooker, and launch smoke |

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

GitHub may attempt its `build-mode: none` overlay optimization before detecting Herta's manual build. The action can warn and fall back to a normal full database. Keep the manual traced build because exact Premake defines, include paths, generated sources, and target selection matter more than removing a cosmetic optimization warning.

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

The CodeQL analysis job grants `actions: read`, `contents: read`, `packages: read`, and `security-events: write`. `actions: read` is required for CodeQL workflow-run metadata and avoids silently degraded telemetry and feature selection. Public repositories run CodeQL and Dependency Review automatically. Private repositories skip the hosted security steps unless GitHub Code Security is enabled and the repository variable `HERTA_CODE_SECURITY_ENABLED` is set to `true`; Herta's dependency-manifest validation remains active regardless.

Security rules:

- Pin every third-party action to a full 40-character commit SHA, with its release tag in a comment for readability.
- Configure Dependabot for `github-actions` and `gitsubmodule` ecosystems to propose reviewed updates. Group the components of one action into a single pull request, and exclude submodules intentionally pinned to a release or non-default branch from automatic revision changes.
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

- Require pull requests and the stable aggregate checks `CI / required` and `Quality / required`.
- Require conversation resolution and block force pushes and deletion.
- Require Dependency Review once it contains meaningful coverage. Treat weekly CodeQL as repository security scanning rather than a pull-request gate.
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
- Logging category filtering, sink routing, deterministic flush, sink-failure behavior, bounded-history truncation, cursor reset, and concurrent producer ordering.
- Task cancellation, scope teardown, IO-lane isolation, main-thread continuation ownership, reload quiescence, starvation, and deterministic single-worker execution.
- Serialization versioning and corrupted-input rejection.
- RenderGraph dependency, lifetime, and barrier planning.
- Asset build-key determinism and atomic import behavior.
- Drag-and-drop batch validation, fuzzy-search cancellation and ranking, numeric-expression failures, and cross-scene clipboard ID remapping.
- Graph validation and compiler IR.
- ECS structural barriers, query access declarations, component relocation, and deterministic system ordering.
- StateTree compilation, direct transitions, bounded jump loops, instance-data layout, tracing, and cooked-data rejection.
- Navmesh cook determinism, path queries, tile streaming, cancellation, and coordinate adapters.
- Audio null-backend behavior, command-queue limits, asset streaming, Steam Audio buffer and coordinate adapters, acoustics publication, and real-time thread constraints.
- Package index validation, zstd bounds, VFS mount precedence, embedded-package discovery, and mod compatibility.
- Package signature verification, unknown-key and tamper rejection, unsigned-mod trust policy, and reproducibility with signing disabled.
- Fixed-step accumulation and physics-event buffering.
- Project-template validation, safe substitution, cancellation cleanup, and no-overwrite behavior.
- Game-module API/build rejection, quiescence, successful state transfer, and rollback after candidate failure.
- Script binding validation, cooked-program version rejection, resource-budget enforcement, cancellation, sandbox denial, and hot-reload rollback.
- Locale canonicalization and fallback, plural/select arguments, stale translations, XLIFF rejection, deterministic catalog cooking, pseudolocalization, BiDi, shaping, font fallback, and grapheme-aware editing.
- Network schema negotiation, snapshot/delta validation, prediction and reconciliation, relevancy, rate limits, reconnects, and multi-process behavior under simulated loss, latency, jitter, reordering, and duplication.
- Plugin dependency cycles, safe-mode startup, registration teardown, task quiescence, automation revision checks, capability denial, and MCP cancellation.

Add slower categories as their systems arrive:

- Golden glTF fixtures, Blender-unavailable tests, and conditional `.blend` fixtures on machines with Blender installed.
- Headless Vulkan/NVRHI smoke tests where CI provides a supported driver.
- Swapchain resize, minimize, restore, and device-loss tests.
- TAA still-detail, motion, disocclusion, foliage, thin-geometry, specular, particle, camera-cut, and history-reset render regressions.
- Pure custom-titlebar layout tests covering all hit regions, DPI scaling, Wayland logical coordinates, ImGui capture priority, maximize state, and per-viewport state.
- Pure workspace policy tests covering first-run window placement, toolbar alignment, panel transparency, and default docking preservation.
- Pure Output Log tests covering incremental snapshots, filtering, tail ownership, command-prefix matching, continuous selection ordering, UTF-8-safe copy ranges, and clear or truncation invalidation.
- Windows resource checks for the named `GLFW_ICON`, plus Win32 title-bar and system-menu integration.
- Linux X11 and Wayland build and smoke coverage, including floating ImGui content overlapping the title bar.
- Live move, resize, maximize, restore, and minimize tests proving continuous redraw and event-driven idle behavior.

GLFW API-only tests define `GLFW_INCLUDE_NONE` before including `glfw3.h`. They must not acquire an undeclared OpenGL-header dependency merely because GLFW can include client API headers by default.

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
- Verify signed package indexes before mount and never treat unsigned, user-approved mods as publisher-authenticated content.
- Treat network packets, handshakes, remote arguments, decompression, rates, and client authority as hostile input.
- Treat fonts, localization catalogs, and XLIFF as untrusted input. Disable XML entities and external resources and bound shaping, fallback, and message recursion.
- Treat project and mod scripts as untrusted input. Enforce capability allowlists, execution and memory budgets, cancellation, and disabled filesystem and native-library access before enabling behavior mods.
- Run complex importers in worker processes with timeouts and cancellation.
- Disable embedded Blender script auto-execution.
- Validate sizes, counts, offsets, recursion depth, and decompression limits before allocation.
- Keep native binary mods disabled until Herta defines and deliberately enables a versioned execution boundary.
- Treat native plugins as trusted unrestricted code, require explicit target compatibility, and provide editor safe mode after startup failure.
- Keep MCP local and read-only by default. Authenticate IPC, scope paths and capabilities, confirm sensitive actions, rate-limit requests, and retain an audit trail.
- Sanitize terminal control sequences and terminate owned process trees without routing structured build or source-control operations through a shell.
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
- Add Herta logging with `spdlog` as a compiled private backend, structured console and rotating-file sinks, deterministic flushing, test capture, and a bounded cursor-readable editor sink.
- Add the Herta Tasks contract, run the enkiTS backend spike, and either adopt a pinned revision privately or document the selected implementation.
- Add bounded CPU-worker and blocking-IO lanes, task scopes, cancellation, progress, main-thread continuations, profiling names, and deterministic task tests.
- Create windows, event pumping, input state, and capability reporting.
- Integrate native-titlebar-disabled custom title bars on Win32, X11, and Wayland while retaining native resize and window-manager behavior.
- Add the pinned NVRHI dependency, create the Vulkan instance, device, surface, and swapchain, and expose the minimum Herta RHI presentation path required by ToolUI. Do not add a scene renderer.
- Add ToolUI and the Dear ImGui docking and multi-viewport editor shell.
- Add the EditorFramework Output Log with whole-line category colors, fixed warning and error colors, search and level filtering, tail-aware auto-scroll, continuous multi-line text selection, copy, command history, and command completion through the shared EditorCore registry.
- Add the `EditorCore`, `HertaEditor`, and `HertaEditorCmd` composition boundaries. The headless target starts and stops without GLFW, ImGui, presentation RHI, Renderer, or an audio device.
- Apply the centralized EditorStyle baseline: Roboto Regular and Medium through FreeType, required license metadata, desaturated non-black surfaces, the configurable Cobalt background gradient, additive interaction tinting, transparent-panel modes, white docking previews, and the shared 36 px title-bar geometry.
- Draw title-bar controls with ImGui primitives, compile the Windows `GLFW_ICON` application resource, and add Linux desktop-icon source assets. Defer LunaSVG and the production icon set until an SVG-backed editor tool needs them.
- Add pure title-bar and workspace policy tests plus regression coverage proving that floating ImGui panels and popups consume input instead of dragging the native window.
- Add first-run 80 percent work-area placement, saved-layout preservation, event-driven minimized waiting, and guarded refresh rendering during native live resize.

Exit condition: the interactive editor opens with the documented visual baseline, preserves saved docking, creates platform viewports, preserves ImGui input priority over title-bar dragging, redraws continuously during native resize, idles without spinning while minimized, and closes cleanly on both platforms. Windows builds expose the configured application icon. The Output Log remains responsive under concurrent producers, preserves tail ownership, supports continuous text selection, and executes the same registered commands as the headless editor. The headless editor host starts, reports structured diagnostics, and exits cleanly on both platforms without a display server. Task scopes cancel and drain safely, blocking work cannot starve CPU workers, and synthetic background work does not stall event pumping.

### Milestone 2 - RHI and renderer (complete)

- Expand the private NVRHI Vulkan backend and Herta RHI handles and descriptors beyond the MS1 presentation subset.
- Add RenderGraph, frame contexts, upload staging, and fence-based retirement.
- Add Slang worker compilation and cooked shader assets.
- Render a validation-clean textured mesh with reversed-Z.

Exit condition: render tests and resize/minimize stress runs produce no validation errors or leaked GPU objects.

Implemented: typed RHI resource ownership, three submission-retired graphics contexts, bounded uploads, deterministic single-queue RenderGraph, Slang worker cooking with reflected metadata, and an sRGB textured cube rendered with reversed-Z in the editor Viewport. GPU readback tests cover coverage, depth occlusion, invalid requests, cancellation, and resource retirement. See [Rendering.md](Rendering.md) for the current contracts and verification commands. Interactive shader hot reload, physical transient aliasing, and multi-queue scheduling are later extensions.

### Milestone 2.5 - Viewport camera and gizmos

- Add an editor-owned fly, orbit, pan, and dolly camera with focus, adjustable movement speed, and mouse sensitivity.
- Add pinned im3d privately to EditorFramework for preview-object translation, rotation, and scale gizmos, local/world modes, and snapping.
- Render Herta-owned debug primitives through RHI and RenderGraph, including a depth-tested grid, axes, and bounds plus overlay gizmos.
- Keep camera, gizmo, and ImGui input ownership exclusive across docking, detached viewports, focus changes, and DPI scales.
- Keep the preview transform separate from later entity selection, transactions, undo/redo, and scene persistence in Milestone 4.

Exit condition: the preview object can be inspected and transformed interactively, picking matches the displayed camera, and debug primitives respect reversed-Z and configured depth behavior. Camera math and debug rendering have regression coverage; native resize and viewport input remain responsive.

### Milestone 3 - Asset pipeline

- Add stable asset IDs, registry, build keys, and DerivedDataCache.
- Add fastgltf, mesh cooking, and texture cooking foundations.
- Add optional isolated `.blend` import through a discovered system Blender installation.
- Add file watching, background Blender live reimport, and atomic editor asset generation swaps.
- Add asynchronous drag-and-drop batch import through the same AssetPipeline commands used headlessly.
- Add immutable AssetRegistry snapshots, incremental indexing, and cancellable deterministic fuzzy search.
- Expose validation, import, and reimport commands through `HertaEditorCmd` without initializing interactive editor systems.

Exit condition: Herta operates normally without Blender. When Blender is installed, saving a tracked `.blend` updates dependent editor instances without blocking the UI, the same source, Blender version, and settings produce identical cooked hashes, and failed reimport preserves the previous asset.

### Milestone 4 - World and editor authoring

- Add the Herta ECS contracts, entities, components, hierarchy, scene save/load, and version migration.
- Run the EnTT storage and scheduling spike, record results, and either adopt its pinned revision privately or document why another implementation is required.
- Add deferred structural barriers, explicit query read/write access, buffered events, and deterministic system ordering.
- Add explicit Herta runtime descriptors needed by inspectors and serialization, without requiring C++26 reflection.
- Add stable `FTextId` serialization and localizable descriptor metadata before scene and graph formats settle.
- Add `.hertaproject` loading, transactional project creation from the minimal Game template, and matching headless commands.
- Add editor-only game-module hot reload with build/API validation, shadow copies, safe-point quiescence, rollback, and an explicit restart result for incompatible state.
- Add deterministic UTF-8 scene and prefab serialization, transactions, property editing, selection, gizmos, and play-in-editor lifecycle.
- Add numeric property expressions and transactional cross-scene entity copy/paste with stable-reference remapping.
- Define source-plugin descriptors, extension registries, and semantic EditorCore automation commands without promising dynamic unload or a stable binary ABI.

Exit condition: scenes round-trip in canonical mergeable text, undo/redo and cross-scene paste are reliable, ECS mutation and query rules pass focused and scale tests, a generated Game project builds, and compatible gameplay-system changes reload without losing engine-owned state. Incompatible native state produces an actionable restart requirement, and runtime remains independent of editor modules.

### Milestone 5 - Localization and typography

- Add Localization, TextLayout, LocalizationPipeline, and LocalizationEditor boundaries.
- Add pinned ICU4C with filtered CLDR data and stable MessageFormat support behind Herta APIs.
- Add canonical UTF-8 source and translation catalogs, generated `constexpr` text IDs, deterministic fallback, hot reload, pseudolocalization, and per-culture cooking.
- Add validated XLIFF 2.1 import and export through `HertaEditorCmd` without interactive editor dependencies.
- Add project font fallback, ICU BiDi and break analysis, HarfBuzz shaping, FreeType rasterization, grapheme-aware editing, and asynchronous GPU-atlas publication.

Exit condition: Polish plural and formatting fixtures, forced RTL, mixed Arabic and Latin, Indic shaping, CJK fallback, combining marks, emoji sequences, and grapheme editing behave consistently on Windows and Linux. Catalogs cook deterministically, invalid translation input preserves the last valid generation, and selected language packs mount through the VFS.

### Milestone 6 - Production raster renderer

- Add PBR materials, image-based lighting, direct lights, shadow maps, motion vectors, depth hierarchy, exposure, and tone mapping.
- Add GTAO and crisp native-resolution TAA with correct history rejection, reactive masks, camera-cut resets, render regressions, and configurable modest sharpening.
- Add the Herta temporal-upscaler contract and FSR as the first optional Vulkan adapter while retaining native TAA and non-temporal fallbacks.
- Add GPU timings, feature capability reporting, quality tiers, and validation for multi-viewport editor rendering.
- Keep lightmap authoring, baking, lightmap UVs, and baked-light data out of the asset pipeline.

Exit condition: representative scenes render without baked lighting across the declared low, medium, and high raster tiers. TAA remains stable and crisp under motion, disocclusion, foliage, particles, thin geometry, specular highlights, and camera cuts, and every optional feature has a tested fallback.

### Milestone 7 - Physics

- Add Jolt adapters, collision layers, shapes, bodies, fixed-step simulation, and debug draw.
- Add origin-relative physics transforms and buffered events.

Exit condition: physics tests are repeatable for the supported configuration and world mutation never occurs inside callbacks.

### Milestone 8 - Multiplayer

- Add Networking and Replication with the selected GameNetworkingSockets transport revision behind Herta APIs.
- Add `HertaServer`, direct-IP dedicated servers, and player-hosted listen servers using the same authoritative simulation and replication path.
- Add versioned handshakes and wire schemas, stable network entity IDs, input commands, snapshots and deltas, relevancy, bandwidth budgets, interpolation, prediction, and reconciliation.
- Add loopback and multi-process tests with simulated loss, latency, jitter, reordering, duplication, bandwidth limits, reconnects, incompatibility, and hostile input.

Exit condition: dedicated and player-hosted sessions produce the same authoritative gameplay behavior, local listen-server clients do not bypass replication or permission checks, prediction reconciles under simulated adverse networks, and headless servers run without window, renderer, ImGui, or audio-device dependencies.

### Milestone 9 - Navigation and AI

- Add Recast offline tiled-navmesh cooking, Detour runtime queries and tile streaming, and initial DetourCrowd integration.
- Add asynchronous path requests, cancellation, dynamic-obstacle handling, debug draw, and deterministic fixtures.
- Add AI perception, working memory, scheduling, gameplay-task contracts, and ECS integration.
- Add GraphCore, StateTreeCompiler, and a nested StateTree authoring view for the first narrow graph domain without requiring a free-form node canvas.
- Add the first-party contiguous StateTree runtime program with direct index jumps, pooled instance data, utility selection, tracing, and bounded transitions.

Exit condition: agents navigate a streamed test level, avoidance behaves consistently, and authored StateTrees compile and produce matching headless execution traces after reload.

### Milestone 10 - Animation

- Add skeleton and clip import through ozz offline tools.
- Add sampling, blending, skinning, root motion, and animation events.
- Add an initial Herta-owned animation state machine using explicit cooked data. It is not the NPC StateTree runtime and does not become a GraphCore authoring domain yet.

Exit condition: a cooked skinned asset animates identically after reload and has no runtime dependency on offline ozz tools.

### Milestone 11 - Audio

- Add miniaudio behind Herta Audio with real and null device paths.
- Add cooked sound assets, voices, buses, streaming, spatial playback, concurrency policy, and real-time-safe command queues.
- Add audio diagnostics and deterministic headless tests without requiring an audio device.
- Run a focused Steam Audio spike covering Windows and Linux builds, miniaudio buffer integration, HRTF quality, coordinate conversion, thread ownership, source-count scaling, simulation update rates, dynamic geometry, and fallback behavior.
- If the spike passes, add optional HRTF, occlusion, transmission, and bounded real-time acoustics through the private SteamAudio integration. Defer acoustic probe baking, Embree, and custom renderer ray tracing until measured requirements justify them.

Exit condition: static and streaming sounds play spatially on Windows and Linux, the callback remains real-time safe under stress, and headless gameplay produces the same audio commands through the null path. Steam Audio-enabled builds preserve those constraints and disabled builds contain no Steam Audio code or runtime dependency.

### Milestone 12 - Graph domains

- Add the pinned Herta imgui-node-editor fork behind GraphEditor for the first domain that requires a free-form node canvas.
- Promote the animation state machine into a typed animation-graph domain and compiler.
- Extract only the graph authoring, typed-pin, transaction, validation, diagnostic, and migration infrastructure proven by StateTree and the animation graph.
- Keep Shader Graph, Audio Graph, and gameplay Blueprints as later distinct compiler targets rather than one universal executor.
- Extend debugging and cooked-format migration without linking editor UI into runtime executors.

Exit condition: StateTree and the animation graph compile headless and execute from cooked data without ImGui or imgui-node-editor linked, while sharing only proven GraphCore infrastructure.

### Milestone 13 - Scripting

- Run a focused Umka spike covering Windows and Linux integration, binding-call overhead, VM memory behavior, worker compilation, cooked-program persistence, diagnostics, hot reload, and hostile-script controls.
- Adopt a pinned upstream revision or narrow Herta fork only if the spike satisfies the documented production gates. Record the rejection and evaluate alternatives if it does not.
- Add optional `Scripting` composition, typed Herta binding registration from runtime descriptors, script assets, deterministic cooking, version checks, and source diagnostics.
- Add instruction, stack, heap, recursion, host-call, and per-update budgets with cancellation. Disable filesystem and native implementation libraries by default.
- Add editor breakpoints, stepping, stack frames, variables, profiling, and structured runtime errors without linking editor code into Shipping execution.
- Add transactional script hot reload with explicit persistent-state serialization, binding compatibility checks, rollback, and stable script function handles.
- Allow StateTree and graph domains to invoke validated script functions without compiling their own programs to Umka bytecode.

Exit condition: the same cooked script fixtures produce matching logical results on Windows and Linux, hostile fixtures are stopped by enforced limits, compatible state survives reload, failures preserve the last valid generation, and a scripting-disabled target contains no Umka code or script compiler.

### Milestone 14 - Cooking and distribution

- Add HertaCooker, versioned package indexes, dependency closure, zstd chunk compression, and platform deployment.
- Add Ed25519 package-index signing and verification through libsodium with keys kept outside source and player builds.
- Add loose, external-package, and executable-embedded output modes through the same VFS and package reader.
- Add deterministic base-culture and optional language-pack output through the same package system.
- Add deterministic data-mod discovery, manifests, dependency resolution, namespaces, override policy, and compatibility diagnostics.
- Add opt-in cooked script behavior mods with explicit capability profiles after all Milestone 13 sandbox gates pass. Keep native binary mods disabled.
- Add third-party notices, crash build IDs, and reproducible Shipping configuration.
- Add protected release tags, clean double-build verification, SBOM generation, artifact attestations, and immutable GitHub releases where available.

Exit condition: HertaGame runs from external or embedded cooked data without editor or developer modules, unsigned package output reproduces byte-for-byte, signed packages reject index or chunk tampering, language packs and compatible enabled mods mount deterministically without changing the base package, and disallowed script capabilities fail closed.

### Milestone 15 - Editor tooling and extensibility

- Complete project and engine plugin discovery, dependency diagnostics, restart-required enable and disable, first-party extension registries, and startup safe mode.
- Add the first-party Git source-control plugin using asynchronous Git CLI operations and credential helpers.
- Add the first-party terminal plugin through ConPTY and Linux PTYs without replacing structured build, cook, test, or source-control APIs.
- Add `HertaEditorMcp` over authenticated local IPC and semantic EditorCore automation with read-only defaults, explicit capabilities, transactions, progress, cancellation, and audit records.
- Add final translucent-panel backdrop blur, per-user appearance settings, performance scaling, and detached-viewport fallbacks.

Exit condition: an external source plugin extends the editor without private headers, Git and terminal work cannot stall the editor, automation operations match interactive and headless behavior, MCP mutation requires explicit authority and remains undoable where applicable, and plugin startup failure can be recovered through safe mode.

### Milestone 16 - Advanced dynamic rendering

- Add SSGI as an optional medium-tier dynamic-lighting enhancement.
- Add capability-driven Vulkan KHR ray queries and ray-tracing pipelines for selected effects without weakening the raster fallback.
- Run the Radiance Cascades 3D acceptance spike against path-traced references before deciding whether it becomes a shipping GI backend.
- Add optional DLSS and XeSS plugins through the temporal-upscaler contract. Defer frame generation until latency, frame pacing, UI composition, and swapchain requirements pass dedicated gates.
- Add atmosphere, volumetric fog, cloud shadows, and temporally reconstructed volumetric clouds with measured quality tiers.

Exit condition: advanced features pass capability, memory, performance, camera-cut, multi-view, dynamic-geometry, and render-regression gates; unsupported hardware retains the production raster path; and Herta still has no light-baking pipeline.

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

Milestones 2 and 2.5 are implemented, including viewport camera navigation and preview gizmos. The following Milestone 3 slice should remain limited to:

1. Add stable asset IDs, the asset registry, deterministic build keys, and DerivedDataCache.
2. Introduce fastgltf and canonical mesh and texture cooking behind the worker boundary.
3. Expose import and reimport through shared headless editor commands and asynchronous editor jobs.
4. Add optional system Blender discovery and isolated import without making Blender required.

Keep ECS, localization, networking, graph tooling, physics, animation, audio, and scripting in their later milestones.
