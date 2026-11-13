#include "EditorFrameworkPrivate.h"
#include "Herta/Assets/AssetSearch.h"
#include "Herta/ToolUI/EditorText.h"

namespace Herta
{
namespace
{
enum class EPaletteKind : std::uint8_t
{
	Entity,
	Asset,
	Command,
};

struct FPaletteRow
{
	EPaletteKind Kind = EPaletteKind::Entity;
	// Index into the kind's own list: preview objects, asset options, or commands.
	std::size_t Index = 0;
	std::string_view Label{};
	std::string_view Detail{};
};

namespace Editor = Text::Editor;

constexpr std::array<FTextId, 3> PaletteHeadings{Editor::Palette_Entities, Editor::Palette_Assets, Editor::Palette_Commands};
}

void FEditorFramework::FImplementation::DrawCommandPalette()
{
	const ImGuiIO& IO = ImGui::GetIO();
	if (!IO.AppFocusLost && IO.KeyMods == ImGuiMod_Ctrl && ImGui::IsKeyPressed(ImGuiKey_K, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		bCommandPaletteRequested = true;
	}

	// The title bar box follows the editor's language; the generation moves when the culture or a catalog changes.
	if (const std::uint64_t Generation = Localization.GetGeneration(); TitleSearchGeneration != Generation)
	{
		TitleSearchGeneration = Generation;
		ToolUI->SetTitleSearch(Localization.Format(Editor::Palette_TitleHint), "Ctrl K");
	}

	bCommandPaletteRequested |= ToolUI->ConsumeTitleSearchClick();
	if (std::exchange(bCommandPaletteRequested, false))
	{
		CommandPaletteQuery.fill('\0');
		CommandPaletteSelected = 0;
		// Commands change only when a plugin or module registers one, so one listing serves the popup's lifetime.
		CommandPaletteCommands = EditorCommands ? EditorCommands->List() : std::vector<FEditorCommandInfo>{};
		ImGui::OpenPopup("##CommandPalette");
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const ImGuiViewport* const Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos({Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + 84.f * Scale}, ImGuiCond_Always, {0.5f, 0.f});
	ImGui::SetNextWindowSize({std::min(560.f * Scale, Viewport->Size.x - 32.f * Scale), 0.f});
	ImGui::SetNextWindowBgAlpha(0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f * Scale, 8.f * Scale});
	const bool bOpen = ImGui::BeginPopup("##CommandPalette", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	if (!bOpen)
	{
		return;
	}

	const ImVec2 Position = ImGui::GetWindowPos();
	const ImVec2 Size = ImGui::GetWindowSize();
	ToolUI->DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI->GetMetrics().PopupRounding * Scale);
	if (ImGui::IsWindowAppearing())
	{
		ImGui::SetKeyboardFocusHere();
	}

	ImGui::SetNextItemWidth(-1.f);
	if (ToolUI->DrawSearchField("##PaletteQuery", Localization.Format(Editor::Palette_Hint).c_str(), CommandPaletteQuery.data(), CommandPaletteQuery.size()))
	{
		CommandPaletteSelected = 0;
	}

	// Each kind ranks on its own and shows its best few, so one long list never pushes the others out.
	const std::string_view Query(CommandPaletteQuery.data());
	const std::span<const FPreviewAssetOption> Options = Assets ? Assets->GetOptions() : std::span<const FPreviewAssetOption>{};
	std::vector<FPaletteRow> Rows;
	std::vector<std::string_view> Candidates;
	const auto AddMatches = [&](const EPaletteKind Kind, const std::size_t Limit, const auto& GetDetail)
	{
		const auto Matches = SearchAssets(Candidates, Query);
		for (std::size_t Match = 0; Matches && Match < std::min(Limit, Matches->size()); ++Match)
		{
			const std::size_t Index = (*Matches)[Match].Index;
			Rows.push_back({.Kind = Kind, .Index = Index, .Label = Candidates[Index], .Detail = GetDetail(Index)});
		}

		Candidates.clear();
	};

	for (const FPreviewObject& Object : PreviewObjects)
	{
		Candidates.emplace_back(Object.Label);
	}

	AddMatches(EPaletteKind::Entity, 6, [this](const std::size_t Index)
	{
		return GetPreviewObjectTypeName(PreviewObjects[Index]);
	});

	for (const FPreviewAssetOption& Option : Options)
	{
		Candidates.emplace_back(Option.Label);
	}

	AddMatches(EPaletteKind::Asset, 6, [Options](const std::size_t Index)
	{
		return std::string_view(Options[Index].Importer);
	});

	for (const FEditorCommandInfo& Command : CommandPaletteCommands)
	{
		Candidates.emplace_back(Command.Name);
	}

	AddMatches(EPaletteKind::Command, 5, [this](const std::size_t Index)
	{
		return std::string_view(CommandPaletteCommands[Index].Description);
	});

	const int Count = static_cast<int>(Rows.size());
	if (Count > 0)
	{
		CommandPaletteSelected = (CommandPaletteSelected + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0) + Count) % Count;
	}

	int Chosen = Count > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) ? CommandPaletteSelected : -1;
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ImGui::GetStyle().FrameRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 1.f * Scale});
	ImGui::Dummy({0.f, 4.f * Scale});
	// Up and Down move the palette's own selection; rows that ImGui could navigate to would take focus from the query and stop typing.
	ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
	for (int Row = 0; Row < Count; ++Row)
	{
		const FPaletteRow& Entry = Rows[static_cast<std::size_t>(Row)];
		if (Row == 0 || Rows[static_cast<std::size_t>(Row - 1)].Kind != Entry.Kind)
		{
			ImGui::Dummy({0.f, 3.f * Scale});
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.f * Scale);
			ImGui::TextDisabled("%s", Localization.Format(PaletteHeadings[static_cast<std::size_t>(Entry.Kind)]).c_str());
		}

		ImGui::PushID(Row);
		const ImVec2 Start = ImGui::GetCursorScreenPos();
		const float Height = 30.f * Scale;
		if (ImGui::Selectable("##Row", Row == CommandPaletteSelected, 0, {0.f, Height}))
		{
			Chosen = Row;
		}

		const float Right = ImGui::GetItemRectMax().x - 10.f * Scale;
		const float CenterY = Start.y + Height * 0.5f;
		const EToolUIMenuIcon Icon = Entry.Kind == EPaletteKind::Entity ? EToolUIMenuIcon::Entity : Entry.Kind == EPaletteKind::Asset ? EToolUIMenuIcon::ContentBrowser
		                                                                                                                              : EToolUIMenuIcon::LogPrompt;
		ToolUIIcon(Icon, Start.x + 16.f * Scale, CenterY, 0.85f * Scale, ImGui::GetColorU32(ImGuiCol_TextDisabled));
		const float TextY = CenterY - ImGui::GetFontSize() * 0.5f;
		// The detail keeps to the right third; the label gets the rest and ends in an ellipsis.
		const float DetailWidth = std::min(ImGui::CalcTextSize(Entry.Detail.data(), Entry.Detail.data() + Entry.Detail.size()).x, (Right - Start.x) * 0.45f);
		const float LabelLimit = Right - DetailWidth - 12.f * Scale;
		ImDrawList* const Draw = ImGui::GetWindowDrawList();
		ImGui::RenderTextEllipsis(Draw, {Start.x + 32.f * Scale, TextY}, {LabelLimit, TextY + ImGui::GetFontSize()}, LabelLimit, Entry.Label.data(), Entry.Label.data() + Entry.Label.size(), nullptr);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::RenderTextEllipsis(Draw, {Right - DetailWidth, TextY}, {Right, TextY + ImGui::GetFontSize()}, Right, Entry.Detail.data(), Entry.Detail.data() + Entry.Detail.size(), nullptr);
		ImGui::PopStyleColor();
		ImGui::PopID();
	}

	if (Count == 0)
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.f * Scale);
		ImGui::TextDisabled("%s", Localization.Format(Editor::Palette_Empty).c_str());
	}

	ImGui::PopItemFlag();
	ImGui::PopStyleVar(2);
	ImGui::Dummy({0.f, 4.f * Scale});
	ImGui::Separator();
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.f * Scale);
	ImGui::TextDisabled("%s", Localization.Format(Editor::Palette_Help).c_str());

	if (Chosen >= 0)
	{
		const FPaletteRow& Entry = Rows[static_cast<std::size_t>(Chosen)];
		if (Entry.Kind == EPaletteKind::Entity)
		{
			PreviewSelection.Select(static_cast<int>(Entry.Index));
			OutlinerPanelState.SelectedFolder = {};
			OutlinerPanelState.bRevealSelection = true;
			FocusPreview();
		}
		else if (Entry.Kind == EPaletteKind::Asset)
		{
			// The browser opens on the asset's folder with the asset selected.
			const FPreviewAssetOption& Option = Options[Entry.Index];
			const std::size_t Slash = Option.Label.find_last_of('/');
			NavigateContentFolder(ContentBrowserState, Slash == std::string::npos ? std::string{} : Option.Label.substr(0, Slash), Assets.get());
			ContentBrowserState.Selected = Option.Id;
			ContentBrowserState.bFocusRequested = true;
			if (ToolUI->IsDocumentInFront())
			{
				if (!bContentDrawerOpen)
				{
					ToggleContentDrawer();
				}
			}
			else
			{
				bContentBrowserOpen = bBottomPanelOpen = true;
			}
		}
		else
		{
			// A command usually takes arguments, so it lands in the Output Log's command line to finish and run.
			const std::string Line = CommandPaletteCommands[Entry.Index].Name + " ";
			CommandBuffer.fill('\0');
			std::ranges::copy_n(Line.begin(), static_cast<std::ptrdiff_t>(std::min(Line.size(), CommandBuffer.size() - 1)), CommandBuffer.begin());
			bFocusCommandRequested = true;
			if (ToolUI->IsDocumentInFront())
			{
				if (!bOutputLogDrawerOpen)
				{
					ToggleOutputLogDrawer();
				}
			}
			else
			{
				bOutputLogOpen = bBottomPanelOpen = true;
			}
		}

		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}
}
