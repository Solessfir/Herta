#include "Herta/AI/AiWorld.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
// SplitMix64: small, fast, and the same on every platform.
[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& State)
{
	State += 0x9e3779b97f4a7c15ull;
	std::uint64_t Value = State;
	Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
	Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
	return Value ^ (Value >> 31);
}

[[nodiscard]] float NextUnit(std::uint64_t& State)
{
	return static_cast<float>(NextRandom(State) >> 40) / static_cast<float>(1ull << 24);
}

[[nodiscard]] float HorizontalDistance(const FVector3& First, const FVector3& Second)
{
	return std::hypot(First.X - Second.X, First.Z - Second.Z);
}

// A moving target is chased again once it has moved this far from where the agent was sent.
constexpr float RetargetDistance = 0.75f;
constexpr float DefaultAcceptance = 0.6f;
constexpr float FacingTolerance = 0.2f;
}

struct FAiWorld::FAgent final : IStateTreeContext
{
	FAgent(const FAiAgentId InId, FAiAgentDesc InDesc, FStateTreeInstancePool& Pool, FAiWorld& InWorld)
	    : Id(InId)
	    , Desc(std::move(InDesc))
	    , Execution(Desc.Tree, Pool)
	    , World(InWorld)
	    , Random(Desc.Seed)
	{
	}

	[[nodiscard]] std::optional<FVector3> Resolve(const EStateTreeTarget Target) const
	{
		switch (Target)
		{
			case EStateTreeTarget::Player:
				return Memory.bSeesTarget && World.Target ? std::optional{World.Target->Foot} : Memory.LastSeen;
			case EStateTreeTarget::LastSeen:
				return Memory.LastSeen;
			case EStateTreeTarget::Noise:
				return Memory.Noise;
			case EStateTreeTarget::Home:
				return Desc.Home;
		}

		return std::nullopt;
	}

	[[nodiscard]] float TargetDistance() const
	{
		const std::optional<FVector3> Point = Resolve(EStateTreeTarget::Player);
		return Point ? HorizontalDistance(Desc.Body->GetFootPosition(), *Point) : 1.e9f;
	}

	EStateTreeStatus StartMove(FStateTreeTaskInstance& Instance, const FVector3& Destination, const float Speed)
	{
		Instance.Point = Destination;
		Desc.Body->MoveTo(Destination, Speed);
		return EStateTreeStatus::Running;
	}

	EStateTreeStatus StartTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance) override
	{
		IAiBody& Body = *Desc.Body;
		const float Speed = Task.Values[0] >= 0.5f ? Desc.RunSpeed : Desc.WalkSpeed;
		switch (Task.Type)
		{
			case EStateTreeTaskType::Wait:
				Instance.Duration = Task.Values[0] + (std::max(Task.Values[1], Task.Values[0]) - Task.Values[0]) * NextUnit(Random);
				return EStateTreeStatus::Running;
			case EStateTreeTaskType::MoveTo:
			{
				const std::optional<FVector3> Destination = Resolve(Task.Target);
				return Destination ? StartMove(Instance, *Destination, Speed) : EStateTreeStatus::Failed;
			}
			case EStateTreeTaskType::Wander:
			{
				const std::optional<FVector3> Destination = Environment ? Environment->FindReachablePoint(Desc.Home, std::max(Task.Values[0], 0.5f), Random) : std::nullopt;
				return Destination ? StartMove(Instance, *Destination, Desc.WalkSpeed) : EStateTreeStatus::Failed;
			}
			case EStateTreeTaskType::Flee:
			{
				const std::optional<FVector3> Threat = Resolve(Task.Target);
				if (!Threat || !Environment)
				{
					return EStateTreeStatus::Failed;
				}

				const FVector3 Foot = Body.GetFootPosition();
				FVector3 Away{Foot.X - Threat->X, 0.f, Foot.Z - Threat->Z};
				Away = Away.LengthSquared() > 1.e-4f ? Away.Normalized() : FVector3{std::sin(Body.GetYaw()), 0.f, std::cos(Body.GetYaw())} * -1.f;
				const std::optional<FVector3> Destination = Environment->FindReachablePoint(Foot + Away * std::max(Task.Values[0], 2.f), 2.f, Random);
				return Destination ? StartMove(Instance, *Destination, Desc.RunSpeed) : EStateTreeStatus::Failed;
			}
			case EStateTreeTaskType::Face:
			{
				const std::optional<FVector3> Point = Resolve(Task.Target);
				if (!Point)
				{
					return EStateTreeStatus::Failed;
				}

				Instance.Point = *Point;
				Body.Face(*Point);
				return EStateTreeStatus::Running;
			}
			case EStateTreeTaskType::Attack:
			{
				const std::optional<FVector3> Point = Resolve(Task.Target);
				if (!Point || HorizontalDistance(Body.GetFootPosition(), *Point) > Task.Values[0])
				{
					return EStateTreeStatus::Failed;
				}

				Instance.Point = *Point;
				Body.Face(*Point);
				// Duration marks that the swing has started, so a busy body is asked again next tick.
				Instance.Duration = Body.StartAttack(*Point) ? 1.f : 0.f;
				return EStateTreeStatus::Running;
			}
			case EStateTreeTaskType::Animate:
				if (Task.Key != StateTreeNone)
				{
					Body.Animate(Desc.Tree->Names[Task.Key]);
				}

				return EStateTreeStatus::Succeeded;
			case EStateTreeTaskType::SetFlag:
				std::erase(Memory.Flags, Task.Key);
				if (Task.Values[0] >= 0.5f && Task.Key != StateTreeNone)
				{
					Memory.Flags.push_back(Task.Key);
				}

				return EStateTreeStatus::Succeeded;
			case EStateTreeTaskType::Forget:
				Memory.LastSeen.reset();
				Memory.Noise.reset();
				Memory.SecondsSinceSeen = 1.e9f;
				Memory.SecondsSinceHeard = 1.e9f;
				return EStateTreeStatus::Succeeded;
		}

		return EStateTreeStatus::Failed;
	}

	EStateTreeStatus TickMove(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance)
	{
		IAiBody& Body = *Desc.Body;
		const float Acceptance = Task.Type == EStateTreeTaskType::MoveTo && Task.Values[1] > 0.f ? Task.Values[1] : DefaultAcceptance;
		if (Task.Type == EStateTreeTaskType::MoveTo && Task.Target == EStateTreeTarget::Player)
		{
			const std::optional<FVector3> Destination = Resolve(Task.Target);
			if (!Destination)
			{
				return EStateTreeStatus::Failed;
			}

			if (HorizontalDistance(*Destination, Instance.Point) > RetargetDistance)
			{
				Instance.Point = *Destination;
				Body.MoveTo(*Destination, Task.Values[0] >= 0.5f ? Desc.RunSpeed : Desc.WalkSpeed);
			}
		}

		if (HorizontalDistance(Body.GetFootPosition(), Instance.Point) <= Acceptance)
		{
			Body.Stop();
			return EStateTreeStatus::Succeeded;
		}

		// Arriving short of the point means the rest of the way is blocked.
		const EAiMoveStatus Status = Body.GetMoveStatus();
		return Status == EAiMoveStatus::Failed || Status == EAiMoveStatus::Arrived ? EStateTreeStatus::Failed : EStateTreeStatus::Running;
	}

	EStateTreeStatus TickTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance, const float Seconds) override
	{
		Instance.Seconds += Seconds;
		IAiBody& Body = *Desc.Body;
		switch (Task.Type)
		{
			case EStateTreeTaskType::Wait:
				return Instance.Seconds >= Instance.Duration ? EStateTreeStatus::Succeeded : EStateTreeStatus::Running;
			case EStateTreeTaskType::MoveTo:
			case EStateTreeTaskType::Wander:
			case EStateTreeTaskType::Flee:
				return TickMove(Task, Instance);
			case EStateTreeTaskType::Face:
			{
				const FVector3 Foot = Body.GetFootPosition();
				const float Wanted = std::atan2(Instance.Point.X - Foot.X, Instance.Point.Z - Foot.Z);
				Body.Face(Instance.Point);
				return std::abs(std::remainder(Wanted - Body.GetYaw(), 2.f * std::numbers::pi_v<float>)) < FacingTolerance ? EStateTreeStatus::Succeeded : EStateTreeStatus::Running;
			}
			case EStateTreeTaskType::Attack:
				if (Instance.Duration == 0.f)
				{
					Instance.Duration = Body.StartAttack(Instance.Point) ? 1.f : 0.f;
					return EStateTreeStatus::Running;
				}

				return Body.IsAttacking() ? EStateTreeStatus::Running : EStateTreeStatus::Succeeded;
			default:
				return EStateTreeStatus::Succeeded;
		}
	}

	void StopTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance&) override
	{
		if (Task.Type == EStateTreeTaskType::MoveTo || Task.Type == EStateTreeTaskType::Wander || Task.Type == EStateTreeTaskType::Flee)
		{
			Desc.Body->Stop();
		}
	}

	bool TestCondition(const FStateTreeConditionDesc& Condition) override
	{
		const auto Compare = [&Condition](const float Value)
		{
			return Condition.Compare == EStateTreeCompare::Less ? Value < Condition.Value : Value > Condition.Value;
		};

		switch (Condition.Type)
		{
			case EStateTreeConditionType::SeesTarget:
				return Memory.bSeesTarget;
			case EStateTreeConditionType::RemembersTarget:
				return Memory.LastSeen.has_value();
			case EStateTreeConditionType::HeardNoise:
				return Memory.Noise.has_value();
			case EStateTreeConditionType::TargetDistance:
				return Compare(TargetDistance());
			case EStateTreeConditionType::Health:
				return Compare(Desc.Body->GetHealth());
			case EStateTreeConditionType::Random:
				return NextUnit(Random) < Condition.Value;
			case EStateTreeConditionType::Flag:
				return std::ranges::contains(Memory.Flags, Condition.Key);
			case EStateTreeConditionType::TimeInState:
				return false;
		}

		return false;
	}

	float ReadInput(const EStateTreeInput Input) override
	{
		switch (Input)
		{
			case EStateTreeInput::TargetDistance:
				return TargetDistance();
			case EStateTreeInput::Health:
				return Desc.Body->GetHealth();
			case EStateTreeInput::SecondsSinceSeen:
				return Memory.SecondsSinceSeen;
			case EStateTreeInput::SecondsSinceHeard:
				return Memory.SecondsSinceHeard;
		}

		return 0.f;
	}

	FAiAgentId Id{};
	FAiAgentDesc Desc{};
	FStateTreeExecution Execution;
	FAiWorld& World;
	FAiMemory Memory{};
	std::uint64_t Random = 1;
	// Seconds gathered toward the next think and the next look around, started at a share of the interval so agents take turns.
	float ThinkClock = 0.f;
	float PerceptionClock = 0.f;
	const IAiEnvironment* Environment = nullptr;
};

FAiWorld::FAiWorld(const FAiSchedule& InSchedule)
    : Schedule(InSchedule)
{
}

FAiWorld::~FAiWorld() = default;

FAiAgentId FAiWorld::AddAgent(FAiAgentDesc Desc)
{
	const FAiAgentId Id{NextId++};
	auto Agent = std::make_unique<FAgent>(Id, std::move(Desc), Pool, *this);
	// Spreading first turns over the interval keeps every agent from thinking in the same step.
	const float Share = static_cast<float>(Agents.size() % 8) / 8.f;
	Agent->ThinkClock = Schedule.ThinkSeconds * Share;
	Agent->PerceptionClock = Schedule.PerceptionSeconds * Share;
	Agents.push_back(std::move(Agent));
	return Id;
}

void FAiWorld::RemoveAgent(const FAiAgentId Agent)
{
	const auto Found = std::ranges::find_if(Agents, [Agent](const std::unique_ptr<FAgent>& Candidate)
	{
		return Candidate->Id == Agent;
	});

	if (Found != Agents.end())
	{
		(*Found)->Execution.Stop(**Found);
		Agents.erase(Found);
	}
}

void FAiWorld::SetTarget(const std::optional<FAiTarget>& InTarget)
{
	Target = InTarget;
}

void FAiWorld::PostNoise(const FVector3& Position, const float Radius)
{
	Noises.emplace_back(Position, Radius);
}

void FAiWorld::PostEvent(const FAiAgentId Agent, const std::string_view Event)
{
	if (FAgent* const Found = Find(Agent))
	{
		Found->Execution.PostEvent(Event);
	}
}

void FAiWorld::ReportHit(const FAiAgentId Agent, const FVector3& From)
{
	if (FAgent* const Found = Find(Agent))
	{
		Found->Memory.LastSeen = From;
		Found->Memory.SecondsSinceSeen = 0.f;
		Found->Execution.PostEvent("Hit");
	}
}

void FAiWorld::Perceive(FAgent& Agent, const IAiEnvironment& Environment)
{
	++Stats.Perceptions;
	const FAiPerception& Perception = Agent.Desc.Perception;
	const IAiBody& Body = *Agent.Desc.Body;
	bool bSees = false;
	if (Target)
	{
		const FVector3 Eye = Body.GetEyePosition();
		const float Distance = HorizontalDistance(Body.GetFootPosition(), Target->Foot);
		const float Bearing = std::atan2(Target->Foot.X - Eye.X, Target->Foot.Z - Eye.Z);
		const bool bInView = Distance <= Perception.SightRange && std::abs(std::remainder(Bearing - Body.GetYaw(), 2.f * std::numbers::pi_v<float>)) <= Perception.SightHalfAngle;
		bSees = (Distance <= Perception.TouchRange || bInView) && Environment.HasLineOfSight(Eye, Target->Head);
	}

	if (bSees)
	{
		if (!Agent.Memory.bSeesTarget)
		{
			Agent.Execution.PostEvent("Spotted");
			++Stats.Spotted;
		}

		Agent.Memory.LastSeen = Target->Foot;
		Agent.Memory.SecondsSinceSeen = 0.f;
	}
	else if (Agent.Memory.bSeesTarget)
	{
		Agent.Execution.PostEvent("Lost");
	}

	Agent.Memory.bSeesTarget = bSees;
}

void FAiWorld::Step(const float Seconds, const IAiEnvironment& Environment)
{
	const std::vector<std::pair<FVector3, float>> Heard = std::exchange(Noises, {});
	for (const std::unique_ptr<FAgent>& Pointer : Agents)
	{
		FAgent& Agent = *Pointer;
		Agent.Environment = &Environment;
		FAiMemory& Memory = Agent.Memory;
		Memory.SecondsSinceSeen += Seconds;
		Memory.SecondsSinceHeard += Seconds;
		for (const auto& [Position, Radius] : Heard)
		{
			if ((Position - Agent.Desc.Body->GetFootPosition()).Length() <= std::min(Radius, Agent.Desc.Perception.HearingRange))
			{
				Memory.Noise = Position;
				Memory.SecondsSinceHeard = 0.f;
				Agent.Execution.PostEvent("Noise");
				++Stats.Noises;
			}
		}

		Agent.PerceptionClock += Seconds;
		if (Agent.PerceptionClock >= Schedule.PerceptionSeconds)
		{
			Agent.PerceptionClock -= Schedule.PerceptionSeconds;
			Perceive(Agent, Environment);
		}

		if (Memory.SecondsSinceSeen > Agent.Desc.Perception.MemorySeconds)
		{
			Memory.LastSeen.reset();
		}

		if (Memory.SecondsSinceHeard > Agent.Desc.Perception.MemorySeconds)
		{
			Memory.Noise.reset();
		}

		const bool bFar = !Target || HorizontalDistance(Agent.Desc.Body->GetFootPosition(), Target->Foot) > Schedule.FarDistance;
		const float Interval = bFar ? Schedule.FarThinkSeconds : Schedule.ThinkSeconds;
		// An event, such as a noise or a sighting, wakes the agent on this step whatever its schedule.
		Agent.ThinkClock += Seconds;
		if (Agent.ThinkClock >= Interval || Agent.Execution.HasPendingEvents())
		{
			const float Elapsed = Agent.ThinkClock;
			Agent.ThinkClock = 0.f;
			Agent.Execution.Tick(Elapsed, Agent);
			++Stats.Thinks;
		}
	}
}

void FAiWorld::Stop()
{
	for (const std::unique_ptr<FAgent>& Agent : Agents)
	{
		Agent->Execution.Stop(*Agent);
	}
}

FAiWorld::FAgent* FAiWorld::Find(const FAiAgentId Agent)
{
	const auto Found = std::ranges::find_if(Agents, [Agent](const std::unique_ptr<FAgent>& Candidate)
	{
		return Candidate->Id == Agent;
	});

	return Found != Agents.end() ? Found->get() : nullptr;
}

const FAiWorld::FAgent* FAiWorld::Find(const FAiAgentId Agent) const
{
	const auto Found = std::ranges::find_if(Agents, [Agent](const std::unique_ptr<FAgent>& Candidate)
	{
		return Candidate->Id == Agent;
	});

	return Found != Agents.end() ? Found->get() : nullptr;
}

std::size_t FAiWorld::GetAgentCount() const noexcept
{
	return Agents.size();
}

std::span<const std::uint16_t> FAiWorld::GetActiveStates(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	return Found ? Found->Execution.GetActiveStates() : std::span<const std::uint16_t>{};
}

std::string_view FAiWorld::GetStateName(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	if (!Found || Found->Execution.GetActiveStates().empty())
	{
		return {};
	}

	return Found->Desc.Tree->States[Found->Execution.GetActiveStates().back()].Name;
}

const FAiMemory* FAiWorld::GetMemory(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	return Found ? &Found->Memory : nullptr;
}

const FCookedStateTree* FAiWorld::GetTree(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	return Found ? Found->Desc.Tree.get() : nullptr;
}

void FAiWorld::SetTrace(const FAiAgentId Agent, std::vector<FStateTreeTraceEvent>* const Trace)
{
	if (FAgent* const Found = Find(Agent))
	{
		Found->Execution.SetTrace(Trace);
	}
}

FAiStats FAiWorld::GetStats() const
{
	FAiStats Result = Stats;
	Result.Agents = static_cast<std::uint32_t>(Agents.size());
	return Result;
}
}
