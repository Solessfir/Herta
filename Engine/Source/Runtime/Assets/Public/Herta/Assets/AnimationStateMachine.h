#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Herta
{
// Compiled animation state machine data, which cooked models carry and the Animation module runs. The Animation graph compiles to it, and
// code can build it directly.

enum class EAnimationParameterType : std::uint8_t
{
	Float,
	Bool,
	// Set until the next update, which consumes it whether or not a transition tested it.
	Trigger,
};

struct FAnimationParameterDesc
{
	bool operator==(const FAnimationParameterDesc&) const = default;

	std::string Name{};
	EAnimationParameterType Type = EAnimationParameterType::Float;
	float Default = 0.f;
};

// One point of a state's 1D blend: the clip plays alone at Position and fades towards its neighbours between points.
struct FAnimationBlendSample
{
	bool operator==(const FAnimationBlendSample&) const = default;

	std::string Clip{};
	float Position = 0.f;
};

// Fires once each time the state's playback reaches Phase, from 0 at its start to 1 at its end, such as a footstep.
struct FAnimationEventDesc
{
	bool operator==(const FAnimationEventDesc&) const = default;

	std::string Name{};
	float Phase = 0.f;
};

struct FAnimationStateDesc
{
	bool operator==(const FAnimationStateDesc&) const = default;

	std::string Name{};
	// One sample plays a single clip. Several are a 1D blend along BlendParameter, sorted by position.
	std::vector<FAnimationBlendSample> Samples{};
	std::optional<std::size_t> BlendParameter{};
	bool bLoop = true;
	float Speed = 1.f;
	std::vector<FAnimationEventDesc> Events{};
};

enum class EAnimationCondition : std::uint8_t
{
	Greater,
	Less,
	IsTrue,
	IsFalse,
	Triggered,
};

struct FAnimationConditionDesc
{
	bool operator==(const FAnimationConditionDesc&) const = default;

	std::size_t Parameter = 0;
	EAnimationCondition Test = EAnimationCondition::IsTrue;
	float Value = 0.f;
};

inline constexpr std::size_t AnyAnimationState = std::numeric_limits<std::size_t>::max();

// Fires when every condition holds, and when bAtEnd also once a non-looping From state has played to its end.
struct FAnimationTransitionDesc
{
	bool operator==(const FAnimationTransitionDesc&) const = default;

	std::size_t From = AnyAnimationState;
	std::size_t To = 0;
	float DurationSeconds = 0.2f;
	std::vector<FAnimationConditionDesc> Conditions{};
	bool bAtEnd = false;
};

struct FAnimationStateMachineDesc
{
	bool operator==(const FAnimationStateMachineDesc&) const = default;

	std::vector<FAnimationParameterDesc> Parameters{};
	std::vector<FAnimationStateDesc> States{};
	// Tested in this order, so the order is the priority.
	std::vector<FAnimationTransitionDesc> Transitions{};
	std::size_t Entry = 0;
};
}
