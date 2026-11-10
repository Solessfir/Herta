#include "GraphDocumentPanel.h"

#include "Herta/Platform/FileSystem.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <random>

namespace Herta
{
namespace
{
std::size_t GraphBytes(const FGraph& Graph)
{
	return sizeof(FGraph) + Graph.Nodes.size() * (sizeof(FGraphNode) + 4 * sizeof(FGraphProperty)) + Graph.Links.size() * sizeof(FGraphLink);
}
}

FGraphDocumentPanel::FGraphDocumentPanel(const FGraphDocumentDomain& InDomain)
    : Domain(InDomain)
{
}

void FGraphDocumentPanel::Open(std::string InPath, std::filesystem::path InFile, FGraph Graph, const bool bInReadOnly)
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

bool FGraphDocumentPanel::RequestClose()
{
	if (bOpen && IsDirty() && !bReadOnly)
	{
		bClosePrompt = true;
		return false;
	}

	bOpen = false;
	return true;
}

bool FGraphDocumentPanel::IsOpen() const
{
	return bOpen;
}

bool FGraphDocumentPanel::IsFocused() const
{
	return bFocused;
}

bool FGraphDocumentPanel::IsDirty() const
{
	return Draft != Saved;
}

const std::string& FGraphDocumentPanel::GetPath() const
{
	return Path;
}

const std::filesystem::path& FGraphDocumentPanel::GetFile() const
{
	return File;
}

const FGraph& FGraphDocumentPanel::GetDraft() const
{
	return Draft;
}

std::uint64_t FGraphDocumentPanel::GetDraftRevision() const
{
	return DraftRevision;
}

void FGraphDocumentPanel::Compile()
{
	++DraftRevision;
	Diagnostics = Domain.Compile(Draft);
}

void FGraphDocumentPanel::Apply(FGraph After, std::string Label)
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

std::expected<void, FGraphError> FGraphDocumentPanel::Save()
{
	auto Text = SerializeGraph(Draft, *Domain.Canvas->Schema);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	// Written beside the target and renamed over it, so watchers never read half a file.
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
			return std::unexpected(FGraphError{std::format("Cannot write the {}", Domain.Title)});
		}
	}

	if (const auto Moved = MoveAtomically(Temporary, File, EMoveMode::ReplaceExisting); !Moved)
	{
		std::error_code Ignored;
		std::filesystem::remove(Temporary, Ignored);
		return std::unexpected(FGraphError{std::format("Cannot save the {}: {}", Domain.Title, Moved.error().message())});
	}

	Saved = Draft;
	History.MarkSaved();
	return {};
}

void FGraphDocumentPanel::DrawDetails()
{
	const std::vector<FGraphNodeId>& Selection = Canvas.GetSelection();
	const FGraphNode* const Node = Selection.size() == 1 ? FindGraphNode(Draft, Selection.front()) : nullptr;
	const FGraphNodeDescriptor* const Descriptor = Node ? Domain.Canvas->Schema->FindNode(Node->Type) : nullptr;
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
			if (Domain.DrawProperty && Domain.DrawProperty(NodeType, Property, Value, bChanged))
			{
			}
			else if (float* const Scalar = std::get_if<float>(&Value))
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
			else if (auto* const Text = std::get_if<std::string>(&Value))
			{
				// Typing edits a copy and lands as one change when the field lets go, so a half-typed name never reaches history.
				std::array<char, 512> Buffer{};
				Text->copy(Buffer.data(), std::min(Text->size(), Buffer.size() - 1));
				if (ImGui::InputText("##Value", Buffer.data(), Buffer.size()) && ImGui::IsItemActive())
				{
					*Text = Buffer.data();
					bChanged = true;
				}
			}

			if (const std::string_view Tooltip = Domain.PropertyTooltip ? Domain.PropertyTooltip(NodeType, Property.Key) : std::string_view(); !Tooltip.empty())
			{
				ImGui::SetItemTooltip("%.*s", static_cast<int>(Tooltip.size()), Tooltip.data());
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

void FGraphDocumentPanel::DrawClosePrompt()
{
	if (bClosePrompt)
	{
		ImGui::OpenPopup("Unsaved graph");
		bClosePrompt = false;
	}

	if (!ImGui::BeginPopupModal("Unsaved graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

void FGraphDocumentPanel::Draw(FToolUIContext& ToolUI, const FGraphDocumentPanelContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	const std::string_view FileName = std::string_view(Path).substr(Path.find_last_of('/') + 1);
	const std::string TabLabel = std::format("{}{}###{}", FileName.substr(0, FileName.rfind('.')), IsDirty() ? "*" : "", Domain.Title);
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
		else if (!Context.Status.empty())
		{
			ImGui::TextDisabled("%.*s", static_cast<int>(Context.Status.size()), Context.Status.data());
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

		// The preview and details sit left of the canvas, as in Unreal's graph editors.
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
				if (ImGui::IsItemHovered() && Context.ZoomPreview)
				{
					// The wheel zooms the preview instead of scrolling the panel.
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
					if (const float Wheel = ImGui::GetIO().MouseWheel; Wheel != 0.f)
					{
						Context.ZoomPreview(Wheel);
					}
				}
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
			if (auto Edit = Canvas.Draw("Graph", Draft, *Domain.Canvas, Diagnostics, Context.Active); Edit && !bReadOnly)
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
