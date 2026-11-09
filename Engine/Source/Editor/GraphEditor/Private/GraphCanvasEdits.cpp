#include "Herta/GraphEditor/GraphCanvasEdits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace Herta
{
namespace
{
bool Contains(const std::span<const FGraphNodeId> Nodes, const FGraphNodeId Id)
{
	return std::ranges::find(Nodes, Id) != Nodes.end();
}

const FGraphNodeLayout* FindLayout(const std::span<const FGraphNodeLayout> Layouts, const FGraphNodeId Id)
{
	const auto Found = std::ranges::find(Layouts, Id, &FGraphNodeLayout::Id);
	return Found == Layouts.end() ? nullptr : &*Found;
}

const FGraphNodeDescriptor* FindDescriptor(const FGraph& Graph, const FGraphSchema& Schema, const FGraphNodeId Id)
{
	const FGraphNode* const Node = FindGraphNode(Graph, Id);
	return Node ? Schema.FindNode(Node->Type) : nullptr;
}

const FGraphLink* FindInputLink(const FGraph& Graph, const FGraphPin& Input)
{
	const auto Found = std::ranges::find(Graph.Links, Input, &FGraphLink::To);
	return Found == Graph.Links.end() ? nullptr : &*Found;
}

// The surviving outputs that fed a removed node, nearest first, following chains of removed nodes through their connected inputs in order.
void CollectSources(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeId> Removed, const FGraphNodeId Node, std::set<std::uint32_t>& Visited, std::vector<FGraphPin>& Sources)
{
	const FGraphNodeDescriptor* const Descriptor = FindDescriptor(Graph, Schema, Node);
	if (!Descriptor || !Visited.insert(Node.Value).second)
	{
		return;
	}

	for (const FGraphPinDescriptor& Input : Descriptor->Inputs)
	{
		const FGraphLink* const Link = FindInputLink(Graph, {.Node = Node, .Key = std::string(Input.Key)});
		if (!Link)
		{
			continue;
		}

		if (Contains(Removed, Link->From.Node))
		{
			CollectSources(Graph, Schema, Removed, Link->From.Node, Visited, Sources);
		}
		else
		{
			Sources.push_back(Link->From);
		}
	}
}
}

std::vector<FGraphNodeId> GraphConnectedNodes(const FGraph& Graph, const std::span<const FGraphNodeId> Seeds)
{
	std::vector<FGraphNodeId> Found(Seeds.begin(), Seeds.end());
	for (std::size_t Index = 0; Index < Found.size(); ++Index)
	{
		const FGraphNodeId Current = Found[Index];
		for (const FGraphLink& Link : Graph.Links)
		{
			const FGraphNodeId Other = Link.From.Node == Current ? Link.To.Node : Link.To.Node == Current ? Link.From.Node
			                                                                                              : FGraphNodeId{};
			if (Other.Value != 0 && !Contains(Found, Other))
			{
				Found.push_back(Other);
			}
		}
	}

	return Found;
}

std::optional<std::array<float, 4>> GraphNodeBounds(const std::span<const FGraphNodeLayout> Layouts, const std::span<const FGraphNodeId> Nodes)
{
	std::optional<std::array<float, 4>> Bounds;
	for (const FGraphNodeId Node : Nodes)
	{
		if (const FGraphNodeLayout* const Layout = FindLayout(Layouts, Node))
		{
			const auto& Rect = Layout->Rect;
			Bounds = Bounds ? std::array{std::min((*Bounds)[0], Rect[0]), std::min((*Bounds)[1], Rect[1]), std::max((*Bounds)[2], Rect[2]), std::max((*Bounds)[3], Rect[3])} : Rect;
		}
	}

	return Bounds;
}

std::optional<FGraphNodeId> FindGraphNeighbour(const std::span<const FGraphNodeLayout> Layouts, const FGraphNodeId From, const EGraphDirection Direction)
{
	const FGraphNodeLayout* const Origin = FindLayout(Layouts, From);
	if (!Origin)
	{
		return std::nullopt;
	}

	const auto Center = [](const FGraphNodeLayout& Layout)
	{
		return FCanvasPoint{(Layout.Rect[0] + Layout.Rect[2]) * 0.5f, (Layout.Rect[1] + Layout.Rect[3]) * 0.5f};
	};

	const FCanvasPoint Start = Center(*Origin);
	std::optional<FGraphNodeId> Nearest;
	float NearestDistance = INFINITY;
	for (const FGraphNodeLayout& Layout : Layouts)
	{
		const FCanvasPoint End = Center(Layout);
		const float DeltaX = End[0] - Start[0];
		const float DeltaY = End[1] - Start[1];
		const bool bHorizontal = Direction == EGraphDirection::Left || Direction == EGraphDirection::Right;
		const float Along = bHorizontal ? (Direction == EGraphDirection::Right ? DeltaX : -DeltaX) : (Direction == EGraphDirection::Down ? DeltaY : -DeltaY);
		const float Across = std::abs(bHorizontal ? DeltaY : DeltaX);
		const float Distance = std::hypot(DeltaX, DeltaY);
		if (Layout.Id != From && Along > 0.f && Across <= Along && Distance < NearestDistance)
		{
			Nearest = Layout.Id;
			NearestDistance = Distance;
		}
	}

	return Nearest;
}

FGraphNodePositions SwapGraphNodes(const std::span<const FGraphNodeLayout> Layouts, const FGraphNodeId First, const FGraphNodeId Second)
{
	const FGraphNodeLayout* const A = FindLayout(Layouts, First);
	const FGraphNodeLayout* const B = FindLayout(Layouts, Second);
	if (!A || !B || First == Second)
	{
		return {};
	}

	return {{First.Value, {B->Rect[0], B->Rect[1]}}, {Second.Value, {A->Rect[0], A->Rect[1]}}};
}

std::expected<FGraph, FGraphError> SwapGraphInputs(const FGraph& Graph, const FGraphSchema& Schema, const FGraphNodeId Node, const std::string_view First, const std::string_view Second)
{
	const FGraphPin FirstPin{.Node = Node, .Key = std::string(First)};
	const FGraphPin SecondPin{.Node = Node, .Key = std::string(Second)};
	const FGraphLink* const FirstLink = FindInputLink(Graph, FirstPin);
	const FGraphLink* const SecondLink = FindInputLink(Graph, SecondPin);
	if (First == Second || (!FirstLink && !SecondLink))
	{
		return std::unexpected(FGraphError{"Neither input has a link to swap"});
	}

	FGraph After = Graph;
	std::erase_if(After.Links, [&](const FGraphLink& Link)
	{
		return Link.To == FirstPin || Link.To == SecondPin;
	});

	for (const auto& [Link, Target] : {std::pair{FirstLink, SecondPin}, std::pair{SecondLink, FirstPin}})
	{
		if (!Link)
		{
			continue;
		}

		const FGraphLink Moved{.From = Link->From, .To = Target};
		if (const auto Allowed = CanLinkGraphPins(After, Schema, Moved); !Allowed)
		{
			return std::unexpected(Allowed.error());
		}

		After.Links.push_back(Moved);
	}

	return After;
}

FGraph RemoveGraphNodesKeepingLinks(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeId> Nodes)
{
	FGraph After = Graph;
	std::erase_if(After.Nodes, [Nodes](const FGraphNode& Node)
	{
		return Contains(Nodes, Node.Id);
	});

	std::erase_if(After.Links, [Nodes](const FGraphLink& Link)
	{
		return Contains(Nodes, Link.From.Node) || Contains(Nodes, Link.To.Node);
	});

	std::erase_if(After.Comments, [Nodes](const FGraphComment& Comment)
	{
		return Contains(Nodes, Comment.Id);
	});

	for (const FGraphLink& Link : Graph.Links)
	{
		if (!Contains(Nodes, Link.From.Node) || Contains(Nodes, Link.To.Node))
		{
			continue;
		}

		std::set<std::uint32_t> Visited;
		std::vector<FGraphPin> Sources;
		CollectSources(Graph, Schema, Nodes, Link.From.Node, Visited, Sources);
		for (const FGraphPin& Source : Sources)
		{
			const FGraphLink Bridge{.From = Source, .To = Link.To};
			if (CanLinkGraphPins(After, Schema, Bridge))
			{
				After.Links.push_back(Bridge);
				break;
			}
		}
	}

	return After;
}

std::expected<FGraph, FGraphError> SpliceGraphNode(const FGraph& Graph, const FGraphSchema& Schema, const FGraphNodeId Node, const FGraphLink& Link)
{
	const FGraphNodeDescriptor* const Descriptor = FindDescriptor(Graph, Schema, Node);
	if (!Descriptor || Link.From.Node == Node || Link.To.Node == Node || std::ranges::find(Graph.Links, Link) == Graph.Links.end())
	{
		return std::unexpected(FGraphError{"Only a node outside the link can be inserted into it"});
	}

	FGraph After = Graph;
	std::erase(After.Links, Link);
	std::optional<FGraphLink> Into;
	for (const FGraphPinDescriptor& Input : Descriptor->Inputs)
	{
		const FGraphLink Candidate{.From = Link.From, .To = {.Node = Node, .Key = std::string(Input.Key)}};
		if (!FindInputLink(After, Candidate.To) && CanLinkGraphPins(After, Schema, Candidate))
		{
			Into = Candidate;
			break;
		}
	}

	if (!Into)
	{
		return std::unexpected(FGraphError{"The node has no free input that accepts the link's source"});
	}

	After.Links.push_back(*Into);
	for (const FGraphPinDescriptor& Output : Descriptor->Outputs)
	{
		const FGraphLink Candidate{.From = {.Node = Node, .Key = std::string(Output.Key)}, .To = Link.To};
		if (CanLinkGraphPins(After, Schema, Candidate))
		{
			After.Links.push_back(Candidate);
			return After;
		}
	}

	return std::unexpected(FGraphError{"The node has no output the link's target accepts"});
}

namespace
{
// Adds From -> To after dropping any link already feeding the input, checking it against the graph built so far.
std::expected<void, FGraphError> AddLink(FGraph& Graph, const FGraphSchema& Schema, const FGraphLink& Link)
{
	std::erase_if(Graph.Links, [&Link](const FGraphLink& Existing)
	{
		return Existing.To == Link.To;
	});

	if (auto Allowed = CanLinkGraphPins(Graph, Schema, Link); !Allowed)
	{
		return Allowed;
	}

	Graph.Links.push_back(Link);
	return {};
}

FGraph WithoutLinks(const FGraph& Graph, const std::span<const FGraphLink> Detached)
{
	FGraph After = Graph;
	std::erase_if(After.Links, [Detached](const FGraphLink& Link)
	{
		return std::ranges::find(Detached, Link) != Detached.end();
	});

	return After;
}
}

std::expected<FGraph, FGraphError> RelinkGraphPins(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphPin> Anchors, const bool bAnchorsAreOutputs, const FGraphPin& Target, const std::span<const FGraphLink> Detached)
{
	FGraph After = WithoutLinks(Graph, Detached);
	for (const FGraphPin& Anchor : Anchors)
	{
		const FGraphLink Link = bAnchorsAreOutputs ? FGraphLink{.From = Anchor, .To = Target} : FGraphLink{.From = Target, .To = Anchor};
		if (auto Added = AddLink(After, Schema, Link); !Added)
		{
			return std::unexpected(Added.error());
		}
	}

	return After;
}

std::expected<FGraph, FGraphError> ConnectGraphPinsToNode(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphPin> Anchors, const bool bAnchorsAreOutputs, const FGraphNodeId Node, const std::span<const FGraphLink> Detached)
{
	const FGraphNodeDescriptor* const Descriptor = FindDescriptor(Graph, Schema, Node);
	if (!Descriptor)
	{
		return std::unexpected(FGraphError{"The node to connect is missing"});
	}

	FGraph After = WithoutLinks(Graph, Detached);
	for (const FGraphPin& Anchor : Anchors)
	{
		bool bConnected = false;
		for (const FGraphPinDescriptor& Pin : bAnchorsAreOutputs ? Descriptor->Inputs : Descriptor->Outputs)
		{
			const FGraphPin Target{.Node = Node, .Key = std::string(Pin.Key)};
			const FGraphLink Link = bAnchorsAreOutputs ? FGraphLink{.From = Anchor, .To = Target} : FGraphLink{.From = Target, .To = Anchor};
			// Prefer an input nothing feeds yet, so several anchors spread over the node instead of replacing each other.
			if ((!bAnchorsAreOutputs || !FindInputLink(After, Target)) && CanLinkGraphPins(After, Schema, Link))
			{
				After.Links.push_back(Link);
				bConnected = true;
				break;
			}
		}

		if (!bConnected)
		{
			return std::unexpected(FGraphError{"The new node has no pin that accepts the link"});
		}
	}

	return After;
}

FGraph BreakGraphPinLinks(const FGraph& Graph, const FGraphPin& Pin, const bool bOutput)
{
	FGraph After = Graph;
	std::erase_if(After.Links, [&Pin, bOutput](const FGraphLink& Link)
	{
		return (bOutput ? Link.From : Link.To) == Pin;
	});

	return After;
}

std::expected<std::string, FGraphError> CopyGraphNodes(const FGraph& Graph, const FGraphSchema& Schema, const std::span<const FGraphNodeId> Nodes)
{
	FGraph Copied{.Domain = Graph.Domain, .DomainVersion = Graph.DomainVersion};
	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (Contains(Nodes, Node.Id))
		{
			Copied.Nodes.push_back(Node);
		}
	}

	for (const FGraphLink& Link : Graph.Links)
	{
		if (Contains(Nodes, Link.From.Node) && Contains(Nodes, Link.To.Node))
		{
			Copied.Links.push_back(Link);
		}
	}

	for (const FGraphComment& Comment : Graph.Comments)
	{
		if (Contains(Nodes, Comment.Id))
		{
			Copied.Comments.push_back(Comment);
		}
	}

	if (Copied.Nodes.empty() && Copied.Comments.empty())
	{
		return std::unexpected(FGraphError{"Nothing is selected to copy"});
	}

	return SerializeGraph(Copied, Schema);
}

std::expected<FGraph, FGraphError> PasteGraphNodes(const FGraph& Graph, const FGraphSchema& Schema, const std::string_view Text, const FCanvasPoint Position, std::vector<FGraphNodeId>& Pasted)
{
	auto Copied = DeserializeGraph(Text, Schema);
	if (!Copied)
	{
		return std::unexpected(Copied.error());
	}

	if (Copied->Domain != Graph.Domain)
	{
		return std::unexpected(FGraphError{"The clipboard holds nodes of another graph domain"});
	}

	FGraph After = Graph;
	std::erase_if(Copied->Nodes, [&](const FGraphNode& Node)
	{
		const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Node.Type);
		return !Descriptor || static_cast<std::uint32_t>(std::ranges::count(After.Nodes, Node.Type, &FGraphNode::Type)) >= Descriptor->MaximumCount;
	});

	FCanvasPoint Corner{INFINITY, INFINITY};
	for (const FGraphNode& Node : Copied->Nodes)
	{
		Corner = {std::min(Corner[0], Node.Position[0]), std::min(Corner[1], Node.Position[1])};
	}

	for (const FGraphComment& Comment : Copied->Comments)
	{
		Corner = {std::min(Corner[0], Comment.Rect[0]), std::min(Corner[1], Comment.Rect[1])};
	}

	std::map<std::uint32_t, FGraphNodeId> Identities;
	FGraphNodeId Next = NextGraphNodeId(Graph);
	Pasted.clear();
	for (FGraphNode Node : Copied->Nodes)
	{
		Identities.emplace(Node.Id.Value, Next);
		Node.Id = Next;
		Node.Position = {Position[0] + Node.Position[0] - Corner[0], Position[1] + Node.Position[1] - Corner[1]};
		Pasted.push_back(Next);
		After.Nodes.push_back(std::move(Node));
		Next.Value += 1;
	}

	for (const FGraphLink& Link : Copied->Links)
	{
		const auto From = Identities.find(Link.From.Node.Value);
		const auto To = Identities.find(Link.To.Node.Value);
		if (From != Identities.end() && To != Identities.end())
		{
			After.Links.push_back({.From = {.Node = From->second, .Key = Link.From.Key}, .To = {.Node = To->second, .Key = Link.To.Key}});
		}
	}

	for (FGraphComment Comment : Copied->Comments)
	{
		const float DeltaX = Position[0] - Corner[0];
		const float DeltaY = Position[1] - Corner[1];
		Comment.Id = Next;
		Comment.Rect = {Comment.Rect[0] + DeltaX, Comment.Rect[1] + DeltaY, Comment.Rect[2] + DeltaX, Comment.Rect[3] + DeltaY};
		Pasted.push_back(Next);
		After.Comments.push_back(std::move(Comment));
		Next.Value += 1;
	}

	if (Pasted.empty())
	{
		return std::unexpected(FGraphError{"Every copied node is at its per-graph limit"});
	}

	return After;
}
}
