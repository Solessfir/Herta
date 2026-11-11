#include "EditorFrameworkPrivate.h"

namespace Herta
{
namespace
{
inline constexpr std::array CategoryColors = {
    IM_COL32(126, 200, 255, 255),
    IM_COL32(142, 220, 182, 255),
    IM_COL32(156, 194, 210, 255),
    IM_COL32(203, 166, 255, 255),
    IM_COL32(166, 184, 224, 255),
    IM_COL32(115, 218, 224, 255),
    IM_COL32(152, 202, 194, 255),
    IM_COL32(188, 178, 220, 255),
    IM_COL32(166, 184, 255, 255),
    IM_COL32(160, 198, 230, 255),
    IM_COL32(135, 210, 154, 255),
    IM_COL32(182, 190, 214, 255),
};

[[nodiscard]] ImU32 ResolveLineColor(const FOutputLogLine& Line, const bool bColorizeCategories) noexcept
{
	switch (Line.Record.Level)
	{
		case ELogLevel::Warning:
			return PackColor(ToolUITheme::Warning);
		case ELogLevel::Error:
		case ELogLevel::Critical:
			return PackColor(ToolUITheme::Error);
		case ELogLevel::Trace:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextMuted);
			}

			break;
		case ELogLevel::Debug:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextSecondary);
			}

			break;
		case ELogLevel::Info:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextPrimary);
			}

			break;
		case ELogLevel::Off:
			return PackColor(ToolUITheme::TextMuted);
	}

	return CategoryColors[HashOutputLogCategory(Line.Record.Category) % CategoryColors.size()];
}

[[nodiscard]] FLogTextPosition HitTestText(const std::span<const FOutputLogLine> Lines, const FOutputLogColumns Columns, const ImVec2 TextOrigin, const float LineHeight, const ImVec2 MousePosition)
{
	if (Lines.empty())
	{
		return {};
	}

	std::size_t LineIndex = 0;

	if (MousePosition.y > TextOrigin.y)
	{
		LineIndex = std::min(static_cast<std::size_t>((MousePosition.y - TextOrigin.y) / LineHeight), Lines.size() - 1);
	}

	return {.Line = LineIndex, .Byte = FindOutputLogByteAtX(Lines[LineIndex], MousePosition.x - TextOrigin.x, Columns)};
}

void CopyBuffer(std::span<char> Destination, const std::string_view Source)
{
	std::ranges::fill(Destination, '\0');
	const std::size_t Count = std::min(Source.size(), Destination.size() - 1);
	std::ranges::copy(Source.substr(0, Count), Destination.begin());
}
}

void FEditorFramework::FImplementation::RebuildSuggestions(const std::string_view Prefix)
{
	if (HasConsoleCommandText(Prefix))
	{
		Suggestions = OutputLog->CompleteCommand(Prefix);
	}
	else
	{
		Suggestions.clear();
	}

	if (Suggestions.empty())
	{
		SuggestionIndex = -1;
	}
	else
	{
		SuggestionIndex = std::clamp(SuggestionIndex, 0, static_cast<int>(Suggestions.size()) - 1);
	}
}

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::SubmitCommand()
{
	bReclaimCommandFocus = false;
	ImGui::SetWindowFocus("Viewport");

	if (CommandBuffer[0] == '\0')
	{
		return {};
	}

	std::expected<void, FOutputLogError> SubmitResult = OutputLog->SubmitCommand(CommandBuffer.data());
	if (!SubmitResult)
	{
		return std::unexpected(FEditorFrameworkError{std::move(SubmitResult.error().Message)});
	}

	CommandBuffer.fill('\0');
	Suggestions.clear();
	SuggestionIndex = -1;
	return {};
}

void FEditorFramework::FImplementation::SetBottomPanelOpen(const bool bOpen)
{
	const bool bWasOpen = bBottomPanelOpen;
	if (!bOpen && bWasOpen)
	{
		const ImGuiWindow* const Browser = ImGui::FindWindowByID(ImHashStr("Content Browser"));
		bBottomBrowserSelected = Browser && Browser->DockNode && Browser->DockNode->SelectedTabId == Browser->TabId;
	}

	bBottomPanelOpen = bOpen;

	if (bOpen && !bContentBrowserOpen && !bOutputLogOpen)
	{
		bContentBrowserOpen = true;
	}

	if (bOpen && !bWasOpen)
	{
		bRestoreBottomPanelFocus = true;
		ContentBrowserState.bFocusFolderName |= !ContentBrowserState.RenamingFolder.empty();
	}
}

// The two drawers share the bottom of the workspace, so opening one closes the other.
void FEditorFramework::FImplementation::ToggleContentDrawer()
{
	bContentDrawerOpen = !bContentDrawerOpen;
	bOutputLogDrawerOpen &= !bContentDrawerOpen;
	ContentBrowserState.bFocusRequested = bContentDrawerOpen;
}

void FEditorFramework::FImplementation::ToggleOutputLogDrawer()
{
	bOutputLogDrawerOpen = !bOutputLogDrawerOpen;
	bContentDrawerOpen &= !bOutputLogDrawerOpen;
	bFocusOutputLogRequested = bOutputLogDrawerOpen;
}

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::DrawOutputLog()
{
	const bool bReceivedRecords = OutputLog->Synchronize();

	const bool bDrawer = ToolUI->IsDocumentInFront();
	bOutputLogDrawerOpen &= bDrawer;
	if (bDrawer ? !bOutputLogDrawerOpen : !bBottomPanelOpen || !bOutputLogOpen)
	{
		return {};
	}

	if (bFocusCommandRequested || bFocusOutputLogRequested)
	{
		ImGui::SetNextWindowFocus();
		ImGui::SetNextWindowCollapsed(false);
	}

	const bool bPanelVisible = bDrawer ? ToolUI->BeginPanel("Output Log###Output Log Drawer", &bOutputLogDrawerOpen, EToolUIPanelKind::Drawer) : ToolUI->BeginPanel("Output Log", &bOutputLogOpen);
	bFocusOutputLogRequested = false;

	if (!bPanelVisible)
	{
		ToolUI->EndPanel();
		return {};
	}

	const float ToolbarScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * ToolbarScale, 5.f * ToolbarScale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * ToolbarScale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * ToolbarScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;

	const auto ButtonWidth = [](const char* const Label)
	{
		return ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.f;
	};

	const float ClearWidth = ButtonWidth("Clear");
	const float CopyWidth = ButtonWidth("Copy");
	const auto VisibleLines = OutputLog->GetVisibleLines();
	std::size_t WarningCount = 0;
	std::optional<std::uint64_t> PreviousSequence;

	for (const FOutputLogLine& Line : VisibleLines)
	{
		if (Line.Record.Sequence != PreviousSequence && Line.Record.Level == ELogLevel::Warning)
		{
			++WarningCount;
		}

		PreviousSequence = Line.Record.Sequence;
	}

	const std::string CountLabel = std::to_string(WarningCount);
	const float CountFontSize = ImGui::GetFontSize() * 0.8f;
	const ImVec2 CountTextSize = ImGui::CalcTextSize(CountLabel.c_str());
	const ImVec2 CountSize{CountTextSize.x * 0.8f, CountTextSize.y * 0.8f};
	const ImVec2 BadgeSize{std::max(18.f * ToolbarScale, CountSize.x + 10.f * ToolbarScale), CountFontSize + 4.f * ToolbarScale};
	const float FilterWidth = ButtonWidth("Warnings") + Spacing + BadgeSize.x;
	const float ButtonsWidth = FilterWidth + ButtonWidth("Options") + ClearWidth + CopyWidth + Spacing * 3.f;
	const float AvailableWidth = ImGui::GetContentRegionAvail().x;
	const bool bSingleRow = AvailableWidth >= ButtonsWidth + Spacing + 160.f * ToolbarScale;
	ImGui::SetNextItemWidth(bSingleRow ? std::min(320.f * ToolbarScale, AvailableWidth - ButtonsWidth - Spacing) : -1.f);
	const bool bSearchChanged = ToolUI->DrawSearchField("##OutputLogSearch", "Search Log", SearchBuffer.data(), SearchBuffer.size());

	if (bSingleRow)
	{
		ImGui::SameLine();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.f, 0.5f});
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

	if (ImGui::Button("Warnings###OutputLogFilters", {FilterWidth, 0.f}))
	{
		ImGui::OpenPopup("OutputLogFilter");
	}

	const ImVec2 FilterMinimum = ImGui::GetItemRectMin();
	const ImVec2 FilterMaximum = ImGui::GetItemRectMax();
	const ImVec2 BadgeMinimum{FilterMaximum.x - ImGui::GetStyle().FramePadding.x - BadgeSize.x, (FilterMinimum.y + FilterMaximum.y - BadgeSize.y) * 0.5f};
	ImDrawList* const ToolbarDrawList = ImGui::GetWindowDrawList();
	ToolbarDrawList->AddRectFilled(BadgeMinimum, {BadgeMinimum.x + BadgeSize.x, BadgeMinimum.y + BadgeSize.y}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.08f}), 4.f * ToolbarScale);
	ToolbarDrawList->AddText(ImGui::GetFont(), CountFontSize, {BadgeMinimum.x + (BadgeSize.x - CountSize.x) * 0.5f, BadgeMinimum.y + (BadgeSize.y - CountSize.y) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), CountLabel.c_str());
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::SameLine();

	if (ImGui::Button("Options"))
	{
		ImGui::OpenPopup("OutputLogOptions");
	}

	const float RightX = ImGui::GetWindowContentRegionMax().x - ClearWidth - CopyWidth - Spacing;

	if (bSingleRow || RightX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + Spacing)
	{
		ImGui::SameLine(RightX);
	}

	const bool bClearRequested = ImGui::Button("Clear");
	ImGui::SameLine();
	const bool bCopyRequested = ImGui::Button("Copy");
	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(4);

	if (bClearRequested)
	{
		OutputLog->Clear();
	}

	if (bSearchChanged)
	{
		OutputLog->SetSearch(SearchBuffer.data());
	}

	if (ImGui::BeginPopup("OutputLogFilter"))
	{
		for (const ELogLevel Verbosity : {ELogLevel::Trace, ELogLevel::Debug, ELogLevel::Info, ELogLevel::Warning, ELogLevel::Error, ELogLevel::Critical})
		{
			bool bVisible = OutputLog->IsLevelVisible(Verbosity);
			const std::string LevelName{GetLogLevelName(Verbosity)};

			if (ImGui::MenuItem(LevelName.c_str(), nullptr, &bVisible))
			{
				OutputLog->SetLevelVisible(Verbosity, bVisible);
			}
		}

		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("OutputLogOptions"))
	{
		bool bAutoScroll = OutputLog->IsAutoScroll();
		bool bPaused = OutputLog->IsPaused();
		bool bColorize = OutputLog->IsCategoryColorizationEnabled();

		if (ImGui::MenuItem("Auto-scroll", nullptr, &bAutoScroll))
		{
			OutputLog->SetAutoScroll(bAutoScroll);
		}

		if (ImGui::MenuItem("Pause", nullptr, &bPaused))
		{
			OutputLog->SetPaused(bPaused);
		}

		if (ImGui::MenuItem("Colorize categories", nullptr, &bColorize))
		{
			OutputLog->SetCategoryColorization(bColorize);
		}

		ImGui::EndPopup();
	}

	if (bCopyRequested)
	{
		ImGui::SetClipboardText(OutputLog->CopySelectionOrVisible().c_str());
	}

	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float CommandRowPadding = ImGui::GetStyle().WindowPadding.y;
	const float FooterHeight = ImGui::GetFontSize() + 10.f * InterfaceScale + CommandRowPadding;
	ToolUI->PushLogFont();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.f * InterfaceScale, 8.f * InterfaceScale});

	if (ImGui::BeginChild("OutputLogEntries", {0.f, -FooterHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const std::span<const FOutputLogLine> Lines = OutputLog->GetVisibleLines();
		const std::span<const std::string> TextLines = OutputLog->GetVisibleText();
		const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
		const bool bShouldScroll = !Lines.empty() && ShouldScrollOutputLog(bReceivedRecords, OutputLog->IsAutoScroll(), bWasAtBottom, OutputLog->HasTailRequest());
		const float LineHeight = ImGui::GetFontSize() + 3.f * InterfaceScale;
		const float TextOffsetY = (LineHeight - ImGui::GetFontSize()) * 0.5f;
		const ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		float TimeWidth = 0.f;
		float CategoryWidth = 0.f;

		for (const FOutputLogLine& Line : Lines)
		{
			TimeWidth = std::max(TimeWidth, ImGui::CalcTextSize(Line.Text.data(), Line.Text.data() + Line.TimeEnd, false).x);
			CategoryWidth = std::max(CategoryWidth, ImGui::CalcTextSize(Line.Text.data() + Line.CategoryBegin, Line.Text.data() + Line.CategoryEnd, false).x);
		}

		const float CategoryX = TimeWidth + 12.f * InterfaceScale;
		const FOutputLogColumns Columns{.CategoryX = CategoryX, .MessageX = CategoryX + CategoryWidth + 8.f * InterfaceScale};
		float ContentWidth = AvailableSize.x;

		for (const FOutputLogLine& Line : Lines)
		{
			ContentWidth = std::max(ContentWidth, MeasureOutputLogTextPrefix(Line, Line.Text.size(), Columns) + 8.f * InterfaceScale);
		}

		// Available height includes the scroll offset, so it must not determine content height.
		const float ContentHeight = std::max(1.f, static_cast<float>(TextLines.size()) * LineHeight);
		const ImVec2 TextOrigin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##OutputLogText", {ContentWidth, ContentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
		FLogTextSelection& Selection = OutputLog->GetSelection();

		if (ImGui::IsItemActivated())
		{
			Selection.Begin(HitTestText(Lines, Columns, TextOrigin, LineHeight, ImGui::GetMousePos()), ImGui::GetIO().KeyShift);
		}

		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 MousePosition = ImGui::GetMousePos();
			Selection.Update(HitTestText(Lines, Columns, TextOrigin, LineHeight, MousePosition));
			const ImVec2 WindowPosition = ImGui::GetWindowPos();
			const ImVec2 ContentMinimum = ImGui::GetWindowContentRegionMin();
			const ImVec2 ContentMaximum = ImGui::GetWindowContentRegionMax();
			const float ScrollStep = 360.f * ImGui::GetIO().DeltaTime;

			if (MousePosition.y < WindowPosition.y + ContentMinimum.y)
			{
				ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - ScrollStep));
			}
			else if (MousePosition.y > WindowPosition.y + ContentMaximum.y)
			{
				ImGui::SetScrollY(ImGui::GetScrollY() + ScrollStep);
			}
		}

		if (bShouldScroll)
		{
			ImGui::SetScrollHereY(1.f);
			OutputLog->AcknowledgeTailRequest();
		}

		if (ImGui::IsWindowFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
		{
			Selection.SelectAll(TextLines);
		}

		if (ImGui::IsWindowFocused() && Selection.HasSelection() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
		{
			const std::string Clipboard = Selection.Copy(TextLines);
			ImGui::SetClipboardText(Clipboard.c_str());
		}

		const auto [SelectionFirst, SelectionLast] = Selection.GetOrderedRange();
		const bool bHasSelection = Selection.HasSelection();
		const ImU32 SelectionColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
		const float WindowTop = ImGui::GetWindowPos().y;
		const float WindowBottom = WindowTop + ImGui::GetWindowSize().y;
		const std::size_t FirstVisibleLine = TextOrigin.y < WindowTop ? std::min(TextLines.size(), static_cast<std::size_t>((WindowTop - TextOrigin.y) / LineHeight)) : 0;
		const std::size_t LastVisibleLine = std::min(TextLines.size(), static_cast<std::size_t>(std::max(0.f, (WindowBottom - TextOrigin.y) / LineHeight)) + 1);
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();

		for (std::size_t LineIndex = FirstVisibleLine; LineIndex < LastVisibleLine; ++LineIndex)
		{
			const FOutputLogLine& Line = Lines[LineIndex];
			const std::string& Text = TextLines[LineIndex];
			const float LineY = TextOrigin.y + static_cast<float>(LineIndex) * LineHeight;

			if (bHasSelection && LineIndex >= SelectionFirst.Line && LineIndex <= SelectionLast.Line)
			{
				const std::size_t FirstByte = LineIndex == SelectionFirst.Line ? SelectionFirst.Byte : 0;
				const std::size_t LastByte = LineIndex == SelectionLast.Line ? SelectionLast.Byte : Text.size();
				const float SelectionX = TextOrigin.x + MeasureOutputLogTextPrefix(Line, FirstByte, Columns);
				float SelectionEndX = TextOrigin.x + MeasureOutputLogTextPrefix(Line, LastByte, Columns);

				if (LineIndex < SelectionLast.Line)
				{
					SelectionEndX += ImGui::GetFontSize() * 0.35f;
				}

				DrawList->AddRectFilled({SelectionX, LineY}, {std::max(SelectionX + 1.f, SelectionEndX), LineY + LineHeight}, SelectionColor);
			}

			const ImU32 Color = ResolveLineColor(Line, OutputLog->IsCategoryColorizationEnabled());
			DrawList->AddText({TextOrigin.x, LineY + TextOffsetY}, PackColor(ToolUITheme::TextMuted), Text.data(), Text.data() + Line.TimeEnd);
			DrawList->AddText({TextOrigin.x + Columns.CategoryX, LineY + TextOffsetY}, Color, Text.data() + Line.CategoryBegin, Text.data() + Line.CategoryEnd);
			DrawList->AddText({TextOrigin.x + Columns.MessageX, LineY + TextOffsetY}, Color, Text.data() + Line.MessageBegin, Text.data() + Text.size());
		}
	}

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ToolUI->PopLogFont();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + CommandRowPadding - ImGui::GetStyle().ItemSpacing.y);

	struct FInputCallbackContext
	{
		FImplementation* Editor;
	};

	FInputCallbackContext CallbackContext{this};

	const auto InputCallback = [](ImGuiInputTextCallbackData* const Data)
	{
		FImplementation& Editor = *static_cast<FInputCallbackContext*>(Data->UserData)->Editor;

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
		{
			return HandleConsoleInputShortcuts(*Data);
		}

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit || Data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			HandleConsoleInputShortcuts(*Data);
			if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit || Data->BufDirty)
			{
				Editor.SuggestionIndex = 0;
				Editor.RebuildSuggestions({Data->Buf, static_cast<std::size_t>(Data->BufTextLen)});
			}

			return 0;
		}

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			if (Editor.SuggestionIndex >= 0 && Editor.SuggestionIndex < static_cast<int>(Editor.Suggestions.size()))
			{
				const std::string& Suggestion = Editor.Suggestions[static_cast<std::size_t>(Editor.SuggestionIndex)];
				Data->DeleteChars(0, Data->BufTextLen);
				Data->InsertChars(0, Suggestion.data(), Suggestion.data() + Suggestion.size());
				Editor.Suggestions.clear();
				Editor.SuggestionIndex = -1;
			}

			return 0;
		}

		if (!Editor.Suggestions.empty())
		{
			if (Data->EventKey == ImGuiKey_UpArrow)
			{
				Editor.SuggestionIndex = std::max(0, Editor.SuggestionIndex - 1);
			}
			else if (Data->EventKey == ImGuiKey_DownArrow)
			{
				Editor.SuggestionIndex = std::min(static_cast<int>(Editor.Suggestions.size()) - 1, Editor.SuggestionIndex + 1);
			}

			return 0;
		}

		const int Direction = Data->EventKey == ImGuiKey_UpArrow ? -1 : Data->EventKey == ImGuiKey_DownArrow ? 1
		                                                                                                     : 0;

		if (Direction != 0)
		{
			const std::string HistoryCommand = Editor.OutputLog->NavigateHistory(Direction);
			Data->DeleteChars(0, Data->BufTextLen);

			if (!HistoryCommand.empty())
			{
				Data->InsertChars(0, HistoryCommand.data(), HistoryCommand.data() + HistoryCommand.size());
			}
		}

		return 0;
	};

	constexpr ImGuiInputTextFlags CommandFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCharFilter;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * InterfaceScale, 5.f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	constexpr const char* SubmitLabel = "Enter";
	const float SubmitWidth = ImGui::CalcTextSize(SubmitLabel).x + ImGui::GetStyle().FramePadding.x * 2.f;
	ImGui::SetNextItemWidth(-(SubmitWidth + ImGui::GetStyle().ItemSpacing.x));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {32.f * InterfaceScale, 5.f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.5f + 5.f * InterfaceScale);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(PackColor(ToolUITheme::Border)));

	if (bFocusCommandRequested)
	{
		ImGui::SetKeyboardFocusHere();
		bFocusCommandRequested = false;
	}

	ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
	const bool bCommandSubmitted = ImGui::InputTextWithHint("##OutputLogCommand", "Enter Console Command", CommandBuffer.data(), CommandBuffer.size(), CommandFlags, InputCallback, &CallbackContext);
	const bool bCommandActive = ImGui::IsItemActive();
	const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

	if (bEscapePressed)
	{
		Suggestions.clear();
		SuggestionIndex = -1;
	}

	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(3);

	if (bCommandSubmitted)
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleVar(2);
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}

	const ImVec2 InputMinimum = ImGui::GetItemRectMin();
	const ImVec2 InputMaximum = ImGui::GetItemRectMax();
	const float PromptX = InputMinimum.x + 12.f * InterfaceScale;
	const float PromptY = (InputMinimum.y + InputMaximum.y) * 0.5f;
	const ImU32 PromptColor = PackColor(ToolUITheme::TextMuted);
	ImDrawList* const CommandDraw = ImGui::GetWindowDrawList();
	CommandDraw->PushClipRect(InputMinimum, InputMaximum, true);
	ToolUIIcon(EToolUIMenuIcon::LogPrompt, PromptX + 4.4f * InterfaceScale, PromptY, InterfaceScale, PromptColor);
	CommandDraw->PopClipRect();

	if (bReclaimCommandFocus)
	{
		ImGui::SetKeyboardFocusHere(-1);
		bReclaimCommandFocus = false;
	}

	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});

	if (ImGui::Button(SubmitLabel, {SubmitWidth, 0.f}))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar(4);
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}

	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(4);

	std::optional<std::string> ClickedSuggestion;
	bool bSuggestionsHovered = false;

	if (!Suggestions.empty())
	{
		const std::size_t VisibleCount = std::min<std::size_t>(6, Suggestions.size());
		const float PopupPadding = 4.f * InterfaceScale;
		const float PopupHeight = static_cast<float>(VisibleCount) * ImGui::GetFrameHeight() + PopupPadding * 2.f;
		const ImGuiViewport* const Viewport = ImGui::GetWindowViewport();
		ImGui::SetNextWindowPos({InputMinimum.x, std::max(Viewport->WorkPos.y, InputMinimum.y - PopupHeight)});
		ImGui::SetNextWindowSize({InputMaximum.x - InputMinimum.x, PopupHeight});
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::SetNextWindowBgAlpha(0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PopupPadding, PopupPadding});
		constexpr ImGuiWindowFlags SuggestionFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

		if (ImGui::Begin("Command suggestions###OutputLogCommandSuggestions", nullptr, SuggestionFlags))
		{
			bSuggestionsHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			const ImVec2 Position = ImGui::GetWindowPos();
			const ImVec2 Size = ImGui::GetWindowSize();
			ImGui::GetWindowDrawList()->PushClipRect(Position, {Position.x + Size.x, Position.y + Size.y}, false);
			ToolUI->DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI->GetMetrics().PopupRounding * InterfaceScale);
			ImGui::GetWindowDrawList()->PopClipRect();

			for (std::size_t Index = 0; Index < Suggestions.size(); ++Index)
			{
				ImGui::PushID(static_cast<int>(Index));

				if (ImGui::Selectable(Suggestions[Index].c_str(), SuggestionIndex == static_cast<int>(Index)))
				{
					ClickedSuggestion = Suggestions[Index];
				}

				if (ImGui::IsItemHovered())
				{
					SuggestionIndex = static_cast<int>(Index);
				}

				ImGui::PopID();
			}
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	if (ClickedSuggestion)
	{
		CopyBuffer(CommandBuffer, *ClickedSuggestion);
		Suggestions.clear();
		SuggestionIndex = -1;
		bReclaimCommandFocus = true;
	}
	else if (ShouldDismissConsoleSuggestions(bCommandActive, bSuggestionsHovered, bEscapePressed))
	{
		Suggestions.clear();
		SuggestionIndex = -1;
	}

	ToolUI->EndPanel();
	return {};
}
}
