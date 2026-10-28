# Game runtime

`HertaGame` plays a project's starting level as a standalone game. It composes Application, the NVRHI/Vulkan presenter, the Renderer, GameFramework, and GameUI, and links no editor, ToolUI, or developer module. Editor **Play** runs the same `FGameSession`, so the standalone game and the editor share one gameplay path.

## Modules

- **GameFramework** (runtime): `FGameSession` owns one playthrough on top of copied authored entities. It builds physics through `BuildLevelSimulation`, the same builder the editor's Simulate uses, adds a Jolt `CharacterVirtual` player inside the 60 Hz fixed step, tracks the course objective, and extracts an `FMeshRenderView` with interpolated simulation poses, soft body vertices, lights, atmosphere, and fog. `IGameAssets` supplies render resources: `FStagedGameAssets` loads them from staged content, and the editor shares its loaded assets. `FLevelSimulation`, soft body geometry, and environment filtering moved here from EditorFramework.
- **GameHud** (GameFramework): `DrawGameHud` draws the objective, crosshair, controls hint, and pause menu with GameUI. The host supplies its menu items and acts on the chosen one, so the standalone game and editor Play share one HUD.
- **GameUI** (runtime): immediate-mode screen UI with a FreeType glyph atlas, anchored text, rectangles, images, and a menu whose focus is shared by keys and the pointer. It is independent of ImGui and ToolUI. Layout units are pixels on a 1440-pixel-tall screen, and the atlas is rebuilt at the screen's size when it changes by more than 5 percent, so text stays sharp. The presenter draws its screen lists through the ToolUI pipeline, which also encodes them for HDR10 targets.
- **Assets**: `FCookedContent` mounts staged directories in order and loads one cooked file per asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space. Loading never imports or cooks.

## Player and objective

The player is a 1.8 m, 0.3 m radius capsule with eyes at 1.65 m. It walks at 4.5 m/s, sprints at 7 m/s, and jumps at 3.85 m/s, which lifts the feet about 0.75 m. That matches the Sandbox gauges: it walks up 0.18 m steps, steps up 0.35 m without jumping, jumps 0.5 m steps, the 0.6 m hurdle, and a 2 m gap, but not the 1.3 m hurdle. Jumps are buffered for 0.12 s and allowed for 0.1 s after walking off a ledge. Moving platforms carry the player, and leaving one keeps its motion. Walls and ceilings absorb blocked velocity. The player pushes dynamic bodies with up to 500 N, and an inner kinematic body lets those bodies collide back.

Holding C or Ctrl crouches into a 1 m capsule that fits under a 1.1 m bar, at 2.2 m/s. Crouching at 4 m/s or faster starts a slide, following Shiftrunner's port of Zippy: entering adds 4 m/s, up to 11.5 m/s and at most once every 0.75 s; slopes pull the slide downhill at 40 m/s² per unit of floor tilt; it loses 80 percent of its speed per second; input only steers sideways; and it ends at crouch speed. Standing back up waits until the full capsule fits. Jumping while touching a wall steeper than about 70 degrees in the air kicks 5.5 m/s away from it while rising like a normal jump, at most every 0.2 s, so alternating between the 2.5 m chimney walls climbs it. Air control only adds speed up to the walking or sprinting speed along the steering direction, as in Source and Quake, so slide and wall jump momentum carries through the air.

A Trigger's **Role** places the course: the player spawns at the bottom of the **Start** zone, facing its forward axis, and the clock runs from leaving it until the body's center enters a **Finish** zone. Walking back into Start resets the run, R restarts the course, and falling below -30 m returns to the start. The best time lasts for the session. A level without both zones is played as free exploration.

## Controls

WASD moves, the mouse looks, Space jumps and wall jumps, Shift sprints, C or Ctrl crouches and slides, R restarts, Esc opens the pause menu, and F11 or Alt+Enter toggles borderless fullscreen on the monitor under the window. The pause menu resumes, restarts the course, toggles fullscreen, toggles HDR10 when the display offers it, and cycles the Low, Medium, and High rendering quality tiers. Losing focus pauses the game and releases the pointer.

Settings live per user in `%LOCALAPPDATA%\Herta\<Project>\Settings.ini` on Windows and `$XDG_CONFIG_HOME/Herta/<Project>/Settings.ini` (or `~/.config`) on Linux: `fullscreen`, `hdr`, `paperWhite` and `peakLuminance` in nits (0 follows the operating system, as in the editor), `mouseSensitivity` in radians per pixel, and `quality` (0 Low, 1 Medium, 2 High, the default). Logs are written beside them in `Logs/Game.log`.

## Editor Play

Click **Play** in the viewport toolbar or press **Alt+P** or **Shift+Space**. Play waits until every mesh has loaded, then starts a session on a copy of the level; authoring, selection, and undo history are untouched, and editing is locked as during Simulate. The viewport hides editor overlays and draws the game's own GameUI HUD, the same `DrawGameHud` the standalone game uses, through ImGui with the same registered textures. Clicking the viewport captures the pointer. **Shift+Esc** is the game's Esc: it opens the pause menu (Resume, Restart course, Exit, which stops Play), as does leaving the editor window. Esc or the active Play button stops and returns to the authored level.

## Staging and packaging

`HertaEditorCmd game.stage <project.hertaproject> <output>` cooks the starting level's dependency closure through HertaAssetWorker: meshes, material assets and their textures in each sampled color space, and Sky environments. It writes them as loose files, copies the project descriptor and starting level verbatim, and copies the HUD font with its license:

```text
<output>/Engine/Content/<asset-id>.hasset         engine assets
<output>/Engine/Content/Fonts/Roboto-Medium.ttf   HUD font and OFL.txt
<output>/<Project>/<Project>.hertaproject
<output>/<Project>/<starting level>
<output>/<Project>/<content root>/<asset-id>.hasset
```

Staging replaces only a directory that is empty or was staged before. Materials with custom shaders are rejected for now, because their cooked shaders are not staged; no shipped project uses one yet.

`HertaGame --project=<path>` runs a staged project; without it, the game opens the one project folder next to its executable. Package a relocatable build after building Shipping:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Windows/PackageGame.ps1 -Verify
```

```sh
./Scripts/Linux/PackageGame.sh --verify
```

The package holds the renamed game executable, the cooked engine shaders, the staged content, `LICENSE`, `ThirdPartyLicenses`, and a README, under `Intermediate/Packages/<Project>-<platform>-x86_64-<Configuration>`. Shipping Windows builds have no console. `-Verify` copies the package outside the checkout and plays a scripted run there.

## Checks

- `HertaGame --smoke-test=<frames> [--validation]` plays a scripted run: it walks and jumps, loses focus, pauses and resumes, restarts the course, and exits with failure on any error or validation message.
- `HertaEditor --play-test [--validation]` runs two editor Play/Stop cycles with scripted input, saves `Saved/PlayTest/Play-N.png` from the player's view, and requires every authored model back after each stop.
- `HertaTests` cover the session (spawn, timing, restart, jump limits, respawn, sliding under a low ceiling, wall jumps, the chimney climb, invalid input), staged content loading, trigger roles, GameUI layout and menu focus, and the shared simulation builder.

The 600-frame Sandbox smoke run averaged 3.4 ms per frame at the default window size on an RTX 4090 Laptop GPU, Development and Shipping alike. Linux runs are listed in [LinuxTodo.md](LinuxTodo.md).
