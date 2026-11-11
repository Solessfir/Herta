#include "Herta/AudioGraph/AudioGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <string>

namespace Herta
{
namespace
{
constexpr std::uint32_t AudioPin = 1;
constexpr std::uint32_t FloatPin = 2;
constexpr std::uint32_t BoolPin = 3;
constexpr std::uint32_t TriggerPin = 4;
// A wave player's Play input, which takes a Bool or a Trigger.
constexpr std::uint32_t PlayPin = 5;

constexpr std::array AudioOutputs{FGraphPinDescriptor{.Key = "Out", .Name = "Out", .Type = AudioPin}};
constexpr std::array PlayerInputs{FGraphPinDescriptor{.Key = "Play", .Name = "Play", .Type = PlayPin}};
constexpr std::array MixerInputs{
    FGraphPinDescriptor{.Key = "In1", .Name = "In 1", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "In2", .Name = "In 2", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "In3", .Name = "In 3", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "In4", .Name = "In 4", .Type = AudioPin},
};
constexpr std::array GainInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "Gain", .Name = "Gain", .Type = FloatPin},
};
constexpr std::array PitchInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "Pitch", .Name = "Pitch", .Type = FloatPin},
};
constexpr std::array FilterInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "Cutoff", .Name = "Cutoff", .Type = FloatPin},
};
constexpr std::array OscillatorInputs{
    FGraphPinDescriptor{.Key = "Play", .Name = "Play", .Type = PlayPin},
    FGraphPinDescriptor{.Key = "Frequency", .Name = "Frequency", .Type = FloatPin},
};
constexpr std::array EnvelopeInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = AudioPin},
    FGraphPinDescriptor{.Key = "Gate", .Name = "Gate", .Type = PlayPin},
};
constexpr std::array MapRangeInputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = FloatPin}};
constexpr std::array MapRangeOutputs{FGraphPinDescriptor{.Key = "Result", .Name = "Result", .Type = FloatPin}};
constexpr std::array FloatOutputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = FloatPin}};
constexpr std::array BoolOutputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = BoolPin}};
constexpr std::array TriggerOutputs{FGraphPinDescriptor{.Key = "Fired", .Name = "Fired", .Type = TriggerPin}};
constexpr std::array OutputInputs{FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = AudioPin}};

const std::array PlayerProperties{
    FGraphPropertyDescriptor{.Key = "waves", .Name = "Waves", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "loop", .Name = "Loop", .Default = false},
    FGraphPropertyDescriptor{.Key = "pitchVariation", .Name = "Pitch Variation", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "gainVariation", .Name = "Gain Variation", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "delay", .Name = "Delay (s)", .Default = 0.f},
};
const std::array OscillatorProperties{
    FGraphPropertyDescriptor{.Key = "waveform", .Name = "Waveform", .Default = std::string("Sine")},
    FGraphPropertyDescriptor{.Key = "frequency", .Name = "Frequency (Hz)", .Default = 440.f},
    FGraphPropertyDescriptor{.Key = "duration", .Name = "Duration (s)", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "delay", .Name = "Delay (s)", .Default = 0.f},
};
const std::array EnvelopeProperties{
    FGraphPropertyDescriptor{.Key = "attack", .Name = "Attack (s)", .Default = 0.01f},
    FGraphPropertyDescriptor{.Key = "decay", .Name = "Decay (s)", .Default = 0.1f},
    FGraphPropertyDescriptor{.Key = "sustain", .Name = "Sustain", .Default = 0.7f},
    FGraphPropertyDescriptor{.Key = "release", .Name = "Release (s)", .Default = 0.3f},
    FGraphPropertyDescriptor{.Key = "delay", .Name = "Delay (s)", .Default = 0.f},
};
const std::array SubgraphProperties{FGraphPropertyDescriptor{.Key = "graph", .Name = "Graph", .Default = std::string()}};
const std::array GainProperties{FGraphPropertyDescriptor{.Key = "gain", .Name = "Gain", .Default = 1.f}};
const std::array PitchProperties{FGraphPropertyDescriptor{.Key = "pitch", .Name = "Pitch", .Default = 1.f}};
const std::array LowPassProperties{FGraphPropertyDescriptor{.Key = "cutoff", .Name = "Cutoff (Hz)", .Default = 2000.f}};
const std::array HighPassProperties{FGraphPropertyDescriptor{.Key = "cutoff", .Name = "Cutoff (Hz)", .Default = 200.f}};
const std::array MapRangeProperties{
    FGraphPropertyDescriptor{.Key = "inMin", .Name = "In Min", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "inMax", .Name = "In Max", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "outMin", .Name = "Out Min", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "outMax", .Name = "Out Max", .Default = 1.f},
};
const std::array FloatProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("Value")},
    FGraphPropertyDescriptor{.Key = "default", .Name = "Default", .Default = 0.f},
};
const std::array BoolProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("On")},
    FGraphPropertyDescriptor{.Key = "default", .Name = "Default", .Default = false},
};
const std::array TriggerProperties{FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("Play")}};

const std::array AudioNodes{
    FGraphNodeDescriptor{.Type = "WavePlayer", .Name = "Wave Player", .Category = "Sources", .Inputs = PlayerInputs, .Outputs = AudioOutputs, .Properties = PlayerProperties},
    FGraphNodeDescriptor{.Type = "Oscillator", .Name = "Oscillator", .Category = "Sources", .Inputs = OscillatorInputs, .Outputs = AudioOutputs, .Properties = OscillatorProperties},
    FGraphNodeDescriptor{.Type = "Subgraph", .Name = "Subgraph", .Category = "Sources", .Outputs = AudioOutputs, .Properties = SubgraphProperties},
    FGraphNodeDescriptor{.Type = "Envelope", .Name = "Envelope", .Category = "Mix", .Inputs = EnvelopeInputs, .Outputs = AudioOutputs, .Properties = EnvelopeProperties},
    FGraphNodeDescriptor{.Type = "Mixer", .Name = "Mixer", .Category = "Mix", .Inputs = MixerInputs, .Outputs = AudioOutputs},
    FGraphNodeDescriptor{.Type = "Gain", .Name = "Gain", .Category = "Mix", .Inputs = GainInputs, .Outputs = AudioOutputs, .Properties = GainProperties},
    FGraphNodeDescriptor{.Type = "Pitch", .Name = "Pitch", .Category = "Mix", .Inputs = PitchInputs, .Outputs = AudioOutputs, .Properties = PitchProperties},
    FGraphNodeDescriptor{.Type = "LowPass", .Name = "Low Pass", .Category = "Filters", .Inputs = FilterInputs, .Outputs = AudioOutputs, .Properties = LowPassProperties},
    FGraphNodeDescriptor{.Type = "HighPass", .Name = "High Pass", .Category = "Filters", .Inputs = FilterInputs, .Outputs = AudioOutputs, .Properties = HighPassProperties},
    FGraphNodeDescriptor{.Type = "MapRange", .Name = "Map Range", .Category = "Controls", .Inputs = MapRangeInputs, .Outputs = MapRangeOutputs, .Properties = MapRangeProperties},
    FGraphNodeDescriptor{.Type = "FloatInput", .Name = "Float Input", .Category = "Inputs", .Outputs = FloatOutputs, .Properties = FloatProperties},
    FGraphNodeDescriptor{.Type = "BoolInput", .Name = "Bool Input", .Category = "Inputs", .Outputs = BoolOutputs, .Properties = BoolProperties},
    FGraphNodeDescriptor{.Type = "TriggerInput", .Name = "Trigger Input", .Category = "Inputs", .Outputs = TriggerOutputs, .Properties = TriggerProperties},
    FGraphNodeDescriptor{.Type = "Output", .Name = "Output", .Category = "Output", .Inputs = OutputInputs, .MaximumCount = 1},
};

bool CanConnectAudioPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To || (To == PlayPin && (From == BoolPin || From == TriggerPin));
}

// Version 2 added oscillators, envelopes, subgraphs, and delays; version 1 graphs only lack them.
std::expected<void, FGraphError> UpgradeAudioGraph(FGraph&, const std::uint32_t FromVersion)
{
	return FromVersion >= 1 ? std::expected<void, FGraphError>() : std::unexpected(FGraphError{"Audio Graphs start at version 1"});
}

const FGraphSchema AudioSchema{.Domain = AudioGraphDomain, .Version = AudioGraphVersion, .Nodes = AudioNodes, .CanConnect = CanConnectAudioPins, .Upgrade = UpgradeAudioGraph};

[[nodiscard]] const FGraphValue& Property(const FGraphNode& Node, const std::string_view Key)
{
	return std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value;
}

[[nodiscard]] float FloatProperty(const FGraphNode& Node, const std::string_view Key)
{
	return std::get<float>(Property(Node, Key));
}

[[nodiscard]] std::string_view Trim(std::string_view Text)
{
	while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t'))
	{
		Text.remove_prefix(1);
	}

	while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
	{
		Text.remove_suffix(1);
	}

	return Text;
}

[[nodiscard]] std::vector<std::string_view> SplitList(const std::string_view Text)
{
	std::vector<std::string_view> Items;
	std::size_t Start = 0;
	while (Start <= Text.size())
	{
		const std::size_t End = std::min(Text.find(',', Start), Text.size());
		if (const std::string_view Item = Trim(Text.substr(Start, End - Start)); !Item.empty())
		{
			Items.push_back(Item);
		}

		Start = End + 1;
	}

	return Items;
}

class FAudioGraphCompiler final
{
public:
	FAudioGraphCompiler(const FGraph& InGraph, const FAudioWaveResolver& InResolve, FAudioGraphCompilation& InResult)
	    : Graph(InGraph)
	    , Resolve(InResolve)
	    , Result(InResult)
	{
	}

	// Returns the errors; the compilation is complete only when there are none.
	[[nodiscard]] std::vector<FGraphDiagnostic> Compile();

private:
	void Run();

	void Fail(const FGraphNodeId Node, std::string Message, std::string Pin = {})
	{
		Errors.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Pin = std::move(Pin), .Message = std::move(Message)});
	}

	[[nodiscard]] const FGraphLink* FindSource(const FGraphNodeId Node, const std::string_view Pin) const
	{
		const auto Link = std::ranges::find_if(Graph.Links, [&](const FGraphLink& Candidate)
		{
			return Candidate.To.Node == Node && Candidate.To.Key == Pin;
		});

		return Link == Graph.Links.end() ? nullptr : &*Link;
	}

	[[nodiscard]] FSoundControl Control(const FGraphNode& Node, std::string_view Pin, float Constant);
	// The Bool or Trigger input a started node listens on, and its delay, checked.
	void Gate(const FGraphNode& Node, std::string_view Pin, FSoundNodeDesc& Desc);
	[[nodiscard]] std::uint32_t EmitMapRange(const FGraphNode& Node);
	[[nodiscard]] std::optional<std::uint32_t> EmitAudio(const FGraphNode& Node, const std::vector<FSoundControl>& Pitches);
	[[nodiscard]] std::optional<std::uint32_t> EmitInput(const FGraphNode& Node, std::string_view Pin, const std::vector<FSoundControl>& Pitches);

	const FGraph& Graph;
	const FAudioWaveResolver& Resolve;
	FAudioGraphCompilation& Result;
	std::map<FGraphNodeId, std::uint32_t> Parameters;
	std::map<FGraphNodeId, std::uint32_t> MapRangeSlots;
	std::set<FGraphNodeId> Reached;
	std::uint32_t NextSlot = 0;
	std::vector<FGraphDiagnostic> Errors;
};

std::vector<FGraphDiagnostic> FAudioGraphCompiler::Compile()
{
	Run();
	return std::move(Errors);
}

FSoundControl FAudioGraphCompiler::Control(const FGraphNode& Node, const std::string_view Pin, const float Constant)
{
	const FGraphLink* const Link = FindSource(Node.Id, Pin);
	if (!Link)
	{
		return {.Constant = Constant};
	}

	const FGraphNode& Source = *FindGraphNode(Graph, Link->From.Node);
	Reached.insert(Source.Id);
	if (Source.Type == "MapRange")
	{
		return {.Slot = EmitMapRange(Source)};
	}

	return {.Slot = Parameters.at(Source.Id)};
}

void FAudioGraphCompiler::Gate(const FGraphNode& Node, const std::string_view Pin, FSoundNodeDesc& Desc)
{
	if (const FGraphLink* const Play = FindSource(Node.Id, Pin))
	{
		Reached.insert(Play->From.Node);
		Desc.Play = Parameters.at(Play->From.Node);
	}

	Desc.Delay = FloatProperty(Node, "delay");
	if (!(Desc.Delay >= 0.f && Desc.Delay <= MaximumSoundDelaySeconds))
	{
		Fail(Node.Id, "A delay is 0 to 60 seconds");
	}
}

std::uint32_t FAudioGraphCompiler::EmitMapRange(const FGraphNode& Node)
{
	if (const auto Found = MapRangeSlots.find(Node.Id); Found != MapRangeSlots.end())
	{
		return Found->second;
	}

	if (!FindSource(Node.Id, "Value"))
	{
		Fail(Node.Id, "Link the value a Map Range remaps", "Value");
	}

	const FSoundControl Input = Control(Node, "Value", 0.f);
	const std::array Range{FloatProperty(Node, "inMin"), FloatProperty(Node, "inMax"), FloatProperty(Node, "outMin"), FloatProperty(Node, "outMax")};
	if (Range[0] == Range[1] || !std::ranges::all_of(Range, [](const float Value)
	    {
		    return std::isfinite(Value);
	    }))
	{
		Fail(Node.Id, "A Map Range needs finite values and an input range that is not empty");
	}

	const std::uint32_t Slot = NextSlot++;
	MapRangeSlots.emplace(Node.Id, Slot);
	Result.Graph.Nodes.push_back({.Type = ESoundNodeType::MapRange, .Control = Input.Slot == NoSoundControl ? FSoundControl{.Slot = 0} : Input, .Range = Range, .Slot = Slot});
	return Slot;
}

std::optional<std::uint32_t> FAudioGraphCompiler::EmitInput(const FGraphNode& Node, const std::string_view Pin, const std::vector<FSoundControl>& Pitches)
{
	const FGraphLink* const Link = FindSource(Node.Id, Pin);
	if (!Link)
	{
		Fail(Node.Id, std::format("Link the sound this {} takes", Node.Type), std::string(Pin));
		return std::nullopt;
	}

	return EmitAudio(*FindGraphNode(Graph, Link->From.Node), Pitches);
}

std::optional<std::uint32_t> FAudioGraphCompiler::EmitAudio(const FGraphNode& Node, const std::vector<FSoundControl>& Pitches)
{
	Reached.insert(Node.Id);
	const auto Push = [this](FSoundNodeDesc Desc) -> std::optional<std::uint32_t>
	{
		Result.Graph.Nodes.push_back(std::move(Desc));
		return static_cast<std::uint32_t>(Result.Graph.Nodes.size() - 1);
	};

	if (Node.Type == "WavePlayer")
	{
		FSoundNodeDesc Desc{.Type = ESoundNodeType::WavePlayer, .bLoop = std::get<bool>(Property(Node, "loop")), .PitchVariation = FloatProperty(Node, "pitchVariation"), .GainVariation = FloatProperty(Node, "gainVariation"), .Pitches = Pitches};
		for (const std::string_view Path : SplitList(std::get<std::string>(Property(Node, "waves"))))
		{
			if (const std::optional<FAssetId> Wave = Resolve ? Resolve(Path) : std::nullopt)
			{
				Desc.Waves.push_back(*Wave);
				Result.WavePaths.emplace_back(Path);
			}
			else
			{
				Fail(Node.Id, std::format("Wave '{}' is not a sound in this graph's content", Path));
			}
		}

		if (Desc.Waves.empty())
		{
			Fail(Node.Id, "A Wave Player needs at least one wave");
		}

		if (!(Desc.PitchVariation >= 0.f && Desc.PitchVariation <= 1.f && Desc.GainVariation >= 0.f && Desc.GainVariation <= 1.f))
		{
			Fail(Node.Id, "Variations are fractions from 0 to 1");
		}

		Gate(Node, "Play", Desc);
		return Push(std::move(Desc));
	}

	if (Node.Type == "Oscillator")
	{
		const std::string& Name = std::get<std::string>(Property(Node, "waveform"));
		constexpr std::array Names{std::string_view("Sine"), std::string_view("Saw"), std::string_view("Square"), std::string_view("Triangle"), std::string_view("Noise")};
		const auto Found = std::ranges::find(Names, std::string_view(Name));
		const float Frequency = FloatProperty(Node, "frequency");
		FSoundNodeDesc Desc{.Type = ESoundNodeType::Oscillator, .Control = Control(Node, "Frequency", Frequency), .Pitches = Pitches, .Waveform = static_cast<ESoundWaveform>(Found - Names.begin()), .Duration = FloatProperty(Node, "duration")};
		if (Found == Names.end())
		{
			Fail(Node.Id, std::format("Waveform '{}' must be Sine, Saw, Square, Triangle, or Noise", Name));
			Desc.Waveform = ESoundWaveform::Sine;
		}

		if (!(Frequency >= 0.f && Frequency <= 20000.f) || !(Desc.Duration >= 0.f && Desc.Duration <= MaximumSoundDelaySeconds))
		{
			Fail(Node.Id, "Frequency is 0 to 20000 Hz and duration 0 to 60 seconds");
		}

		Gate(Node, "Play", Desc);
		return Push(std::move(Desc));
	}

	if (Node.Type == "Envelope")
	{
		const std::array Shape{FloatProperty(Node, "attack"), FloatProperty(Node, "decay"), FloatProperty(Node, "sustain"), FloatProperty(Node, "release")};
		if (!(Shape[0] >= 0.f && Shape[1] >= 0.f && Shape[3] >= 0.f && Shape[2] >= 0.f && Shape[2] <= 1.f && std::max({Shape[0], Shape[1], Shape[3]}) <= MaximumSoundDelaySeconds))
		{
			Fail(Node.Id, "Attack, decay, and release are 0 to 60 seconds, and sustain is a level from 0 to 1");
		}

		FSoundNodeDesc Desc{.Type = ESoundNodeType::Envelope, .Range = Shape};
		Gate(Node, "Gate", Desc);
		const auto Input = EmitInput(Node, "In", Pitches);
		if (!Input)
		{
			return std::nullopt;
		}

		Desc.Inputs = {*Input};
		return Push(std::move(Desc));
	}

	if (Node.Type == "Pitch")
	{
		const float Pitch = FloatProperty(Node, "pitch");
		if (!(Pitch > 0.f && Pitch <= 16.f))
		{
			Fail(Node.Id, "Pitch must be above 0 and at most 16");
		}

		// Pitch is a playback rate, so it compiles into every wave player it carries instead of running as a node.
		std::vector<FSoundControl> Carried = Pitches;
		Carried.push_back(Control(Node, "Pitch", Pitch));
		return EmitInput(Node, "In", Carried);
	}

	if (Node.Type == "Mixer")
	{
		FSoundNodeDesc Desc{.Type = ESoundNodeType::Mixer};
		for (const std::string_view Pin : {"In1", "In2", "In3", "In4"})
		{
			if (const FGraphLink* const Link = FindSource(Node.Id, Pin))
			{
				if (const auto Input = EmitAudio(*FindGraphNode(Graph, Link->From.Node), Pitches))
				{
					Desc.Inputs.push_back(*Input);
				}
			}
		}

		if (Desc.Inputs.empty())
		{
			Fail(Node.Id, "A Mixer needs at least one linked input");
			return std::nullopt;
		}

		return Push(std::move(Desc));
	}

	const bool bGain = Node.Type == "Gain";
	const float Value = FloatProperty(Node, bGain ? "gain" : "cutoff");
	if (bGain ? !(Value >= 0.f && Value <= 16.f) : !(Value >= 20.f && Value <= 20000.f))
	{
		Fail(Node.Id, bGain ? "Gain must be from 0 to 16" : "Cutoff must be from 20 to 20000 Hz");
	}

	const FSoundControl Setting = Control(Node, bGain ? "Gain" : "Cutoff", Value);
	const auto Input = EmitInput(Node, "In", Pitches);
	if (!Input)
	{
		return std::nullopt;
	}

	const ESoundNodeType Type = bGain ? ESoundNodeType::Gain : Node.Type == "LowPass" ? ESoundNodeType::LowPass : ESoundNodeType::HighPass;
	return Push({.Type = Type, .Inputs = {*Input}, .Control = Setting});
}

void FAudioGraphCompiler::Run()
{
	std::vector<const FGraphNode*> Nodes;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Nodes.push_back(&Node);
	}

	std::ranges::sort(Nodes, {}, [](const FGraphNode* Node)
	{
		return Node->Id;
	});

	// Inputs become parameters in node order, which is the order gameplay sees them in.
	for (const FGraphNode* Node : Nodes)
	{
		const bool bFloat = Node->Type == "FloatInput";
		const bool bBool = Node->Type == "BoolInput";
		if (!bFloat && !bBool && Node->Type != "TriggerInput")
		{
			continue;
		}

		const std::string& Name = std::get<std::string>(Property(*Node, "name"));
		if (Name.empty() || Name.size() > 64 || std::ranges::contains(Result.Graph.Parameters, Name, &FSoundParameterDesc::Name))
		{
			Fail(Node->Id, std::format("Input names must be unique, not empty, and at most 64 characters ('{}')", Name));
		}

		const ESoundParameterType Type = bFloat ? ESoundParameterType::Float : bBool ? ESoundParameterType::Bool : ESoundParameterType::Trigger;
		const float Default = bFloat ? FloatProperty(*Node, "default") : bBool && std::get<bool>(Property(*Node, "default")) ? 1.f : 0.f;
		Parameters.emplace(Node->Id, static_cast<std::uint32_t>(Result.Graph.Parameters.size()));
		Result.Graph.Parameters.push_back({.Name = Name, .Type = Type, .Default = Default});
	}

	if (Result.Graph.Parameters.size() > MaximumSoundGraphParameters)
	{
		Fail({}, std::format("An Audio Graph has at most {} inputs", MaximumSoundGraphParameters));
	}

	NextSlot = static_cast<std::uint32_t>(Result.Graph.Parameters.size());

	// Each audio output feeds one input, so every wave player has one path to the output and one set of pitches on it.
	std::map<FGraphNodeId, std::size_t> AudioUses;
	for (const FGraphLink& Link : Graph.Links)
	{
		const FGraphNode* const From = FindGraphNode(Graph, Link.From.Node);
		if (From && Link.From.Key == "Out" && ++AudioUses[From->Id] == 2)
		{
			Fail(From->Id, "A sound output can feed only one input; use another Wave Player for a second path", "Out");
		}
	}

	const auto Output = std::ranges::find(Nodes, std::string_view("Output"), [](const FGraphNode* Node)
	{
		return std::string_view(Node->Type);
	});

	if (Output == Nodes.end())
	{
		Fail({}, "An Audio Graph needs an Output node");
		return;
	}

	Reached.insert((*Output)->Id);
	if (!Errors.empty())
	{
		return;
	}

	if (const auto Final = EmitInput(**Output, "In", {}))
	{
		Result.Graph.Nodes.push_back({.Type = ESoundNodeType::Output, .Inputs = {*Final}});
	}

	Result.Graph.ControlSlots = NextSlot;
	if (Result.Graph.Nodes.size() > MaximumSoundGraphNodes)
	{
		Fail({}, std::format("An Audio Graph compiles to at most {} nodes", MaximumSoundGraphNodes));
	}

	for (const FGraphNode* Node : Nodes)
	{
		const bool bInput = Node->Type == "FloatInput" || Node->Type == "BoolInput" || Node->Type == "TriggerInput";
		if (!bInput && !Reached.contains(Node->Id))
		{
			Result.Diagnostics.push_back({.Severity = EGraphSeverity::Warning, .Node = Node->Id, .Message = "This node does not reach the Output, so it never plays"});
		}
	}

	if (Errors.empty())
	{
		if (auto Valid = ValidateSoundGraph(Result.Graph); !Valid)
		{
			Fail({}, std::format("The compiled graph is invalid: {}", Valid.error().Message));
		}
	}
}
}

// Copies every Subgraph node's graph into this one, recursively, so the compiler sees one flat graph. Copied nodes take fresh identities, and
// Origins maps each back to the Subgraph node a problem inside it is reported on.
class FAudioSubgraphFlattener
{
public:
	FAudioSubgraphFlattener(const FAudioGraphLoader& InLoad, FAudioGraphCompilation& InResult)
	    : Load(InLoad)
	    , Result(InResult)
	{
	}

	[[nodiscard]] FGraph Flatten(FGraph Graph, std::vector<std::string>& Path);

	std::vector<FGraphDiagnostic> Errors{};
	std::map<FGraphNodeId, FGraphNodeId> Origins{};

private:
	const FAudioGraphLoader& Load;
	FAudioGraphCompilation& Result;
};

FGraph FAudioSubgraphFlattener::Flatten(FGraph Graph, std::vector<std::string>& Path)
{
	const auto IsInput = [](const FGraphNode& Node)
	{
		return Node.Type == "FloatInput" || Node.Type == "BoolInput" || Node.Type == "TriggerInput";
	};

	const auto Name = [](const FGraphNode& Node)
	{
		return std::get<std::string>(Property(Node, "name"));
	};

	for (std::size_t Index = 0; Index < Graph.Nodes.size();)
	{
		if (Graph.Nodes[Index].Type != "Subgraph")
		{
			++Index;
			continue;
		}

		const FGraphNode Holder = Graph.Nodes[Index];
		const std::string File = std::get<std::string>(Property(Holder, "graph"));
		const auto Fail = [&](std::string Message)
		{
			Errors.push_back({.Severity = EGraphSeverity::Error, .Node = Origins.contains(Holder.Id) ? Origins.at(Holder.Id) : Holder.Id, .Message = std::move(Message)});
		};

		std::optional<FGraph> Inner = Load && !File.empty() && !std::ranges::contains(Path, File) && Path.size() < 8 ? Load(File) : std::nullopt;
		Graph.Nodes.erase(Graph.Nodes.begin() + static_cast<std::ptrdiff_t>(Index));
		if (!Inner)
		{
			Fail(std::format("Subgraph '{}' cannot be loaded, contains this graph, or nests too deep", File));
			std::erase_if(Graph.Links, [&Holder](const FGraphLink& Link)
			{
				return Link.From.Node == Holder.Id || Link.To.Node == Holder.Id;
			});

			continue;
		}

		Result.GraphPaths.push_back(File);
		Path.push_back(File);
		FGraph Flat = Flatten(std::move(*Inner), Path);
		Path.pop_back();

		// Inputs of a name this graph already has become that input; the rest are new inputs of this graph.
		std::map<FGraphNodeId, FGraphNodeId> Renamed;
		FGraphNodeId Next = NextGraphNodeId(Graph);
		const FGraphNode* InnerOutput = nullptr;
		for (const FGraphNode& Node : Flat.Nodes)
		{
			if (Node.Type == "Output")
			{
				InnerOutput = &Node;
				continue;
			}

			const auto Shared = IsInput(Node) ? std::ranges::find_if(Graph.Nodes, [&](const FGraphNode& Outer)
			{
				return Outer.Type == Node.Type && IsInput(Outer) && Name(Outer) == Name(Node);
			})
			                                  : Graph.Nodes.end();
			if (Shared != Graph.Nodes.end())
			{
				Renamed[Node.Id] = Shared->Id;
				continue;
			}

			FGraphNode Copy = Node;
			Copy.Id = Next;
			Next.Value += 1;
			Renamed[Node.Id] = Copy.Id;
			Origins[Copy.Id] = Origins.contains(Holder.Id) ? Origins.at(Holder.Id) : Holder.Id;
			Graph.Nodes.push_back(std::move(Copy));
		}

		std::optional<FGraphPin> Produced;
		for (const FGraphLink& Link : Flat.Links)
		{
			if (InnerOutput && Link.To.Node == InnerOutput->Id)
			{
				Produced = FGraphPin{.Node = Renamed.at(Link.From.Node), .Key = Link.From.Key};
				continue;
			}

			Graph.Links.push_back({.From = {.Node = Renamed.at(Link.From.Node), .Key = Link.From.Key}, .To = {.Node = Renamed.at(Link.To.Node), .Key = Link.To.Key}});
		}

		if (!Produced)
		{
			Fail(std::format("Subgraph '{}' has no Output with a linked input", File));
		}

		for (auto Link = Graph.Links.begin(); Link != Graph.Links.end();)
		{
			if (Link->From.Node == Holder.Id && Produced)
			{
				Link->From = *Produced;
				++Link;
			}
			else if (Link->From.Node == Holder.Id || Link->To.Node == Holder.Id)
			{
				Link = Graph.Links.erase(Link);
			}
			else
			{
				++Link;
			}
		}
	}

	return Graph;
}

const FGraphSchema& GetAudioGraphSchema()
{
	return AudioSchema;
}

std::expected<FAudioGraphCompilation, std::vector<FGraphDiagnostic>> CompileAudioGraph(const FGraph& Source, const FAudioWaveResolver& Resolve, const FAudioGraphLoader& Load)
{
	std::vector<FGraphDiagnostic> Errors;
	for (FGraphDiagnostic& Diagnostic : ValidateGraph(Source, AudioSchema))
	{
		if (Diagnostic.Severity == EGraphSeverity::Error)
		{
			Errors.push_back(std::move(Diagnostic));
		}
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	FAudioGraphCompilation Result;
	FAudioSubgraphFlattener Flattener(Load, Result);
	std::vector<std::string> Path;
	const FGraph Graph = Flattener.Flatten(Source, Path);
	const auto Report = [&Flattener](std::vector<FGraphDiagnostic>& Diagnostics)
	{
		for (FGraphDiagnostic& Diagnostic : Diagnostics)
		{
			if (const auto Origin = Flattener.Origins.find(Diagnostic.Node); Origin != Flattener.Origins.end())
			{
				Diagnostic.Node = Origin->second;
				Diagnostic.Pin.clear();
			}
		}
	};

	if (!Flattener.Errors.empty())
	{
		return std::unexpected(std::move(Flattener.Errors));
	}

	FAudioGraphCompiler Compiler(Graph, Resolve, Result);
	if (auto Failures = Compiler.Compile(); !Failures.empty())
	{
		Report(Failures);
		return std::unexpected(std::move(Failures));
	}

	Report(Result.Diagnostics);
	return Result;
}
}
