#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Assets/AnimationStateMachine.h"

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
	void Update(float DeltaSeconds);
	// Events the active state reached during the last update, in authored order. A state fading out fires none.
	[[nodiscard]] std::span<const std::string> GetEvents() const noexcept;
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
		// Until its first update, events at the very start of the state fire too.
		bool bStarted = false;
	};

	FAnimationStateMachine() = default;

	[[nodiscard]] bool Holds(const FAnimationConditionDesc& Condition) const noexcept;
	[[nodiscard]] std::optional<std::size_t> FindTransition() const noexcept;
	// Returns the phase before wrapping or clamping, which may pass 1.
	float Advance(FPlayback& Playback, float DeltaSeconds) const noexcept;
	void AppendLayers(const FPlayback& Playback, float Weight);

	FAnimationStateMachineDesc Desc;
	std::vector<FResolvedState> Resolved{};
	std::vector<float> Values{};
	std::vector<bool> Triggers{};

	FPlayback Current;
	std::optional<FPlayback> Previous;
	std::optional<std::size_t> Transition{};
	float TransitionElapsed = 0.f;
	std::vector<FAnimationLayer> Layers{};
	std::vector<std::string> Events{};
};
}
