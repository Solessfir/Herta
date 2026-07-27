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
- Put engine code in the `Herta` namespace.
- Use Unreal-style names where they carry engine meaning: `F` for value types, `I` for interfaces, `E` for enums, and `b` for booleans.
- Reserve `U` and `A` for future reflected object and actor semantics.
- Prefer RAII, explicit ownership, `std::unique_ptr`, `std::span`, `std::string_view`, concepts, ranges, and `std::expected`.
- Use standard containers and strings until Herta has a measured semantic reason to own alternatives.
- Do not throw exceptions across module boundaries.
- Keep third-party types behind Herta-owned adapters and out of public APIs.
- Do not use reinterpret casts to bridge Herta and third-party math types.
- Keep headers self-contained and minimize their dependencies.

Comments should read like one developer explaining a non-obvious decision to another. Explain why a constraint, engine quirk, or lifetime rule exists instead of narrating what the code already shows. Do not add comments for obvious behavior or generate documentation-style comments for every declaration. Keep comments concise and naturally formatted, without abrupt wrapping or long blocks. If a comment becomes long, first consider whether the code can express the decision more clearly.

## Modules and dependencies

- Add dependencies only when the current milestone requires them.
- Source dependencies belong under `External` as pinned Git submodules.
- Downloaded tools and SDKs belong under ignored, versioned `SDK/<platform>/<tool>/<version>` paths.
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
- Treat submodule revisions as intentional dependency locks.
- Never use destructive Git or filesystem commands to discard user work without explicit authorization.

## Current repository state

Milestone 0 is complete and Milestone 1 is next. The repository contains a real Premake workspace, Setup and project-generation scripts, Core, Math, Platform, HertaTests, and required CI and quality workflows. The Windows and Linux build and test paths are locally verified, and both remain required CI gates. The application shell, Vulkan integration, editor, and game runtime have not started.

Update this section when a milestone changes those facts.
