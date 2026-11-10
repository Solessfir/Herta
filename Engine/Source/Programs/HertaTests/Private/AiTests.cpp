#include "Herta/AI/AiWorld.h"
#include "Herta/AI/StateTreeRuntime.h"
#include "Herta/Assets/CookedAsset.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace Herta
{
namespace
{
// Lays out a tree depth first from states added parents first, the way the StateTree compiler does.
class FTreeBuilder
{
public:
	std::uint16_t State(const std::string& Name, const std::uint16_t Parent = StateTreeNone, const EStateTreeSelection Selection = EStateTreeSelection::FirstEligible, const float Score = 0.f)
	{
		Entries.push_back({.Desc = {.Name = Name, .Node = static_cast<std::uint32_t>(Entries.size() + 1), .Parent = Parent, .Selection = Selection, .Score = Score}});
		return static_cast<std::uint16_t>(Entries.size() - 1);
	}

	void Task(const std::uint16_t State, const FStateTreeTaskDesc& Task)
	{
		Entries[State].Tasks.push_back(Task);
	}

	void Condition(const std::uint16_t State, const FStateTreeConditionDesc& Condition)
	{
		Entries[State].Conditions.push_back(Condition);
	}

	void Transition(const std::uint16_t State, const FStateTreeTransitionDesc& Transition, const std::vector<FStateTreeConditionDesc>& Conditions = {})
	{
		Entries[State].Transitions.emplace_back(Transition, Conditions);
	}

	void Consideration(const std::uint16_t State, const FStateTreeConsiderationDesc& Consideration)
	{
		Entries[State].Considerations.push_back(Consideration);
	}

	std::uint16_t Name(const std::string& Text)
	{
		const auto Found = std::ranges::find(Names, Text);
		if (Found != Names.end())
		{
			return static_cast<std::uint16_t>(Found - Names.begin());
		}

		Names.push_back(Text);
		return static_cast<std::uint16_t>(Names.size() - 1);
	}

	// States must be added so that each subtree is contiguous, as a depth-first walk adds them.
	[[nodiscard]] std::shared_ptr<const FCookedStateTree> Finish()
	{
		FCookedStateTree Tree;
		Tree.Names = Names;
		std::vector<std::uint16_t> Active(Entries.size(), 0);
		for (std::size_t Index = 0; Index < Entries.size(); ++Index)
		{
			FStateTreeStateDesc State = Entries[Index].Desc;
			if (State.Parent != StateTreeNone)
			{
				FStateTreeStateDesc& Parent = Tree.States[State.Parent];
				State.Depth = static_cast<std::uint16_t>(Parent.Depth + 1);
				if (Parent.FirstChild == StateTreeNone)
				{
					Parent.FirstChild = static_cast<std::uint16_t>(Index);
				}
				else
				{
					std::uint16_t Last = Parent.FirstChild;
					while (Tree.States[Last].NextSibling != StateTreeNone)
					{
						Last = Tree.States[Last].NextSibling;
					}

					Tree.States[Last].NextSibling = static_cast<std::uint16_t>(Index);
				}
			}

			State.FirstTask = static_cast<std::uint16_t>(Tree.Tasks.size());
			State.TaskCount = static_cast<std::uint16_t>(Entries[Index].Tasks.size());
			Tree.Tasks.insert(Tree.Tasks.end(), Entries[Index].Tasks.begin(), Entries[Index].Tasks.end());
			State.FirstCondition = static_cast<std::uint16_t>(Tree.Conditions.size());
			State.ConditionCount = static_cast<std::uint16_t>(Entries[Index].Conditions.size());
			Tree.Conditions.insert(Tree.Conditions.end(), Entries[Index].Conditions.begin(), Entries[Index].Conditions.end());
			State.FirstConsideration = static_cast<std::uint16_t>(Tree.Considerations.size());
			State.ConsiderationCount = static_cast<std::uint16_t>(Entries[Index].Considerations.size());
			Tree.Considerations.insert(Tree.Considerations.end(), Entries[Index].Considerations.begin(), Entries[Index].Considerations.end());
			Tree.States.push_back(State);
			Active[Index] = static_cast<std::uint16_t>(State.TaskCount + (State.Parent == StateTreeNone ? 0 : Active[State.Parent]));
			Tree.MaximumActiveTasks = std::max(Tree.MaximumActiveTasks, Active[Index]);
		}

		for (std::size_t Index = 0; Index < Entries.size(); ++Index)
		{
			Tree.States[Index].FirstTransition = static_cast<std::uint16_t>(Tree.Transitions.size());
			Tree.States[Index].TransitionCount = static_cast<std::uint16_t>(Entries[Index].Transitions.size());
			for (auto [Transition, Conditions] : Entries[Index].Transitions)
			{
				Transition.FirstCondition = static_cast<std::uint16_t>(Tree.Conditions.size());
				Transition.ConditionCount = static_cast<std::uint16_t>(Conditions.size());
				Tree.Conditions.insert(Tree.Conditions.end(), Conditions.begin(), Conditions.end());
				Tree.Transitions.push_back(Transition);
			}
		}

		REQUIRE(ValidateStateTree(Tree));
		return std::make_shared<const FCookedStateTree>(std::move(Tree));
	}

private:
	struct FEntry
	{
		FStateTreeStateDesc Desc{};
		std::vector<FStateTreeTaskDesc> Tasks{};
		std::vector<FStateTreeConditionDesc> Conditions{};
		std::vector<std::pair<FStateTreeTransitionDesc, std::vector<FStateTreeConditionDesc>>> Transitions{};
		std::vector<FStateTreeConsiderationDesc> Considerations{};
	};

	std::vector<FEntry> Entries{};
	std::vector<std::string> Names{};
};

// Waits finish after their first value in seconds; conditions read named switches; inputs are fixed.
class FScriptContext final : public IStateTreeContext
{
public:
	EStateTreeStatus StartTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance) override
	{
		++Started;
		Instance.Duration = Task.Values[0];
		return Task.Type == EStateTreeTaskType::Wait ? EStateTreeStatus::Running : EStateTreeStatus::Succeeded;
	}

	EStateTreeStatus TickTask(const FStateTreeTaskDesc&, FStateTreeTaskInstance& Instance, const float Seconds) override
	{
		Instance.Seconds += Seconds;
		return Instance.Seconds + 1.e-4f >= Instance.Duration ? (Instance.Duration < 0.f ? EStateTreeStatus::Failed : EStateTreeStatus::Succeeded) : EStateTreeStatus::Running;
	}

	void StopTask(const FStateTreeTaskDesc&, FStateTreeTaskInstance&) override
	{
		++Stopped;
	}

	bool TestCondition(const FStateTreeConditionDesc& Condition) override
	{
		return Condition.Type == EStateTreeConditionType::SeesTarget && bSees;
	}

	float ReadInput(const EStateTreeInput Input) override
	{
		return Input == EStateTreeInput::TargetDistance ? Distance : 1.f;
	}

	bool bSees = false;
	float Distance = 10.f;
	int Started = 0;
	int Stopped = 0;
};

[[nodiscard]] std::vector<std::string> Names(const FStateTreeExecution& Execution)
{
	std::vector<std::string> Result;
	for (const std::uint16_t State : Execution.GetActiveStates())
	{
		Result.push_back(Execution.GetProgram().States[State].Name);
	}

	return Result;
}
}

TEST_CASE("StateTrees select, run tasks, and take transitions from the leaf up with a deterministic trace")
{
	FTreeBuilder Builder;
	const std::uint16_t Root = Builder.State("Root");
	const std::uint16_t Combat = Builder.State("Combat", Root);
	Builder.Condition(Combat, {.Type = EStateTreeConditionType::SeesTarget});
	const std::uint16_t Strike = Builder.State("Strike", Combat);
	Builder.Task(Strike, {.Type = EStateTreeTaskType::Wait, .Values = {0.3f, 0.f}});
	const std::uint16_t Idle = Builder.State("Idle", Root);
	const std::uint16_t Rest = Builder.State("Rest", Idle);
	Builder.Task(Rest, {.Type = EStateTreeTaskType::Wait, .Values = {0.2f, 0.f}});
	const std::uint16_t Look = Builder.State("Look", Idle);
	Builder.Task(Look, {.Type = EStateTreeTaskType::Wait, .Values = {0.1f, 0.f}});
	Builder.Transition(Rest, {.Trigger = EStateTreeTrigger::OnSucceeded, .Target = Look});
	Builder.Transition(Idle, {.Trigger = EStateTreeTrigger::OnTick, .Target = Combat}, {{.Type = EStateTreeConditionType::SeesTarget}});
	Builder.Transition(Combat, {.Trigger = EStateTreeTrigger::OnEvent, .Kind = EStateTreeTransitionKind::GotoState, .Target = Idle, .Event = Builder.Name("Hit")});
	const auto Tree = Builder.Finish();

	FStateTreeInstancePool Pool;
	const auto Run = [&](std::vector<FStateTreeTraceEvent>& Trace)
	{
		FScriptContext Context;
		FStateTreeExecution Execution(Tree, Pool);
		Execution.SetTrace(&Trace);
		std::vector<std::vector<std::string>> Paths;
		for (int Tick = 0; Tick < 12; ++Tick)
		{
			Context.bSees = Tick >= 6 && Tick < 9;
			if (Tick == 8)
			{
				Execution.PostEvent("Hit");
				Execution.PostEvent("Unknown");
			}

			Execution.Tick(0.1f, Context);
			Paths.push_back(Names(Execution));
		}

		Execution.Stop(Context);
		CHECK(Context.Stopped > 0);
		return Paths;
	};

	std::vector<FStateTreeTraceEvent> First;
	const auto Paths = Run(First);
	CHECK(Paths[0] == std::vector<std::string>{"Root", "Idle", "Rest"});
	CHECK(Paths[1] == std::vector<std::string>{"Root", "Idle", "Look"});
	// Look finishes and nothing handles it, so the tree starts over and Idle's first child runs again.
	CHECK(Paths[2] == std::vector<std::string>{"Root", "Idle", "Rest"});
	CHECK(Paths[6] == std::vector<std::string>{"Root", "Combat", "Strike"});
	// The Hit event sends Combat back to Idle even while the target is still in view.
	CHECK(Paths[8] == std::vector<std::string>{"Root", "Idle", "Rest"});

	std::vector<FStateTreeTraceEvent> Second;
	CHECK(Run(Second) == Paths);
	CHECK(First == Second);
	CHECK(std::ranges::contains(First, EStateTreeTraceType::Event, &FStateTreeTraceEvent::Type));
	CHECK(Pool.GetSlotCount() == 1);
}

TEST_CASE("StateTree utility selection, finishing transitions, and the per-tick transition limit")
{
	FTreeBuilder Builder;
	const std::uint16_t Root = Builder.State("Root", StateTreeNone, EStateTreeSelection::Utility);
	const std::uint16_t Near = Builder.State("Near", Root, EStateTreeSelection::FirstEligible, 0.f);
	Builder.Consideration(Near, {.Input = EStateTreeInput::TargetDistance, .Minimum = 0.f, .Maximum = 10.f, .Weight = 2.f, .bInvert = true});
	const std::uint16_t Probe = Builder.State("Probe", Near);
	Builder.Task(Probe, {.Type = EStateTreeTaskType::Wait, .Values = {-1.f, 0.f}});
	Builder.Transition(Probe, {.Trigger = EStateTreeTrigger::OnFailed, .Kind = EStateTreeTransitionKind::Fail});
	const std::uint16_t Far = Builder.State("Far", Root, EStateTreeSelection::FirstEligible, 0.5f);
	Builder.Transition(Near, {.Trigger = EStateTreeTrigger::OnFailed, .Target = Far});
	const std::uint16_t Ping = Builder.State("Ping", Far);
	const std::uint16_t Pong = Builder.State("Pong", Far);
	// Two empty leaves that hand over to each other forever.
	Builder.Transition(Ping, {.Trigger = EStateTreeTrigger::OnCompleted, .Target = Pong});
	Builder.Transition(Pong, {.Trigger = EStateTreeTrigger::OnCompleted, .Target = Ping});
	const auto Tree = Builder.Finish();
	CHECK(Tree->States[Ping].Name == "Ping");

	FStateTreeInstancePool Pool;
	FScriptContext Context;
	FStateTreeExecution Execution(Tree, Pool);
	std::vector<FStateTreeTraceEvent> Trace;
	Execution.SetTrace(&Trace);

	// Far scores 0.5; Near scores 2 * (1 - 9 / 10) = 0.2 at 9 m and 1.8 at 1 m.
	Context.Distance = 9.f;
	Execution.Tick(0.1f, Context);
	CHECK(std::ranges::contains(Names(Execution), std::string("Far")));
	CHECK(std::ranges::contains(Trace, EStateTreeTraceType::TransitionLimit, &FStateTreeTraceEvent::Type));

	Execution.Stop(Context);
	Trace.clear();
	Context.Distance = 1.f;
	Execution.Tick(0.1f, Context);
	// The failing probe fails Near, whose failure transition goes to Far.
	CHECK(std::ranges::contains(Names(Execution), std::string("Far")));
	CHECK(std::ranges::contains(Trace, FStateTreeTraceEvent{.Tick = 2, .Type = EStateTreeTraceType::Enter, .State = Probe}));
}

TEST_CASE("Cooked StateTrees round trip and reject broken layouts")
{
	FTreeBuilder Builder;
	const std::uint16_t Root = Builder.State("Root");
	const std::uint16_t Leaf = Builder.State("Leaf", Root);
	Builder.Task(Leaf, {.Type = EStateTreeTaskType::Animate, .Key = Builder.Name("Hit")});
	const auto Tree = Builder.Finish();
	const auto Bytes = SerializeCookedAsset(FCookedAsset{*Tree});
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	REQUIRE(std::holds_alternative<FCookedStateTree>(*Loaded));
	CHECK(std::get<FCookedStateTree>(*Loaded) == *Tree);

	FCookedStateTree Broken = *Tree;
	Broken.States[1].Parent = 1;
	CHECK_FALSE(ValidateStateTree(Broken));
	Broken = *Tree;
	Broken.MaximumActiveTasks = 0;
	CHECK_FALSE(ValidateStateTree(Broken));
	Broken = *Tree;
	Broken.Tasks[0].Key = 7;
	CHECK_FALSE(ValidateStateTree(Broken));
	CHECK_FALSE(DeserializeCookedAsset(std::span(*Bytes).first(Bytes->size() - 1)));
}

namespace
{
// Walks straight toward its destination at its speed, which is all the AI needs to see its tasks play out.
class FTestBody final : public IAiBody
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
		Status = EAiMoveStatus::Moving;
	}

	void Stop() override
	{
		Destination.reset();
		Status = EAiMoveStatus::Idle;
	}

	EAiMoveStatus GetMoveStatus() const override
	{
		return Status;
	}

	void Face(const FVector3& Point) override
	{
		Yaw = std::atan2(Point.X - Foot.X, Point.Z - Foot.Z);
	}

	void Animate(const std::string_view Trigger) override
	{
		Animations.emplace_back(Trigger);
	}

	bool StartAttack(const FVector3&) override
	{
		if (AttackSeconds > 0.f)
		{
			return false;
		}

		AttackSeconds = 0.5f;
		++Attacks;
		return true;
	}

	bool IsAttacking() const override
	{
		return AttackSeconds > 0.f;
	}

	void Advance(const float Seconds)
	{
		AttackSeconds = std::max(AttackSeconds - Seconds, 0.f);
		if (!Destination)
		{
			return;
		}

		const FVector3 Offset{Destination->X - Foot.X, 0.f, Destination->Z - Foot.Z};
		const float Distance = Offset.Length();
		if (Distance <= Speed * Seconds)
		{
			Foot = {Destination->X, Foot.Y, Destination->Z};
			Destination.reset();
			Status = EAiMoveStatus::Arrived;
			return;
		}

		Foot += Offset * (Speed * Seconds / Distance);
		Yaw = std::atan2(Offset.X, Offset.Z);
	}

	FVector3 Foot{};
	float Yaw = 0.f;
	float Health = 1.f;
	std::optional<FVector3> Destination{};
	float Speed = 0.f;
	EAiMoveStatus Status = EAiMoveStatus::Idle;
	std::vector<std::string> Animations{};
	float AttackSeconds = 0.f;
	int Attacks = 0;
};

class FTestEnvironment final : public IAiEnvironment
{
public:
	bool HasLineOfSight(const FVector3& From, const FVector3& To) const override
	{
		// A wall along x = 5 blocks the view across it.
		return !bWall || (From.X - 5.f) * (To.X - 5.f) > 0.f;
	}

	std::optional<FVector3> FindReachablePoint(const FVector3& Center, const float Radius, std::uint64_t& Random) const override
	{
		Random = Random * 6364136223846793005ull + 1442695040888963407ull;
		const float Angle = static_cast<float>(Random >> 40) / static_cast<float>(1ull << 24) * 6.2831853f;
		return Center + FVector3{std::sin(Angle), 0.f, std::cos(Angle)} * Radius * 0.5f;
	}

	bool bWall = false;
};

// A guard: chase and swing at the player while it is seen, look where it was last seen, investigate noises, and otherwise wander.
[[nodiscard]] std::shared_ptr<const FCookedStateTree> MakeGuardTree()
{
	FTreeBuilder Builder;
	const std::uint16_t Root = Builder.State("Root");
	const std::uint16_t Fight = Builder.State("Fight", Root);
	Builder.Condition(Fight, {.Type = EStateTreeConditionType::SeesTarget});
	const std::uint16_t Chase = Builder.State("Chase", Fight);
	Builder.Task(Chase, {.Type = EStateTreeTaskType::MoveTo, .Target = EStateTreeTarget::Player, .Values = {1.f, 1.2f}});
	const std::uint16_t Swing = Builder.State("Swing", Fight);
	Builder.Task(Swing, {.Type = EStateTreeTaskType::Attack, .Target = EStateTreeTarget::Player, .Values = {1.6f, 0.f}});
	Builder.Transition(Chase, {.Trigger = EStateTreeTrigger::OnSucceeded, .Target = Swing});
	const std::uint16_t Search = Builder.State("Search", Root);
	Builder.Condition(Search, {.Type = EStateTreeConditionType::RemembersTarget});
	Builder.Task(Search, {.Type = EStateTreeTaskType::MoveTo, .Target = EStateTreeTarget::LastSeen, .Values = {0.f, 0.8f}});
	const std::uint16_t Shrug = Builder.State("Shrug", Root);
	Builder.Condition(Shrug, {.Type = EStateTreeConditionType::RemembersTarget});
	Builder.Task(Shrug, {.Type = EStateTreeTaskType::Forget});
	Builder.Transition(Search, {.Trigger = EStateTreeTrigger::OnCompleted, .Target = Shrug});
	// A lost target is searched for rather than chased where it no longer is.
	Builder.Transition(Fight, {.Trigger = EStateTreeTrigger::OnTick, .Target = Search}, {{.Type = EStateTreeConditionType::SeesTarget, .bInvert = true}});
	const std::uint16_t Investigate = Builder.State("Investigate", Root);
	Builder.Condition(Investigate, {.Type = EStateTreeConditionType::HeardNoise});
	const std::uint16_t Listen = Builder.State("Listen", Investigate);
	Builder.Task(Listen, {.Type = EStateTreeTaskType::Animate, .Key = Builder.Name("Alert")});
	Builder.Task(Listen, {.Type = EStateTreeTaskType::MoveTo, .Target = EStateTreeTarget::Noise, .Values = {0.f, 0.8f}});
	const std::uint16_t Calm = Builder.State("Calm", Investigate);
	Builder.Task(Calm, {.Type = EStateTreeTaskType::Forget});
	Builder.Transition(Listen, {.Trigger = EStateTreeTrigger::OnCompleted, .Target = Calm});
	const std::uint16_t Wander = Builder.State("Wander", Root);
	Builder.Task(Wander, {.Type = EStateTreeTaskType::Wander, .Values = {4.f, 0.f}});
	Builder.Transition(Root, {.Trigger = EStateTreeTrigger::OnEvent, .Target = Fight, .Event = Builder.Name("Spotted")});
	Builder.Transition(Root, {.Trigger = EStateTreeTrigger::OnEvent, .Target = Investigate, .Event = Builder.Name("Noise")});
	Builder.Name("Hit");
	return Builder.Finish();
}
}

TEST_CASE("AI agents perceive, remember, investigate, and chase through their StateTree on a schedule")
{
	FTestBody Body;
	FTestEnvironment Environment;
	FAiWorld World;
	const FAiAgentId Agent = World.AddAgent({.Name = "Guard", .Body = &Body, .Tree = MakeGuardTree(), .Perception = {.SightRange = 12.f, .MemorySeconds = 3.f}, .WalkSpeed = 1.5f, .RunSpeed = 4.f, .Seed = 7});
	const auto Run = [&](const float Seconds)
	{
		for (int Step = 0; Step < static_cast<int>(std::round(Seconds * 60.f)); ++Step)
		{
			World.Step(1.f / 60.f, Environment);
			Body.Advance(1.f / 60.f);
		}
	};

	// Alone, with nobody to watch for, the guard wanders around home at the slow rate.
	Run(1.f);
	CHECK(World.GetStateName(Agent) == "Wander");
	CHECK(World.GetStats().Thinks >= 1);
	CHECK(World.GetStats().Thinks <= 3);

	// A noise brings it to look, playing its alert first.
	World.PostNoise({3.f, 0.f, 3.f}, 20.f);
	Run(0.2f);
	CHECK(World.GetStateName(Agent) == "Listen");
	CHECK(std::ranges::contains(Body.Animations, std::string("Alert")));
	Run(4.f);
	CHECK(World.GetMemory(Agent)->SecondsSinceHeard > 3.f);
	CHECK(!World.GetMemory(Agent)->Noise);

	// The player in front, in range, and in sight: chase and swing.
	World.SetTarget(FAiTarget{.Foot = Body.Foot + FVector3{std::sin(Body.Yaw), 0.f, std::cos(Body.Yaw)} * 6.f, .Head = Body.Foot + FVector3{std::sin(Body.Yaw), 1.7f, std::cos(Body.Yaw)} * 6.f});
	Run(0.5f);
	CHECK(World.GetMemory(Agent)->bSeesTarget);
	CHECK(World.GetStateName(Agent) == "Chase");
	Run(2.5f);
	CHECK(Body.Attacks > 0);

	// Behind a wall the guard loses sight, searches where the player was, and gives up once it forgets.
	Environment.bWall = true;
	World.SetTarget(FAiTarget{.Foot = {9.f, 0.f, 0.f}, .Head = {9.f, 1.7f, 0.f}});
	Body.Foot = {0.f, 0.f, 0.f};
	Run(0.4f);
	CHECK(!World.GetMemory(Agent)->bSeesTarget);
	Run(6.f);
	CHECK(!World.GetMemory(Agent)->LastSeen);
	CHECK(World.GetStateName(Agent) == "Wander");
	World.Stop();
}

TEST_CASE("AI agents far from the target think less often, and the same seed makes the same choices")
{
	const auto Simulate = [](const std::uint64_t Seed, const FVector3& Home)
	{
		FTestBody Body;
		Body.Foot = Home;
		FTestEnvironment Environment;
		FAiWorld World;
		const FAiAgentId Agent = World.AddAgent({.Body = &Body, .Tree = MakeGuardTree(), .Home = Home, .Seed = Seed});
		World.SetTarget(FAiTarget{.Foot = {0.f, 0.f, -200.f}, .Head = {0.f, 1.7f, -200.f}});
		std::vector<FStateTreeTraceEvent> Trace;
		World.SetTrace(Agent, &Trace);
		for (int Step = 0; Step < 600; ++Step)
		{
			World.Step(1.f / 60.f, Environment);
			Body.Advance(1.f / 60.f);
		}

		World.SetTrace(Agent, nullptr);
		return std::tuple{World.GetStats().Thinks, Body.Foot, Trace};
	};

	const auto [FarThinks, FarFoot, FarTrace] = Simulate(3, {0.f, 0.f, 0.f});
	CHECK(FarThinks <= 21);
	const auto [Thinks, Foot, Trace] = Simulate(3, {0.f, 0.f, -190.f});
	CHECK(Thinks >= 90);
	const auto [AgainThinks, AgainFoot, AgainTrace] = Simulate(3, {0.f, 0.f, -190.f});
	CHECK(AgainFoot == Foot);
	CHECK(AgainTrace == Trace);
	const auto [OtherThinks, OtherFoot, OtherTrace] = Simulate(4, {0.f, 0.f, -190.f});
	// Both end at the player once they spot it, but wander there by different ways.
	CHECK_FALSE(OtherTrace == Trace);
}
}
