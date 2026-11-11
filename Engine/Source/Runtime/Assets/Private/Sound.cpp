#include "SoundSerialization.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <set>

namespace Herta
{
namespace
{
inline constexpr std::size_t MaximumBusNameLength = 64;
inline constexpr std::size_t MaximumParameterNameLength = 64;
inline constexpr std::size_t MaximumNodeInputs = 16;
inline constexpr std::size_t MaximumNodePitches = 16;

[[nodiscard]] std::unexpected<FAssetError> Invalid(std::string Message)
{
	return std::unexpected(FAssetError{std::move(Message)});
}

void WritePlayback(FBinaryWriter& Writer, const FSoundPlayback& Playback)
{
	Writer.WriteString(Playback.Bus);
	Writer.WriteFloat(Playback.Volume);
	Writer.Write(Playback.MaximumInstances);
	Writer.Write(static_cast<std::uint8_t>(Playback.Concurrency));
	Writer.WriteFloat(Playback.MinimumDistance);
	Writer.WriteFloat(Playback.MaximumDistance);
}

[[nodiscard]] FSoundPlayback ReadPlayback(FBinaryReader& Reader)
{
	FSoundPlayback Playback;
	Playback.Bus = Reader.ReadString(MaximumBusNameLength);
	Playback.Volume = Reader.ReadFloat();
	Playback.MaximumInstances = Reader.Read<std::uint32_t>();
	Playback.Concurrency = static_cast<ESoundConcurrency>(Reader.Read<std::uint8_t>());
	Playback.MinimumDistance = Reader.ReadFloat();
	Playback.MaximumDistance = Reader.ReadFloat();
	return Playback;
}

void WriteControl(FBinaryWriter& Writer, const FSoundControl& Control)
{
	Writer.Write(Control.Slot);
	Writer.WriteFloat(Control.Constant);
}

[[nodiscard]] FSoundControl ReadControl(FBinaryReader& Reader)
{
	FSoundControl Control;
	Control.Slot = Reader.Read<std::uint32_t>();
	Control.Constant = Reader.ReadFloat();
	return Control;
}

[[nodiscard]] std::uint32_t ReadCount(FBinaryReader& Reader, const std::size_t Maximum)
{
	const auto Count = Reader.Read<std::uint32_t>();
	if (Count > Maximum)
	{
		Reader.Invalidate();
		return 0;
	}

	return Count;
}
}

std::expected<void, FAssetError> ValidateSoundPlayback(const FSoundPlayback& Playback)
{
	if (Playback.Bus.empty() || Playback.Bus.size() > MaximumBusNameLength)
	{
		return Invalid("A sound must name the bus it plays on");
	}

	if (!std::isfinite(Playback.Volume) || Playback.Volume < 0.f || Playback.Volume > 16.f)
	{
		return Invalid("Sound volume must be between 0 and 16");
	}

	if (Playback.MaximumInstances == 0 || Playback.MaximumInstances > MaximumSoundInstances)
	{
		return Invalid(std::format("Sound instance limits must be between 1 and {}", MaximumSoundInstances));
	}

	if (Playback.Concurrency > ESoundConcurrency::Reject)
	{
		return Invalid("Unknown sound concurrency policy");
	}

	if (!std::isfinite(Playback.MinimumDistance) || !std::isfinite(Playback.MaximumDistance) || Playback.MinimumDistance <= 0.f || Playback.MaximumDistance <= Playback.MinimumDistance || Playback.MaximumDistance > 10000.f)
	{
		return Invalid("Sound distances must be positive, with the maximum beyond the minimum");
	}

	return {};
}

std::expected<void, FAssetError> ValidateCookedSound(const FCookedSound& Sound)
{
	if (auto Valid = ValidateSoundPlayback(Sound.Playback); !Valid)
	{
		return Valid;
	}

	if (Sound.SampleRate < MinimumSoundSampleRate || Sound.SampleRate > MaximumSoundSampleRate)
	{
		return Invalid(std::format("Sound sample rates must be between {} and {} Hz", MinimumSoundSampleRate, MaximumSoundSampleRate));
	}

	if (Sound.Channels == 0 || Sound.Channels > MaximumSoundChannels)
	{
		return Invalid("Sounds must be mono or stereo");
	}

	if (Sound.Samples.empty() || Sound.Samples.size() > MaximumCookedSoundSamples || Sound.Samples.size() % Sound.Channels != 0)
	{
		return Invalid("A sound needs whole frames of samples within the size limit");
	}

	return {};
}

std::expected<void, FAssetError> ValidateSoundGraph(const FSoundGraphDesc& Graph)
{
	if (Graph.Parameters.size() > MaximumSoundGraphParameters || Graph.Nodes.empty() || Graph.Nodes.size() > MaximumSoundGraphNodes)
	{
		return Invalid(std::format("A sound graph needs 1 to {} nodes and at most {} parameters", MaximumSoundGraphNodes, MaximumSoundGraphParameters));
	}

	std::set<std::string_view> Names;
	for (const FSoundParameterDesc& Parameter : Graph.Parameters)
	{
		if (Parameter.Name.empty() || Parameter.Name.size() > MaximumParameterNameLength || !Names.insert(Parameter.Name).second || Parameter.Type > ESoundParameterType::Trigger || !std::isfinite(Parameter.Default))
		{
			return Invalid(std::format("Sound graph parameter '{}' needs a unique short name, a known type, and a finite default", Parameter.Name));
		}
	}

	// A control reads a parameter, or a Map Range that ran earlier in the frame.
	std::uint32_t Produced = static_cast<std::uint32_t>(Graph.Parameters.size());
	const auto IsReadable = [&Produced](const FSoundControl& Control)
	{
		return (Control.Slot == NoSoundControl || Control.Slot < Produced) && std::isfinite(Control.Constant);
	};

	std::size_t Waves = 0;
	for (std::size_t Index = 0; Index < Graph.Nodes.size(); ++Index)
	{
		const FSoundNodeDesc& Node = Graph.Nodes[Index];
		const bool bLast = Index + 1 == Graph.Nodes.size();
		if ((Node.Type == ESoundNodeType::Output) != bLast || Node.Type > ESoundNodeType::Envelope)
		{
			return Invalid("A sound graph ends with its only Output node");
		}

		const auto Error = [Index](const std::string_view Message)
		{
			return Invalid(std::format("Sound graph node {}: {}", Index, Message));
		};

		if (Node.Inputs.size() > MaximumNodeInputs || std::ranges::any_of(Node.Inputs, [Index](const std::uint32_t Input)
		    {
			    return Input >= Index;
		    }))
		{
			return Error("audio inputs must name earlier nodes");
		}

		const std::size_t Expected = Node.Type == ESoundNodeType::WavePlayer || Node.Type == ESoundNodeType::MapRange || Node.Type == ESoundNodeType::Oscillator ? 0 : 1;
		if (Node.Type == ESoundNodeType::Mixer ? Node.Inputs.empty() : Node.Inputs.size() != Expected)
		{
			return Error("wrong number of audio inputs");
		}

		if (!IsReadable(Node.Control))
		{
			return Error("its control reads a slot that is not set yet");
		}

		const bool bGated = Node.Type == ESoundNodeType::WavePlayer || Node.Type == ESoundNodeType::Oscillator || Node.Type == ESoundNodeType::Envelope;
		const bool bPlayValid = Node.Play == NoSoundControl || (Node.Play < Graph.Parameters.size() && Graph.Parameters[Node.Play].Type != ESoundParameterType::Float);
		if (!(Node.Delay >= 0.f && Node.Delay <= MaximumSoundDelaySeconds && Node.Duration >= 0.f && Node.Duration <= MaximumSoundDelaySeconds) || Node.Waveform > ESoundWaveform::Noise || (!bGated && (Node.Delay != 0.f || Node.Play != NoSoundControl)) || (bGated && !bPlayValid))
		{
			return Error("delays and durations are 0 to 60 seconds, only players, oscillators, and envelopes start late, and they start on a Bool or Trigger parameter");
		}

		if (Node.Type == ESoundNodeType::Oscillator && (Node.Pitches.size() > MaximumNodePitches || !std::ranges::all_of(Node.Pitches, IsReadable)))
		{
			return Error("an oscillator's pitches read set slots");
		}

		if (Node.Type == ESoundNodeType::Envelope && !(Node.Range[0] >= 0.f && Node.Range[1] >= 0.f && Node.Range[2] >= 0.f && Node.Range[2] <= 1.f && Node.Range[3] >= 0.f && Node.Range[0] <= MaximumSoundDelaySeconds && Node.Range[1] <= MaximumSoundDelaySeconds && Node.Range[3] <= MaximumSoundDelaySeconds))
		{
			return Error("an envelope's attack, decay, and release are 0 to 60 seconds and its sustain is a level from 0 to 1");
		}

		if (Node.Type == ESoundNodeType::WavePlayer)
		{
			Waves += Node.Waves.size();
			if (Node.Waves.empty() || Waves > MaximumSoundGraphWaves || !std::ranges::all_of(Node.Waves, &FAssetId::IsValid) || !bPlayValid)
			{
				return Error("a wave player needs waves and plays on a Bool or Trigger parameter");
			}

			if (!(Node.PitchVariation >= 0.f && Node.PitchVariation <= 1.f && Node.GainVariation >= 0.f && Node.GainVariation <= 1.f) || Node.Pitches.size() > MaximumNodePitches || !std::ranges::all_of(Node.Pitches, IsReadable))
			{
				return Error("variations are fractions from 0 to 1, and pitches read set slots");
			}
		}

		if (Node.Type == ESoundNodeType::MapRange)
		{
			if (Node.Slot != Produced || Node.Control.Slot == NoSoundControl || !std::ranges::all_of(Node.Range, [](const float Value)
			    {
				    return std::isfinite(Value);
			    }) || Node.Range[0] == Node.Range[1])
			{
				return Error("a Map Range reads a slot, has a finite non-empty input range, and writes the next slot");
			}

			++Produced;
		}
		else if (Node.Slot != NoSoundControl)
		{
			return Error("only Map Range nodes write slots");
		}
	}

	if (Graph.ControlSlots != Produced)
	{
		return Invalid("A sound graph's control slots are its parameters and Map Range nodes");
	}

	return {};
}

std::vector<FAssetId> GetSoundGraphWaves(const FSoundGraphDesc& Graph)
{
	std::vector<FAssetId> Waves;
	for (const FSoundNodeDesc& Node : Graph.Nodes)
	{
		for (const FAssetId& Wave : Node.Waves)
		{
			// A plain comparison; MSVC's vectorized find rejects 16-byte elements under Clang.
			if (std::ranges::none_of(Waves, [&Wave](const FAssetId& Known)
			    {
				    return Known == Wave;
			    }))
			{
				Waves.push_back(Wave);
			}
		}
	}

	return Waves;
}

void WriteCookedSound(FBinaryWriter& Writer, const FCookedSound& Sound)
{
	WritePlayback(Writer, Sound.Playback);
	Writer.Write(Sound.SampleRate);
	Writer.Write(Sound.Channels);
	Writer.Write(static_cast<std::uint8_t>(Sound.bLoop ? 1 : 0));
	Writer.Write(static_cast<std::uint8_t>(Sound.bStream ? 1 : 0));
	Writer.Write(static_cast<std::uint64_t>(Sound.Samples.size()));
	for (const std::int16_t Sample : Sound.Samples)
	{
		Writer.Write(std::bit_cast<std::uint16_t>(Sample));
	}
}

std::uint64_t ReadCookedSoundHeader(FBinaryReader& Reader, FCookedSound& Sound)
{
	Sound.Playback = ReadPlayback(Reader);
	Sound.SampleRate = Reader.Read<std::uint32_t>();
	Sound.Channels = Reader.Read<std::uint32_t>();
	Sound.bLoop = Reader.Read<std::uint8_t>() != 0;
	Sound.bStream = Reader.Read<std::uint8_t>() != 0;
	return Reader.Read<std::uint64_t>();
}

FCookedSound ReadCookedSound(FBinaryReader& Reader)
{
	FCookedSound Sound;
	const std::uint64_t Count = ReadCookedSoundHeader(Reader, Sound);
	if (Count > MaximumCookedSoundSamples || !Reader.CanRead(Count, sizeof(std::int16_t)))
	{
		Reader.Invalidate();
		return Sound;
	}

	Sound.Samples.resize(static_cast<std::size_t>(Count));
	for (std::int16_t& Sample : Sound.Samples)
	{
		Sample = std::bit_cast<std::int16_t>(Reader.Read<std::uint16_t>());
	}

	return Sound;
}

bool NeedsSynthesisFormat(const FSoundGraphDesc& Graph)
{
	return std::ranges::any_of(Graph.Nodes, [](const FSoundNodeDesc& Node)
	{
		return Node.Type > ESoundNodeType::Output || Node.Delay != 0.f || Node.Duration != 0.f || Node.Waveform != ESoundWaveform::Sine;
	});
}

void WriteCookedSoundGraph(FBinaryWriter& Writer, const FCookedSoundGraph& Graph, const std::uint32_t Version)
{
	WritePlayback(Writer, Graph.Playback);
	Writer.Write(static_cast<std::uint32_t>(Graph.Graph.Parameters.size()));
	for (const FSoundParameterDesc& Parameter : Graph.Graph.Parameters)
	{
		Writer.WriteString(Parameter.Name);
		Writer.Write(static_cast<std::uint8_t>(Parameter.Type));
		Writer.WriteFloat(Parameter.Default);
	}

	Writer.Write(static_cast<std::uint32_t>(Graph.Graph.Nodes.size()));
	for (const FSoundNodeDesc& Node : Graph.Graph.Nodes)
	{
		Writer.Write(static_cast<std::uint8_t>(Node.Type));
		Writer.Write(static_cast<std::uint32_t>(Node.Inputs.size()));
		for (const std::uint32_t Input : Node.Inputs)
		{
			Writer.Write(Input);
		}

		WriteControl(Writer, Node.Control);
		Writer.Write(static_cast<std::uint32_t>(Node.Waves.size()));
		for (const FAssetId& Wave : Node.Waves)
		{
			Writer.Write(Wave.GetHigh());
			Writer.Write(Wave.GetLow());
		}

		Writer.Write(Node.Play);
		Writer.Write(static_cast<std::uint8_t>(Node.bLoop ? 1 : 0));
		Writer.WriteFloat(Node.PitchVariation);
		Writer.WriteFloat(Node.GainVariation);
		Writer.Write(static_cast<std::uint32_t>(Node.Pitches.size()));
		for (const FSoundControl& Pitch : Node.Pitches)
		{
			WriteControl(Writer, Pitch);
		}

		for (const float Value : Node.Range)
		{
			Writer.WriteFloat(Value);
		}

		Writer.Write(Node.Slot);
		if (Version >= 9)
		{
			Writer.Write(static_cast<std::uint8_t>(Node.Waveform));
			Writer.WriteFloat(Node.Delay);
			Writer.WriteFloat(Node.Duration);
		}
	}

	Writer.Write(Graph.Graph.ControlSlots);
}

FCookedSoundGraph ReadCookedSoundGraph(FBinaryReader& Reader, const std::uint32_t Version)
{
	FCookedSoundGraph Graph;
	Graph.Playback = ReadPlayback(Reader);
	Graph.Graph.Parameters.resize(ReadCount(Reader, MaximumSoundGraphParameters));
	for (FSoundParameterDesc& Parameter : Graph.Graph.Parameters)
	{
		Parameter.Name = Reader.ReadString(MaximumParameterNameLength);
		Parameter.Type = static_cast<ESoundParameterType>(Reader.Read<std::uint8_t>());
		Parameter.Default = Reader.ReadFloat();
	}

	Graph.Graph.Nodes.resize(ReadCount(Reader, MaximumSoundGraphNodes));
	for (FSoundNodeDesc& Node : Graph.Graph.Nodes)
	{
		Node.Type = static_cast<ESoundNodeType>(Reader.Read<std::uint8_t>());
		Node.Inputs.resize(ReadCount(Reader, MaximumNodeInputs));
		for (std::uint32_t& Input : Node.Inputs)
		{
			Input = Reader.Read<std::uint32_t>();
		}

		Node.Control = ReadControl(Reader);
		Node.Waves.resize(ReadCount(Reader, MaximumSoundGraphWaves));
		for (FAssetId& Wave : Node.Waves)
		{
			const auto High = Reader.Read<std::uint64_t>();
			Wave = FAssetId(High, Reader.Read<std::uint64_t>());
		}

		Node.Play = Reader.Read<std::uint32_t>();
		Node.bLoop = Reader.Read<std::uint8_t>() != 0;
		Node.PitchVariation = Reader.ReadFloat();
		Node.GainVariation = Reader.ReadFloat();
		Node.Pitches.resize(ReadCount(Reader, MaximumNodePitches));
		for (FSoundControl& Pitch : Node.Pitches)
		{
			Pitch = ReadControl(Reader);
		}

		for (float& Value : Node.Range)
		{
			Value = Reader.ReadFloat();
		}

		Node.Slot = Reader.Read<std::uint32_t>();
		if (Version >= 9)
		{
			Node.Waveform = static_cast<ESoundWaveform>(Reader.Read<std::uint8_t>());
			Node.Delay = Reader.ReadFloat();
			Node.Duration = Reader.ReadFloat();
		}
	}

	Graph.Graph.ControlSlots = Reader.Read<std::uint32_t>();
	return Graph;
}
}
