# Gameplay Graphs

Gameplay graphs are Blueprint-style event graphs that run on an entity. They are one typed program with two backends: a call list that the editor and development builds run, and C++ generated from the same program that Shipping builds compile in. Tests run both on the same graphs and require the same calls and the same final frame. The C++ game stays fully playable without any graph.

## Authoring

A `.hgameplaygraph` is a GraphCore document in the `gameplay` domain, imported by the `GameplayGraph` importer and compiled by the developer `GameplayGraph` module. White execution wires run from event nodes through flow and action nodes; float, bool, and vector wires carry values from pure nodes.

- **Events**: On Start (once per session start), On Player Enter and On Player Exit (any player stepping into or out of the entity's Trigger box), and On Tick (every update).
- **Flow**: Branch on a bool, and Sequence, which runs up to three outputs in order. An execution output runs one node; fanning one out is an error that points at Sequence.
- **Actions**: Play Sound and Spawn Effect name a level sound or effect cue (`objective`, `sparks`, `break`, and the rest under **Viewport settings**) and default to the entity's position; Set Variable stores a float; Log writes a message and a value to the session's event log.
- **Values and math**: Get Variable, Constant, Self Position, Delta Seconds, Player Distance (to the nearest player), Add, Subtract, Multiply, Greater, and Not. An unlinked float input uses the node's own property.

Variables are floats named on Get and Set nodes and kept per entity until the session restarts. Pure nodes are evaluated again by each action that reads them, as in Blueprints, so a value read after a Set sees the new value.

Compile errors name the node: an unknown cue, a fanned-out execution output, a graph without events, or a program past its limits. Nodes no event reaches only warn. `HertaEditorCmd gameplay.compile <graph>` checks one headless.

In the editor, **New Gameplay Graph** in the Content Browser's menu creates one, and double-clicking or **Edit Gameplay Graph** opens it in the graph document panel. A **Gameplay Graph** component on an entity runs it during Play; add a **Trigger** to the same entity for the enter and exit events. Level schema 19 adds the `gameplayGraph` component with one `graph` asset reference; schemas 1 through 18 load without it.

## Program

`FCookedGameplayGraph` (cooked format 11) is a flat program: a frame of float slots with their starting values, an entry instruction per event, the instructions, and the cue and message strings. Vectors take three slots and bools are 0 or 1. Each instruction has an operation, slot operands, a forward jump for Branch and Jump, and the graph node it came from. `ValidateGameplayGraph` rejects any operand outside the frame, any backward or out-of-range jump, and code that does not finish with End, so every run ends within the program's length. Limits are 4096 slots, 16384 instructions, and 256 strings.

The compiler lays out each Branch as the condition, the true side, a jump over the false side, and the false side. Code generation relies on that shape to rebuild an `if` and `else` without labels.

## Backends

- **Call list**: `FGameplayProgram` binds each instruction to a direct function pointer, one per operation, so an instruction costs one indirect call and binding costs one table lookup per instruction. It records the nodes it runs for the editor, and a recooked graph swaps in during Play.
- **Generated C++**: `HertaEditorCmd gameplay.generate <content> <output.cpp> <function>` compiles every graph under a content folder and writes `<function>`, which returns a table of functions per program keyed by the FNV-1a hash of the cooked program. A game module registers the table through `IGameModuleRegistry`, which passes it to `RegisterGeneratedGameplayGraphs` and unregisters it before the module unloads, so the engine holds no game's code; the Sandbox's is `Games/Sandbox/Source/SandboxGame/Private/GeneratedSandboxGameplayGraphs.cpp`. `FGameplayProgram::Create` with the native backend uses generated code only when a registered program's hash matches exactly, and otherwise falls back to the call list. Engine tests generate `GetTestGameplayGraphs` from their fixtures into `Engine/Source/Programs/HertaTests/Private/GeneratedTestGameplayGraphs.cpp`, and a test fails when it is out of date. Native code is not traced.

Shipping prefers the native backend; Debug and Development run the call list.

## Play

GameFramework's session runs every Gameplay Graph component after the players move and before effects start, so a graph's effects join that update's sorted requests. Sounds go through the level sounds, effects through the level effects, and logs into `TakeEvents`. Graphs drive presentation only: their frames are not in rollback snapshots and they never change the simulation. Entity positions and trigger boxes are the authored ones, so a graph on a moving entity acts where the entity was placed.

While a graph's panel is open during Play, nodes that ran in the last second light up. `FGameSession::GetGameplayGraphActivity` reports when each node last ran.

The Sandbox's `Logic/Shrine.hgameplaygraph` counts visits to its trigger: each visit throws sparks and logs the count, and the third breaks into dust with the objective chime and starts counting again.

## Checks

- `HertaTests` cover events, branches, variables, and actions through the call list; diagnostics on their nodes; cooked round trips and the validation that stops loops and out-of-frame access; the generated code being current and matching the call list call for call; headless cooking; and a session that runs a level's graph when the player stands in its trigger.
