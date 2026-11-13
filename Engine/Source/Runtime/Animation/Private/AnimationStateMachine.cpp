#include "Herta/Animation/AnimationStateMachine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>

namespace Herta
{
namespace
{
inline constexpr std::size_t MaximumStates = 256;
inline constexpr std::size_t MaximumSamplesPerState = 16;

struct FBlendPoint
{
	std::size_t Low = 0;
	std::size_t High = 0;
	// Weight of High; Low has 1 - Alpha.
	float Alpha = 0.f;
};

template <typename T, typename F> [[nodiscard]] FBlendPoint FindBlend(const std::vector<T>& Points, const F Position, const float Value) noexcept
{
	if (Points.size() <= 1 || Value <= Points.front().*Position)
	{
		return {};
	}

	if (Value >= Points.back().*Position)
	{
		return {.Low = Points.size() - 1, .High = Points.size() - 1, .Alpha = 0.f};
	}

	const auto Upper = std::ranges::upper_bound(Points, Value, {}, Position);
	const auto High = static_cast<std::size_t>(Upper - Points.begin());
	const float Span = Points[High].*Position - Points[High - 1].*Position;
	return {.Low = High - 1, .High = High, .Alpha = Span > 0.f ? (Value - Points[High - 1].*Position) / Span : 0.f};
}

[[nodiscard]] FBlendPoint FindBlend(const std::vector<FAnimationBlendSample>& Samples, const float Position) noexcept
{
	return FindBlend(Samples, &FAnimationBlendSample::Position, Position);
}

inline constexpr std::size_t MaximumSubMachineDepth = 4;
}

std::expected<FAnimationStateMachine, FAnimationError> FAnimationStateMachine::Create(FAnimationStateMachineDesc Desc, const FAnimationLibrary& Library)
{
	return CreateNested(std::move(Desc), Library, 0);
}

std::expected<FAnimationStateMachine, FAnimationError> FAnimationStateMachine::CreateNested(FAnimationStateMachineDesc Desc, const FAnimationLibrary& Library, const std::size_t Depth)
{
	const auto Fail = [](std::string Message)
	{
		return std::unexpected(FAnimationError{std::move(Message)});
	};

	if (Desc.States.empty() || Desc.States.size() > MaximumStates || Desc.Entry >= Desc.States.size())
	{
		return Fail(std::format("A state machine needs between 1 and {} states and a valid entry state", MaximumStates));
	}

	for (const FAnimationParameterDesc& Parameter : Desc.Parameters)
	{
		if (Parameter.Name.empty() || !std::isfinite(Parameter.Default) || std::ranges::count(Desc.Parameters, Parameter.Name, &FAnimationParameterDesc::Name) != 1)
		{
			return Fail(std::format("State machine parameter '{}' needs a unique name and a finite default", Parameter.Name));
		}
	}

	FAnimationStateMachine Machine;
	Machine.Resolved.reserve(Desc.States.size());

	if (Depth > MaximumSubMachineDepth)
	{
		return Fail(std::format("State machines nest at most {} deep", MaximumSubMachineDepth));
	}

	const auto IsFloat = [&Desc](const std::optional<std::size_t>& Parameter)
	{
		return !Parameter || (*Parameter < Desc.Parameters.size() && Desc.Parameters[*Parameter].Type == EAnimationParameterType::Float);
	};

	// Nested machines see this machine's parameters, and are built first so states can check their indices.
	for (FAnimationStateMachineDesc& Nested : Desc.SubMachines)
	{
		Nested.Parameters = Desc.Parameters;
		auto Created = CreateNested(Nested, Library, Depth + 1);
		if (!Created)
		{
			return std::unexpected(std::move(Created.error()));
		}

		Machine.SubMachines.push_back(std::move(*Created));
	}

	for (const FAnimationPoseOperationDesc& Operation : Desc.PoseOperations)
	{
		const auto Joint = Library.FindJoint(Operation.Joint);
		const float Length = std::hypot(Operation.Axis[0], Operation.Axis[1], Operation.Axis[2]);
		if (!Joint || !IsFloat(Operation.Parameter) || !std::isfinite(Operation.Scale) || !std::isfinite(Length) || Length < 1.e-6f)
		{
			return Fail(std::format("Pose operation on joint '{}' needs a joint the skeleton has, an axis, and a float parameter", Operation.Joint));
		}

		Machine.PoseJoints.push_back(*Joint);
	}

	for (FAnimationStateDesc& State : Desc.States)
	{
		const std::size_t Sources = (State.Samples.empty() ? 0 : 1) + (State.Tree.empty() ? 0 : 1) + (State.SubMachine ? 1 : 0);
		if (Sources != 1 || State.Samples.size() > MaximumSamplesPerState || !std::isfinite(State.Speed) || State.Speed < 0.f)
		{
			return Fail(std::format("State '{}' needs one of clips, a blend tree, or a nested machine, at most {} clips, and a nonnegative speed", State.Name, MaximumSamplesPerState));
		}

		if (State.SubMachine && *State.SubMachine >= Machine.SubMachines.size())
		{
			return Fail(std::format("State '{}' plays a nested machine this machine does not have", State.Name));
		}

		if (State.Samples.size() > 1 && (!State.BlendParameter || *State.BlendParameter >= Desc.Parameters.size() || Desc.Parameters[*State.BlendParameter].Type != EAnimationParameterType::Float))
		{
			return Fail(std::format("Blend state '{}' needs a float parameter to blend along", State.Name));
		}

		if (!std::ranges::all_of(State.Events, [](const FAnimationEventDesc& Event)
		{
			return !Event.Name.empty() && Event.Phase >= 0.f && Event.Phase <= 1.f;
		}))
		{
			return Fail(std::format("State '{}' has an event without a name or outside its playback", State.Name));
		}

		std::ranges::stable_sort(State.Samples, {}, &FAnimationBlendSample::Position);
		FResolvedState& Resolved = Machine.Resolved.emplace_back();
		Resolved.TreeClips.assign(State.Tree.size(), 0);
		Resolved.TreeDurations.assign(State.Tree.size(), 0.f);
		Resolved.TreeMasks.assign(State.Tree.size(), std::nullopt);
		// Children come after their parent, so the tree cannot loop, and layered blends do not nest, so each layer has one mask.
		std::vector<bool> UnderLayer(State.Tree.size(), false);
		for (std::size_t Index = 0; Index < State.Tree.size(); ++Index)
		{
			FAnimationBlendNodeDesc& Node = State.Tree[Index];
			const auto Bad = [&](const std::string_view Why)
			{
				return Fail(std::format("Blend tree node {} of state '{}' {}", Index, State.Name, Why));
			};

			if (!std::ranges::all_of(Node.Children, [Index, &State](const FAnimationBlendChild& Child)
			{
				return Child.Node > Index && Child.Node < State.Tree.size() && std::isfinite(Child.X) && std::isfinite(Child.Y);
			}))
			{
				return Bad("links a child that does not come after it");
			}

			if (!IsFloat(Node.ParameterX) || !IsFloat(Node.ParameterY) || !std::isfinite(Node.Weight))
			{
				return Bad("needs float parameters and a finite weight");
			}

			const bool bLayered = Node.Type == EAnimationBlendNodeType::Layered;
			for (const FAnimationBlendChild& Child : Node.Children)
			{
				UnderLayer[Child.Node] = UnderLayer[Child.Node] || UnderLayer[Index] || bLayered;
			}

			switch (Node.Type)
			{
				case EAnimationBlendNodeType::Clip:
				{
					const auto Clip = Library.FindClip(Node.Clip);
					if (!Clip || !Node.Children.empty())
					{
						return Bad(std::format("plays clip '{}', which the skeleton does not have", Node.Clip));
					}

					Resolved.TreeClips[Index] = *Clip;
					Resolved.TreeDurations[Index] = Library.GetClipDuration(*Clip);
					break;
				}
				case EAnimationBlendNodeType::Blend1D:
				case EAnimationBlendNodeType::Blend2D:
					if (Node.Children.empty() || !Node.ParameterX || (Node.Type == EAnimationBlendNodeType::Blend2D && !Node.ParameterY))
					{
						return Bad("blends no children or has no parameter to blend along");
					}

					std::ranges::stable_sort(Node.Children, {}, &FAnimationBlendChild::X);
					break;
				case EAnimationBlendNodeType::Additive:
				case EAnimationBlendNodeType::Layered:
					if (Node.Children.size() != 2)
					{
						return Bad("needs a base and one more child");
					}

					if (bLayered)
					{
						Resolved.TreeMasks[Index] = Library.FindJoint(Node.MaskJoint);
						if (!Resolved.TreeMasks[Index] || UnderLayer[Index])
						{
							return Bad(std::format("masks joint '{}', which the skeleton does not have, or sits inside another layered blend", Node.MaskJoint));
						}
					}

					break;
				default:
					return Bad("has an unknown type");
			}
		}

		for (const FAnimationBlendSample& Sample : State.Samples)
		{
			const auto Clip = Library.FindClip(Sample.Clip);
			if (!Clip || !std::isfinite(Sample.Position))
			{
				return Fail(std::format("State '{}' plays clip '{}', which the skeleton does not have", State.Name, Sample.Clip));
			}

			Resolved.Clips.push_back(*Clip);
			Resolved.Durations.push_back(Library.GetClipDuration(*Clip));
		}
	}

	for (const FAnimationTransitionDesc& Transition : Desc.Transitions)
	{
		const bool bValidStates = (Transition.From == AnyAnimationState || Transition.From < Desc.States.size()) && Transition.To < Desc.States.size();
		const bool bValidConditions = std::ranges::all_of(Transition.Conditions, [&](const FAnimationConditionDesc& Condition)
		{
			if (Condition.Parameter >= Desc.Parameters.size() || !std::isfinite(Condition.Value))
			{
				return false;
			}

			const EAnimationParameterType Type = Desc.Parameters[Condition.Parameter].Type;
			switch (Condition.Test)
			{
				case EAnimationCondition::Greater:
				case EAnimationCondition::Less:
					return Type == EAnimationParameterType::Float;
				case EAnimationCondition::IsTrue:
				case EAnimationCondition::IsFalse:
					return Type == EAnimationParameterType::Bool;
				case EAnimationCondition::Triggered:
					return Type == EAnimationParameterType::Trigger;
			}

			return false;
		});

		if (!bValidStates || !bValidConditions || !std::isfinite(Transition.DurationSeconds) || Transition.DurationSeconds < 0.f)
		{
			return Fail("A state machine transition names a missing state or parameter, tests a parameter of the wrong type, or has a negative duration");
		}
	}

	Machine.Values.reserve(Desc.Parameters.size());

	for (const FAnimationParameterDesc& Parameter : Desc.Parameters)
	{
		Machine.Values.push_back(Parameter.Type == EAnimationParameterType::Trigger ? 0.f : Parameter.Default);
	}

	Machine.Triggers.assign(Desc.Parameters.size(), false);
	Machine.Current = {.State = Desc.Entry};
	Machine.Base = {.State = Desc.Entry};
	Machine.Desc = std::move(Desc);
	Machine.Layers.reserve(FAnimationPose::MaximumLayers);
	Machine.AppendLayers(Machine.Current, 1.f);
	return Machine;
}

void FAnimationStateMachine::Restart()
{
	Current = {.State = Desc.Entry};
	Base = Current;
	Previous.reset();
	Transition.reset();
	TransitionElapsed = 0.f;
	for (FAnimationStateMachine& Nested : SubMachines)
	{
		Nested.Restart();
	}
}

void FAnimationStateMachine::ClearTriggers() noexcept
{
	std::ranges::fill(Triggers, false);
	for (FAnimationStateMachine& Nested : SubMachines)
	{
		Nested.ClearTriggers();
	}
}

float FAnimationStateMachine::Value(const std::optional<std::size_t>& Parameter, const float Fallback) const noexcept
{
	return Parameter && *Parameter < Values.size() ? Values[*Parameter] : Fallback;
}

const FAnimationStateMachine* FAnimationStateMachine::GetSubMachine(const std::size_t Index) const noexcept
{
	return Index < SubMachines.size() ? &SubMachines[Index] : nullptr;
}

void FAnimationStateMachine::ApplyPoseOperations(FAnimationPose& Pose) const
{
	for (std::size_t Index = 0; Index < Desc.PoseOperations.size(); ++Index)
	{
		const FAnimationPoseOperationDesc& Operation = Desc.PoseOperations[Index];
		const float Angle = Operation.Scale * Value(Operation.Parameter, 1.f);
		if (Angle != 0.f)
		{
			const FVector3 Axis = FVector3{Operation.Axis[0], Operation.Axis[1], Operation.Axis[2]}.Normalized();
			Pose.RotateJoint(PoseJoints[Index], FQuaternion::FromAxisAngle(Axis, Angle));
		}
	}
}

void FAnimationStateMachine::SumTreeDuration(const std::size_t State, const std::size_t Node, const float Weight, float& Weights, float& Seconds) const noexcept
{
	const FAnimationBlendNodeDesc& Tree = Desc.States[State].Tree[Node];
	switch (Tree.Type)
	{
		case EAnimationBlendNodeType::Clip:
			Weights += Weight;
			Seconds += Weight * Resolved[State].TreeDurations[Node];
			return;
		case EAnimationBlendNodeType::Blend1D:
		{
			const FBlendPoint Blend = FindBlend(Tree.Children, &FAnimationBlendChild::X, Value(Tree.ParameterX, 0.f));
			SumTreeDuration(State, Tree.Children[Blend.Low].Node, Weight * (1.f - Blend.Alpha), Weights, Seconds);
			if (Blend.High != Blend.Low)
			{
				SumTreeDuration(State, Tree.Children[Blend.High].Node, Weight * Blend.Alpha, Weights, Seconds);
			}

			return;
		}
		case EAnimationBlendNodeType::Blend2D:
			for (const FAnimationBlendChild& Child : Tree.Children)
			{
				SumTreeDuration(State, Child.Node, Weight / static_cast<float>(Tree.Children.size()), Weights, Seconds);
			}

			return;
		case EAnimationBlendNodeType::Additive:
		case EAnimationBlendNodeType::Layered:
			SumTreeDuration(State, Tree.Children[0].Node, Weight, Weights, Seconds);
			return;
	}
}

void FAnimationStateMachine::AppendTree(const std::size_t State, const std::size_t Node, const float Weight, const FLayerScope& Scope, const float Phase)
{
	if (Weight <= 1.e-4f)
	{
		return;
	}

	const FAnimationBlendNodeDesc& Tree = Desc.States[State].Tree[Node];
	switch (Tree.Type)
	{
		case EAnimationBlendNodeType::Clip:
			if (Layers.size() < FAnimationPose::MaximumLayers)
			{
				Layers.push_back({.Clip = Resolved[State].TreeClips[Node], .TimeSeconds = Phase * Resolved[State].TreeDurations[Node], .Weight = Weight, .UpperBodyWeight = Scope.UpperBody, .LowerBodyWeight = Scope.LowerBody, .bAdditive = Scope.bAdditive, .MaskJoint = Scope.MaskJoint, .InsideMask = Scope.InsideMask, .OutsideMask = Scope.OutsideMask});
			}

			return;
		case EAnimationBlendNodeType::Blend1D:
		{
			const FBlendPoint Blend = FindBlend(Tree.Children, &FAnimationBlendChild::X, Value(Tree.ParameterX, 0.f));
			AppendTree(State, Tree.Children[Blend.Low].Node, Weight * (1.f - Blend.Alpha), Scope, Phase);
			if (Blend.High != Blend.Low)
			{
				AppendTree(State, Tree.Children[Blend.High].Node, Weight * Blend.Alpha, Scope, Phase);
			}

			return;
		}
		case EAnimationBlendNodeType::Blend2D:
		{
			// Inverse squared distance: exact at each point and smooth between them.
			const float X = Value(Tree.ParameterX, 0.f);
			const float Y = Value(Tree.ParameterY, 0.f);
			std::array<float, FAnimationPose::MaximumLayers> Shares{};
			const std::size_t Count = std::min(Tree.Children.size(), Shares.size());
			float Total = 0.f;
			for (std::size_t Child = 0; Child < Count; ++Child)
			{
				const float Distance = (Tree.Children[Child].X - X) * (Tree.Children[Child].X - X) + (Tree.Children[Child].Y - Y) * (Tree.Children[Child].Y - Y);
				Shares[Child] = Distance < 1.e-8f ? 1.e8f : 1.f / Distance;
				Total += Shares[Child];
			}

			for (std::size_t Child = 0; Child < Count; ++Child)
			{
				AppendTree(State, Tree.Children[Child].Node, Weight * Shares[Child] / Total, Scope, Phase);
			}

			return;
		}
		case EAnimationBlendNodeType::Additive:
		{
			AppendTree(State, Tree.Children[0].Node, Weight, Scope, Phase);
			FLayerScope Added = Scope;
			Added.bAdditive = true;
			AppendTree(State, Tree.Children[1].Node, Weight * std::max(Tree.Weight * Value(Tree.ParameterX, 1.f), 0.f), Added, Phase);
			return;
		}
		case EAnimationBlendNodeType::Layered:
		{
			// Inside the mask the layer takes Share of the joints from the base; outside it the base keeps them.
			const float Share = std::clamp(Tree.Weight * Value(Tree.ParameterX, 1.f), 0.f, 1.f);
			FLayerScope Under = Scope;
			Under.MaskJoint = Resolved[State].TreeMasks[Node];
			Under.InsideMask = 1.f - Share;
			Under.OutsideMask = 1.f;
			AppendTree(State, Tree.Children[0].Node, Weight, Under, Phase);
			FLayerScope Over = Scope;
			Over.MaskJoint = Under.MaskJoint;
			Over.InsideMask = Share;
			Over.OutsideMask = 0.f;
			AppendTree(State, Tree.Children[1].Node, Weight, Over, Phase);
			return;
		}
	}
}

std::optional<std::size_t> FAnimationStateMachine::FindParameter(const std::string_view Name) const noexcept
{
	const auto Found = std::ranges::find(Desc.Parameters, Name, &FAnimationParameterDesc::Name);
	return Found == Desc.Parameters.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Desc.Parameters.begin()));
}

void FAnimationStateMachine::SetFloat(const std::size_t Parameter, const float Value) noexcept
{
	if (Parameter < Values.size() && std::isfinite(Value))
	{
		Values[Parameter] = Value;
	}

	for (FAnimationStateMachine& Nested : SubMachines)
	{
		Nested.SetFloat(Parameter, Value);
	}
}

void FAnimationStateMachine::SetBool(const std::size_t Parameter, const bool bValue) noexcept
{
	if (Parameter < Values.size())
	{
		Values[Parameter] = bValue ? 1.f : 0.f;
	}

	for (FAnimationStateMachine& Nested : SubMachines)
	{
		Nested.SetBool(Parameter, bValue);
	}
}

void FAnimationStateMachine::SetTrigger(const std::size_t Parameter) noexcept
{
	if (Parameter < Triggers.size())
	{
		Triggers[Parameter] = true;
	}

	for (FAnimationStateMachine& Nested : SubMachines)
	{
		Nested.SetTrigger(Parameter);
	}
}

bool FAnimationStateMachine::Holds(const FAnimationConditionDesc& Condition) const noexcept
{
	const float Value = Values[Condition.Parameter];
	switch (Condition.Test)
	{
		case EAnimationCondition::Greater:
			return Value > Condition.Value;
		case EAnimationCondition::Less:
			return Value < Condition.Value;
		case EAnimationCondition::IsTrue:
			return Value != 0.f;
		case EAnimationCondition::IsFalse:
			return Value == 0.f;
		case EAnimationCondition::Triggered:
			return Triggers[Condition.Parameter];
	}

	return false;
}

float FAnimationStateMachine::Advance(FPlayback& Playback, const float DeltaSeconds) const noexcept
{
	const FAnimationStateDesc& State = Desc.States[Playback.State];
	const FResolvedState& Clips = Resolved[Playback.State];
	// A blend advances at the weighted duration of the clips it mixes, so both finish a cycle together. A nested machine keeps its own time,
	// so its state only cycles once a second.
	float Duration = 1.f;
	if (!State.Tree.empty())
	{
		float Weights = 0.f;
		float Seconds = 0.f;
		SumTreeDuration(Playback.State, 0, 1.f, Weights, Seconds);
		Duration = Weights > 0.f ? Seconds / Weights : 1.f;
	}
	else if (!State.Samples.empty())
	{
		const FBlendPoint Blend = State.BlendParameter ? FindBlend(State.Samples, Values[*State.BlendParameter]) : FBlendPoint{};
		Duration = Clips.Durations[Blend.Low] * (1.f - Blend.Alpha) + Clips.Durations[Blend.High] * Blend.Alpha;
	}

	Duration = std::max(Duration, 1.e-3f);
	const float Phase = Playback.Phase + DeltaSeconds * State.Speed / Duration;
	if (State.bLoop)
	{
		Playback.Phase = Phase - std::floor(Phase);
	}
	else
	{
		Playback.Phase = std::min(Phase, 1.f);
		Playback.bFinished = Phase >= 1.f;
	}

	return Phase;
}

void FAnimationStateMachine::AppendLayers(const FPlayback& Playback, const float Weight, const float UpperBodyWeight, const float LowerBodyWeight)
{
	const FAnimationStateDesc& State = Desc.States[Playback.State];
	const FResolvedState& Clips = Resolved[Playback.State];
	const float Phase = State.bReverse ? 1.f - Playback.Phase : Playback.Phase;
	if (State.SubMachine)
	{
		for (const FAnimationLayer& Nested : SubMachines[*State.SubMachine].GetLayers())
		{
			if (Layers.size() < FAnimationPose::MaximumLayers)
			{
				FAnimationLayer& Layer = Layers.emplace_back(Nested);
				Layer.Weight *= Weight;
				Layer.UpperBodyWeight *= UpperBodyWeight;
				Layer.LowerBodyWeight *= LowerBodyWeight;
			}
		}

		return;
	}

	if (!State.Tree.empty())
	{
		AppendTree(Playback.State, 0, Weight, {.UpperBody = UpperBodyWeight, .LowerBody = LowerBodyWeight}, Phase);
		return;
	}

	const FBlendPoint Blend = State.BlendParameter ? FindBlend(State.Samples, Values[*State.BlendParameter]) : FBlendPoint{};
	const auto Add = [&](const std::size_t Sample, const float SampleWeight)
	{
		if (SampleWeight > 0.f && Layers.size() < FAnimationPose::MaximumLayers)
		{
			Layers.push_back({.Clip = Clips.Clips[Sample], .TimeSeconds = Phase * Clips.Durations[Sample], .Weight = SampleWeight, .UpperBodyWeight = UpperBodyWeight, .LowerBodyWeight = LowerBodyWeight});
		}
	};

	Add(Blend.Low, Weight * (1.f - Blend.Alpha));
	if (Blend.High != Blend.Low)
	{
		Add(Blend.High, Weight * Blend.Alpha);
	}
}

// The first transition in authored order that can fire wins, whether it leaves the active state or any state, so the order is the priority.
// An active fade may be interrupted.
std::optional<std::size_t> FAnimationStateMachine::FindTransition() const noexcept
{
	for (std::size_t Index = 0; Index < Desc.Transitions.size(); ++Index)
	{
		const FAnimationTransitionDesc& Candidate = Desc.Transitions[Index];
		const bool bFrom = Candidate.From == AnyAnimationState ? Candidate.To != Current.State : Candidate.From == Current.State;
		const bool bReady = !Candidate.bAtEnd || Current.bFinished;
		const bool bHolds = std::ranges::all_of(Candidate.Conditions, [this](const FAnimationConditionDesc& Condition)
		{
			return Holds(Condition);
		});

		if (bFrom && bReady && bHolds)
		{
			return Index;
		}
	}

	return std::nullopt;
}

void FAnimationStateMachine::Update(const float DeltaSeconds)
{
	const float Step = std::isfinite(DeltaSeconds) ? std::max(DeltaSeconds, 0.f) : 0.f;
	const float Start = Current.Phase;
	const bool bFirst = !Current.bStarted;
	const float End = Advance(Current, Step);
	Events.clear();
	// A paused update plays nothing, so it leaves the state's start for the first update that does.
	const FAnimationStateDesc& Playing = Desc.States[Current.State];
	for (const FAnimationEventDesc& Event : Step > 0.f ? std::span<const FAnimationEventDesc>(Playing.Events) : std::span<const FAnimationEventDesc>{})
	{
		// A loop reaches the event at Phase plus a whole number of cycles; the last such point this update decides, so a long step fires it once.
		const float Reached = Playing.bLoop ? Event.Phase + std::floor(End - Event.Phase) : Event.Phase;
		if (Reached <= End && (bFirst ? Reached >= Start : Reached > Start))
		{
			Events.push_back(Event.Name);
		}
	}

	Current.bStarted |= Step > 0.f;
	if (!Desc.States[Current.State].bUpperBody)
	{
		Base = Current;
	}
	else
	{
		Advance(Base, Step);
	}

	if (Previous)
	{
		Advance(*Previous, Step);
		TransitionElapsed += Step;
		const float Duration = Desc.Transitions[*Transition].DurationSeconds;
		if (TransitionElapsed >= Duration)
		{
			Previous.reset();
			Transition.reset();
		}
	}

	if (const auto Taken = FindTransition())
	{
		const FAnimationTransitionDesc& Chosen = Desc.Transitions[*Taken];
		Previous = Chosen.DurationSeconds > 0.f ? std::optional(Current) : std::nullopt;
		Transition = Chosen.DurationSeconds > 0.f ? Taken : std::nullopt;
		TransitionElapsed = 0.f;
		// Returning to the state the legs kept playing picks up their cycle where it is.
		Current = {.State = Chosen.To, .Phase = Chosen.To == Base.State && Desc.States[Base.State].bLoop ? Base.Phase : 0.f};
		if (const auto Nested = Desc.States[Current.State].SubMachine; Nested && !(Previous && Desc.States[Previous->State].SubMachine == Nested))
		{
			SubMachines[*Nested].Restart();
		}
	}

	// Each nested machine that is playing steps once, even when two playbacks share it; the rest drop their triggers.
	std::array<bool, 32> Stepped{};
	for (const FPlayback* Playback : {&Current, Previous ? &*Previous : nullptr, &Base})
	{
		const auto Nested = Playback ? Desc.States[Playback->State].SubMachine : std::nullopt;
		if (Nested && *Nested < Stepped.size() && !Stepped[*Nested])
		{
			Stepped[*Nested] = true;
			SubMachines[*Nested].Update(Step);
		}
	}

	for (std::size_t Nested = 0; Nested < SubMachines.size(); ++Nested)
	{
		if (Nested >= Stepped.size() || !Stepped[Nested])
		{
			SubMachines[Nested].ClearTriggers();
		}
	}

	if (const auto Nested = Desc.States[Current.State].SubMachine)
	{
		const FAnimationStateMachine& NestedMachine = SubMachines[*Nested];
		std::ranges::copy(NestedMachine.GetEvents(), std::back_inserter(Events));
		Current.bFinished = NestedMachine.Current.bFinished;
	}

	std::ranges::fill(Triggers, false);
	Layers.clear();
	const float Alpha = Previous ? std::clamp(TransitionElapsed / Desc.Transitions[*Transition].DurationSeconds, 0.f, 1.f) : 1.f;
	// Upper-body states keep the whole body while standing and give the legs to the base as the character moves.
	const float Motion = GetBaseMotion();
	float UpperBody = 0.f;
	for (const auto& [Playback, Weight] : {std::pair{&Current, Alpha}, std::pair{Previous ? &*Previous : nullptr, 1.f - Alpha}})
	{
		if (!Playback)
		{
			continue;
		}

		const bool bUpper = Desc.States[Playback->State].bUpperBody;
		AppendLayers(*Playback, Weight, 1.f, bUpper ? 1.f - Motion : 1.f);
		UpperBody += bUpper ? Weight : 0.f;
	}

	if (UpperBody > 0.f && Motion > 0.f)
	{
		AppendLayers(Base, UpperBody * Motion, 0.f, 1.f);
	}
}

float FAnimationStateMachine::GetBaseMotion() const noexcept
{
	const FAnimationStateDesc& Legs = Desc.States[Base.State];
	if (!Legs.Tree.empty())
	{
		const FAnimationBlendNodeDesc& Root = Legs.Tree.front();
		if (Root.Type != EAnimationBlendNodeType::Blend1D || Root.Children.size() < 2 || Root.Children[1].X <= Root.Children[0].X)
		{
			return 0.f;
		}

		return std::clamp((Value(Root.ParameterX, 0.f) - Root.Children[0].X) / (Root.Children[1].X - Root.Children[0].X), 0.f, 1.f);
	}

	if (!Legs.BlendParameter || Legs.Samples.size() < 2 || Legs.Samples[1].Position <= Legs.Samples[0].Position)
	{
		return 0.f;
	}

	return std::clamp((Values[*Legs.BlendParameter] - Legs.Samples[0].Position) / (Legs.Samples[1].Position - Legs.Samples[0].Position), 0.f, 1.f);
}

std::span<const std::string> FAnimationStateMachine::GetEvents() const noexcept
{
	return Events;
}

std::span<const FAnimationLayer> FAnimationStateMachine::GetLayers() const noexcept
{
	return Layers;
}

float FAnimationStateMachine::GetActivePhase() const noexcept
{
	return Current.Phase;
}

std::size_t FAnimationStateMachine::GetActiveState() const noexcept
{
	return Current.State;
}

std::optional<std::size_t> FAnimationStateMachine::GetActiveTransition() const noexcept
{
	return Transition;
}

const FAnimationStateMachineDesc& FAnimationStateMachine::GetDesc() const noexcept
{
	return Desc;
}
}
