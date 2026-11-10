#include "Herta/Audio/AudioEngine.h"

#include "AudioDevice.h"
#include "AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace Herta
{
std::vector<FAudioBusDesc> GetDefaultAudioBuses()
{
	return {
	    {.Name = "Master"},
	    {.Name = "Effects", .Parent = "Master"},
	    {.Name = "Ambient", .Parent = "Master"},
	    {.Name = "Interface", .Parent = "Master"},
	};
}

struct FAudioEngine::FImplementation
{
	FImplementation(const FAudioEngineDesc& Desc);
	~FImplementation();

	FImplementation(const FImplementation&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	[[nodiscard]] std::expected<std::uint32_t, FAudioError> FindBus(std::string_view Name) const;
	[[nodiscard]] FMixerSound* Resolve(const FAssetId& Id);
	void Retire(std::shared_ptr<FMixerSound> Sound);
	void Invalidate();
	void Push(const FMixerCommand& Command);
	void StreamLoop();

	std::uint32_t SampleRate = 48000;
	std::vector<FAudioBusDesc> Buses;
	std::unique_ptr<FMixer> Mixer;
	std::string DeviceError;

	std::map<FAssetId, std::shared_ptr<FMixerSound>> Sounds;
	std::map<FAssetId, std::shared_ptr<const FCookedSoundGraph>> Graphs;
	std::map<FAssetId, std::shared_ptr<FMixerSound>> ResolvedGraphs;
	std::vector<std::shared_ptr<FMixerSound>> Retired;
	// Addressed voices and how many of their plays have not ended, since an identity can be played again before the last play ends.
	std::map<std::uint32_t, std::pair<FMixerSound*, std::uint32_t>> Playing;

	std::uint32_t PrivateVoices = 0;
	std::uint64_t CommandsSubmitted = 0;
	std::uint64_t CommandsDropped = 0;
	std::uint64_t MissingSounds = 0;
	std::array<float, MaximumAudioBuses> LastPeaks{};
	double NullFrames = 0.;
	std::vector<float> NullScratch;

	std::mutex StreamMutex;
	std::condition_variable StreamWake;
	bool bStreaming = true;
	std::thread Streamer;

	// Declared last so the device stops calling into the mixer before anything else goes away.
	std::unique_ptr<FAudioDevice> Device;
};

FAudioEngine::FImplementation::FImplementation(const FAudioEngineDesc& Desc)
    : SampleRate(Desc.SampleRate)
    , Buses(Desc.Buses)
    , Mixer(std::make_unique<FMixer>(Desc.SampleRate, Desc.Buses, Desc.Device == EAudioDevice::Null))
{
}

FAudioEngine::FImplementation::~FImplementation()
{
	Device.reset();
	if (Streamer.joinable())
	{
		{
			const std::scoped_lock Lock(StreamMutex);
			bStreaming = false;
		}

		StreamWake.notify_all();
		Streamer.join();
	}
}

std::expected<std::uint32_t, FAudioError> FAudioEngine::FImplementation::FindBus(const std::string_view Name) const
{
	const auto Found = std::ranges::find(Buses, Name, &FAudioBusDesc::Name);
	if (Found == Buses.end())
	{
		return std::unexpected(FAudioError{std::format("Unknown audio bus '{}'", Name)});
	}

	return static_cast<std::uint32_t>(Found - Buses.begin());
}

void FAudioEngine::FImplementation::Retire(std::shared_ptr<FMixerSound> Sound)
{
	if (Sound)
	{
		Retired.push_back(std::move(Sound));
	}
}

// Graphs resolve their waves again on their next play after any sound changes.
void FAudioEngine::FImplementation::Invalidate()
{
	for (auto& [Id, Sound] : ResolvedGraphs)
	{
		Retire(std::move(Sound));
	}

	ResolvedGraphs.clear();
}

FMixerSound* FAudioEngine::FImplementation::Resolve(const FAssetId& Id)
{
	if (const auto Found = Sounds.find(Id); Found != Sounds.end())
	{
		return Found->second.get();
	}

	if (const auto Found = ResolvedGraphs.find(Id); Found != ResolvedGraphs.end())
	{
		return Found->second.get();
	}

	const auto Graph = Graphs.find(Id);
	if (Graph == Graphs.end())
	{
		return nullptr;
	}

	auto Sound = std::make_shared<FMixerSound>();
	Sound->Playback = Graph->second->Playback;
	Sound->Bus = FindBus(Sound->Playback.Bus).value_or(0);
	Sound->Graph = Graph->second->Graph;
	Sound->GraphAsset = Graph->second;
	Sound->NodeWaves.resize(Sound->Graph.Nodes.size());
	for (std::size_t Index = 0; Index < Sound->Graph.Nodes.size(); ++Index)
	{
		const FSoundNodeDesc& Node = Sound->Graph.Nodes[Index];
		Sound->bParameterDriven = Sound->bParameterDriven || (Node.Type == ESoundNodeType::WavePlayer && Node.Play != NoSoundControl);
		for (const FAssetId& Wave : Node.Waves)
		{
			const auto Found = Sounds.find(Wave);
			if (Found == Sounds.end())
			{
				return nullptr;
			}

			Sound->NodeWaves[Index].push_back(&Found->second->Wave);
			if (!std::ranges::contains(Sound->WaveSounds, Found->second))
			{
				Sound->WaveSounds.push_back(Found->second);
			}
		}
	}

	return ResolvedGraphs.emplace(Id, std::move(Sound)).first->second.get();
}

void FAudioEngine::FImplementation::Push(const FMixerCommand& Command)
{
	if (Command.Type == EMixerCommandType::Play)
	{
		Command.Sound->Voices.fetch_add(1, std::memory_order_acq_rel);
	}

	if (!Mixer->Commands.Push(Command))
	{
		++CommandsDropped;
		if (Command.Type == EMixerCommandType::Play)
		{
			Command.Sound->Voices.fetch_sub(1, std::memory_order_acq_rel);
		}

		return;
	}

	if (Command.Type == EMixerCommandType::Play && IsAddressedVoice(Command.Voice))
	{
		auto& [Sound, Plays] = Playing[Command.Voice];
		Sound = Command.Sound;
		++Plays;
	}

	if (Command.Type == EMixerCommandType::Play && Command.Sound->Stream)
	{
		StreamWake.notify_one();
	}
}

void FAudioEngine::FImplementation::StreamLoop()
{
	std::unique_lock Lock(StreamMutex);
	while (bStreaming)
	{
		Lock.unlock();
		const bool bBusy = Mixer->ServiceStreams();
		Lock.lock();

		// A busy stream refills every few milliseconds, well inside the ring's second of audio; an idle thread waits for the next stream.
		StreamWake.wait_for(Lock, bBusy ? std::chrono::milliseconds(4) : std::chrono::milliseconds(100), [this]
		{
			return !bStreaming;
		});
	}
}

namespace
{
void RenderDevice(void* const User, float* const Output, const std::uint32_t Frames)
{
	static_cast<FMixer*>(User)->Render(Output, Frames);
}
}

std::expected<std::unique_ptr<FAudioEngine>, FAudioError> FAudioEngine::Create(const FAudioEngineDesc& Desc)
{
	if (Desc.SampleRate < MinimumSoundSampleRate || Desc.SampleRate > MaximumSoundSampleRate)
	{
		return std::unexpected(FAudioError{"Unsupported audio sample rate"});
	}

	if (Desc.Buses.empty() || Desc.Buses.size() > MaximumAudioBuses || !Desc.Buses.front().Parent.empty())
	{
		return std::unexpected(FAudioError{std::format("Audio needs 1 to {} buses, starting with the master", MaximumAudioBuses)});
	}

	for (std::size_t Index = 0; Index < Desc.Buses.size(); ++Index)
	{
		const auto Before = std::span(Desc.Buses).first(Index);
		const bool bParentKnown = Index == 0 || std::ranges::contains(Before, Desc.Buses[Index].Parent, &FAudioBusDesc::Name);
		if (Desc.Buses[Index].Name.empty() || std::ranges::contains(Before, Desc.Buses[Index].Name, &FAudioBusDesc::Name) || !bParentKnown || !(Desc.Buses[Index].Volume >= 0.f && Desc.Buses[Index].Volume <= 16.f))
		{
			return std::unexpected(FAudioError{std::format("Audio bus '{}' needs a unique name, a parent listed before it, and a volume from 0 to 16", Desc.Buses[Index].Name)});
		}
	}

	auto Implementation = std::make_unique<FImplementation>(Desc);
	if (Desc.Device == EAudioDevice::Default)
	{
		auto Device = FAudioDevice::Open(Desc.SampleRate, RenderDevice, Implementation->Mixer.get());
		if (Device)
		{
			Implementation->Device = std::move(*Device);
			Implementation->Streamer = std::thread([Raw = Implementation.get()]
			{
				Raw->StreamLoop();
			});
		}
		else
		{
			// The null path renders on the game thread, so its streams can open and fill there too.
			Implementation->DeviceError = std::move(Device.error().Message);
			Implementation->Mixer = std::make_unique<FMixer>(Desc.SampleRate, Desc.Buses, true);
		}
	}

	return std::unique_ptr<FAudioEngine>(new FAudioEngine(std::move(Implementation)));
}

FAudioEngine::FAudioEngine(std::unique_ptr<FImplementation> InImplementation)
    : Implementation(std::move(InImplementation))
{
}

FAudioEngine::~FAudioEngine() = default;

std::expected<void, FAudioError> FAudioEngine::AddSound(const FAssetId& Id, std::shared_ptr<const FCookedSound> Sound)
{
	if (!Id.IsValid() || !Sound)
	{
		return std::unexpected(FAudioError{"A sound needs an ID and data"});
	}

	if (auto Valid = ValidateCookedSound(*Sound); !Valid)
	{
		return std::unexpected(FAudioError{std::move(Valid.error().Message)});
	}

	const auto Bus = Implementation->FindBus(Sound->Playback.Bus);
	if (!Bus)
	{
		return std::unexpected(Bus.error());
	}

	auto Mixer = std::make_shared<FMixerSound>();
	Mixer->Playback = Sound->Playback;
	Mixer->Bus = *Bus;
	Mixer->Wave = {.Samples = Sound->Samples.data(), .Frames = Sound->Samples.size() / Sound->Channels, .Channels = Sound->Channels, .SampleRate = Sound->SampleRate};
	Mixer->Sound = std::move(Sound);
	Mixer->Graph = {.Nodes = {{.Type = ESoundNodeType::WavePlayer, .Waves = {Id}, .bLoop = Mixer->Sound->bLoop}, {.Type = ESoundNodeType::Output, .Inputs = {0}}}};
	Mixer->NodeWaves = {{&Mixer->Wave}, {}};

	Implementation->Retire(std::exchange(Implementation->Sounds[Id], std::move(Mixer)));
	Implementation->Invalidate();
	return {};
}

std::expected<void, FAudioError> FAudioEngine::AddSoundStream(const FAssetId& Id, const FCookedSoundStream& Stream)
{
	const auto Bus = Implementation->FindBus(Stream.Playback.Bus);
	if (!Id.IsValid() || !Bus || Stream.Frames == 0 || Stream.Channels == 0 || Stream.Channels > MaximumSoundChannels)
	{
		return std::unexpected(Bus ? FAudioError{"A sound stream needs an ID and whole frames"} : Bus.error());
	}

	auto Mixer = std::make_shared<FMixerSound>();
	Mixer->Playback = Stream.Playback;
	Mixer->Bus = *Bus;
	Mixer->Stream = Stream;
	Mixer->Wave = {.Frames = Stream.Frames, .Channels = Stream.Channels, .SampleRate = Stream.SampleRate, .Stream = &*Mixer->Stream};
	Mixer->Graph = {.Nodes = {{.Type = ESoundNodeType::WavePlayer, .Waves = {Id}, .bLoop = Stream.bLoop}, {.Type = ESoundNodeType::Output, .Inputs = {0}}}};
	Mixer->NodeWaves = {{&Mixer->Wave}, {}};

	Implementation->Retire(std::exchange(Implementation->Sounds[Id], std::move(Mixer)));
	Implementation->Invalidate();
	return {};
}

std::expected<void, FAudioError> FAudioEngine::AddSoundGraph(const FAssetId& Id, std::shared_ptr<const FCookedSoundGraph> Graph)
{
	if (!Id.IsValid() || !Graph)
	{
		return std::unexpected(FAudioError{"A sound graph needs an ID and data"});
	}

	if (auto Valid = ValidateSoundPlayback(Graph->Playback); !Valid)
	{
		return std::unexpected(FAudioError{std::move(Valid.error().Message)});
	}

	if (auto Valid = ValidateSoundGraph(Graph->Graph); !Valid)
	{
		return std::unexpected(FAudioError{std::move(Valid.error().Message)});
	}

	if (const auto Bus = Implementation->FindBus(Graph->Playback.Bus); !Bus)
	{
		return std::unexpected(Bus.error());
	}

	Implementation->Graphs[Id] = std::move(Graph);
	Implementation->Invalidate();
	return {};
}

bool FAudioEngine::HasSound(const FAssetId& Id) const
{
	return Implementation->Sounds.contains(Id) || Implementation->Graphs.contains(Id);
}

void FAudioEngine::RemoveSound(const FAssetId& Id)
{
	if (const auto Found = Implementation->Sounds.find(Id); Found != Implementation->Sounds.end())
	{
		Implementation->Retire(std::move(Found->second));
		Implementation->Sounds.erase(Found);
	}

	Implementation->Graphs.erase(Id);
	Implementation->Invalidate();
}

void FAudioEngine::Submit(const std::span<const FAudioCommand> Commands)
{
	for (const FAudioCommand& Command : Commands)
	{
		Submit(Command);
	}
}

void FAudioEngine::Submit(const FAudioCommand& Command)
{
	FImplementation& State = *Implementation;
	++State.CommandsSubmitted;

	const auto FindParameter = [](const FMixerSound& Sound, const std::string_view Name) -> std::optional<std::uint32_t>
	{
		const auto& Parameters = Sound.Graph.Parameters;
		const auto Found = std::ranges::find(Parameters, Name, &FSoundParameterDesc::Name);
		return Found == Parameters.end() ? std::nullopt : std::optional(static_cast<std::uint32_t>(Found - Parameters.begin()));
	};

	if (const auto* Play = std::get_if<FAudioPlayCommand>(&Command))
	{
		FMixerSound* const Sound = State.Resolve(Play->Sound);
		if (!Sound)
		{
			++State.MissingSounds;
			return;
		}

		if ((Play->Voice.Value & PrivateVoiceBit) != 0)
		{
			++State.CommandsDropped;
			return;
		}

		std::uint32_t Voice = Play->Voice.Value;
		if (Voice == 0 && !Play->Parameters.empty())
		{
			State.PrivateVoices = (State.PrivateVoices + 1) & ~PrivateVoiceBit;
			Voice = PrivateVoiceBit | std::max(State.PrivateVoices, 1u);
		}

		State.Push({
		    .Type = EMixerCommandType::Play,
		    .Voice = Voice,
		    .Sound = Sound,
		    .Volume = std::clamp(Play->Volume, 0.f, 16.f),
		    .Pitch = std::clamp(Play->Pitch, 0.01f, 16.f),
		    .bSpatial = Play->bSpatial,
		    .Position = Play->Position,
		    .MinimumDistance = Play->MinimumDistance > 0.f ? Play->MinimumDistance : Sound->Playback.MinimumDistance,
		    .MaximumDistance = Play->MaximumDistance > 0.f ? Play->MaximumDistance : Sound->Playback.MaximumDistance,
		});

		for (const FAudioParameterValue& Parameter : Play->Parameters)
		{
			if (const auto Index = FindParameter(*Sound, Parameter.Name))
			{
				State.Push({.Type = EMixerCommandType::Parameter, .Voice = Voice, .Index = *Index, .Value = Parameter.Value});
			}
		}

		return;
	}

	if (const auto* Stop = std::get_if<FAudioStopCommand>(&Command))
	{
		State.Push({.Type = EMixerCommandType::Stop, .Voice = Stop->Voice.Value, .Value = Stop->FadeSeconds});
	}
	else if (const auto* Move = std::get_if<FAudioMoveCommand>(&Command))
	{
		State.Push({.Type = EMixerCommandType::Move, .Voice = Move->Voice.Value, .Position = Move->Position});
	}
	else if (const auto* Parameter = std::get_if<FAudioParameterCommand>(&Command))
	{
		const auto Found = State.Playing.find(Parameter->Voice.Value);
		if (Found != State.Playing.end())
		{
			if (const auto Index = FindParameter(*Found->second.first, Parameter->Parameter.Name))
			{
				State.Push({.Type = EMixerCommandType::Parameter, .Voice = Parameter->Voice.Value, .Index = *Index, .Value = Parameter->Parameter.Value});
			}
		}
	}
	else if (const auto* Listener = std::get_if<FAudioListenerCommand>(&Command))
	{
		State.Push({.Type = EMixerCommandType::Listener, .Listener = Listener->Listener});
	}
	else if (const auto* Bus = std::get_if<FAudioBusVolumeCommand>(&Command))
	{
		if (const auto Index = State.FindBus(Bus->Bus))
		{
			State.Push({.Type = EMixerCommandType::BusVolume, .Index = *Index, .Value = std::clamp(Bus->Volume, 0.f, 16.f)});
		}
	}
	else if (const auto* StopAll = std::get_if<FAudioStopAllCommand>(&Command))
	{
		State.Push({.Type = EMixerCommandType::StopAll, .Value = StopAll->FadeSeconds});
	}
}

void FAudioEngine::Update(const float DeltaSeconds)
{
	FImplementation& State = *Implementation;
	if (!State.Device && DeltaSeconds > 0.f)
	{
		// Long stalls render at most a quarter second, so a hitch never turns into a long burst of mixing.
		State.NullFrames += std::min(DeltaSeconds, 0.25f) * static_cast<float>(State.SampleRate);
		State.NullScratch.resize(std::size_t{4096} * 2);
		while (State.NullFrames >= 1.)
		{
			const auto Frames = static_cast<std::uint32_t>(std::min(State.NullFrames, 4096.));
			Render(std::span(State.NullScratch).first(std::size_t{Frames} * 2));
			State.NullFrames -= Frames;
		}
	}

	FMixerEvent Event;
	while (State.Mixer->Events.Pop(Event))
	{
		if (const auto Found = State.Playing.find(Event.Voice); Found != State.Playing.end() && --Found->second.second == 0)
		{
			State.Playing.erase(Found);
		}
	}

	for (std::size_t Bus = 0; Bus < State.Buses.size(); ++Bus)
	{
		State.LastPeaks[Bus] = State.Mixer->BusPeaks[Bus].exchange(0.f, std::memory_order_relaxed);
	}

	std::erase_if(State.Retired, [](const std::shared_ptr<FMixerSound>& Sound)
	{
		return Sound.use_count() == 1 && Sound->Voices.load(std::memory_order_acquire) == 0;
	});
}

void FAudioEngine::Render(const std::span<float> Interleaved)
{
	FImplementation& State = *Implementation;
	if (State.Device)
	{
		return;
	}

	// Streams fill before every block, so a null render never starves and repeats exactly.
	float* Output = Interleaved.data();
	std::size_t Frames = Interleaved.size() / 2;
	while (Frames > 0)
	{
		State.Mixer->ServiceStreams();
		const std::size_t Count = std::min<std::size_t>(Frames, AudioBlockFrames);
		State.Mixer->Render(Output, static_cast<std::uint32_t>(Count));
		Output += Count * 2;
		Frames -= Count;
	}
}

bool FAudioEngine::IsVoicePlaying(const FAudioVoiceId Voice) const
{
	return Implementation->Playing.contains(Voice.Value);
}

FAudioStats FAudioEngine::GetStats() const
{
	const FImplementation& State = *Implementation;
	const FMixer& Mixer = *State.Mixer;
	FAudioStats Stats{
	    .bNullDevice = !State.Device,
	    .Device = State.Device ? State.Device->GetName() : std::string("Null"),
	    .DeviceError = State.DeviceError,
	    .SampleRate = State.SampleRate,
	    .Sounds = static_cast<std::uint32_t>(State.Sounds.size() + State.Graphs.size()),
	    .Voices = Mixer.ActiveVoices.load(std::memory_order_relaxed),
	    .AudibleVoices = Mixer.AudibleVoices.load(std::memory_order_relaxed),
	    .Streams = Mixer.ActiveStreams.load(std::memory_order_relaxed),
	    .VoicesStarted = Mixer.VoicesStarted.load(std::memory_order_relaxed),
	    .VoicesStolen = Mixer.VoicesStolen.load(std::memory_order_relaxed),
	    .VoicesRejected = Mixer.VoicesRejected.load(std::memory_order_relaxed),
	    .MissingSounds = State.MissingSounds,
	    .StreamStarvedFrames = Mixer.StarvedFrames.load(std::memory_order_relaxed),
	    .CommandsSubmitted = State.CommandsSubmitted,
	    .CommandsDropped = State.CommandsDropped,
	    .Blocks = Mixer.Blocks.load(std::memory_order_relaxed),
	    .BlockBudgetMicroseconds = static_cast<float>(AudioBlockFrames) * 1e6f / static_cast<float>(State.SampleRate),
	};

	if (Stats.Blocks > 0)
	{
		Stats.BlockMicrosecondsAverage = static_cast<float>(static_cast<double>(Mixer.BlockNanoseconds.load(std::memory_order_relaxed)) / static_cast<double>(Stats.Blocks) / 1000.);
	}

	Stats.BlockMicrosecondsPeak = static_cast<float>(Mixer.BlockNanosecondsPeak.load(std::memory_order_relaxed)) / 1000.f;
	for (std::size_t Bus = 0; Bus < State.Buses.size(); ++Bus)
	{
		Stats.Buses.push_back({.Name = State.Buses[Bus].Name, .Volume = Mixer.BusVolumes[Bus].load(std::memory_order_relaxed), .Peak = State.LastPeaks[Bus]});
	}

	return Stats;
}
}
