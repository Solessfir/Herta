#include "Herta/GraphEditor/GraphCanvasEdits.h"
#include "Herta/MaterialGraph/MaterialGraph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

namespace Herta
{
namespace
{
struct FEditsFixture
{
	FGraphNodeId Add(const std::string_view Type, const FCanvasPoint Position)
	{
		const FGraphNodeId Id = NextGraphNodeId(Graph);
		auto Node = MakeGraphNode(GetMaterialGraphSchema(), Type, Id, Position);
		REQUIRE(Node);
		Graph.Nodes.push_back(std::move(*Node));
		return Id;
	}

	void Link(const FGraphNodeId From, const std::string_view FromPin, const FGraphNodeId To, const std::string_view ToPin)
	{
		Graph.Links.push_back({.From = {.Node = From, .Key = std::string(FromPin)}, .To = {.Node = To, .Key = std::string(ToPin)}});
	}

	std::vector<FGraphNodeLayout> Layouts() const
	{
		std::vector<FGraphNodeLayout> Result;
		for (const FGraphNode& Node : Graph.Nodes)
		{
			Result.push_back(LayoutGraphNode(Node, *GetMaterialGraphSchema().FindNode(Node.Type), {}, FGraphCanvasMetrics{}, [](const std::string_view Text)
			{
				return 7.f * static_cast<float>(Text.size());
			}));
		}

		return Result;
	}

	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
};

bool HasLink(const FGraph& Graph, const FGraphNodeId From, const std::string_view FromPin, const FGraphNodeId To, const std::string_view ToPin)
{
	return std::ranges::find(Graph.Links, FGraphLink{.From = {.Node = From, .Key = std::string(FromPin)}, .To = {.Node = To, .Key = std::string(ToPin)}}) != Graph.Links.end();
}
}

TEST_CASE("Graph canvas edits find trees and spatial neighbours")
{
	FEditsFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {400.f, 0.f});
	const FGraphNodeId Multiply = Fixture.Add("Multiply", {200.f, 0.f});
	const FGraphNodeId Value = Fixture.Add("Constant", {0.f, 0.f});
	const FGraphNodeId Below = Fixture.Add("Constant", {0.f, 300.f});
	const FGraphNodeId Loose = Fixture.Add("Constant", {-900.f, -900.f});
	Fixture.Link(Value, "Value", Multiply, "A");
	Fixture.Link(Below, "Value", Multiply, "B");
	Fixture.Link(Multiply, "Result", Output, "Roughness");

	auto Tree = GraphConnectedNodes(Fixture.Graph, std::array{Value});
	std::ranges::sort(Tree);
	CHECK(Tree == std::vector{Output, Multiply, Value, Below});
	CHECK_FALSE(std::ranges::contains(Tree, Loose));

	const auto Layouts = Fixture.Layouts();
	CHECK(FindGraphNeighbour(Layouts, Value, EGraphDirection::Right) == Multiply);
	CHECK(FindGraphNeighbour(Layouts, Value, EGraphDirection::Down) == Below);
	CHECK(FindGraphNeighbour(Layouts, Multiply, EGraphDirection::Left) == Value);
	CHECK_FALSE(FindGraphNeighbour(Layouts, Output, EGraphDirection::Right));

	const auto Bounds = GraphNodeBounds(Layouts, std::array{Value, Below});
	REQUIRE(Bounds);
	CHECK((*Bounds)[0] == 0.f);
	CHECK((*Bounds)[1] == 0.f);
	CHECK((*Bounds)[3] > 300.f);

	const auto Swapped = SwapGraphNodes(Layouts, Value, Below);
	CHECK(Swapped.at(Value.Value) == FCanvasPoint{0.f, 300.f});
	CHECK(Swapped.at(Below.Value) == FCanvasPoint{0.f, 0.f});
}

TEST_CASE("Graph canvas edits swap inputs and bridge deleted nodes")
{
	FEditsFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {600.f, 0.f});
	const FGraphNodeId Saturate = Fixture.Add("Saturate", {400.f, 0.f});
	const FGraphNodeId Lerp = Fixture.Add("Lerp", {200.f, 0.f});
	const FGraphNodeId First = Fixture.Add("Constant", {0.f, 0.f});
	const FGraphNodeId Second = Fixture.Add("Constant", {0.f, 100.f});
	Fixture.Link(First, "Value", Lerp, "A");
	Fixture.Link(Second, "Value", Lerp, "B");
	Fixture.Link(Lerp, "Result", Saturate, "In");
	Fixture.Link(Saturate, "Result", Output, "Roughness");

	const auto Swapped = SwapGraphInputs(Fixture.Graph, GetMaterialGraphSchema(), Lerp, "A", "B");
	REQUIRE(Swapped);
	CHECK(HasLink(*Swapped, First, "Value", Lerp, "B"));
	CHECK(HasLink(*Swapped, Second, "Value", Lerp, "A"));

	// An empty input swaps with a connected one.
	const auto Moved = SwapGraphInputs(Fixture.Graph, GetMaterialGraphSchema(), Lerp, "A", "Alpha");
	REQUIRE(Moved);
	CHECK(HasLink(*Moved, First, "Value", Lerp, "Alpha"));
	CHECK_FALSE(SwapGraphInputs(Fixture.Graph, GetMaterialGraphSchema(), Saturate, "In", "In"));

	// Removing a chain keeps the first connected input's source flowing to the surviving consumer.
	const FGraph Removed = RemoveGraphNodesKeepingLinks(Fixture.Graph, GetMaterialGraphSchema(), std::array{Lerp, Saturate});
	CHECK(Removed.Nodes.size() == 3);
	CHECK(HasLink(Removed, First, "Value", Output, "Roughness"));
	CHECK(Removed.Links.size() == 1);
}

TEST_CASE("Graph canvas edits splice nodes into links")
{
	FEditsFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {600.f, 0.f});
	const FGraphNodeId Value = Fixture.Add("Constant", {0.f, 0.f});
	const FGraphNodeId Saturate = Fixture.Add("Saturate", {300.f, 200.f});
	const FGraphNodeId Other = Fixture.Add("Constant", {300.f, 400.f});
	Fixture.Link(Value, "Value", Output, "Roughness");
	const FGraphLink Original = Fixture.Graph.Links.front();

	const auto Spliced = SpliceGraphNode(Fixture.Graph, GetMaterialGraphSchema(), Saturate, Original);
	REQUIRE(Spliced);
	CHECK(HasLink(*Spliced, Value, "Value", Saturate, "In"));
	CHECK(HasLink(*Spliced, Saturate, "Result", Output, "Roughness"));
	CHECK_FALSE(HasLink(*Spliced, Value, "Value", Output, "Roughness"));

	// A node without inputs cannot take the link.
	CHECK_FALSE(SpliceGraphNode(Fixture.Graph, GetMaterialGraphSchema(), Other, Original));
	CHECK_FALSE(SpliceGraphNode(Fixture.Graph, GetMaterialGraphSchema(), Value, Original));
}

TEST_CASE("Graph canvas edits copy and paste nodes under new identities")
{
	FEditsFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {600.f, 0.f});
	const FGraphNodeId Value = Fixture.Add("Constant", {100.f, 50.f});
	const FGraphNodeId Saturate = Fixture.Add("Saturate", {300.f, 50.f});
	Fixture.Link(Value, "Value", Saturate, "In");
	Fixture.Link(Saturate, "Result", Output, "Roughness");

	const auto Text = CopyGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), std::array{Value, Saturate, Output});
	REQUIRE(Text);

	std::vector<FGraphNodeId> Pasted;
	const auto After = PasteGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), *Text, {1000.f, 1000.f}, Pasted);
	REQUIRE(After);

	// The output node is at its limit of one, so only the other two arrive, still linked to each other.
	REQUIRE(Pasted.size() == 2);
	CHECK(After->Nodes.size() == 5);
	CHECK(FindGraphNode(*After, Pasted[0])->Position == FCanvasPoint{1000.f, 1000.f});
	CHECK(FindGraphNode(*After, Pasted[1])->Position == FCanvasPoint{1200.f, 1000.f});
	CHECK(HasLink(*After, Pasted[0], "Value", Pasted[1], "In"));
	CHECK(After->Links.size() == 3);
	CHECK(ValidateGraph(*After, GetMaterialGraphSchema()).empty());

	CHECK_FALSE(CopyGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), std::span<const FGraphNodeId>{}));
	CHECK_FALSE(PasteGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), "not a graph", {}, Pasted));
}

TEST_CASE("Graph canvas edits move lifted links to new pins and new nodes")
{
	FEditsFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {600.f, 0.f});
	const FGraphNodeId Value = Fixture.Add("Constant", {0.f, 0.f});
	const FGraphNodeId Other = Fixture.Add("Constant", {0.f, 200.f});
	Fixture.Link(Value, "Value", Output, "Roughness");
	Fixture.Link(Value, "Value", Output, "Metallic");
	const auto Lifted = Fixture.Graph.Links;

	// Ctrl-dragging the output lifts both links; dropping on another output moves them together.
	const std::vector<FGraphPin> Inputs{{.Node = Output, .Key = "Roughness"}, {.Node = Output, .Key = "Metallic"}};
	const auto Moved = RelinkGraphPins(Fixture.Graph, GetMaterialGraphSchema(), Inputs, false, {.Node = Other, .Key = "Value"}, Lifted);
	REQUIRE(Moved);
	CHECK(Moved->Links.size() == 2);
	CHECK(HasLink(*Moved, Other, "Value", Output, "Roughness"));
	CHECK(HasLink(*Moved, Other, "Value", Output, "Metallic"));

	// A plain drag from an output to an input that already has a link replaces it.
	const auto Replaced = RelinkGraphPins(Fixture.Graph, GetMaterialGraphSchema(), std::array{FGraphPin{.Node = Other, .Key = "Value"}}, true, {.Node = Output, .Key = "Roughness"}, {});
	REQUIRE(Replaced);
	CHECK(HasLink(*Replaced, Other, "Value", Output, "Roughness"));
	CHECK_FALSE(HasLink(*Replaced, Value, "Value", Output, "Roughness"));
	CHECK(Replaced->Links.size() == 2);

	// Dropping both lifted links on a new node connects each to a pin that accepts it.
	FGraph WithNode = Fixture.Graph;
	const FGraphNodeId Add = NextGraphNodeId(WithNode);
	WithNode.Nodes.push_back(*MakeGraphNode(GetMaterialGraphSchema(), "Add", Add, {300.f, 0.f}));
	const auto Connected = ConnectGraphPinsToNode(WithNode, GetMaterialGraphSchema(), Inputs, false, Add, Lifted);
	REQUIRE(Connected);
	CHECK(HasLink(*Connected, Add, "Result", Output, "Roughness"));
	CHECK(HasLink(*Connected, Add, "Result", Output, "Metallic"));

	// A new node's free inputs take several held outputs one each.
	const std::vector<FGraphPin> Outputs{{.Node = Value, .Key = "Value"}, {.Node = Other, .Key = "Value"}};
	const auto Fed = ConnectGraphPinsToNode(WithNode, GetMaterialGraphSchema(), Outputs, true, Add, {});
	REQUIRE(Fed);
	CHECK(HasLink(*Fed, Value, "Value", Add, "A"));
	CHECK(HasLink(*Fed, Other, "Value", Add, "B"));

	const FGraph Broken = BreakGraphPinLinks(Fixture.Graph, {.Node = Value, .Key = "Value"}, true);
	CHECK(Broken.Links.empty());
}

TEST_CASE("Graph comments carry the nodes inside them and travel through the clipboard")
{
	FEditsFixture Fixture;
	const FGraphNodeId Inside = Fixture.Add("Constant", {40.f, 60.f});
	const FGraphNodeId Outside = Fixture.Add("Constant", {600.f, 60.f});
	const FGraphNodeId Comment = NextGraphNodeId(Fixture.Graph);
	Fixture.Graph.Comments.push_back({.Id = Comment, .Text = "Inputs", .Rect = {0.f, 0.f, 400.f, 300.f}});

	const auto Layouts = Fixture.Layouts();
	CHECK(GraphNodesInComment(Layouts, Fixture.Graph.Comments.front()) == std::vector{Inside});

	const auto Text = CopyGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), std::array{Comment, Inside});
	REQUIRE(Text);
	std::vector<FGraphNodeId> Pasted;
	const auto After = PasteGraphNodes(Fixture.Graph, GetMaterialGraphSchema(), *Text, {1000.f, 2000.f}, Pasted);
	REQUIRE(After);
	REQUIRE(Pasted.size() == 2);
	REQUIRE(After->Comments.size() == 2);
	CHECK(After->Comments.back().Rect == std::array{1000.f, 2000.f, 1400.f, 2300.f});
	CHECK(FindGraphNode(*After, Pasted[0])->Position == FCanvasPoint{1040.f, 2060.f});
	CHECK(ValidateGraph(*After, GetMaterialGraphSchema()).empty());

	const FGraph Removed = RemoveGraphNodesKeepingLinks(Fixture.Graph, GetMaterialGraphSchema(), std::array{Comment});
	CHECK(Removed.Comments.empty());
	CHECK(Removed.Nodes.size() == 2);
	CHECK(FindGraphNode(Removed, Outside));
}
}
