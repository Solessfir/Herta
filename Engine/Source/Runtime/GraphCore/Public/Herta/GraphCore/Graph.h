#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Herta
{
struct FGraphError
{
	std::string Message{};
};

// Unique within its graph, so links, selection, and transactions keep pointing at the same node across edits and reloads. Zero is invalid.
struct FGraphNodeId
{
	auto operator<=>(const FGraphNodeId&) const = default;

	std::uint32_t Value = 0;
};

// Property values are typed by the schema's default, so files never have to tell an integer from a float on their own.
using FGraphValue = std::variant<bool, std::int32_t, float, std::array<float, 4>, std::string>;

struct FGraphProperty
{
	bool operator==(const FGraphProperty&) const = default;

	std::string Key{};
	FGraphValue Value{};
};

struct FGraphNode
{
	bool operator==(const FGraphNode&) const = default;

	FGraphNodeId Id{};
	std::string Type{};
	// Canvas units; layout only, never semantics.
	std::array<float, 2> Position{};
	// Exactly the schema's properties, in its order.
	std::vector<FGraphProperty> Properties{};
};

struct FGraphPin
{
	auto operator<=>(const FGraphPin&) const = default;

	FGraphNodeId Node{};
	std::string Key{};
};

// Data and control both flow from an output pin to an input pin.
struct FGraphLink
{
	auto operator<=>(const FGraphLink&) const = default;

	FGraphPin From{};
	FGraphPin To{};
};

// A titled box behind nodes for notes and grouping; layout only, never semantics.
struct FGraphComment
{
	bool operator==(const FGraphComment&) const = default;

	// Shares the node identity space, so one selection can hold nodes and comments.
	FGraphNodeId Id{};
	std::string Text{};
	// Canvas units: min x, min y, max x, max y.
	std::array<float, 4> Rect{};
};

struct FGraph
{
	bool operator==(const FGraph&) const = default;

	std::string Domain{};
	std::uint32_t DomainVersion = 0;
	std::vector<FGraphNode> Nodes{};
	std::vector<FGraphLink> Links{};
	std::vector<FGraphComment> Comments{};
};

struct FGraphPinDescriptor
{
	std::string_view Key{};
	std::string_view Name{};
	// A domain-defined type; the schema decides which types may connect.
	std::uint32_t Type = 0;
};

struct FGraphPropertyDescriptor
{
	std::string_view Key{};
	std::string_view Name{};
	FGraphValue Default{};
};

struct FGraphNodeDescriptor
{
	std::string_view Type{};
	std::string_view Name{};
	std::string_view Category{};
	std::span<const FGraphPinDescriptor> Inputs{};
	std::span<const FGraphPinDescriptor> Outputs{};
	std::span<const FGraphPropertyDescriptor> Properties{};
	// Output nodes allow one per graph.
	std::uint32_t MaximumCount = UINT32_MAX;
	// Only routes one input to one output, so editors draw it as a small knot on the wire.
	bool bReroute = false;
};

// A domain's node types and connection rules. Schemas are static data owned by the domain, so descriptors refer to them by view.
struct FGraphSchema
{
	[[nodiscard]] const FGraphNodeDescriptor* FindNode(std::string_view Type) const;

	std::string_view Domain{};
	std::uint32_t Version = 0;
	std::span<const FGraphNodeDescriptor> Nodes{};
	// Whether an output of type From may feed an input of type To; domains allow conversions such as a scalar into a vector.
	bool (*CanConnect)(std::uint32_t From, std::uint32_t To) = nullptr;
	// Dataflow domains reject cycles; execution graphs that loop through explicit nodes may allow them.
	bool bAcyclic = true;
	// Brings a graph saved by an older domain version up to Version when it loads. Properties a version added already have their defaults by
	// then, so a schema that only added properties or node types upgrades with nothing to do. Without it, older graphs are rejected.
	std::expected<void, FGraphError> (*Upgrade)(FGraph& Graph, std::uint32_t FromVersion) = nullptr;
};

enum class EGraphSeverity : std::uint8_t
{
	Error,
	Warning,
};

struct FGraphDiagnostic
{
	bool operator==(const FGraphDiagnostic&) const = default;

	EGraphSeverity Severity = EGraphSeverity::Error;
	// Zero when the problem belongs to the graph as a whole.
	FGraphNodeId Node{};
	std::string Pin{};
	std::string Message{};
};

// Version 2 added comments; version 1 files load without them.
inline constexpr std::uint32_t GraphFormatVersion = 2;
inline constexpr std::size_t MaximumGraphNodes = 65536;
inline constexpr std::size_t MaximumGraphLinks = 262144;
inline constexpr std::size_t MaximumGraphComments = 4096;
inline constexpr std::size_t MaximumGraphCommentBytes = 4096;

// A node of Type with the schema's default properties; fails when the schema has no such type.
[[nodiscard]] std::expected<FGraphNode, FGraphError> MakeGraphNode(const FGraphSchema& Schema, std::string_view Type, FGraphNodeId Id, std::array<float, 2> Position);
// One above the largest identity in use by a node or a comment.
[[nodiscard]] FGraphNodeId NextGraphNodeId(const FGraph& Graph);
[[nodiscard]] const FGraphNode* FindGraphNode(const FGraph& Graph, FGraphNodeId Id);
[[nodiscard]] const FGraphComment* FindGraphComment(const FGraph& Graph, FGraphNodeId Id);
[[nodiscard]] const FGraphPinDescriptor* FindGraphPin(const FGraphNodeDescriptor& Node, std::string_view Key, bool bOutput);
// Structural problems, such as unknown types, mistyped properties, dangling or mistyped links, a second link into one input, or cycles in an acyclic domain.
// Domains add their own semantic checks on top.
[[nodiscard]] std::vector<FGraphDiagnostic> ValidateGraph(const FGraph& Graph, const FGraphSchema& Schema);
// Whether linking From to To would keep the graph valid, for editors that preview a connection before making it.
[[nodiscard]] std::expected<void, FGraphError> CanLinkGraphPins(const FGraph& Graph, const FGraphSchema& Schema, const FGraphLink& Link);

// Canonical text: nodes in identity order and links in the order of the input they feed, so saving an unchanged graph rewrites the same bytes.
[[nodiscard]] std::expected<std::string, FGraphError> SerializeGraph(const FGraph& Graph, const FGraphSchema& Schema);
[[nodiscard]] std::expected<FGraph, FGraphError> DeserializeGraph(std::string_view Text, const FGraphSchema& Schema);
}
