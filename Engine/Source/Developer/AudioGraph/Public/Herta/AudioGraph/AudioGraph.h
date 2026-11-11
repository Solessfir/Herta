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
// Audio Graphs build a sound from wave players, oscillators, envelopes, mixers, gain, pitch, and filters, with Float, Bool, and Trigger
// inputs that gameplay sets while the sound plays. Wave players, oscillators, and envelopes start with the sound, on a Trigger, or while a
// Bool is on, and each can wait a delay first, kept to the sample. A Subgraph node plays another Audio Graph in place: its nodes are copied
// in when this graph compiles, and its inputs join this graph's inputs of the same name.
inline constexpr std::string_view AudioGraphDomain = "audio";
inline constexpr std::uint32_t AudioGraphVersion = 2;
inline constexpr std::string_view AudioGraphExtension = ".haudiograph";

[[nodiscard]] const FGraphSchema& GetAudioGraphSchema();

// Finds a wave by its path relative to the graph's content root, such as "Audio/Impact_1.wav".
using FAudioWaveResolver = std::function<std::optional<FAssetId>(std::string_view Path)>;
// Loads another Audio Graph by its path relative to the content root, such as "Audio/Blip.haudiograph", for Subgraph nodes.
using FAudioGraphLoader = std::function<std::optional<FGraph>(std::string_view Path)>;

struct FAudioGraphCompilation
{
	FSoundGraphDesc Graph{};
	// The wave paths each player named, resolved, in compiled order, so cookers can record them as dependencies.
	std::vector<std::string> WavePaths{};
	// The subgraph files it copied in, for the same reason.
	std::vector<std::string> GraphPaths{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Parameters compile in node order. Nodes run inputs first from the output back, so every node feeds only later ones; nodes that do not
// reach the output are dropped with a warning. A wave player lists its waves as comma-separated paths and picks one at random each start.
// Without a loader, Subgraph nodes fail to compile. Problems inside a subgraph are reported on the Subgraph node that brought it in.
[[nodiscard]] std::expected<FAudioGraphCompilation, std::vector<FGraphDiagnostic>> CompileAudioGraph(const FGraph& Graph, const FAudioWaveResolver& Resolve, const FAudioGraphLoader& Load = {});
}
