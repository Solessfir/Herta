#include "Herta/AnimationGraph/AnimationGraph.h"
#include "Herta/GameFramework/GameSession.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace Herta
{
namespace
{
FAnimationStateMachineDesc MakeSmallMachine()
{
	return {
	    .Parameters = {{.Name = "Speed"}, {.Name = "Jump", .Type = EAnimationParameterType::Trigger}, {.Name = "Grounded", .Type = EAnimationParameterType::Bool, .Default = 1.f}},
	    .States = {
	        {.Name = "Idle", .Samples = {{.Clip = "Idle_Loop"}}, .Events = {{.Name = "Breath", .Phase = 0.5f}}},
	        {.Name = "Run", .Samples = {{.Clip = "Walk_Loop", .Position = 0.f}, {.Clip = "Sprint_Loop", .Position = 7.f}}, .BlendParameter = 0, .Speed = 1.25f},
	        {.Name = "Air", .Samples = {{.Clip = "Jump_Loop"}}, .bLoop = false},
	    },
	    .Transitions = {
	        {.From = AnyAnimationState, .To = 2, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = 1, .Test = EAnimationCondition::Triggered}}},
	        {.From = 0, .To = 1, .Conditions = {{.Parameter = 0, .Test = EAnimationCondition::Greater, .Value = 0.5f}}},
	        {.From = 1, .To = 0, .Conditions = {{.Parameter = 0, .Test = EAnimationCondition::Less, .Value = 0.25f}, {.Parameter = 2, .Test = EAnimationCondition::IsTrue}}},
	        {.From = 2, .To = 0, .DurationSeconds = 0.f, .Conditions = {{.Parameter = 2, .Test = EAnimationCondition::IsTrue}}, .bAtEnd = true},
	    },
	    .Entry = 0,
	};
}

FGraphNode& FindNode(FGraph& Graph, const std::string_view Type, const std::size_t Nth = 0)
{
	std::size_t Seen = 0;
	const auto Found = std::ranges::find_if(Graph.Nodes, [&](const FGraphNode& Node)
	{
		return Node.Type == Type && Seen++ == Nth;
	});

	REQUIRE(Found != Graph.Nodes.end());
	return *Found;
}

void SetProperty(FGraphNode& Node, const std::string_view Key, FGraphValue Value)
{
	const auto Property = std::ranges::find(Node.Properties, Key, &FGraphProperty::Key);
	REQUIRE(Property != Node.Properties.end());
	Property->Value = std::move(Value);
}

bool Fails(const FGraph& Graph, const std::string_view Message)
{
	const auto Compiled = CompileAnimationGraph(Graph);
	return !Compiled && std::ranges::any_of(Compiled.error(), [&](const FGraphDiagnostic& Diagnostic)
	{
		return Diagnostic.Message.find(Message) != std::string::npos;
	});
}
}

TEST_CASE("Animation graphs carry a state machine through the canvas format unchanged")
{
	for (const FAnimationStateMachineDesc& Machine : {MakeSmallMachine(), MakeDefaultPlayerStateMachine()})
	{
		const FGraph Graph = MakeAnimationGraph(Machine);
		CHECK(ValidateGraph(Graph, GetAnimationGraphSchema()).empty());
		const auto Text = SerializeGraph(Graph, GetAnimationGraphSchema());
		REQUIRE(Text);
		const auto Loaded = DeserializeGraph(*Text, GetAnimationGraphSchema());
		REQUIRE(Loaded);
		const auto Compiled = CompileAnimationGraph(*Loaded);
		REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
		CHECK(Compiled->StateMachine == Machine);
		CHECK(Compiled->StateNodes.size() == Machine.States.size());
		REQUIRE(Compiled->TransitionNodes.size() == Machine.Transitions.size());
		CHECK(FindGraphNode(*Loaded, Compiled->TransitionNodes.front())->Type == "Transition");
	}
}

// The Sandbox's Mannequin plays the built-in machine from its own graph file; when they part, the expected file is written beside the build
// to copy over it.
TEST_CASE("The Sandbox Mannequin's animation graph is the built-in player machine")
{
	const auto Text = SerializeGraph(MakeAnimationGraph(MakeDefaultPlayerStateMachine()), GetAnimationGraphSchema());
	REQUIRE(Text);
	std::ifstream Stream("Games/Sandbox/Content/Characters/Mannequin/Mannequin.hanimgraph", std::ios::binary);
	const std::string File{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	if (File != *Text)
	{
		std::filesystem::create_directories("Saved/Tests");
		std::ofstream("Saved/Tests/Mannequin.hanimgraph", std::ios::binary) << *Text;
	}

	CHECK_MESSAGE(File == *Text, "Copy Saved/Tests/Mannequin.hanimgraph over the Sandbox's to match the built-in machine");
}

TEST_CASE("Animation graph transitions compile by priority and any-state transitions leave From unlinked")
{
	FGraph Graph = MakeAnimationGraph(MakeSmallMachine());
	// The last transition, given the lowest priority number, is tested first; the rest keep their order.
	SetProperty(FindNode(Graph, "Transition", 3), "priority", std::int32_t{-1});
	const auto Compiled = CompileAnimationGraph(Graph);
	REQUIRE(Compiled);
	REQUIRE(Compiled->StateMachine.Transitions.size() == 4);
	CHECK(Compiled->StateMachine.Transitions[0].From == 2);
	CHECK(Compiled->StateMachine.Transitions[1].From == AnyAnimationState);
	CHECK(Compiled->TransitionNodes[0] == FindNode(Graph, "Transition", 3).Id);
}

TEST_CASE("Animation graphs reject what the state machine could not run, naming the node")
{
	const FGraph Valid = MakeAnimationGraph(MakeSmallMachine());
	const auto Edited = [&](const std::string_view Type, const std::size_t Nth, const std::string_view Key, FGraphValue Value)
	{
		FGraph Graph = Valid;
		SetProperty(FindNode(Graph, Type, Nth), Key, std::move(Value));
		return Graph;
	};

	CHECK(Fails(Edited("Transition", 1, "conditions", std::string("Speed")), "needs a bool or trigger"));
	CHECK(Fails(Edited("Transition", 1, "conditions", std::string("!Jump")), "needs a bool parameter"));
	CHECK(Fails(Edited("Transition", 1, "conditions", std::string("Missing > 1")), "not a parameter"));
	CHECK(Fails(Edited("Transition", 1, "conditions", std::string("Grounded > 1")), "compares a float"));
	CHECK(Fails(Edited("State", 1, "name", std::string("Idle")), "must be unique"));
	CHECK(Fails(Edited("State", 1, "blend", std::string()), "needs a blend parameter"));
	CHECK(Fails(Edited("State", 0, "clips", std::string(" , ")), "exactly one of clips"));
	CHECK(Fails(Edited("State", 0, "events", std::string("Breath@2")), "phase from 0 to 1"));
	CHECK(Fails(Edited("Parameter", 0, "type", std::string("Vector")), "Float, Bool, or Trigger"));

	FGraph NoEntry = Valid;
	std::erase_if(NoEntry.Links, [&](const FGraphLink& Link)
	{
		return Link.To.Node == FindNode(NoEntry, "Entry").Id;
	});

	CHECK(Fails(NoEntry, "Link the Entry node"));
	FGraph Broken = Edited("Transition", 2, "conditions", std::string("Jump > 1"));
	const auto Compiled = CompileAnimationGraph(Broken);
	REQUIRE_FALSE(Compiled);
	CHECK(Compiled.error().front().Node == FindNode(Broken, "Transition", 2).Id);
}

TEST_CASE("Animation graphs carry blend trees, nested machines, and pose rotations, and older graphs upgrade on load")
{
	const FAnimationStateMachineDesc Machine{
	    .Parameters = {{.Name = "Speed"}, {.Name = "Turn"}, {.Name = "Strike", .Type = EAnimationParameterType::Trigger}, {.Name = "Pitch"}},
	    .States = {
	        {.Name = "Move", .Tree = {
	                             {.Type = EAnimationBlendNodeType::Additive, .ParameterX = 1, .Weight = 0.5f, .Children = {{.Node = 1}, {.Node = 4}}},
	                             {.Type = EAnimationBlendNodeType::Blend2D, .ParameterX = 0, .ParameterY = 1, .Children = {{.Node = 2, .X = 0.f, .Y = 0.f}, {.Node = 3, .X = 2.f, .Y = 1.f}}},
	                             {.Clip = "Idle_Loop"},
	                             {.Clip = "Walk_Loop"},
	                             {.Clip = "Hit_Chest"},
	                         }},
	        {.Name = "Fight", .SubMachine = 0},
	    },
	    .Transitions = {{.From = 0, .To = 1, .Conditions = {{.Parameter = 2, .Test = EAnimationCondition::Triggered}}}},
	    .SubMachines = {{
	        .States = {
	            {.Name = "Guard", .Tree = {{.Type = EAnimationBlendNodeType::Layered, .MaskJoint = "spine_01", .Children = {{.Node = 1}, {.Node = 2}}}, {.Clip = "Idle_Loop"}, {.Clip = "Sword_Block"}}},
	            {.Name = "Cut", .Samples = {{.Clip = "Sword_Regular_A"}}, .bLoop = false},
	        },
	        .Transitions = {{.From = 0, .To = 1, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = 2, .Test = EAnimationCondition::Triggered}}}},
	    }},
	    .PoseOperations = {{.Joint = "spine_02", .Axis = {1.f, 0.f, 0.f}, .Parameter = 3, .Scale = -0.3f}},
	};

	const FGraph Graph = MakeAnimationGraph(Machine);
	CHECK(ValidateGraph(Graph, GetAnimationGraphSchema()).empty());
	const auto Text = SerializeGraph(Graph, GetAnimationGraphSchema());
	REQUIRE(Text);
	const auto Loaded = DeserializeGraph(*Text, GetAnimationGraphSchema());
	REQUIRE(Loaded);
	const auto Compiled = CompileAnimationGraph(*Loaded);
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	CHECK(Compiled->StateMachine == Machine);
	CHECK(Compiled->StateNodes.size() == Machine.States.size());

	// A version 1 file, from before these nodes, loads at the current version with the new properties at their defaults.
	std::string Older = *SerializeGraph(MakeAnimationGraph(MakeSmallMachine()), GetAnimationGraphSchema());
	const std::string Current = std::format("\"domainVersion\": {}", AnimationGraphVersion);
	REQUIRE(Older.find(Current) != std::string::npos);
	Older.replace(Older.find(Current), Current.size(), "\"domainVersion\": 1");
	const auto Upgraded = DeserializeGraph(Older, GetAnimationGraphSchema());
	REQUIRE_MESSAGE(Upgraded, (Upgraded ? std::string() : Upgraded.error().Message));
	CHECK(Upgraded->DomainVersion == AnimationGraphVersion);
	CHECK(CompileAnimationGraph(*Upgraded));

	// A machine no state plays, and a state given both clips and a pose, are mistakes.
	FGraph Orphan = Graph;
	for (FGraphNode& Node : Orphan.Nodes)
	{
		if (Node.Type == "State" && std::get<std::string>(std::ranges::find(Node.Properties, std::string_view("name"), &FGraphProperty::Key)->Value) == "Fight")
		{
			SetProperty(Node, "plays", std::string());
			SetProperty(Node, "clips", std::string("Idle_Loop"));
		}
	}

	CHECK(Fails(Orphan, "No state plays machine"));
	FGraph Both = Graph;
	SetProperty(FindNode(Both, "State", 0), "clips", std::string("Idle_Loop"));
	CHECK(Fails(Both, "exactly one of clips"));
}
}
