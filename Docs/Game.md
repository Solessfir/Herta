# Game runtime

`HertaGame` plays a project's starting level as a standalone game. It composes Application, the NVRHI/Vulkan presenter, the Renderer, GameFramework, and GameUI, and links no editor, ToolUI, or developer module. Editor **Play** runs the same `FGameSession`, so the standalone game and the editor share one gameplay path.

## Modules

- **GameFramework** (runtime): `FGameSession` owns one playthrough on top of copied authored entities. It builds physics through `BuildLevelSimulation`, the same builder the editor's Simulate uses, adds a Jolt `CharacterVirtual` player inside the 60 Hz fixed step, tracks the course objective, and extracts an `FMeshRenderView` with interpolated simulation poses, soft body vertices, lights, atmosphere, and fog. `IGameAssets` supplies render resources: `FStagedGameAssets` loads them from staged content, and the editor shares its loaded assets. `FLevelSimulation`, soft body geometry, and environment filtering moved here from EditorFramework.
- **GameHud** (GameFramework): `DrawGameHud` draws the controls as key caps in the top right corner, and the pause menu with GameUI. The host supplies its menu items and acts on the chosen one, so the standalone game and editor Play share one HUD.
- **GameUI** (runtime): immediate-mode screen UI with a FreeType glyph atlas, anchored text, rectangles, images, and a menu whose focus is shared by keys and the pointer. It is independent of ImGui and ToolUI. Layout units are pixels on a 1440-pixel-tall screen, and the atlas is rebuilt at the screen's size when it changes by more than 5 percent, so text stays sharp. The presenter draws its screen lists through the ToolUI pipeline, which also encodes them for HDR10 targets.
- **GameMenu and GameControls** (GameFramework): `FGameMenu` draws the pause, settings, and controls screens and routes focus; `FGameBindings` maps rebindable actions to keys, mouse buttons, and gamepad buttons. See [Menus, settings, and rebinding](#menus-settings-and-rebinding).
- **GameSave** (GameFramework): versioned save files with atomic writes and backup recovery; see [Saves](#saves).
- **Assets**: `FCookedContent` mounts staged directories in order and loads one cooked file per asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space. Loading never imports or cooks.

## Player and objective

The player moves like Faith in Mirror's Edge. `FCharacterMover` in GameFramework follows the game's walking, falling, slide, and wall run rules from its scripts and native code, with the game's own numbers. The capsule is 1.8 m tall with a 0.3 m radius and steps up 0.35 m; eyes sit 0.14 m under its top, at 1.66 m standing and 1.08 m crouched.

- **Running**: holding forward runs at the 4 m/s base speed; holding Shift as well sprints at 7.2 m/s, reached in under half a second, as in most games rather than Mirror's Edge's seven-second build-up. Strafing and backing up move at the base speed, letting go brakes to a stop in under half a second, and a tap that lets go within 0.15 s stops dead. Crouched, the 1.22 m capsule walks at 1.44 m/s.
- **Jumping**: the jump launches at 6.3 m/s under the player's 16.4 m/s² gravity, lifting the feet about 1.2 m, and adds 1 m/s forward when already moving that way. Landing a plain jump caps the speed at the take-off speed less 0.65 m/s, so hopping costs speed. Air control is the game's: 1.5 m/s² toward the input, so a jump goes where the run sent it. Jumps are buffered for 0.15 s and allowed for 0.12 s after running off a ledge. Moving platforms carry the player, and leaving one keeps its motion.
- **Sliding**: pressing crouch at 3.5 m/s or more in any direction slides the way the character is already going, sideways and backwards included, and crouch held through a landing or the end of a dash slides too. The slide brakes at a tenth of ground friction, so from a full sprint it lasts about 1.3 s and a dash carries it past the ground speed. Input across it turns it at 1.2 rad/s, the view draws it round at 0.2 of the angle per second, and it ends below 2.5 m/s, on pushing against it, or on letting go of crouch after the first 0.5 s, keeping 90 percent of the speed. Jumping from a slide keeps all of it. The slide fits under a 1.3 m bar; standing back up waits until the full capsule fits.
- **Dashing**: pressing sprint bursts at 14 m/s for 0.15 s toward the move input with gravity off, then leaves at 9 m/s. With no move input, a tap that lets go within 0.2 s dashes backwards. Dashing works from walking, falling, and sliding with a 0.4 s cooldown, once per trip through the air; the ground or a wall run gives it back. A dash chains into a slide, a jump, or a swing.
- **Wall running**: running at a wall in the air at 2 m/s or more, facing at most 57 degrees into it, with forward held, runs along it when the wall is at least 1.92 m tall. The search reaches 0.5 m ahead at run speed and 0.9 m at full sprint, or to the side when strafing toward a wall right after jumping; falling, it refuses when ground lies just under the run's path. Rising, the run lifts the player to 1.7 m above the jump under its own 8.2 m/s² gravity, then falls at 5 m/s², drags 5 percent of the speed along the wall per second, and ends on landing, leaving the wall, crouching, pulling back, or falling faster than 5 m/s. Jumping off pushes out 9 m/s and up 1 m when looking along the wall, enough to cross 6 m and start a run on the opposite wall, found to the side without looking at it; looking away from it pushes up to 10 m/s out and 1.6 m up but keeps as little as a tenth of the speed along the wall. A run straight out of another's jump is chained: no lift, and twice the pull. The sword swings during a wall run. A wall that was run cannot be run again before landing. The body leans 0.35 rad away from the wall and the view tilts 12 degrees with it.
- **Wall kick and mantle** are Herta's own: jumping while touching a wall in the air kicks 5.5 m/s away from it while rising like a jump, at most every 0.2 s, so alternating between the 2.5 m chimney walls climbs it. Moving forward into a wall in the air mantles onto its top when the top is 0.5 to 1.9 m above the feet: a 0.1 m sphere swept down 0.3 m past the capsule finds the top, and two capsule-wide sweeps check that the player fits there and that nothing overhangs the way up. The climb moves the capsule along a straight line to the top over the clip's 0.67 s, matching the clip's root, and settles onto it so walking takes over without a step of falling. A top under 0.5 m above the feet is vaulted instead: the feet rise just enough to clear it and air control carries the player over, and the jump press is spent, so pressing jump against a low hurdle never kicks off it backwards. Mirror's Edge's vaults will replace the mantle.

That matches the Sandbox gauges: the player walks up 0.18 m steps, steps up 0.35 m without jumping, jumps the 0.5 m steps, the 0.6 m and 1.3 m hurdles, and a 2 m gap, slides under the 1.3 m bar, and mantles out of the wall run lane's 1.8 m pit.

The mover runs movement modes in priority order: custom modes first, then the mantle, dash, wall run, wall kick, and slide, with walking and falling underneath. Each mode decides whether it takes over from the active one, then moves the capsule for the fixed step through shared helpers, and hands back to walking or falling when it ends; all of them read and write one `FMoverState`, so speed, timers, and the last jump carry across. `FGamePlayerSettings::Modes` turns built-in modes off, and `CustomModes` adds `IMovementMode` implementations, as Unreal's Mover adds movement modes. `FMovementTuning` holds every number, named after the game's fields.

An entity with a **Player** component (level schema 21) on a Character is who the player plays: Play spawns the player at its position facing its forward axis, as its skinned Static Mesh with its Character's weapon, sounds, and effects, and starts in third person when its **Third person** is set, the default. Every networked player plays it. A second Player is ignored with a warning. Without one, the player is the bare, silent capsule spawned at the bottom of the **Start** zone, facing its forward axis.

A Trigger's **Role** places the course: the clock runs from leaving the **Start** zone until the body's center enters a **Finish** zone. Walking back into Start resets the run, R restarts the course, and falling below -30 m returns to the start. The best time lasts for the session. A level without both zones is played as free exploration.

## Controls

WASD moves, holding Shift sprints, pressing Shift dashes toward the held direction and a quick tap on its own dashes backwards, the mouse looks, Space jumps, jumps off wall runs, and kicks off walls, C or Ctrl crouches and slides, F throws a ball, the left mouse button swings the sword and the right mouse button blocks, V switches between the third-person camera, where Play starts, and the first-person one, Z collapses the player into a ragdoll and pressing it again gets back up where it lies, R restarts, Esc opens the pause menu, and F11 or Alt+Enter toggles borderless fullscreen on the monitor under the window. These are the default bindings; every action except moving on the gamepad's stick and pausing can be rebound. Losing focus pauses the game and releases the pointer.

Gamepads in the standard layout work alongside them, in the game and in editor Play: XInput pads on Windows, and pads in GLFW's built-in SDL mapping database elsewhere, the first one connected. The left stick moves at its own speed, the right stick looks at up to about 200 degrees a second across and 140 up and down, A or cross jumps, clicking the left stick toggles sprinting, which also ends when the stick returns to rest, B, circle, or clicking the right stick crouches, X or square throws, the right shoulder swings the sword and the left shoulder blocks, Y or triangle switches the camera, Back restarts, and Start opens the pause menu, where the D-pad moves, A chooses, and B resumes. Sticks ignore 15% of drift around centre, and look grows with the square of the push past it, so small corrections stay fine. `FGameGamepadControls` in GameFramework maps them, and `FApplication::GetGamepadState` reads the pad.

## Menus, settings, and rebinding

Esc or Start opens the pause menu: **Resume**, **Save game**, **Load game** (disabled without a save), **Restart course**, **Settings**, and **Quit**. **Settings** holds fullscreen, HDR10 when the display offers it, the Low, Medium, and High quality tiers, Master, Effects, and Ambient volumes, mouse sensitivity as a multiple of the default 0.0022 radians per pixel, and **Controls**. **Controls** lists every rebindable action with its key or mouse button and its gamepad button; choosing one waits for the next key, mouse button, or gamepad button, and Esc cancels. A key taken by another action moves to the new one, so one key never does two things, and **Reset to defaults** restores them. The HUD's controls line names the keys bound now.

Keys, the pointer, and the gamepad share one focus: the arrows, WASD, or the D-pad move it and change a setting, Enter, Space, or A chooses, and Esc or B backs out one screen. While the menu is open or waiting for a binding it owns all input, so a key pressed to rebind never also jumps. Short notices such as "Game saved" show over the HUD.

Settings live per user in `%LOCALAPPDATA%\Herta\<Project>\Settings.ini` on Windows and `$XDG_CONFIG_HOME/Herta/<Project>/Settings.ini` (or `~/.config`) on Linux: `fullscreen`, `hdr`, `paperWhite` and `peakLuminance` in nits (0 follows the operating system, as in the editor), `sensitivity`, `quality` (0 Low, 1 Medium, 2 High, the default), `volumeMaster`, `volumeEffects`, and `volumeAmbient` from 0 to 1, and one `bind.<Action>` line per action for its key (`key:<code>` or `mouse:<button>`) and one `bind.<Action>.pad` line for its gamepad button. Unknown or out-of-range lines keep their defaults, and the file is written beside itself and renamed over. Logs are written beside it in `Logs/Game.log`.

## Saves

A save records the run's progress and the world state it changed, apart from the authored level: the objective state, elapsed and best times, respawns, the player's feet and facing, which destructible bonds are broken, and which characters died, all by stable object IDs. `FGameSession::CaptureSave` takes one and `ApplySave` restarts and puts it back; objects the level no longer has are skipped and listed. The game saves from the menu and automatically on finishing the course, to `Saves/<level id>.hsave` beside the settings.

Saves are versioned text ending in a checksum, so a truncated or damaged file is detected rather than half loaded. `WriteGameSave` keeps the previous save as `<file>.previous`, writes beside the file, and renames over it, so a crash mid-write never loses both. `LoadGameSave` falls back to the backup when the save is missing or damaged and says so; a save from another level is refused.

## Effects and streaming

Gameplay events spawn the effects their components name through sorted requests; see [Particles](Particles.md). Levels that place regions stream them in and out around the player on worker tasks; see [World streaming](WorldStreaming.md). Regions add and remove their navigation patches as they do.

## Navigation and agents

The game loads `<level>.hnav`, which staging cooks beside the level, and every StateTree its AI agents run. Agents walk the navigation crowd, perceive and fight the player, and start over when the course restarts; see [Navigation](Navigation.md) and [AI](AI.md). Editor Play builds the open level's navigation on a worker and hands it to the session when it is ready, and loads agents' StateTrees with the level's other assets.

## Sword combat

Sword fights follow Sekiro's rules, with numbers taken from its data where they are known. `FCombatant` in GameFramework's `Combat.h` holds a fighter's vitality and posture; `FHitStop` freezes animation briefly when blades meet.

- **Guard and deflect.** Pressing the guard (right mouse button) opens a 0.2 s deflect window, then holds a block. Pressing again within a second of the last press shrinks the window to 0.1 s, then 0.067 s, then nothing, so mashing the guard only blocks.
- **Posture.** The player has 120 posture, recovering at 30 per second, twice as fast behind the guard. Guards have 75 at 20 per second, slowing to x0.6, x0.3, and x0.1 as their vitality drops below 80, 60, and 40 %. A guard's overhead slash costs the player 25 vitality and 18 posture on a hit, 25 posture on a block, and 8 on a deflect; a deflect never breaks the player's guard. Blocks that fill the bar break it, staggering the player for 1.2 s.
- **Deflects.** A deflected guard reels for 0.7 s and loses 45 posture, then 22, 11, and 6 for deflects in a row, the run resetting after a 2 s pause.
- **Guards' defence.** A guard facing the player that is neither swinging nor reeling takes the player's swing on its sword: sparks, 20 posture, and the swing cuts nothing more. A swinging, reeling, or turned guard is cut as before.
- **Deathblow.** A broken posture staggers a guard for 3 s; during the first 2.5 s a red mark shows over its posture bar, and a cut is a deathblow that kills it.
- **Being struck down.** A blow that empties the player's vitality knocks them down as a ragdoll; they get up 2.5 s later with full vitality and posture.

The fight is about small things that make a hit read:

- **Stance.** Within 7 m of a foe, walking keeps the body facing it: sideways steps turn it at most 50 degrees toward the step, and stepping away backpedals instead of turning around. Sprinting breaks off. Sword states move only the upper body while walking, so swings and the guard walk and backpedal.
- **Step-in.** A swing turns to the foe within 3.2 m in front and steps toward it for 0.18 s, stopping 1.3 m short, so a cut reaches someone a stride away.
- **Strike windows.** Each swing cuts only around where the arm moves fastest, measured from the clips: 0.42 to 0.82 of the first swing, 0.34 to 0.66 of the second, and 0.26 to 0.42 of the third, `Sword_Regular_C`, which a third press chains.
- **Blades meet.** A guard's raised sword stops the player's blade where the two cross, sparking there, before the blade reaches its body. The blade also bounces off walls, props, and destructibles it sweeps into, throwing the arm back; the floor only scrapes, so low cuts land.
- **Guards deflect.** A guard blocks the first two swings of a quick chain and deflects the third: the player's arm is thrown back, they lose posture, and the guard answers at once. Pausing between swings, or waiting for a guard to attack and deflecting it, opens it up.
- **Weight.** Besides hitstop, heavy clashes slow a local game to a fifth of its speed for 0.05 to 0.25 s and shake the camera for 0.25 s; deathblows slow it longest. Networked sessions never slow down.

While a swing cuts, the blade draws its Character's **Trail** effect, for the player and guards alike; clients do not draw it yet. Blades meeting play the defending fighter's Character **Deflect** or **Block** sound and its **Sparks** effect (level schema 21 moved them from the level onto the Character), and freeze both fighters' animation for 0.06 to 0.16 s: still for 0.06 s, then easing back. The HUD shows vitality at the bottom left, posture filling out from the middle above the controls, and the facing guard's posture at the top. Combat resolves on the server; replicas do not yet carry posture or staggers, so clients see the outcome without the bars. Perilous attacks, the Mikiri counter, and dedicated strafe clips wait for the fights and assets that need them; sideways steps play the forward walk turned partway.

## Editor Play

Click **Play** in the viewport toolbar or press **Alt+P** or **Shift+Space**. Play waits until every mesh, weapon, and the sounds and effects components name have loaded, then starts a session on a copy of the level; authoring, selection, and undo history are untouched, and editing is locked as during Simulate. The viewport hides editor overlays and draws the game's own GameUI HUD, the same `DrawGameHud` the standalone game uses, through ImGui with the same registered textures. Clicking the viewport captures the pointer. **Shift+Esc** is the game's Esc: it opens the pause menu (Resume, Restart course, Exit, which stops Play), as does leaving the editor window. Esc or the active Play button stops and returns to the authored level. Sounds play through the editor's audio engine and mute while paused; see [Audio](Audio.md).

## Staging and packaging

`HertaEditorCmd game.stage <project.hertaproject> <output>` cooks the starting level's dependency closure through HertaAssetWorker: meshes, material assets and their textures in each sampled color space, Sky environments, weapons, the sounds and particle systems components name with every graph's waves, its agents' StateTrees, and its region levels with their scenery's assets, and builds the level's navigation with its regions' patches into `<level>.hnav`. The game opens the default audio device, or plays through the null path without one and during `--smoke-test`, and streams streamed sounds from their staged files. It writes them as loose files, copies the project descriptor, starting level, and region levels verbatim, and copies the HUD font with its license:

```text
<output>/Engine/Content/<asset-id>.hasset         engine assets
<output>/Engine/Content/Fonts/Roboto-Medium.ttf   HUD font and OFL.txt
<output>/<Project>/<Project>.hertaproject
<output>/<Project>/<starting level>
<output>/<Project>/<content root>/<asset-id>.hasset
```

Staging replaces only a directory that is empty or was staged before. A material with a custom shader or Material Graph has its twelve shading, depth prepass, bake, and shadow stages compiled through `HertaShaderWorker` without debug info to `Shaders/<hash>.<stage>.hshader` under the shader's mount, named by a hash of the shader key so deep output directories stay within path limits, with its parameter table beside them as `<hash>.parameters`. The game publishes them before play, so it links neither the shader compiler nor MaterialGraph.

`HertaGame --project=<path>` runs a staged project; without it, the game opens the content beside its executable: a package embedded in it, `.hpak` packages beside it, or the one loose project folder there, with any data mods in `Mods` over it. See [Distribution](Distribution.md). Package a relocatable build after building Shipping:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/Windows/PackageGame.ps1 -Project Games/Sandbox/Sandbox.hertaproject -Verify
```

```sh
./Scripts/Linux/PackageGame.sh --project Games/Sandbox/Sandbox.hertaproject --verify
```

The package holds the renamed game executable (`<Project>.exe` on Windows, `<Project>.x86_64` on Linux, named apart from a loose `<Project>` content folder), the dedicated server (`<Project>-Server.exe` or `<Project>.x86_64-server`), the content, `LICENSE`, `ThirdPartyLicenses`, and a README, under `Intermediate/Packages/<Project>-<platform>-x86_64-<Configuration>`. `-ContentMode`/`--content-mode` chooses the content: `Package`, the default, writes `<Project>.hpak` holding the staged files and engine shaders; `Embedded` builds that package into both executables, which then stand alone; `Loose` copies the staged files and `Shaders` folder. `-SigningKey`/`--signing-key` signs the package. Shipping Windows builds have no console. `-Verify` copies the package outside the checkout and plays a scripted run there, then starts the dedicated server and plays a hosted scripted run, and in package mode cooks the content again and requires identical package bytes.

## Crash reports

The game and the dedicated server write a report when they fault: the build revision, configuration, and platform, and the exception code or signal, as `Crash-<time>.txt` in the game's `Crashes` folder beside its logs, or `Saved/Crashes` for the server; on Windows a minidump goes beside it. Match a report to its build's symbols: the PDB on Windows, and the GNU build id on Linux.

## Multiplayer

`HertaGame --host[=port]` hosts a game on port 27015 or the given one and plays it through a loopback client, `--connect=host:port` joins a host or a `HertaServer`, and `--name=` names the player. See [Multiplayer](Multiplayer.md). `--culture=<tag>`, such as `pl`, `ru`, or `qps-ploc`, overrides the saved language for one run; see [Localization](Localization.md).

## Checks

- `HertaGame --smoke-test=<frames> [--validation]` plays a scripted run: it walks and jumps, loses focus, pauses and resumes, saves and loads, restarts the course, crosses the level's regions and back, checks that the level's agents run their StateTrees on the navigation, and exits with failure on any error or validation message. Smoke saves go to the temporary directory. With `--host` or `--connect` it plays over the network instead of saving, loading, restarting, and crossing regions, checks the agents on the host's server, and fails if no snapshot arrived.
- `Herta <level or project> --play-test [--validation]`, such as `Games/Sandbox/Sandbox.hertaproject`, runs two editor Play/Stop cycles with scripted input, saves `Saved/PlayTest/Play-N.png` from the player's view, and requires every authored model back after each stop.
- `HertaTests` cover the mover (the sprint speed curve, landing speed loss, slide braking and exit, disabled and custom modes, sphere casts) and the session (spawn, timing, restart, jump limits, respawn, sliding under a low ceiling, wall kicks, the chimney climb, mantles and their room check, wall runs from a run but not a standing jump, invalid input), staged content loading, trigger roles, GameUI layout, the menus' focus routing, rebinding, and settings, saves and their recovery, and the shared simulation builder.

The 600-frame Sandbox smoke run averaged 3.4 ms per frame at the default window size on an RTX 4090 Laptop GPU, Development and Shipping alike. Native Linux runs on X11 and Wayland pass the same checks.
