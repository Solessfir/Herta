#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Herta
{
inline constexpr std::uint16_t StateTreeNone = 0xffff;
inline constexpr std::size_t MaximumStateTreeStates = 1024;
inline constexpr std::size_t MaximumStateTreeDepth = 16;
inline constexpr std::size_t MaximumStateTreeItems = 8192;
inline constexpr std::size_t MaximumStateTreeNames = 1024;

// How a state with children picks the one to enter.
enum class EStateTreeSelection : std::uint8_t
{
	// The first child, in authored order, whose enter conditions hold and that can itself enter a leaf.
	FirstEligible,
	// The eligible child with the highest utility score; ties go to the earlier child.
	Utility,
};

// Built-in gameplay tasks, carried out through the agent's body.
enum class EStateTreeTaskType : std::uint8_t
{
	// Waits between Values[0] and Values[1] seconds, picked at random on entering.
	Wait,
	// Walks, or runs when Values[0] is at least 0.5, to the target, following a moving one, until within Values[1] meters.
	MoveTo,
	// Walks to a random reachable point within Values[0] meters of home.
	Wander,
	// Runs at least Values[0] meters away from the target.
	Flee,
	// Turns to face the target.
	Face,
	// Swings at the target when it is within Values[0] meters; fails when it is not.
	Attack,
	// Fires the animation trigger named by Key and succeeds.
	Animate,
	// Sets the flag named by Key when Values[0] is at least 0.5, or clears it, and succeeds.
	SetFlag,
	// Forgets the target and the last noise, and succeeds.
	Forget,
	// Stands in a ring Values[0] meters around the enemy with the others surrounding it, walking as the ring turns and the enemy moves,
	// and succeeds once handed one of the enemy's attack turns; fails without an enemy.
	Surround,
};

enum class EStateTreeTarget : std::uint8_t
{
	// The enemy the agent fights: the nearest hostile player or agent it sees, else where it last saw one.
	Enemy,
	// Where the agent last saw its enemy.
	LastSeen,
	// Where the agent last heard a noise.
	Noise,
	// Where the agent started.
	Home,
};

enum class EStateTreeConditionType : std::uint8_t
{
	// The player is in view now.
	SeesTarget,
	// The player was seen within the agent's memory.
	RemembersTarget,
	// A noise was heard within the agent's memory.
	HeardNoise,
	// Meters to the player, or to where it was last seen when out of view, compared with Value.
	TargetDistance,
	// Health from 0 to 1 compared with Value.
	Health,
	// Holds with the chance Value each time it is checked.
	Random,
	// Seconds since the state holding the condition, or the transition's state, was entered, compared with Value.
	TimeInState,
	// The flag named by Key is set.
	Flag,
};

enum class EStateTreeCompare : std::uint8_t
{
	Less,
	Greater,
};

enum class EStateTreeTrigger : std::uint8_t
{
	// The state's tasks finished, whether they succeeded or failed.
	OnCompleted,
	OnSucceeded,
	OnFailed,
	// Checked every time the agent thinks.
	OnTick,
	// The event named by Event arrived since the agent last thought, such as Hit, Noise, or Spotted.
	OnEvent,
};

enum class EStateTreeTransitionKind : std::uint8_t
{
	GotoState,
	// Finishes the parent state as if its tasks had succeeded or failed, so the parent's own transitions decide what comes next.
	Succeed,
	Fail,
};

enum class EStateTreeInput : std::uint8_t
{
	TargetDistance,
	Health,
	SecondsSinceSeen,
	SecondsSinceHeard,
};

struct FStateTreeTaskDesc
{
	bool operator==(const FStateTreeTaskDesc&) const = default;

	EStateTreeTaskType Type = EStateTreeTaskType::Wait;
	EStateTreeTarget Target = EStateTreeTarget::Enemy;
	std::array<float, 2> Values{};
	std::uint16_t Key = StateTreeNone;
};

struct FStateTreeConditionDesc
{
	bool operator==(const FStateTreeConditionDesc&) const = default;

	EStateTreeConditionType Type = EStateTreeConditionType::SeesTarget;
	EStateTreeCompare Compare = EStateTreeCompare::Less;
	bool bInvert = false;
	float Value = 0.f;
	std::uint16_t Key = StateTreeNone;
};

struct FStateTreeTransitionDesc
{
	bool operator==(const FStateTreeTransitionDesc&) const = default;

	EStateTreeTrigger Trigger = EStateTreeTrigger::OnCompleted;
	EStateTreeTransitionKind Kind = EStateTreeTransitionKind::GotoState;
	std::uint16_t Target = StateTreeNone;
	std::uint16_t Event = StateTreeNone;
	std::uint16_t FirstCondition = 0;
	std::uint16_t ConditionCount = 0;
};

// Adds Weight times the input mapped from Minimum..Maximum to 0..1, or 1..0 when inverted, to a utility state's score.
struct FStateTreeConsiderationDesc
{
	bool operator==(const FStateTreeConsiderationDesc&) const = default;

	EStateTreeInput Input = EStateTreeInput::TargetDistance;
	float Minimum = 0.f;
	float Maximum = 1.f;
	float Weight = 1.f;
	bool bInvert = false;
};

// States are stored depth first from the root at index 0, so a state's first child follows it and NextSibling jumps past its subtree.
// Every range indexes the program's flat arrays.
struct FStateTreeStateDesc
{
	bool operator==(const FStateTreeStateDesc&) const = default;

	std::string Name{};
	// The authoring graph's node, for diagnostics and for keeping a running tree in the same state across a reload.
	std::uint32_t Node = 0;
	std::uint16_t Parent = StateTreeNone;
	std::uint16_t FirstChild = StateTreeNone;
	std::uint16_t NextSibling = StateTreeNone;
	std::uint16_t Depth = 0;
	std::uint16_t FirstTask = 0;
	std::uint16_t TaskCount = 0;
	std::uint16_t FirstCondition = 0;
	std::uint16_t ConditionCount = 0;
	std::uint16_t FirstTransition = 0;
	std::uint16_t TransitionCount = 0;
	std::uint16_t FirstConsideration = 0;
	std::uint16_t ConsiderationCount = 0;
	EStateTreeSelection Selection = EStateTreeSelection::FirstEligible;
	float Score = 0.f;
};

// A compiled StateTree, shared by every agent that runs it.
struct FCookedStateTree
{
	bool operator==(const FCookedStateTree&) const = default;

	std::vector<FStateTreeStateDesc> States{};
	std::vector<FStateTreeTaskDesc> Tasks{};
	std::vector<FStateTreeConditionDesc> Conditions{};
	std::vector<FStateTreeTransitionDesc> Transitions{};
	std::vector<FStateTreeConsiderationDesc> Considerations{};
	// Flag, trigger, and event names that Key and Event index.
	std::vector<std::string> Names{};
	// The most tasks active at once on any path from the root, which sizes each agent's pooled task data.
	std::uint16_t MaximumActiveTasks = 0;
};

// Checks every index and range, the depth-first layout, and that MaximumActiveTasks covers every path.
[[nodiscard]] std::expected<void, FAssetError> ValidateStateTree(const FCookedStateTree& Tree);
}
