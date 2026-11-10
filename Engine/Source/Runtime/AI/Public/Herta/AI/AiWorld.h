#pragma once

#include "Herta/AI/StateTreeRuntime.h"
#include "Herta/Math/Vector.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EAiMoveStatus : std::uint8_t
{
	Idle,
	Moving,
	Arrived,
	// The destination cannot be reached, or the body has no navigation under it.
	Failed,
};

// How one faction regards another. Agents fight what they regard as hostile, help what they regard as friendly when it is attacked, and
// leave the rest alone unless it hurts them.
enum class EAiRelation : std::uint8_t
{
	Hostile,
	Neutral,
	Friendly,
};

// An agent's body in the game, which the game moves, animates, and fights with. Tasks act only through it.
class IAiBody
{
public:
	IAiBody() = default;
	virtual ~IAiBody() = default;

	IAiBody(const IAiBody&) = delete;
	IAiBody& operator=(const IAiBody&) = delete;
	IAiBody(IAiBody&&) = delete;
	IAiBody& operator=(IAiBody&&) = delete;

	[[nodiscard]] virtual FVector3 GetFootPosition() const = 0;
	[[nodiscard]] virtual FVector3 GetEyePosition() const = 0;
	// 0 faces +Z; positive turns toward +X.
	[[nodiscard]] virtual float GetYaw() const = 0;
	// From 0, dead, to 1, whole.
	[[nodiscard]] virtual float GetHealth() const = 0;
	virtual void MoveTo(const FVector3& Destination, float Speed) = 0;
	virtual void Stop() = 0;
	[[nodiscard]] virtual EAiMoveStatus GetMoveStatus() const = 0;
	virtual void Face(const FVector3& Point) = 0;
	virtual void Animate(std::string_view Trigger) = 0;
	// Starts a swing at Target; false while one is already under way.
	virtual bool StartAttack(const FVector3& Target) = 0;
	[[nodiscard]] virtual bool IsAttacking() const = 0;
};

// The world agents sense and walk, which the game answers from physics and navigation.
class IAiEnvironment
{
public:
	IAiEnvironment() = default;
	virtual ~IAiEnvironment() = default;

	IAiEnvironment(const IAiEnvironment&) = delete;
	IAiEnvironment& operator=(const IAiEnvironment&) = delete;
	IAiEnvironment(IAiEnvironment&&) = delete;
	IAiEnvironment& operator=(IAiEnvironment&&) = delete;

	[[nodiscard]] virtual bool HasLineOfSight(const FVector3& From, const FVector3& To) const = 0;
	// A point on the navigation within Radius of Center, drawn from Random, which it advances.
	[[nodiscard]] virtual std::optional<FVector3> FindReachablePoint(const FVector3& Center, float Radius, std::uint64_t& Random) const = 0;
};

struct FAiPerception
{
	float SightRange = 15.f;
	// Half the field of view about where the body faces.
	float SightHalfAngle = 1.0471976f;
	float HearingRange = 12.f;
	// How long a sighting or a noise is remembered after it stops.
	float MemorySeconds = 8.f;
	// Closer than this the agent senses a target whichever way it faces.
	float TouchRange = 1.5f;
};

struct FAiAgentId
{
	auto operator<=>(const FAiAgentId&) const = default;

	std::uint32_t Value = 0;
};

struct FAiAgentDesc
{
	std::string Name{};
	IAiBody* Body = nullptr;
	std::shared_ptr<const FCookedStateTree> Tree{};
	FAiPerception Perception{};
	float WalkSpeed = 1.5f;
	float RunSpeed = 4.f;
	FVector3 Home{};
	// Seeds the agent's random choices, so the same agent makes the same ones on every run.
	std::uint64_t Seed = 1;
	std::uint8_t Faction = 0;
	// The key its body has among the targets, which hits on it and grudges against it name.
	std::uint32_t Key = 0;
};

// Someone the agent holds a hit against, hostile to it until the grudge runs out.
struct FAiGrudge
{
	std::uint32_t Key = 0;
	float Seconds = 0.f;
};

// What the agent knows. Seconds since a sighting or noise grow until memory forgets it.
struct FAiMemory
{
	bool bSeesTarget = false;
	// The target key of the enemy it fights: the nearest hostile one in view while it sees one, else the one it last saw or that last hit it.
	std::optional<std::uint32_t> Enemy{};
	std::optional<FVector3> LastSeen{};
	float SecondsSinceSeen = 1.e9f;
	std::optional<FVector3> Noise{};
	float SecondsSinceHeard = 1.e9f;
	// Flag names, as indices into the agent's tree's names.
	std::vector<std::uint16_t> Flags{};
	std::vector<FAiGrudge> Grudges{};
};

// Something agents watch for: a player, or another agent's body.
struct FAiTarget
{
	FVector3 Foot{};
	FVector3 Head{};
	std::uint8_t Faction = 0;
	// The game's name for it, which agents report back as their enemy.
	std::uint32_t Key = 0;
	// Set when it is an agent's body, so the agent never watches itself.
	std::optional<FAiAgentId> Agent{};
};

// A blow the game saw land. The victim remembers its attacker, and agents friendly to the victim that hear it come to help.
struct FAiHit
{
	std::uint32_t Victim = 0;
	std::optional<FAiAgentId> VictimAgent{};
	FVector3 VictimFoot{};
	std::uint8_t VictimFaction = 0;
	// Empty when nobody struck it, such as after a fall.
	std::optional<std::uint32_t> Attacker{};
	FVector3 AttackerFoot{};
	std::uint8_t AttackerFaction = 0;
};

// How often agents sense and think, in seconds. Agents spread their turns over the interval, and ones far from every player think less often.
struct FAiSchedule
{
	float ThinkSeconds = 0.1f;
	float PerceptionSeconds = 0.2f;
	float FarDistance = 40.f;
	float FarThinkSeconds = 0.5f;
	// Agents surrounding one enemy take turns to attack it: this many at once, each for at most AttackTokenSeconds.
	std::size_t AttackersPerTarget = 2;
	float AttackTokenSeconds = 6.f;
	float GrudgeSeconds = 30.f;
};

struct FAiStats
{
	std::uint32_t Agents = 0;
	std::uint64_t Thinks = 0;
	std::uint64_t Perceptions = 0;
	std::uint64_t Spotted = 0;
	std::uint64_t Noises = 0;
	std::uint64_t AttackTokens = 0;
};

// Agents that perceive targets, remember them, and act out StateTrees through their bodies. Everything advances in Step in agent order, so
// the same calls give the same choices on every run.
class FAiWorld final
{
public:
	explicit FAiWorld(const FAiSchedule& InSchedule = {});
	~FAiWorld();

	FAiWorld(const FAiWorld&) = delete;
	FAiWorld& operator=(const FAiWorld&) = delete;
	FAiWorld(FAiWorld&&) = delete;
	FAiWorld& operator=(FAiWorld&&) = delete;

	[[nodiscard]] FAiAgentId AddAgent(FAiAgentDesc Desc);
	// Stops the agent's tasks and forgets it, such as when its character dies.
	void RemoveAgent(FAiAgentId Agent);
	// How each faction regards each other one: Table[From * FactionCount + To]. Without a table every target is hostile.
	void SetRelations(std::size_t FactionCount, std::span<const EAiRelation> Table);
	void SetTarget(const std::optional<FAiTarget>& Target);
	// Every player and agent body agents watch for; each agent fights the nearest hostile one it sees.
	void SetTargets(std::span<const FAiTarget> InTargets);
	// Agents within both Radius and their hearing range hear it at their next step.
	void PostNoise(const FVector3& Position, float Radius);
	// Such as Hit; the agent's tree sees it the next time it thinks.
	void PostEvent(FAiAgentId Agent, std::string_view Event);
	// Something struck the agent from From: it remembers the attacker there, as if it had just seen it, and gets a Hit event.
	void ReportHit(FAiAgentId Agent, const FVector3& From);
	// A blow landed on a player or an agent. The victim holds a grudge against its attacker, and agents friendly to the victim that hear it
	// turn on the attacker too, unless they regard it as friendly, and get an AllyHit event.
	void ReportAttack(const FAiHit& Hit);
	void Step(float Seconds, const IAiEnvironment& Environment);
	// Stops every agent's tasks.
	void Stop();

	[[nodiscard]] std::size_t GetAgentCount() const noexcept;
	[[nodiscard]] std::span<const std::uint16_t> GetActiveStates(FAiAgentId Agent) const;
	// The active leaf state's name, or empty.
	[[nodiscard]] std::string_view GetStateName(FAiAgentId Agent) const;
	[[nodiscard]] const FAiMemory* GetMemory(FAiAgentId Agent) const;
	[[nodiscard]] const FCookedStateTree* GetTree(FAiAgentId Agent) const;
	// Whether the agent holds one of its enemy's attack turns.
	[[nodiscard]] bool HasAttackToken(FAiAgentId Agent) const;
	// Where the agent stands in the ring around its enemy while it surrounds it.
	[[nodiscard]] std::optional<FVector3> GetSurroundSlot(FAiAgentId Agent) const;
	void SetTrace(FAiAgentId Agent, std::vector<FStateTreeTraceEvent>* Trace);
	[[nodiscard]] FAiStats GetStats() const;

private:
	struct FAgent;

	[[nodiscard]] FAgent* Find(FAiAgentId Agent);
	[[nodiscard]] const FAgent* Find(FAiAgentId Agent) const;
	[[nodiscard]] const FAiTarget* FindTarget(std::uint32_t Key) const;
	[[nodiscard]] EAiRelation GetRelation(std::uint8_t From, std::uint8_t To) const;
	[[nodiscard]] bool IsHostile(const FAgent& Agent, const FAiTarget& Target) const;
	void Perceive(FAgent& Agent, const IAiEnvironment& Environment);
	// Spreads each enemy's surrounding agents evenly around it and hands out its attack turns.
	void Engage(float Seconds);

	FAiSchedule Schedule{};
	FStateTreeInstancePool Pool{};
	std::vector<std::unique_ptr<FAgent>> Agents{};
	std::vector<FAiTarget> Targets{};
	std::vector<std::pair<FVector3, float>> Noises{};
	std::size_t FactionCount = 0;
	std::vector<EAiRelation> Relations{};
	std::uint32_t NextId = 1;
	FAiStats Stats{};
};
}
