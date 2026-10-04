# Herta Agent Guide

## Scope

This file applies to the entire repository. A more specific `AGENTS.md` in a subdirectory may add or override rules for that subtree.

Read [README.md](README.md) and [Docs/EngineDesign.md](Docs/EngineDesign.md) before making architectural changes. The current user request takes precedence over repository guidance.

## Project direction

Herta is a C++23 game engine and editor for Windows and Linux. It is a learning project with a production-engine quality bar. Build systems in the correct order, but do not treat learning status as permission for disposable architecture.

Core conventions are fixed unless the user explicitly changes them:

- Right-handed Left-Up-Forward coordinates: +X left, +Y up, +Z forward.
- Meters, kilograms, and seconds.
- Radians internally and degrees only at editor-facing boundaries.
- Column vectors, column-major matrices, and `Translation * Rotation * Scale`.
- Vulkan through Herta RHI and NVRHI.
- GLFW through the Herta fork, including custom title bars on Win32, X11, and Wayland.
- Unreal-style C++ naming and module boundaries without copying Unreal's accumulated complexity.

Do not introduce project-configurable coordinate conventions or unit scales.

## Working method

- Inspect existing code, tests, documentation, and `git status` before editing.
- Follow current architecture and naming. Do not create a competing pattern without a concrete reason.
- Preserve user changes and unrelated dirty-worktree content.
- Prefer the simplest maintainable implementation. Avoid speculative abstractions and premature frameworks.
- Use reasonable judgment for the task size. Do not create extra projects, helper tools, or heavy workflows for a simple change.
- Before a large refactor, summarize the intended changes, migration impact, and main risks.
- When diagnosing a failure, gather ground truth with focused logging, assertions, and other instrumentation. If that is insufficient, use the appropriate debugger, validation layer, sanitizer, or profiler. Do not guess.
- Ask a concise clarifying question when missing information would materially change the design or risk user data.
- Delegate concrete, independent work to suitable sub-agents when it improves speed or review quality. Keep final integration and verification coherent.
- When work exposes an important missing rule, code-style improvement, or workflow safeguard, suggest a concrete addition to this file.

Do not preserve obsolete compatibility paths merely because an earlier unshipped prototype used them. Once a public serialized format or API exists, use explicit versioning and migrations.

## C++ rules

- Use C++23. Use newer language features only when supported consistently by the project's selected MSVC, Clang, and GCC toolchains.
- Prefer `constexpr` for pure operations and values that naturally support compile-time use. Use `consteval` when compile-time evaluation is required by the contract, not merely possible.
- Put engine code in the `Herta` namespace.
- Use Unreal-style names where they carry engine meaning: `F` for value types, `I` for interfaces, `E` for enums, and `b` for booleans.
- Reserve `U` and `A` for future reflected object and actor semantics.
- Prefer RAII, explicit ownership, `std::unique_ptr`, `std::span`, `std::string_view`, concepts, ranges, and `std::expected`.
- Use standard containers and strings until Herta has a measured semantic reason to own alternatives.
- Do not throw exceptions across module boundaries, and do not use them for control flow inside a module. Report expected failures with `std::expected`.
- Treat allocation failure as fatal. Do not catch `std::bad_alloc`, and do not add error codes, `std::expected` results, or "unavailable" states whose only purpose is reporting it.
- Use `try` and `catch` only where an exception can really arrive and must stop: native callbacks (GLFW, window procedures), thread and task entry points that run caller-supplied code, cross-module callbacks, process entry points, noexcept cleanup, and third-party or standard calls that throw on ordinary failures. Prefer non-throwing overloads, such as `std::filesystem` with `std::error_code` and `std::from_chars`, over wrapping throwing ones.
- Keep third-party types behind Herta-owned adapters and out of public APIs.
- Do not use reinterpret casts to bridge Herta and third-party math types.
- Keep headers self-contained and minimize their dependencies.
- Keep function declarations and definitions on one line, including their parameter lists. Multiline call sites are allowed when they make an operation easier to scan.
- Format C++ with the repository `.clang-format`. Lambda bodies indent from the enclosing statement, not from the call's opening parenthesis, and wrapped conditions start the continuation line with the operator.
- Use designated initializers when building a descriptor or request aggregate with more than one field, such as render graph accesses, pipeline and draw descriptors, and process requests. Positional initialization is fine for math values whose order is conventional, such as `FVector3{X, Y, Z}` and colors.
- Put a blank line after the closing brace of an `if`, `else`, `for`, `while`, `switch`, or `try` block unless the next line closes the enclosing scope or continues the statement (`else`, `catch`, or the `while` of a `do` loop).
- Put a blank line before a control statement unless the statement directly above produces the value it tests, as in `auto Result = Load(); if (!Result)`.
- Put a blank line before a statement whose lambda body spans several lines, unless the line above declares something the statement uses.
- Put a blank line after any statement that spans several lines and ends with a closing brace, such as `});` or a multi-line braced initializer ending in `};`, even when the next statement tests its result.
- Mark a function `[[nodiscard]]` only when ignoring its result is a bug, such as an error or an owning handle. Informational results such as clicked, changed, visible, or an optional handle do not qualify. If callers routinely discard the result, remove the attribute instead of casting to `void`. Cast to `void` only for a deliberate discard of a `[[nodiscard]]` result, never on calls that are not marked.
- Separate type and function definitions, including in-class constructor and function bodies, with a blank line. clang-format enforces this with `SeparateDefinitionBlocks`.
- End a braced list that spans several lines with a trailing comma, so each element stays on its own line and the closing `};` sits on its own line.
- Always brace the bodies of `if`, `else`, `for`, `while`, and `do`, even single statements. clang-format enforces this with `InsertBraces`.
- Write float literals without trailing zeros in the fraction: `1.f`, `0.5f`, `10.f`, not `1.0f`, `0.50f`, or `1`.
- A class that declares a destructor, copy operation, or move operation declares all five. RAII guards and owners usually delete copy and move. Clang-Tidy enforces this with `cppcoreguidelines-special-member-functions`.
- Mark member functions `const` when they leave the object's observable state unchanged. A pimpl method that mutates state through its implementation pointer stays non-const even when the compiler would accept `const`.
- Prefer `std::ranges` algorithms and whole-range overloads, such as `std::ranges::fill(Buffer, 0)`, over iterator-pair `std` algorithms. Clang-Tidy enforces this with `modernize-use-ranges`.
- In classes and structs, declare member functions first, in public, protected, then private order, and data members after them.
- Keep class bodies to declarations. Define member functions outside the class, in the source file, or below the class for `inline`, `constexpr`, and template code that must stay in a header. Only `= default` and `= delete` stay in the body.
- Group data members by concern, such as services, a panel's state, or gizmo settings, and separate the groups with a blank line. Keep members that depend on destruction order in that order and say why in a comment.
- Separate other logical steps inside a function body with a blank line, for example setup, the main work, and the final return.
- Mark non-mutated locals and implementation parameters `const` where it improves the contract. Public declarations may omit top-level `const` on by-value parameters because it does not affect callers.
- Prefer `std::print` and `std::println` for direct console output in bootstrap code. Engine diagnostics use Herta logging once it is available.
- Use `#ifdef` and `#ifndef` for simple macro-presence checks.

Comments should read like one developer explaining a non-obvious decision to another. Explain why a constraint, engine quirk, or lifetime rule exists instead of narrating what the code already shows. Do not add comments for obvious behavior or generate documentation-style comments for every declaration. Keep comments concise and naturally formatted, without abrupt wrapping or long blocks. If a comment becomes long, first consider whether the code can express the decision more clearly.

## Editor UI

- Build polished, production-ready UI by default, including early milestone features. New features should not look like debug controls or unfinished placeholders.
- Match the existing editor's visual language. Reuse shared widgets and icons; keep typography, spacing, alignment, and control sizes consistent across panels and menus.
- Design hover, active, selected, disabled, and keyboard-focus states deliberately. Keep shortcuts visible and related actions grouped without excessive headings or separators.
- Make layouts work at supported DPI scales and narrow panel sizes. Prefer restrained visual polish over decorative effects that reduce readability or responsiveness.
- Check visual changes against screenshots or live UI when available. Compilation alone does not verify appearance.

## Modules and dependencies

- Add dependencies only when the current milestone requires them.
- Source dependencies belong under `External`. Libraries distributed as one or two self-contained files, such as single-header and amalgamated releases, are vendored unmodified with their license and an `UPSTREAM.md` that records the source, exact revision, copied files, and update steps. Every other source dependency is a pinned Git submodule.
- Downloaded host tools belong under ignored, versioned `External/<tool>/<platform>/<version>` paths.
- Downloaded development SDKs belong under ignored, versioned `SDK/<platform>/<sdk>/<version>` paths.
- `Config/Dependencies.lock` is the source of truth for downloaded tool versions, URLs, SHA-256 hashes, and installed entry points.
- Normal project generation and builds must not perform network access. Only Setup and explicit dependency-update tooling may download files.
- Never resolve a moving `latest` release during normal Setup. Pin an exact version and checksum, then update the lock file through a reviewed change.
- Setup must check each required dependency and tool, automatically acquire the pinned project-local version when missing, and validate an existing installation before reuse.
- Setup must be idempotent, verify downloads before extraction, avoid partial installations, and emit actionable failures.
- Setup must remove downloaded installers and archives after a successful installation. Keep only the validated installed tool or SDK tree.
- Premake is bootstrapped locally by Setup. Do not commit Premake binaries.
- Setup downloads the pinned Vulkan development SDK into the repository when it is missing. GPU drivers and the production Vulkan loader remain platform responsibilities.
- When a required platform package cannot be installed locally, detect it and automate installation where safe and supported. Otherwise provide the exact actionable command instead of failing later during compilation.
- Blender is an optional system-wide authoring integration. Detect it, but never download or install it. Herta must build, test, run, cook existing assets, and import glTF without Blender.
- Blender's embedded Python is sufficient for Herta exporter scripts. Do not require a separate system Python installation for `.blend` import.

Keep a fresh clone self-contained: after running the supported Setup script, it should be ready to generate projects, build, and run with minimal manual configuration.

Cleanup scripts remove only explicit Herta-managed generated paths. Without Git metadata they must never guess whether arbitrary source-tree files are untracked.

## Tests and verification

- Implement doctest unit tests as part of new functionality and important logic, without waiting for a separate request.
- Lock down math conventions, serialization, asset conversion, stable IDs, and failure behavior with focused tests.
- Add regression tests with bug fixes when practical.
- Run the smallest relevant test or build first. Expand verification only when the affected boundary warrants it.
- For meaningful Windows C++ changes, run `Scripts/Windows/VerifyWindowsCompilers.ps1` to rebuild all projects and run tests with ClangCL and MSVC. Resolve all reported errors and warnings before declaring verification complete.
- Run Cppcheck on the Visual Studio project of each module touched by a meaningful C++ change, for example `& "C:\Program Files\Cppcheck\cppcheck.exe" --project=Intermediate/ProjectFiles/vs2026/Platform/Platform.vcxproj "--project-configuration=Development|x64" --enable=warning,performance,portability --inline-suppr --quiet --error-exitcode=1`. On Linux, use `cppcheck` from `PATH` with `--project=compile_commands.json`. Fix findings or suppress a false positive inline with `// cppcheck-suppress <id>` and a short reason. Cppcheck is optional; skip it and say so when it is not installed.
- Do not rebuild the entire engine for comments, documentation-only edits, or other non-code changes.
- Verify Windows and Linux implications for platform, build, filesystem, threading, and rendering changes.
- Do not ask the user to run checks that can be performed directly in the workspace or CI logs.
- Report exactly what was verified and what could not be verified.

## Documentation and writing

- Keep `README.md` at the repository root.
- Put project documentation under `Docs` using PascalCase filenames such as `EngineDesign.md` and `AssetPipeline.md`.
- Update README and relevant design documents when behavior, setup, architecture, or dependencies change.
- Assume the reader is an experienced Unreal Engine C++ programmer. Do not explain basic C++ or Unreal concepts unless asked or needed to understand a Herta-specific decision.
- Keep explanations brief and technical. Use analogies only when they improve understanding.
- Never use em dashes or en dashes. Use an ASCII hyphen or rewrite the sentence.
- Preserve exact path casing for Linux compatibility.

## Git and commits

- Do not commit, amend, push, rebase, or rewrite history unless the user explicitly requests it.
- Commit messages are short and natural, such as `Added math tests` or `Updated engine design`.
- Do not use `feat:`, `fix:`, `chore:`, or similar prefixes.
- Do not add `Co-authored-by` trailers.
- Preserve upstream copyright and license notices. Do not replace third-party authorship with Herta authorship.
- Treat submodule revisions and vendored revisions as intentional dependency locks. Never edit vendored files; update them only by copying a new upstream revision.
- Never use destructive Git or filesystem commands to discard user work without explicit authorization.

## Current repository state

Milestones 0 through 2.5 are implemented. The repository contains structured logging, Tasks, Application, RHI, NvrhiVulkan, RenderGraph, Renderer, ShaderCompiler, HertaShaderWorker, ToolUI, EditorCore, EditorFramework, HertaEditor, HertaEditorCmd, a configured application icon, focused tests, and required Windows/Linux CI and quality workflows. Slang cooks shaders during builds; the editor Viewport renders a textured cube with reversed-Z, camera navigation, private im3d transform gizmos, and depth-tested debug primitives. Camera math belongs to EditorCore; viewport transform adapters and input routing belong to EditorFramework. Local Windows, X11, and Wayland renderer readback, debug drawing, resize, and resource-retirement tests are verified. Pointer drags remain desktop-edge bounded. Milestone 3 - Asset pipeline is implemented. The Assets runtime module owns asset IDs, registry snapshots, and cooked texture and model formats. The AssetPipeline developer module owns `.hmeta` sidecars, content scanning and import, build keys, DerivedDataCache, the texture, glTF, and Blender cookers, and `asset.*` commands. Untrusted sources are only parsed inside `HertaAssetWorker`; editors and commands run it through `CookAssetInWorker`. The editor previews cooked models and textures through Details, loading them asynchronously from the `Engine` and `Game` content mounts. The preview cube and floor use the 1 m engine cube asset, and the Renderer owns no content. Content lives in `Games/Sandbox/Content`. `.blend` files import through a detected system Blender inside the worker. The editor polls content for live reimport, imports dropped files through `asset.import` on a background task, and filters the Static Mesh picker with `SearchAssets`. The game runtime has not started.

An early Physics preview slice uses Jolt behind Herta-owned box-body APIs. Simulate or Alt+S simulates all meshed Dynamic rigid bodies against all meshed Static rigid bodies and other dynamics. Rigid Body authors mass, friction, restitution, linear/angular damping, and gravity scale through undoable Details controls; scene schema 2 persists these settings and schema 1 loads with defaults. Escape stops simulation and restores every authored transform. It does not implement the full physics milestone.

Milestone 4 has started. The runtime Scene module owns `FWorld`, stable object UUIDs, world-scoped generational handles, hierarchy validation, deferred structural changes, atomic before/after authoring patches, and versioned canonical `.hscene` save/load, with EnTT v3.16.0 private to its implementation. Cube and Floor are authored Scene entities adapted to the editor's float viewport. Ctrl+S persists names, transforms, mesh choices, and rigid body components in `Games/Sandbox/Scenes/Sandbox.hscene`; simulation poses remain transient. EditorCore owns bounded transaction history; EditorScene records stable-ID changes and selection for grouped property gestures, create/duplicate/delete, component add/remove/edit, and canonical JSON clipboard operations. The editor opens flat scenes with empty and mesh entities. Details authors independently optional Static Mesh and Rigid Body components; empty entities use selectable viewport markers and never schedule asset loads. Runtime and headless validation support hierarchy, but hierarchy authoring, runtime descriptors, project loading, Content Browser authoring, and gameplay-system scheduling remain later slices.

Update this section when a milestone changes those facts.
