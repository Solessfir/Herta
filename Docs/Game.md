# Game runtime

`HertaGame` plays a project's starting level as a standalone game. It composes Application, the NVRHI/Vulkan presenter, the Renderer, GameFramework, and GameUI, and links no editor, ToolUI, or developer module. Editor **Play** runs the same `FGameSession`, so the standalone game and the editor share one gameplay path.

## Modules

- **GameFramework** (runtime): `FGameSession` owns one playthrough on top of copied authored entities. It builds physics through `BuildLevelSimulation`, the same builder the editor's Simulate uses, adds a Jolt `CharacterVirtual` player inside the 60 Hz fixed step, tracks the course objective, and extracts an `FMeshRenderView` with interpolated simulation poses, soft body vertices, lights, atmosphere, and fog. `IGameAssets` supplies render resources: `FStagedGameAssets` loads them from staged content, and the editor shares its loaded assets. `FLevelSimulation`, soft body geometry, and environment filtering moved here from EditorFramework.
- **GameHud** (GameFramework): `DrawGameHud` draws the objective, crosshair, controls hint, and pause menu with GameUI. The host supplies its menu items and acts on the chosen one, so the standalone game and editor Play share one HUD.
- **GameUI** (runtime): immediate-mode screen UI with a FreeType glyph atlas, anchored text, rectangles, images, and a menu whose focus is shared by keys and the pointer. It is independent of ImGui and ToolUI. Layout units are pixels on a 1440-pixel-tall screen, and the atlas is rebuilt at the screen's size when it changes by more than 5 percent, so text stays sharp. The presenter draws its screen lists through the ToolUI pipeline, which also encodes them for HDR10 targets.
- **Assets**: `FCookedContent` mounts staged directories in order and loads one cooked file per asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space. Loading never imports or cooks.

## Player and objective

The player moves like Faith in Mirror's Edge. `FCharacterMover` in GameFramework follows the game's walking, falling, slide, and wall run rules from its scripts and native code, with the game's own numbers. The capsule is 1.8 m tall with a 0.3 m radius and steps up 0.35 m; eyes sit 0.14 m under its top, at 1.66 m standing and 1.08 m crouched.

- **Running**: holding forward runs at the 4 m/s base speed; holding Shift as well sprints at 7.2 m/s, reached in under half a second, as in most games rather than Mirror's Edge's seven-second build-up. Strafing and backing up move at the base speed, letting go brakes to a stop in under half a second, and a tap that lets go within 0.15 s stops dead. Crouched, the 1.22 m capsule walks at 1.44 m/s.
- **Jumping**: the jump launches at 6.3 m/s under the player's 16.4 m/s² gravity, lifting the feet about 1.2 m, and adds 1 m/s forward when already moving that way. Landing a plain jump caps the speed at the take-off speed less 0.65 m/s, so hopping costs speed. Air control is the game's: 1.5 m/s² toward the input, so a jump goes where the run sent it. Jumps are buffered for 0.15 s and allowed for 0.12 s after running off a ledge. Moving platforms carry the player, and leaving one keeps its motion.
- **Sliding**: pressing crouch at 3.5 m/s or more along the view slides. Move input is ignored; the slide brakes at a tenth of ground friction, so from a full sprint it lasts about 1.3 s, turns toward the view at 0.2 of the angle per second and 11 degrees per second with strafing, and ends below 2.5 m/s, on pulling back, or on letting go of crouch after the first 0.5 s. However it ends, it halves the speed. Jump does nothing during a slide. The slide fits under a 1.3 m bar; standing back up waits until the full capsule fits.
- **Wall running**: running at a wall in the air at 2 m/s or more, facing at most 57 degrees into it, with forward held, runs along it when the wall is at least 1.92 m tall. The search reaches 0.5 m ahead at run speed and 0.9 m at full sprint, or to the side when strafing toward a wall right after jumping; falling, it refuses when ground lies just under the run's path. Rising, the run lifts the player to 1.7 m above the jump under its own 8.2 m/s² gravity, then falls at 5 m/s², drags 5 percent of the speed along the wall per second, and ends on landing, leaving the wall, crouching, pulling back, or falling faster than 5 m/s. Jumping off pushes out 1.2 m/s and up 1 m when looking along the wall; looking away from it pushes up to 5.2 m/s out and 1.6 m up but keeps as little as a tenth of the speed along the wall. A run straight out of another's jump is chained: no lift, and twice the pull, three times for the next. A wall that was run cannot be run again before landing. The body leans 0.35 rad away from the wall and the view tilts 12 degrees with it.
- **Wall kick and mantle** are Herta's own: jumping while touching a wall in the air kicks 5.5 m/s away from it while rising like a jump, at most every 0.2 s, so alternating between the 2.5 m chimney walls climbs it. Moving forward into a wall in the air mantles onto its top when the top is 0.5 to 1.9 m above the feet: a 0.1 m sphere swept down 0.3 m past the capsule finds the top, and two capsule-wide sweeps check that the player fits there and that nothing overhangs the way up. The climb moves the capsule along a straight line to the top over the clip's 0.67 s, matching the clip's root. Mirror's Edge's vaults will replace the mantle.

That matches the Sandbox gauges: the player walks up 0.18 m steps, steps up 0.35 m without jumping, jumps the 0.5 m steps, the 0.6 m and 1.3 m hurdles, and a 2 m gap, slides under the 1.3 m bar, and mantles out of the wall run lane's 1.8 m pit.

The mover runs movement modes in priority order: custom modes first, then the mantle, wall run, wall kick, and slide, with walking and falling underneath. Each mode decides whether it takes over from the active one, then moves the capsule for the fixed step through shared helpers, and hands back to walking or falling when it ends; all of them read and write one `FMoverState`, so speed, timers, and the last jump carry across. `FGamePlayerSettings::Modes` turns built-in modes off, and `CustomModes` adds `IMovementMode` implementations, as Unreal's Mover adds movement modes. `FMovementTuning` holds every number, named after the game's fields.

A Trigger's **Role** places the course: the player spawns at the bottom of the **Start** zone, facing its forward axis, and the clock runs from leaving it until the body's center enters a **Finish** zone. Walking back into Start resets the run, R restarts the course, and falling below -30 m returns to the start. The best time lasts for the session. A level without both zones is played as free exploration.

## Controls

WASD moves, Shift sprints, the mouse looks, Space jumps, jumps off wall runs, and kicks off walls, C or Ctrl crouches and slides, F or the left mouse button throws a ball, V switches between the third-person camera, where Play starts, and the first-person one, R restarts, Esc opens the pause menu, and F11 or Alt+Enter toggles borderless fullscreen on the monitor under the window. The pause menu resumes, restarts the course, toggles fullscreen, toggles HDR10 when the display offers it, and cycles the Low, Medium, and High rendering quality tiers. Losing focus pauses the game and releases the pointer.

Gamepads in the standard layout work alongside them, in the game and in editor Play: XInput pads on Windows, and pads in GLFW's built-in SDL mapping database elsewhere, the first one connected. The left stick moves at its own speed, the right stick looks at up to about 200 degrees a second across and 140 up and down, A or cross jumps, clicking the left stick toggles sprinting, which also ends when the stick returns to rest, B, circle, or clicking the right stick crouches, X or square throws, Y or triangle switches the camera, Back restarts, and Start opens the pause menu, where the D-pad moves, A chooses, and B resumes. Sticks ignore 15% of drift around centre, and look grows with the square of the push past it, so small corrections stay fine. `FGameGamepadControls` in GameFramework maps them, and `FApplication::GetGamepadState` reads the pad.

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

Staging replaces only a directory that is empty or was staged before. A material with a custom shader or Material Graph has its twelve shading, depth prepass, bake, and shadow stages compiled through `HertaShaderWorker` without debug info to `Shaders/<hash>.<stage>.hshader` under the shader's mount, named by a hash of the shader key so deep output directories stay within path limits, with its parameter table beside them as `<hash>.parameters`. The game publishes them before play, so it links neither the shader compiler nor MaterialGraph.

`HertaGame --project=<path>` runs a staged project; without it, the game opens the one project folder next to its executable. Package a relocatable build after building Shipping:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Windows/PackageGame.ps1 -Verify
```

```sh
./Scripts/Linux/PackageGame.sh --verify
```

The package holds the renamed game executable (`<Project>.exe` on Windows, `<Project>.x86_64` on Linux beside the `<Project>` content folder), the cooked engine shaders, the staged content, `LICENSE`, `ThirdPartyLicenses`, and a README, under `Intermediate/Packages/<Project>-<platform>-x86_64-<Configuration>`. Shipping Windows builds have no console. `-Verify` copies the package outside the checkout and plays a scripted run there.

## Checks

- `HertaGame --smoke-test=<frames> [--validation]` plays a scripted run: it walks and jumps, loses focus, pauses and resumes, restarts the course, and exits with failure on any error or validation message.
- `HertaEditor --play-test [--validation]` runs two editor Play/Stop cycles with scripted input, saves `Saved/PlayTest/Play-N.png` from the player's view, and requires every authored model back after each stop.
- `HertaTests` cover the mover (the sprint speed curve, landing speed loss, slide braking and exit, disabled and custom modes, sphere casts) and the session (spawn, timing, restart, jump limits, respawn, sliding under a low ceiling, wall kicks, the chimney climb, mantles and their room check, wall runs from a run but not a standing jump, invalid input), staged content loading, trigger roles, GameUI layout and menu focus, and the shared simulation builder.

The 600-frame Sandbox smoke run averaged 3.4 ms per frame at the default window size on an RTX 4090 Laptop GPU, Development and Shipping alike. Native Linux runs on X11 and Wayland pass the same checks.
