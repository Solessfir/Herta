#pragma once

#include <array>
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

enum class EAnimationBlendNodeType : std::uint8_t
{
	// Plays one clip.
	Clip,
	// Blends its children along ParameterX by their X positions, two at a time.
	Blend1D,
	// Blends its children by their distance from (ParameterX, ParameterY) to their (X, Y) points.
	Blend2D,
	// Adds the second child's motion away from its clip's first frame onto the first child, by Weight.
	Additive,
	// Replaces the first child with the second within MaskJoint and everything below it, by Weight.
	Layered,
};

struct FAnimationBlendChild
{
	bool operator==(const FAnimationBlendChild&) const = default;

	// Index of a later node in the same tree.
	std::size_t Node = 0;
	float X = 0.f;
	float Y = 0.f;
};

// One node of a state's blend tree. Additive and Layered weigh their second child by Weight, times ParameterX when it is set.
struct FAnimationBlendNodeDesc
{
	bool operator==(const FAnimationBlendNodeDesc&) const = default;

	EAnimationBlendNodeType Type = EAnimationBlendNodeType::Clip;
	std::string Clip{};
	std::optional<std::size_t> ParameterX{};
	std::optional<std::size_t> ParameterY{};
	float Weight = 1.f;
	std::string MaskJoint{};
	std::vector<FAnimationBlendChild> Children{};
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
	// Plays its clips backwards, such as a walk played as a backpedal.
	bool bReverse = false;
	// Moves only the upper body while the character moves: the legs keep playing the state before it, so a swing walks. Standing still, the state
	// plays on the whole body.
	bool bUpperBody = false;
	// A blend tree rooted at its first node, which plays instead of Samples when it is not empty.
	std::vector<FAnimationBlendNodeDesc> Tree{};
	// Plays one of the machine's SubMachines instead of clips; entering the state starts it from its entry.
	std::optional<std::size_t> SubMachine{};
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

enum class EAnimationPoseOperationType : std::uint8_t
{
	// Turns a joint and everything below it about a model-space axis by Scale radians, times Parameter when it is set.
	RotateJoint,
};

// Runs on the blended pose, in order, such as bending the spine toward where the camera looks.
struct FAnimationPoseOperationDesc
{
	bool operator==(const FAnimationPoseOperationDesc&) const = default;

	EAnimationPoseOperationType Type = EAnimationPoseOperationType::RotateJoint;
	std::string Joint{};
	std::array<float, 3> Axis{1.f, 0.f, 0.f};
	std::optional<std::size_t> Parameter{};
	float Scale = 1.f;
};

struct FAnimationStateMachineDesc
{
	bool operator==(const FAnimationStateMachineDesc&) const = default;

	std::vector<FAnimationParameterDesc> Parameters{};
	std::vector<FAnimationStateDesc> States{};
	// Tested in this order, so the order is the priority.
	std::vector<FAnimationTransitionDesc> Transitions{};
	std::size_t Entry = 0;
	// Machines that states play nested inside this one. They share this machine's parameters, so their own Parameters stay empty and their
	// parameter indices refer to this machine's list.
	std::vector<FAnimationStateMachineDesc> SubMachines{};
	// Only the outermost machine's operations run.
	std::vector<FAnimationPoseOperationDesc> PoseOperations{};
};
}
