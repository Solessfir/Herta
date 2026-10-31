#include "Herta/GraphEditor/GraphCanvasFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <set>

namespace Herta
{
namespace
{
struct FInputEdge
{
	// Pin positions relative to each node's top.
	float InputOffset = 0.f;
	std::uint32_t Source = 0;
	float OutputOffset = 0.f;
};

float Snap(const float Value, const float Grid)
{
	return Grid > 0.f ? std::round(Value / Grid) * Grid : Value;
}

// Column edges round leftwards, so rounding never eats into the gap before the next column.
float SnapLeft(const float Value, const float Grid)
{
	return Grid > 0.f ? std::floor(Value / Grid) * Grid : Value;
}

const FGraphNodeLayout* FindLayout(const std::span<const FGraphNodeLayout> Layouts, const std::uint32_t Id)
{
	const auto Found = std::ranges::find(Layouts, FGraphNodeId{Id}, &FGraphNodeLayout::Id);
	return Found == Layouts.end() ? nullptr : &*Found;
}

// A pin's height below its node's top, from the node's layout.
std::optional<float> PinOffset(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeLayout> Layouts, const FGraphPin& Pin, const bool bOutput)
{
	const FGraphNode* const Node = FindGraphNode(Graph, Pin.Node);
	const FGraphNodeDescriptor* const Descriptor = Node ? Schema.FindNode(Node->Type) : nullptr;
	const FGraphNodeLayout* const Layout = FindLayout(Layouts, Pin.Node.Value);
	if (!Descriptor || !Layout)
	{
		return std::nullopt;
	}

	const auto Pins = bOutput ? Descriptor->Outputs : Descriptor->Inputs;
	const auto Found = std::ranges::find(Pins, Pin.Key, &FGraphPinDescriptor::Key);
	const auto& Positions = bOutput ? Layout->Outputs : Layout->Inputs;
	const auto Index = static_cast<std::size_t>(Found - Pins.begin());
	if (Found == Pins.end() || Index >= Positions.size())
	{
		return std::nullopt;
	}

	return Positions[Index][1] - Layout->Rect[1];
}

float Width(const FGraphNodeLayout& Layout)
{
	return Layout.Rect[2] - Layout.Rect[0];
}

float Height(const FGraphNodeLayout& Layout)
{
	return Layout.Rect[3] - Layout.Rect[1];
}

std::vector<const FGraphNodeLayout*> Selected(const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Selection)
{
	std::vector<const FGraphNodeLayout*> Result;
	for (const FGraphNodeId Id : Selection)
	{
		if (const FGraphNodeLayout* const Layout = FindLayout(Layouts, Id.Value))
		{
			Result.push_back(Layout);
		}
	}

	return Result;
}
}

std::vector<FGraphNodeId> GraphFormatRoots(const FGraph& Graph, const std::span<const FGraphNodeId> Selection)
{
	std::set<std::uint32_t> Fed;
	for (const FGraphLink& Link : Graph.Links)
	{
		Fed.insert(Link.From.Node.Value);
	}

	// Without a selection every sink is a root; with one, only the sinks of the trees the selection touches.
	std::set<std::uint32_t> Component;
	if (!Selection.empty())
	{
		std::vector<std::uint32_t> Stack;
		for (const FGraphNodeId Id : Selection)
		{
			Stack.push_back(Id.Value);
		}

		while (!Stack.empty())
		{
			const std::uint32_t Node = Stack.back();
			Stack.pop_back();
			if (!Component.insert(Node).second)
			{
				continue;
			}

			for (const FGraphLink& Link : Graph.Links)
			{
				if (Link.From.Node.Value == Node)
				{
					Stack.push_back(Link.To.Node.Value);
				}
				else if (Link.To.Node.Value == Node)
				{
					Stack.push_back(Link.From.Node.Value);
				}
			}
		}
	}

	std::vector<const FGraphNode*> Roots;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (!Fed.contains(Node.Id.Value) && (Selection.empty() || Component.contains(Node.Id.Value)))
		{
			Roots.push_back(&Node);
		}
	}

	// Top to bottom, so trees stack in the order they already read.
	std::ranges::sort(Roots, {}, [](const FGraphNode* const Node)
	{
		return Node->Position[1];
	});

	std::vector<FGraphNodeId> Result;
	for (const FGraphNode* const Node : Roots)
	{
		Result.push_back(Node->Id);
	}

	return Result;
}

FGraphNodePositions FormatGraph(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Roots, const FGraphFormatSettings& Settings)
{
	// Each node's inputs in pin order, so the first input's tree sits highest, beside it.
	std::map<std::uint32_t, std::vector<FInputEdge>> Inputs;
	std::map<std::uint32_t, std::vector<std::uint32_t>> Consumers;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Node.Type);
		if (!Descriptor)
		{
			continue;
		}

		for (const FGraphPinDescriptor& Pin : Descriptor->Inputs)
		{
			const FGraphPin Input{.Node = Node.Id, .Key = std::string(Pin.Key)};
			const auto Link = std::ranges::find(Graph.Links, Input, &FGraphLink::To);
			if (Link == Graph.Links.end())
			{
				continue;
			}

			const auto InputOffset = PinOffset(Graph, Schema, Layouts, Link->To, false);
			const auto OutputOffset = PinOffset(Graph, Schema, Layouts, Link->From, true);
			if (InputOffset && OutputOffset)
			{
				Inputs[Node.Id.Value].push_back({.InputOffset = *InputOffset, .Source = Link->From.Node.Value, .OutputOffset = *OutputOffset});
				Consumers[Link->From.Node.Value].push_back(Node.Id.Value);
			}
		}
	}

	FGraphNodePositions Positions;
	std::set<std::uint32_t> Formatted;
	std::optional<float> TreesBottom;
	for (const FGraphNodeId Root : Roots)
	{
		const FGraphNodeLayout* const RootLayout = FindLayout(Layouts, Root.Value);
		if (!RootLayout || Formatted.contains(Root.Value))
		{
			continue;
		}

		// The tree: everything upstream of the root that an earlier tree has not already placed.
		std::set<std::uint32_t> Tree;
		std::vector<std::uint32_t> Stack{Root.Value};
		while (!Stack.empty())
		{
			const std::uint32_t Node = Stack.back();
			Stack.pop_back();
			if (Formatted.contains(Node) || !FindLayout(Layouts, Node) || !Tree.insert(Node).second)
			{
				continue;
			}

			for (const FInputEdge& Edge : Inputs[Node])
			{
				Stack.push_back(Edge.Source);
			}
		}

		// A node's column is its longest path to the root, so it sits left of every consumer and links never run backwards.
		std::map<std::uint32_t, int> Depth;
		std::set<std::uint32_t> Visiting;
		const std::function<int(std::uint32_t)> DepthOf = [&](const std::uint32_t Node) -> int
		{
			if (Node == Root.Value)
			{
				return 0;
			}

			if (const auto Found = Depth.find(Node); Found != Depth.end())
			{
				return Found->second;
			}

			// A cycle in domains that allow them falls back to the column it was reached from.
			if (!Visiting.insert(Node).second)
			{
				return 0;
			}

			int Deepest = 0;
			for (const std::uint32_t Consumer : Consumers[Node])
			{
				if (Tree.contains(Consumer))
				{
					Deepest = std::max(Deepest, DepthOf(Consumer) + 1);
				}
			}

			Visiting.erase(Node);
			Depth[Node] = Deepest;
			return Deepest;
		};

		std::map<int, float> ColumnWidth;
		for (const std::uint32_t Node : Tree)
		{
			const int Column = DepthOf(Node);
			ColumnWidth[Column] = std::max(ColumnWidth[Column], Width(*FindLayout(Layouts, Node)));
		}

		// Nodes align their right edges, where their outputs are, to their column's right edge.
		// The root snaps its own corner and keeps its width, so formatting never shifts it by a fraction of a cell.
		std::map<int, float> ColumnRight{{0, Snap(RootLayout->Rect[0], Settings.Grid) + Width(*RootLayout)}};
		for (int Column = 1; ColumnWidth.contains(Column); ++Column)
		{
			ColumnRight[Column] = SnapLeft(ColumnRight[Column - 1] - ColumnWidth[Column - 1] - Settings.ColumnGap, Settings.Grid);
		}

		std::map<int, float> ColumnBottom;
		std::map<std::uint32_t, float> Tops;
		const auto Place = [&](const std::uint32_t Node, const float DesiredTop)
		{
			const int Column = DepthOf(Node);
			const FGraphNodeLayout& Layout = *FindLayout(Layouts, Node);
			const auto Bottom = ColumnBottom.find(Column);
			const float Top = Bottom == ColumnBottom.end() ? DesiredTop : std::max(DesiredTop, Bottom->second + Settings.RowGap);
			Tops[Node] = Top;
			ColumnBottom[Column] = Top + Height(Layout);
			Positions[Node] = {ColumnRight[Column] - Width(Layout), Top};
		};

		const std::function<void(std::uint32_t)> PlaceInputs = [&](const std::uint32_t Node)
		{
			for (const FInputEdge& Edge : Inputs[Node])
			{
				if (Tree.contains(Edge.Source) && !Tops.contains(Edge.Source))
				{
					Place(Edge.Source, Tops[Node] + Edge.InputOffset - Edge.OutputOffset);
					PlaceInputs(Edge.Source);
				}
			}
		};

		Place(Root.Value, Snap(RootLayout->Rect[1], Settings.Grid));
		PlaceInputs(Root.Value);

		// Later trees move down as a whole when they would reach into the ones above.
		float Top = INFINITY, Bottom = -INFINITY;
		for (const std::uint32_t Node : Tree)
		{
			Top = std::min(Top, Tops[Node]);
			Bottom = std::max(Bottom, Tops[Node] + Height(*FindLayout(Layouts, Node)));
		}

		const float Shift = TreesBottom && Top < *TreesBottom + Settings.TreeGap ? Snap(*TreesBottom + Settings.TreeGap - Top, Settings.Grid) : 0.f;
		for (const std::uint32_t Node : Tree)
		{
			Positions[Node][1] += Shift;
			Formatted.insert(Node);
		}

		TreesBottom = std::max(TreesBottom.value_or(-INFINITY), Bottom + Shift);
	}

	return Positions;
}

FGraphNodePositions StraightenGraphLinks(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Selection)
{
	const auto IsSelected = [&Selection](const FGraphNodeId Id)
	{
		return std::ranges::find(Selection, Id) != Selection.end();
	};

	FGraphNodePositions Positions;
	for (const FGraphNodeLayout* const Layout : Selected(Layouts, Selection))
	{
		// The link to an unselected node wins, so a selected chain straightens against what it connects to.
		const FGraphLink* Best = nullptr;
		bool bBestOutput = false;
		for (const FGraphLink& Link : Graph.Links)
		{
			const bool bOutput = Link.From.Node == Layout->Id;
			if (!bOutput && Link.To.Node != Layout->Id)
			{
				continue;
			}

			const bool bOtherSelected = IsSelected(bOutput ? Link.To.Node : Link.From.Node);
			if (!Best || (!bOtherSelected && IsSelected(bBestOutput ? Best->To.Node : Best->From.Node)))
			{
				Best = &Link;
				bBestOutput = bOutput;
			}
		}

		if (!Best)
		{
			continue;
		}

		const auto Own = PinOffset(Graph, Schema, Layouts, bBestOutput ? Best->From : Best->To, bBestOutput);
		const FGraphPin& OtherPin = bBestOutput ? Best->To : Best->From;
		const auto Other = PinOffset(Graph, Schema, Layouts, OtherPin, !bBestOutput);
		const FGraphNodeLayout* const OtherLayout = FindLayout(Layouts, OtherPin.Node.Value);
		if (Own && Other && OtherLayout)
		{
			Positions[Layout->Id.Value] = {Layout->Rect[0], OtherLayout->Rect[1] + *Other - *Own};
		}
	}

	return Positions;
}

FGraphNodePositions AlignGraphNodes(const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Selection, const EGraphAlignment Alignment)
{
	const auto Nodes = Selected(Layouts, Selection);
	FGraphNodePositions Positions;
	if (Nodes.size() < 2)
	{
		return Positions;
	}

	std::array<float, 4> Bounds{INFINITY, INFINITY, -INFINITY, -INFINITY};
	for (const FGraphNodeLayout* const Layout : Nodes)
	{
		Bounds = {std::min(Bounds[0], Layout->Rect[0]), std::min(Bounds[1], Layout->Rect[1]), std::max(Bounds[2], Layout->Rect[2]), std::max(Bounds[3], Layout->Rect[3])};
	}

	for (const FGraphNodeLayout* const Layout : Nodes)
	{
		FCanvasPoint Position{Layout->Rect[0], Layout->Rect[1]};
		switch (Alignment)
		{
			case EGraphAlignment::Top:
				Position[1] = Bounds[1];
				break;
			case EGraphAlignment::Middle:
				Position[1] = (Bounds[1] + Bounds[3] - Height(*Layout)) * 0.5f;
				break;
			case EGraphAlignment::Bottom:
				Position[1] = Bounds[3] - Height(*Layout);
				break;
			case EGraphAlignment::Left:
				Position[0] = Bounds[0];
				break;
			case EGraphAlignment::Center:
				Position[0] = (Bounds[0] + Bounds[2] - Width(*Layout)) * 0.5f;
				break;
			case EGraphAlignment::Right:
				Position[0] = Bounds[2] - Width(*Layout);
				break;
		}

		Positions[Layout->Id.Value] = Position;
	}

	return Positions;
}

FGraphNodePositions DistributeGraphNodes(const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Selection, const bool bHorizontal)
{
	auto Nodes = Selected(Layouts, Selection);
	FGraphNodePositions Positions;
	if (Nodes.size() < 3)
	{
		return Positions;
	}

	const std::size_t Axis = bHorizontal ? 0 : 1;
	std::ranges::sort(Nodes, {}, [Axis](const FGraphNodeLayout* const Layout)
	{
		return Layout->Rect[Axis] + Layout->Rect[Axis + 2];
	});

	float Sizes = 0.f;
	for (const FGraphNodeLayout* const Layout : Nodes)
	{
		Sizes += Layout->Rect[Axis + 2] - Layout->Rect[Axis];
	}

	const float Span = Nodes.back()->Rect[Axis + 2] - Nodes.front()->Rect[Axis];
	const float Gap = (Span - Sizes) / static_cast<float>(Nodes.size() - 1);
	float Edge = Nodes.front()->Rect[Axis];
	for (const FGraphNodeLayout* const Layout : Nodes)
	{
		FCanvasPoint Position{Layout->Rect[0], Layout->Rect[1]};
		Position[Axis] = Edge;
		Positions[Layout->Id.Value] = Position;
		Edge += Layout->Rect[Axis + 2] - Layout->Rect[Axis] + Gap;
	}

	return Positions;
}
}
