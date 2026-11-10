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
};
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

const FGraphSchema AudioSchema{.Domain = AudioGraphDomain, .Version = AudioGraphVersion, .Nodes = AudioNodes, .CanConnect = CanConnectAudioPins};

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

		if (const FGraphLink* const Play = FindSource(Node.Id, "Play"))
		{
			Reached.insert(Play->From.Node);
			Desc.Play = Parameters.at(Play->From.Node);
		}

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

const FGraphSchema& GetAudioGraphSchema()
{
	return AudioSchema;
}

std::expected<FAudioGraphCompilation, std::vector<FGraphDiagnostic>> CompileAudioGraph(const FGraph& Graph, const FAudioWaveResolver& Resolve)
{
	std::vector<FGraphDiagnostic> Errors;
	for (FGraphDiagnostic& Diagnostic : ValidateGraph(Graph, AudioSchema))
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
	FAudioGraphCompiler Compiler(Graph, Resolve, Result);
	if (auto Failures = Compiler.Compile(); !Failures.empty())
	{
		return std::unexpected(std::move(Failures));
	}

	return Result;
}
}
