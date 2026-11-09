#pragma once

#include "Herta/Animation/Animation.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EAnimationParameterType : std::uint8_t
{
	Float,
	Bool,
	// Set until the next update, which consumes it whether or not a transition tested it.
	Trigger,
};

struct FAnimationParameterDesc
{
	std::string Name;
	EAnimationParameterType Type = EAnimationParameterType::Float;
	float Default = 0.f;
};

// One point of a state's 1D blend: the clip plays alone at Position and fades towards its neighbours between points.
struct FAnimationBlendSample
{
	std::string Clip;
	float Position = 0.f;
};

struct FAnimationStateDesc
{
	std::string Name;
	// One sample plays a single clip. Several are a 1D blend along BlendParameter, sorted by position.
	std::vector<FAnimationBlendSample> Samples;
	std::optional<std::size_t> BlendParameter;
	bool bLoop = true;
	float Speed = 1.f;
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
	std::size_t Parameter = 0;
	EAnimationCondition Test = EAnimationCondition::IsTrue;
	float Value = 0.f;
};

inline constexpr std::size_t AnyAnimationState = std::numeric_limits<std::size_t>::max();

// Fires when every condition holds, and when bAtEnd also once a non-looping From state has played to its end.
struct FAnimationTransitionDesc
{
	std::size_t From = AnyAnimationState;
	std::size_t To = 0;
	float DurationSeconds = 0.2f;
	std::vector<FAnimationConditionDesc> Conditions;
	bool bAtEnd = false;
};

// Compiled state machine data, indexed by position. The Animation graph compiles to this; code can also build it directly.
struct FAnimationStateMachineDesc
{
	std::vector<FAnimationParameterDesc> Parameters;
	std::vector<FAnimationStateDesc> States;
	std::vector<FAnimationTransitionDesc> Transitions;
	std::size_t Entry = 0;
};

// Validates a description against a library's clips, resolving clip names once so updates never search.
class FAnimationStateMachine
{
public:
	[[nodiscard]] static std::expected<FAnimationStateMachine, FAnimationError> Create(FAnimationStateMachineDesc Desc, const FAnimationLibrary& Library);

	FAnimationStateMachine(const FAnimationStateMachine&) = default;
	FAnimationStateMachine(FAnimationStateMachine&&) noexcept = default;
	FAnimationStateMachine& operator=(const FAnimationStateMachine&) = default;
	FAnimationStateMachine& operator=(FAnimationStateMachine&&) noexcept = default;
	~FAnimationStateMachine() = default;

	[[nodiscard]] std::optional<std::size_t> FindParameter(std::string_view Name) const noexcept;
	// Unknown indices are ignored, so a graph without a parameter the game drives still plays.
	void SetFloat(std::size_t Parameter, float Value) noexcept;
	void SetBool(std::size_t Parameter, bool bValue) noexcept;
	void SetTrigger(std::size_t Parameter) noexcept;

	// Advances time, takes at most one transition, and clears triggers nothing consumed.
	void Update(float DeltaSeconds) noexcept;
	// Layers to evaluate this frame: the active state's samples, plus the previous state's while a transition fades it out.
	[[nodiscard]] std::span<const FAnimationLayer> GetLayers() const noexcept;

	[[nodiscard]] std::size_t GetActiveState() const noexcept;
	// The transition being blended, for live highlighting; empty when none is.
	[[nodiscard]] std::optional<std::size_t> GetActiveTransition() const noexcept;
	[[nodiscard]] const FAnimationStateMachineDesc& GetDesc() const noexcept;

private:
	struct FResolvedState
	{
		std::vector<std::size_t> Clips;
		std::vector<float> Durations;
	};

	struct FPlayback
	{
		std::size_t State = 0;
		// Normalized phase in [0, 1], shared by a blend's samples so their strides stay in step.
		float Phase = 0.f;
		bool bFinished = false;
	};

	FAnimationStateMachine() = default;

	[[nodiscard]] bool Holds(const FAnimationConditionDesc& Condition) const noexcept;
	[[nodiscard]] std::optional<std::size_t> FindTransition() const noexcept;
	void Advance(FPlayback& Playback, float DeltaSeconds) const noexcept;
	void AppendLayers(const FPlayback& Playback, float Weight);

	FAnimationStateMachineDesc Desc;
	std::vector<FResolvedState> Resolved;
	std::vector<float> Values;
	std::vector<bool> Triggers;

	FPlayback Current;
	std::optional<FPlayback> Previous;
	std::optional<std::size_t> Transition;
	float TransitionElapsed = 0.f;
	std::vector<FAnimationLayer> Layers;
};
}
