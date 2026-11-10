#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Herta
{
enum class ESoundConcurrency : std::uint8_t
{
	// A new instance over the limit stops the oldest one.
	StealOldest,
	// A new instance over the limit stops the quietest one, or does not start when it would be the quietest.
	StealQuietest,
	// A new instance over the limit does not start.
	Reject,
};

// How a sound plays unless a play request overrides it: the bus it mixes into, its volume, how many instances may sound at once, and how
// far a spatial instance carries. Both wave sounds and sound graphs carry one, authored in their metadata.
struct FSoundPlayback
{
	bool operator==(const FSoundPlayback&) const = default;

	std::string Bus = "Effects";
	// Linear gain.
	float Volume = 1.f;
	std::uint32_t MaximumInstances = 8;
	ESoundConcurrency Concurrency = ESoundConcurrency::StealOldest;
	// Meters. A spatial instance plays at full volume within the minimum distance and falls silent at the maximum.
	float MinimumDistance = 1.f;
	float MaximumDistance = 40.f;
};

inline constexpr std::uint32_t MaximumSoundChannels = 2;
inline constexpr std::uint32_t MinimumSoundSampleRate = 8000;
inline constexpr std::uint32_t MaximumSoundSampleRate = 192000;
// Ten minutes of 48 kHz stereo.
inline constexpr std::uint64_t MaximumCookedSoundSamples = std::uint64_t{48000} * 60 * 10 * 2;
inline constexpr std::uint32_t MaximumSoundInstances = 64;

// Interleaved signed 16-bit PCM at the source rate; the mixer resamples while it plays. A streamed sound keeps its samples on disk and the
// mixer reads them as it plays, which is why they come last in the cooked layout, uncompressed, at a fixed offset.
struct FCookedSound
{
	FSoundPlayback Playback{};
	std::uint32_t SampleRate = 48000;
	std::uint32_t Channels = 1;
	bool bLoop = false;
	bool bStream = false;
	std::vector<std::int16_t> Samples{};
};

// Where a streamed sound's samples are, so the mixer can read them from the file without loading the asset.
struct FCookedSoundStream
{
	FSoundPlayback Playback{};
	std::uint32_t SampleRate = 48000;
	std::uint32_t Channels = 1;
	bool bLoop = false;
	// Whether the sound was cooked to stream; the header reads the same either way.
	bool bStream = false;
	std::uint64_t Frames = 0;
	std::filesystem::path File{};
	// Bytes from the start of the file to the first sample.
	std::uint64_t Offset = 0;
};

enum class ESoundParameterType : std::uint8_t
{
	Float,
	Bool,
	Trigger,
};

struct FSoundParameterDesc
{
	bool operator==(const FSoundParameterDesc&) const = default;

	std::string Name{};
	ESoundParameterType Type = ESoundParameterType::Float;
	float Default = 0.f;
};

inline constexpr std::uint32_t NoSoundControl = UINT32_MAX;

// A control value: the slot it reads, a parameter or a Map Range result, or the constant when no slot feeds it.
struct FSoundControl
{
	bool operator==(const FSoundControl&) const = default;

	std::uint32_t Slot = NoSoundControl;
	float Constant = 0.f;
};

enum class ESoundNodeType : std::uint8_t
{
	// Plays one of its waves; several waves pick one at random each time it starts.
	WavePlayer,
	Mixer,
	Gain,
	LowPass,
	HighPass,
	// Remaps a control into Slot: clamps Input to the input range and maps it onto the output range.
	MapRange,
	Output,
};

// Nodes run in order, and every audio input names an earlier node. Pitch nodes compile into the playback rate of the wave players they
// carry, so they are not nodes here.
struct FSoundNodeDesc
{
	bool operator==(const FSoundNodeDesc&) const = default;

	ESoundNodeType Type = ESoundNodeType::Output;
	std::vector<std::uint32_t> Inputs{};
	// Gain: the gain. Filters: the cutoff in hertz. Map Range: its input.
	FSoundControl Control{};

	std::vector<FAssetId> Waves{};
	// A Bool or Trigger parameter that starts the player, or none to start with the sound. A Bool also stops it when it turns false.
	std::uint32_t Play = NoSoundControl;
	bool bLoop = false;
	// Random variation each time the player starts, as fractions of the pitch and gain.
	float PitchVariation = 0.f;
	float GainVariation = 0.f;
	// Pitch ratios multiplied into the playback rate, from the Pitch nodes between the player and the output.
	std::vector<FSoundControl> Pitches{};

	// Map Range: input minimum, input maximum, output minimum, output maximum.
	std::array<float, 4> Range{0.f, 1.f, 0.f, 1.f};
	std::uint32_t Slot = NoSoundControl;
};

inline constexpr std::size_t MaximumSoundGraphNodes = 64;
inline constexpr std::size_t MaximumSoundGraphParameters = 16;
inline constexpr std::size_t MaximumSoundGraphWaves = 32;

// A cooked Audio Graph. Control slots are the parameters first, in order, then one per Map Range node.
struct FSoundGraphDesc
{
	bool operator==(const FSoundGraphDesc&) const = default;

	std::vector<FSoundParameterDesc> Parameters{};
	std::vector<FSoundNodeDesc> Nodes{};
	std::uint32_t ControlSlots = 0;
};

struct FCookedSoundGraph
{
	FSoundPlayback Playback{};
	FSoundGraphDesc Graph{};
};

[[nodiscard]] std::expected<void, FAssetError> ValidateCookedSound(const FCookedSound& Sound);
[[nodiscard]] std::expected<void, FAssetError> ValidateSoundPlayback(const FSoundPlayback& Playback);
[[nodiscard]] std::expected<void, FAssetError> ValidateSoundGraph(const FSoundGraphDesc& Graph);
// The waves a graph plays, each once, in the order the nodes first name them.
[[nodiscard]] std::vector<FAssetId> GetSoundGraphWaves(const FSoundGraphDesc& Graph);

// Reads just the header of a staged or cached cooked sound. Offset is where the cooked asset starts in the file, such as past a cache entry header.
[[nodiscard]] std::expected<FCookedSoundStream, FAssetError> ReadCookedSoundStream(const std::filesystem::path& File, std::uint64_t Offset = 0);
}
