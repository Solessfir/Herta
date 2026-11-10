# Particles and VFX Graphs

Effects are cooked particle systems: up to 4 emitters, each choosing among fixed update kernels by the features it uses, so nothing interprets a per-particle program. They are authored as VFX Graphs on the shared graph canvas, compiled headless, and spawned from sorted gameplay events in editor Play and the game.

## Runtime

- **Assets** owns `FCookedParticleSystem` (cooked format 7). An emitter spawns for `DurationSeconds` (zero spawns until stopped) at `SpawnRate` per second after an initial `Burst`, within `SpawnRadius` of the effect's origin, with velocities in a cone around `Direction`. Lifetime, diameter, and linear color with opacity ease from birth to death; `Emissive` adds luminance in nits on top of the lit color. `Gravity` scales gravity and `Drag` slows particles per second. Collision bounces with `Restitution` and `Friction`, or sticks particles where they land, as blood drops do. `ValidateParticleSystem` rejects nonfinite or out-of-range values.
- **Particles** (runtime) owns `FParticleWorld`: at most 16384 live particles, 4096 per emitter, 256 effects, and 512 collision casts per update. Spawns past a budget are dropped and counted, never allocated. Each effect has its own random stream seeded from its start, so the same requests produce the same particles. Collision goes through a caller-supplied sphere cast; the session casts against the physics world through `CastSphereHit`. A zero-length update does nothing, and one update advances at most 0.25 s, so a hitch slows effects instead of tunneling them through walls.
- **Renderer**: `FMeshRenderView::Particles` lists camera-facing discs, sorted back to front and lit on the CPU, since particles have no normals: the directional lights at a half-lit average plus the sky as a quarter of the sun. The **Particles** pass composites them over opaque geometry into HDR scene color, testing depth without writing it, before fog and tone mapping, so emissive embers bloom and exposure treats them like the scene. Buffers grow to the largest frame and never shrink; uploads are padded to the buffer's size. Particles write no motion vectors and Herta has no temporal AA yet, so there is no history for them to smear; the pass is skipped when there are none.

## VFX Graphs

A `.hvfxgraph` is a GraphCore document in the `vfx` domain, imported by the `VfxGraph` importer and compiled by the developer `VfxGraph` module:

- **System** collects up to 4 **Emitter** nodes.
- **Emitter** sets the name, particle budget, duration, and spawn radius, and takes one node per module: **Spawn** (rate and burst), **Cone Velocity** (direction, half-angle in degrees, and speed range), **Lifetime**, **Size Over Life**, **Color Over Life** (with emissive), **Forces** (gravity and drag), and optionally **Collision** (restitution, friction, stick).

Compile errors name the node. `HertaEditorCmd vfx.compile <graph.hvfxgraph>` compiles one headless and prints its emitters. In the editor, **New VFX Graph** in the Content Browser's menu creates one, and double-clicking or **Edit VFX Graph** opens it in the graph document panel; the sidebar's **Play at pivot** previews the unsaved graph in the viewport at the selected object. Dropping a VFX Graph on the viewport places a Particle Emitter.

## Levels and gameplay

Level schema 14 adds the `particleEmitter` component (`system`, `autoPlay`, `scale`) and a top-level `effects` object naming the system for each gameplay event: `impact`, `break`, `land`, `slice`, and `sever`. Set them under **Viewport settings > Effects**. The session gathers effect requests through an update, sorts them by cue and position, then starts them, so the particles never depend on which system asked first. Auto-play emitters start with the session and follow their entity. Impact clouds grow with the contact impulse, up to twice their authored size, and face away from the surface.

The Sandbox's effects are `Content/Effects`: dust for impacts and landings, debris and dust for breaks, blood for slices and severed limbs, and embers rising over the finish ledge. They are written by hand as graphs; change them on the canvas and Play to see them.

## Checks

- `HertaTests` cover spawning, budgets, determinism, sticking and bouncing collision, retirement of finished effects, the renderer's particle pass and buffer growth, VFX Graph compilation and its errors, and the session's sorted effect requests.
- `play_state` over editor automation reports live particles, effects, spawns, drops, retirements, and collisions.
