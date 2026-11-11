#include "EditorFrameworkPrivate.h"

#include "Herta/Assets/VirtualFileSystem.h"

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

		// Particle systems ride the same keep-loaded list, so Play starts with its effects ready too.
		if (Entity.ParticleEmitter)
		{
			Wanted.push_back(Entity.ParticleEmitter->System);
		}

		// So do StateTrees, so agents start with Play.
		if (Entity.AiAgent)
		{
			Wanted.push_back(Entity.AiAgent->StateTree);
		}
	}

	if (Level)
	{
		for (const FAssetId& Effect : Level->GetEffects())
		{
			Wanted.push_back(Effect);
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
				if (std::ranges::none_of(Wanted, [&Wave](const FAssetId& Known)
				    {
					    return Known == Wave;
				    }))
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
			if (Slot && Slot->ParticleSystem && PlaySession)
			{
				(void)PlaySession->AddParticleSystem(Sound, Slot->ParticleSystem);
			}

			// Adding a tree restarts every agent, so each tree goes to a session once.
			if (Slot && Slot->StateTree && PlaySession && !PlayStateTrees.contains(Sound))
			{
				PlayStateTrees.insert(Sound);
				if (auto Added = PlaySession->AddStateTree(Sound, Slot->StateTree); !Added)
				{
					HERTA_LOG_WARNING(*Log, EditorLog, "{} will not run: {}", Slot->Label, Added.error().Message);
				}
			}

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

	return std::ranges::any_of(Level->GetSounds(), Loading) || std::ranges::any_of(Level->GetEffects(), Loading) || std::ranges::any_of(VisualEntities, [&Loading](const FLevelEntity& Entity)
	{
		return (Entity.AudioEmitter && Loading(Entity.AudioEmitter->Sound)) || (Entity.ParticleEmitter && Loading(Entity.ParticleEmitter->System)) || (Entity.AiAgent && Loading(Entity.AiAgent->StateTree));
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

namespace
{
// Identities of their own, so an audition never collides with a level's sounds or Play's voices.
constexpr FAssetId AuditionSound{0x4155444954494f4e, 1};
constexpr FAudioVoiceId AuditionVoice{0x7fff0001};
}

std::optional<FAssetId> FEditorFramework::FImplementation::ResolveAudioWave(const std::string_view Mount, const std::string_view Path) const
{
	if (!Assets)
	{
		return std::nullopt;
	}

	const std::string Label = std::format("{}/{}", Mount, Path);
	for (const FPreviewAssetOption& Option : Assets->GetOptions())
	{
		if (Option.Label == Label && Option.Importer == "Sound")
		{
			return Option.Id;
		}
	}

	return std::nullopt;
}

std::optional<FGraph> FEditorFramework::FImplementation::LoadAudioSubgraph(const std::string_view Mount, const std::string_view Path) const
{
	const std::filesystem::path& Root = Mount == "Engine" ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot;
	if (Root.empty() || !IsValidAssetPath(Path))
	{
		return std::nullopt;
	}

	auto Text = ReadDiskFile(Root / std::filesystem::path(std::u8string(Path.begin(), Path.end())));
	if (!Text)
	{
		return std::nullopt;
	}

	auto Loaded = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Text->data()), Text->size()), GetAudioGraphSchema());
	return Loaded ? std::optional(std::move(*Loaded)) : std::nullopt;
}

void FEditorFramework::FImplementation::DrawAudioGraphPanel()
{
	if (!AudioGraphPanel.IsOpen())
	{
		if (bAuditionShown)
		{
			bAuditionShown = false;
			AuditionGraphs.clear();
			AuditionDesc.reset();
			if (Audio)
			{
				Audio->Submit(FAudioStopCommand{.Voice = AuditionVoice});
				Audio->RemoveSound(AuditionSound);
			}
		}

		return;
	}

	bAuditionShown = true;
	const std::string& Path = AudioGraphPanel.GetPath();
	const std::string Mount = Path.substr(0, Path.find('/'));
	const auto Resolve = [this, Mount](const std::string_view Wave)
	{
		return ResolveAudioWave(Mount, Wave);
	};

	const auto Load = [this, Mount](const std::string_view Subgraph)
	{
		return LoadAudioSubgraph(Mount, Subgraph);
	};

	SetAudioGraphDocumentResolver(Resolve, Load);
	const std::uint64_t OptionsGeneration = Assets ? Assets->GetOptionsGeneration() : 0;
	if (AudioGraphRevision != AudioGraphPanel.GetDraftRevision() || AudioGraphOptionsGeneration != OptionsGeneration)
	{
		if (AudioGraphOptionsGeneration != OptionsGeneration)
		{
			AudioGraphPanel.Recompile();
		}

		AudioGraphRevision = AudioGraphPanel.GetDraftRevision();
		AudioGraphOptionsGeneration = OptionsGeneration;
		const auto Compiled = CompileAudioGraph(AudioGraphPanel.GetDraft(), Resolve, Load);
		if (Compiled)
		{
			// Inputs keep their values across edits while their names stay.
			std::vector<float> Values;
			for (const FSoundParameterDesc& Parameter : Compiled->Graph.Parameters)
			{
				const auto Previous = AuditionDesc ? std::ranges::find(AuditionDesc->Parameters, Parameter.Name, &FSoundParameterDesc::Name) : std::vector<FSoundParameterDesc>::const_iterator{};
				const bool bKept = AuditionDesc && Previous != AuditionDesc->Parameters.end() && Parameter.Type != ESoundParameterType::Trigger;
				Values.push_back(bKept ? AuditionValues[static_cast<std::size_t>(Previous - AuditionDesc->Parameters.begin())] : Parameter.Default);
			}

			AuditionValues = std::move(Values);
			AuditionDesc = Compiled->Graph;
			AuditionError.clear();
			auto Graph = std::make_shared<const FCookedSoundGraph>(FCookedSoundGraph{.Graph = Compiled->Graph});
			AuditionGraphs = {{AuditionSound, Graph}};
			if (Audio)
			{
				if (auto Added = Audio->AddSoundGraph(AuditionSound, std::move(Graph)); !Added)
				{
					AuditionError = Added.error().Message;
				}
			}
		}
		else
		{
			AuditionError = Compiled.error().empty() ? std::string("The graph does not compile") : Compiled.error().front().Message;
		}
	}

	const std::string_view Status = AudioGraphPanel.IsDirty() ? "Auditions play the draft; saving recooks it for Play" : "Auditions play the graph as Play will";
	AudioGraphPanel.Draw(*ToolUI, {.Status = Status, .DrawSidebar = [this]
	{
		DrawAudioAudition();
	}});
}

void FEditorFramework::FImplementation::DrawAudioAudition()
{
	ImGui::TextDisabled("Audition");
	if (!Audio)
	{
		ImGui::TextDisabled("Audio is unavailable");
		return;
	}

	if (!AuditionDesc)
	{
		ImGui::PushTextWrapPos();
		ImGui::TextDisabled("%s", AuditionError.empty() ? "Fix the graph to audition it" : AuditionError.c_str());
		ImGui::PopTextWrapPos();
		return;
	}

	const bool bLoading = std::ranges::any_of(GetSoundGraphWaves(*AuditionDesc), [this](const FAssetId& Wave)
	{
		const FPreviewMeshSlot* const Slot = Assets ? Assets->GetCachedAsset(Wave) : nullptr;
		return !Slot || Slot->bLoading;
	});

	ImGui::BeginDisabled(bLoading);
	if (ToolUIButton("Play", EToolUIMenuIcon::Sound))
	{
		FAudioPlayCommand Play{.Voice = AuditionVoice, .Sound = AuditionSound};
		for (std::size_t Index = 0; Index < AuditionDesc->Parameters.size(); ++Index)
		{
			if (AuditionDesc->Parameters[Index].Type != ESoundParameterType::Trigger)
			{
				Play.Parameters.push_back({.Name = AuditionDesc->Parameters[Index].Name, .Value = AuditionValues[Index]});
			}
		}

		Audio->Submit(FAudioStopCommand{.Voice = AuditionVoice, .FadeSeconds = 0.02f});
		Audio->Submit(Play);
	}

	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ToolUIButton("Stop", EToolUIMenuIcon::Delete))
	{
		Audio->Submit(FAudioStopCommand{.Voice = AuditionVoice});
	}

	ImGui::SameLine();
	ImGui::TextDisabled("%s", bLoading ? "Loading waves..." : Audio->IsVoicePlaying(AuditionVoice) ? "Playing"
	                                                                                                  : "Stopped");
	if (!AuditionError.empty())
	{
		ImGui::PushTextWrapPos();
		ImGui::TextColored(ImVec4(1.f, 0.49f, 0.49f, 1.f), "%s", AuditionError.c_str());
		ImGui::PopTextWrapPos();
	}

	// Inputs change the playing audition as gameplay would.
	for (std::size_t Index = 0; Index < AuditionDesc->Parameters.size(); ++Index)
	{
		const FSoundParameterDesc& Parameter = AuditionDesc->Parameters[Index];
		ImGui::PushID(static_cast<int>(Index));
		bool bChanged = false;
		float& Value = AuditionValues[Index];
		if (Parameter.Type == ESoundParameterType::Trigger)
		{
			if (ToolUIButton(std::format("Fire {}", Parameter.Name), EToolUIMenuIcon::Sound))
			{
				Value = 1.f;
				bChanged = true;
			}
		}
		else if (Parameter.Type == ESoundParameterType::Bool)
		{
			bool bOn = Value != 0.f;
			if (ImGui::Checkbox(Parameter.Name.c_str(), &bOn))
			{
				Value = bOn ? 1.f : 0.f;
				bChanged = true;
			}
		}
		else
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			bChanged = ImGui::DragFloat("##Value", &Value, 0.05f, 0.f, 0.f, std::format("{} %.2f", Parameter.Name).c_str());
		}

		if (bChanged)
		{
			Audio->Submit(FAudioParameterCommand{.Voice = AuditionVoice, .Parameter = {.Name = Parameter.Name, .Value = Value}});
		}

		ImGui::PopID();
	}
}

void FEditorFramework::FImplementation::OpenAudioGraphKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetAudioGraphSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open the Audio Graph {}: {}", Key, Graph.error().Message);
		return;
	}

	if (AudioGraphPanel.IsOpen() && AudioGraphPanel.GetPath() != Key && AudioGraphPanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another Audio Graph", AudioGraphPanel.GetPath());
		return;
	}

	AudioGraphPanel.Open(Key, File, std::move(*Graph), bEngine);
	AudioGraphRevision = UINT64_MAX;
	AuditionDesc.reset();
}
}
