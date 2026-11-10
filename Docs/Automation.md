# Editor automation

Coding agents, and any other local MCP client, can drive a running editor through `HertaEditorMcp`: read its state and log, capture the viewport, and run Play with scripted input, without window focus or keystrokes.

## Running it

1. Start the editor with automation: `HertaEditor --automation` (read-only) or `HertaEditor --automation=write`. The log says where it listens.
2. Point the MCP client at `HertaEditorMcp`. The repository's `.mcp.json` registers the Windows Development build for Claude Code; on Linux use `Binaries/linux/x86_64/Development/HertaEditorMcp`. Build `HertaEditorMcp` before starting the client, since clients start their MCP servers when a session begins.

`HertaEditorMcp` finds the editor through `Saved/Automation/Session.json`, searching upward from its own folder for the repository, or under `HERTA_ROOT` when set. A session file left by an editor that crashed is harmless: the server keeps reading the file for up to 10 seconds until an editor answers.

## Tools

| Tool | Does |
|---|---|
| `editor_status` | The open level, entity count, viewport size, whether writes are allowed, and Play state. |
| `editor_log` | Log lines after a cursor; pass the returned `since` and `generation` back to read only newer lines. |
| `level_entities` | Entities with IDs, names, components, and parent-relative translation, rotation, and scale, filtered by `name_contains`. |
| `viewport_capture` | Renders `frames_ahead` more frames, then returns what the viewport panel shows as a PNG read back from the GPU, scaled to at most `max_width`. The render target extends behind the editor's glass panels; the capture crops to the panel. |
| `gpu_timings` | GPU milliseconds per render pass in the latest measured frame. |
| `play_start`, `play_stop` | Start Play, waiting up to 20 s for meshes and the character to load, or stop it. `at` [x, y, z] and `yaw` place the player there once Play runs. |
| `play_teleport` | Places the player's feet at `position` [x, y, z], facing `yaw` or keeping the current facing, standing and still, as a respawn does. The run and its clock carry on. |
| `play_state` | Player position, velocity, look yaw, grounded, crouched, sliding, mantling, and wall-running flags, the active movement mode, speed, wall contact and normal, ground normal, camera position and distance from the body, camera mode, animation state, and objective. |
| `play_input` | Drives the player for a number of frames: `move` [strafe right, forward], `look` total [yaw left, pitch up] radians, `face` a world [x, z] point to turn toward on the first frame, `jump`, `throw`, and `toggle_camera` on the first frame, `sprint` and `crouch` held; `sprint` with `move` forward runs at 7.2 m/s, and without it at 4 m/s. `steer_to` [x, z] turns toward a point every frame and stops within 0.3 m of it; `until` stops early once `grounded`, `airborne`, `landed`, `mode_changed`, or `mode:<Name>` holds, naming a movement mode such as `WallRun`; `trace` N samples the mode, feet, velocity, ground, wall contact, animation state, and animation events every N frames and on every mode change. Returns the Play state with `framesPlayed`, `stoppedBy` (`frames`, `arrived`, or `until`), `animationEvents` counting each event the character fired over the run, and the trace. |

Scripted Play advances a fixed 1/60 s per frame, so the same input gives the same result however fast the editor is drawing. Play started by `play_start` belongs to the client until `play_stop`: the mouse, keyboard, and gamepads cannot move the player, and the world holds still between calls, so only `play_input` frames advance it and runs repeat exactly. Play started by hand stays interactive.

To test movement, place the player with `play_teleport`, run input with `until` and `trace`, and read the trace's mode changes instead of adding logging; for example, run at a wall `until` `mode:WallRun`, then jump `until` `landed` with `trace` 10.

## Design

- `Platform` owns the transport: named pipes on Windows that reject remote clients and only admit their owner, and Unix domain sockets with mode 0600 in the user's runtime directory on Linux. Messages are a 32-bit length and up to 16 MiB of UTF-8 JSON.
- `EditorCore`'s `FAutomationServer` runs one client at a time on its own thread. The first message must carry the random session token from the session file; tool calls queue for the editor's main thread, which answers at a safe point each frame, possibly frames later. Unanswered calls fail after 30 s, so a stalled editor never hangs the client. Every call is recorded in `Saved/Automation/Audit.log`.
- `EditorCore`'s `HandleMcpMessage` turns MCP JSON-RPC lines into replies, so the protocol is unit-tested without processes; `HertaEditorMcp` only reads stdin and forwards calls. Captures come back as MCP image content.
- Tools that change content will refuse read-only sessions; none of the first tools write. No tool clicks ImGui or runs a shell.

Added tools follow a real need, as these did; see Milestone 7.5 in [EngineDesign.md](EngineDesign.md).
