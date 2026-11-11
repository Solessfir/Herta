#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/GameFramework/GameplayProgram.h"
#include "Herta/GameplayGraph/GameplayGraph.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>

using namespace Herta;

namespace
{
// Every call a graph makes, in order, so two runs compare as text.
class FRecordingHost final : public IGameplayHost
{
public:
	void PlaySound(const std::string_view Cue, const FVector3& Position) override
	{
		Calls.push_back(std::format("sound {} {} {} {}", Cue, Position.X, Position.Y, Position.Z));
	}

	void SpawnEffect(const std::string_view Cue, const FVector3& Position, const float Scale) override
	{
		Calls.push_back(std::format("effect {} {} {} {} {}", Cue, Position.X, Position.Y, Position.Z, Scale));
	}

	void Log(const std::string_view Message, const float Value) override
	{
		Calls.push_back(std::format("log {} {}", Message, Value));
	}

	std::vector<std::string> Calls{};
};

struct FGraphBuilder
{
	FGraphNodeId Add(const std::string_view Type, std::initializer_list<std::pair<std::string_view, FGraphValue>> Properties = {})
	{
		const FGraphNodeId Id = NextGraphNodeId(Graph);
		FGraphNode Node = *MakeGraphNode(GetGameplayGraphSchema(), Type, Id, {0.f, 0.f});
		for (const auto& [Key, Value] : Properties)
		{
			std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value = Value;
		}

		Graph.Nodes.push_back(std::move(Node));
		return Id;
	}

	void Link(const FGraphNodeId From, const std::string_view FromKey, const FGraphNodeId To, const std::string_view ToKey)
	{
		Graph.Links.push_back({.From = {.Node = From, .Key = std::string(FromKey)}, .To = {.Node = To, .Key = std::string(ToKey)}});
	}

	FGraph Graph{.Domain = std::string(GameplayGraphDomain), .DomainVersion = GameplayGraphVersion};
};

// Entering counts visits; the third one throws sparks and starts counting again, and every other one chimes.
FGraph MakeCounterGraph()
{
	FGraphBuilder Builder;
	const FGraphNodeId Enter = Builder.Add("OnPlayerEnter");
	const FGraphNodeId Count = Builder.Add("GetVariable", {{"variable", std::string("Visits")}});
	const FGraphNodeId Plus = Builder.Add("Add", {{"b", 1.f}});
	const FGraphNodeId Store = Builder.Add("SetVariable", {{"variable", std::string("Visits")}});
	const FGraphNodeId Third = Builder.Add("Greater", {{"b", 2.5f}});
	const FGraphNodeId Branch = Builder.Add("Branch");
	const FGraphNodeId Sparks = Builder.Add("SpawnEffect", {{"scale", 2.f}});
	const FGraphNodeId Reset = Builder.Add("SetVariable", {{"variable", std::string("Visits")}});
	const FGraphNodeId Chime = Builder.Add("PlaySound");
	const FGraphNodeId Report = Builder.Add("Log", {{"message", std::string("Visits")}});
	Builder.Link(Count, "Value", Plus, "A");
	Builder.Link(Plus, "Value", Store, "Value");
	Builder.Link(Enter, "Then", Store, "In");
	Builder.Link(Store, "Then", Branch, "In");
	Builder.Link(Count, "Value", Third, "A");
	Builder.Link(Third, "Value", Branch, "Condition");
	Builder.Link(Branch, "True", Sparks, "In");
	Builder.Link(Sparks, "Then", Reset, "In");
	Builder.Link(Branch, "False", Chime, "In");
	Builder.Link(Chime, "Then", Report, "In");
	Builder.Link(Count, "Value", Report, "Value");
	return Builder.Graph;
}

std::vector<std::string> RunVisits(const FGameplayProgram& Program, const int Visits, std::vector<float>& Frame, std::vector<std::uint32_t>* Trace = nullptr)
{
	FRecordingHost Host;
	FGameplayContext Context{.Slots = Frame, .Host = &Host, .SelfPosition = {1.f, 2.f, 3.f}, .Trace = Trace};
	Program.Run(EGameplayEvent::Start, Context);
	for (int Visit = 0; Visit < Visits; ++Visit)
	{
		Program.Run(EGameplayEvent::PlayerEnter, Context);
		Program.Run(EGameplayEvent::PlayerExit, Context);
	}

	return Host.Calls;
}

std::string ReadText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	std::erase(Text, '\r');
	return Text;
}
}

TEST_CASE("Gameplay graphs run events through branches, variables, and actions")
{
	const auto Compiled = CompileGameplayGraph(MakeCounterGraph());
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	CHECK(Compiled->Diagnostics.empty());
	const FGameplayProgram Program = FGameplayProgram::Create(std::make_shared<FCookedGameplayGraph>(Compiled->Program), EGameplayBackend::CallList);
	CHECK(Program.Handles(EGameplayEvent::PlayerEnter));
	CHECK_FALSE(Program.Handles(EGameplayEvent::Tick));
	std::vector<float> Frame = Program.MakeFrame();
	std::vector<std::uint32_t> Trace;
	const auto Calls = RunVisits(Program, 4, Frame, &Trace);
	const std::vector<std::string> Expected{
	    "sound objective 1 2 3",
	    "log Visits 1",
	    "sound objective 1 2 3",
	    "log Visits 2",
	    "effect sparks 1 2 3 2",
	    "sound objective 1 2 3",
	    "log Visits 1",
	};
	CHECK(Calls == Expected);
	// The trace names the graph's nodes in the order they ran, for the editor to light.
	REQUIRE_FALSE(Trace.empty());
	CHECK(std::ranges::contains(Trace, 7u));
}

TEST_CASE("Gameplay graph mistakes are reported on their nodes")
{
	FGraphBuilder FanOut;
	const FGraphNodeId Enter = FanOut.Add("OnPlayerEnter");
	FanOut.Link(Enter, "Then", FanOut.Add("PlaySound"), "In");
	FanOut.Link(Enter, "Then", FanOut.Add("PlaySound"), "In");
	const auto Fanned = CompileGameplayGraph(FanOut.Graph);
	REQUIRE_FALSE(Fanned);
	CHECK(Fanned.error().front().Node == Enter);

	FGraphBuilder Unknown;
	const FGraphNodeId Start = Unknown.Add("OnStart");
	const FGraphNodeId Sound = Unknown.Add("PlaySound", {{"cue", std::string("thunder")}});
	Unknown.Link(Start, "Then", Sound, "In");
	const auto Missing = CompileGameplayGraph(Unknown.Graph);
	REQUIRE_FALSE(Missing);
	CHECK(Missing.error().front().Node == Sound);

	FGraphBuilder Empty;
	Empty.Add("PlaySound");
	CHECK_FALSE(CompileGameplayGraph(Empty.Graph));

	// A node no event reaches only warns.
	FGraphBuilder Stray;
	Stray.Link(Stray.Add("OnStart"), "Then", Stray.Add("PlaySound"), "In");
	const FGraphNodeId Lost = Stray.Add("PlaySound");
	const auto Warned = CompileGameplayGraph(Stray.Graph);
	REQUIRE(Warned);
	REQUIRE(Warned->Diagnostics.size() == 1);
	CHECK(Warned->Diagnostics[0].Node == Lost);
	CHECK(Warned->Diagnostics[0].Severity == EGraphSeverity::Warning);
}

TEST_CASE("Cooked gameplay graphs round-trip and reject programs that could loop or reach outside their frame")
{
	const auto Compiled = CompileGameplayGraph(MakeCounterGraph());
	REQUIRE(Compiled);
	const auto Bytes = SerializeCookedAsset(Compiled->Program);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	CHECK(std::get<FCookedGameplayGraph>(*Loaded) == Compiled->Program);
	CHECK(HashGameplayGraph(Compiled->Program) == HashGameplayGraph(std::get<FCookedGameplayGraph>(*Loaded)));
	CHECK_FALSE(SerializeCookedAsset(Compiled->Program, 10));

	FCookedGameplayGraph Bad = Compiled->Program;
	const auto Branch = std::ranges::find(Bad.Code, EGameplayOp::Branch, &FGameplayInstruction::Op);
	REQUIRE(Branch != Bad.Code.end());
	Branch->Jump = 0;
	CHECK_FALSE(ValidateGameplayGraph(Bad));
	Bad = Compiled->Program;
	Bad.Code.front().Out = static_cast<std::uint16_t>(Bad.Slots.size());
	CHECK_FALSE(ValidateGameplayGraph(Bad));
	Bad = Compiled->Program;
	Bad.Code.pop_back();
	CHECK_FALSE(ValidateGameplayGraph(Bad));
	CHECK_FALSE(ValidateGameplayGraph({}));
}

TEST_CASE("Generated gameplay graph code is current and matches the call list")
{
	const auto Sources = CollectGameplayGraphs("Games/Sandbox/Content");
	REQUIRE_MESSAGE(Sources, (Sources ? std::string() : Sources.error()));
	REQUIRE_FALSE(Sources->empty());
	const auto Generated = GenerateGameplayGraphCpp(*Sources);
	REQUIRE_MESSAGE(Generated, (Generated ? std::string() : Generated.error()));
	CHECK_MESSAGE(*Generated == ReadText("Engine/Source/Runtime/GameFramework/Private/GeneratedGameplayGraphs.cpp"), "Run HertaEditorCmd gameplay.generate");

	for (const FGameplayGraphSource& Source : *Sources)
	{
		const auto Graph = std::make_shared<FCookedGameplayGraph>(Source.Program);
		const FGameplayProgram Calls = FGameplayProgram::Create(Graph, EGameplayBackend::CallList);
		const FGameplayProgram Native = FGameplayProgram::Create(Graph, EGameplayBackend::Native);
		REQUIRE(Native.GetBackend() == EGameplayBackend::Native);
		std::vector<float> CallFrame = Calls.MakeFrame();
		std::vector<float> NativeFrame = Native.MakeFrame();
		for (int Step = 0; Step < 12; ++Step)
		{
			FRecordingHost CallHost;
			FRecordingHost NativeHost;
			for (auto [Frame, Host, Program] : {std::tuple{&CallFrame, &CallHost, &Calls}, std::tuple{&NativeFrame, &NativeHost, &Native}})
			{
				FGameplayContext Context{.Slots = *Frame, .Host = Host, .SelfPosition = {0.5f, 1.f, -2.f}, .DeltaSeconds = 1.f / 60.f, .PlayerDistance = static_cast<float>(Step)};
				const EGameplayEvent Event = Step == 0 ? EGameplayEvent::Start : Step % 3 == 0 ? EGameplayEvent::PlayerEnter
				                                                             : Step % 3 == 1   ? EGameplayEvent::Tick
				                                                                               : EGameplayEvent::PlayerExit;
				Program->Run(Event, Context);
			}

			CHECK(CallHost.Calls == NativeHost.Calls);
			CHECK(CallFrame == NativeFrame);
		}
	}
}

TEST_CASE("Gameplay graphs cook headless")
{
	const Tests::FScratchDirectory Scratch("GameplayCook");
	const auto Text = SerializeGraph(MakeCounterGraph(), GetGameplayGraphSchema());
	REQUIRE(Text);
	Tests::WriteText(Scratch.GetPath() / "Content/Logic/Counter.hgameplaygraph", *Text);
	Tests::WriteText(Scratch.GetPath() / "Content/Logic/Counter.hgameplaygraph.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000c01\nImporter = GameplayGraph\n");
	const auto Result = CookAsset({.ContentRoot = Scratch.GetPath() / "Content", .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Logic/Counter.hgameplaygraph", .TargetPlatform = "TestPlatform"});
	REQUIRE_MESSAGE(Result, (Result ? std::string() : Result.error().Message));
	const auto Asset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Result->Key);
	REQUIRE(Asset);
	CHECK(std::get<FCookedGameplayGraph>(*Asset) == CompileGameplayGraph(MakeCounterGraph())->Program);
}
