#include "MaterialGraphPanel.h"

#include "Herta/MaterialGraph/MaterialGraph.h"
#include "Herta/Platform/FileSystem.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <random>

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

const FGraphCanvasDomain MaterialDomain{.Schema = &GetMaterialGraphSchema(), .PinColor = MaterialPinColor, .CategoryColor = MaterialCategoryColor};

std::size_t GraphBytes(const FGraph& Graph)
{
	return sizeof(FGraph) + Graph.Nodes.size() * (sizeof(FGraphNode) + 4 * sizeof(FGraphProperty)) + Graph.Links.size() * sizeof(FGraphLink);
}
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

void FMaterialGraphPanel::Open(std::string InPath, std::filesystem::path InFile, FGraph Graph, const bool bInReadOnly)
{
	Path = std::move(InPath);
	File = std::move(InFile);
	Draft = std::move(Graph);
	Saved = Draft;
	bReadOnly = bInReadOnly;
	bOpen = true;
	bClosePrompt = false;
	GestureBefore.reset();
	Error.clear();
	History.Clear();
	Canvas.SetSelection({});
	Canvas.RequestFrame();
	Compile();
}

bool FMaterialGraphPanel::RequestClose()
{
	if (bOpen && IsDirty() && !bReadOnly)
	{
		bClosePrompt = true;
		return false;
	}

	bOpen = false;
	return true;
}

bool FMaterialGraphPanel::IsOpen() const
{
	return bOpen;
}

bool FMaterialGraphPanel::IsFocused() const
{
	return bFocused;
}

bool FMaterialGraphPanel::IsDirty() const
{
	return Draft != Saved;
}

const std::string& FMaterialGraphPanel::GetPath() const
{
	return Path;
}

void FMaterialGraphPanel::Compile()
{
	const auto Compiled = CompileMaterialGraph(Draft);
	Diagnostics = Compiled ? Compiled->Diagnostics : Compiled.error();
}

void FMaterialGraphPanel::Apply(FGraph After, std::string Label)
{
	if (bReadOnly || After == Draft)
	{
		return;
	}

	const std::size_t Cost = GraphBytes(Draft) + GraphBytes(After);
	FGraph Before = Draft;
	const auto Applied = History.Execute({.Label = std::move(Label), .Apply = [this, Before = std::move(Before), After = std::move(After)](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		Draft = bUndo ? Before : After;
		Compile();
		return {};
	},
	                                      .MemoryCost = Cost});
	if (!Applied)
	{
		Error = Applied.error().Message;
	}
}

std::expected<void, FGraphError> FMaterialGraphPanel::Save()
{
	auto Text = SerializeGraph(Draft, GetMaterialGraphSchema());
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	// Written beside the target and renamed over it, so the shader watcher never reads half a file.
	std::random_device Device;
	std::filesystem::path Temporary = File;
	Temporary += std::format(".{:08x}.tmp", Device());
	{
		std::ofstream Stream(Temporary, std::ios::binary | std::ios::trunc);
		Stream.write(Text->data(), static_cast<std::streamsize>(Text->size()));
		Stream.close();
		if (!Stream)
		{
			std::error_code Ignored;
			std::filesystem::remove(Temporary, Ignored);
			return std::unexpected(FGraphError{"Cannot write the Material Graph"});
		}
	}

	if (const auto Moved = MoveAtomically(Temporary, File, EMoveMode::ReplaceExisting); !Moved)
	{
		std::error_code Ignored;
		std::filesystem::remove(Temporary, Ignored);
		return std::unexpected(FGraphError{"Cannot save the Material Graph: " + Moved.error().message()});
	}

	Saved = Draft;
	History.MarkSaved();
	return {};
}

void FMaterialGraphPanel::DrawDetails()
{
	const std::vector<FGraphNodeId>& Selection = Canvas.GetSelection();
	const FGraphNode* const Node = Selection.size() == 1 ? FindGraphNode(Draft, Selection.front()) : nullptr;
	const FGraphNodeDescriptor* const Descriptor = Node ? GetMaterialGraphSchema().FindNode(Node->Type) : nullptr;
	if (!Descriptor)
	{
		ImGui::TextDisabled(Selection.empty() ? "Select a node to edit it." : "Select one node to edit it.");
	}
	else
	{
		ImGui::TextUnformatted(std::string(Descriptor->Name).c_str());
		ImGui::TextDisabled("%s", std::string(Descriptor->Category).c_str());
		if (Descriptor->Properties.empty())
		{
			ImGui::TextDisabled("No properties");
		}

		// Edits replace the draft, so the loop works from the node's index and type rather than a pointer into it.
		const auto NodeIndex = static_cast<std::size_t>(Node - Draft.Nodes.data());
		const std::string NodeType = Node->Type;
		ImGui::BeginDisabled(bReadOnly);
		for (std::size_t Index = 0; Index < Descriptor->Properties.size(); ++Index)
		{
			const FGraphPropertyDescriptor& Property = Descriptor->Properties[Index];
			ImGui::PushID(static_cast<int>(Index));
			ImGui::TextUnformatted(std::string(Property.Name).c_str());
			ImGui::SetNextItemWidth(-FLT_MIN);
			FGraphValue Value = Draft.Nodes[NodeIndex].Properties[Index].Value;
			bool bChanged = false;
			if (float* const Scalar = std::get_if<float>(&Value))
			{
				bChanged = ImGui::DragFloat("##Value", Scalar, 0.01f, 0.f, 0.f, "%.4g");
			}
			else if (auto* const Vector = std::get_if<std::array<float, 4>>(&Value))
			{
				bChanged = ImGui::ColorEdit4("##Value", Vector->data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
			}
			else if (bool* const Flag = std::get_if<bool>(&Value))
			{
				bChanged = ImGui::Checkbox("##Value", Flag);
			}
			else if (std::int32_t* const Integer = std::get_if<std::int32_t>(&Value))
			{
				bChanged = ImGui::DragInt("##Value", Integer);
			}
			else if (auto* const Text = std::get_if<std::string>(&Value); Text && NodeType == "TextureSample" && Property.Key == "slot")
			{
				const auto Found = std::ranges::find(TextureSlots, *Text);
				int Slot = Found == TextureSlots.end() ? 0 : static_cast<int>(Found - TextureSlots.begin());
				if (ImGui::Combo("##Value", &Slot, TextureSlotNames.data(), static_cast<int>(TextureSlotNames.size())))
				{
					*Text = TextureSlots[static_cast<std::size_t>(Slot)];
					bChanged = true;
				}

				ImGui::SetItemTooltip("Samples this slot's texture from the material asset");
			}

			// Drags and typing preview live and enter history once, when the widget lets go; one-shot widgets such as combos record at once.
			const std::string Label = std::format("Edit {}", Property.Name);
			if (ImGui::IsItemActivated())
			{
				GestureBefore = Draft;
			}

			if (bChanged && GestureBefore)
			{
				Draft.Nodes[NodeIndex].Properties[Index].Value = std::move(Value);
				Compile();
			}
			else if (bChanged)
			{
				FGraph After = Draft;
				After.Nodes[NodeIndex].Properties[Index].Value = std::move(Value);
				Apply(std::move(After), Label);
			}

			if (ImGui::IsItemDeactivated() && GestureBefore)
			{
				FGraph After = std::move(Draft);
				Draft = std::move(*GestureBefore);
				GestureBefore.reset();
				Apply(std::move(After), Label);
			}

			ImGui::PopID();
		}

		ImGui::EndDisabled();
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Diagnostics");
	if (Diagnostics.empty())
	{
		ImGui::TextDisabled("The graph compiles.");
	}

	for (std::size_t Index = 0; Index < Diagnostics.size(); ++Index)
	{
		const FGraphDiagnostic& Diagnostic = Diagnostics[Index];
		const FToolUIColor Color = Diagnostic.Severity == EGraphSeverity::Error ? ToolUITheme::Error : ToolUITheme::Warning;
		ImGui::PushID(static_cast<int>(Index));
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha));
		ImGui::PushTextWrapPos();
		if (ImGui::Selectable(Diagnostic.Message.c_str(), false) && Diagnostic.Node.Value != 0)
		{
			Canvas.SetSelection({Diagnostic.Node});
		}

		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
}

void FMaterialGraphPanel::DrawClosePrompt()
{
	if (bClosePrompt)
	{
		ImGui::OpenPopup("Unsaved material graph");
		bClosePrompt = false;
	}

	if (!ImGui::BeginPopupModal("Unsaved material graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}

	ImGui::Text("Save changes to %s?", Path.c_str());
	ImGui::TextDisabled("Unsaved graph edits will otherwise be discarded.");
	ImGui::Spacing();
	const float Width = ImGui::GetFontSize() * 6.f;
	if (ImGui::Button("Save", {Width, 0.f}))
	{
		if (const auto Result = Save(); Result)
		{
			bOpen = false;
			ImGui::CloseCurrentPopup();
		}
		else
		{
			Error = Result.error().Message;
		}
	}

	ImGui::SameLine();
	if (ImGui::Button("Discard", {Width, 0.f}))
	{
		Draft = Saved;
		bOpen = false;
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();
	if (ImGui::Button("Cancel", {Width, 0.f}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		bOpen = true;
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void FMaterialGraphPanel::Draw(FToolUIContext& ToolUI, const FMaterialGraphPanelContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	const std::string_view FileName = std::string_view(Path).substr(Path.find_last_of('/') + 1);
	const std::string TabLabel = std::format("{}{}###Material Graph", FileName.substr(0, FileName.rfind('.')), IsDirty() ? "*" : "");
	bool bKeepOpen = true;
	const bool bVisible = ToolUI.BeginPanel(TabLabel, &bKeepOpen, EToolUIPanelKind::Document);
	bFocused = bVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (bVisible && Context.RequestPreview)
	{
		Context.RequestPreview();
	}

	if (bVisible)
	{
		ImGui::TextUnformatted((Path + (IsDirty() ? " *" : "")).c_str());
		ImGui::SameLine();
		const std::size_t Errors = static_cast<std::size_t>(std::ranges::count(Diagnostics, EGraphSeverity::Error, &FGraphDiagnostic::Severity));
		if (Errors != 0)
		{
			ImGui::TextColored(ImVec4(1.f, 0.49f, 0.49f, 1.f), "%zu error%s", Errors, Errors == 1 ? "" : "s");
		}
		else if (!Context.ShaderStatus.empty())
		{
			ImGui::TextDisabled("%.*s", static_cast<int>(Context.ShaderStatus.size()), Context.ShaderStatus.data());
		}

		if (bReadOnly)
		{
			ImGui::TextDisabled("Engine content is read-only.");
		}

		ImGui::BeginDisabled(bReadOnly || !IsDirty());
		if (ToolUIButton("Save", EToolUIMenuIcon::Save) || (!bReadOnly && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused)))
		{
			if (const auto Result = Save(); !Result)
			{
				Error = Result.error().Message;
			}
		}

		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!History.CanUndo() || bReadOnly);
		if (ToolUIButton("Undo", EToolUIMenuIcon::Undo) || (!bReadOnly && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)))
		{
			if (const auto Result = History.Undo(); !Result)
			{
				Error = Result.error().Message;
			}
		}

		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!History.CanRedo() || bReadOnly);
		if (ToolUIButton("Redo", EToolUIMenuIcon::Redo) || (!bReadOnly && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteFocused)))
		{
			if (const auto Result = History.Redo(); !Result)
			{
				Error = Result.error().Message;
			}
		}

		ImGui::EndDisabled();
		if (!Error.empty())
		{
			ImGui::TextColored(ImVec4(1.f, 0.49f, 0.49f, 1.f), "%s", Error.c_str());
		}

		// The preview and details sit left of the canvas, as in Unreal's material editor.
		const float Sidebar = 280.f * Scale;
		const float Height = ImGui::GetContentRegionAvail().y;
		if (ImGui::BeginChild("Details", {Sidebar, Height}, ImGuiChildFlags_None))
		{
			if (Context.PreviewTexture != 0)
			{
				const float Width = ImGui::GetContentRegionAvail().x;
				const ImVec2 Position = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(Context.PreviewTexture)), Position, {Position.x + Width, Position.y + Width}, {0, 0}, {1, 1}, IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
				ImGui::Dummy({Width, Width});
			}

			if (!Context.PreviewStatus.empty())
			{
				ImGui::PushTextWrapPos();
				ImGui::TextDisabled("%.*s", static_cast<int>(Context.PreviewStatus.size()), Context.PreviewStatus.data());
				ImGui::PopTextWrapPos();
			}

			if (Context.PreviewTexture != 0 || !Context.PreviewStatus.empty())
			{
				ImGui::Spacing();
			}

			DrawDetails();
		}

		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("Canvas", {std::max(1.f, ImGui::GetContentRegionAvail().x), Height}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (auto Edit = Canvas.Draw("MaterialGraph", Draft, MaterialDomain, Diagnostics); Edit && !bReadOnly)
			{
				Apply(std::move(Edit->Graph), std::move(Edit->Label));
			}
		}

		ImGui::EndChild();
	}

	ToolUI.EndPanel();
	if (!bKeepOpen)
	{
		if (IsDirty() && !bReadOnly)
		{
			bClosePrompt = true;
		}
		else
		{
			bOpen = false;
		}
	}

	DrawClosePrompt();
}
}
