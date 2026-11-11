# Projects

[Editor usage](EditorGuide.md) | [Level contracts](Levels.md)

## Descriptor and runtime API

`Project` owns `.hertaproject` parsing, canonical serialization, path resolution, and transactional template creation. Its public API is `ParseProject`, `SerializeProject`, `LoadProject`, and `CreateProject`, returning `std::expected` with `FProjectError`. `FLoadedProject` contains the validated descriptor and resolved project, content, and starting-level paths; it does not load native code or instantiate a world.

Descriptors use UTF-8 JSON with `format: "HertaProject"`, `formatVersion: 1`, and `schemaVersion: 2`. They record a UUID, project name, engine association, native modules, editor targets, one `Game` content root, an empty feature list, and `startingLevel`. Loading currently requires `engineAssociation: "Herta"`. Unknown or duplicate fields, unsupported versions/features/dependencies, unsafe relative paths, and paths escaping the project through symbolic links are rejected. The descriptor limit is 1 MiB.

Legacy schema 1 descriptors remain readable with `startingScene`; their `Scene` module dependency migrates in memory to `Level`. Serialization always writes schema 2 with `startingLevel`. Mixed or cross-version starting-document fields are rejected. Parsing does not rewrite descriptors or rename referenced files, so an existing `.hscene` path stays unchanged until explicitly migrated. See [Level contracts](Levels.md#file-contract) for document canonicalization.

Modules live at `Source/<ModuleName>`, with `Public` headers and `Private` implementation files. Supported dependencies are `Core`, `Math`, `Assets`, and `Level`; names cannot collide with engine modules. Targets currently describe editor composition, not standalone game executables. Paths use forward slashes and remain relative to the descriptor directory.

`LoadProject` checks the content, level file, and module directories exist. Starting-level JSON is validated when the editor loads it; project creation validates the template level before publishing. `Herta --project=<descriptor>` opens the project; without this flag it opens `Games/Sandbox/Sandbox.hertaproject`. A plain `.hlevel` argument, such as a file dropped on the executable, opens that level in the project `FindOwningProject` locates, and a plain `.hertaproject` argument opens that project.

Projects live outside the engine: nothing under `Engine`, `Scripts`, `Build`, or `Templates` names one. The Sandbox sits in `Games/Sandbox` as an ordinary project with its own `.gitignore`, opened by path like any other. `Herta <project.hertaproject>` opens a project, and with no project the editor starts on a picker of recent projects, which it keeps per user. Asset commands in `HertaEditorCmd` without `--content-root` use the project the working directory is in. `PackageGame` and the visual and play tests take the project or level to use. The editor package carries the engine and the project template only.

## Game modules

A project's modules are game modules: shared libraries (`<Name>.dll`, `lib<Name>.so`) that the editor, `HertaGame`, and `HertaServer` load at startup. A module links no engine library, since its own copies of engine globals, such as the physics factory, would never be initialized. It includes `Herta/GameFramework/GameModule.h` and reaches the engine only through its interfaces, whose virtual calls run the host's code:

- `HERTA_GAME_MODULE(Register)` exports the entry point, with the interface version, engine revision, and configuration the module was built for. The host refuses a module that does not match all three, since it shares the host's types, heap, and inline code.
- `Register` receives an `IGameModuleRegistry` and adds systems by name and the table of C++ generated from the project's gameplay graphs.
- An `IGameSystem` runs every session update after the players and gameplay graphs, through an `IGameSystemContext` that reads each player, logs, and plays level sound and effect cues. Systems drive presentation and their own state, never the simulation, so they stay out of rollback snapshots.

The editor loads modules from the project's `Binaries/<platform>/x86_64/<configuration>`, matching its own configuration folder, through a copy in `Intermediate/HotReload`, so the build can replace the original while it runs. It checks each module's build once a second and reloads one that changed and has stood still for a second, between frames, where no system runs. Each running system saves its state, tagged with its version, and the new build's system of the same name loads it. When every system takes its state, the new build runs and the old one unloads; when one refuses, the old build keeps running, Play goes on unchanged, and the log says a restart is needed. `module.reload [name]` and the MCP tool `module_reload` reload on demand and need write authority. `game.stage` copies built modules beside the staged project and names any it could not find; `PackageGame` copies them beside the executables and refuses to package without them.

The Sandbox's `SandboxGame` module registers its gameplay graphs' generated C++ and `RunStats`, which calls out every 50 m the first player runs on foot with the jumps taken, with the objective chime every 200 m, and keeps its count across reloads. `SandboxTests` checks that the module loads and that its generated code matches the graphs.

A project's own checks live in `Source/Tests/Private` with a doctest main; generating with `--project` adds a `<Project>Tests` program that links the engine's runtime and developer modules and the project's modules, and runs from the project root. `SandboxTests` keeps the Sandbox's levels canonical, cooks every graph asset it authors, and keeps the Mannequin's animation graph equal to the built-in player machine. Engine tests read their own copies under `Engine/Source/Programs/HertaTests/Fixtures`, so they pass without any project.

## Creation and headless commands

`project.create <name> <module> <destination>` uses `Templates/Projects/Game` from the associated source checkout. The destination must not exist and its parent directory must already exist. Names use portable C++-style identifiers. Creation expands only whitelisted manifest substitutions in a sibling staging directory, validates the result, and atomically publishes without replacing existing data. Cancellation or failure removes only that staging directory. The C++ request exposes a `std::stop_token`.

The template supplies a descriptor, empty starting level, content directory, and one native module with a deferred entity-creation example. It does not generate a standalone game application. `project.validate <descriptor>` checks the same project-loading contract; `--json` must precede the command when machine-readable diagnostics are wanted.

Windows example, after building `HertaEditorCmd`:

```powershell
$EnginePath = "C:\Git\Herta"
$ProjectRoot = "C:\Git\MyGame"
& "$EnginePath\Binaries\windows\x86_64\Development\HertaEditorCmd.exe" project.create MyGame MyGameplay $ProjectRoot
& "$EnginePath\Binaries\windows\x86_64\Development\HertaEditorCmd.exe" --json project.validate "$ProjectRoot\MyGame.hertaproject"
& "$EnginePath\Binaries\windows\x86_64\Development\Herta.exe" "--project=$ProjectRoot\MyGame.hertaproject"
```

Linux uses `Binaries/linux/x86_64/Development/HertaEditorCmd` with the same positional command arguments, and `Herta --project=/path/to/MyGame/MyGame.hertaproject`.

## Building the generated native module

Run the associated engine checkout's Setup first. Use its installed, pinned Premake and explicitly select both the engine script and project descriptor. Project descriptors and templates are data, not executable build scripts.

```powershell
$env:HERTA_VULKAN_SDK = "$EnginePath\SDK\Windows\Vulkan\1.4.363.0"
& "$EnginePath\External\Premake\Windows\5.0.0-beta8\premake5.exe" "--file=$EnginePath\premake5.lua" "--project=$ProjectRoot\MyGame.hertaproject" vs2026
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "$ProjectRoot\Intermediate\ProjectFiles\vs2026\MyGameplay\MyGameplay.vcxproj" /m /p:Configuration=Development /p:Platform=x64
```

Use the MSBuild executable from the installed Visual Studio. `vs2022` selects the matching older workspace instead. Windows solutions are placed at the project root; module projects are under `Intermediate/ProjectFiles/<action>/<ModuleName>`.

```sh
env HERTA_VULKAN_SDK=/path/to/Herta/SDK/Linux/Vulkan/1.4.363.0 /path/to/Herta/External/Premake/Linux/5.0.0-beta8/premake5 --file=/path/to/Herta/premake5.lua --project=/path/to/MyGame/MyGame.hertaproject gmake
make --directory=/path/to/MyGame/Intermediate/ProjectFiles/gmake --jobs=2 config=development MyGameplay
```

The module is a game module: `Binaries/windows/x86_64/Development/MyGameplay.dll` or `Binaries/linux/x86_64/Development/libMyGameplay.so` under the project root; see [Game modules](#game-modules). Its objects are under project `Intermediate/Build`; engine outputs remain under the engine checkout. Generation records the associated engine revision in project `Intermediate/HertaEngineBuild.json`. Rebuild after switching engine revisions; the host refuses modules built for another one. `HertaGame` plays a staged project; see [Game runtime](Game.md).
