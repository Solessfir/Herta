#include "TestFiles.h"

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/VfxGraph/VfxGraph.h"

#include <doctest/doctest.h>

#include <format>
#include <fstream>
#include <iterator>
#include <string>

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
	CHECK_FALSE(Compiled->System.Emitters[0].bRibbon);
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

TEST_CASE("The Sandbox's sword trail compiles to a ribbon that does not move on its own")
{
	std::ifstream Stream("Engine/Source/Programs/HertaTests/Fixtures/Content/Effects/Sword_Trail.hvfxgraph", std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	const auto Graph = DeserializeGraph(Text, GetVfxGraphSchema());
	REQUIRE(Graph);
	const auto Compiled = CompileVfxGraph(*Graph);
	REQUIRE(Compiled);
	REQUIRE(Compiled->System.Emitters.size() == 1);
	CHECK(Compiled->System.Emitters[0].bRibbon);
	CHECK(Compiled->System.Emitters[0].SpeedMaximum == 0.f);
	CHECK(Compiled->System.Emitters[0].Gravity == 0.f);
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

TEST_CASE("VFX Graph stages chain their modules, and events target emitters by their System pin")
{
	FGraph Graph = MakeDefaultVfxGraph();
	const FGraphSchema& Schema = GetVfxGraphSchema();
	const FGraphNodeId Emitter = std::ranges::find(Graph.Nodes, std::string("Emitter"), &FGraphNode::Type)->Id;
	const FGraphNodeId System = std::ranges::find(Graph.Nodes, std::string("System"), &FGraphNode::Type)->Id;
	const auto Add = [&](const std::string_view Type)
	{
		const FGraphNodeId Id = NextGraphNodeId(Graph);
		Graph.Nodes.push_back(*MakeGraphNode(Schema, Type, Id, {0.f, 0.f}));
		return Id;
	};

	const FGraphNodeId Stage = Add("Stage");
	const FGraphNodeId Drag = Add("Drag");
	const FGraphNodeId Vortex = Add("Vortex");
	Graph.Links.push_back({.From = {.Node = Drag, .Key = "Module"}, .To = {.Node = Stage, .Key = "Modules"}});
	Graph.Links.push_back({.From = {.Node = Vortex, .Key = "Module"}, .To = {.Node = Drag, .Key = "Next"}});
	Graph.Links.push_back({.From = {.Node = Stage, .Key = "Stage"}, .To = {.Node = Emitter, .Key = "Stage1"}});

	// A second emitter fed only by the first one's deaths needs no Spawn module.
	const FGraphNodeId Embers = Add("Emitter");
	const FGraphNodeId Event = Add("Event");
	Graph.Links.push_back({.From = {.Node = Embers, .Key = "Emitter"}, .To = {.Node = System, .Key = "Emitter2"}});
	Graph.Links.push_back({.From = {.Node = Embers, .Key = "Emitter"}, .To = {.Node = Event, .Key = "Target"}});
	Graph.Links.push_back({.From = {.Node = Event, .Key = "Event"}, .To = {.Node = Emitter, .Key = "Event1"}});
	REQUIRE(ValidateGraph(Graph, Schema).empty());
	const auto Compiled = CompileVfxGraph(Graph);
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	REQUIRE(Compiled->System.Emitters.size() == 2);
	const FParticleEmitterDesc& First = Compiled->System.Emitters[0];
	REQUIRE(First.Stages.size() == 1);
	REQUIRE(First.Stages[0].Modules.size() == 2);
	CHECK(First.Stages[0].Modules[0].Type == EParticleModuleType::Drag);
	CHECK(First.Stages[0].Modules[1].Type == EParticleModuleType::Vortex);
	REQUIRE(First.Events.size() == 1);
	CHECK(First.Events[0].Emitter == 1);
	CHECK(First.Events[0].Type == EParticleEventType::Death);

	// An event aimed at an emitter off the System node does not compile.
	std::erase_if(Graph.Links, [&](const FGraphLink& Link)
	{
		return Link.From.Node == Embers && Link.To.Node == System;
	});

	CHECK_FALSE(CompileVfxGraph(Graph));
}

TEST_CASE("Version 1 VFX Graphs upgrade to version 2")
{
	std::string Text = *SerializeGraph(MakeDefaultVfxGraph(), GetVfxGraphSchema());
	const std::string Current = std::format("\"domainVersion\": {}", VfxGraphVersion);
	REQUIRE(Text.find(Current) != std::string::npos);
	Text.replace(Text.find(Current), Current.size(), "\"domainVersion\": 1");
	const auto Loaded = DeserializeGraph(Text, GetVfxGraphSchema());
	REQUIRE_MESSAGE(Loaded, (Loaded ? std::string() : Loaded.error().Message));
	CHECK(Loaded->DomainVersion == VfxGraphVersion);
}

TEST_CASE("The Sandbox's sword sparks drop embers where they hit")
{
	std::ifstream Stream("Engine/Source/Programs/HertaTests/Fixtures/Content/Effects/Sword_Sparks.hvfxgraph", std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	const auto Graph = DeserializeGraph(Text, GetVfxGraphSchema());
	REQUIRE(Graph);
	const auto Compiled = CompileVfxGraph(*Graph);
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	REQUIRE(Compiled->System.Emitters.size() == 3);
	REQUIRE(Compiled->System.Emitters[0].Events.size() == 1);
	CHECK(Compiled->System.Emitters[0].Events[0].Type == EParticleEventType::Collision);
	CHECK(Compiled->System.Emitters[0].Events[0].Emitter == 2);
	CHECK(Compiled->System.Emitters[2].Stages.size() == 1);
}
