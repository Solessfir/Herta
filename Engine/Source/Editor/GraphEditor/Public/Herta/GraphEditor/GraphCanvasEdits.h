#pragma once

#include "Herta/GraphEditor/GraphCanvasFormat.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EGraphDirection : std::uint8_t
{
	Left,
	Right,
	Up,
	Down,
};

// Every node linked to Seeds through any chain of links in either direction, Seeds included.
[[nodiscard]] std::vector<FGraphNodeId> GraphConnectedNodes(const FGraph& Graph, std::span<const FGraphNodeId> Seeds);
// Min x, min y, max x, max y around the nodes' layouts; nothing when none of them has one.
[[nodiscard]] std::optional<std::array<float, 4>> GraphNodeBounds(std::span<const FGraphNodeLayout> Layouts, std::span<const FGraphNodeId> Nodes);
// The nearest node whose center lies within 45 degrees of Direction from the center of From.
[[nodiscard]] std::optional<FGraphNodeId> FindGraphNeighbour(std::span<const FGraphNodeLayout> Layouts, FGraphNodeId From, EGraphDirection Direction);
// Exchanges two nodes' places.
[[nodiscard]] FGraphNodePositions SwapGraphNodes(std::span<const FGraphNodeLayout> Layouts, FGraphNodeId First, FGraphNodeId Second);
// Exchanges what feeds two inputs of one node, failing when either input could not take the other's source.
[[nodiscard]] std::expected<FGraph, FGraphError> SwapGraphInputs(const FGraph& Graph, const FGraphSchema& Schema, FGraphNodeId Node, std::string_view First, std::string_view Second);
// Removes nodes and bridges the gaps: each surviving input a removed node fed takes, where its type allows, whatever fed the removed node's first
// connected input, following chains of removed nodes upstream.
[[nodiscard]] FGraph RemoveGraphNodesKeepingLinks(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeId> Nodes);
// Inserts a node into a link: the link's source feeds the node's first free input that accepts it, and the node's first output the target accepts feeds the target.
[[nodiscard]] std::expected<FGraph, FGraphError> SpliceGraphNode(const FGraph& Graph, const FGraphSchema& Schema, FGraphNodeId Node, const FGraphLink& Link);

// The nodes and the links among them as canonical graph text, for the clipboard.
[[nodiscard]] std::expected<std::string, FGraphError> CopyGraphNodes(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeId> Nodes);
// Adds copied nodes under new identities with their top-left corner at Position, skipping types already at their per-graph limit, and reports the new identities.
[[nodiscard]] std::expected<FGraph, FGraphError> PasteGraphNodes(const FGraph& Graph, const FGraphSchema& Schema, std::string_view Text, FCanvasPoint Position, std::vector<FGraphNodeId>& Pasted);
}
