#pragma once

#include "Herta/Audio/AudioCommands.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Herta
{
// The system's default playback device through miniaudio, rendering interleaved float stereo on its own real-time thread.
class FAudioDevice final
{
public:
	using FCallback = void (*)(void* User, float* Output, std::uint32_t Frames);

	[[nodiscard]] static std::expected<std::unique_ptr<FAudioDevice>, FAudioError> Open(std::uint32_t SampleRate, FCallback Callback, void* User);

	~FAudioDevice();

	FAudioDevice(const FAudioDevice&) = delete;
	FAudioDevice& operator=(const FAudioDevice&) = delete;
	FAudioDevice(FAudioDevice&&) = delete;
	FAudioDevice& operator=(FAudioDevice&&) = delete;

	[[nodiscard]] const std::string& GetName() const noexcept;

	struct FState;

private:
	explicit FAudioDevice(std::unique_ptr<FState> InState);

	std::unique_ptr<FState> State;
};
}
