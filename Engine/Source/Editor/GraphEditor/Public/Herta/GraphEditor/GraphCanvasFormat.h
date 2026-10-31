#pragma once

#include "Herta/GraphEditor/GraphCanvasGeometry.h"

#include <map>
#include <span>
#include <vector>

namespace Herta
{
struct FGraphFormatSettings
{
	// Room between a node and the column of nodes feeding it.
	float ColumnGap = 80.f;
	// Room between nodes stacked in one column.
	float RowGap = 24.f;
	// Room between separate trees when the whole graph formats.
	float TreeGap = 64.f;
	// Columns and roots land on multiples of it; zero leaves them unrounded. Other heights follow their pins, so links stay straight.
	float Grid = 16.f;
};

using FGraphNodePositions = std::map<std::uint32_t, FCanvasPoint>;

enum class EGraphAlignment : std::uint8_t
{
	Top,
	Middle,
	Bottom,
	Left,
	Center,
	Right,
};

// Layouts must cover the graph's nodes at their stored positions. Each function returns new positions for the nodes it moves and leaves the rest out.

// Lays out the trees that feed Roots, upstream from them: every node sits in a column left of all its consumers, so links run left to right, and is
// placed where its output pin lines up with the first input it feeds, below whatever already holds that column. Roots keep their places, except
// that a later tree moves below the ones formatted before it.
[[nodiscard]] FGraphNodePositions FormatGraph(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeLayout> Layouts, std::span<const FGraphNodeId> Roots, const FGraphFormatSettings& Settings);
// The roots Format uses: with a selection, the most downstream nodes of each tree a selected node belongs to; without one, every node nothing reads.
[[nodiscard]] std::vector<FGraphNodeId> GraphFormatRoots(const FGraph& Graph, std::span<const FGraphNodeId> Selection);
// Moves each selected node vertically so its first link, preferring one to an unselected node, runs straight.
[[nodiscard]] FGraphNodePositions StraightenGraphLinks(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeLayout> Layouts, std::span<const FGraphNodeId> Selection);
[[nodiscard]] FGraphNodePositions AlignGraphNodes(std::span<const FGraphNodeLayout> Layouts, std::span<const FGraphNodeId> Selection, EGraphAlignment Alignment);
// Spaces three or more nodes so the gaps between them are equal, keeping the outermost two in place.
[[nodiscard]] FGraphNodePositions DistributeGraphNodes(std::span<const FGraphNodeLayout> Layouts, std::span<const FGraphNodeId> Selection, bool bHorizontal);
}
