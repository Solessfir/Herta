#pragma once

#include "Herta/Assets/Material.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Herta
{
// Pin types are vector widths. Dynamic pins take any width and the compiler resolves them per node, as math nodes do for whatever feeds them.
enum class EMaterialPinType : std::uint32_t
{
	Float = 1,
	Float2 = 2,
	Float3 = 3,
	Float4 = 4,
	Dynamic = 8,
};

inline constexpr std::string_view MaterialGraphDomain = "material";
inline constexpr std::uint32_t MaterialGraphVersion = 1;
inline constexpr std::string_view MaterialGraphOutputType = "MaterialOutput";

[[nodiscard]] const FGraphSchema& GetMaterialGraphSchema();

struct FMaterialGraphCompilation
{
	// A complete material shader for the custom material contract: the shared mesh shading include plus a generated EvaluateMaterial.
	std::string Source{};
	// Warnings when the graph compiled; errors, keyed to the node that caused them, when it did not.
	std::vector<FGraphDiagnostic> Diagnostics{};
	// Every parameter node, connected or not, in slot order, which is name order, so a material's values keep their meaning as the graph changes.
	std::vector<FMaterialShaderParameter> Parameters{};
	// Every static switch, by name, with its default in Default[0] as 0 or 1. Switches take no slot: they choose the code that compiles.
	std::vector<FMaterialShaderParameter> Switches{};
};

// Compiles the part of the graph that reaches the output node. Unconnected output inputs keep the material asset's own parameters and textures,
// so a graph only replaces what it connects. Switches, sorted by name, override static switch defaults; names the graph lacks are ignored.
[[nodiscard]] std::expected<FMaterialGraphCompilation, std::vector<FGraphDiagnostic>> CompileMaterialGraph(const FGraph& Graph, std::span<const FMaterialSwitchValue> Switches = {});
}
