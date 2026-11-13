#include "Herta/AI/AiWorld.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
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
// A surrounding agent walks to a slot that moved this far, and stands once within SlotAcceptance of it.
constexpr float SlotRetargetDistance = 0.5f;
constexpr float SlotAcceptance = 0.6f;
// Farther from its slot than this it runs there.
constexpr float SlotRunDistance = 4.f;
}

bool IAiBody::CanMove() const
{
	return true;
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
			case EStateTreeTarget::Enemy:
			{
				const FAiTarget* const Enemy = Memory.bSeesTarget && Memory.Enemy ? World.FindTarget(*Memory.Enemy) : nullptr;
				return Enemy ? std::optional{Enemy->Foot} : Memory.LastSeen;
			}
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
		const std::optional<FVector3> Point = Resolve(EStateTreeTarget::Enemy);
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
			case EStateTreeTaskType::Surround:
				if (!Memory.Enemy || !Resolve(EStateTreeTarget::Enemy) || !Body.CanMove())
				{
					return EStateTreeStatus::Failed;
				}

				bSurrounding = true;
				SurroundRadius = std::max(Task.Values[0], 1.f);
				WaitSeconds = 0.f;
				return EStateTreeStatus::Running;
			case EStateTreeTaskType::Forget:
				Memory.Enemy.reset();
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
		if (Task.Type == EStateTreeTaskType::MoveTo && Task.Target == EStateTreeTarget::Enemy)
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

				if (Body.IsAttacking())
				{
					return EStateTreeStatus::Running;
				}

				// The swing is over, so its turn passes to the agent that has waited longest around the enemy.
				Token.reset();
				return EStateTreeStatus::Succeeded;
			case EStateTreeTaskType::Surround:
			{
				// Finished tasks are not stopped, so the ring is left here.
				const EStateTreeStatus Status = TickSurround(Instance);
				if (Status != EStateTreeStatus::Running)
				{
					LeaveRing();
				}

				return Status;
			}
			default:
				return EStateTreeStatus::Succeeded;
		}
	}

	// Waits in its slot around the enemy, walking there as the ring moves, until it is handed an attack turn.
	EStateTreeStatus TickSurround(FStateTreeTaskInstance& Instance)
	{
		IAiBody& Body = *Desc.Body;
		const std::optional<FVector3> Enemy = Resolve(EStateTreeTarget::Enemy);
		if (!Memory.Enemy || !Enemy)
		{
			return EStateTreeStatus::Failed;
		}

		if (Token == Memory.Enemy)
		{
			return EStateTreeStatus::Succeeded;
		}

		if (!SurroundSlot)
		{
			return EStateTreeStatus::Running;
		}

		const float Distance = HorizontalDistance(Body.GetFootPosition(), *SurroundSlot);
		if (Distance <= SlotAcceptance)
		{
			if (Instance.Duration != 0.f)
			{
				Body.Stop();
				Instance.Duration = 0.f;
			}

			Body.Face(*Enemy);
			return EStateTreeStatus::Running;
		}

		// Duration marks a walk under way toward Point.
		if (Instance.Duration == 0.f || HorizontalDistance(Instance.Point, *SurroundSlot) > SlotRetargetDistance)
		{
			Instance.Point = *SurroundSlot;
			Instance.Duration = 1.f;
			Body.MoveTo(*SurroundSlot, Distance > SlotRunDistance ? Desc.RunSpeed : Desc.WalkSpeed);
		}

		return EStateTreeStatus::Running;
	}

	void StopTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance&) override
	{
		if (Task.Type == EStateTreeTaskType::MoveTo || Task.Type == EStateTreeTaskType::Wander || Task.Type == EStateTreeTaskType::Flee)
		{
			Desc.Body->Stop();
		}
		else if (Task.Type == EStateTreeTaskType::Surround)
		{
			LeaveRing();
		}
		else if (Task.Type == EStateTreeTaskType::Attack)
		{
			Token.reset();
		}
	}

	void LeaveRing()
	{
		bSurrounding = false;
		SurroundSlot.reset();
		Desc.Body->Stop();
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

	// While a Surround task runs: how far from the enemy it stands and the slot it was given there.
	bool bSurrounding = false;
	float SurroundRadius = 3.f;
	std::optional<FVector3> SurroundSlot{};
	// The enemy whose attack turn it holds, and for how long; how long it has waited in the ring for one.
	std::optional<std::uint32_t> Token{};
	float TokenSeconds = 0.f;
	float WaitSeconds = 0.f;
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

void FAiWorld::SetRelations(const std::size_t InFactionCount, const std::span<const EAiRelation> Table)
{
	FactionCount = Table.size() == InFactionCount * InFactionCount ? InFactionCount : 0;
	Relations.assign(Table.begin(), Table.begin() + static_cast<std::ptrdiff_t>(FactionCount * FactionCount));
}

EAiRelation FAiWorld::GetRelation(const std::uint8_t From, const std::uint8_t To) const
{
	return From < FactionCount && To < FactionCount ? Relations[From * FactionCount + To] : EAiRelation::Hostile;
}

bool FAiWorld::IsHostile(const FAgent& Agent, const FAiTarget& Target) const
{
	return GetRelation(Agent.Desc.Faction, Target.Faction) == EAiRelation::Hostile || std::ranges::contains(Agent.Memory.Grudges, Target.Key, &FAiGrudge::Key);
}

const FAiTarget* FAiWorld::FindTarget(const std::uint32_t Key) const
{
	const auto Found = std::ranges::find(Targets, Key, &FAiTarget::Key);
	return Found != Targets.end() ? &*Found : nullptr;
}

void FAiWorld::SetTarget(const std::optional<FAiTarget>& InTarget)
{
	Targets.clear();
	if (InTarget)
	{
		Targets.push_back(*InTarget);
	}
}

void FAiWorld::SetTargets(const std::span<const FAiTarget> InTargets)
{
	Targets.assign(InTargets.begin(), InTargets.end());
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

void FAiWorld::ReportAttack(const FAiHit& Hit)
{
	const auto HoldGrudge = [this, &Hit](FAgent& Agent)
	{
		if (!Hit.Attacker)
		{
			return;
		}

		const auto Held = std::ranges::find(Agent.Memory.Grudges, *Hit.Attacker, &FAiGrudge::Key);
		if (Held != Agent.Memory.Grudges.end())
		{
			Held->Seconds = Schedule.GrudgeSeconds;
		}
		else
		{
			Agent.Memory.Grudges.push_back({.Key = *Hit.Attacker, .Seconds = Schedule.GrudgeSeconds});
		}
	};

	for (const std::unique_ptr<FAgent>& Pointer : Agents)
	{
		FAgent& Agent = *Pointer;
		FAiMemory& Memory = Agent.Memory;
		if (Hit.VictimAgent && Agent.Id == *Hit.VictimAgent)
		{
			// Its own side's stray blows are forgiven; anyone else's are not.
			if (!Hit.Attacker || GetRelation(Agent.Desc.Faction, Hit.AttackerFaction) != EAiRelation::Friendly)
			{
				HoldGrudge(Agent);
				Memory.Enemy = Hit.Attacker ? Hit.Attacker : Memory.Enemy;
			}

			Memory.LastSeen = Hit.AttackerFoot;
			Memory.SecondsSinceSeen = 0.f;
			Agent.Execution.PostEvent("Hit");
			continue;
		}

		const bool bAlly = GetRelation(Agent.Desc.Faction, Hit.VictimFaction) == EAiRelation::Friendly;
		const bool bHeard = HorizontalDistance(Agent.Desc.Body->GetFootPosition(), Hit.VictimFoot) <= Agent.Desc.Perception.HearingRange;
		if (!bAlly || !bHeard || !Hit.Attacker || GetRelation(Agent.Desc.Faction, Hit.AttackerFaction) == EAiRelation::Friendly)
		{
			continue;
		}

		HoldGrudge(Agent);
		if (!Memory.bSeesTarget)
		{
			Memory.Enemy = Hit.Attacker;
			Memory.LastSeen = Hit.AttackerFoot;
			Memory.SecondsSinceSeen = 0.f;
		}

		Agent.Execution.PostEvent("AllyHit");
	}
}

void FAiWorld::Engage(const float Seconds)
{
	// Each enemy's ring, in key order so the turns go out the same way on every run.
	std::map<std::uint32_t, std::vector<FAgent*>> Rings;
	std::map<std::uint32_t, std::size_t> Holders;
	for (const std::unique_ptr<FAgent>& Pointer : Agents)
	{
		FAgent& Agent = *Pointer;
		if (Agent.Token)
		{
			++Holders[*Agent.Token];
		}

		if (Agent.bSurrounding && Agent.Memory.Enemy)
		{
			Rings[*Agent.Memory.Enemy].push_back(&Agent);
			Agent.WaitSeconds += Agent.Token ? 0.f : Seconds;
		}
	}

	for (auto& [Key, Ring] : Rings)
	{
		const FAiTarget* const Target = FindTarget(Key);
		if (!Target)
		{
			continue;
		}

		// The agents keep their order around the enemy and spread evenly, the ring turned to where they already stand, so none crosses another.
		const FVector3 Center = Target->Foot;
		const auto Bearing = [&Center](const FAgent* Agent)
		{
			const FVector3 Foot = Agent->Desc.Body->GetFootPosition();
			return std::atan2(Foot.X - Center.X, Foot.Z - Center.Z);
		};

		std::ranges::stable_sort(Ring, {}, Bearing);
		const float Spacing = 2.f * std::numbers::pi_v<float> / static_cast<float>(Ring.size());
		float SineSum = 0.f;
		float CosineSum = 0.f;
		for (std::size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const float Offset = Bearing(Ring[Index]) - Spacing * static_cast<float>(Index);
			SineSum += std::sin(Offset);
			CosineSum += std::cos(Offset);
		}

		const float Turn = std::atan2(SineSum, CosineSum);
		for (std::size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const float Angle = Turn + Spacing * static_cast<float>(Index);
			Ring[Index]->SurroundSlot = Center + FVector3{std::sin(Angle), 0.f, std::cos(Angle)} * Ring[Index]->SurroundRadius;
		}

		// Free turns go to the agents that have waited longest, so one that just swung goes to the back; the nearest breaks a tie.
		std::vector<FAgent*> Waiting;
		std::ranges::copy_if(Ring, std::back_inserter(Waiting), [](const FAgent* Agent)
		{
			return !Agent->Token;
		});

		std::ranges::stable_sort(Waiting, [&Center](const FAgent* First, const FAgent* Second)
		{
			if (First->WaitSeconds != Second->WaitSeconds)
			{
				return First->WaitSeconds > Second->WaitSeconds;
			}

			return HorizontalDistance(First->Desc.Body->GetFootPosition(), Center) < HorizontalDistance(Second->Desc.Body->GetFootPosition(), Center);
		});

		for (FAgent* const Agent : Waiting)
		{
			if (Holders[Key] >= Schedule.AttackersPerTarget)
			{
				break;
			}

			Agent->Token = Key;
			Agent->TokenSeconds = 0.f;
			++Holders[Key];
			++Stats.AttackTokens;
		}
	}
}

void FAiWorld::Perceive(FAgent& Agent, const IAiEnvironment& Environment)
{
	++Stats.Perceptions;
	const FAiPerception& Perception = Agent.Desc.Perception;
	const IAiBody& Body = *Agent.Desc.Body;
	const FAiTarget* Seen = nullptr;
	float Nearest = 1.e9f;
	const FVector3 Eye = Body.GetEyePosition();
	for (const FAiTarget& Target : Targets)
	{
		if ((Target.Agent && *Target.Agent == Agent.Id) || !IsHostile(Agent, Target))
		{
			continue;
		}

		const float Distance = HorizontalDistance(Body.GetFootPosition(), Target.Foot);
		const float Bearing = std::atan2(Target.Foot.X - Eye.X, Target.Foot.Z - Eye.Z);
		const bool bInView = Distance <= Perception.SightRange && std::abs(std::remainder(Bearing - Body.GetYaw(), 2.f * std::numbers::pi_v<float>)) <= Perception.SightHalfAngle;
		if (Distance < Nearest && (Distance <= Perception.TouchRange || bInView) && Environment.HasLineOfSight(Eye, Target.Head))
		{
			Nearest = Distance;
			Seen = &Target;
			Agent.Memory.Enemy = Target.Key;
		}
	}

	if (Seen)
	{
		if (!Agent.Memory.bSeesTarget)
		{
			Agent.Execution.PostEvent("Spotted");
			++Stats.Spotted;
		}

		Agent.Memory.LastSeen = Seen->Foot;
		Agent.Memory.SecondsSinceSeen = 0.f;
	}
	else if (Agent.Memory.bSeesTarget)
	{
		Agent.Execution.PostEvent("Lost");
	}

	Agent.Memory.bSeesTarget = Seen != nullptr;
}

void FAiWorld::Step(const float Seconds, const IAiEnvironment& Environment)
{
	const std::vector<std::pair<FVector3, float>> Heard = std::exchange(Noises, {});
	Engage(Seconds);
	for (const std::unique_ptr<FAgent>& Pointer : Agents)
	{
		FAgent& Agent = *Pointer;
		Agent.Environment = &Environment;
		FAiMemory& Memory = Agent.Memory;
		Memory.SecondsSinceSeen += Seconds;
		Memory.SecondsSinceHeard += Seconds;
		for (FAiGrudge& Grudge : Memory.Grudges)
		{
			Grudge.Seconds -= Seconds;
		}

		std::erase_if(Memory.Grudges, [](const FAiGrudge& Grudge)
		{
			return Grudge.Seconds <= 0.f;
		});

		// A turn not used in time, such as by an agent that lost its way, passes on.
		Agent.TokenSeconds += Seconds;
		if (Agent.Token && (Agent.TokenSeconds > Schedule.AttackTokenSeconds || Agent.Token != Memory.Enemy))
		{
			Agent.Token.reset();
		}

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
			Memory.Enemy.reset();
		}

		if (Memory.SecondsSinceHeard > Agent.Desc.Perception.MemorySeconds)
		{
			Memory.Noise.reset();
		}

		// Other agents' bodies do not count: an agent thinks often only near a player.
		const bool bFar = std::ranges::none_of(Targets, [&](const FAiTarget& Target)
		{
			return !Target.Agent && HorizontalDistance(Agent.Desc.Body->GetFootPosition(), Target.Foot) <= Schedule.FarDistance;
		});

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

bool FAiWorld::HasAttackToken(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	return Found && Found->Token.has_value();
}

std::optional<FVector3> FAiWorld::GetSurroundSlot(const FAiAgentId Agent) const
{
	const FAgent* const Found = Find(Agent);
	return Found ? Found->SurroundSlot : std::nullopt;
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
