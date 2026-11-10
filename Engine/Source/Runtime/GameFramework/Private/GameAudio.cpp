#include "GameAudio.h"

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
[[nodiscard]] FVector3 GetPosition(const FMatrix4& Matrix)
{
	return {Matrix(0, 3), Matrix(1, 3), Matrix(2, 3)};
}
}

void FGameAudio::SetSounds(const FLevelSounds& InSounds)
{
	Sounds = InSounds;
}

FAudioVoiceId FGameAudio::NextVoice()
{
	return {++Voices};
}

void FGameAudio::Start(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> World)
{
	Commands.push_back(FAudioStopAllCommand{.FadeSeconds = 0.1f});
	Voices = 0;
	Emitters.clear();
	EmitterEntities.clear();
	Loops.fill(std::nullopt);
	Listener.reset();
	for (std::size_t Index = 0; Index < Entities.size() && Index < World.size(); ++Index)
	{
		const auto& Emitter = Entities[Index].AudioEmitter;
		if (!Emitter || !Emitter->Sound.IsValid() || !Emitter->bAutoPlay)
		{
			continue;
		}

		const FAudioVoiceId Voice = NextVoice();
		const FVector3 Position = GetPosition(World[Index]);
		Emitters.push_back({.Entity = Index, .Voice = Voice, .Position = Position});
		EmitterEntities.push_back(Index);
		Commands.push_back(FAudioPlayCommand{
		    .Voice = Voice,
		    .Sound = Emitter->Sound,
		    .Volume = Emitter->Volume,
		    .Pitch = Emitter->Pitch,
		    .bSpatial = Emitter->bSpatial,
		    .Position = Position,
		    .MinimumDistance = Emitter->MinimumDistance,
		    .MaximumDistance = Emitter->MaximumDistance,
		});
	}
}

void FGameAudio::Play(const ELevelSound Cue, const FVector3& Position, std::vector<FAudioParameterValue> Parameters)
{
	const FAssetId& Sound = Sounds[static_cast<std::size_t>(Cue)];
	if (Sound.IsValid())
	{
		Commands.push_back(FAudioPlayCommand{.Sound = Sound, .bSpatial = true, .Position = Position, .Parameters = std::move(Parameters)});
	}
}

void FGameAudio::PlayFlat(const ELevelSound Cue, std::vector<FAudioParameterValue> Parameters)
{
	const FAssetId& Sound = Sounds[static_cast<std::size_t>(Cue)];
	if (Sound.IsValid())
	{
		Commands.push_back(FAudioPlayCommand{.Sound = Sound, .Parameters = std::move(Parameters)});
	}
}

void FGameAudio::SetLoop(const ELevelSound Cue, const bool bOn, const FVector3& Position)
{
	std::optional<FLoop>& Loop = Loops[static_cast<std::size_t>(Cue)];
	const FAssetId& Sound = Sounds[static_cast<std::size_t>(Cue)];
	if (bOn && !Loop && Sound.IsValid())
	{
		Loop = FLoop{.Voice = NextVoice(), .Position = Position};
		Commands.push_back(FAudioPlayCommand{.Voice = Loop->Voice, .Sound = Sound, .bSpatial = true, .Position = Position});
	}
	else if (!bOn && Loop)
	{
		Commands.push_back(FAudioStopCommand{.Voice = Loop->Voice, .FadeSeconds = 0.15f});
		Loop.reset();
	}
	else if (Loop && (Position - Loop->Position).LengthSquared() > 0.0001f)
	{
		Loop->Position = Position;
		Commands.push_back(FAudioMoveCommand{.Voice = Loop->Voice, .Position = Position});
	}
}

void FGameAudio::MoveEmitters(const std::span<const FMatrix4> World)
{
	for (FEmitter& Emitter : Emitters)
	{
		if (Emitter.Entity >= World.size())
		{
			continue;
		}

		const FVector3 Position = GetPosition(World[Emitter.Entity]);
		if ((Position - Emitter.Position).LengthSquared() > 0.0001f)
		{
			Emitter.Position = Position;
			Commands.emplace_back(std::in_place_type<FAudioMoveCommand>, FAudioMoveCommand{.Voice = Emitter.Voice, .Position = Position});
		}
	}
}

void FGameAudio::SetListener(const FAudioListener& Next)
{
	const bool bMoved = !Listener || (Next.Position - Listener->Position).LengthSquared() > 0.0001f || Next.Forward.Dot(Listener->Forward) < 0.9999f || Next.Up.Dot(Listener->Up) < 0.9999f;
	if (bMoved)
	{
		Listener = Next;
		Commands.emplace_back(std::in_place_type<FAudioListenerCommand>, FAudioListenerCommand{.Listener = Next});
	}
}

std::vector<FAudioCommand> FGameAudio::TakeCommands()
{
	return std::exchange(Commands, {});
}

std::vector<FAssetId> FGameAudio::GetSounds(const std::span<const FLevelEntity> Entities) const
{
	std::vector<FAssetId> Result;
	for (const FAssetId& Sound : Sounds)
	{
		if (Sound.IsValid())
		{
			Result.push_back(Sound);
		}
	}

	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.AudioEmitter && Entity.AudioEmitter->Sound.IsValid())
		{
			Result.push_back(Entity.AudioEmitter->Sound);
		}
	}

	std::ranges::sort(Result);
	const auto Duplicates = std::ranges::unique(Result);
	Result.erase(Duplicates.begin(), Duplicates.end());
	return Result;
}

std::span<const std::size_t> FGameAudio::GetEmitterEntities() const noexcept
{
	return EmitterEntities;
}
}
