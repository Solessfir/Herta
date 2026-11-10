#include "EditorFrameworkPrivate.h"

namespace Herta
{
void FEditorFramework::FImplementation::UpdateAudio(const float DeltaSeconds)
{
	if (!Audio && !bAudioUnavailable)
	{
		auto Created = FAudioEngine::Create();
		if (!Created)
		{
			bAudioUnavailable = true;
			HERTA_LOG_ERROR(*Log, EditorLog, "Audio is unavailable: {}", Created.error().Message);
			return;
		}

		Audio = std::move(*Created);
		const FAudioStats Device = Audio->GetStats();
		if (Device.bNullDevice)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Audio plays through the null path: {}", Device.DeviceError);
		}
		else
		{
			HERTA_LOG_INFO(*Log, EditorLog, "Audio plays on {} at {} Hz", Device.Device, Device.SampleRate);
		}
	}

	if (!Audio || !Assets)
	{
		return;
	}

	// The open level's sounds stay loaded so Play starts without waiting, together with the waves of every loaded graph and the audition.
	std::vector<FAssetId> Wanted;
	if (Level)
	{
		for (const FAssetId& Sound : Level->GetSounds())
		{
			Wanted.push_back(Sound);
		}
	}

	for (const FLevelEntity& Entity : VisualEntities)
	{
		if (Entity.AudioEmitter)
		{
			Wanted.push_back(Entity.AudioEmitter->Sound);
		}
	}

	for (const auto& [Id, Graph] : AuditionGraphs)
	{
		for (const FAssetId& Wave : GetSoundGraphWaves(Graph->Graph))
		{
			Wanted.push_back(Wave);
		}
	}

	for (std::size_t Index = 0; Index < Wanted.size(); ++Index)
	{
		const FPreviewMeshSlot* const Slot = Assets->GetCachedAsset(Wanted[Index]);
		if (Slot && Slot->SoundGraph)
		{
			for (const FAssetId& Wave : GetSoundGraphWaves(Slot->SoundGraph->Graph))
			{
				if (!std::ranges::contains(Wanted, Wave))
				{
					Wanted.push_back(Wave);
				}
			}
		}
	}

	std::erase_if(Wanted, [](const FAssetId& Sound)
	{
		return !Sound.IsValid();
	});

	Assets->SetSoundAssets(Wanted);
	for (const FAssetId& Sound : Wanted)
	{
		const FPreviewMeshSlot* const Slot = Assets->GetCachedAsset(Sound);
		if (!Slot || Slot->bLoading || (!Slot->Sound && !Slot->SoundStream && !Slot->SoundGraph))
		{
			continue;
		}

		const auto Known = AudioKeys.find(Sound);
		if (Known != AudioKeys.end() && Known->second == Slot->Key)
		{
			continue;
		}

		const auto Added = Slot->Sound ? Audio->AddSound(Sound, Slot->Sound) : Slot->SoundStream ? Audio->AddSoundStream(Sound, *Slot->SoundStream)
		                                                                                         : Audio->AddSoundGraph(Sound, Slot->SoundGraph);
		AudioKeys[Sound] = Slot->Key;
		if (!Added)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not play {}: {}", Slot->Label, Added.error().Message);
		}
	}

	for (const auto& [Id, Graph] : AuditionGraphs)
	{
		if (!AuditionKeys.contains(Id))
		{
			if (auto Added = Audio->AddSoundGraph(Id, Graph); !Added)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not audition the graph: {}", Added.error().Message);
			}

			AuditionKeys.insert(Id);
		}
	}

	// A paused game mutes rather than stops, so its loops pick up where they were.
	const bool bMuted = PlaySession && bPlayPaused;
	if (bMuted != bAudioMuted)
	{
		bAudioMuted = bMuted;
		Audio->Submit(FAudioBusVolumeCommand{.Bus = "Master", .Volume = bMuted ? 0.f : 1.f});
	}

	Audio->Update(DeltaSeconds);
}

bool FEditorFramework::FImplementation::AreLevelSoundsLoading() const
{
	if (!Audio || !Assets || !Level)
	{
		return false;
	}

	const auto Loading = [this](const FAssetId& Sound)
	{
		const FPreviewMeshSlot* const Slot = Sound.IsValid() ? Assets->GetCachedAsset(Sound) : nullptr;
		return Slot && Slot->bLoading;
	};

	return std::ranges::any_of(Level->GetSounds(), Loading) || std::ranges::any_of(VisualEntities, [&Loading](const FLevelEntity& Entity)
	{
		return Entity.AudioEmitter && Loading(Entity.AudioEmitter->Sound);
	});
}

void FEditorFramework::FImplementation::SubmitPlayAudio()
{
	if (!PlaySession)
	{
		return;
	}

	std::vector<FAudioCommand> Commands = PlaySession->TakeAudioCommands();
	if (Audio)
	{
		Audio->Submit(Commands);
	}
}

void FEditorFramework::FImplementation::StopPlayAudio()
{
	if (Audio)
	{
		Audio->Submit(FAudioStopAllCommand{.FadeSeconds = 0.1f});
	}
}
}
