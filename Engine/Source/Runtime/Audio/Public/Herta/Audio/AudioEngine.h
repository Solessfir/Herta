#pragma once

#include "Herta/Assets/Sound.h"
#include "Herta/Audio/AudioCommands.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
struct FAudioBusDesc
{
	std::string Name{};
	// Empty for the master bus, which must come first; every other parent must come before its children.
	std::string Parent{};
	float Volume = 1.f;
};

// Master, with Effects, Ambient, and Interface beneath it.
[[nodiscard]] std::vector<FAudioBusDesc> GetDefaultAudioBuses();

enum class EAudioDevice : std::uint8_t
{
	// The system's default output, or the null path when it cannot open.
	Default,
	// No device: Update renders elapsed time into nothing, and Render renders on demand, servicing streams on the calling thread so the
	// output is the same on every run.
	Null,
};

struct FAudioEngineDesc
{
	EAudioDevice Device = EAudioDevice::Default;
	std::vector<FAudioBusDesc> Buses = GetDefaultAudioBuses();
	std::uint32_t SampleRate = 48000;
};

inline constexpr std::uint32_t MaximumAudioVoices = 64;
inline constexpr std::uint32_t MaximumAudioStreams = 8;
inline constexpr std::uint32_t AudioBlockFrames = 256;

struct FAudioBusStats
{
	std::string Name{};
	float Volume = 1.f;
	// The loudest sample the bus carried since the previous Update.
	float Peak = 0.f;
};

struct FAudioStats
{
	bool bNullDevice = true;
	std::string Device{};
	// Why the default device fell back to the null path, if it did.
	std::string DeviceError{};
	std::uint32_t SampleRate = 0;
	std::uint32_t Sounds = 0;

	std::uint32_t Voices = 0;
	std::uint32_t AudibleVoices = 0;
	std::uint32_t Streams = 0;
	std::uint64_t VoicesStarted = 0;
	std::uint64_t VoicesStolen = 0;
	std::uint64_t VoicesRejected = 0;
	// Plays of sounds that were never added, or graphs whose waves are missing.
	std::uint64_t MissingSounds = 0;
	// Frames a playing stream had to skip because the streaming thread fell behind; opening a stream is not counted.
	std::uint64_t StreamStarvedFrames = 0;
	std::uint64_t CommandsSubmitted = 0;
	// Commands that found the queue to the mixer full; a nonzero count means gameplay issues more than a block can take.
	std::uint64_t CommandsDropped = 0;
	std::uint64_t Blocks = 0;
	// Mixer time per block against the block's duration, which is the real-time budget.
	float BlockMicrosecondsAverage = 0.f;
	float BlockMicrosecondsPeak = 0.f;
	float BlockBudgetMicroseconds = 0.f;
	std::vector<FAudioBusStats> Buses{};
};

// A play the game thread submitted, for tools that ask what sounded recently.
struct FAudioPlayRecord
{
	FAssetId Sound{};
	std::uint32_t Voice = 0;
	std::string Bus{};
	float Volume = 1.f;
	float Pitch = 1.f;
	bool bSpatial = false;
	FVector3 Position{};
	float SecondsAgo = 0.f;
	// Addressed voices until the mixer reports their end; others while a plain sound's length has not passed.
	bool bPlaying = false;
};

inline constexpr std::size_t MaximumAudioPlayRecords = 64;

// Plays cooked sounds and sound graphs. The game thread adds sounds and submits commands; the mixer runs on the device's real-time
// thread, takes commands through a fixed-size lock-free queue, and never allocates, locks, or touches files. Streamed sounds are read by
// a streaming thread into fixed ring buffers.
class FAudioEngine final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FAudioEngine>, FAudioError> Create(const FAudioEngineDesc& Desc = {});

	~FAudioEngine();

	FAudioEngine(const FAudioEngine&) = delete;
	FAudioEngine& operator=(const FAudioEngine&) = delete;
	FAudioEngine(FAudioEngine&&) = delete;
	FAudioEngine& operator=(FAudioEngine&&) = delete;

	// Replacing a sound keeps voices already playing the old one until they end.
	[[nodiscard]] std::expected<void, FAudioError> AddSound(const FAssetId& Id, std::shared_ptr<const FCookedSound> Sound);
	[[nodiscard]] std::expected<void, FAudioError> AddSoundStream(const FAssetId& Id, const FCookedSoundStream& Stream);
	// The graph's waves must be added too, before or after it; a graph plays once all of them are.
	[[nodiscard]] std::expected<void, FAudioError> AddSoundGraph(const FAssetId& Id, std::shared_ptr<const FCookedSoundGraph> Graph);
	[[nodiscard]] bool HasSound(const FAssetId& Id) const;
	void RemoveSound(const FAssetId& Id);

	void Submit(std::span<const FAudioCommand> Commands);
	void Submit(const FAudioCommand& Command);
	// Collects ended voices and statistics, and releases sounds nothing plays any longer. A null device also renders DeltaSeconds of audio.
	void Update(float DeltaSeconds);
	// Null device only: renders interleaved stereo frames on the calling thread.
	void Render(std::span<float> Interleaved);

	// As of the last Update.
	[[nodiscard]] bool IsVoicePlaying(FAudioVoiceId Voice) const;
	[[nodiscard]] FAudioStats GetStats() const;
	// The last MaximumAudioPlayRecords plays, newest first.
	[[nodiscard]] std::vector<FAudioPlayRecord> GetRecentPlays() const;

	struct FImplementation;

private:
	explicit FAudioEngine(std::unique_ptr<FImplementation> InImplementation);

	std::unique_ptr<FImplementation> Implementation;
};
}
