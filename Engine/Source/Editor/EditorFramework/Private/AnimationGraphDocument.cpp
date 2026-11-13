#include "GraphDocumentPanel.h"
#include "Herta/AnimationGraph/AnimationGraph.h"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace Herta
{
namespace
{
FToolUIColor AnimationPinColor(std::uint32_t)
{
	return {120, 190, 230, 255};
}

FToolUIColor AnimationCategoryColor(const std::string_view Category)
{
	if (Category == "States")
	{
		return {90, 150, 220, 255};
	}

	if (Category == "Parameters")
	{
		return {200, 150, 80, 255};
	}

	return ToolUITheme::Surface2;
}

std::vector<FGraphDiagnostic> CompileAnimationDocument(const FGraph& Graph)
{
	const auto Compiled = CompileAnimationGraph(Graph);
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

// A parameter's type is one of three, so it is a choice rather than free text.
bool DrawAnimationProperty(const std::string_view NodeType, const FGraphPropertyDescriptor& Property, FGraphValue& Value, bool& bChanged)
{
	auto* const Text = std::get_if<std::string>(&Value);
	if (!Text || NodeType != "Parameter" || Property.Key != "type")
	{
		return false;
	}

	constexpr std::array<const char*, 3> Types{"Float", "Bool", "Trigger"};
	const auto Found = std::ranges::find(Types, *Text);
	int Type = Found == Types.end() ? 0 : static_cast<int>(Found - Types.begin());
	if (ImGui::Combo("##Value", &Type, Types.data(), static_cast<int>(Types.size())))
	{
		*Text = Types[static_cast<std::size_t>(Type)];
		bChanged = true;
	}

	return true;
}

std::string_view AnimationPropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (NodeType == "State")
	{
		if (Key == "clips")
		{
			return "A clip name, or several as Clip@Position, Clip@Position to blend along the blend parameter";
		}

		if (Key == "blend")
		{
			return "The float parameter a state of several clips blends along, such as Speed";
		}

		if (Key == "events")
		{
			return "Events the state fires as Name@Phase, from 0 at its start to 1 at its end, such as Footstep@0, Footstep@0.5";
		}

		if (Key == "speed")
		{
			return "How fast the clips play; 1 is as authored";
		}
	}

	if (NodeType == "Transition")
	{
		if (Key == "conditions")
		{
			return "All must hold: Parameter (a bool is true or a trigger fired), !Parameter (a bool is false), Parameter > Value, or Parameter < Value, separated by commas";
		}

		if (Key == "priority")
		{
			return "Lower numbers are tested first, and the first transition that can fire wins";
		}

		if (Key == "atEnd")
		{
			return "Also waits until the state it leaves has played to its end";
		}

		if (Key == "duration")
		{
			return "Seconds the outgoing state fades out over";
		}
	}

	if (NodeType == "Parameter" && Key == "name")
	{
		return "The game drives parameters by name: Speed, Grounded, Crouched, Sliding, WallRunning, Jump, WallJump, and Mantle";
	}

	return {};
}

const FGraphCanvasDomain AnimationCanvas{.Schema = &GetAnimationGraphSchema(), .PinColor = AnimationPinColor, .CategoryColor = AnimationCategoryColor};
const FGraphDocumentDomain AnimationDocument{.Canvas = &AnimationCanvas, .Title = "Animation Graph", .Compile = CompileAnimationDocument, .DrawProperty = DrawAnimationProperty, .PropertyTooltip = AnimationPropertyTooltip};
}

const FGraphDocumentDomain& GetAnimationGraphDocumentDomain()
{
	return AnimationDocument;
}
}
