#include "Herta/GraphEditor/GraphCanvasFormat.h"
#include "Herta/MaterialGraph/MaterialGraph.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <string>

namespace Herta
{
namespace
{
struct FFormatFixture
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
		Result.reserve(Graph.Nodes.size());
		for (const FGraphNode& Node : Graph.Nodes)
		{
			Result.push_back(LayoutGraphNode(Node, *GetMaterialGraphSchema().FindNode(Node.Type), {}, FGraphCanvasMetrics{}, [](const std::string_view Text)
			{
				return 7.f * static_cast<float>(Text.size());
			}));
		}

		return Result;
	}

	void Apply(const FGraphNodePositions& Positions)
	{
		for (FGraphNode& Node : Graph.Nodes)
		{
			if (const auto Found = Positions.find(Node.Id.Value); Found != Positions.end())
			{
				Node.Position = Found->second;
			}
		}
	}

	const FGraphNodeLayout& LayoutOf(const std::vector<FGraphNodeLayout>& Layouts, const FGraphNodeId Id) const
	{
		return *std::ranges::find(Layouts, Id, &FGraphNodeLayout::Id);
	}

	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
};

bool Overlaps(const FGraphNodeLayout& First, const FGraphNodeLayout& Second)
{
	return First.Rect[0] < Second.Rect[2] && Second.Rect[0] < First.Rect[2] && First.Rect[1] < Second.Rect[3] && Second.Rect[1] < First.Rect[3];
}
}

TEST_CASE("Graph formatting lays inputs out leftwards with straight links and no overlaps")
{
	FFormatFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {480.f, 160.f});
	const FGraphNodeId Lerp = Fixture.Add("Lerp", {-500.f, 900.f});
	const FGraphNodeId First = Fixture.Add("Constant", {300.f, -400.f});
	const FGraphNodeId Second = Fixture.Add("Constant", {300.f, -400.f});
	const FGraphNodeId Alpha = Fixture.Add("Constant", {0.f, 0.f});
	Fixture.Link(Lerp, "Result", Output, "BaseColor");
	Fixture.Link(First, "Value", Lerp, "A");
	Fixture.Link(Second, "Value", Lerp, "B");
	Fixture.Link(Alpha, "Value", Lerp, "Alpha");
	// The alpha constant also feeds the output directly, so it must sit left of the lerp, not beside it.
	Fixture.Link(Alpha, "Value", Output, "Roughness");

	const FGraphFormatSettings Settings;
	const auto Roots = GraphFormatRoots(Fixture.Graph, {});
	REQUIRE(Roots.size() == 1);
	CHECK(Roots[0] == Output);
	Fixture.Apply(FormatGraph(Fixture.Graph, GetMaterialGraphSchema(), Fixture.Layouts(), Roots, Settings));
	const auto Layouts = Fixture.Layouts();

	const auto& OutputLayout = Fixture.LayoutOf(Layouts, Output);
	const auto& LerpLayout = Fixture.LayoutOf(Layouts, Lerp);
	CHECK(OutputLayout.Rect[0] == 480.f);
	CHECK(OutputLayout.Rect[1] == 160.f);
	CHECK(LerpLayout.Rect[2] <= OutputLayout.Rect[0] - Settings.ColumnGap);
	CHECK(LerpLayout.Outputs[0][1] == doctest::Approx(OutputLayout.Inputs[0][1]));

	const auto& FirstLayout = Fixture.LayoutOf(Layouts, First);
	CHECK(FirstLayout.Rect[2] <= LerpLayout.Rect[0] - Settings.ColumnGap);
	CHECK(FirstLayout.Outputs[0][1] == doctest::Approx(LerpLayout.Inputs[0][1]));
	CHECK(std::fmod(FirstLayout.Rect[2], Settings.Grid) == 0.f);

	const auto& AlphaLayout = Fixture.LayoutOf(Layouts, Alpha);
	CHECK(AlphaLayout.Rect[2] <= LerpLayout.Rect[0]);
	for (std::size_t A = 0; A < Layouts.size(); ++A)
	{
		for (std::size_t B = A + 1; B < Layouts.size(); ++B)
		{
			CHECK_FALSE(Overlaps(Layouts[A], Layouts[B]));
		}
	}
}

TEST_CASE("Graph formatting stacks separate trees and limits a selection to its own tree")
{
	FFormatFixture Fixture;
	const FGraphNodeId TopRoot = Fixture.Add("Saturate", {400.f, 0.f});
	const FGraphNodeId TopInput = Fixture.Add("Constant", {0.f, 0.f});
	const FGraphNodeId LowerRoot = Fixture.Add("Saturate", {400.f, 10.f});
	const FGraphNodeId LowerInput = Fixture.Add("Constant", {0.f, 10.f});
	Fixture.Link(TopInput, "Value", TopRoot, "In");
	Fixture.Link(LowerInput, "Value", LowerRoot, "In");

	const auto Selected = GraphFormatRoots(Fixture.Graph, std::array{LowerInput});
	REQUIRE(Selected.size() == 1);
	CHECK(Selected[0] == LowerRoot);

	const auto Positions = FormatGraph(Fixture.Graph, GetMaterialGraphSchema(), Fixture.Layouts(), GraphFormatRoots(Fixture.Graph, {}), {});
	CHECK(Positions.size() == 4);
	Fixture.Apply(Positions);
	const auto Layouts = Fixture.Layouts();
	CHECK(Fixture.LayoutOf(Layouts, LowerRoot).Rect[1] >= Fixture.LayoutOf(Layouts, TopRoot).Rect[3] + FGraphFormatSettings{}.TreeGap);
	CHECK_FALSE(Overlaps(Fixture.LayoutOf(Layouts, TopInput), Fixture.LayoutOf(Layouts, LowerInput)));
}

TEST_CASE("Graph links straighten and nodes align and distribute")
{
	FFormatFixture Fixture;
	const FGraphNodeId Output = Fixture.Add("MaterialOutput", {400.f, 100.f});
	const FGraphNodeId Value = Fixture.Add("Constant", {0.f, 237.f});
	Fixture.Link(Value, "Value", Output, "Metallic");
	Fixture.Apply(StraightenGraphLinks(Fixture.Graph, GetMaterialGraphSchema(), Fixture.Layouts(), std::array{Value}));
	auto Layouts = Fixture.Layouts();
	CHECK(Fixture.LayoutOf(Layouts, Value).Outputs[0][1] == doctest::Approx(Fixture.LayoutOf(Layouts, Output).Inputs[2][1]));
	CHECK(Fixture.LayoutOf(Layouts, Value).Rect[0] == 0.f);

	FFormatFixture Row;
	const FGraphNodeId A = Row.Add("Constant", {0.f, 30.f});
	const FGraphNodeId B = Row.Add("Constant", {500.f, 10.f});
	const FGraphNodeId C = Row.Add("Constant", {170.f, 50.f});
	Row.Apply(AlignGraphNodes(Row.Layouts(), std::array{A, B, C}, EGraphAlignment::Top));
	Layouts = Row.Layouts();
	CHECK(Row.LayoutOf(Layouts, A).Rect[1] == 10.f);
	CHECK(Row.LayoutOf(Layouts, C).Rect[1] == 10.f);

	Row.Apply(DistributeGraphNodes(Row.Layouts(), std::array{A, B, C}, true));
	Layouts = Row.Layouts();
	const float FirstGap = Row.LayoutOf(Layouts, C).Rect[0] - Row.LayoutOf(Layouts, A).Rect[2];
	const float SecondGap = Row.LayoutOf(Layouts, B).Rect[0] - Row.LayoutOf(Layouts, C).Rect[2];
	CHECK(FirstGap == doctest::Approx(SecondGap));
	CHECK(Row.LayoutOf(Layouts, A).Rect[0] == 0.f);
	CHECK(Row.LayoutOf(Layouts, B).Rect[0] == 500.f);
	CHECK(AlignGraphNodes(Row.Layouts(), std::array{A}, EGraphAlignment::Left).empty());
}
}
