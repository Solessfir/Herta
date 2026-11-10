# Multiplayer

Herta plays one authoritative game session per server, and every player, including a listen server's own player, plays it through a client. Initial scope is direct-IP dedicated and listen servers; P2P, NAT traversal, matchmaking, and host migration are later work.

## Modules

- `Networking` wraps [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) (submodule, static, P2P/ICE/WebRTC off) behind `FNetworkTransport`: encrypted reliable and unreliable messages over UDP, loopback pairs between two transports in one process, per-connection statistics, and process-wide simulated loss, lag, jitter, reordering, duplication, and rate limits. GameNetworkingSockets' wire messages are Protobuf 21.12, the last release without Abseil, built from its submodule together with its own `protoc`, so Setup downloads nothing for it. Windows encrypts with BCrypt and the bundled 25519 code; Linux uses the system OpenSSL `libcrypto`. Direct-IP play has no certificate authority, so peers encrypt with their own keys without signed identities.
- `Replication` owns the wire protocol (`ReplicationProtocol.h`), `FGameServer`, and `FGameClient`. Transport callbacks never touch the session; everything arrives through `Poll` on the ticking thread.
- `GameFramework` gives the session what replication needs: several players, authority, replicas, outcomes, and rollback snapshots.

## Session

`FGameSession` holds up to 16 players. Per-player state lives in `FPlayerSlot`, which the session derives from; it processes players one at a time by moving each one's slot into its base, so movement, animation, cuts, and sounds read the same members for every player. `UpdatePlayers(DeltaSeconds, Inputs)` takes one input per player; `Update(DeltaSeconds, Input)` remains for one player. `AddPlayer`, `RemovePlayer`, `ResetPlayer`, `PlacePlayer`, and `SetViewPlayer` manage them. Later players spawn beside the first, alternating sides 0.8 m apart. AI agents see every player and chase the nearest one in view; world streaming keeps regions around every player loaded.

`SetAuthority(false)` makes a session a client: agents, destructible breaks, blade and ball cuts, thrown balls, and ball ageing come from the server. `SetPlayerSimulated(Player, false)` makes a remote player a puppet that only replicas place.

Replicas are the server's view of what a client needs: `FGamePlayerReplica` (the mover snapshot, look, run, ragdoll, block, and a swing counter), `FLevelBodyReplica` for Dynamic level bodies, `FGameProjectileReplica` under server-assigned ids, and `FGameNpcReplica` for AI characters. Breaks and cuts are `FGameOutcome`s the server decides and every client applies once, in order: a break carries the destructible's remaining bonds, a slice its world plane, a sever its joint name.

## Rollback

Physics `SaveState` now includes every Jolt `CharacterVirtual` and a fingerprint of the world's bodies, constraints, and characters, so a state only restores into a world holding the same ones. `FLevelSimulation::SaveSnapshot` adds the simulation's own state, and `FGameSession::SaveSnapshot` adds every player's mover, look, run, and knockdown and the thrown balls. A test restores a snapshot, replays the same inputs for both players, and gets the same `ComputeStateHash`. A snapshot goes stale once bodies come or go, such as a spawn, a break, or a ragdoll; AI, animation, sound, and effects are not part of it.

## Protocol

Version 2, checked at the handshake together with a content hash of the staged starting level. Messages:

| Message | Delivery | Contents |
| --- | --- | --- |
| Hello | Reliable | Magic, protocol version, content hash, reconnect token, name |
| Welcome | Reliable | Player id, server tick, reconnect token |
| Reject | Reliable | Reason, also given as the connection's close reason |
| Input | Unreliable | Snapshot acknowledgement with a 32-bit mask, and the newest 8 inputs |
| Snapshot | Unreliable | Sequence, tick, the receiver's applied input tick, changed entities, removals |
| Outcome | Reliable | A break, cut, or kill by blows with its tick |

The server ticks at 60 Hz and snapshots every third tick. Entities have stable ids: players count up from 1, and bodies, balls, and characters add their level index or server id to fixed bases. Each entity's state is sent as the XOR against the state the client last acknowledged, run-length coded, so a resting body costs nothing and a running player a few dozen bytes; one the client never acknowledged goes whole. Entities beyond 80 m of the client's player are left out and removed on the client. Changed entities are sent in priority order, which grows with kind and nearness each snapshot they wait, until the 1100-byte budget is used; the client's own player always goes.

Decoding validates sizes, counts, enums, and that every float is finite. The server drops a connection after 16 malformed messages, clamps move input to full deflection, refuses look turns over half a circle per tick, and ignores inputs older than applied or more than 120 ticks ahead. A fuzz test feeds 20000 corrupted messages to every decoder.

## Server

`FGameServer` takes a session and listens on a port (zero picks one). Each tick it receives, applies each player's input for the tick, steps the session, sends outcomes, and sends snapshots. Input arrives redundantly; a missing tick repeats the last held movement and buttons without presses, a tick whose copies were all lost is skipped once a newer one arrives, and a backlog over 6 inputs skips ahead. Unowned session players are parked out of the way, since the session always keeps one. A dropped player waits 30 s, and a client reconnecting with its token takes it back where it left it. A late joiner receives every break and cut so far.

`HertaServer` is the dedicated server: `--project=`, `--port=` (default 27015), `--max-players=` (default 8), and `--ticks=` for unattended runs. It loads the staged project through the same `CreateStagedSession` as the game, with no device: meshes are bounds-only for collision and materials are skipped. It links no Vulkan, window, ImGui, or audio-device libraries: on Windows it imports only the CRT, kernel, and network DLLs, and on Linux only `libcrypto` beside the C and C++ runtimes. It logs to `Saved/Logs/Server.log` under the working directory.

## Client

`FGameClient::Connect` joins over UDP and `ConnectLocal` through a loopback pair to a server in the process. Each tick it receives, sends its input, interpolates remote players and characters 6 ticks behind the newest snapshot, and predicts the local player by stepping its session with local input. When a snapshot's own state differs from the prediction at the applied input tick by more than 1 cm, or the run or ragdoll state differs, it applies the server's state and replays every unacknowledged input through `ReplayPlayer`, which steps only that player's mover. Bodies take the latest snapshot and simulate locally in between; balls spawn, move, and despawn to match the server. Corrections snap without smoothing.

`FTickInput` gathers rendered frames into fixed ticks: look turns add up and presses wait for the next tick.

## Game

`HertaGame --host[=port]` runs a server with its own headless session and plays through a loopback client; `--connect=host:port` joins a server; `--name=` names the player. A network game never pauses and the server owns its course, saves, and restarts. Smoke runs in either mode report snapshots, corrections, and outcomes and fail if the game never played. Packages include `<Project>-Server`, and package verification runs it and a hosted smoke run from the relocated package.

## Checks

- `Network transports *` - UDP and loopback, reliable and unreliable.
- `Game sessions play several players *` and `Game session snapshots *` - players, views, removal, and rollback replay to the same hash.
- `Replication *` - round trips, deltas, refusals of truncated, oversized, and non-finite data, and fuzzing.
- `Replicated sessions play the course *` - a listen server's loopback client and a UDP client finish the course, push a crate, and see each other.
- `Replicated clients reconnect *` - reconnects, a full server, other content, another protocol version, and garbage.
- `Replicated prediction *` (Slow) - 10% loss, 40 ms lag, 15 ms jitter, 5% reordering and duplication, and a 64 KB/s limit; prediction ends within 5 cm of the server.
- `Dedicated servers in their own process *` (Slow) - a `HertaServer` process with two clients that finish the course.

Measured on the Sandbox: a hosted smoke run receives about 20 snapshots a second at 6 to 11 KiB/s for one player.
