#pragma once

#include "Herta/GraphCore/Graph.h"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
using FCanvasPoint = std::array<float, 2>;

// Canvas units at zoom 1 equal logical pixels; everything below is in canvas units unless it says screen.
struct FGraphCanvasMetrics
{
	float TitleHeight = 28.f;
	float RowHeight = 22.f;
	float Padding = 10.f;
	float PinRadius = 5.f;
	// Room between an input label and an output label on one row.
	float LabelGap = 24.f;
	float MinimumWidth = 120.f;
	float Rounding = 6.f;
	float GridSpacing = 32.f;
};

inline constexpr float MinimumGraphCanvasZoom = 0.2f;
inline constexpr float MaximumGraphCanvasZoom = 2.5f;

// Screen = Origin + Pan + Canvas * Zoom, with Origin the canvas widget's top-left corner on screen.
struct FGraphCanvasView
{
	[[nodiscard]] FCanvasPoint ToScreen(FCanvasPoint Canvas) const;
	[[nodiscard]] FCanvasPoint ToCanvas(FCanvasPoint Screen) const;
	// Zooms by Factor about a screen point, keeping the canvas point under it in place.
	void ZoomAbout(FCanvasPoint Screen, float Factor);

	FCanvasPoint Origin{};
	FCanvasPoint Pan{};
	float Zoom = 1.f;
};

struct FGraphNodeLayout
{
	FGraphNodeId Id{};
	// Min x, min y, max x, max y.
	std::array<float, 4> Rect{};
	std::vector<FCanvasPoint> Inputs{};
	std::vector<FCanvasPoint> Outputs{};
	// Rows below the pins that preview each property.
	std::size_t PropertyRows = 0;
};

// Lays out a node at its position plus Offset, which carries a drag in progress. MeasureText returns a label's width in canvas units.
[[nodiscard]] FGraphNodeLayout LayoutGraphNode(const FGraphNode& Node, const FGraphNodeDescriptor& Descriptor, FCanvasPoint Offset, const FGraphCanvasMetrics& Metrics, const std::function<float(std::string_view)>& MeasureText);

// Where a pin sits, or nothing when its node, its type, or its layout is missing.
[[nodiscard]] const FCanvasPoint* FindGraphPinPosition(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeLayout> Layouts, const FGraphPin& Pin, bool bOutput);

// The cubic curve from an output pin to an input pin: control points pull horizontally so links leave and enter their pins level.
[[nodiscard]] std::array<FCanvasPoint, 4> GraphLinkCurve(FCanvasPoint Output, FCanvasPoint Input);
[[nodiscard]] float DistanceToGraphLink(FCanvasPoint Point, FCanvasPoint Output, FCanvasPoint Input);

enum class EGraphCanvasHit : std::uint8_t
{
	None,
	Pin,
	Node,
	Link,
};

struct FGraphCanvasHitResult
{
	EGraphCanvasHit Kind = EGraphCanvasHit::None;
	FGraphNodeId Node{};
	std::string Pin{};
	bool bOutput = false;
	// Index into the graph's links for a link hit.
	std::size_t Link = 0;
};

// Pins win over the nodes they sit on, nodes over the links passing under them, and later nodes over earlier ones, as they draw on top.
// Tolerance is the pick radius for pins and links in canvas units.
[[nodiscard]] FGraphCanvasHitResult HitTestGraphCanvas(const FGraph& Graph, const FGraphSchema& Schema, std::span<const FGraphNodeLayout> Layouts, FCanvasPoint Point, float Tolerance);

// Nodes whose rectangle overlaps the box spanned by two corners in any order.
[[nodiscard]] std::vector<FGraphNodeId> GraphNodesInBox(std::span<const FGraphNodeLayout> Layouts, FCanvasPoint First, FCanvasPoint Second);
}
