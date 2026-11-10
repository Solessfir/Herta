#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/Sound.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace Herta
{
// Audio Graphs build a sound from wave players, mixers, gain, pitch, and filters, with Float, Bool, and Trigger inputs that gameplay sets
// while the sound plays. Wave players start with the sound, on a Trigger, or while a Bool is on.
inline constexpr std::string_view AudioGraphDomain = "audio";
inline constexpr std::uint32_t AudioGraphVersion = 1;
inline constexpr std::string_view AudioGraphExtension = ".haudiograph";

[[nodiscard]] const FGraphSchema& GetAudioGraphSchema();

// Finds a wave by its path relative to the graph's content root, such as "Audio/Impact_1.wav".
using FAudioWaveResolver = std::function<std::optional<FAssetId>(std::string_view Path)>;

struct FAudioGraphCompilation
{
	FSoundGraphDesc Graph{};
	// The wave paths each player named, resolved, in compiled order, so cookers can record them as dependencies.
	std::vector<std::string> WavePaths{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Parameters compile in node order. Nodes run inputs first from the output back, so every node feeds only later ones; nodes that do not
// reach the output are dropped with a warning. A wave player lists its waves as comma-separated paths and picks one at random each start.
[[nodiscard]] std::expected<FAudioGraphCompilation, std::vector<FGraphDiagnostic>> CompileAudioGraph(const FGraph& Graph, const FAudioWaveResolver& Resolve);
}
