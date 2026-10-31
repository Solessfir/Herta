#include "Herta/GraphCore/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

namespace Herta
{
namespace
{
enum ETestPinType : std::uint32_t
{
	Scalar,
	Vector,
};

constexpr std::array ScalarOutput{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = Scalar}};
constexpr std::array VectorOutput{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = Vector}};
constexpr std::array AddInputs{FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Scalar}, FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Scalar}};
constexpr std::array AddOutputs{FGraphPinDescriptor{.Key = "Result", .Name = "Result", .Type = Scalar}};
constexpr std::array OutputInputs{FGraphPinDescriptor{.Key = "Color", .Name = "Color", .Type = Vector}};

const std::array ConstantProperties{FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = 0.5f}};
const std::array ColorProperties{FGraphPropertyDescriptor{.Key = "color", .Name = "Color", .Default = std::array<float, 4>{1.f, 0.f, 0.f, 1.f}}};
const std::array OptionProperties{
    FGraphPropertyDescriptor{.Key = "enabled", .Name = "Enabled", .Default = true},
    FGraphPropertyDescriptor{.Key = "count", .Name = "Count", .Default = std::int32_t{3}},
    FGraphPropertyDescriptor{.Key = "label", .Name = "Label", .Default = std::string("Plain")},
};

const std::array TestNodes{
    FGraphNodeDescriptor{.Type = "Constant", .Name = "Constant", .Category = "Values", .Outputs = ScalarOutput, .Properties = ConstantProperties},
    FGraphNodeDescriptor{.Type = "Color", .Name = "Color", .Category = "Values", .Outputs = VectorOutput, .Properties = ColorProperties},
    FGraphNodeDescriptor{.Type = "Add", .Name = "Add", .Category = "Math", .Inputs = AddInputs, .Outputs = AddOutputs},
    FGraphNodeDescriptor{.Type = "Options", .Name = "Options", .Category = "Values", .Outputs = ScalarOutput, .Properties = OptionProperties},
    FGraphNodeDescriptor{.Type = "Output", .Name = "Output", .Category = "Output", .Inputs = OutputInputs, .MaximumCount = 1},
};

// A scalar widens into a vector, never the reverse.
bool CanConnectTest(const std::uint32_t From, const std::uint32_t To)
{
	return From == To || (From == Scalar && To == Vector);
}

const FGraphSchema TestSchema{.Domain = "test", .Version = 2, .Nodes = TestNodes, .CanConnect = CanConnectTest};

FGraphNode Node(const std::string_view Type, const std::uint32_t Id)
{
	auto Made = MakeGraphNode(TestSchema, Type, {Id}, {static_cast<float>(Id) * 10.f, -5.f});
	REQUIRE(Made);
	return *Made;
}

FGraphLink Link(const std::uint32_t From, const std::string_view FromPin, const std::uint32_t To, const std::string_view ToPin)
{
	return {.From = {.Node = {From}, .Key = std::string(FromPin)}, .To = {.Node = {To}, .Key = std::string(ToPin)}};
}

// Constant 1 and Constant 2 feed Add 3, which feeds Output 4 through a scalar-to-vector widening.
FGraph SampleGraph()
{
	FGraph Graph{.Domain = "test", .DomainVersion = 2};
	Graph.Nodes = {Node("Constant", 1), Node("Constant", 2), Node("Add", 3), Node("Output", 4)};
	Graph.Links = {Link(1, "Value", 3, "A"), Link(2, "Value", 3, "B"), Link(3, "Result", 4, "Color")};
	return Graph;
}

bool HasError(const FGraph& Graph, const std::string_view Fragment)
{
	const auto Diagnostics = ValidateGraph(Graph, TestSchema);
	return std::ranges::any_of(Diagnostics, [Fragment](const FGraphDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity == EGraphSeverity::Error && Diagnostic.Message.find(Fragment) != std::string::npos;
	});
}
}

TEST_CASE("Graph nodes start with their schema's default properties")
{
	const FGraphNode Options = Node("Options", 7);
	REQUIRE(Options.Properties.size() == 3);
	CHECK(Options.Properties[0].Key == "enabled");
	CHECK(std::get<bool>(Options.Properties[0].Value));
	CHECK(std::get<std::int32_t>(Options.Properties[1].Value) == 3);
	CHECK(std::get<std::string>(Options.Properties[2].Value) == "Plain");
	CHECK_FALSE(MakeGraphNode(TestSchema, "Missing", {1}, {}));
	CHECK(NextGraphNodeId(SampleGraph()).Value == 5);
}

TEST_CASE("Valid graphs pass structural validation and round-trip canonically")
{
	FGraph Graph = SampleGraph();
	CHECK(ValidateGraph(Graph, TestSchema).empty());

	const auto Text = SerializeGraph(Graph, TestSchema);
	REQUIRE(Text);
	const auto Loaded = DeserializeGraph(*Text, TestSchema);
	REQUIRE(Loaded);
	CHECK(*Loaded == Graph);

	// Order in memory does not change the saved bytes.
	std::ranges::reverse(Graph.Nodes);
	std::ranges::reverse(Graph.Links);
	const auto Shuffled = SerializeGraph(Graph, TestSchema);
	REQUIRE(Shuffled);
	CHECK(*Shuffled == *Text);
}

TEST_CASE("Graph validation reports structural errors")
{
	FGraph Duplicate = SampleGraph();
	Duplicate.Nodes[1].Id = {1};
	CHECK(HasError(Duplicate, "unique"));

	FGraph Unknown = SampleGraph();
	Unknown.Nodes[0].Type = "Missing";
	CHECK(HasError(Unknown, "Unknown node type"));

	FGraph Mistyped = SampleGraph();
	Mistyped.Nodes[0].Properties[0].Value = std::int32_t{1};
	CHECK(HasError(Mistyped, "properties do not match"));

	FGraph NotFinite = SampleGraph();
	NotFinite.Nodes[0].Properties[0].Value = std::numeric_limits<float>::infinity();
	CHECK(HasError(NotFinite, "properties do not match"));

	FGraph Dangling = SampleGraph();
	Dangling.Links.push_back(Link(9, "Value", 3, "A"));
	CHECK(HasError(Dangling, "missing node"));

	FGraph WrongPin = SampleGraph();
	WrongPin.Links[0].To.Key = "C";
	CHECK(HasError(WrongPin, "missing pin"));

	FGraph Narrowing = SampleGraph();
	Narrowing.Nodes.push_back(Node("Color", 5));
	Narrowing.Links[0] = Link(5, "Value", 3, "A");
	CHECK(HasError(Narrowing, "cannot take"));

	FGraph DoubleInput = SampleGraph();
	DoubleInput.Links.push_back(Link(1, "Value", 3, "B"));
	CHECK(HasError(DoubleInput, "more than one link"));

	FGraph TwoOutputs = SampleGraph();
	TwoOutputs.Nodes.push_back(Node("Output", 5));
	CHECK(HasError(TwoOutputs, "only 1"));

	FGraph Cycle{.Domain = "test", .DomainVersion = 2};
	Cycle.Nodes = {Node("Add", 1), Node("Add", 2)};
	Cycle.Links = {Link(1, "Result", 2, "A"), Link(2, "Result", 1, "A")};
	CHECK(HasError(Cycle, "cycle"));

	FGraph OtherDomain = SampleGraph();
	OtherDomain.Domain = "material";
	CHECK(HasError(OtherDomain, "Expected a test graph"));

	FGraph OldVersion = SampleGraph();
	OldVersion.DomainVersion = 1;
	CHECK(HasError(OldVersion, "reads version 2"));
	CHECK_FALSE(SerializeGraph(OldVersion, TestSchema));
}

TEST_CASE("Graph links are checked before an editor makes them")
{
	const FGraph Graph = SampleGraph();
	// Replacing what feeds an input is allowed; the editor removes the old link.
	CHECK(CanLinkGraphPins(Graph, TestSchema, Link(2, "Value", 3, "A")));
	CHECK_FALSE(CanLinkGraphPins(Graph, TestSchema, Link(3, "Result", 3, "A")));
	CHECK_FALSE(CanLinkGraphPins(Graph, TestSchema, Link(3, "A", 4, "Color")));
	CHECK_FALSE(CanLinkGraphPins(Graph, TestSchema, Link(9, "Value", 3, "A")));

	FGraph Chain{.Domain = "test", .DomainVersion = 2};
	Chain.Nodes = {Node("Add", 1), Node("Add", 2), Node("Add", 3)};
	Chain.Links = {Link(1, "Result", 2, "A"), Link(2, "Result", 3, "A")};
	CHECK_FALSE(CanLinkGraphPins(Chain, TestSchema, Link(3, "Result", 1, "A")));
	CHECK(CanLinkGraphPins(Chain, TestSchema, Link(1, "Result", 3, "B")));
}

TEST_CASE("Graph text reads values by schema type and keeps defaults for missing properties")
{
	FGraph Graph{.Domain = "test", .DomainVersion = 2};
	Graph.Nodes = {Node("Options", 1)};
	Graph.Nodes[0].Properties[2].Value = std::string("Quote \" slash \\ line\nend");
	const auto Text = SerializeGraph(Graph, TestSchema);
	REQUIRE(Text);
	const auto Loaded = DeserializeGraph(*Text, TestSchema);
	REQUIRE(Loaded);
	CHECK(*Loaded == Graph);

	constexpr std::string_view Header = R"({"format": "HertaGraph", "version": 1, "domain": "test", "domainVersion": 2, "links": [], "nodes": [)";
	const auto Parse = [Header](const std::string_view Nodes)
	{
		return DeserializeGraph(std::string(Header) + std::string(Nodes) + "]}", TestSchema);
	};

	const auto Defaults = Parse(R"({"id": 1, "type": "Options", "position": [0, 0], "properties": {"count": 7}})");
	REQUIRE(Defaults);
	CHECK(std::get<std::int32_t>(Defaults->Nodes[0].Properties[1].Value) == 7);
	CHECK(std::get<std::string>(Defaults->Nodes[0].Properties[2].Value) == "Plain");

	const auto IntegerFloat = Parse(R"({"id": 1, "type": "Constant", "position": [0, 0], "properties": {"value": 2}})");
	REQUIRE(IntegerFloat);
	CHECK(std::get<float>(IntegerFloat->Nodes[0].Properties[0].Value) == 2.f);

	CHECK_FALSE(Parse(R"({"id": 1, "type": "Options", "position": [0, 0], "properties": {"count": 1.5}})"));
	CHECK_FALSE(Parse(R"({"id": 1, "type": "Options", "position": [0, 0], "properties": {"other": 1}})"));
	CHECK_FALSE(Parse(R"({"id": 0, "type": "Constant", "position": [0, 0], "properties": {}})"));
	CHECK_FALSE(Parse(R"({"id": 1, "type": "Constant", "position": [0], "properties": {}})"));
	CHECK_FALSE(Parse(R"({"id": 1, "type": "Constant", "position": [0, 0], "properties": {}, "extra": 1})"));
	CHECK_FALSE(DeserializeGraph(R"({"format": "HertaGraph", "version": 2, "domain": "test", "domainVersion": 2, "nodes": [], "links": []})", TestSchema));
	CHECK_FALSE(DeserializeGraph("not json", TestSchema));
}
}
