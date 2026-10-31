#include "Herta/GraphEditor/GraphCanvasGeometry.h"
#include "Herta/MaterialGraph/MaterialGraph.h"

#include <doctest/doctest.h>

#include <string>

namespace Herta
{
namespace
{
// Every character is 7 units wide, so layouts do not depend on a font.
float FixedWidth(const std::string_view Text)
{
	return 7.f * static_cast<float>(Text.size());
}

FGraphNode MaterialNode(const std::string_view Type, const std::uint32_t Id, const FCanvasPoint Position)
{
	auto Node = MakeGraphNode(GetMaterialGraphSchema(), Type, {Id}, Position);
	REQUIRE(Node);
	return *Node;
}

std::vector<FGraphNodeLayout> Layout(const FGraph& Graph)
{
	std::vector<FGraphNodeLayout> Layouts;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Layouts.push_back(LayoutGraphNode(Node, *GetMaterialGraphSchema().FindNode(Node.Type), {}, FGraphCanvasMetrics{}, FixedWidth));
	}

	return Layouts;
}
}

TEST_CASE("Graph canvas zoom keeps the point under the cursor in place")
{
	FGraphCanvasView View{.Origin = {100.f, 50.f}, .Pan = {30.f, -20.f}, .Zoom = 1.f};
	const FCanvasPoint Cursor{400.f, 300.f};
	const FCanvasPoint Before = View.ToCanvas(Cursor);
	View.ZoomAbout(Cursor, 1.5f);
	const FCanvasPoint After = View.ToCanvas(Cursor);
	CHECK(After[0] == doctest::Approx(Before[0]));
	CHECK(After[1] == doctest::Approx(Before[1]));
	CHECK(View.Zoom == doctest::Approx(1.5f));

	View.ZoomAbout(Cursor, 100.f);
	CHECK(View.Zoom == MaximumGraphCanvasZoom);
	const FCanvasPoint Round = View.ToCanvas(View.ToScreen({12.f, -7.f}));
	CHECK(Round[0] == doctest::Approx(12.f));
	CHECK(Round[1] == doctest::Approx(-7.f));
}

TEST_CASE("Graph nodes lay out pins on their edges below the title")
{
	const FGraphCanvasMetrics Metrics;
	const FGraphNode Node = MaterialNode("Lerp", 1, {50.f, 20.f});
	const FGraphNodeLayout Placed = LayoutGraphNode(Node, *GetMaterialGraphSchema().FindNode("Lerp"), {10.f, 5.f}, Metrics, FixedWidth);
	CHECK(Placed.Rect[0] == 60.f);
	CHECK(Placed.Rect[1] == 25.f);
	CHECK(Placed.Rect[2] - Placed.Rect[0] >= Metrics.MinimumWidth);
	REQUIRE(Placed.Inputs.size() == 3);
	REQUIRE(Placed.Outputs.size() == 1);
	CHECK(Placed.Inputs[0][0] == Placed.Rect[0]);
	CHECK(Placed.Outputs[0][0] == Placed.Rect[2]);
	CHECK(Placed.Inputs[1][1] - Placed.Inputs[0][1] == Metrics.RowHeight);
	CHECK(Placed.Inputs[0][1] == Placed.Rect[1] + Metrics.TitleHeight + Metrics.RowHeight * 0.5f);
	CHECK(Placed.Rect[3] - Placed.Rect[1] == doctest::Approx(Metrics.TitleHeight + 3.f * Metrics.RowHeight + Metrics.Padding * 0.5f));

	// A long title widens the node.
	const FGraphNodeLayout Wide = LayoutGraphNode(Node, *GetMaterialGraphSchema().FindNode("Lerp"), {}, Metrics, [](const std::string_view Text)
	{
		return Text == "Lerp" ? 400.f : 7.f;
	});

	CHECK(Wide.Rect[2] - Wide.Rect[0] == doctest::Approx(400.f + 2.f * Metrics.Padding));
}

TEST_CASE("Graph links leave and enter their pins level and are picked near the curve")
{
	const auto Curve = GraphLinkCurve({0.f, 0.f}, {200.f, 100.f});
	CHECK(Curve[1][1] == 0.f);
	CHECK(Curve[2][1] == 100.f);
	CHECK(Curve[1][0] > 0.f);
	CHECK(Curve[2][0] < 200.f);
	CHECK(DistanceToGraphLink({0.f, 0.f}, {0.f, 0.f}, {200.f, 100.f}) < 0.01f);
	CHECK(DistanceToGraphLink({100.f, 50.f}, {0.f, 0.f}, {200.f, 100.f}) < 2.f);
	CHECK(DistanceToGraphLink({100.f, -60.f}, {0.f, 0.f}, {200.f, 100.f}) > 40.f);
}

TEST_CASE("Graph canvas hits pins before nodes and nodes before links")
{
	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
	Graph.Nodes = {MaterialNode("Constant", 1, {0.f, 0.f}), MaterialNode("MaterialOutput", 2, {400.f, 0.f}), MaterialNode("Constant", 3, {380.f, -10.f})};
	Graph.Links = {{.From = {.Node = {1}, .Key = "Value"}, .To = {.Node = {2}, .Key = "Roughness"}}};
	const auto Layouts = Layout(Graph);

	const FGraphCanvasHitResult Pin = HitTestGraphCanvas(Graph, GetMaterialGraphSchema(), Layouts, Layouts[0].Outputs[0], 6.f);
	CHECK(Pin.Kind == EGraphCanvasHit::Pin);
	CHECK(Pin.Node == FGraphNodeId{1});
	CHECK(Pin.Pin == "Value");
	CHECK(Pin.bOutput);

	// Node 3 overlaps node 2 and draws later, so it takes the click.
	const FGraphCanvasHitResult Top = HitTestGraphCanvas(Graph, GetMaterialGraphSchema(), Layouts, {Layouts[2].Rect[0] + 30.f, Layouts[2].Rect[1] + 12.f}, 6.f);
	CHECK(Top.Kind == EGraphCanvasHit::Node);
	CHECK(Top.Node == FGraphNodeId{3});

	const FCanvasPoint Middle{(Layouts[0].Outputs[0][0] + Layouts[1].Inputs[3][0]) * 0.5f, (Layouts[0].Outputs[0][1] + Layouts[1].Inputs[3][1]) * 0.5f};
	const FGraphCanvasHitResult Link = HitTestGraphCanvas(Graph, GetMaterialGraphSchema(), Layouts, Middle, 6.f);
	CHECK(Link.Kind == EGraphCanvasHit::Link);
	CHECK(Link.Link == 0);

	CHECK(HitTestGraphCanvas(Graph, GetMaterialGraphSchema(), Layouts, {-500.f, -500.f}, 6.f).Kind == EGraphCanvasHit::None);
}

TEST_CASE("Graph canvas box selection takes every node the box touches")
{
	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
	Graph.Nodes = {MaterialNode("Constant", 1, {0.f, 0.f}), MaterialNode("Constant", 2, {300.f, 0.f}), MaterialNode("Constant", 3, {0.f, 300.f})};
	const auto Layouts = Layout(Graph);
	const auto Inside = GraphNodesInBox(Layouts, {310.f, 10.f}, {-10.f, -10.f});
	REQUIRE(Inside.size() == 2);
	CHECK(Inside[0] == FGraphNodeId{1});
	CHECK(Inside[1] == FGraphNodeId{2});
}
}
