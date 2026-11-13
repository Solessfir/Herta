#include "GraphDocumentPanel.h"
#include "Herta/StateTreeCompiler/StateTreeCompiler.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace Herta
{
namespace
{
// Hierarchy, state items, transition conditions, and conditions, matching the schema's pin types.
FToolUIColor StateTreePinColor(const std::uint32_t Type)
{
	constexpr std::array<FToolUIColor, 5> Colors{{
	    {200, 200, 200, 255},
	    {120, 190, 230, 255},
	    {230, 170, 90, 255},
	    {200, 130, 200, 255},
	    {200, 130, 200, 255},
	}};

	return Colors[std::min<std::size_t>(Type, Colors.size() - 1)];
}

FToolUIColor StateTreeCategoryColor(const std::string_view Category)
{
	if (Category == "Hierarchy")
	{
		return {90, 150, 220, 255};
	}

	if (Category == "Tasks")
	{
		return {90, 160, 110, 255};
	}

	return {180, 120, 80, 255};
}

std::vector<FGraphDiagnostic> CompileStateTreeDocument(const FGraph& Graph)
{
	const auto Compiled = CompileStateTree(Graph);
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

// Text properties with a fixed set of values are a choice rather than free text.
bool DrawStateTreeProperty(const std::string_view NodeType, const FGraphPropertyDescriptor& Property, FGraphValue& Value, bool& bChanged)
{
	auto* const Text = std::get_if<std::string>(&Value);
	const std::span<const std::string_view> Choices = GetStateTreeChoices(NodeType, Property.Key);
	if (!Text || Choices.empty())
	{
		return false;
	}

	if (ImGui::BeginCombo("##Value", Text->c_str()))
	{
		for (const std::string_view Choice : Choices)
		{
			const bool bSelected = *Text == Choice;
			if (ImGui::Selectable(std::string(Choice).c_str(), bSelected) && !bSelected)
			{
				*Text = std::string(Choice);
				bChanged = true;
			}
		}

		ImGui::EndCombo();
	}

	return true;
}

std::string_view StateTreePropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (Key == "order")
	{
		return "Siblings and attached items run from the lowest order up; equal orders keep the order the nodes were made in";
	}

	if (Key == "selection")
	{
		return "first enters the first child whose conditions hold; utility enters the eligible child with the highest score";
	}

	if (NodeType == "State" && Key == "score")
	{
		return "A utility parent's base score for this state, which its considerations add to";
	}

	if (Key == "target" && NodeType == "Transition")
	{
		return "The name of the state to go to, anywhere in the tree";
	}

	if (Key == "target")
	{
		return "player, where the player was last seen, where a noise was last heard, or where the agent started";
	}

	if (NodeType == "Transition")
	{
		if (Key == "trigger")
		{
			return "completed, succeeded, or failed when the state's tasks finish, tick every time the agent thinks, or event when the named event arrives";
		}

		if (Key == "event")
		{
			return "Spotted and Lost when the player comes into or leaves view, Noise when the agent hears one, and Hit when something strikes it";
		}

		if (Key == "kind")
		{
			return "goto enters the target state; succeed and fail finish the parent state as if its tasks had";
		}
	}

	if (NodeType == "Condition")
	{
		if (Key == "type")
		{
			return "sees or remembers the player, heard a noise, distance to the player, health from 0 to 1, a random chance, seconds in the state, or a flag";
		}

		if (Key == "value")
		{
			return "Compared with the distance in meters, health, chance, or seconds";
		}
	}

	if (NodeType == "MoveTo" && Key == "acceptance")
	{
		return "Meters from the target that count as arrived";
	}

	if (NodeType == "Attack" && Key == "reach")
	{
		return "The swing fails when the target is farther than this";
	}

	if (NodeType == "Animate" && Key == "trigger")
	{
		return "A trigger of the character's state machine, such as Hit or Attack";
	}

	return {};
}

const FGraphCanvasDomain StateTreeCanvas{.Schema = &GetStateTreeSchema(), .PinColor = StateTreePinColor, .CategoryColor = StateTreeCategoryColor};
const FGraphDocumentDomain StateTreeDocument{.Canvas = &StateTreeCanvas, .Title = "StateTree", .Compile = CompileStateTreeDocument, .DrawProperty = DrawStateTreeProperty, .PropertyTooltip = StateTreePropertyTooltip};
}

const FGraphDocumentDomain& GetStateTreeDocumentDomain()
{
	return StateTreeDocument;
}
}
