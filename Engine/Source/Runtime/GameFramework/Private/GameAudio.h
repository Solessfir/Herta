#pragma once

#include "Herta/Audio/AudioCommands.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Math/Matrix.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// Turns a session's gameplay into audio commands: the level's emitters, its gameplay sounds, and the listener. Voice identities restart with
// every session start, so a replay of the same input issues the same commands.
class FGameAudio final
{
public:
	void SetSounds(const FLevelSounds& InSounds);
	// Stops everything and starts the auto-play emitters at the entities' world transforms.
	void Start(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> World);
	void Play(ELevelSound Cue, const FVector3& Position, std::vector<FAudioParameterValue> Parameters = {});
	void PlayFlat(ELevelSound Cue, std::vector<FAudioParameterValue> Parameters = {});
	// Keeps a cue's looping voice playing while On, following Position, and fades it out when it turns off.
	void SetLoop(ELevelSound Cue, bool bOn, const FVector3& Position);
	// Moves emitters whose entity moved more than a centimeter.
	void MoveEmitters(std::span<const FMatrix4> World);
	// Sends the listener when it moved or turned.
	void SetListener(const FAudioListener& Listener);
	[[nodiscard]] std::vector<FAudioCommand> TakeCommands();
	// Every sound the level's emitters and gameplay may play, for hosts to load before play.
	[[nodiscard]] std::vector<FAssetId> GetSounds(std::span<const FLevelEntity> Entities) const;
	[[nodiscard]] std::span<const std::size_t> GetEmitterEntities() const noexcept;

private:
	struct FEmitter
	{
		std::size_t Entity = 0;
		FAudioVoiceId Voice{};
		FVector3 Position{};
	};

	struct FLoop
	{
		FAudioVoiceId Voice{};
		FVector3 Position{};
	};

	[[nodiscard]] FAudioVoiceId NextVoice();

	FLevelSounds Sounds{};
	std::vector<FAudioCommand> Commands{};
	std::uint32_t Voices = 0;
	std::vector<FEmitter> Emitters{};
	std::vector<std::size_t> EmitterEntities{};
	std::array<std::optional<FLoop>, LevelSoundCues.size()> Loops{};
	std::optional<FAudioListener> Listener{};
};
}
