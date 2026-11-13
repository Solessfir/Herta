#include "GraphDocumentPanel.h"
#include "Herta/AudioGraph/AudioGraph.h"

namespace Herta
{
namespace
{
// Sound flows in teal, controls in green, and the Bool and Trigger inputs that start players in amber and white.
FToolUIColor AudioPinColor(const std::uint32_t Type)
{
	switch (Type)
	{
		case 1:
			return {80, 200, 190, 255};
		case 2:
			return {140, 210, 110, 255};
		case 3:
			return {220, 120, 100, 255};
		default:
			return {230, 230, 230, 255};
	}
}

FToolUIColor AudioCategoryColor(const std::string_view Category)
{
	if (Category == "Sources")
	{
		return {60, 150, 150, 255};
	}

	if (Category == "Inputs")
	{
		return {200, 150, 80, 255};
	}

	if (Category == "Controls")
	{
		return {110, 160, 90, 255};
	}

	if (Category == "Output")
	{
		return {150, 110, 190, 255};
	}

	return ToolUITheme::Surface2;
}

FAudioWaveResolver& GetResolver()
{
	static FAudioWaveResolver Resolver;
	return Resolver;
}

FAudioGraphLoader& GetLoader()
{
	static FAudioGraphLoader Loader;
	return Loader;
}

std::vector<FGraphDiagnostic> CompileAudioDocument(const FGraph& Graph)
{
	const auto Compiled = CompileAudioGraph(Graph, GetResolver(), GetLoader());
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

std::string_view AudioPropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (NodeType == "WavePlayer")
	{
		if (Key == "waves")
		{
			return "Sounds in this graph's content, as paths such as Audio/Impact_1.wav, separated by commas; each start picks one at random";
		}

		if (Key == "pitchVariation" || Key == "gainVariation")
		{
			return "Random variation each start, as a fraction: 0.1 plays from 0.9 to 1.1 times as high or loud";
		}

		if (Key == "loop")
		{
			return "Plays the wave again from its start when it ends, until the player stops";
		}
	}

	if ((NodeType == "LowPass" || NodeType == "HighPass") && Key == "cutoff")
	{
		return "The cutoff in hertz, from 20 to 20000, used when the Cutoff pin is not linked";
	}

	if (NodeType == "Pitch" && Key == "pitch")
	{
		return "A playback rate for every wave player feeding it: 2 is an octave up; used when the Pitch pin is not linked";
	}

	if (NodeType == "MapRange")
	{
		return "Clamps the value to the input range and maps it onto the output range, which may run backwards";
	}

	if ((NodeType == "FloatInput" || NodeType == "BoolInput" || NodeType == "TriggerInput") && Key == "name")
	{
		return "The name gameplay sets this input by; the Sandbox sets Speed on footsteps, Impulse on impacts, and Finished on the run's chimes";
	}

	return {};
}

const FGraphCanvasDomain AudioCanvas{.Schema = &GetAudioGraphSchema(), .PinColor = AudioPinColor, .CategoryColor = AudioCategoryColor};
const FGraphDocumentDomain AudioDocument{.Canvas = &AudioCanvas, .Title = "Audio Graph", .Compile = CompileAudioDocument, .PropertyTooltip = AudioPropertyTooltip};
}

const FGraphDocumentDomain& GetAudioGraphDocumentDomain()
{
	return AudioDocument;
}

void SetAudioGraphDocumentResolver(FAudioWaveResolver Resolver, FAudioGraphLoader Loader)
{
	GetResolver() = std::move(Resolver);
	GetLoader() = std::move(Loader);
}

FGraph MakeDefaultAudioGraph()
{
	FGraph Graph{.Domain = std::string(AudioGraphDomain), .DomainVersion = AudioGraphVersion};
	auto Player = MakeGraphNode(GetAudioGraphSchema(), "WavePlayer", {1}, {-300.f, 0.f});
	auto Output = MakeGraphNode(GetAudioGraphSchema(), "Output", {2}, {0.f, 0.f});
	if (Player && Output)
	{
		Graph.Nodes = {std::move(*Player), std::move(*Output)};
		Graph.Links = {{.From = {.Node = {1}, .Key = "Out"}, .To = {.Node = {2}, .Key = "In"}}};
	}

	return Graph;
}
}
