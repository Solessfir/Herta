# Plugins

Editor plugins add panels, Tools menu items, console commands, and status bar text. A plugin is a folder with a `<Name>.hplugin` descriptor and, for one with code, a `Source` folder that builds into a shared library against `Herta/EditorCore/EditorPlugin.h` alone. Engine plugins live in `Engine/Plugins`; a project's live in its `Plugins` folder and build into its `Binaries`.

## Descriptor

```json
{
  "format": "HertaPlugin",
  "formatVersion": 1,
  "name": "Git",
  "friendlyName": "Git source control",
  "description": "Shown in Tools > Plugins.",
  "module": "HertaGit",
  "dependencies": [],
  "enabledByDefault": true
}
```

`name` must match the file name. `module` names the library without its platform prefix or extension and may be empty for a plugin without code. A project plugin with an engine plugin's name is ignored with a diagnostic rather than replacing it.

Premake creates one project per plugin with a `Source` folder when generating, so plugins build with the engine or the project. The editor depends on the engine's plugins, so building it builds them too.

## Loading, enabling, and safe mode

The editor discovers plugins at startup, resolves dependencies, and loads the enabled ones, dependencies first. A plugin whose dependency is missing, disabled, or part of a cycle stays unloaded, and Tools > Plugins, which opens as a tab beside the level, shows the reason beside it. Toggling a plugin there writes the choice to the per-user `Herta/Editor/Plugins.txt` in the user settings folder and takes effect at the next start; the panel says when a restart is pending. Plugins never unload while the editor runs, since what they registered is destroyed as late as the editor's last shutdown step.

Like game modules, a plugin links no engine library and must come from the same engine revision, configuration, and `EditorPluginInterfaceVersion`; the editor refuses others with a message to rebuild them. Plugins draw through `IEditorPluginUI`, whose calls run the editor's own widgets, so they never link ImGui or share its state. `IEditorPluginHost::RunProcess` runs programs on the editor's blocking-IO tasks and delivers the result on the main thread between frames.

`Herta --safe-mode` starts without plugins. The editor also writes `Herta/Editor/PluginStartup.marker` before loading plugins and removes it after a minute of running or a normal exit; if the marker is still there at the next start, that start skips plugins and says so, so a plugin that crashes the editor can be disabled in Tools > Plugins.

## Git

The Git plugin, enabled by default, adds a Source Control panel: branch and distance from upstream, changed files with staging and unstaging, commits of staged changes, and fast-forward pulls and pushes. It runs the `git` on `PATH` in the project's repository with `GIT_TERMINAL_PROMPT=0`, so Git never waits on a terminal; credential helpers such as Git Credential Manager still ask in their own windows. Commands run one at a time with 30 s timeouts, 120 s for pulls and pushes, and the editor kills them when it closes. `git.status` answers from the last status, and `git.refresh` reads it again in the background, so automation never waits for Git either.

## Terminal

The Terminal plugin, disabled by default, runs a shell in a dockable panel: PowerShell 7, Windows PowerShell, or `%ComSpec%` through ConPTY on Windows, and `$SHELL` through a pseudo terminal with `TERM=dumb` on Linux. A reader thread drains output and a writer thread feeds input, so a busy or stuck shell never holds a frame. Output is line-oriented: escape sequences are dropped, carriage returns, backspaces, and line erases are followed, and full-screen programs that address the cursor do not display correctly. Interrupt sends Ctrl+C; Windows passes an ignored Ctrl+C on to child processes, so an editor started that way, such as in the background from Git Bash, starts a shell that Interrupt cannot stop. Builds, cooks, tests, and source control keep their structured commands; the terminal is for everything else.

## Writing a plugin

```cpp
#include "Herta/EditorCore/EditorPlugin.h"

namespace
{
class FHelloPanel final : public Herta::IEditorPanel
{
public:
	void Draw(Herta::IEditorPluginUI& UI) override
	{
		UI.Text("Hello");
	}
};

void Register(Herta::IEditorPluginRegistry& Registry)
{
	Registry.AddPanel("Hello", std::make_unique<FHelloPanel>());
}
}

HERTA_EDITOR_PLUGIN(Register)
```

`Engine/Source/Programs/HertaTestPlugin` is the plugin `HertaTests` loads to check that one built from the public header registers its panel, menu item, and command.
