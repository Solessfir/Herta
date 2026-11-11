#include "TestFiles.h"

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AudioGraph/AudioGraph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <format>

using namespace Herta;

namespace
{
constexpr FAssetId HitA{2, 1};
constexpr FAssetId HitB{2, 2};

struct FGraphBuilder
{
	FGraphNodeId Add(const std::string_view Type, std::vector<FGraphProperty> Properties = {})
	{
		const FGraphNodeId Id = NextGraphNodeId(Graph);
		auto Node = MakeGraphNode(GetAudioGraphSchema(), Type, Id, {static_cast<float>(Id.Value) * 200.f, 0.f});
		REQUIRE(Node);
		for (FGraphProperty& Property : Properties)
		{
			std::ranges::find(Node->Properties, Property.Key, &FGraphProperty::Key)->Value = std::move(Property.Value);
		}

		Graph.Nodes.push_back(std::move(*Node));
		return Id;
	}

	void Link(const FGraphNodeId From, const std::string_view FromPin, const FGraphNodeId To, const std::string_view ToPin)
	{
		Graph.Links.push_back({.From = {.Node = From, .Key = std::string(FromPin)}, .To = {.Node = To, .Key = std::string(ToPin)}});
	}

	FGraph Graph{.Domain = std::string(AudioGraphDomain), .DomainVersion = AudioGraphVersion};
};

[[nodiscard]] std::optional<FAssetId> ResolveTestWave(const std::string_view Path)
{
	return Path == "Audio/HitA.wav" ? std::optional(HitA) : Path == "Audio/HitB.wav" ? std::optional(HitB) : std::nullopt;
}

// Trigger -> random wave -> pitch and gain from an impulse -> low pass -> output.
[[nodiscard]] FGraph MakeImpactGraph()
{
	FGraphBuilder Builder;
	const FGraphNodeId Hit = Builder.Add("TriggerInput", {{.Key = "name", .Value = std::string("Hit")}});
	const FGraphNodeId Impulse = Builder.Add("FloatInput", {{.Key = "name", .Value = std::string("Impulse")}});
	const FGraphNodeId Player = Builder.Add("WavePlayer", {{.Key = "waves", .Value = std::string("Audio/HitA.wav, Audio/HitB.wav")}, {.Key = "pitchVariation", .Value = 0.05f}});
	const FGraphNodeId Loudness = Builder.Add("MapRange", {{.Key = "inMax", .Value = 20.f}, {.Key = "outMin", .Value = 0.2f}});
	const FGraphNodeId Tone = Builder.Add("MapRange", {{.Key = "inMax", .Value = 20.f}, {.Key = "outMin", .Value = 1.15f}, {.Key = "outMax", .Value = 0.85f}});
	const FGraphNodeId Pitch = Builder.Add("Pitch");
	const FGraphNodeId Gain = Builder.Add("Gain");
	const FGraphNodeId Filter = Builder.Add("LowPass", {{.Key = "cutoff", .Value = 6000.f}});
	const FGraphNodeId Output = Builder.Add("Output");
	Builder.Link(Hit, "Fired", Player, "Play");
	Builder.Link(Impulse, "Value", Loudness, "Value");
	Builder.Link(Impulse, "Value", Tone, "Value");
	Builder.Link(Player, "Out", Pitch, "In");
	Builder.Link(Tone, "Result", Pitch, "Pitch");
	Builder.Link(Pitch, "Out", Gain, "In");
	Builder.Link(Loudness, "Result", Gain, "Gain");
	Builder.Link(Gain, "Out", Filter, "In");
	Builder.Link(Filter, "Out", Output, "In");
	return Builder.Graph;
}
}

TEST_CASE("Audio Graphs compile to sound graphs with pitches folded into their players")
{
	const FGraph Graph = MakeImpactGraph();
	CHECK(ValidateGraph(Graph, GetAudioGraphSchema()).empty());
	const auto Compiled = CompileAudioGraph(Graph, ResolveTestWave);
	REQUIRE(Compiled);
	const FSoundGraphDesc& Desc = Compiled->Graph;
	REQUIRE(Desc.Parameters.size() == 2);
	CHECK(Desc.Parameters[0].Name == "Hit");
	CHECK(Desc.Parameters[0].Type == ESoundParameterType::Trigger);
	CHECK(Desc.Parameters[1].Type == ESoundParameterType::Float);
	CHECK(Desc.ControlSlots == 4);
	CHECK(ValidateSoundGraph(Desc));

	const auto Player = std::ranges::find(Desc.Nodes, ESoundNodeType::WavePlayer, &FSoundNodeDesc::Type);
	REQUIRE(Player != Desc.Nodes.end());
	CHECK(Player->Waves == std::vector{HitA, HitB});
	CHECK(Player->Play == 0);
	REQUIRE(Player->Pitches.size() == 1);
	CHECK(Player->Pitches[0].Slot != NoSoundControl);
	CHECK(std::ranges::count(Desc.Nodes, ESoundNodeType::MapRange, &FSoundNodeDesc::Type) == 2);
	CHECK(Desc.Nodes.back().Type == ESoundNodeType::Output);
	CHECK(Compiled->WavePaths == std::vector<std::string>{"Audio/HitA.wav", "Audio/HitB.wav"});

	const auto Text = SerializeGraph(Graph, GetAudioGraphSchema());
	REQUIRE(Text);
	const auto Loaded = DeserializeGraph(*Text, GetAudioGraphSchema());
	REQUIRE(Loaded);
	// Canonical text orders links by the input they feed, so the loaded graph saves the same bytes.
	CHECK(Loaded->Nodes == Graph.Nodes);
	const auto Saved = SerializeGraph(*Loaded, GetAudioGraphSchema());
	REQUIRE(Saved);
	CHECK(*Saved == *Text);
}

TEST_CASE("Audio Graph compilation reports missing waves, links, and fan-out")
{
	const auto ErrorsOf = [](const FGraph& Graph)
	{
		const auto Compiled = CompileAudioGraph(Graph, ResolveTestWave);
		return Compiled ? std::vector<FGraphDiagnostic>{} : Compiled.error();
	};

	FGraph Missing = MakeImpactGraph();
	std::ranges::find(Missing.Nodes, std::string("WavePlayer"), &FGraphNode::Type)->Properties[0].Value = std::string("Audio/Nope.wav");
	CHECK_FALSE(ErrorsOf(Missing).empty());

	FGraphBuilder Unlinked;
	const FGraphNodeId Gain = Unlinked.Add("Gain");
	const FGraphNodeId Output = Unlinked.Add("Output");
	Unlinked.Link(Gain, "Out", Output, "In");
	const auto UnlinkedErrors = ErrorsOf(Unlinked.Graph);
	REQUIRE_FALSE(UnlinkedErrors.empty());
	CHECK(UnlinkedErrors[0].Node == Gain);

	FGraphBuilder NoOutput;
	(void)NoOutput.Add("WavePlayer", {{.Key = "waves", .Value = std::string("Audio/HitA.wav")}});
	CHECK_FALSE(ErrorsOf(NoOutput.Graph).empty());

	FGraphBuilder FanOut;
	const FGraphNodeId Player = FanOut.Add("WavePlayer", {{.Key = "waves", .Value = std::string("Audio/HitA.wav")}});
	const FGraphNodeId Mixer = FanOut.Add("Mixer");
	const FGraphNodeId Final = FanOut.Add("Output");
	FanOut.Link(Player, "Out", Mixer, "In1");
	FanOut.Link(Player, "Out", Mixer, "In2");
	FanOut.Link(Mixer, "Out", Final, "In");
	CHECK_FALSE(ErrorsOf(FanOut.Graph).empty());

	// A Float cannot start a wave player, and the schema rejects that link before compiling.
	FGraphBuilder Typed;
	const FGraphNodeId Value = Typed.Add("FloatInput");
	const FGraphNodeId Wave = Typed.Add("WavePlayer", {{.Key = "waves", .Value = std::string("Audio/HitA.wav")}});
	CHECK_FALSE(CanLinkGraphPins(Typed.Graph, GetAudioGraphSchema(), {.From = {.Node = Value, .Key = "Value"}, .To = {.Node = Wave, .Key = "Play"}}));

	// Unreachable nodes only warn.
	FGraph Extra = MakeImpactGraph();
	FGraphBuilder Spare{.Graph = Extra};
	(void)Spare.Add("HighPass");
	const auto Compiled = CompileAudioGraph(Spare.Graph, ResolveTestWave);
	REQUIRE(Compiled);
	REQUIRE(Compiled->Diagnostics.size() == 1);
	CHECK(Compiled->Diagnostics[0].Severity == EGraphSeverity::Warning);
}

TEST_CASE("Sounds and Audio Graphs cook from content with their playback settings")
{
	const Tests::FScratchDirectory Scratch("SoundCook");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	std::vector<std::int16_t> Samples(4410);
	for (std::size_t Index = 0; Index < Samples.size(); ++Index)
	{
		Samples[Index] = static_cast<std::int16_t>(std::sin(static_cast<float>(Index) * 0.1f) * 12000.f);
	}

	Tests::WriteBytes(Content / "Audio/HitA.wav", Tests::EncodeWav(44100, 1, Samples));
	Tests::WriteBytes(Content / "Audio/HitB.wav", Tests::EncodeWav(22050, 1, Samples));
	Tests::WriteText(Content / "Audio/HitA.wav.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000a01\nImporter = Sound\nSetting.Bus = Effects\nSetting.MaximumInstances = 4\nSetting.Concurrency = StealQuietest\nSetting.Loop = true\n");
	Tests::WriteText(Content / "Audio/HitB.wav.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000a02\nImporter = Sound\nSetting.Stream = true\n");
	const auto Graph = SerializeGraph(MakeImpactGraph(), GetAudioGraphSchema());
	REQUIRE(Graph);
	Tests::WriteText(Content / "Audio/Impact.haudiograph", *Graph);
	Tests::WriteText(Content / "Audio/Impact.haudiograph.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000a03\nImporter = AudioGraph\nSetting.MaximumDistance = 60\n");

	const auto Cook = [&](const std::string_view Path)
	{
		const auto Result = CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = std::string(Path), .TargetPlatform = "TestPlatform"});
		REQUIRE(Result);
		auto Asset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Result->Key);
		REQUIRE(Asset);
		return std::move(*Asset);
	};

	const FCookedAsset A = Cook("Audio/HitA.wav");
	const auto& SoundA = std::get<FCookedSound>(A);
	CHECK(SoundA.SampleRate == 44100);
	CHECK(SoundA.Channels == 1);
	CHECK(SoundA.Samples == Samples);
	CHECK(SoundA.bLoop);
	CHECK_FALSE(SoundA.bStream);
	CHECK(SoundA.Playback.MaximumInstances == 4);
	CHECK(SoundA.Playback.Concurrency == ESoundConcurrency::StealQuietest);
	CHECK(std::get<FCookedSound>(Cook("Audio/HitB.wav")).bStream);

	const FCookedAsset Impact = Cook("Audio/Impact.haudiograph");
	const auto& Sound = std::get<FCookedSoundGraph>(Impact);
	CHECK(Sound.Playback.MaximumDistance == 60.f);
	const std::vector<FAssetId> Waves = GetSoundGraphWaves(Sound.Graph);
	REQUIRE(Waves.size() == 2);
	CHECK(Waves[0].ToString() == "00000000-0000-4000-8000-000000000a01");

	// A wave's new identity changes the graph's build key, since the graph cooks that identity in.
	const auto Before = CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Audio/Impact.haudiograph", .TargetPlatform = "TestPlatform"});
	Tests::WriteText(Content / "Audio/HitA.wav.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000a09\nImporter = Sound\n");
	const auto After = CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Audio/Impact.haudiograph", .TargetPlatform = "TestPlatform"});
	REQUIRE(Before);
	REQUIRE(After);
	CHECK(Before->Key != After->Key);

	Tests::WriteText(Content / "Audio/HitB.wav.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000a02\nImporter = Sound\nSetting.Loudness = 3\n");
	CHECK_FALSE(CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Audio/HitB.wav", .TargetPlatform = "TestPlatform"}));
}

TEST_CASE("Audio Graphs compile oscillators, envelopes, delays, and subgraphs that share inputs by name")
{
	// The subgraph: a gated square blip.
	FGraphBuilder Inner;
	const FGraphNodeId InnerHit = Inner.Add("TriggerInput", {{.Key = "name", .Value = std::string("Hit")}});
	const FGraphNodeId Detune = Inner.Add("FloatInput", {{.Key = "name", .Value = std::string("Detune")}, {.Key = "default", .Value = 1.f}});
	const FGraphNodeId Square = Inner.Add("Oscillator", {{.Key = "waveform", .Value = std::string("Square")}, {.Key = "frequency", .Value = 880.f}, {.Key = "delay", .Value = 0.05f}});
	const FGraphNodeId Shape = Inner.Add("Envelope", {{.Key = "release", .Value = 0.5f}});
	const FGraphNodeId InnerOutput = Inner.Add("Output");
	Inner.Link(InnerHit, "Fired", Shape, "Gate");
	Inner.Link(Detune, "Value", Square, "Frequency");
	Inner.Link(Square, "Out", Shape, "In");
	Inner.Link(Shape, "Out", InnerOutput, "In");

	// The outer graph shares Hit and gains the subgraph's Detune.
	FGraphBuilder Outer;
	const FGraphNodeId Hit = Outer.Add("TriggerInput", {{.Key = "name", .Value = std::string("Hit")}});
	const FGraphNodeId Blip = Outer.Add("Subgraph", {{.Key = "graph", .Value = std::string("Audio/Blip.haudiograph")}});
	const FGraphNodeId Gain = Outer.Add("Gain", {{.Key = "gain", .Value = 0.5f}});
	const FGraphNodeId Output = Outer.Add("Output");
	Outer.Link(Blip, "Out", Gain, "In");
	Outer.Link(Gain, "Out", Output, "In");
	(void)Hit;

	const auto Load = [&Inner](const std::string_view Path) -> std::optional<FGraph>
	{
		return Path == "Audio/Blip.haudiograph" ? std::optional(Inner.Graph) : std::nullopt;
	};

	const auto Compiled = CompileAudioGraph(Outer.Graph, ResolveTestWave, Load);
	REQUIRE_MESSAGE(Compiled, (Compiled ? std::string() : Compiled.error().front().Message));
	const FSoundGraphDesc& Desc = Compiled->Graph;
	REQUIRE(Desc.Parameters.size() == 2);
	CHECK(Desc.Parameters[0].Name == "Hit");
	CHECK(Desc.Parameters[1].Name == "Detune");
	CHECK(Compiled->GraphPaths == std::vector<std::string>{"Audio/Blip.haudiograph"});
	const auto Oscillator = std::ranges::find(Desc.Nodes, ESoundNodeType::Oscillator, &FSoundNodeDesc::Type);
	const auto Envelope = std::ranges::find(Desc.Nodes, ESoundNodeType::Envelope, &FSoundNodeDesc::Type);
	REQUIRE(Oscillator != Desc.Nodes.end());
	REQUIRE(Envelope != Desc.Nodes.end());
	CHECK(Oscillator->Waveform == ESoundWaveform::Square);
	CHECK(Oscillator->Delay == doctest::Approx(0.05f));
	CHECK(Oscillator->Control.Slot == 1);
	CHECK(Envelope->Play == 0);
	CHECK(Envelope->Range[3] == doctest::Approx(0.5f));
	CHECK(ValidateSoundGraph(Desc));

	// A subgraph that cannot load, or that copies itself in, is reported on the Subgraph node.
	const auto Missing = CompileAudioGraph(Outer.Graph, ResolveTestWave);
	REQUIRE_FALSE(Missing);
	CHECK(Missing.error().front().Node == Blip);
	FGraphBuilder Loop = Outer;
	const auto Self = [&Loop](const std::string_view) -> std::optional<FGraph>
	{
		return Loop.Graph;
	};

	const auto Recursive = CompileAudioGraph(Loop.Graph, ResolveTestWave, Self);
	REQUIRE_FALSE(Recursive);
	CHECK(Recursive.error().front().Node == Blip);

	// A version 1 graph upgrades on load.
	std::string Text = *SerializeGraph(MakeImpactGraph(), GetAudioGraphSchema());
	const std::string Current = std::format("\"domainVersion\": {}", AudioGraphVersion);
	REQUIRE(Text.find(Current) != std::string::npos);
	Text.replace(Text.find(Current), Current.size(), "\"domainVersion\": 1");
	const auto Upgraded = DeserializeGraph(Text, GetAudioGraphSchema());
	REQUIRE_MESSAGE(Upgraded, (Upgraded ? std::string() : Upgraded.error().Message));
	CHECK(Upgraded->DomainVersion == AudioGraphVersion);
}
