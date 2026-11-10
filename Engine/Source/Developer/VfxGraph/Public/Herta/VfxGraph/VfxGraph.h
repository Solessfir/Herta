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
inline constexpr std::string_view VfxGraphDomain = "vfx";
inline constexpr std::uint32_t VfxGraphVersion = 1;
inline constexpr std::string_view VfxGraphExtension = ".hvfxgraph";

[[nodiscard]] const FGraphSchema& GetVfxGraphSchema();

struct FVfxGraphCompilation
{
	FCookedParticleSystem System{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Emitters compile in the order of the System node's pins. An emitter needs a Spawn module; every other module it lacks takes its defaults.
[[nodiscard]] std::expected<FVfxGraphCompilation, std::vector<FGraphDiagnostic>> CompileVfxGraph(const FGraph& Graph);
// A new graph: one emitter puffing a short burst of particles upward into the System node.
[[nodiscard]] FGraph MakeDefaultVfxGraph();
}
