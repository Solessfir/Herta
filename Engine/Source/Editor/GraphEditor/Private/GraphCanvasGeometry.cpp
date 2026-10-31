#include "Herta/GraphEditor/GraphCanvasGeometry.h"

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
float DistanceToSegment(const FCanvasPoint Point, const FCanvasPoint Start, const FCanvasPoint End)
{
	const float DeltaX = End[0] - Start[0];
	const float DeltaY = End[1] - Start[1];
	const float LengthSquared = DeltaX * DeltaX + DeltaY * DeltaY;
	const float Along = LengthSquared > 0.f ? std::clamp(((Point[0] - Start[0]) * DeltaX + (Point[1] - Start[1]) * DeltaY) / LengthSquared, 0.f, 1.f) : 0.f;
	return std::hypot(Point[0] - (Start[0] + DeltaX * Along), Point[1] - (Start[1] + DeltaY * Along));
}

bool Contains(const std::array<float, 4>& Rect, const FCanvasPoint Point)
{
	return Point[0] >= Rect[0] && Point[0] <= Rect[2] && Point[1] >= Rect[1] && Point[1] <= Rect[3];
}

const FGraphNodeLayout* FindLayout(const std::span<const FGraphNodeLayout> Layouts, const FGraphNodeId Id)
{
	const auto Found = std::ranges::find(Layouts, Id, &FGraphNodeLayout::Id);
	return Found == Layouts.end() ? nullptr : &*Found;
}
}

const FCanvasPoint* FindGraphPinPosition(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeLayout> Layouts, const FGraphPin& Pin, const bool bOutput)
{
	const FGraphNode* const Node = FindGraphNode(Graph, Pin.Node);
	const FGraphNodeDescriptor* const Descriptor = Node ? Schema.FindNode(Node->Type) : nullptr;
	const FGraphNodeLayout* const Layout = FindLayout(Layouts, Pin.Node);
	if (!Descriptor || !Layout)
	{
		return nullptr;
	}

	const auto Pins = bOutput ? Descriptor->Outputs : Descriptor->Inputs;
	const auto Found = std::ranges::find(Pins, Pin.Key, &FGraphPinDescriptor::Key);
	const auto& Positions = bOutput ? Layout->Outputs : Layout->Inputs;
	const auto Index = static_cast<std::size_t>(Found - Pins.begin());
	return Found == Pins.end() || Index >= Positions.size() ? nullptr : &Positions[Index];
}

FCanvasPoint FGraphCanvasView::ToScreen(const FCanvasPoint Canvas) const
{
	return {Origin[0] + Pan[0] + Canvas[0] * Zoom, Origin[1] + Pan[1] + Canvas[1] * Zoom};
}

FCanvasPoint FGraphCanvasView::ToCanvas(const FCanvasPoint Screen) const
{
	return {(Screen[0] - Origin[0] - Pan[0]) / Zoom, (Screen[1] - Origin[1] - Pan[1]) / Zoom};
}

void FGraphCanvasView::ZoomAbout(const FCanvasPoint Screen, const float Factor)
{
	const FCanvasPoint Anchor = ToCanvas(Screen);
	Zoom = std::clamp(Zoom * Factor, MinimumGraphCanvasZoom, MaximumGraphCanvasZoom);
	Pan = {Screen[0] - Origin[0] - Anchor[0] * Zoom, Screen[1] - Origin[1] - Anchor[1] * Zoom};
}

FGraphNodeLayout LayoutGraphNode(const FGraphNode& Node, const FGraphNodeDescriptor& Descriptor, const FCanvasPoint Offset, const FGraphCanvasMetrics& Metrics, const std::function<float(std::string_view)>& MeasureText)
{
	FGraphNodeLayout Layout{.Id = Node.Id, .PropertyRows = Descriptor.Properties.size()};
	float Width = std::max(Metrics.MinimumWidth, MeasureText(Descriptor.Name) + 2.f * Metrics.Padding);
	const std::size_t PinRows = std::max(Descriptor.Inputs.size(), Descriptor.Outputs.size());
	for (std::size_t Row = 0; Row < PinRows; ++Row)
	{
		const float Input = Row < Descriptor.Inputs.size() ? MeasureText(Descriptor.Inputs[Row].Name) : 0.f;
		const float Output = Row < Descriptor.Outputs.size() ? MeasureText(Descriptor.Outputs[Row].Name) : 0.f;
		Width = std::max(Width, Input + Output + Metrics.LabelGap + 2.f * (Metrics.Padding + Metrics.PinRadius));
	}

	for (const FGraphPropertyDescriptor& Property : Descriptor.Properties)
	{
		Width = std::max(Width, MeasureText(Property.Name) + Metrics.LabelGap + 64.f + 2.f * Metrics.Padding);
	}

	const float Left = Node.Position[0] + Offset[0];
	const float Top = Node.Position[1] + Offset[1];
	const float Height = Metrics.TitleHeight + Metrics.RowHeight * static_cast<float>(PinRows + Layout.PropertyRows) + Metrics.Padding * 0.5f;
	Layout.Rect = {Left, Top, Left + Width, Top + Height};
	for (std::size_t Row = 0; Row < Descriptor.Inputs.size(); ++Row)
	{
		Layout.Inputs.push_back({Left, Top + Metrics.TitleHeight + Metrics.RowHeight * (static_cast<float>(Row) + 0.5f)});
	}

	for (std::size_t Row = 0; Row < Descriptor.Outputs.size(); ++Row)
	{
		Layout.Outputs.push_back({Left + Width, Top + Metrics.TitleHeight + Metrics.RowHeight * (static_cast<float>(Row) + 0.5f)});
	}

	return Layout;
}

std::array<FCanvasPoint, 4> GraphLinkCurve(const FCanvasPoint Output, const FCanvasPoint Input)
{
	const float Pull = std::max(40.f, std::abs(Input[0] - Output[0]) * 0.5f);
	return {Output, FCanvasPoint{Output[0] + Pull, Output[1]}, FCanvasPoint{Input[0] - Pull, Input[1]}, Input};
}

float DistanceToGraphLink(const FCanvasPoint Point, const FCanvasPoint Output, const FCanvasPoint Input)
{
	constexpr int Segments = 24;
	const auto Curve = GraphLinkCurve(Output, Input);
	const auto Evaluate = [&Curve](const float T)
	{
		const float U = 1.f - T;
		const float A = U * U * U, B = 3.f * U * U * T, C = 3.f * U * T * T, D = T * T * T;
		return FCanvasPoint{A * Curve[0][0] + B * Curve[1][0] + C * Curve[2][0] + D * Curve[3][0], A * Curve[0][1] + B * Curve[1][1] + C * Curve[2][1] + D * Curve[3][1]};
	};

	float Nearest = INFINITY;
	FCanvasPoint Previous = Curve[0];
	for (int Segment = 1; Segment <= Segments; ++Segment)
	{
		const FCanvasPoint Next = Evaluate(static_cast<float>(Segment) / Segments);
		Nearest = std::min(Nearest, DistanceToSegment(Point, Previous, Next));
		Previous = Next;
	}

	return Nearest;
}

FGraphCanvasHitResult HitTestGraphCanvas(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeLayout> Layouts, const FCanvasPoint Point, const float Tolerance)
{
	for (auto Layout = Layouts.rbegin(); Layout != Layouts.rend(); ++Layout)
	{
		const FGraphNode* const Node = FindGraphNode(Graph, Layout->Id);
		const FGraphNodeDescriptor* const Descriptor = Node ? Schema.FindNode(Node->Type) : nullptr;
		if (!Descriptor)
		{
			continue;
		}

		for (const bool bOutput : {true, false})
		{
			const auto& Positions = bOutput ? Layout->Outputs : Layout->Inputs;
			const auto Pins = bOutput ? Descriptor->Outputs : Descriptor->Inputs;
			for (std::size_t Index = 0; Index < Positions.size(); ++Index)
			{
				if (std::hypot(Point[0] - Positions[Index][0], Point[1] - Positions[Index][1]) <= Tolerance)
				{
					return {.Kind = EGraphCanvasHit::Pin, .Node = Layout->Id, .Pin = std::string(Pins[Index].Key), .bOutput = bOutput};
				}
			}
		}

		if (Contains(Layout->Rect, Point))
		{
			return {.Kind = EGraphCanvasHit::Node, .Node = Layout->Id};
		}
	}

	for (std::size_t Index = 0; Index < Graph.Links.size(); ++Index)
	{
		const FGraphLink& Link = Graph.Links[Index];
		const FCanvasPoint* const Output = FindGraphPinPosition(Graph, Schema, Layouts, Link.From, true);
		const FCanvasPoint* const Input = FindGraphPinPosition(Graph, Schema, Layouts, Link.To, false);
		if (Output && Input && DistanceToGraphLink(Point, *Output, *Input) <= Tolerance)
		{
			return {.Kind = EGraphCanvasHit::Link, .Link = Index};
		}
	}

	return {};
}

std::vector<FGraphNodeId> GraphNodesInBox(const std::span<const FGraphNodeLayout> Layouts, const FCanvasPoint First, const FCanvasPoint Second)
{
	const float Left = std::min(First[0], Second[0]), Right = std::max(First[0], Second[0]);
	const float Top = std::min(First[1], Second[1]), Bottom = std::max(First[1], Second[1]);
	std::vector<FGraphNodeId> Inside;
	for (const FGraphNodeLayout& Layout : Layouts)
	{
		if (Layout.Rect[0] <= Right && Layout.Rect[2] >= Left && Layout.Rect[1] <= Bottom && Layout.Rect[3] >= Top)
		{
			Inside.push_back(Layout.Id);
		}
	}

	return Inside;
}
}
