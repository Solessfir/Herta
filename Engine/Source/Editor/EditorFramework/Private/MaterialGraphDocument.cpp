#include "GraphDocumentPanel.h"
#include "Herta/MaterialGraph/MaterialGraph.h"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace Herta
{
namespace
{
constexpr std::array<std::string_view, 6> TextureSlots{"baseColor", "metallic", "roughness", "normal", "occlusion", "emissive"};
constexpr std::array<const char*, 6> TextureSlotNames{"Base color", "Metallic", "Roughness", "Normal", "Occlusion", "Emissive"};

// Pins are colored by width, so a mismatch is visible before the compiler reports it.
FToolUIColor MaterialPinColor(const std::uint32_t Type)
{
	switch (static_cast<EMaterialPinType>(Type))
	{
		case EMaterialPinType::Float:
			return {150, 200, 120, 255};
		case EMaterialPinType::Float2:
			return {90, 190, 190, 255};
		case EMaterialPinType::Float3:
			return {230, 200, 90, 255};
		case EMaterialPinType::Float4:
			return {220, 120, 170, 255};
		default:
			return ToolUITheme::NeutralAccent;
	}
}

FToolUIColor MaterialCategoryColor(const std::string_view Category)
{
	if (Category == "Values")
	{
		return {80, 130, 220, 255};
	}

	if (Category == "Inputs")
	{
		return {80, 180, 120, 255};
	}

	if (Category == "Textures")
	{
		return {210, 110, 80, 255};
	}

	if (Category == "Channels")
	{
		return {150, 100, 210, 255};
	}

	if (Category == "Output")
	{
		return {220, 220, 225, 255};
	}

	return ToolUITheme::Surface2;
}

std::vector<FGraphDiagnostic> CompileMaterialDocument(const FGraph& Graph)
{
	const auto Compiled = CompileMaterialGraph(Graph);
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

// The texture slot is a choice from the material's six slots rather than free text.
bool DrawMaterialProperty(const std::string_view NodeType, const FGraphPropertyDescriptor& Property, FGraphValue& Value, bool& bChanged)
{
	auto* const Text = std::get_if<std::string>(&Value);
	if (!Text || NodeType != "TextureSample" || Property.Key != "slot")
	{
		return false;
	}

	const auto Found = std::ranges::find(TextureSlots, *Text);
	int Slot = Found == TextureSlots.end() ? 0 : static_cast<int>(Found - TextureSlots.begin());
	if (ImGui::Combo("##Value", &Slot, TextureSlotNames.data(), static_cast<int>(TextureSlotNames.size())))
	{
		*Text = TextureSlots[static_cast<std::size_t>(Slot)];
		bChanged = true;
	}

	return true;
}

std::string_view MaterialPropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (NodeType == "TextureSample" && Key == "slot")
	{
		return "Samples this slot's texture from the material asset";
	}

	return Key == "name" ? "Materials using this graph set the parameter by this name" : std::string_view();
}

const FGraphCanvasDomain MaterialCanvas{.Schema = &GetMaterialGraphSchema(), .PinColor = MaterialPinColor, .CategoryColor = MaterialCategoryColor};
const FGraphDocumentDomain MaterialDocument{.Canvas = &MaterialCanvas, .Title = "Material Graph", .Compile = CompileMaterialDocument, .DrawProperty = DrawMaterialProperty, .PropertyTooltip = MaterialPropertyTooltip};
}

const FGraphDocumentDomain& GetMaterialGraphDocumentDomain()
{
	return MaterialDocument;
}

FGraph MakeDefaultMaterialGraph()
{
	const FGraphSchema& Schema = GetMaterialGraphSchema();
	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
	if (auto Texture = MakeGraphNode(Schema, "TextureSample", {1}, {-260.f, -40.f}))
	{
		Graph.Nodes.push_back(std::move(*Texture));
	}

	if (auto Output = MakeGraphNode(Schema, MaterialGraphOutputType, {2}, {60.f, -80.f}))
	{
		Graph.Nodes.push_back(std::move(*Output));
	}

	Graph.Links = {
	    {.From = {.Node = {1}, .Key = "RGB"}, .To = {.Node = {2}, .Key = "BaseColor"}},
	    {.From = {.Node = {1}, .Key = "A"}, .To = {.Node = {2}, .Key = "Alpha"}},
	};
	return Graph;
}
}
