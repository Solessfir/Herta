#include "Herta/AI/StateTreeRuntime.h"

#include <algorithm>
#include <numeric>

namespace Herta
{
std::uint32_t FStateTreeInstancePool::Allocate(const std::uint16_t Stride)
{
	for (std::size_t Index = 0; Index < Slots.size(); ++Index)
	{
		if (Slots[Index].bFree && Slots[Index].Stride == Stride)
		{
			Slots[Index].bFree = false;
			return static_cast<std::uint32_t>(Index);
		}
	}

	Slots.push_back({.Offset = static_cast<std::uint32_t>(Data.size()), .Stride = Stride});
	Data.resize(Data.size() + Stride);
	return static_cast<std::uint32_t>(Slots.size() - 1);
}

void FStateTreeInstancePool::Release(const std::uint32_t Slot) noexcept
{
	if (Slot < Slots.size())
	{
		Slots[Slot].bFree = true;
		std::ranges::fill(Get(Slot), FStateTreeTaskInstance{});
	}
}

std::span<FStateTreeTaskInstance> FStateTreeInstancePool::Get(const std::uint32_t Slot) noexcept
{
	return Slot < Slots.size() ? std::span(Data).subspan(Slots[Slot].Offset, Slots[Slot].Stride) : std::span<FStateTreeTaskInstance>{};
}

std::size_t FStateTreeInstancePool::GetSlotCount() const noexcept
{
	return Slots.size();
}

FStateTreeExecution::FStateTreeExecution(std::shared_ptr<const FCookedStateTree> InProgram, FStateTreeInstancePool& InPool)
    : Program(std::move(InProgram))
    , Pool(InPool)
    , Slot(InPool.Allocate(std::max<std::uint16_t>(Program->MaximumActiveTasks, 1)))
{
}

FStateTreeExecution::~FStateTreeExecution()
{
	Pool.Release(Slot);
}

void FStateTreeExecution::SetTrace(std::vector<FStateTreeTraceEvent>* const InTrace) noexcept
{
	Trace = InTrace;
}

std::span<const std::uint16_t> FStateTreeExecution::GetActiveStates() const noexcept
{
	return std::span(Active).first(ActiveCount);
}

const FCookedStateTree& FStateTreeExecution::GetProgram() const noexcept
{
	return *Program;
}

std::uint64_t FStateTreeExecution::GetTickCount() const noexcept
{
	return TickCount;
}

bool FStateTreeExecution::HasPendingEvents() const noexcept
{
	return !Events.empty();
}

void FStateTreeExecution::Record(const EStateTreeTraceType Type, const std::uint16_t State, const std::uint16_t Detail)
{
	if (Trace)
	{
		Trace->push_back({.Tick = TickCount, .Type = Type, .State = State, .Detail = Detail});
	}
}

void FStateTreeExecution::PostEvent(const std::string_view Event)
{
	const auto Found = std::ranges::find(Program->Names, Event);
	if (Found != Program->Names.end())
	{
		Events.push_back(static_cast<std::uint16_t>(Found - Program->Names.begin()));
	}
}

std::span<FStateTreeTaskInstance> FStateTreeExecution::GetTasks(const std::size_t Depth) noexcept
{
	std::size_t Offset = 0;
	for (std::size_t Index = 0; Index < Depth; ++Index)
	{
		Offset += Program->States[Active[Index]].TaskCount;
	}

	return Pool.Get(Slot).subspan(Offset, Program->States[Active[Depth]].TaskCount);
}

bool FStateTreeExecution::TestConditions(const std::uint16_t First, const std::uint16_t Count, const std::size_t Depth, IStateTreeContext& Context)
{
	for (std::uint16_t Index = First; Index < First + Count; ++Index)
	{
		const FStateTreeConditionDesc& Condition = Program->Conditions[Index];
		bool bHolds = false;
		if (Condition.Type == EStateTreeConditionType::TimeInState)
		{
			// A state about to be entered has been in it for no time at all.
			const float Elapsed = Depth < ActiveCount ? Seconds[Depth] : 0.f;
			bHolds = Condition.Compare == EStateTreeCompare::Less ? Elapsed < Condition.Value : Elapsed > Condition.Value;
		}
		else
		{
			bHolds = Context.TestCondition(Condition);
		}

		if (bHolds == Condition.bInvert)
		{
			return false;
		}
	}

	return true;
}

bool FStateTreeExecution::Select(const std::uint16_t State, std::vector<std::uint16_t>& Path, IStateTreeContext& Context)
{
	const FStateTreeStateDesc& Parent = Program->States[State];
	if (Parent.FirstChild == StateTreeNone)
	{
		return true;
	}

	std::vector<std::pair<float, std::uint16_t>> Candidates;
	for (std::uint16_t Child = Parent.FirstChild; Child != StateTreeNone; Child = Program->States[Child].NextSibling)
	{
		const FStateTreeStateDesc& Desc = Program->States[Child];
		if (!TestConditions(Desc.FirstCondition, Desc.ConditionCount, MaximumStateTreeDepth, Context))
		{
			continue;
		}

		float Score = Desc.Score;
		for (std::uint16_t Index = Desc.FirstConsideration; Index < Desc.FirstConsideration + Desc.ConsiderationCount; ++Index)
		{
			const FStateTreeConsiderationDesc& Consideration = Program->Considerations[Index];
			const float Normalized = std::clamp((Context.ReadInput(Consideration.Input) - Consideration.Minimum) / (Consideration.Maximum - Consideration.Minimum), 0.f, 1.f);
			Score += Consideration.Weight * (Consideration.bInvert ? 1.f - Normalized : Normalized);
		}

		Candidates.emplace_back(Parent.Selection == EStateTreeSelection::Utility ? Score : 0.f, Child);
	}

	// Stable, so equal scores keep the authored order and first-eligible selection keeps it outright.
	std::ranges::stable_sort(Candidates, std::ranges::greater{}, &std::pair<float, std::uint16_t>::first);
	for (const auto& [Score, Child] : Candidates)
	{
		const std::size_t Mark = Path.size();
		Path.push_back(Child);
		if (Select(Child, Path, Context))
		{
			return true;
		}

		Path.resize(Mark);
	}

	return false;
}

void FStateTreeExecution::ExitTo(const std::size_t Depth, IStateTreeContext& Context)
{
	while (ActiveCount > Depth)
	{
		const std::size_t Leaf = ActiveCount - 1;
		const FStateTreeStateDesc& State = Program->States[Active[Leaf]];
		const std::span<FStateTreeTaskInstance> Tasks = GetTasks(Leaf);
		for (std::size_t Index = Tasks.size(); Index-- > 0;)
		{
			if (Tasks[Index].Status == EStateTreeStatus::Running)
			{
				Context.StopTask(Program->Tasks[State.FirstTask + Index], Tasks[Index]);
			}
		}

		Record(EStateTreeTraceType::Exit, Active[Leaf]);
		--ActiveCount;
	}
}

void FStateTreeExecution::EnterPath(const std::span<const std::uint16_t> Path, IStateTreeContext& Context)
{
	for (const std::uint16_t StateIndex : Path)
	{
		const std::size_t Depth = ActiveCount++;
		Active[Depth] = StateIndex;
		Seconds[Depth] = 0.f;
		Forced[Depth].reset();
		Record(EStateTreeTraceType::Enter, StateIndex);
		const FStateTreeStateDesc& State = Program->States[StateIndex];
		const std::span<FStateTreeTaskInstance> Tasks = GetTasks(Depth);
		for (std::size_t Index = 0; Index < Tasks.size(); ++Index)
		{
			Tasks[Index] = {};
			Tasks[Index].Status = Context.StartTask(Program->Tasks[State.FirstTask + Index], Tasks[Index]);
		}
	}
}

bool FStateTreeExecution::Goto(const std::uint16_t Target, IStateTreeContext& Context)
{
	std::vector<std::uint16_t> Route;
	for (std::uint16_t State = Target; State != StateTreeNone; State = Program->States[State].Parent)
	{
		Route.push_back(State);
	}

	std::ranges::reverse(Route);

	// The target itself is always entered anew, even when it is active, so a transition to it restarts it.
	std::size_t Shared = 0;
	while (Shared + 1 < Route.size() && Shared < ActiveCount && Active[Shared] == Route[Shared])
	{
		++Shared;
	}

	Selection.assign(Route.begin() + static_cast<std::ptrdiff_t>(Shared), Route.end());
	if (!Select(Target, Selection, Context))
	{
		return false;
	}

	ExitTo(Shared, Context);
	EnterPath(Selection, Context);
	return true;
}

EStateTreeStatus FStateTreeExecution::GetStatus(const std::size_t Depth) noexcept
{
	if (Forced[Depth])
	{
		return *Forced[Depth];
	}

	const FStateTreeStateDesc& State = Program->States[Active[Depth]];
	if (State.TaskCount == 0)
	{
		// A container runs as long as its children do; an empty leaf finishes at once.
		return State.FirstChild == StateTreeNone ? EStateTreeStatus::Succeeded : EStateTreeStatus::Running;
	}

	const std::span<FStateTreeTaskInstance> Tasks = GetTasks(Depth);
	if (std::ranges::contains(Tasks, EStateTreeStatus::Failed, &FStateTreeTaskInstance::Status))
	{
		return EStateTreeStatus::Failed;
	}

	return std::ranges::contains(Tasks, EStateTreeStatus::Running, &FStateTreeTaskInstance::Status) ? EStateTreeStatus::Running : EStateTreeStatus::Succeeded;
}

void FStateTreeExecution::Tick(const float DeltaSeconds, IStateTreeContext& Context)
{
	++TickCount;
	if (ActiveCount == 0)
	{
		Selection.assign(1, 0);
		const FStateTreeStateDesc& Root = Program->States[0];
		if (!TestConditions(Root.FirstCondition, Root.ConditionCount, MaximumStateTreeDepth, Context) || !Select(0, Selection, Context))
		{
			Events.clear();
			return;
		}

		EnterPath(Selection, Context);
	}

	for (std::size_t Depth = 0; Depth < ActiveCount; ++Depth)
	{
		Seconds[Depth] += DeltaSeconds;
		const FStateTreeStateDesc& State = Program->States[Active[Depth]];
		const std::span<FStateTreeTaskInstance> Tasks = GetTasks(Depth);
		for (std::size_t Index = 0; Index < Tasks.size(); ++Index)
		{
			if (Tasks[Index].Status == EStateTreeStatus::Running)
			{
				Tasks[Index].Status = Context.TickTask(Program->Tasks[State.FirstTask + Index], Tasks[Index], DeltaSeconds);
				if (Tasks[Index].Status != EStateTreeStatus::Running)
				{
					Record(Tasks[Index].Status == EStateTreeStatus::Succeeded ? EStateTreeTraceType::Succeeded : EStateTreeTraceType::Failed, Active[Depth], static_cast<std::uint16_t>(Index));
				}
			}
		}
	}

	for (const std::uint16_t Event : Events)
	{
		Record(EStateTreeTraceType::Event, StateTreeNone, Event);
	}

	std::size_t Taken = 0;
	while (ActiveCount > 0)
	{
		std::optional<std::pair<std::size_t, std::uint16_t>> Chosen;
		for (std::size_t Depth = ActiveCount; Depth-- > 0 && !Chosen;)
		{
			const FStateTreeStateDesc& State = Program->States[Active[Depth]];
			const EStateTreeStatus Status = GetStatus(Depth);
			for (std::uint16_t Index = State.FirstTransition; Index < State.FirstTransition + State.TransitionCount; ++Index)
			{
				const FStateTreeTransitionDesc& Transition = Program->Transitions[Index];
				// Tick transitions only start a tick, so they cannot bounce straight back; an event waits through finishing transitions until one
				// takes it, and each event is taken once.
				const bool bTriggered = (Transition.Trigger == EStateTreeTrigger::OnCompleted && Status != EStateTreeStatus::Running) || (Transition.Trigger == EStateTreeTrigger::OnSucceeded && Status == EStateTreeStatus::Succeeded)
				                        || (Transition.Trigger == EStateTreeTrigger::OnFailed && Status == EStateTreeStatus::Failed) || (Taken == 0 && Transition.Trigger == EStateTreeTrigger::OnTick)
				                        || (Transition.Trigger == EStateTreeTrigger::OnEvent && std::ranges::contains(Events, Transition.Event));
				// A tick or event transition into a state that is already running would restart it every time, so it waits until that state ends.
				const bool bRestartsActive = (Transition.Trigger == EStateTreeTrigger::OnTick) && Transition.Kind == EStateTreeTransitionKind::GotoState && std::ranges::contains(GetActiveStates(), Transition.Target);
				if (bTriggered && !bRestartsActive && TestConditions(Transition.FirstCondition, Transition.ConditionCount, Depth, Context))
				{
					Chosen.emplace(Depth, Index);
					break;
				}
			}
		}

		const bool bLeafDone = GetStatus(ActiveCount - 1) != EStateTreeStatus::Running;
		if (!Chosen && !bLeafDone)
		{
			break;
		}

		if (Taken >= MaximumStateTreeTransitionsPerTick)
		{
			Record(EStateTreeTraceType::TransitionLimit, StateTreeNone, static_cast<std::uint16_t>(Taken));
			break;
		}

		++Taken;
		if (!Chosen)
		{
			// Nothing handled the finished leaf, so the tree starts over from the root.
			Record(EStateTreeTraceType::Transition, Active[ActiveCount - 1]);
			ExitTo(0, Context);
			Selection.assign(1, 0);
			const FStateTreeStateDesc& Root = Program->States[0];
			if (!TestConditions(Root.FirstCondition, Root.ConditionCount, MaximumStateTreeDepth, Context) || !Select(0, Selection, Context))
			{
				break;
			}

			EnterPath(Selection, Context);
			continue;
		}

		const auto [Depth, Index] = *Chosen;
		const FStateTreeTransitionDesc& Transition = Program->Transitions[Index];
		Record(EStateTreeTraceType::Transition, Active[Depth], Index);
		if (Transition.Trigger == EStateTreeTrigger::OnEvent)
		{
			std::erase(Events, Transition.Event);
		}

		if (Transition.Kind == EStateTreeTransitionKind::GotoState)
		{
			if (!Goto(Transition.Target, Context))
			{
				break;
			}

			continue;
		}

		const EStateTreeStatus Finished = Transition.Kind == EStateTreeTransitionKind::Succeed ? EStateTreeStatus::Succeeded : EStateTreeStatus::Failed;
		ExitTo(Depth, Context);
		if (Depth == 0)
		{
			break;
		}

		Forced[Depth - 1] = Finished;
	}

	Events.clear();
}

void FStateTreeExecution::Stop(IStateTreeContext& Context)
{
	ExitTo(0, Context);
	Events.clear();
}
}
