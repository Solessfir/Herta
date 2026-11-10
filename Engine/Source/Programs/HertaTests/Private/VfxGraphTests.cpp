#include "TestFiles.h"

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/VfxGraph/VfxGraph.h"

#include <doctest/doctest.h>

#include <numbers>

using namespace Herta;

TEST_CASE("VFX Graphs compile emitters from their modules, in degrees on the canvas and radians cooked")
{
	FGraph Graph = MakeDefaultVfxGraph();
	CHECK(ValidateGraph(Graph, GetVfxGraphSchema()).empty());
	auto Compiled = CompileVfxGraph(Graph);
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	REQUIRE(Compiled->System.Emitters.size() == 1);
	const FParticleEmitterDesc& Emitter = Compiled->System.Emitters[0];
	CHECK(Emitter.Burst == 20);
	CHECK(Emitter.ConeAngle == doctest::Approx(std::numbers::pi_v<float> / 6.f));
	CHECK(Emitter.ColorEnd[3] == 0.f);
	CHECK_FALSE(Emitter.bCollision);

	// A collision module turns collision on, and an emitter left off the System node only warns.
	const FGraphNodeId Collision = NextGraphNodeId(Graph);
	Graph.Nodes.push_back(*MakeGraphNode(GetVfxGraphSchema(), "Collision", Collision, {0.f, 400.f}));
	Graph.Links.push_back({.From = {.Node = Collision, .Key = "Collision"}, .To = {.Node = {2}, .Key = "Collision"}});
	Graph.Nodes.push_back(*MakeGraphNode(GetVfxGraphSchema(), "Emitter", NextGraphNodeId(Graph), {0.f, 600.f}));
	Compiled = CompileVfxGraph(Graph);
	REQUIRE(Compiled);
	CHECK(Compiled->System.Emitters[0].bCollision);
	REQUIRE(Compiled->Diagnostics.size() == 1);
	CHECK(Compiled->Diagnostics[0].Severity == EGraphSeverity::Warning);

	// An emitter without a Spawn module, or with impossible values, does not compile.
	FGraph Unspawned = MakeDefaultVfxGraph();
	std::erase_if(Unspawned.Links, [](const FGraphLink& Link)
	{
		return Link.To.Key == "Spawn";
	});

	CHECK_FALSE(CompileVfxGraph(Unspawned));
	FGraph Backwards = MakeDefaultVfxGraph();
	std::ranges::find(Backwards.Nodes, std::string("Lifetime"), &FGraphNode::Type)->Properties[1].Value = 0.01f;
	CHECK_FALSE(CompileVfxGraph(Backwards));
	FGraph Empty{.Domain = std::string(VfxGraphDomain), .DomainVersion = VfxGraphVersion};
	CHECK_FALSE(CompileVfxGraph(Empty));
}

TEST_CASE("VFX Graphs cook into particle systems")
{
	const Tests::FScratchDirectory Scratch("VfxCook");
	const auto Text = SerializeGraph(MakeDefaultVfxGraph(), GetVfxGraphSchema());
	REQUIRE(Text);
	Tests::WriteText(Scratch.GetPath() / "Content/Effects/Puff.hvfxgraph", *Text);
	Tests::WriteText(Scratch.GetPath() / "Content/Effects/Puff.hvfxgraph.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000b01\nImporter = VfxGraph\n");
	const auto Result = CookAsset({.ContentRoot = Scratch.GetPath() / "Content", .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Effects/Puff.hvfxgraph", .TargetPlatform = "TestPlatform"});
	REQUIRE_MESSAGE(Result, (Result ? std::string() : Result.error().Message));
	const auto Asset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Result->Key);
	REQUIRE(Asset);
	CHECK(std::get<FCookedParticleSystem>(*Asset) == CompileVfxGraph(MakeDefaultVfxGraph())->System);
}
