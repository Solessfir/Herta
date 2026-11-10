#include "StateTreeSerialization.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FAssetError> Invalid(const std::string_view Message)
{
	return std::unexpected(FAssetError{std::format("Invalid StateTree: {}", Message)});
}

[[nodiscard]] bool InRange(const std::uint16_t First, const std::uint16_t Count, const std::size_t Size)
{
	return static_cast<std::size_t>(First) + Count <= Size;
}

template <typename TEnum> [[nodiscard]] bool IsEnum(const TEnum Value, const TEnum Last)
{
	return static_cast<std::uint8_t>(Value) <= static_cast<std::uint8_t>(Last);
}

template <typename TEnum> TEnum ReadEnum(FBinaryReader& Reader, const TEnum Last)
{
	const auto Value = Reader.Read<std::uint8_t>();
	if (Value > static_cast<std::uint8_t>(Last))
	{
		Reader.Invalidate();
		return Last;
	}

	return static_cast<TEnum>(Value);
}

[[nodiscard]] std::size_t ReadCount(FBinaryReader& Reader, const std::size_t Maximum)
{
	const auto Count = Reader.Read<std::uint32_t>();
	if (Count > Maximum)
	{
		Reader.Invalidate();
		return 0;
	}

	return Count;
}
}

std::expected<void, FAssetError> ValidateStateTree(const FCookedStateTree& Tree)
{
	if (Tree.States.empty() || Tree.States.size() > MaximumStateTreeStates || Tree.Names.size() > MaximumStateTreeNames)
	{
		return Invalid(std::format("needs 1 to {} states and at most {} names", MaximumStateTreeStates, MaximumStateTreeNames));
	}

	if (Tree.Tasks.size() > MaximumStateTreeItems || Tree.Conditions.size() > MaximumStateTreeItems || Tree.Transitions.size() > MaximumStateTreeItems || Tree.Considerations.size() > MaximumStateTreeItems)
	{
		return Invalid(std::format("has more than {} tasks, conditions, transitions, or considerations", MaximumStateTreeItems));
	}

	const auto IsName = [&Tree](const std::uint16_t Name)
	{
		return Name == StateTreeNone || Name < Tree.Names.size();
	};

	const std::size_t Count = Tree.States.size();
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FStateTreeStateDesc& State = Tree.States[Index];
		const bool bRoot = Index == 0;
		if (bRoot != (State.Parent == StateTreeNone) || (!bRoot && (State.Parent >= Index || State.Depth != Tree.States[State.Parent].Depth + 1)) || State.Depth >= MaximumStateTreeDepth)
		{
			return Invalid(std::format("state {} has no earlier parent one level up, or is deeper than {}", Index, MaximumStateTreeDepth));
		}

		// Depth first: a first child follows its parent, and a sibling follows the whole subtree before it.
		if ((State.FirstChild != StateTreeNone && (State.FirstChild != Index + 1 || Tree.States[State.FirstChild].Parent != Index)) || (State.NextSibling != StateTreeNone && (State.NextSibling <= Index || State.NextSibling >= Count || Tree.States[State.NextSibling].Parent != State.Parent)))
		{
			return Invalid(std::format("state {} breaks the depth-first layout", Index));
		}

		if (!InRange(State.FirstTask, State.TaskCount, Tree.Tasks.size()) || !InRange(State.FirstCondition, State.ConditionCount, Tree.Conditions.size()) || !InRange(State.FirstTransition, State.TransitionCount, Tree.Transitions.size())
		    || !InRange(State.FirstConsideration, State.ConsiderationCount, Tree.Considerations.size()) || !IsEnum(State.Selection, EStateTreeSelection::Utility) || !std::isfinite(State.Score))
		{
			return Invalid(std::format("state {} has a range past the program or an invalid selection", Index));
		}
	}

	for (const FStateTreeTaskDesc& Task : Tree.Tasks)
	{
		if (!IsEnum(Task.Type, EStateTreeTaskType::Surround) || !IsEnum(Task.Target, EStateTreeTarget::Home) || !IsName(Task.Key) || !std::ranges::all_of(Task.Values, [](const float Value)
		{
			return std::isfinite(Value);
		}))
		{
			return Invalid("a task has an unknown type or target, a missing name, or a value that is not finite");
		}
	}

	for (const FStateTreeConditionDesc& Condition : Tree.Conditions)
	{
		if (!IsEnum(Condition.Type, EStateTreeConditionType::Flag) || !IsEnum(Condition.Compare, EStateTreeCompare::Greater) || !IsName(Condition.Key) || !std::isfinite(Condition.Value))
		{
			return Invalid("a condition has an unknown type or comparison, a missing name, or a value that is not finite");
		}
	}

	for (const FStateTreeTransitionDesc& Transition : Tree.Transitions)
	{
		const bool bTarget = Transition.Kind != EStateTreeTransitionKind::GotoState || Transition.Target < Count;
		if (!IsEnum(Transition.Trigger, EStateTreeTrigger::OnEvent) || !IsEnum(Transition.Kind, EStateTreeTransitionKind::Fail) || !bTarget || !IsName(Transition.Event) || (Transition.Trigger == EStateTreeTrigger::OnEvent && Transition.Event == StateTreeNone)
		    || !InRange(Transition.FirstCondition, Transition.ConditionCount, Tree.Conditions.size()))
		{
			return Invalid("a transition has an unknown trigger or kind, a missing target or event, or conditions past the program");
		}
	}

	for (const FStateTreeConsiderationDesc& Consideration : Tree.Considerations)
	{
		if (!IsEnum(Consideration.Input, EStateTreeInput::SecondsSinceHeard) || !std::isfinite(Consideration.Minimum) || !std::isfinite(Consideration.Maximum) || Consideration.Maximum <= Consideration.Minimum || !std::isfinite(Consideration.Weight))
		{
			return Invalid("a consideration has an unknown input, an empty range, or a weight that is not finite");
		}
	}

	// Each state's tasks stay active while its children run, so the deepest sum along any path bounds an agent's task data.
	std::vector<std::size_t> Active(Count, 0);
	std::size_t Deepest = 0;
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FStateTreeStateDesc& State = Tree.States[Index];
		Active[Index] = State.TaskCount + (Index == 0 ? 0 : Active[State.Parent]);
		Deepest = std::max(Deepest, Active[Index]);
	}

	if (Deepest > Tree.MaximumActiveTasks)
	{
		return Invalid(std::format("a path runs {} tasks at once, more than its {}", Deepest, Tree.MaximumActiveTasks));
	}

	return {};
}

void WriteStateTree(FBinaryWriter& Writer, const FCookedStateTree& Tree)
{
	Writer.Write(static_cast<std::uint32_t>(Tree.States.size()));
	for (const FStateTreeStateDesc& State : Tree.States)
	{
		Writer.WriteString(State.Name);
		Writer.Write(State.Node);
		for (const std::uint16_t Value : {State.Parent, State.FirstChild, State.NextSibling, State.Depth, State.FirstTask, State.TaskCount, State.FirstCondition, State.ConditionCount, State.FirstTransition, State.TransitionCount, State.FirstConsideration, State.ConsiderationCount})
		{
			Writer.Write(Value);
		}

		Writer.Write(static_cast<std::uint8_t>(State.Selection));
		Writer.WriteFloat(State.Score);
	}

	Writer.Write(static_cast<std::uint32_t>(Tree.Tasks.size()));
	for (const FStateTreeTaskDesc& Task : Tree.Tasks)
	{
		Writer.Write(static_cast<std::uint8_t>(Task.Type));
		Writer.Write(static_cast<std::uint8_t>(Task.Target));
		Writer.WriteFloat(Task.Values[0]);
		Writer.WriteFloat(Task.Values[1]);
		Writer.Write(Task.Key);
	}

	Writer.Write(static_cast<std::uint32_t>(Tree.Conditions.size()));
	for (const FStateTreeConditionDesc& Condition : Tree.Conditions)
	{
		Writer.Write(static_cast<std::uint8_t>(Condition.Type));
		Writer.Write(static_cast<std::uint8_t>(Condition.Compare));
		Writer.Write(static_cast<std::uint8_t>(Condition.bInvert ? 1 : 0));
		Writer.WriteFloat(Condition.Value);
		Writer.Write(Condition.Key);
	}

	Writer.Write(static_cast<std::uint32_t>(Tree.Transitions.size()));
	for (const FStateTreeTransitionDesc& Transition : Tree.Transitions)
	{
		Writer.Write(static_cast<std::uint8_t>(Transition.Trigger));
		Writer.Write(static_cast<std::uint8_t>(Transition.Kind));
		Writer.Write(Transition.Target);
		Writer.Write(Transition.Event);
		Writer.Write(Transition.FirstCondition);
		Writer.Write(Transition.ConditionCount);
	}

	Writer.Write(static_cast<std::uint32_t>(Tree.Considerations.size()));
	for (const FStateTreeConsiderationDesc& Consideration : Tree.Considerations)
	{
		Writer.Write(static_cast<std::uint8_t>(Consideration.Input));
		Writer.WriteFloat(Consideration.Minimum);
		Writer.WriteFloat(Consideration.Maximum);
		Writer.WriteFloat(Consideration.Weight);
		Writer.Write(static_cast<std::uint8_t>(Consideration.bInvert ? 1 : 0));
	}

	Writer.Write(static_cast<std::uint32_t>(Tree.Names.size()));
	for (const std::string& Name : Tree.Names)
	{
		Writer.WriteString(Name);
	}

	Writer.Write(Tree.MaximumActiveTasks);
}

FCookedStateTree ReadStateTree(FBinaryReader& Reader)
{
	FCookedStateTree Tree;
	Tree.States.resize(ReadCount(Reader, MaximumStateTreeStates));
	for (FStateTreeStateDesc& State : Tree.States)
	{
		State.Name = Reader.ReadString(128);
		State.Node = Reader.Read<std::uint32_t>();
		for (std::uint16_t* const Value : {&State.Parent, &State.FirstChild, &State.NextSibling, &State.Depth, &State.FirstTask, &State.TaskCount, &State.FirstCondition, &State.ConditionCount, &State.FirstTransition, &State.TransitionCount, &State.FirstConsideration, &State.ConsiderationCount})
		{
			*Value = Reader.Read<std::uint16_t>();
		}

		State.Selection = ReadEnum(Reader, EStateTreeSelection::Utility);
		State.Score = Reader.ReadFloat();
	}

	Tree.Tasks.resize(ReadCount(Reader, MaximumStateTreeItems));
	for (FStateTreeTaskDesc& Task : Tree.Tasks)
	{
		Task.Type = ReadEnum(Reader, EStateTreeTaskType::Surround);
		Task.Target = ReadEnum(Reader, EStateTreeTarget::Home);
		Task.Values = {Reader.ReadFloat(), Reader.ReadFloat()};
		Task.Key = Reader.Read<std::uint16_t>();
	}

	Tree.Conditions.resize(ReadCount(Reader, MaximumStateTreeItems));
	for (FStateTreeConditionDesc& Condition : Tree.Conditions)
	{
		Condition.Type = ReadEnum(Reader, EStateTreeConditionType::Flag);
		Condition.Compare = ReadEnum(Reader, EStateTreeCompare::Greater);
		Condition.bInvert = Reader.Read<std::uint8_t>() != 0;
		Condition.Value = Reader.ReadFloat();
		Condition.Key = Reader.Read<std::uint16_t>();
	}

	Tree.Transitions.resize(ReadCount(Reader, MaximumStateTreeItems));
	for (FStateTreeTransitionDesc& Transition : Tree.Transitions)
	{
		Transition.Trigger = ReadEnum(Reader, EStateTreeTrigger::OnEvent);
		Transition.Kind = ReadEnum(Reader, EStateTreeTransitionKind::Fail);
		Transition.Target = Reader.Read<std::uint16_t>();
		Transition.Event = Reader.Read<std::uint16_t>();
		Transition.FirstCondition = Reader.Read<std::uint16_t>();
		Transition.ConditionCount = Reader.Read<std::uint16_t>();
	}

	Tree.Considerations.resize(ReadCount(Reader, MaximumStateTreeItems));
	for (FStateTreeConsiderationDesc& Consideration : Tree.Considerations)
	{
		Consideration.Input = ReadEnum(Reader, EStateTreeInput::SecondsSinceHeard);
		Consideration.Minimum = Reader.ReadFloat();
		Consideration.Maximum = Reader.ReadFloat();
		Consideration.Weight = Reader.ReadFloat();
		Consideration.bInvert = Reader.Read<std::uint8_t>() != 0;
	}

	Tree.Names.resize(ReadCount(Reader, MaximumStateTreeNames));
	for (std::string& Name : Tree.Names)
	{
		Name = Reader.ReadString(128);
	}

	Tree.MaximumActiveTasks = Reader.Read<std::uint16_t>();
	return Tree;
}
}
