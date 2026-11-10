#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Math/Vector.h"

#include <compare>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Herta
{
struct FAudioError
{
	std::string Message{};
};

// Chosen by whoever plays the sound, so a replay of the same gameplay issues the same identities. Zero plays a sound nobody addresses again;
// identities use the low 31 bits.
struct FAudioVoiceId
{
	auto operator<=>(const FAudioVoiceId&) const = default;

	std::uint32_t Value = 0;
};

struct FAudioListener
{
	bool operator==(const FAudioListener&) const = default;

	FVector3 Position{};
	FVector3 Forward{0.f, 0.f, 1.f};
	FVector3 Up{0.f, 1.f, 0.f};
};

struct FAudioParameterValue
{
	bool operator==(const FAudioParameterValue&) const = default;

	std::string Name{};
	// A Bool is on when nonzero; any value fires a Trigger.
	float Value = 0.f;
};

struct FAudioPlayCommand
{
	bool operator==(const FAudioPlayCommand&) const = default;

	FAudioVoiceId Voice{};
	FAssetId Sound{};
	float Volume = 1.f;
	float Pitch = 1.f;
	bool bSpatial = false;
	FVector3 Position{};
	// Zero keeps the sound's own distances.
	float MinimumDistance = 0.f;
	float MaximumDistance = 0.f;
	// Applied before the voice renders anything, so a Trigger here starts a graph's wave players on the first sample.
	std::vector<FAudioParameterValue> Parameters{};
};

struct FAudioStopCommand
{
	bool operator==(const FAudioStopCommand&) const = default;

	FAudioVoiceId Voice{};
	float FadeSeconds = 0.05f;
};

struct FAudioMoveCommand
{
	bool operator==(const FAudioMoveCommand&) const = default;

	FAudioVoiceId Voice{};
	FVector3 Position{};
};

struct FAudioParameterCommand
{
	bool operator==(const FAudioParameterCommand&) const = default;

	FAudioVoiceId Voice{};
	FAudioParameterValue Parameter{};
};

struct FAudioListenerCommand
{
	bool operator==(const FAudioListenerCommand&) const = default;

	FAudioListener Listener{};
};

struct FAudioBusVolumeCommand
{
	bool operator==(const FAudioBusVolumeCommand&) const = default;

	std::string Bus{};
	// Linear gain, eased over a few milliseconds so changes do not click.
	float Volume = 1.f;
};

struct FAudioStopAllCommand
{
	bool operator==(const FAudioStopAllCommand&) const = default;

	float FadeSeconds = 0.05f;
};

// What gameplay asks of audio. Headless sessions record the same commands a device would play, so tests compare them without a device.
using FAudioCommand = std::variant<FAudioPlayCommand, FAudioStopCommand, FAudioMoveCommand, FAudioParameterCommand, FAudioListenerCommand, FAudioBusVolumeCommand, FAudioStopAllCommand>;

// One line per command, for diagnostics and test failures.
[[nodiscard]] std::string FormatAudioCommand(const FAudioCommand& Command);
}
