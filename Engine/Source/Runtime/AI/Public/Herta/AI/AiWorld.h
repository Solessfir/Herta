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
	// Closer than this the agent senses the player whichever way it faces.
	float TouchRange = 1.5f;
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
};

struct FAiAgentId
{
	auto operator<=>(const FAiAgentId&) const = default;

	std::uint32_t Value = 0;
};

// What the agent knows. Seconds since a sighting or noise grow until memory forgets it.
struct FAiMemory
{
	bool bSeesTarget = false;
	// Which of the targets it sees, the nearest in view, while it sees one.
	std::size_t SeenTarget = 0;
	std::optional<FVector3> LastSeen{};
	float SecondsSinceSeen = 1.e9f;
	std::optional<FVector3> Noise{};
	float SecondsSinceHeard = 1.e9f;
	// Flag names, as indices into the agent's tree's names.
	std::vector<std::uint16_t> Flags{};
};

// The player, or whatever agents watch for.
struct FAiTarget
{
	FVector3 Foot{};
	FVector3 Head{};
};

// How often agents sense and think, in seconds. Agents spread their turns over the interval, and ones far from the target think less often.
struct FAiSchedule
{
	float ThinkSeconds = 0.1f;
	float PerceptionSeconds = 0.2f;
	float FarDistance = 40.f;
	float FarThinkSeconds = 0.5f;
};

struct FAiStats
{
	std::uint32_t Agents = 0;
	std::uint64_t Thinks = 0;
	std::uint64_t Perceptions = 0;
	std::uint64_t Spotted = 0;
	std::uint64_t Noises = 0;
};

// Agents that perceive a target, remember it, and act out StateTrees through their bodies. Everything advances in Step in agent order, so
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
	void SetTarget(const std::optional<FAiTarget>& Target);
	// Every player agents watch for; each agent follows the nearest one it sees.
	void SetTargets(std::span<const FAiTarget> InTargets);
	// Agents within both Radius and their hearing range hear it at their next step.
	void PostNoise(const FVector3& Position, float Radius);
	// Such as Hit; the agent's tree sees it the next time it thinks.
	void PostEvent(FAiAgentId Agent, std::string_view Event);
	// Something struck the agent from From: it remembers the attacker there, as if it had just seen it, and gets a Hit event.
	void ReportHit(FAiAgentId Agent, const FVector3& From);
	void Step(float Seconds, const IAiEnvironment& Environment);
	// Stops every agent's tasks.
	void Stop();

	[[nodiscard]] std::size_t GetAgentCount() const noexcept;
	[[nodiscard]] std::span<const std::uint16_t> GetActiveStates(FAiAgentId Agent) const;
	// The active leaf state's name, or empty.
	[[nodiscard]] std::string_view GetStateName(FAiAgentId Agent) const;
	[[nodiscard]] const FAiMemory* GetMemory(FAiAgentId Agent) const;
	[[nodiscard]] const FCookedStateTree* GetTree(FAiAgentId Agent) const;
	void SetTrace(FAiAgentId Agent, std::vector<FStateTreeTraceEvent>* Trace);
	[[nodiscard]] FAiStats GetStats() const;

private:
	struct FAgent;

	[[nodiscard]] FAgent* Find(FAiAgentId Agent);
	[[nodiscard]] const FAgent* Find(FAiAgentId Agent) const;
	void Perceive(FAgent& Agent, const IAiEnvironment& Environment);

	FAiSchedule Schedule{};
	FStateTreeInstancePool Pool{};
	std::vector<std::unique_ptr<FAgent>> Agents{};
	std::vector<FAiTarget> Targets{};
	std::vector<std::pair<FVector3, float>> Noises{};
	std::uint32_t NextId = 1;
	FAiStats Stats{};
};
}
