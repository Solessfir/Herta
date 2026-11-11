#include "Herta/Animation/AnimationStateMachine.h"

#include <algorithm>
#include <cmath>
#include <format>

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

[[nodiscard]] FBlendPoint FindBlend(const std::vector<FAnimationBlendSample>& Samples, const float Position) noexcept
{
	if (Samples.size() == 1 || Position <= Samples.front().Position)
	{
		return {};
	}

	if (Position >= Samples.back().Position)
	{
		return {.Low = Samples.size() - 1, .High = Samples.size() - 1, .Alpha = 0.f};
	}

	const auto Upper = std::ranges::upper_bound(Samples, Position, {}, &FAnimationBlendSample::Position);
	const auto High = static_cast<std::size_t>(Upper - Samples.begin());
	const float Span = Samples[High].Position - Samples[High - 1].Position;
	return {.Low = High - 1, .High = High, .Alpha = Span > 0.f ? (Position - Samples[High - 1].Position) / Span : 0.f};
}
}

std::expected<FAnimationStateMachine, FAnimationError> FAnimationStateMachine::Create(FAnimationStateMachineDesc Desc, const FAnimationLibrary& Library)
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

	for (FAnimationStateDesc& State : Desc.States)
	{
		if (State.Samples.empty() || State.Samples.size() > MaximumSamplesPerState || !std::isfinite(State.Speed) || State.Speed < 0.f)
		{
			return Fail(std::format("State '{}' needs between 1 and {} clips and a nonnegative speed", State.Name, MaximumSamplesPerState));
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
}

void FAnimationStateMachine::SetBool(const std::size_t Parameter, const bool bValue) noexcept
{
	if (Parameter < Values.size())
	{
		Values[Parameter] = bValue ? 1.f : 0.f;
	}
}

void FAnimationStateMachine::SetTrigger(const std::size_t Parameter) noexcept
{
	if (Parameter < Triggers.size())
	{
		Triggers[Parameter] = true;
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
	const FBlendPoint Blend = State.BlendParameter ? FindBlend(State.Samples, Values[*State.BlendParameter]) : FBlendPoint{};
	// A blend advances at the weighted duration of the clips it mixes, so both finish a cycle together.
	const float Duration = std::max(Clips.Durations[Blend.Low] * (1.f - Blend.Alpha) + Clips.Durations[Blend.High] * Blend.Alpha, 1.e-3f);
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
	const FBlendPoint Blend = State.BlendParameter ? FindBlend(State.Samples, Values[*State.BlendParameter]) : FBlendPoint{};
	const float Phase = State.bReverse ? 1.f - Playback.Phase : Playback.Phase;
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
		Transition = Chosen.DurationSeconds > 0.f ? std::optional(*Taken) : std::nullopt;
		TransitionElapsed = 0.f;
		// Returning to the state the legs kept playing picks up their cycle where it is.
		Current = {.State = Chosen.To, .Phase = Chosen.To == Base.State && Desc.States[Base.State].bLoop ? Base.Phase : 0.f};
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
