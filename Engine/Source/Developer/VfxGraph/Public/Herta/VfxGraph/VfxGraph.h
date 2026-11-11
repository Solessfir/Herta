#pragma once

#include "Herta/Assets/ParticleSystem.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace Herta
{
// VFX Graphs author particle systems: module nodes for spawning, velocity, lifetime, size, color, forces, and collision feed Emitter nodes,
// and the System node takes up to four emitters. Angles are degrees on the canvas and radians once cooked.
//
// An emitter's module stack is up to four Stage nodes, each a stretch of particle life running a chain of Force, Drag, Vortex, Attract, and
// Turbulence modules in order: the stage takes the first module, and each module takes the next in its Next pin. Event nodes take another
// emitter in Target and feed an emitter's Event pins, so its particles that die or collide spawn particles in the target.
inline constexpr std::string_view VfxGraphDomain = "vfx";
inline constexpr std::uint32_t VfxGraphVersion = 2;
inline constexpr std::string_view VfxGraphExtension = ".hvfxgraph";

[[nodiscard]] const FGraphSchema& GetVfxGraphSchema();

struct FVfxGraphCompilation
{
	FCookedParticleSystem System{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Emitters compile in the order of the System node's pins. An emitter needs a Spawn module unless an event feeds it; every other module it lacks takes its defaults.
[[nodiscard]] std::expected<FVfxGraphCompilation, std::vector<FGraphDiagnostic>> CompileVfxGraph(const FGraph& Graph);
// A new graph: one emitter puffing a short burst of particles upward into the System node.
[[nodiscard]] FGraph MakeDefaultVfxGraph();
}
