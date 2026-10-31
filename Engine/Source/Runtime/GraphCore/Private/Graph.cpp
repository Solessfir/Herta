#include "Herta/GraphCore/Graph.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

namespace Herta
{
namespace
{
std::unexpected<FGraphError> Failed(std::string Message)
{
	return std::unexpected(FGraphError{std::move(Message)});
}

bool IsFinite(const FGraphValue& Value)
{
	if (const float* const Scalar = std::get_if<float>(&Value))
	{
		return std::isfinite(*Scalar);
	}

	if (const auto* const Vector = std::get_if<std::array<float, 4>>(&Value))
	{
		return std::isfinite((*Vector)[0]) && std::isfinite((*Vector)[1]) && std::isfinite((*Vector)[2]) && std::isfinite((*Vector)[3]);
	}

	return true;
}

// Whether Target can be reached from Start by following Edges.
bool Reaches(const std::multimap<std::uint32_t, std::uint32_t>& Edges, const std::uint32_t Start, const std::uint32_t Target)
{
	std::vector<std::uint32_t> Stack{Start};
	std::vector<std::uint32_t> Visited;

	while (!Stack.empty())
	{
		const std::uint32_t Node = Stack.back();
		Stack.pop_back();
		if (Node == Target)
		{
			return true;
		}

		if (std::ranges::contains(Visited, Node))
		{
			continue;
		}

		Visited.push_back(Node);
		const auto [First, Last] = Edges.equal_range(Node);
		for (auto Iterator = First; Iterator != Last; ++Iterator)
		{
			Stack.push_back(Iterator->second);
		}
	}

	return false;
}
}

const FGraphNodeDescriptor* FGraphSchema::FindNode(const std::string_view Type) const
{
	const auto Found = std::ranges::find(Nodes, Type, &FGraphNodeDescriptor::Type);
	return Found == Nodes.end() ? nullptr : &*Found;
}

std::expected<FGraphNode, FGraphError> MakeGraphNode(const FGraphSchema& Schema, const std::string_view Type, const FGraphNodeId Id, const std::array<float, 2> Position)
{
	const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Type);
	if (!Descriptor)
	{
		return Failed(std::format("The {} graph has no node type '{}'", Schema.Domain, Type));
	}

	FGraphNode Node{.Id = Id, .Type = std::string(Type), .Position = Position};
	for (const FGraphPropertyDescriptor& Property : Descriptor->Properties)
	{
		Node.Properties.push_back({.Key = std::string(Property.Key), .Value = Property.Default});
	}

	return Node;
}

FGraphNodeId NextGraphNodeId(const FGraph& Graph)
{
	std::uint32_t Largest = 0;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Largest = std::max(Largest, Node.Id.Value);
	}

	return {Largest + 1};
}

const FGraphNode* FindGraphNode(const FGraph& Graph, const FGraphNodeId Id)
{
	const auto Found = std::ranges::find(Graph.Nodes, Id, &FGraphNode::Id);
	return Found == Graph.Nodes.end() ? nullptr : &*Found;
}

const FGraphPinDescriptor* FindGraphPin(const FGraphNodeDescriptor& Node, const std::string_view Key, const bool bOutput)
{
	const auto Pins = bOutput ? Node.Outputs : Node.Inputs;
	const auto Found = std::ranges::find(Pins, Key, &FGraphPinDescriptor::Key);
	return Found == Pins.end() ? nullptr : &*Found;
}

std::vector<FGraphDiagnostic> ValidateGraph(const FGraph& Graph, const FGraphSchema& Schema)
{
	std::vector<FGraphDiagnostic> Diagnostics;
	const auto Error = [&Diagnostics](const FGraphNodeId Node, std::string Pin, std::string Message)
	{
		Diagnostics.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Pin = std::move(Pin), .Message = std::move(Message)});
	};

	if (Graph.Domain != Schema.Domain)
	{
		Error({}, {}, std::format("Expected a {} graph, found '{}'", Schema.Domain, Graph.Domain));
		return Diagnostics;
	}

	if (Graph.DomainVersion != Schema.Version)
	{
		Error({}, {}, std::format("The graph is {} version {}; this build reads version {}", Schema.Domain, Graph.DomainVersion, Schema.Version));
		return Diagnostics;
	}

	if (Graph.Nodes.size() > MaximumGraphNodes || Graph.Links.size() > MaximumGraphLinks)
	{
		Error({}, {}, "The graph exceeds the node or link limit");
		return Diagnostics;
	}

	std::unordered_map<std::uint32_t, const FGraphNodeDescriptor*> Types;
	std::map<std::string_view, std::uint32_t> Counts;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (Node.Id.Value == 0 || Types.contains(Node.Id.Value))
		{
			Error(Node.Id, {}, "Node identities must be nonzero and unique");
			continue;
		}

		const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Node.Type);
		Types.emplace(Node.Id.Value, Descriptor);
		if (!Descriptor)
		{
			Error(Node.Id, {}, std::format("Unknown node type '{}'", Node.Type));
			continue;
		}

		if (++Counts[Descriptor->Type] == Descriptor->MaximumCount + 1)
		{
			Error(Node.Id, {}, std::format("A graph may hold only {} '{}' node{}", Descriptor->MaximumCount, Descriptor->Name, Descriptor->MaximumCount == 1 ? "" : "s"));
		}

		if (!std::isfinite(Node.Position[0]) || !std::isfinite(Node.Position[1]))
		{
			Error(Node.Id, {}, "Node positions must be finite");
		}

		bool bMatches = Node.Properties.size() == Descriptor->Properties.size();
		for (std::size_t Index = 0; bMatches && Index < Node.Properties.size(); ++Index)
		{
			const FGraphProperty& Property = Node.Properties[Index];
			bMatches = Property.Key == Descriptor->Properties[Index].Key && Property.Value.index() == Descriptor->Properties[Index].Default.index() && IsFinite(Property.Value);
		}

		if (!bMatches)
		{
			Error(Node.Id, {}, std::format("'{}' properties do not match its schema", Descriptor->Name));
		}
	}

	std::multimap<std::uint32_t, std::uint32_t> Edges;
	std::map<FGraphPin, int> Fed;
	for (const FGraphLink& Link : Graph.Links)
	{
		const auto From = Types.find(Link.From.Node.Value);
		const auto To = Types.find(Link.To.Node.Value);
		if (From == Types.end() || To == Types.end())
		{
			Error(Link.To.Node, Link.To.Key, "A link refers to a missing node");
			continue;
		}

		if (!From->second || !To->second)
		{
			continue;
		}

		const FGraphPinDescriptor* const Output = FindGraphPin(*From->second, Link.From.Key, true);
		const FGraphPinDescriptor* const Input = FindGraphPin(*To->second, Link.To.Key, false);
		if (!Output || !Input)
		{
			Error(Link.To.Node, Link.To.Key, "A link refers to a missing pin");
			continue;
		}

		if (Schema.CanConnect && !Schema.CanConnect(Output->Type, Input->Type))
		{
			Error(Link.To.Node, Link.To.Key, std::format("'{}' cannot take '{}'", Input->Name, Output->Name));
		}

		if (++Fed[Link.To] == 2)
		{
			Error(Link.To.Node, Link.To.Key, std::format("'{}' has more than one link", Input->Name));
		}

		Edges.emplace(Link.From.Node.Value, Link.To.Node.Value);
	}

	if (Schema.bAcyclic)
	{
		// Kahn's algorithm: whatever never reaches zero remaining inputs sits on or behind a cycle.
		std::unordered_map<std::uint32_t, std::uint32_t> Remaining;
		for (const auto& [From, To] : Edges)
		{
			++Remaining[To];
		}

		std::vector<std::uint32_t> Ready;
		for (const FGraphNode& Node : Graph.Nodes)
		{
			if (!Remaining.contains(Node.Id.Value))
			{
				Ready.push_back(Node.Id.Value);
			}
		}

		while (!Ready.empty())
		{
			const std::uint32_t Node = Ready.back();
			Ready.pop_back();
			const auto [First, Last] = Edges.equal_range(Node);
			for (auto Iterator = First; Iterator != Last; ++Iterator)
			{
				if (--Remaining[Iterator->second] == 0)
				{
					Ready.push_back(Iterator->second);
				}
			}
		}

		for (const FGraphNode& Node : Graph.Nodes)
		{
			const auto Found = Remaining.find(Node.Id.Value);
			if (Found != Remaining.end() && Found->second != 0)
			{
				Error(Node.Id, {}, "The node is part of a cycle");
			}
		}
	}

	return Diagnostics;
}

std::expected<void, FGraphError> CanLinkGraphPins(const FGraph& Graph, const FGraphSchema& Schema, const FGraphLink& Link)
{
	const FGraphNode* const FromNode = FindGraphNode(Graph, Link.From.Node);
	const FGraphNode* const ToNode = FindGraphNode(Graph, Link.To.Node);
	const FGraphNodeDescriptor* const FromType = FromNode ? Schema.FindNode(FromNode->Type) : nullptr;
	const FGraphNodeDescriptor* const ToType = ToNode ? Schema.FindNode(ToNode->Type) : nullptr;
	const FGraphPinDescriptor* const Output = FromType ? FindGraphPin(*FromType, Link.From.Key, true) : nullptr;
	const FGraphPinDescriptor* const Input = ToType ? FindGraphPin(*ToType, Link.To.Key, false) : nullptr;
	if (!Output || !Input)
	{
		return Failed("Links connect an output pin to an input pin");
	}

	if (Link.From.Node == Link.To.Node)
	{
		return Failed("A node cannot feed itself");
	}

	if (Schema.CanConnect && !Schema.CanConnect(Output->Type, Input->Type))
	{
		return Failed(std::format("'{}' cannot take '{}'", Input->Name, Output->Name));
	}

	// The new link replaces whatever fed the input, so that one does not count towards a cycle.
	if (Schema.bAcyclic)
	{
		std::multimap<std::uint32_t, std::uint32_t> Edges;
		for (const FGraphLink& Existing : Graph.Links)
		{
			if (Existing.To != Link.To)
			{
				Edges.emplace(Existing.From.Node.Value, Existing.To.Node.Value);
			}
		}

		if (Reaches(Edges, Link.To.Node.Value, Link.From.Node.Value))
		{
			return Failed("The link would create a cycle");
		}
	}

	return {};
}
}
