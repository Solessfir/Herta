#include "GraphDocumentPanel.h"
#include "Herta/GameplayGraph/GameplayGraph.h"

namespace Herta
{
namespace
{
// Execution wires are white, as in Blueprints; floats, bools, and vectors each get their own color.
FToolUIColor GameplayPinColor(const std::uint32_t Type)
{
	constexpr std::array<FToolUIColor, 4> Colors{{
	    {235, 235, 235, 255},
	    {120, 200, 120, 255},
	    {210, 90, 90, 255},
	    {230, 200, 90, 255},
	}};

	return Colors[std::min<std::size_t>(Type, Colors.size() - 1)];
}

FToolUIColor GameplayCategoryColor(const std::string_view Category)
{
	if (Category == "Events")
	{
		return {180, 60, 60, 255};
	}

	if (Category == "Actions")
	{
		return {60, 110, 170, 255};
	}

	if (Category == "Flow")
	{
		return {110, 110, 120, 255};
	}

	return {70, 140, 110, 255};
}

std::vector<FGraphDiagnostic> CompileGameplayDocument(const FGraph& Graph)
{
	const auto Compiled = CompileGameplayGraph(Graph);
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

std::string_view GameplayPropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (Key == "cue")
	{
		return NodeType == "PlaySound" ? "A level sound, such as objective, impact, or swing, as set under Viewport settings > Sounds" : "A level effect, such as sparks, break, or impact, as set under Viewport settings > Effects";
	}

	if (Key == "variable")
	{
		return "A float the entity keeps for the whole session; Get and Set nodes with the same name share it";
	}

	if (Key == "a" || Key == "b" || Key == "value" || Key == "scale")
	{
		return "Used when nothing is linked to the matching input";
	}

	if (Key == "message")
	{
		return "Written to the event log with the value, during Play";
	}

	return {};
}

const FGraphCanvasDomain GameplayCanvas{.Schema = &GetGameplayGraphSchema(), .PinColor = GameplayPinColor, .CategoryColor = GameplayCategoryColor};
const FGraphDocumentDomain GameplayDocument{.Canvas = &GameplayCanvas, .Title = "Gameplay Graph", .Compile = CompileGameplayDocument, .PropertyTooltip = GameplayPropertyTooltip};
}

const FGraphDocumentDomain& GetGameplayGraphDocumentDomain()
{
	return GameplayDocument;
}
}
