#include "Herta/AI/AiWorld.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/StateTreeCompiler/StateTreeCompiler.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace Herta
{
namespace
{
[[nodiscard]] std::string ReadGuardTree()
{
	for (std::filesystem::path Path = std::filesystem::current_path(); !Path.empty(); Path = Path.parent_path())
	{
		const auto Candidate = Path / "Games/Sandbox/Content/AI/Guard.hstatetree";
		if (std::filesystem::exists(Candidate))
		{
			std::ifstream File(Candidate, std::ios::binary);
			return {std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
		}

		if (Path == Path.parent_path())
		{
			break;
		}
	}

	return {};
}

[[nodiscard]] FGraphNode& FindState(FGraph& Graph, const std::string_view Name)
{
	return *std::ranges::find_if(Graph.Nodes, [Name](const FGraphNode& Node)
	{
		return Node.Type == "State" && std::get<std::string>(Node.Properties[0].Value) == Name;
	});
}

void SetProperty(FGraphNode& Node, const std::string_view Key, FGraphValue Value)
{
	std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value = std::move(Value);
}

class FWalker final : public IAiBody
{
public:
	FVector3 GetFootPosition() const override
	{
		return Foot;
	}

	FVector3 GetEyePosition() const override
	{
		return Foot + FVector3{0.f, 1.6f, 0.f};
	}

	float GetYaw() const override
	{
		return Yaw;
	}

	float GetHealth() const override
	{
		return Health;
	}

	void MoveTo(const FVector3& InDestination, const float InSpeed) override
	{
		Destination = InDestination;
		Speed = InSpeed;
	}

	void Stop() override
	{
		Destination.reset();
	}

	EAiMoveStatus GetMoveStatus() const override
	{
		return Destination ? EAiMoveStatus::Moving : EAiMoveStatus::Arrived;
	}

	void Face(const FVector3& Point) override
	{
		Yaw = std::atan2(Point.X - Foot.X, Point.Z - Foot.Z);
	}

	void Animate(std::string_view) override
	{
	}

	bool StartAttack(const FVector3&) override
	{
		Swing = 0.6f;
		return true;
	}

	bool IsAttacking() const override
	{
		return Swing > 0.f;
	}

	void Advance(const float Seconds)
	{
		Swing = std::max(Swing - Seconds, 0.f);
		if (Destination)
		{
			const FVector3 Offset{Destination->X - Foot.X, 0.f, Destination->Z - Foot.Z};
			const float Distance = Offset.Length();
			if (Distance <= Speed * Seconds)
			{
				Foot = {Destination->X, 0.f, Destination->Z};
				Destination.reset();
			}
			else
			{
				Foot += Offset * (Speed * Seconds / Distance);
				Yaw = std::atan2(Offset.X, Offset.Z);
			}
		}
	}

	FVector3 Foot{};
	float Yaw = 0.f;
	float Health = 1.f;
	std::optional<FVector3> Destination{};
	float Speed = 0.f;
	float Swing = 0.f;
};

class FOpenField final : public IAiEnvironment
{
public:
	bool HasLineOfSight(const FVector3&, const FVector3&) const override
	{
		return true;
	}

	std::optional<FVector3> FindReachablePoint(const FVector3& Center, const float Radius, std::uint64_t& Random) const override
	{
		Random = Random * 6364136223846793005ull + 1442695040888963407ull;
		const float Angle = static_cast<float>(Random >> 40) / static_cast<float>(1ull << 24) * 6.2831853f;
		return Center + FVector3{std::sin(Angle), 0.f, std::cos(Angle)} * Radius * 0.7f;
	}
};

// Runs a guard for twenty seconds: it patrols, hears a noise, sees the player walk past, chases and strikes, loses it, and gets hurt.
[[nodiscard]] std::vector<FStateTreeTraceEvent> RunGuard(std::shared_ptr<const FCookedStateTree> Tree)
{
	FWalker Body;
	FOpenField Field;
	FAiWorld World;
	const FAiAgentId Agent = World.AddAgent({.Name = "Guard", .Body = &Body, .Tree = std::move(Tree), .Seed = 11});
	std::vector<FStateTreeTraceEvent> Trace;
	World.SetTrace(Agent, &Trace);
	for (int Step = 0; Step < 1200; ++Step)
	{
		const float Seconds = static_cast<float>(Step) / 60.f;
		if (Step == 120)
		{
			World.PostNoise({4.f, 0.f, 4.f}, 30.f);
		}

		if (Seconds > 5.f && Seconds < 11.f)
		{
			const FVector3 Player{-6.f + Seconds, 0.f, 3.f};
			World.SetTarget(FAiTarget{.Foot = Player, .Head = Player + FVector3{0.f, 1.7f, 0.f}});
		}
		else
		{
			World.SetTarget(std::nullopt);
		}

		if (Step == 900)
		{
			Body.Health = 0.4f;
			World.ReportHit(Agent, {2.f, 0.f, 2.f});
		}

		World.Step(1.f / 60.f, Field);
		Body.Advance(1.f / 60.f);
	}

	World.SetTrace(Agent, nullptr);
	return Trace;
}
}

TEST_CASE("The default StateTree graph compiles to idle and wander states that hand over to each other")
{
	const FGraph Graph = MakeDefaultStateTreeGraph();
	const auto Compiled = CompileStateTree(Graph);
	REQUIRE(Compiled);
	const FCookedStateTree& Tree = Compiled->Tree;
	REQUIRE(Tree.States.size() == 3);
	CHECK(Tree.States[1].Name == "Idle");
	CHECK(Tree.States[2].Name == "Wander");
	CHECK(Tree.States[0].FirstChild == 1);
	CHECK(Tree.States[1].NextSibling == 2);
	REQUIRE(Tree.Transitions.size() == 2);
	CHECK(Tree.Transitions[Tree.States[1].FirstTransition].Target == 2);
	CHECK(Tree.Transitions[Tree.States[2].FirstTransition].Target == 1);
	CHECK(Tree.MaximumActiveTasks == 1);
	CHECK(Compiled->Diagnostics.empty());
}

TEST_CASE("StateTree compilation depends on order and links, never on canvas positions")
{
	const std::string Text = ReadGuardTree();
	REQUIRE(!Text.empty());
	auto Graph = DeserializeGraph(Text, GetStateTreeSchema());
	REQUIRE(Graph);
	const auto Compiled = CompileStateTree(*Graph);
	REQUIRE(Compiled);
	CHECK(Compiled->Diagnostics.empty());
	const auto Bytes = SerializeCookedAsset(FCookedAsset{Compiled->Tree});
	REQUIRE(Bytes);

	// Canonical text round trips, and moving every node compiles to the same bytes.
	const auto Canonical = SerializeGraph(*Graph, GetStateTreeSchema());
	REQUIRE(Canonical);
	auto Moved = *DeserializeGraph(*Canonical, GetStateTreeSchema());
	for (FGraphNode& Node : Moved.Nodes)
	{
		Node.Position = {Node.Position[1] * 0.5f, -Node.Position[0]};
	}

	const auto MovedCompiled = CompileStateTree(Moved);
	REQUIRE(MovedCompiled);
	CHECK(*SerializeCookedAsset(FCookedAsset{MovedCompiled->Tree}) == *Bytes);

	// Order decides which sibling comes first.
	SetProperty(FindState(Moved, "Patrol"), "order", std::int32_t{-1});
	const auto Reordered = CompileStateTree(Moved);
	REQUIRE(Reordered);
	CHECK(Reordered->Tree.States[1].Name == "Patrol");
}

TEST_CASE("StateTree compilation reports duplicate names, missing targets, endless loops, and orphans")
{
	FGraph Graph = MakeDefaultStateTreeGraph();
	SetProperty(FindState(Graph, "Wander"), "name", std::string("Idle"));
	auto Compiled = CompileStateTree(Graph);
	REQUIRE(!Compiled);
	CHECK(std::ranges::any_of(Compiled.error(), [](const FGraphDiagnostic& Diagnostic)
	{
		return Diagnostic.Message.contains("already named");
	}));

	Graph = MakeDefaultStateTreeGraph();
	for (FGraphNode& Node : Graph.Nodes)
	{
		if (Node.Type == "Transition")
		{
			SetProperty(Node, "target", std::string("Nowhere"));
			break;
		}
	}

	Compiled = CompileStateTree(Graph);
	REQUIRE(!Compiled);
	CHECK(Compiled.error()[0].Message.contains("Nowhere"));

	// Without their tasks, Idle and Wander finish at once and pass to each other forever.
	Graph = MakeDefaultStateTreeGraph();
	std::erase_if(Graph.Links, [&Graph](const FGraphLink& Link)
	{
		const FGraphNode* const Node = FindGraphNode(Graph, Link.To.Node);
		return IsStateTreeTask(Node->Type);
	});

	Compiled = CompileStateTree(Graph);
	REQUIRE(!Compiled);
	CHECK(Compiled.error()[0].Message.contains("forever"));

	// A state hanging off nothing compiles with a warning and is left out.
	Graph = MakeDefaultStateTreeGraph();
	auto Orphan = MakeGraphNode(GetStateTreeSchema(), "State", NextGraphNodeId(Graph), {0.f, 600.f});
	REQUIRE(Orphan);
	SetProperty(*Orphan, "name", std::string("Orphan"));
	Graph.Nodes.push_back(*Orphan);
	const auto Warned = CompileStateTree(Graph);
	REQUIRE(Warned);
	CHECK(Warned->Tree.States.size() == 3);
	REQUIRE(Warned->Diagnostics.size() == 1);
	CHECK(Warned->Diagnostics[0].Severity == EGraphSeverity::Warning);
	CHECK(Warned->Diagnostics[0].Node == Orphan->Id);

	// An event transition needs its event.
	Graph = MakeDefaultStateTreeGraph();
	for (FGraphNode& Node : Graph.Nodes)
	{
		if (Node.Type == "Transition")
		{
			SetProperty(Node, "trigger", std::string("event"));
			break;
		}
	}

	CHECK(!CompileStateTree(Graph));
}

TEST_CASE("The Sandbox guard's compiled StateTree runs the same headless trace after a cook and reload")
{
	auto Graph = DeserializeGraph(ReadGuardTree(), GetStateTreeSchema());
	REQUIRE(Graph);
	const auto Compiled = CompileStateTree(*Graph);
	REQUIRE(Compiled);
	const auto Bytes = SerializeCookedAsset(FCookedAsset{Compiled->Tree});
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	REQUIRE(std::holds_alternative<FCookedStateTree>(*Loaded));

	const auto Original = RunGuard(std::make_shared<const FCookedStateTree>(Compiled->Tree));
	const auto Reloaded = RunGuard(std::make_shared<const FCookedStateTree>(std::get<FCookedStateTree>(*Loaded)));
	CHECK(Original == Reloaded);
	CHECK(Original.size() > 20);

	// The run passes through patrol, investigation, the fight, and the wounded flight.
	const auto Entered = [&](const std::string_view Name)
	{
		const auto State = std::ranges::find(Compiled->Tree.States, Name, &FStateTreeStateDesc::Name);
		const auto Index = static_cast<std::uint16_t>(State - Compiled->Tree.States.begin());
		return std::ranges::contains(Original, FStateTreeTraceEvent{.Type = EStateTreeTraceType::Enter, .State = Index}, [](const FStateTreeTraceEvent& Event)
		{
			return FStateTreeTraceEvent{.Type = Event.Type, .State = Event.State};
		});
	};

	CHECK(Entered("Stroll"));
	CHECK(Entered("Go look"));
	CHECK(Entered("Chase"));
	CHECK(Entered("Strike"));
	CHECK(Entered("Run"));
}
}
