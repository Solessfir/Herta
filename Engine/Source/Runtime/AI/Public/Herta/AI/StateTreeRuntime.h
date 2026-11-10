#pragma once

#include "Herta/Assets/StateTree.h"
#include "Herta/Math/Vector.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// Transitions one tick may take, so states that finish as soon as they start cannot loop forever.
inline constexpr std::size_t MaximumStateTreeTransitionsPerTick = 8;

enum class EStateTreeStatus : std::uint8_t
{
	Running,
	Succeeded,
	Failed,
};

// A running task's mutable data, pooled per agent; tasks keep whatever they need in these fields.
struct FStateTreeTaskInstance
{
	EStateTreeStatus Status = EStateTreeStatus::Running;
	float Seconds = 0.f;
	float Duration = 0.f;
	FVector3 Point{};
};

// What a running tree asks of the game: the contract every built-in task and condition goes through.
class IStateTreeContext
{
public:
	IStateTreeContext() = default;
	virtual ~IStateTreeContext() = default;

	IStateTreeContext(const IStateTreeContext&) = delete;
	IStateTreeContext& operator=(const IStateTreeContext&) = delete;
	IStateTreeContext(IStateTreeContext&&) = delete;
	IStateTreeContext& operator=(IStateTreeContext&&) = delete;

	virtual EStateTreeStatus StartTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance) = 0;
	virtual EStateTreeStatus TickTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance, float Seconds) = 0;
	virtual void StopTask(const FStateTreeTaskDesc& Task, FStateTreeTaskInstance& Instance) = 0;
	// Every condition but TimeInState, which the tree answers itself.
	virtual bool TestCondition(const FStateTreeConditionDesc& Condition) = 0;
	virtual float ReadInput(EStateTreeInput Input) = 0;
};

enum class EStateTreeTraceType : std::uint8_t
{
	Enter,
	Exit,
	Succeeded,
	Failed,
	// Detail is the transition's index in the program, or StateTreeNone for a restart from the root.
	Transition,
	// Detail is the event's name index.
	Event,
	// Detail is how many transitions the tick took when it hit the limit.
	TransitionLimit,
};

struct FStateTreeTraceEvent
{
	bool operator==(const FStateTreeTraceEvent&) const = default;

	std::uint64_t Tick = 0;
	EStateTreeTraceType Type = EStateTreeTraceType::Enter;
	std::uint16_t State = StateTreeNone;
	std::uint16_t Detail = StateTreeNone;
};

// Task data for many agents in one allocation; an agent holds a slot sized for its tree's busiest path.
class FStateTreeInstancePool final
{
public:
	[[nodiscard]] std::uint32_t Allocate(std::uint16_t Stride);
	void Release(std::uint32_t Slot) noexcept;
	[[nodiscard]] std::span<FStateTreeTaskInstance> Get(std::uint32_t Slot) noexcept;
	[[nodiscard]] std::size_t GetSlotCount() const noexcept;

private:
	struct FSlot
	{
		std::uint32_t Offset = 0;
		std::uint16_t Stride = 0;
		bool bFree = false;
	};

	std::vector<FStateTreeTaskInstance> Data{};
	std::vector<FSlot> Slots{};
};

// One agent's run of a shared program: its active states from the root to a leaf, queued events, and a slot of pooled task data. Ticks
// run the active tasks, then take transitions from the leaf up, then restart from the root when the leaf finished and nothing handled it.
class FStateTreeExecution final
{
public:
	FStateTreeExecution(std::shared_ptr<const FCookedStateTree> InProgram, FStateTreeInstancePool& InPool);
	~FStateTreeExecution();

	FStateTreeExecution(const FStateTreeExecution&) = delete;
	FStateTreeExecution& operator=(const FStateTreeExecution&) = delete;
	FStateTreeExecution(FStateTreeExecution&&) = delete;
	FStateTreeExecution& operator=(FStateTreeExecution&&) = delete;

	void Tick(float Seconds, IStateTreeContext& Context);
	// Unknown events are ignored; known ones wait for the next tick.
	void PostEvent(std::string_view Event);
	// Exits every active state, stopping its tasks.
	void Stop(IStateTreeContext& Context);
	// Records what the tree does from now on, until passed null; the caller owns the trace.
	void SetTrace(std::vector<FStateTreeTraceEvent>* InTrace) noexcept;

	[[nodiscard]] std::span<const std::uint16_t> GetActiveStates() const noexcept;
	[[nodiscard]] const FCookedStateTree& GetProgram() const noexcept;
	[[nodiscard]] std::uint64_t GetTickCount() const noexcept;
	[[nodiscard]] bool HasPendingEvents() const noexcept;

private:
	[[nodiscard]] std::span<FStateTreeTaskInstance> GetTasks(std::size_t Depth) noexcept;
	[[nodiscard]] bool TestConditions(std::uint16_t First, std::uint16_t Count, std::size_t Depth, IStateTreeContext& Context);
	// The path below State to a leaf the tree may enter, or empty when no child can be entered.
	[[nodiscard]] bool Select(std::uint16_t State, std::vector<std::uint16_t>& Path, IStateTreeContext& Context);
	void ExitTo(std::size_t Depth, IStateTreeContext& Context);
	void EnterPath(std::span<const std::uint16_t> Path, IStateTreeContext& Context);
	// Enters Target through the path from the root, keeping the states it shares with the current one; false when it has no leaf to enter.
	bool Goto(std::uint16_t Target, IStateTreeContext& Context);
	[[nodiscard]] EStateTreeStatus GetStatus(std::size_t Depth) noexcept;
	void Record(EStateTreeTraceType Type, std::uint16_t State, std::uint16_t Detail = StateTreeNone);

	std::shared_ptr<const FCookedStateTree> Program;
	FStateTreeInstancePool& Pool;
	std::uint32_t Slot = 0;

	std::array<std::uint16_t, MaximumStateTreeDepth> Active{};
	std::array<float, MaximumStateTreeDepth> Seconds{};
	// A state finished by a child's Succeed or Fail transition rather than by its own tasks.
	std::array<std::optional<EStateTreeStatus>, MaximumStateTreeDepth> Forced{};
	std::size_t ActiveCount = 0;
	std::vector<std::uint16_t> Events{};
	std::vector<std::uint16_t> Selection{};
	std::uint64_t TickCount = 0;
	std::vector<FStateTreeTraceEvent>* Trace = nullptr;
};
}
