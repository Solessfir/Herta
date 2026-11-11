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
	// Runs the machine's pose operations on a pose just evaluated from its layers.
	void ApplyPoseOperations(FAnimationPose& Pose) const;

	[[nodiscard]] std::size_t GetActiveState() const noexcept;
	// How far through its clip the active state is, from 0 to 1.
	[[nodiscard]] float GetActivePhase() const noexcept;
	// The transition being blended, for live highlighting; empty when none is.
	[[nodiscard]] std::optional<std::size_t> GetActiveTransition() const noexcept;
	[[nodiscard]] const FAnimationStateMachineDesc& GetDesc() const noexcept;
	// The machine a nested state plays, which shares this machine's parameters; empty for an unknown index.
	[[nodiscard]] const FAnimationStateMachine* GetSubMachine(std::size_t Index) const noexcept;

private:
	struct FResolvedState
	{
		std::vector<std::size_t> Clips;
		std::vector<float> Durations;
		// Per blend tree node: the clip and its length for clip nodes, and the mask joint for layered nodes.
		std::vector<std::size_t> TreeClips;
		std::vector<float> TreeDurations;
		std::vector<std::optional<std::size_t>> TreeMasks;
	};

	// What a blend tree passes down to the layers below a node.
	struct FLayerScope
	{
		float UpperBody = 1.f;
		float LowerBody = 1.f;
		bool bAdditive = false;
		std::optional<std::size_t> MaskJoint{};
		float InsideMask = 1.f;
		float OutsideMask = 1.f;
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

	[[nodiscard]] static std::expected<FAnimationStateMachine, FAnimationError> CreateNested(FAnimationStateMachineDesc Desc, const FAnimationLibrary& Library, std::size_t Depth);
	// Back to the entry state, as when a state that plays this machine is entered.
	void Restart();
	void ClearTriggers() noexcept;
	void AppendTree(std::size_t State, std::size_t Node, float Weight, const FLayerScope& Scope, float Phase);
	// The weighted length of the clips a tree blends, which its phase advances by; additive and layered-over clips do not count.
	void SumTreeDuration(std::size_t State, std::size_t Node, float Weight, float& Weights, float& Seconds) const noexcept;
	[[nodiscard]] float Value(const std::optional<std::size_t>& Parameter, float Fallback) const noexcept;
	[[nodiscard]] bool Holds(const FAnimationConditionDesc& Condition) const noexcept;
	[[nodiscard]] std::optional<std::size_t> FindTransition() const noexcept;
	// Returns the phase before wrapping or clamping, which may pass 1.
	float Advance(FPlayback& Playback, float DeltaSeconds) const noexcept;
	void AppendLayers(const FPlayback& Playback, float Weight, float UpperBodyWeight = 1.f, float LowerBodyWeight = 1.f);
	// How far the legs' state's blend has moved from its first sample to its second, such as from standing to walking; 0 without a blend.
	[[nodiscard]] float GetBaseMotion() const noexcept;

	FAnimationStateMachineDesc Desc;
	std::vector<FResolvedState> Resolved{};
	std::vector<float> Values{};
	std::vector<bool> Triggers{};

	FPlayback Current;
	std::optional<FPlayback> Previous;
	// The last full-body state, which keeps playing on the legs under upper-body states and picks up in step when it returns.
	FPlayback Base;
	std::optional<std::size_t> Transition{};
	float TransitionElapsed = 0.f;
	std::vector<FAnimationLayer> Layers{};
	std::vector<std::string> Events{};
	std::vector<FAnimationStateMachine> SubMachines{};
	std::vector<std::size_t> PoseJoints{};
};
}
