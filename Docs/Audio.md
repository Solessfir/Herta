# Audio

The runtime Audio module plays cooked sounds and Audio Graphs. miniaudio opens the playback device and decodes sources at import; Herta mixes. The developer AudioGraph module compiles graphs, and GameFramework turns gameplay into audio commands that the editor and the game submit.

## Engine

`FAudioEngine::Create` opens the system's default device at 48 kHz stereo, or falls back to the null path and reports why in `FAudioStats::DeviceError`. `EAudioDevice::Null` asks for the null path directly: `Update` renders elapsed time into nothing, and `Render` renders on demand on the calling thread, opening and filling streams there, so its output is the same on every run. Headless tests and `HertaGame --smoke-test` use it.

The game thread adds sounds (`AddSound`, `AddSoundStream`, `AddSoundGraph`) and submits `FAudioCommand`s: play, stop with a fade, move, set a graph input, move the listener, set a bus volume, and stop everything. The mixer runs on the device's real-time thread. Commands reach it through a fixed 4096-entry single-producer, single-consumer queue of plain structs whose names and asset IDs the game thread has already resolved; ended voices come back through a second queue. The mixer never allocates, locks, or touches files: a test counts allocations on the rendering thread while it renders graphs and spatial voices under bus and parameter changes, and another renders on its own thread while the game thread submits 50,000 commands and replaces sounds that are playing. A replaced or removed sound stays alive while any voice or queued play still refers to it.

Voices: 64. Each renders in 256-frame blocks with linear resampling, so any source rate and pitch play directly. Spatial voices attenuate by inverse distance from their minimum distance, windowed to silence at their maximum, and pan with equal power from the listener's left vector (up cross forward, since +X is left). A voice whose gain is silent keeps time without reading or filtering samples. Buses form a tree under Master: Effects, Ambient, and Interface by default. Volumes ease over a few blocks; `FAudioStats` reports each bus's peak since the last update.

Concurrency: each sound allows `MaximumInstances` voices. Over the limit, `StealOldest` fades the oldest out in 10 ms, `StealQuietest` fades the quietest or skips the new one if it would be quieter, and `Reject` skips it. When all 64 voices are busy, the quietest voice, preferring one already fading, ends at once unless the new one would be quieter.

Streaming: a sound cooked with `Stream` keeps its samples on disk. Its voice claims one of 8 rings of 65,536 frames, which a streaming thread fills from the staged file or, in the editor, the cache entry in place; the cooked layout puts the samples last, uncompressed, so they read at an offset with no decoding. Opening a stream takes a block or two of silence; frames a playing stream skips because the thread fell behind count as `StreamStarvedFrames`. Looping streams wrap the file and the ring without seams; the test plays a stream longer than its ring at a pitch of 1.37 and compares it with the same sound loaded whole.

Measured on the Sandbox in the editor: 23 sounds loaded, up to 8 voices and one stream, 6 microseconds of mixing per 5.3 ms block on average and 200 at the peak. The stress test, with all 64 voices busy, averages 300 microseconds in Development builds.

## Sounds and Audio Graphs

`.wav`, `.flac`, and `.mp3` files import with the `Sound` importer, which decodes them in the asset worker at their own rate, mixes more than two channels to stereo, and cooks 16-bit PCM. `.haudiograph` files import with the `AudioGraph` importer. Both take these metadata settings: `Bus`, `Volume` (linear), `MaximumInstances` (1 to 64), `Concurrency` (`StealOldest`, `StealQuietest`, or `Reject`), and `MinimumDistance` and `MaximumDistance` in meters. Sounds also take `Loop` and `Stream` (`true` or `false`).

An Audio Graph builds one sound from nodes:

| Node | Does |
|---|---|
| Wave Player | Plays one of its waves, picked at random each start, with optional pitch and gain variation and looping. It starts with the sound, or on its Play pin: each fire of a Trigger input, or while a Bool input is on. |
| Mixer | Sums up to four sounds. |
| Gain | Scales a sound by a constant or a Float control, ramping across each block. |
| Pitch | Multiplies the playback rate of every wave player that feeds it. It compiles into those players rather than running as a node. |
| Low Pass, High Pass | State-variable filters with a constant or controlled cutoff. |
| Map Range | Clamps a Float to an input range and maps it onto an output range. |
| Float, Bool, Trigger Input | The named inputs gameplay sets. |
| Output | The graph's sound. |

Wave paths are relative to the graph's content root, such as `Audio/Impact_1.wav`, and resolve to asset IDs through the waves' sidecars, which the cooked graph depends on. Each sound output feeds one input, so every wave player has one path, and one set of pitches, to the output. The compiler emits nodes inputs first, so every node feeds only later ones, and drops nodes that do not reach the output with a warning. A graph voice ends once its players are idle, unless it is addressed and a player waits on an input; a voice played without an identity but with inputs gets a private one so the inputs reach it.

`HertaEditorCmd audio.compile <graph.haudiograph>` compiles a graph headless and lists its inputs and waves or what stops it compiling.

Procedural synthesis, envelopes, sample-accurate scheduling, and subgraphs wait for Milestone 15.

## Levels

Level schema 13 adds `audioEmitter` and `audioListener` components and the level's `sounds`. An emitter plays a sound or graph from its entity during Play: `volume` and `pitch` multiply the sound's own, `autoPlay` starts it with Play, `spatial` pans and fades it, and `minimumDistance` and `maximumDistance` override the sound's. Emitters on moving entities follow them. A listener hears the level from its entity instead of the player's head; the active one with the lowest ID wins.

The level's sounds name what gameplay plays for its own events: `footstep`, `land`, `jump`, `slide` (a loop while sliding), `impact`, `swing`, `slice`, `sever`, and `objective`. Footsteps carry a `Speed` input, impacts an `Impulse` in N*s, and the run's start and finish a `Finished` input of 0 or 1. Impacts come from contacts that begin with Dynamic bodies, thrown balls, and destructibles: the four hardest each frame above 1.5 N*s, and every break. Edit them under **Viewport settings > Sounds**.

`FGameSession::TakeAudioCommands` returns what the session asked for since the last call. Voice identities restart with every session start, so the same input issues the same commands; a test plays a scripted run twice, compares the commands, and plays them through the null path.

## Editor

The editor keeps the open level's sounds and the waves of their graphs loaded in its own audio engine, so Play starts without waiting, and mutes it while Play is paused. Dragging a sound or Audio Graph from the Content Browser into the viewport places an entity with an Audio Emitter a meter above where it lands. Double-clicking an Audio Graph, **Edit Audio Graph** on its context menu, or naming a `.haudiograph` on the command line opens it on the graph canvas; **New Audio Graph** in a Game folder's context menu creates one. The panel compiles the draft as it changes and auditions it through the editor's engine with **Play** and **Stop**, with a control for each input: a slider for Floats, a check box for Bools, and a button for each Trigger, which change the playing audition as gameplay would.

`play_state` and `editor_status` over MCP report the engine's device, voices, streams, steals, rejections, missing sounds, starvation, dropped commands, mixing time against the block budget, and each bus's volume and peak.

## Platforms

The Windows backend is WASAPI. On Linux, miniaudio loads PulseAudio, PipeWire through its PulseAudio interface, or ALSA at runtime, so the module links only `dl`, `m`, and `pthread`, and a machine without a sound server plays through the null path. Advanced acoustics, such as Steam Audio, wait until a real scene shows the need.
