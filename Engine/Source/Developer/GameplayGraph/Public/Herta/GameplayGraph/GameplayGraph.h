#pragma once

#include "Herta/Assets/GameplayGraph.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// Gameplay graphs are Blueprint-style event graphs run on an entity: event nodes start white execution wires through branches,
// sequences, and actions, and pure nodes compute the float, bool, and vector values those read. An execution output runs one node;
// a Sequence runs several in order. Variables are floats named on Get and Set nodes, kept per entity for the whole session.
inline constexpr std::string_view GameplayGraphDomain = "gameplay";
inline constexpr std::uint32_t GameplayGraphVersion = 1;
inline constexpr std::string_view GameplayGraphExtension = ".hgameplaygraph";

[[nodiscard]] const FGraphSchema& GetGameplayGraphSchema();

struct FGameplayGraphCompilation
{
	FCookedGameplayGraph Program{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Pure nodes are evaluated again by every action that reads them, as in Blueprints, so a value always reflects the variables at that point.
[[nodiscard]] std::expected<FGameplayGraphCompilation, std::vector<FGraphDiagnostic>> CompileGameplayGraph(const FGraph& Graph);
// A new graph: entering the trigger plays the objective sound at the entity.
[[nodiscard]] FGraph MakeDefaultGameplayGraph();

struct FGameplayGraphSource
{
	// Shown in a comment above the program's code, such as its content path.
	std::string Name{};
	FCookedGameplayGraph Program{};
};

// Compiles every gameplay graph under ContentRoot, in path order, named by its content path; fails on the first that does not compile.
[[nodiscard]] std::expected<std::vector<FGameplayGraphSource>, std::string> CollectGameplayGraphs(const std::filesystem::path& ContentRoot);
// C++ for every program, defining GetGeneratedGameplayGraphs; each program's functions do exactly what the call list does for it.
[[nodiscard]] std::expected<std::string, std::string> GenerateGameplayGraphCpp(std::span<const FGameplayGraphSource> Sources);
}
