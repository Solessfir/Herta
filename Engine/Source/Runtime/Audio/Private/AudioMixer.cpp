#include "AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

namespace Herta
{
namespace
{
static_assert(std::endian::native == std::endian::little, "Cooked sound samples are little-endian and stream straight into the rings");

constexpr float SilentGain = 1e-5f;
// Stolen voices fade out this fast, which is short enough to free the slot quickly and long enough not to click.
constexpr float StealFadeSeconds = 0.01f;

[[nodiscard]] float ReadControl(const FVoice& Voice, const FSoundControl& Control)
{
	return Control.Slot == NoSoundControl ? Control.Constant : Voice.Controls[Control.Slot];
}

enum class EGate : std::uint8_t
{
	None,
	Open,
	Close,
};

// What a node's Play input asks of it this block: no Play opens once with the sound, a Trigger opens each time it fires, and a Bool opens when
// it turns on and closes when it turns off.
[[nodiscard]] EGate ReadGate(const FVoice& Voice, const FSoundNodeDesc& Node, FPlayerState& Player)
{
	if (Node.Play == NoSoundControl)
	{
		const bool bFirst = !Player.bStarted;
		Player.bStarted = true;
		return bFirst ? EGate::Open : EGate::None;
	}

	if (Voice.Sound->Graph.Parameters[Node.Play].Type == ESoundParameterType::Trigger)
	{
		return Voice.Fired[Node.Play] ? EGate::Open : EGate::None;
	}

	const bool bOn = Voice.Controls[Node.Play] != 0.f;
	const EGate Gate = bOn && !Player.bWasOn ? EGate::Open : !bOn && Player.bWasOn ? EGate::Close
	                                                                               : EGate::None;
	Player.bWasOn = bOn;
	return Gate;
}

[[nodiscard]] std::uint32_t DelayFrames(const FSoundNodeDesc& Node, const std::uint32_t SampleRate)
{
	return static_cast<std::uint32_t>(std::lround(static_cast<double>(Node.Delay) * SampleRate));
}

// Frames the block starts with before a delayed start sounds, counting the delay down.
[[nodiscard]] std::uint32_t TakeDelay(FPlayerState& Player)
{
	const std::uint32_t Skipped = std::min(Player.DelayFrames, AudioBlockFrames);
	Player.DelayFrames -= Skipped;
	return Skipped;
}

// Deterministic per voice, so null-device renders repeat exactly.
[[nodiscard]] float NextRandom(std::uint32_t& State)
{
	State ^= State << 13;
	State ^= State >> 17;
	State ^= State << 5;
	return static_cast<float>(State >> 8) / static_cast<float>(1u << 24) * 2.f - 1.f;
}

[[nodiscard]] float ToFloat(const std::int16_t Sample)
{
	return static_cast<float>(Sample) * (1.f / 32768.f);
}
}

FMixer::FMixer(const std::uint32_t InSampleRate, const std::span<const FAudioBusDesc> Buses, const bool bInSynchronousStreams)
    : SampleRate(InSampleRate)
    , bSynchronousStreams(bInSynchronousStreams)
    , Scratch(MaximumSoundGraphNodes * AudioBlockFrames * 2, 0.f)
{
	BusCount = std::min(Buses.size(), MaximumAudioBuses);
	for (std::size_t Index = 0; Index < BusCount; ++Index)
	{
		const auto Parent = std::ranges::find(Buses.begin(), Buses.begin() + static_cast<std::ptrdiff_t>(Index), Buses[Index].Parent, &FAudioBusDesc::Name);
		BusParents[Index] = Index == 0 ? 0 : static_cast<std::uint32_t>(Parent - Buses.begin());
		BusTargets[Index] = Buses[Index].Volume;
		BusCurrent[Index] = Buses[Index].Volume;
		BusVolumes[Index].store(Buses[Index].Volume, std::memory_order_relaxed);
	}

	for (FStreamSlot& Slot : Streams)
	{
		Slot.Ring = std::make_unique<std::int16_t[]>(std::size_t{StreamRingFrames} * MaximumSoundChannels);
	}
}

void FMixer::Render(float* Output, std::uint32_t Frames)
{
	while (Frames > 0)
	{
		if (BlockCursor == AudioBlockFrames)
		{
			RenderBlock();
			BlockCursor = 0;
		}

		const std::uint32_t Count = std::min(Frames, AudioBlockFrames - BlockCursor);
		std::memcpy(Output, Block.data() + std::size_t{BlockCursor} * 2, std::size_t{Count} * 2 * sizeof(float));
		Output += std::size_t{Count} * 2;
		Frames -= Count;
		BlockCursor += Count;
	}
}

void FMixer::RenderBlock()
{
	const auto Start = std::chrono::steady_clock::now();

	FMixerCommand Command;
	while (Commands.Pop(Command))
	{
		Execute(Command);
	}

	// Bus volumes ease toward their targets; parents multiply into their children.
	for (std::size_t Bus = 0; Bus < BusCount; ++Bus)
	{
		BusCurrent[Bus] += (BusTargets[Bus] - BusCurrent[Bus]) * 0.5f;
		BusGains[Bus] = BusCurrent[Bus] * (Bus == 0 ? 1.f : BusGains[BusParents[Bus]]);
		BlockPeaks[Bus] = 0.f;
	}

	Block.fill(0.f);
	std::uint32_t Active = 0;
	std::uint32_t Audible = 0;
	for (FVoice& Voice : Voices)
	{
		if (Voice.bActive)
		{
			RenderVoice(Voice);
			++Active;
			Audible += Voice.Audibility > SilentGain ? 1 : 0;
		}
	}

	for (float& Sample : Block)
	{
		Sample = std::clamp(Sample, -1.f, 1.f);
	}

	// A bus carries its children, so its peak is the loudest of theirs too.
	for (std::size_t Bus = BusCount; Bus-- > 1;)
	{
		BlockPeaks[BusParents[Bus]] = std::max(BlockPeaks[BusParents[Bus]], BlockPeaks[Bus]);
	}

	for (std::size_t Bus = 0; Bus < BusCount; ++Bus)
	{
		if (BlockPeaks[Bus] > BusPeaks[Bus].load(std::memory_order_relaxed))
		{
			BusPeaks[Bus].store(BlockPeaks[Bus], std::memory_order_relaxed);
		}
	}

	std::uint32_t Streaming = 0;
	for (const FStreamSlot& Slot : Streams)
	{
		Streaming += Slot.State.load(std::memory_order_relaxed) != EStreamState::Free ? 1 : 0;
	}

	ActiveVoices.store(Active, std::memory_order_relaxed);
	AudibleVoices.store(Audible, std::memory_order_relaxed);
	ActiveStreams.store(Streaming, std::memory_order_relaxed);
	Blocks.fetch_add(1, std::memory_order_relaxed);
	const auto Nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count());
	BlockNanoseconds.fetch_add(Nanoseconds, std::memory_order_relaxed);
	if (Nanoseconds > BlockNanosecondsPeak.load(std::memory_order_relaxed))
	{
		BlockNanosecondsPeak.store(Nanoseconds, std::memory_order_relaxed);
	}
}

void FMixer::Execute(const FMixerCommand& Command)
{
	const auto ForVoice = [this, &Command](auto&& Function)
	{
		for (FVoice& Voice : Voices)
		{
			if (Voice.bActive && Voice.Id == Command.Voice)
			{
				Function(Voice);
			}
		}
	};

	switch (Command.Type)
	{
		case EMixerCommandType::Play:
			Play(Command);
			break;
		case EMixerCommandType::Stop:
			if (Command.Voice != 0)
			{
				ForVoice([this, &Command](FVoice& Voice)
				{
					Voice.FadeStep = Command.Value > 0.f ? 1.f / (Command.Value * static_cast<float>(SampleRate)) : 1.f;
				});
			}

			break;
		case EMixerCommandType::Move:
			if (Command.Voice != 0)
			{
				ForVoice([&Command](FVoice& Voice)
				{
					Voice.Position = Command.Position;
				});
			}

			break;
		case EMixerCommandType::Parameter:
			if (Command.Voice != 0)
			{
				ForVoice([&Command](FVoice& Voice)
				{
					const auto& Parameters = Voice.Sound->Graph.Parameters;
					if (Command.Index < Parameters.size())
					{
						Voice.Controls[Command.Index] = Command.Value;
						Voice.Fired[Command.Index] = Voice.Fired[Command.Index] || Parameters[Command.Index].Type == ESoundParameterType::Trigger;
					}
				});
			}

			break;
		case EMixerCommandType::Listener:
			Listener = Command.Listener;
			break;
		case EMixerCommandType::BusVolume:
			if (Command.Index < BusCount)
			{
				BusTargets[Command.Index] = Command.Value;
				BusVolumes[Command.Index].store(Command.Value, std::memory_order_relaxed);
			}

			break;
		case EMixerCommandType::StopAll:
			for (FVoice& Voice : Voices)
			{
				if (Voice.bActive)
				{
					Voice.FadeStep = Command.Value > 0.f ? 1.f / (Command.Value * static_cast<float>(SampleRate)) : 1.f;
				}
			}

			break;
	}
}

float FMixer::Attenuation(const FVector3& Position, const float MinimumDistance, const float MaximumDistance) const
{
	// Inverse distance from the minimum, windowed so it reaches silence at the maximum instead of carrying forever.
	const float Distance = (Position - Listener.Position).Length();
	if (Distance >= MaximumDistance)
	{
		return 0.f;
	}

	const float Inverse = MinimumDistance / std::max(Distance, MinimumDistance);
	const float Window = std::clamp((MaximumDistance - Distance) / (MaximumDistance * 0.25f), 0.f, 1.f);
	return Inverse * Window;
}

float FMixer::EstimateAudibility(const FMixerSound& Sound, const float Volume, const bool bSpatial, const FVector3& Position, const float MinimumDistance, const float MaximumDistance) const
{
	const float Gain = Volume * Sound.Playback.Volume * BusGains[Sound.Bus];
	return bSpatial ? Gain * Attenuation(Position, MinimumDistance, MaximumDistance) : Gain;
}

void FMixer::Play(const FMixerCommand& Command)
{
	FMixerSound& Sound = *Command.Sound;
	const auto Reject = [this, &Command, &Sound]
	{
		VoicesRejected.fetch_add(1, std::memory_order_relaxed);
		Sound.Voices.fetch_sub(1, std::memory_order_acq_rel);
		if (IsAddressedVoice(Command.Voice))
		{
			(void)Events.Push({.Voice = Command.Voice});
		}
	};

	const float Audibility = EstimateAudibility(Sound, Command.Volume, Command.bSpatial, Command.Position, Command.MinimumDistance, Command.MaximumDistance);

	// The sound's own limit counts its voices that are not already fading out.
	std::uint32_t Instances = 0;
	FVoice* Oldest = nullptr;
	FVoice* Quietest = nullptr;
	for (FVoice& Voice : Voices)
	{
		if (Voice.bActive && Voice.Sound == &Sound && Voice.FadeStep == 0.f)
		{
			++Instances;
			Oldest = !Oldest || Voice.Sequence < Oldest->Sequence ? &Voice : Oldest;
			Quietest = !Quietest || Voice.Audibility < Quietest->Audibility ? &Voice : Quietest;
		}
	}

	if (Instances >= Sound.Playback.MaximumInstances)
	{
		FVoice* Victim = nullptr;
		switch (Sound.Playback.Concurrency)
		{
			case ESoundConcurrency::StealOldest:
				Victim = Oldest;
				break;
			case ESoundConcurrency::StealQuietest:
				Victim = Quietest && Quietest->Audibility <= Audibility ? Quietest : nullptr;
				break;
			case ESoundConcurrency::Reject:
				break;
		}

		if (!Victim)
		{
			Reject();
			return;
		}

		Victim->FadeStep = 1.f / (StealFadeSeconds * static_cast<float>(SampleRate));
		VoicesStolen.fetch_add(1, std::memory_order_relaxed);
	}

	const auto Free = std::ranges::find(Voices, false, &FVoice::bActive);
	FVoice* Slot = Free == Voices.end() ? nullptr : &*Free;
	if (!Slot)
	{
		// Every voice is busy: the quietest one, preferring those already fading, ends now so the new one can start.
		FVoice* Victim = nullptr;
		for (FVoice& Voice : Voices)
		{
			const bool bFading = Voice.FadeStep > 0.f;
			const bool bVictimFading = Victim && Victim->FadeStep > 0.f;
			if (!Victim || (bFading && !bVictimFading) || (bFading == bVictimFading && Voice.Audibility < Victim->Audibility))
			{
				Victim = &Voice;
			}
		}

		if (Victim->FadeStep == 0.f && Victim->Audibility > Audibility)
		{
			Reject();
			return;
		}

		End(*Victim);
		VoicesStolen.fetch_add(1, std::memory_order_relaxed);
		Slot = Victim;
	}

	FVoice& Voice = *Slot;
	Voice.bActive = true;
	Voice.Id = Command.Voice;
	Voice.Sound = &Sound;
	Voice.Sequence = ++Sequence;
	Voice.Volume = Command.Volume;
	Voice.Pitch = Command.Pitch;
	Voice.bSpatial = Command.bSpatial;
	Voice.Position = Command.Position;
	Voice.MinimumDistance = Command.MinimumDistance;
	Voice.MaximumDistance = Command.MaximumDistance;
	Voice.Random = (Command.Voice * 2654435761u) ^ static_cast<std::uint32_t>(Voice.Sequence * 40503u) ^ 0x9e3779b9u;
	Voice.Random = Voice.Random == 0 ? 1 : Voice.Random;
	Voice.Fired.fill(false);
	Voice.Controls.fill(0.f);
	for (std::size_t Index = 0; Index < Sound.Graph.Parameters.size(); ++Index)
	{
		Voice.Controls[Index] = Sound.Graph.Parameters[Index].Default;
	}

	Voice.Nodes.fill({});
	Voice.bFirstBlock = true;
	Voice.Left = 0.f;
	Voice.Right = 0.f;
	Voice.Fade = 1.f;
	Voice.FadeStep = 0.f;
	Voice.Audibility = Audibility;
	VoicesStarted.fetch_add(1, std::memory_order_relaxed);
}

void FMixer::End(FVoice& Voice)
{
	for (FNodeState& Node : Voice.Nodes)
	{
		StopPlayer(Node.Player);
	}

	Voice.bActive = false;
	Voice.Sound->Voices.fetch_sub(1, std::memory_order_acq_rel);
	if (IsAddressedVoice(Voice.Id))
	{
		(void)Events.Push({.Voice = Voice.Id});
	}

	Voice.Sound = nullptr;
}

void FMixer::StopPlayer(FPlayerState& Player)
{
	if (Player.Stream >= 0)
	{
		Streams[static_cast<std::size_t>(Player.Stream)].State.store(EStreamState::Closing, std::memory_order_release);
		Player.Stream = -1;
	}

	Player.bPlaying = false;
}

void FMixer::StartPlayer(FVoice& Voice, const FSoundNodeDesc& Node, const std::size_t NodeIndex)
{
	FPlayerState& Player = Voice.Nodes[NodeIndex].Player;
	StopPlayer(Player);

	const auto& Waves = Voice.Sound->NodeWaves[NodeIndex];
	const std::uint32_t Pick = Waves.size() > 1 ? static_cast<std::uint32_t>((NextRandom(Voice.Random) * 0.5f + 0.5f) * static_cast<float>(Waves.size())) : 0;
	Player.Wave = Waves[std::min<std::size_t>(Pick, Waves.size() - 1)];
	Player.Position = 0.;
	Player.Gain = 1.f + Node.GainVariation * NextRandom(Voice.Random);
	Player.PitchVariation = 1.f + Node.PitchVariation * NextRandom(Voice.Random);
	Player.bPlaying = true;
	Player.DelayFrames = DelayFrames(Node, SampleRate);

	if (Player.Wave->Stream)
	{
		// Without a free ring the player stays silent; the stream count in the statistics shows the limit was reached.
		const auto Free = std::ranges::find_if(Streams, [](const FStreamSlot& Slot)
		{
			return Slot.State.load(std::memory_order_acquire) == EStreamState::Free;
		});

		if (Free == Streams.end())
		{
			Player.bPlaying = false;
			VoicesRejected.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		Free->Wave = Player.Wave;
		Free->WriteFrame.store(0, std::memory_order_relaxed);
		Free->ReadFrame.store(0, std::memory_order_relaxed);
		Free->State.store(EStreamState::Opening, std::memory_order_release);
		Player.Stream = static_cast<std::int32_t>(Free - Streams.begin());
		if (bSynchronousStreams)
		{
			ServiceStream(*Free);
		}
	}
}

void FMixer::RenderPlayer(FVoice& Voice, const FSoundNodeDesc& Node, const std::size_t NodeIndex, float* const Output, const bool bVirtual)
{
	FPlayerState& Player = Voice.Nodes[NodeIndex].Player;
	const EGate Gate = ReadGate(Voice, Node, Player);
	if (Gate == EGate::Open)
	{
		StartPlayer(Voice, Node, NodeIndex);
	}
	else if (Gate == EGate::Close)
	{
		StopPlayer(Player);
	}

	std::fill_n(Output, AudioBlockFrames * 2, 0.f);
	if (!Player.bPlaying)
	{
		return;
	}

	const std::uint32_t Begin = TakeDelay(Player);
	if (Begin == AudioBlockFrames)
	{
		return;
	}

	const FMixerWave& Wave = *Player.Wave;
	float Pitch = Voice.Pitch * Player.PitchVariation;
	for (const FSoundControl& Control : Node.Pitches)
	{
		Pitch *= ReadControl(Voice, Control);
	}

	const double Step = static_cast<double>(Wave.SampleRate) / static_cast<double>(SampleRate) * std::clamp(Pitch, 0.01f, 16.f);
	const double Frames = static_cast<double>(Wave.Frames);
	const float Gain = Player.Gain;
	const bool bLoop = Node.bLoop;
	const bool bStereo = Wave.Channels == 2;

	if (Wave.Stream)
	{
		FStreamSlot& Slot = Streams[static_cast<std::size_t>(Player.Stream)];
		// Until the streaming thread opens the file the player waits silently; that startup is not starvation.
		if (Slot.State.load(std::memory_order_acquire) != EStreamState::Ready)
		{
			return;
		}

		const std::uint64_t Written = Slot.WriteFrame.load(std::memory_order_acquire);
		const std::int16_t* const Ring = Slot.Ring.get();
		std::uint64_t Starved = 0;
		for (std::uint32_t Frame = Begin; Frame < AudioBlockFrames; ++Frame)
		{
			const auto Index = static_cast<std::uint64_t>(Player.Position);
			const bool bLastFrame = !bLoop && Index + 1 >= Wave.Frames;
			if (Index >= Written || (!bLastFrame && Index + 1 >= Written))
			{
				++Starved;
				continue;
			}

			if (!bVirtual)
			{
				const auto Fraction = static_cast<float>(Player.Position - static_cast<double>(Index));
				const std::size_t A = (Index % StreamRingFrames) * Wave.Channels;
				const std::size_t B = ((Index + 1) % StreamRingFrames) * Wave.Channels;
				for (std::uint32_t Channel = 0; Channel < 2; ++Channel)
				{
					const std::uint32_t Source = bStereo ? Channel : 0;
					const float First = ToFloat(Ring[A + Source]);
					const float Second = bLastFrame ? 0.f : ToFloat(Ring[B + Source]);
					Output[Frame * 2 + Channel] = (First + (Second - First) * Fraction) * Gain;
				}
			}

			Player.Position += Step;
			if (!bLoop && Player.Position >= Frames)
			{
				StopPlayer(Player);
				break;
			}
		}

		if (Player.Stream >= 0)
		{
			Slot.ReadFrame.store(static_cast<std::uint64_t>(Player.Position), std::memory_order_release);
		}

		StarvedFrames.fetch_add(Starved, std::memory_order_relaxed);
		return;
	}

	if (bVirtual)
	{
		Player.Position += Step * (AudioBlockFrames - Begin);
		if (Player.Position >= Frames)
		{
			if (bLoop)
			{
				Player.Position = std::fmod(Player.Position, Frames);
			}
			else
			{
				StopPlayer(Player);
			}
		}

		return;
	}

	const std::int16_t* const Samples = Wave.Samples;
	for (std::uint32_t Frame = Begin; Frame < AudioBlockFrames; ++Frame)
	{
		const auto Index = static_cast<std::uint64_t>(Player.Position);
		const auto Fraction = static_cast<float>(Player.Position - static_cast<double>(Index));
		const std::uint64_t Next = Index + 1 < Wave.Frames ? Index + 1 : (bLoop ? 0 : UINT64_MAX);
		for (std::uint32_t Channel = 0; Channel < 2; ++Channel)
		{
			const std::uint32_t Source = bStereo ? Channel : 0;
			const float First = ToFloat(Samples[Index * Wave.Channels + Source]);
			const float Second = Next == UINT64_MAX ? 0.f : ToFloat(Samples[Next * Wave.Channels + Source]);
			Output[Frame * 2 + Channel] = (First + (Second - First) * Fraction) * Gain;
		}

		Player.Position += Step;
		if (Player.Position >= Frames)
		{
			if (!bLoop)
			{
				StopPlayer(Player);
				break;
			}

			Player.Position = std::fmod(Player.Position, Frames);
		}
	}
}

void FMixer::RenderOscillator(FVoice& Voice, const FSoundNodeDesc& Node, const std::size_t NodeIndex, float* const Output, const bool bVirtual)
{
	FPlayerState& Player = Voice.Nodes[NodeIndex].Player;
	const EGate Gate = ReadGate(Voice, Node, Player);
	if (Gate == EGate::Open)
	{
		Player.bPlaying = true;
		Player.Position = 0.;
		Player.DelayFrames = DelayFrames(Node, SampleRate);
		Player.Remaining = Node.Duration > 0.f ? std::llround(static_cast<double>(Node.Duration) * SampleRate) : -1;
	}
	else if (Gate == EGate::Close)
	{
		Player.bPlaying = false;
	}

	std::fill_n(Output, AudioBlockFrames * 2, 0.f);
	if (!Player.bPlaying)
	{
		return;
	}

	float Pitch = Voice.Pitch;
	for (const FSoundControl& Control : Node.Pitches)
	{
		Pitch *= ReadControl(Voice, Control);
	}

	// Phase runs from 0 to 1 per cycle, so every waveform reads the same clock.
	const double Step = std::clamp(static_cast<double>(ReadControl(Voice, Node.Control)) * Pitch, 0., SampleRate * 0.45) / SampleRate;
	for (std::uint32_t Frame = TakeDelay(Player); Frame < AudioBlockFrames; ++Frame)
	{
		if (Player.Remaining == 0)
		{
			Player.bPlaying = false;
			break;
		}

		const auto Phase = static_cast<float>(Player.Position);
		float Value = 0.f;
		switch (Node.Waveform)
		{
			case ESoundWaveform::Sine:
				Value = std::sin(Phase * 2.f * std::numbers::pi_v<float>);
				break;
			case ESoundWaveform::Saw:
				Value = Phase * 2.f - 1.f;
				break;
			case ESoundWaveform::Square:
				Value = Phase < 0.5f ? 1.f : -1.f;
				break;
			case ESoundWaveform::Triangle:
				Value = 1.f - 4.f * std::abs(Phase - 0.5f);
				break;
			case ESoundWaveform::Noise:
				Value = NextRandom(Voice.Random);
				break;
		}

		if (!bVirtual)
		{
			Output[Frame * 2] = Value;
			Output[Frame * 2 + 1] = Value;
		}

		Player.Position += Step;
		Player.Position -= std::floor(Player.Position);
		Player.Remaining -= Player.Remaining > 0 ? 1 : 0;
	}
}

void FMixer::RenderEnvelope(FVoice& Voice, const FSoundNodeDesc& Node, const std::size_t NodeIndex, const float* const Input, float* const Output, const bool bVirtual)
{
	FNodeState& State = Voice.Nodes[NodeIndex];
	const EGate Gate = ReadGate(Voice, Node, State.Player);
	if (Gate == EGate::Open)
	{
		State.Stage = EEnvelopeStage::Attack;
		State.Player.DelayFrames = DelayFrames(Node, SampleRate);
		State.bHeld = Node.Play != NoSoundControl && Voice.Sound->Graph.Parameters[Node.Play].Type == ESoundParameterType::Bool;
	}
	else if (Gate == EGate::Close && State.Stage != EEnvelopeStage::Idle && State.Stage != EEnvelopeStage::Done)
	{
		State.bHeld = false;
		if (State.Stage == EEnvelopeStage::Sustain || State.Stage == EEnvelopeStage::Attack || State.Stage == EEnvelopeStage::Decay)
		{
			State.Stage = EEnvelopeStage::Release;
			State.ReleaseStep = 0.f;
		}
	}

	const auto Rate = [this](const float Seconds)
	{
		return Seconds > 0.f ? 1.f / (Seconds * static_cast<float>(SampleRate)) : 1.f;
	};

	const float Sustain = Node.Range[2];
	const std::uint32_t Begin = TakeDelay(State.Player);
	for (std::uint32_t Frame = 0; Frame < AudioBlockFrames; ++Frame)
	{
		if (Frame >= Begin)
		{
			switch (State.Stage)
			{
				case EEnvelopeStage::Attack:
					State.Level = std::min(State.Level + Rate(Node.Range[0]), 1.f);
					State.Stage = State.Level >= 1.f ? EEnvelopeStage::Decay : State.Stage;
					break;
				case EEnvelopeStage::Decay:
					State.Level = std::max(State.Level - (1.f - Sustain) * Rate(Node.Range[1]), Sustain);
					State.Stage = State.Level <= Sustain ? (State.bHeld ? EEnvelopeStage::Sustain : EEnvelopeStage::Release) : State.Stage;
					break;
				case EEnvelopeStage::Release:
					// Linear from wherever the release began, so it takes its full time from any level.
					State.ReleaseStep = State.ReleaseStep > 0.f ? State.ReleaseStep : std::max(State.Level, 1.e-6f) * Rate(Node.Range[3]);
					State.Level = std::max(State.Level - State.ReleaseStep, 0.f);
					State.Stage = State.Level <= 0.f ? EEnvelopeStage::Done : State.Stage;
					break;
				default:
					break;
			}
		}

		if (!bVirtual)
		{
			Output[Frame * 2] = Input[Frame * 2] * State.Level;
			Output[Frame * 2 + 1] = Input[Frame * 2 + 1] * State.Level;
		}
	}
}

void FMixer::RenderVoice(FVoice& Voice)
{
	const FMixerSound& Sound = *Voice.Sound;
	const FSoundGraphDesc& Graph = Sound.Graph;

	// Gains come first so an inaudible voice can skip reading and filtering samples while it keeps time.
	const float Base = Voice.Volume * Sound.Playback.Volume * BusGains[Sound.Bus];
	float TargetLeft = Base;
	float TargetRight = Base;
	if (Voice.bSpatial)
	{
		const float Distance = (Voice.Position - Listener.Position).Length();
		const float Gain = Base * Attenuation(Voice.Position, Voice.MinimumDistance, Voice.MaximumDistance);
		// Left-Up-Forward is right-handed, so up cross forward points left. Equal-power panning keeps the level steady as a source circles.
		const FVector3 Left = Listener.Up.Cross(Listener.Forward);
		const float Pan = Distance > 1e-4f ? std::clamp(-(Voice.Position - Listener.Position).Dot(Left) / Distance, -1.f, 1.f) : 0.f;
		const float Angle = (Pan + 1.f) * std::numbers::pi_v<float> * 0.25f;
		TargetLeft = Gain * std::cos(Angle);
		TargetRight = Gain * std::sin(Angle);
	}

	if (Voice.bFirstBlock)
	{
		Voice.Left = TargetLeft;
		Voice.Right = TargetRight;
	}

	Voice.Audibility = std::max(TargetLeft, TargetRight) * Voice.Fade;
	const bool bVirtual = std::max({TargetLeft, TargetRight, Voice.Left, Voice.Right}) < SilentGain;

	const float* Final = nullptr;
	bool bIdle = true;
	for (std::size_t Index = 0; Index < Graph.Nodes.size(); ++Index)
	{
		const FSoundNodeDesc& Node = Graph.Nodes[Index];
		FNodeState& State = Voice.Nodes[Index];
		float* const Output = Scratch.data() + Index * AudioBlockFrames * 2;
		const float* const Input = Node.Inputs.empty() ? nullptr : Scratch.data() + Node.Inputs[0] * AudioBlockFrames * 2;

		switch (Node.Type)
		{
			case ESoundNodeType::WavePlayer:
				RenderPlayer(Voice, Node, Index, Output, bVirtual);
				bIdle = bIdle && !State.Player.bPlaying;
				break;
			case ESoundNodeType::Oscillator:
				RenderOscillator(Voice, Node, Index, Output, bVirtual);
				bIdle = bIdle && !State.Player.bPlaying;
				break;
			case ESoundNodeType::Envelope:
				RenderEnvelope(Voice, Node, Index, Input, Output, bVirtual);
				break;
			case ESoundNodeType::MapRange:
			{
				const float Value = ReadControl(Voice, Node.Control);
				const float Alpha = std::clamp((Value - Node.Range[0]) / (Node.Range[1] - Node.Range[0]), 0.f, 1.f);
				Voice.Controls[Node.Slot] = Node.Range[2] + (Node.Range[3] - Node.Range[2]) * Alpha;
				break;
			}
			case ESoundNodeType::Mixer:
				if (!bVirtual)
				{
					std::fill_n(Output, AudioBlockFrames * 2, 0.f);
					for (const std::uint32_t Source : Node.Inputs)
					{
						const float* const From = Scratch.data() + Source * AudioBlockFrames * 2;
						for (std::size_t Sample = 0; Sample < AudioBlockFrames * 2; ++Sample)
						{
							Output[Sample] += From[Sample];
						}
					}
				}

				break;
			case ESoundNodeType::Gain:
			{
				// Ramps across the block so a moving gain does not step.
				const float Gain = ReadControl(Voice, Node.Control);
				const float From = State.LastGain < 0.f ? Gain : State.LastGain;
				State.LastGain = Gain;
				if (!bVirtual)
				{
					const float Delta = (Gain - From) / static_cast<float>(AudioBlockFrames);
					for (std::uint32_t Frame = 0; Frame < AudioBlockFrames; ++Frame)
					{
						const float Current = From + Delta * static_cast<float>(Frame + 1);
						Output[Frame * 2] = Input[Frame * 2] * Current;
						Output[Frame * 2 + 1] = Input[Frame * 2 + 1] * Current;
					}
				}

				break;
			}
			case ESoundNodeType::LowPass:
			case ESoundNodeType::HighPass:
				if (!bVirtual)
				{
					// A topology-preserving state-variable filter, which stays stable while its cutoff moves.
					const float Cutoff = std::clamp(ReadControl(Voice, Node.Control), 20.f, static_cast<float>(SampleRate) * 0.45f);
					const float G = std::tan(std::numbers::pi_v<float> * Cutoff / static_cast<float>(SampleRate));
					constexpr float K = std::numbers::sqrt2_v<float>;
					const float A1 = 1.f / (1.f + G * (G + K));
					const float A2 = G * A1;
					const float A3 = G * A2;
					const bool bLowPass = Node.Type == ESoundNodeType::LowPass;
					for (std::uint32_t Frame = 0; Frame < AudioBlockFrames; ++Frame)
					{
						for (std::uint32_t Channel = 0; Channel < 2; ++Channel)
						{
							const float V0 = Input[Frame * 2 + Channel];
							const float V3 = V0 - State.Ic2[Channel];
							const float V1 = A1 * State.Ic1[Channel] + A2 * V3;
							const float V2 = State.Ic2[Channel] + A2 * State.Ic1[Channel] + A3 * V3;
							State.Ic1[Channel] = 2.f * V1 - State.Ic1[Channel];
							State.Ic2[Channel] = 2.f * V2 - State.Ic2[Channel];
							Output[Frame * 2 + Channel] = bLowPass ? V2 : V0 - K * V1 - V2;
						}
					}
				}

				break;
			case ESoundNodeType::Output:
				Final = Input;
				break;
		}
	}

	Voice.Fired.fill(false);
	Voice.bFirstBlock = false;

	// A sound that ends in an envelope also ends when that envelope has released, however long its sources would play.
	const FSoundNodeDesc& Last = Graph.Nodes.back();
	const bool bReleased = !Last.Inputs.empty() && Graph.Nodes[Last.Inputs[0]].Type == ESoundNodeType::Envelope && Voice.Nodes[Last.Inputs[0]].Stage == EEnvelopeStage::Done;
	bool bEnded = (bIdle || bReleased) && (!IsAddressedVoice(Voice.Id) || !Sound.bParameterDriven);
	if (!bVirtual && Final)
	{
		const float DeltaLeft = (TargetLeft - Voice.Left) / static_cast<float>(AudioBlockFrames);
		const float DeltaRight = (TargetRight - Voice.Right) / static_cast<float>(AudioBlockFrames);
		float Peak = 0.f;
		for (std::uint32_t Frame = 0; Frame < AudioBlockFrames; ++Frame)
		{
			const float Fade = Voice.Fade;
			Voice.Fade = std::max(Voice.Fade - Voice.FadeStep, 0.f);
			const float Left = (Voice.Left + DeltaLeft * static_cast<float>(Frame + 1)) * Fade;
			const float Right = (Voice.Right + DeltaRight * static_cast<float>(Frame + 1)) * Fade;
			float SourceLeft = Final[Frame * 2];
			float SourceRight = Final[Frame * 2 + 1];
			if (Voice.bSpatial)
			{
				SourceLeft = SourceRight = (SourceLeft + SourceRight) * 0.5f;
			}

			const float OutLeft = SourceLeft * Left;
			const float OutRight = SourceRight * Right;
			Block[Frame * 2] += OutLeft;
			Block[Frame * 2 + 1] += OutRight;
			Peak = std::max({Peak, std::abs(OutLeft), std::abs(OutRight)});
		}

		BlockPeaks[Sound.Bus] = std::max(BlockPeaks[Sound.Bus], Peak);
	}
	else
	{
		Voice.Fade = std::max(Voice.Fade - Voice.FadeStep * static_cast<float>(AudioBlockFrames), 0.f);
	}

	Voice.Left = TargetLeft;
	Voice.Right = TargetRight;
	bEnded = bEnded || (Voice.FadeStep > 0.f && Voice.Fade <= 0.f);
	if (bEnded)
	{
		End(Voice);
	}
}

bool FMixer::ServiceStreams()
{
	bool bBusy = false;
	for (FStreamSlot& Slot : Streams)
	{
		if (Slot.State.load(std::memory_order_acquire) != EStreamState::Free)
		{
			bBusy = true;
			ServiceStream(Slot);
		}
	}

	return bBusy;
}

void FMixer::ServiceStream(FStreamSlot& Slot)
{
	EStreamState State = Slot.State.load(std::memory_order_acquire);
	if (State == EStreamState::Free)
	{
		return;
	}

	if (State == EStreamState::Closing)
	{
		Slot.File.close();
		Slot.File.clear();
		Slot.FileFrame = UINT64_MAX;
		Slot.State.store(EStreamState::Free, std::memory_order_release);
		return;
	}

	const FMixerWave& Wave = *Slot.Wave;
	const FCookedSoundStream& Stream = *Wave.Stream;
	if (State == EStreamState::Opening)
	{
		Slot.File.close();
		Slot.File.clear();
		Slot.File.open(Stream.File, std::ios::binary);
		Slot.FileFrame = UINT64_MAX;
		if (!Slot.File)
		{
			// Without the file the stream starves, which the statistics report, until the mixer releases it.
			return;
		}

		// The mixer may have released the slot while it opened.
		if (!Slot.State.compare_exchange_strong(State, EStreamState::Ready, std::memory_order_acq_rel))
		{
			return;
		}
	}

	// Keep two frames free so the mixer's interpolation never reads a frame being written.
	const std::uint64_t Read = Slot.ReadFrame.load(std::memory_order_acquire);
	std::uint64_t Write = std::max(Slot.WriteFrame.load(std::memory_order_relaxed), Read);
	const std::uint64_t Limit = Stream.bLoop ? Read + StreamRingFrames - 2 : std::min<std::uint64_t>(Read + StreamRingFrames - 2, Stream.Frames);
	while (Write < Limit && Slot.File)
	{
		const std::uint64_t FileFrame = Write % Stream.Frames;
		const std::uint64_t RingIndex = Write % StreamRingFrames;
		const std::uint64_t Count = std::min({Limit - Write, Stream.Frames - FileFrame, StreamRingFrames - RingIndex, std::uint64_t{8192}});
		if (Slot.FileFrame != FileFrame)
		{
			Slot.File.seekg(static_cast<std::streamoff>(Stream.Offset + FileFrame * Stream.Channels * sizeof(std::int16_t)));
		}

		Slot.File.read(reinterpret_cast<char*>(Slot.Ring.get() + RingIndex * Stream.Channels), static_cast<std::streamsize>(Count * Stream.Channels * sizeof(std::int16_t)));
		Slot.FileFrame = FileFrame + Count;
		Write += Count;
		Slot.WriteFrame.store(Write, std::memory_order_release);
	}
}
}
