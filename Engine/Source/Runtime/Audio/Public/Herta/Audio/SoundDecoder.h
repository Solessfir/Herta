#pragma once

#include "Herta/Audio/AudioCommands.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Herta
{
struct FDecodedSound
{
	std::uint32_t SampleRate = 0;
	std::uint32_t Channels = 0;
	// Interleaved signed 16-bit PCM.
	std::vector<std::int16_t> Samples{};
};

// Decodes WAV, FLAC, or MP3 at its own sample rate, mixing more than two channels down to stereo. Sources are untrusted, so the asset
// pipeline only calls this inside its worker.
[[nodiscard]] std::expected<FDecodedSound, FAudioError> DecodeSound(std::span<const std::byte> Encoded);
}
