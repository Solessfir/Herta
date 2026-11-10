#pragma once

#include "Herta/Audio/AudioEngine.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
#ifdef _MSC_VER
// The padding the alignment adds is the point.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// One producer and one consumer, never blocking either. The mixer is the consumer of commands and the producer of events.
template <typename T, std::size_t Capacity>
class TSpscQueue final
{
	static_assert(std::has_single_bit(Capacity));

public:
	[[nodiscard]] bool Push(const T& Value);
	[[nodiscard]] bool Pop(T& Value);

private:
	// Separate cache lines keep the two threads from invalidating each other's index on every operation.
	alignas(128) std::atomic<std::size_t> WriteIndex{0};
	alignas(128) std::atomic<std::size_t> ReadIndex{0};
	std::array<T, Capacity> Items{};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

template <typename T, std::size_t Capacity>
bool TSpscQueue<T, Capacity>::Push(const T& Value)
{
	const std::size_t Write = WriteIndex.load(std::memory_order_relaxed);
	if (Write - ReadIndex.load(std::memory_order_acquire) == Capacity)
	{
		return false;
	}

	Items[Write % Capacity] = Value;
	WriteIndex.store(Write + 1, std::memory_order_release);
	return true;
}

template <typename T, std::size_t Capacity>
bool TSpscQueue<T, Capacity>::Pop(T& Value)
{
	const std::size_t Read = ReadIndex.load(std::memory_order_relaxed);
	if (Read == WriteIndex.load(std::memory_order_acquire))
	{
		return false;
	}

	Value = Items[Read % Capacity];
	ReadIndex.store(Read + 1, std::memory_order_release);
	return true;
}

struct FMixerWave
{
	const std::int16_t* Samples = nullptr;
	std::uint64_t Frames = 0;
	std::uint32_t Channels = 1;
	std::uint32_t SampleRate = 48000;
	const FCookedSoundStream* Stream = nullptr;
};

// Built on the game thread and immutable afterwards. The mixer reads it through a raw pointer while Voices counts the plays that may still
// reach it, so the game thread only releases it at zero. Plain sounds become one wave player feeding the output.
struct FMixerSound
{
	std::atomic<std::uint32_t> Voices{0};
	FSoundPlayback Playback{};
	std::uint32_t Bus = 0;
	FSoundGraphDesc Graph{};
	std::vector<std::vector<const FMixerWave*>> NodeWaves{};
	// Whether a wave player waits on a parameter; addressed voices of such graphs stay alive while idle.
	bool bParameterDriven = false;

	std::shared_ptr<const FCookedSound> Sound{};
	std::optional<FCookedSoundStream> Stream{};
	FMixerWave Wave{};
	std::shared_ptr<const FCookedSoundGraph> GraphAsset{};
	std::vector<std::shared_ptr<FMixerSound>> WaveSounds{};
};

enum class EMixerCommandType : std::uint8_t
{
	Play,
	Stop,
	Move,
	Parameter,
	Listener,
	BusVolume,
	StopAll,
};

struct FMixerCommand
{
	EMixerCommandType Type = EMixerCommandType::Play;
	std::uint32_t Voice = 0;
	FMixerSound* Sound = nullptr;
	float Volume = 1.f;
	float Pitch = 1.f;
	bool bSpatial = false;
	FVector3 Position{};
	float MinimumDistance = 1.f;
	float MaximumDistance = 40.f;
	// The parameter or bus.
	std::uint32_t Index = 0;
	// The parameter value, bus volume, or fade time.
	float Value = 0.f;
	FAudioListener Listener{};
};

// The mixer reports each addressed play once it ends or is rejected.
struct FMixerEvent
{
	std::uint32_t Voice = 0;
};

enum class EStreamState : std::uint8_t
{
	Free,
	Opening,
	Ready,
	Closing,
};

// A ring of a streamed wave's frames. The mixer claims a free slot and releases it; the streaming thread opens, fills, and closes it.
// Frame counters run past the end of looping waves, so the ring and both counters never wrap back.
struct FStreamSlot
{
	std::atomic<EStreamState> State{EStreamState::Free};
	const FMixerWave* Wave = nullptr;
	std::atomic<std::uint64_t> WriteFrame{0};
	std::atomic<std::uint64_t> ReadFrame{0};
	std::unique_ptr<std::int16_t[]> Ring{};

	// Streaming thread only.
	std::ifstream File{};
	std::uint64_t FileFrame = UINT64_MAX;
};

// Fire-and-forget plays that carry parameters get an identity with this bit so the parameters can reach them; they still end like
// unaddressed voices and report nothing back.
inline constexpr std::uint32_t PrivateVoiceBit = 0x80000000u;

[[nodiscard]] constexpr bool IsAddressedVoice(const std::uint32_t Voice)
{
	return Voice != 0 && (Voice & PrivateVoiceBit) == 0;
}

inline constexpr std::uint32_t StreamRingFrames = 1u << 16;
inline constexpr std::size_t MaximumAudioBuses = 16;
inline constexpr std::size_t MaximumSoundControls = MaximumSoundGraphParameters + MaximumSoundGraphNodes;

struct FPlayerState
{
	const FMixerWave* Wave = nullptr;
	double Position = 0.;
	float Gain = 1.f;
	float PitchVariation = 1.f;
	bool bPlaying = false;
	bool bWasOn = false;
	bool bStarted = false;
	std::int32_t Stream = -1;
};

struct FNodeState
{
	FPlayerState Player{};
	std::array<float, 2> Ic1{};
	std::array<float, 2> Ic2{};
	float LastGain = -1.f;
};

struct FVoice
{
	bool bActive = false;
	std::uint32_t Id = 0;
	FMixerSound* Sound = nullptr;
	std::uint64_t Sequence = 0;
	float Volume = 1.f;
	float Pitch = 1.f;
	bool bSpatial = false;
	FVector3 Position{};
	float MinimumDistance = 1.f;
	float MaximumDistance = 40.f;
	std::uint32_t Random = 1;

	std::array<float, MaximumSoundControls> Controls{};
	std::array<bool, MaximumSoundGraphParameters> Fired{};
	std::array<FNodeState, MaximumSoundGraphNodes> Nodes{};

	bool bFirstBlock = true;
	float Left = 0.f;
	float Right = 0.f;
	float Fade = 1.f;
	float FadeStep = 0.f;
	float Audibility = 0.f;
};

// Everything the real-time thread touches, allocated up front.
class FMixer final
{
public:
	// Synchronous streams open and fill on the mixer's own thread, which only a null device may do; its output then never depends on timing.
	FMixer(std::uint32_t InSampleRate, std::span<const FAudioBusDesc> Buses, bool bInSynchronousStreams);

	// Real-time safe: no allocation, locks, or file access.
	void Render(float* Output, std::uint32_t Frames);
	// Streaming thread, or the rendering thread of a null device. Returns whether any slot is in use.
	bool ServiceStreams();
	void ServiceStream(FStreamSlot& Slot);

	TSpscQueue<FMixerCommand, 4096> Commands;
	TSpscQueue<FMixerEvent, 1024> Events;
	std::array<FStreamSlot, MaximumAudioStreams> Streams;

	std::atomic<std::uint32_t> ActiveVoices{0};
	std::atomic<std::uint32_t> AudibleVoices{0};
	std::atomic<std::uint32_t> ActiveStreams{0};
	std::atomic<std::uint64_t> VoicesStarted{0};
	std::atomic<std::uint64_t> VoicesStolen{0};
	std::atomic<std::uint64_t> VoicesRejected{0};
	std::atomic<std::uint64_t> StarvedFrames{0};
	std::atomic<std::uint64_t> Blocks{0};
	std::atomic<std::uint64_t> BlockNanoseconds{0};
	std::atomic<std::uint64_t> BlockNanosecondsPeak{0};
	std::array<std::atomic<float>, MaximumAudioBuses> BusPeaks{};
	std::array<std::atomic<float>, MaximumAudioBuses> BusVolumes{};

private:
	void RenderBlock();
	void Execute(const FMixerCommand& Command);
	void Play(const FMixerCommand& Command);
	void End(FVoice& Voice);
	void RenderVoice(FVoice& Voice);
	void RenderPlayer(FVoice& Voice, const FSoundNodeDesc& Node, std::size_t NodeIndex, float* Output, bool bVirtual);
	void StartPlayer(FVoice& Voice, const FSoundNodeDesc& Node, std::size_t NodeIndex);
	void StopPlayer(FPlayerState& Player);
	[[nodiscard]] float EstimateAudibility(const FMixerSound& Sound, float Volume, bool bSpatial, const FVector3& Position, float MinimumDistance, float MaximumDistance) const;
	[[nodiscard]] float Attenuation(const FVector3& Position, float MinimumDistance, float MaximumDistance) const;

	std::uint32_t SampleRate = 48000;
	bool bSynchronousStreams = false;
	FAudioListener Listener{};
	std::uint64_t Sequence = 0;

	std::size_t BusCount = 0;
	std::array<std::uint32_t, MaximumAudioBuses> BusParents{};
	std::array<float, MaximumAudioBuses> BusTargets{};
	std::array<float, MaximumAudioBuses> BusCurrent{};
	std::array<float, MaximumAudioBuses> BusGains{};
	std::array<float, MaximumAudioBuses> BlockPeaks{};

	std::array<FVoice, MaximumAudioVoices> Voices{};
	// One stereo block per graph node, shared by every voice in turn.
	std::vector<float> Scratch;
	std::array<float, AudioBlockFrames * 2> Block{};
	std::uint32_t BlockCursor = AudioBlockFrames;
};
}
