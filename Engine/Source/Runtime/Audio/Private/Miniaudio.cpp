#include "AudioDevice.h"
#include "Herta/Assets/Sound.h"
#include "Herta/Audio/SoundDecoder.h"

#include <algorithm>
#include <format>

// Herta mixes and decodes itself; only devices and decoders are compiled in.
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
// MSVC still reports external level 3 warnings from the implementation, which miniaudio's own conversions trip.
#ifdef _MSC_VER
	#pragma warning(push)
	#pragma warning(disable : 4244)
#endif
#include <miniaudio.h>
#ifdef _MSC_VER
	#pragma warning(pop)
#endif

namespace Herta
{
struct FAudioDevice::FState
{
	ma_device Device{};
	FCallback Callback = nullptr;
	void* User = nullptr;
	std::string Name{};
	bool bInitialized = false;
};

namespace
{
void DataCallback(ma_device* const Device, void* const Output, const void*, const ma_uint32 Frames)
{
	const auto* const State = static_cast<const FAudioDevice::FState*>(Device->pUserData);
	State->Callback(State->User, static_cast<float*>(Output), Frames);
}
}

std::expected<std::unique_ptr<FAudioDevice>, FAudioError> FAudioDevice::Open(const std::uint32_t SampleRate, const FCallback Callback, void* const User)
{
	auto State = std::make_unique<FState>();
	State->Callback = Callback;
	State->User = User;

	ma_device_config Config = ma_device_config_init(ma_device_type_playback);
	Config.playback.format = ma_format_f32;
	Config.playback.channels = 2;
	Config.sampleRate = SampleRate;
	Config.dataCallback = DataCallback;
	Config.pUserData = State.get();
	// The mixer writes every frame and clamps its own output.
	Config.noPreSilencedOutputBuffer = MA_TRUE;
	Config.noClip = MA_TRUE;

	if (const ma_result Result = ma_device_init(nullptr, &Config, &State->Device); Result != MA_SUCCESS)
	{
		return std::unexpected(FAudioError{std::format("Cannot open the default audio device: {}", ma_result_description(Result))});
	}

	State->bInitialized = true;
	State->Name = State->Device.playback.name;
	if (const ma_result Result = ma_device_start(&State->Device); Result != MA_SUCCESS)
	{
		return std::unexpected(FAudioError{std::format("Cannot start the audio device: {}", ma_result_description(Result))});
	}

	return std::unique_ptr<FAudioDevice>(new FAudioDevice(std::move(State)));
}

FAudioDevice::FAudioDevice(std::unique_ptr<FState> InState)
    : State(std::move(InState))
{
}

FAudioDevice::~FAudioDevice()
{
	if (State && State->bInitialized)
	{
		ma_device_uninit(&State->Device);
	}
}

const std::string& FAudioDevice::GetName() const noexcept
{
	return State->Name;
}

std::expected<FDecodedSound, FAudioError> DecodeSound(const std::span<const std::byte> Encoded)
{
	if (Encoded.empty())
	{
		return std::unexpected(FAudioError{"The sound file is empty"});
	}

	const auto Open = [&Encoded](ma_decoder& Decoder, const ma_uint32 Channels)
	{
		ma_decoder_config Config = ma_decoder_config_init(ma_format_s16, Channels, 0);
		return ma_decoder_init_memory(Encoded.data(), Encoded.size(), &Config, &Decoder);
	};

	ma_decoder Decoder{};
	if (const ma_result Result = Open(Decoder, 0); Result != MA_SUCCESS)
	{
		return std::unexpected(FAudioError{std::format("Cannot decode the sound: {}", ma_result_description(Result))});
	}

	// More than two channels mix down to stereo through the decoder's own channel converter.
	if (Decoder.outputChannels > MaximumSoundChannels)
	{
		ma_decoder_uninit(&Decoder);
		if (const ma_result Result = Open(Decoder, MaximumSoundChannels); Result != MA_SUCCESS)
		{
			return std::unexpected(FAudioError{std::format("Cannot decode the sound: {}", ma_result_description(Result))});
		}
	}

	FDecodedSound Sound{.SampleRate = Decoder.outputSampleRate, .Channels = Decoder.outputChannels};
	if (Sound.Channels == 0 || Sound.SampleRate < MinimumSoundSampleRate || Sound.SampleRate > MaximumSoundSampleRate)
	{
		ma_decoder_uninit(&Decoder);
		return std::unexpected(FAudioError{std::format("Unsupported sound format: {} channels at {} Hz", Sound.Channels, Sound.SampleRate)});
	}

	// Some formats do not know their length up front, so frames are read in chunks up to the cooked size limit.
	constexpr ma_uint64 ChunkFrames = 16384;
	ma_result Result = MA_SUCCESS;
	while (Result == MA_SUCCESS)
	{
		const std::size_t Start = Sound.Samples.size();
		if (Start > MaximumCookedSoundSamples)
		{
			break;
		}

		Sound.Samples.resize(Start + ChunkFrames * Sound.Channels);
		ma_uint64 Read = 0;
		Result = ma_decoder_read_pcm_frames(&Decoder, Sound.Samples.data() + Start, ChunkFrames, &Read);
		Sound.Samples.resize(Start + static_cast<std::size_t>(Read) * Sound.Channels);
	}

	ma_decoder_uninit(&Decoder);
	if (Result != MA_SUCCESS && Result != MA_AT_END)
	{
		return std::unexpected(FAudioError{std::format("Cannot decode the sound: {}", ma_result_description(Result))});
	}

	if (Sound.Samples.empty() || Sound.Samples.size() > MaximumCookedSoundSamples)
	{
		return std::unexpected(FAudioError{Sound.Samples.empty() ? "The sound has no samples" : "The sound is longer than ten minutes of 48 kHz stereo"});
	}

	return Sound;
}
}
