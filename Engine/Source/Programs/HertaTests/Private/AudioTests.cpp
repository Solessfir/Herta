#include "Herta/Assets/CookedAsset.h"
#include "Herta/Audio/AudioEngine.h"
#include "Herta/Audio/SoundDecoder.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <numbers>
#include <thread>

// Counts allocations made by the thread that is rendering, so tests can prove the mixer never allocates.
namespace
{
thread_local bool bCountAllocations = false;
thread_local std::size_t Allocations = 0;
}

#ifndef __SANITIZE_ADDRESS__
void* operator new(const std::size_t Size)
{
	if (bCountAllocations)
	{
		++Allocations;
	}

	if (void* const Memory = std::malloc(Size == 0 ? 1 : Size))
	{
		return Memory;
	}

	throw std::bad_alloc();
}

// Replaced too, so memory that standard algorithms take with nothrow new, such as stable_sort's buffer, is freed by the allocator that made it.
void* operator new(const std::size_t Size, const std::nothrow_t&) noexcept
{
	if (bCountAllocations)
	{
		++Allocations;
	}

	return std::malloc(Size == 0 ? 1 : Size);
}

void operator delete(void* const Memory, const std::nothrow_t&) noexcept
{
	std::free(Memory);
}

void operator delete(void* const Memory) noexcept
{
	std::free(Memory);
}

void operator delete(void* const Memory, std::size_t) noexcept
{
	std::free(Memory);
}
#endif

using namespace Herta;

namespace
{
constexpr FAssetId ToneId{1, 1};
constexpr FAssetId LowToneId{1, 2};
constexpr FAssetId GraphId{1, 3};
constexpr FAssetId StreamId{1, 4};

[[nodiscard]] std::shared_ptr<FCookedSound> MakeTone(const float Frequency, const std::uint32_t Frames, const bool bLoop = false, const std::uint32_t Channels = 1)
{
	auto Sound = std::make_shared<FCookedSound>();
	Sound->SampleRate = 48000;
	Sound->Channels = Channels;
	Sound->bLoop = bLoop;
	Sound->Samples.resize(std::size_t{Frames} * Channels);
	for (std::uint32_t Frame = 0; Frame < Frames; ++Frame)
	{
		const float Value = std::sin(2.f * std::numbers::pi_v<float> * Frequency * static_cast<float>(Frame) / 48000.f) * 0.5f;
		for (std::uint32_t Channel = 0; Channel < Channels; ++Channel)
		{
			Sound->Samples[std::size_t{Frame} * Channels + Channel] = static_cast<std::int16_t>(Value * 32767.f * (Channel == 0 ? 1.f : -1.f));
		}
	}

	return Sound;
}

[[nodiscard]] std::unique_ptr<FAudioEngine> MakeNullEngine()
{
	auto Engine = FAudioEngine::Create({.Device = EAudioDevice::Null});
	REQUIRE(Engine);
	return std::move(*Engine);
}

[[nodiscard]] std::vector<float> RenderFrames(FAudioEngine& Engine, const std::size_t Frames)
{
	std::vector<float> Output(Frames * 2);
	Engine.Render(Output);
	Engine.Update(0.f);
	return Output;
}

[[nodiscard]] float Rms(const std::span<const float> Samples, const std::size_t Channel)
{
	double Sum = 0.;
	std::size_t Count = 0;
	for (std::size_t Index = Channel; Index < Samples.size(); Index += 2)
	{
		Sum += static_cast<double>(Samples[Index]) * Samples[Index];
		++Count;
	}

	return Count == 0 ? 0.f : static_cast<float>(std::sqrt(Sum / static_cast<double>(Count)));
}

// Impact-style graph: a trigger plays one of two waves through a gain that an impulse maps from quiet to loud.
[[nodiscard]] std::shared_ptr<FCookedSoundGraph> MakeImpactGraph()
{
	auto Graph = std::make_shared<FCookedSoundGraph>();
	Graph->Graph.Parameters = {{.Name = "Hit", .Type = ESoundParameterType::Trigger}, {.Name = "Impulse", .Type = ESoundParameterType::Float}};
	Graph->Graph.Nodes = {
	    {.Type = ESoundNodeType::MapRange, .Control = {.Slot = 1}, .Range = {0.f, 20.f, 0.1f, 1.f}, .Slot = 2},
	    {.Type = ESoundNodeType::WavePlayer, .Waves = {ToneId, LowToneId}, .Play = 0, .PitchVariation = 0.05f},
	    {.Type = ESoundNodeType::Gain, .Inputs = {1}, .Control = {.Slot = 2}},
	    {.Type = ESoundNodeType::LowPass, .Inputs = {2}, .Control = {.Constant = 8000.f}},
	    {.Type = ESoundNodeType::Output, .Inputs = {3}},
	};
	Graph->Graph.ControlSlots = 3;
	return Graph;
}
}

TEST_CASE("Cooked sounds and sound graphs round-trip and validate")
{
	const auto Tone = MakeTone(440.f, 4800, true, 2);
	Tone->Playback = {.Bus = "Ambient", .Volume = 0.5f, .MaximumInstances = 3, .Concurrency = ESoundConcurrency::StealQuietest, .MinimumDistance = 2.f, .MaximumDistance = 25.f};
	const auto Bytes = SerializeCookedAsset(*Tone);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	const auto* const Sound = std::get_if<FCookedSound>(&*Loaded);
	REQUIRE(Sound);
	CHECK(Sound->Playback == Tone->Playback);
	CHECK(Sound->Samples == Tone->Samples);
	CHECK(Sound->bLoop);

	const auto Graph = MakeImpactGraph();
	const auto GraphBytes = SerializeCookedAsset(*Graph);
	REQUIRE(GraphBytes);
	const auto LoadedGraph = DeserializeCookedAsset(*GraphBytes);
	REQUIRE(LoadedGraph);
	CHECK(std::get<FCookedSoundGraph>(*LoadedGraph).Graph == Graph->Graph);
	CHECK(GetSoundGraphWaves(Graph->Graph) == std::vector{ToneId, LowToneId});

	// Corrupt data is rejected before it can drive allocations.
	for (std::size_t Cut = 9; Cut < GraphBytes->size(); Cut += 7)
	{
		CHECK_FALSE(DeserializeCookedAsset(std::span(*GraphBytes).first(Cut)));
	}

	FSoundGraphDesc Bad = Graph->Graph;
	Bad.Nodes[2].Inputs = {3};
	CHECK_FALSE(ValidateSoundGraph(Bad));
	Bad = Graph->Graph;
	Bad.Nodes[1].Play = 1;
	CHECK_FALSE(ValidateSoundGraph(Bad));
	Bad = Graph->Graph;
	Bad.Nodes[2].Control.Slot = 3;
	CHECK_FALSE(ValidateSoundGraph(Bad));
	Bad = Graph->Graph;
	std::swap(Bad.Nodes[3], Bad.Nodes[4]);
	CHECK_FALSE(ValidateSoundGraph(Bad));

	FCookedSound Empty = *Tone;
	Empty.Samples.clear();
	CHECK_FALSE(SerializeCookedAsset(Empty));
	FCookedSound Mismatched = *Tone;
	Mismatched.Samples.pop_back();
	CHECK_FALSE(ValidateCookedSound(Mismatched));
}

TEST_CASE("The null mixer plays sounds exactly, pans by position, and fades with distance")
{
	auto Engine = MakeNullEngine();
	const auto Tone = MakeTone(440.f, 48000);
	REQUIRE(Engine->AddSound(ToneId, Tone));

	Engine->Submit(FAudioPlayCommand{.Voice = {1}, .Sound = ToneId, .Volume = 0.5f});
	const auto Output = RenderFrames(*Engine, 1024);
	for (std::size_t Frame = 0; Frame < 1024; ++Frame)
	{
		const float Expected = static_cast<float>(Tone->Samples[Frame]) / 32768.f * 0.5f;
		REQUIRE(Output[Frame * 2] == doctest::Approx(Expected).epsilon(1e-5));
		REQUIRE(Output[Frame * 2 + 1] == doctest::Approx(Expected).epsilon(1e-5));
	}

	CHECK(Engine->IsVoicePlaying({1}));
	(void)RenderFrames(*Engine, 48000);
	CHECK_FALSE(Engine->IsVoicePlaying({1}));
	CHECK(Engine->GetStats().Voices == 0);

	// +X is left, so a source at -X plays in the right channel.
	const FAudioListener Listener{.Position = {}, .Forward = {0.f, 0.f, 1.f}, .Up = {0.f, 1.f, 0.f}};
	Engine->Submit(FAudioListenerCommand{.Listener = Listener});
	Engine->Submit(FAudioPlayCommand{.Voice = {2}, .Sound = ToneId, .bSpatial = true, .Position = {-3.f, 0.f, 0.f}});
	const auto Right = RenderFrames(*Engine, 4096);
	CHECK(Rms(Right, 1) > Rms(Right, 0) * 10.f);
	Engine->Submit(FAudioStopCommand{.Voice = {2}, .FadeSeconds = 0.f});

	const auto Measure = [&Engine](const FVector3& Position)
	{
		static std::uint32_t Voice = 100;
		Engine->Submit(FAudioPlayCommand{.Voice = {++Voice}, .Sound = ToneId, .bSpatial = true, .Position = Position, .MinimumDistance = 1.f, .MaximumDistance = 20.f});
		const auto Rendered = RenderFrames(*Engine, 4096);
		Engine->Submit(FAudioStopCommand{.Voice = {Voice}, .FadeSeconds = 0.f});
		(void)RenderFrames(*Engine, 256);
		return Rms(Rendered, 0) + Rms(Rendered, 1);
	};

	const float Near = Measure({0.f, 0.f, 1.f});
	const float Middle = Measure({0.f, 0.f, 4.f});
	const float Far = Measure({0.f, 0.f, 19.f});
	CHECK(Near > Middle * 3.f);
	CHECK(Middle > Far);
	CHECK(Measure({0.f, 0.f, 25.f}) == 0.f);
}

TEST_CASE("Sound concurrency steals or rejects instances over the limit")
{
	auto Engine = MakeNullEngine();
	auto Tone = MakeTone(440.f, 48000, true);
	Tone->Playback.MaximumInstances = 2;
	REQUIRE(Engine->AddSound(ToneId, Tone));
	for (std::uint32_t Voice = 1; Voice <= 3; ++Voice)
	{
		Engine->Submit(FAudioPlayCommand{.Voice = {Voice}, .Sound = ToneId});
	}

	(void)RenderFrames(*Engine, 2048);
	CHECK(Engine->GetStats().VoicesStolen == 1);
	CHECK_FALSE(Engine->IsVoicePlaying({1}));
	CHECK(Engine->IsVoicePlaying({2}));
	CHECK(Engine->IsVoicePlaying({3}));

	auto Rejecting = MakeTone(220.f, 48000, true);
	Rejecting->Playback.MaximumInstances = 1;
	Rejecting->Playback.Concurrency = ESoundConcurrency::Reject;
	REQUIRE(Engine->AddSound(LowToneId, Rejecting));
	Engine->Submit(FAudioPlayCommand{.Voice = {10}, .Sound = LowToneId});
	Engine->Submit(FAudioPlayCommand{.Voice = {11}, .Sound = LowToneId});
	(void)RenderFrames(*Engine, 256);
	CHECK(Engine->GetStats().VoicesRejected == 1);
	CHECK(Engine->IsVoicePlaying({10}));
	CHECK_FALSE(Engine->IsVoicePlaying({11}));

	// The voice pool steals the quietest voice once every slot is busy.
	for (std::uint32_t Voice = 0; Voice < MaximumAudioVoices + 8; ++Voice)
	{
		Engine->Submit(FAudioPlayCommand{.Voice = {1000 + Voice}, .Sound = ToneId, .Volume = 0.1f + static_cast<float>(Voice) * 0.01f});
	}

	(void)RenderFrames(*Engine, 256);
	CHECK(Engine->GetStats().Voices <= MaximumAudioVoices);

	Engine->Submit(FAudioStopAllCommand{.FadeSeconds = 0.01f});
	(void)RenderFrames(*Engine, 2048);
	CHECK(Engine->GetStats().Voices == 0);
}

TEST_CASE("Sound graphs pick waves on triggers, map parameters, and follow bools")
{
	auto Engine = MakeNullEngine();
	REQUIRE(Engine->AddSound(ToneId, MakeTone(440.f, 4800)));
	REQUIRE(Engine->AddSoundGraph(GraphId, MakeImpactGraph()));
	// The graph cannot play until all of its waves are in.
	Engine->Submit(FAudioPlayCommand{.Sound = GraphId, .Parameters = {{.Name = "Hit", .Value = 1.f}}});
	CHECK(Engine->GetStats().MissingSounds == 1);
	REQUIRE(Engine->AddSound(LowToneId, MakeTone(220.f, 4800)));

	const auto Impact = [&Engine](const float Impulse)
	{
		Engine->Submit(FAudioPlayCommand{.Sound = GraphId, .Parameters = {{.Name = "Hit", .Value = 1.f}, {.Name = "Impulse", .Value = Impulse}}});
		const auto Rendered = RenderFrames(*Engine, 4096);
		return Rms(Rendered, 0);
	};

	const float Soft = Impact(0.f);
	const float Hard = Impact(20.f);
	CHECK(Soft > 0.f);
	CHECK(Hard > Soft * 5.f);
	// Fire-and-forget graph voices end once their one-shot does.
	(void)RenderFrames(*Engine, 4800);
	CHECK(Engine->GetStats().Voices == 0);

	// An untriggered graph voice is idle and ends at once.
	Engine->Submit(FAudioPlayCommand{.Sound = GraphId});
	CHECK(Rms(RenderFrames(*Engine, 512), 0) == 0.f);
	CHECK(Engine->GetStats().Voices == 0);

	auto Loop = std::make_shared<FCookedSoundGraph>();
	Loop->Graph.Parameters = {{.Name = "Sliding", .Type = ESoundParameterType::Bool}};
	Loop->Graph.Nodes = {{.Type = ESoundNodeType::WavePlayer, .Waves = {ToneId}, .Play = 0, .bLoop = true}, {.Type = ESoundNodeType::Output, .Inputs = {0}}};
	Loop->Graph.ControlSlots = 1;
	constexpr FAssetId LoopId{1, 9};
	REQUIRE(Engine->AddSoundGraph(LoopId, Loop));
	Engine->Submit(FAudioPlayCommand{.Voice = {7}, .Sound = LoopId});
	CHECK(Rms(RenderFrames(*Engine, 1024), 0) == 0.f);
	CHECK(Engine->IsVoicePlaying({7}));
	Engine->Submit(FAudioParameterCommand{.Voice = {7}, .Parameter = {.Name = "Sliding", .Value = 1.f}});
	CHECK(Rms(RenderFrames(*Engine, 9600), 0) > 0.1f);
	Engine->Submit(FAudioParameterCommand{.Voice = {7}, .Parameter = {.Name = "Sliding", .Value = 0.f}});
	(void)RenderFrames(*Engine, 256);
	CHECK(Rms(RenderFrames(*Engine, 1024), 0) == 0.f);
	CHECK(Engine->IsVoicePlaying({7}));
	Engine->Submit(FAudioStopCommand{.Voice = {7}});
	(void)RenderFrames(*Engine, 4096);
	CHECK_FALSE(Engine->IsVoicePlaying({7}));
}

TEST_CASE("Oscillators sound their waveform at their frequency, envelopes shape and end them, and delays land on the exact sample")
{
	auto Engine = MakeNullEngine();
	const auto FirstSound = [](const std::span<const float> Samples)
	{
		for (std::size_t Frame = 0; Frame * 2 < Samples.size(); ++Frame)
		{
			if (Samples[Frame * 2] != 0.f)
			{
				return Frame;
			}
		}

		return Samples.size();
	};

	// A sine for a tenth of a second crosses zero twice a cycle, then the oscillator stops and the voice ends.
	auto Sine = std::make_shared<FCookedSoundGraph>();
	Sine->Graph.Nodes = {{.Type = ESoundNodeType::Oscillator, .Control = {.Constant = 440.f}, .Duration = 0.1f}, {.Type = ESoundNodeType::Output, .Inputs = {0}}};
	constexpr FAssetId SineId{3, 1};
	REQUIRE(Engine->AddSoundGraph(SineId, Sine));
	Engine->Submit(FAudioPlayCommand{.Sound = SineId});
	const auto Tone = RenderFrames(*Engine, 4800);
	int Crossings = 0;
	for (std::size_t Frame = 1; Frame < 4800; ++Frame)
	{
		Crossings += (Tone[Frame * 2 - 2] < 0.f) != (Tone[Frame * 2] < 0.f) ? 1 : 0;
	}

	CHECK(std::abs(Crossings - 88) <= 2);
	CHECK(Rms(Tone, 0) == doctest::Approx(0.7071f).epsilon(0.02));
	(void)RenderFrames(*Engine, 512);
	CHECK(Engine->GetStats().Voices == 0);

	// A play starts at the next mixing block, so delays are measured against the same sound without one. Renders a whole number of blocks long
	// keep every play at the same point of its block.
	std::uint64_t NextId = 10;
	const auto StartOf = [&](std::shared_ptr<FCookedSoundGraph> Graph, const float Delay)
	{
		Graph->Graph.Nodes[0].Delay = Delay;
		const FAssetId Id{3, NextId++};
		REQUIRE(Engine->AddSoundGraph(Id, std::move(Graph)));
		Engine->Submit(FAudioPlayCommand{.Sound = Id});
		return FirstSound(RenderFrames(*Engine, 8192));
	};

	// A square that waits 10 ms starts 480 samples later, inside a block, and the envelope it feeds ends the voice once it has released.
	const auto Blip = []()
	{
		auto Graph = std::make_shared<FCookedSoundGraph>();
		Graph->Graph.Nodes = {
		    {.Type = ESoundNodeType::Oscillator, .Control = {.Constant = 1000.f}, .Waveform = ESoundWaveform::Square},
		    {.Type = ESoundNodeType::Envelope, .Inputs = {0}, .Range = {0.005f, 0.01f, 0.5f, 0.02f}},
		    {.Type = ESoundNodeType::Output, .Inputs = {1}},
		};
		return Graph;
	};

	(void)RenderFrames(*Engine, 8192 - 4800 - 512 + 8192);
	const std::size_t Prompt = StartOf(Blip(), 0.f);
	CHECK(StartOf(Blip(), 0.01f) == Prompt + 480);
	CHECK(Engine->GetStats().Voices == 0);

	// A wave player's delay lands on its sample too.
	REQUIRE(Engine->AddSound(ToneId, MakeTone(440.f, 4800)));
	const auto Late = []()
	{
		auto Graph = std::make_shared<FCookedSoundGraph>();
		Graph->Graph.Nodes = {{.Type = ESoundNodeType::WavePlayer, .Waves = {ToneId}}, {.Type = ESoundNodeType::Output, .Inputs = {0}}};
		return Graph;
	};

	const std::size_t Now = StartOf(Late(), 0.f);
	CHECK(StartOf(Late(), 0.0123f) == Now + 590);

	// A Bool gate holds the sustain while it is on and releases when it turns off.
	auto Held = std::make_shared<FCookedSoundGraph>();
	Held->Graph.Parameters = {{.Name = "Hold", .Type = ESoundParameterType::Bool}};
	Held->Graph.Nodes = {
	    {.Type = ESoundNodeType::Oscillator, .Control = {.Constant = 200.f}, .Waveform = ESoundWaveform::Square},
	    {.Type = ESoundNodeType::Envelope, .Inputs = {0}, .Play = 0, .Range = {0.001f, 0.001f, 0.25f, 0.01f}},
	    {.Type = ESoundNodeType::Output, .Inputs = {1}},
	};
	Held->Graph.ControlSlots = 1;
	constexpr FAssetId HeldId{3, 4};
	REQUIRE(Engine->AddSoundGraph(HeldId, Held));
	Engine->Submit(FAudioPlayCommand{.Voice = {9}, .Sound = HeldId, .Parameters = {{.Name = "Hold", .Value = 1.f}}});
	(void)RenderFrames(*Engine, 1024);
	CHECK(Rms(RenderFrames(*Engine, 9600), 0) == doctest::Approx(0.25f).epsilon(0.02));
	Engine->Submit(FAudioParameterCommand{.Voice = {9}, .Parameter = {.Name = "Hold", .Value = 0.f}});
	(void)RenderFrames(*Engine, 1024);
	CHECK(Rms(RenderFrames(*Engine, 1024), 0) == 0.f);
	Engine->Submit(FAudioStopCommand{.Voice = {9}});
	(void)RenderFrames(*Engine, 1024);
}

TEST_CASE("Streamed sounds play the same samples as loaded ones, across the ring and loops")
{
	const Tests::FScratchDirectory Directory("AudioStream");
	// Longer than the ring, so the stream wraps it several times, and looping, so it wraps the file too.
	const auto Tone = MakeTone(333.f, 100003, true, 2);
	FCookedSound Streamed = *Tone;
	Streamed.bStream = true;
	const auto Bytes = SerializeCookedAsset(Streamed);
	REQUIRE(Bytes);
	const std::filesystem::path File = Directory.GetPath() / "Stream.hasset";
	std::vector<std::byte> Prefixed(17, std::byte{0xcd});
	Prefixed.insert(Prefixed.end(), Bytes->begin(), Bytes->end());
	Tests::WriteBytes(File, Prefixed);

	const auto Stream = ReadCookedSoundStream(File, 17);
	REQUIRE(Stream);
	CHECK(Stream->Frames == 100003);
	CHECK(Stream->Channels == 2);
	CHECK(Stream->bLoop);
	CHECK_FALSE(ReadCookedSoundStream(File, 0));

	auto Memory = MakeNullEngine();
	auto Disk = MakeNullEngine();
	REQUIRE(Memory->AddSound(StreamId, Tone));
	REQUIRE(Disk->AddSoundStream(StreamId, *Stream));
	for (FAudioEngine* const Engine : {Memory.get(), Disk.get()})
	{
		Engine->Submit(FAudioPlayCommand{.Voice = {1}, .Sound = StreamId, .Pitch = 1.37f});
	}

	const auto Expected = RenderFrames(*Memory, 300000);
	const auto Actual = RenderFrames(*Disk, 300000);
	CHECK(Rms(Expected, 0) > 0.1f);
	// The loaded copy wraps its position at the loop and the stream keeps counting, so they may differ in the last bits of interpolation.
	float Difference = 0.f;
	for (std::size_t Index = 0; Index < Expected.size(); ++Index)
	{
		Difference = std::max(Difference, std::abs(Expected[Index] - Actual[Index]));
	}

	CHECK(Difference < 1e-4f);
	CHECK(Disk->GetStats().StreamStarvedFrames == 0);
	CHECK(Disk->GetStats().Streams == 1);

	Disk->Submit(FAudioStopCommand{.Voice = {1}, .FadeSeconds = 0.f});
	(void)RenderFrames(*Disk, 512);
	(void)RenderFrames(*Disk, 512);
	CHECK(Disk->GetStats().Streams == 0);
}

TEST_CASE("The mixer renders the same output for the same commands and never allocates")
{
	const auto Run = [](std::size_t* const CountedAllocations)
	{
		auto Engine = MakeNullEngine();
		REQUIRE(Engine->AddSound(ToneId, MakeTone(440.f, 9000)));
		REQUIRE(Engine->AddSound(LowToneId, MakeTone(110.f, 48000, true, 2)));
		REQUIRE(Engine->AddSoundGraph(GraphId, MakeImpactGraph()));
		Engine->Submit(FAudioListenerCommand{.Listener = {.Position = {1.f, 0.f, 0.f}}});
		std::vector<float> Mixed;
		std::vector<float> Block(std::size_t{512} * 2);
		for (std::uint32_t Step = 0; Step < 200; ++Step)
		{
			const auto Position = FVector3{std::sin(static_cast<float>(Step) * 0.1f) * 6.f, 0.f, 3.f};
			Engine->Submit(FAudioPlayCommand{.Sound = GraphId, .bSpatial = true, .Position = Position, .Parameters = {{.Name = "Hit", .Value = 1.f}, {.Name = "Impulse", .Value = static_cast<float>(Step % 20)}}});
			if (Step % 25 == 0)
			{
				Engine->Submit(FAudioPlayCommand{.Voice = {Step + 1}, .Sound = LowToneId, .bSpatial = true, .Position = Position});
			}

			Engine->Submit(FAudioMoveCommand{.Voice = {1}, .Position = Position});
			Engine->Submit(FAudioBusVolumeCommand{.Bus = "Effects", .Volume = 0.5f + static_cast<float>(Step % 10) * 0.05f});
			bCountAllocations = true;
			Engine->Render(Block);
			bCountAllocations = false;
			Engine->Update(0.f);
			Mixed.insert(Mixed.end(), Block.begin(), Block.end());
		}

		*CountedAllocations = Allocations;
		return Mixed;
	};

	Allocations = 0;
	std::size_t First = 0;
	std::size_t Second = 0;
	const auto A = Run(&First);
	Allocations = 0;
	const auto B = Run(&Second);
	CHECK(A == B);
	CHECK(Rms(A, 0) > 0.01f);
#ifndef __SANITIZE_ADDRESS__
	CHECK(First == 0);
	CHECK(Second == 0);
#endif
}

TEST_CASE("The mixer stays consistent while another thread renders under command stress")
{
	auto Engine = MakeNullEngine();
	REQUIRE(Engine->AddSound(ToneId, MakeTone(440.f, 2400)));
	REQUIRE(Engine->AddSound(LowToneId, MakeTone(220.f, 4800, true)));
	REQUIRE(Engine->AddSoundGraph(GraphId, MakeImpactGraph()));

	std::atomic<bool> bRunning = true;
	std::atomic<std::size_t> RenderAllocations = 0;
	std::thread Renderer([&]
	{
		std::vector<float> Block(std::size_t{256} * 2);
		bCountAllocations = true;
		while (bRunning.load(std::memory_order_acquire))
		{
			Engine->Render(Block);
		}

		bCountAllocations = false;
		RenderAllocations = Allocations;
	});

	for (std::uint32_t Index = 0; Index < 50000; ++Index)
	{
		const std::uint32_t Voice = Index % 97 + 1;
		switch (Index % 5)
		{
			case 0:
				Engine->Submit(FAudioPlayCommand{.Voice = {Voice}, .Sound = LowToneId, .bSpatial = true, .Position = {static_cast<float>(Index % 13), 0.f, 2.f}});
				break;
			case 1:
				Engine->Submit(FAudioPlayCommand{.Sound = GraphId, .Parameters = {{.Name = "Hit", .Value = 1.f}, {.Name = "Impulse", .Value = 5.f}}});
				break;
			case 2:
				Engine->Submit(FAudioMoveCommand{.Voice = {Voice}, .Position = {0.f, 0.f, static_cast<float>(Index % 7)}});
				break;
			case 3:
				Engine->Submit(FAudioStopCommand{.Voice = {Voice}, .FadeSeconds = 0.01f});
				break;
			default:
				Engine->Submit(FAudioPlayCommand{.Sound = ToneId});
				break;
		}

		if (Index % 64 == 0)
		{
			Engine->Update(0.f);
			// Swapping a sound out while it plays keeps the old one alive until its voices end.
			REQUIRE(Engine->AddSound(ToneId, MakeTone(440.f + static_cast<float>(Index % 5), 2400)));
		}
	}

	// The queue may be full of the burst above, so the stop repeats until a block has run with every voice gone.
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < Deadline)
	{
		Engine->Submit(FAudioStopAllCommand{.FadeSeconds = 0.f});
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		Engine->Update(0.f);
		if (Engine->GetStats().Voices == 0)
		{
			break;
		}
	}

	bRunning = false;
	Renderer.join();
	Engine->Update(0.f);
	const FAudioStats Stats = Engine->GetStats();
	MESSAGE(std::format("{} commands, {} dropped, {} started, {} stolen, {} rejected, {:.1f} us average and {:.1f} us peak per {:.0f} us block", Stats.CommandsSubmitted, Stats.CommandsDropped, Stats.VoicesStarted, Stats.VoicesStolen, Stats.VoicesRejected, Stats.BlockMicrosecondsAverage, Stats.BlockMicrosecondsPeak, Stats.BlockBudgetMicroseconds));
	CHECK(Stats.Voices == 0);
	CHECK(Stats.CommandsSubmitted > 50000);
	CHECK(Stats.VoicesStarted > 1000);
	for (std::uint32_t Voice = 1; Voice <= 97; ++Voice)
	{
		CHECK_FALSE(Engine->IsVoicePlaying({Voice}));
	}
#ifndef __SANITIZE_ADDRESS__
	CHECK(RenderAllocations == 0);
#endif
}

TEST_CASE("Sound decoding reads WAV files and rejects garbage")
{
	// A 16-bit stereo WAV with a known ramp.
	constexpr std::uint32_t Frames = 1000;
	std::vector<std::int16_t> Ramp;
	for (std::uint32_t Frame = 0; Frame < Frames; ++Frame)
	{
		Ramp.push_back(static_cast<std::int16_t>(Frame * 10));
		Ramp.push_back(static_cast<std::int16_t>(-static_cast<std::int32_t>(Frame) * 10));
	}

	const std::vector<std::byte> Wav = Tests::EncodeWav(22050, 2, Ramp);
	const auto Decoded = DecodeSound(Wav);
	REQUIRE(Decoded);
	CHECK(Decoded->SampleRate == 22050);
	CHECK(Decoded->Channels == 2);
	REQUIRE(Decoded->Samples.size() == Frames * 2);
	CHECK(Decoded->Samples[std::size_t{2} * 500] == 5000);
	CHECK(Decoded->Samples[2 * 500 + 1] == -5000);

	std::vector<std::byte> Garbage(4096, std::byte{0x5a});
	CHECK_FALSE(DecodeSound(Garbage));
	CHECK_FALSE(DecodeSound({}));
}

TEST_CASE("Sound graphs with synthesis need the current cooked format, and older graphs still load")
{
	FCookedSoundGraph Graph;
	Graph.Graph.Nodes = {
	    {.Type = ESoundNodeType::Oscillator, .Control = {.Constant = 330.f}, .Waveform = ESoundWaveform::Triangle, .Delay = 0.25f, .Duration = 1.f},
	    {.Type = ESoundNodeType::Envelope, .Inputs = {0}, .Range = {0.1f, 0.2f, 0.4f, 0.5f}},
	    {.Type = ESoundNodeType::Output, .Inputs = {1}},
	};

	const auto Bytes = SerializeCookedAsset(Graph);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	CHECK(std::get<FCookedSoundGraph>(*Loaded).Graph == Graph.Graph);
	CHECK_FALSE(SerializeCookedAsset(Graph, 8));

	// Out-of-range synthesis settings are rejected before they reach the mixer.
	FCookedSoundGraph Bad = Graph;
	Bad.Graph.Nodes[1].Range[2] = 1.5f;
	CHECK_FALSE(SerializeCookedAsset(Bad));
	Bad = Graph;
	Bad.Graph.Nodes[0].Delay = -1.f;
	CHECK_FALSE(SerializeCookedAsset(Bad));

	FCookedSoundGraph Plain;
	Plain.Graph.Parameters = {{.Name = "Hit", .Type = ESoundParameterType::Trigger}};
	Plain.Graph.Nodes = {{.Type = ESoundNodeType::WavePlayer, .Waves = {ToneId}, .Play = 0}, {.Type = ESoundNodeType::Output, .Inputs = {0}}};
	Plain.Graph.ControlSlots = 1;
	const auto Older = SerializeCookedAsset(Plain, 8);
	REQUIRE(Older);
	const auto OlderLoaded = DeserializeCookedAsset(*Older);
	REQUIRE(OlderLoaded);
	CHECK(std::get<FCookedSoundGraph>(*OlderLoaded).Graph == Plain.Graph);
}
