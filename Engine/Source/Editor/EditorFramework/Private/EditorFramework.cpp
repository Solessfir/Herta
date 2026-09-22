#include "Herta/EditorFramework/EditorFramework.h"

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr std::array CategoryColors = {
    IM_COL32(126, 200, 255, 255),
    IM_COL32(142, 220, 182, 255),
    IM_COL32(244, 202, 128, 255),
    IM_COL32(203, 166, 255, 255),
    IM_COL32(255, 157, 170, 255),
    IM_COL32(115, 218, 224, 255),
    IM_COL32(192, 215, 128, 255),
    IM_COL32(240, 166, 219, 255),
    IM_COL32(166, 184, 255, 255),
    IM_COL32(235, 186, 151, 255),
    IM_COL32(135, 210, 154, 255),
    IM_COL32(225, 168, 255, 255)};

[[nodiscard]] ImU32 PackColor(const FToolUIColor Color) noexcept
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

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

[[nodiscard]] std::size_t GetNextUtf8Boundary(const std::string_view Text, const std::size_t ByteOffset) noexcept
{
	if (ByteOffset >= Text.size())
	{
		return Text.size();
	}

	const unsigned char FirstByte = static_cast<unsigned char>(Text[ByteOffset]);
	std::size_t CodePointSize = 1;
	if ((FirstByte & 0xE0u) == 0xC0u)
	{
		CodePointSize = 2;
	}
	else if ((FirstByte & 0xF0u) == 0xE0u)
	{
		CodePointSize = 3;
	}
	else if ((FirstByte & 0xF8u) == 0xF0u)
	{
		CodePointSize = 4;
	}
	return std::min(Text.size(), ByteOffset + CodePointSize);
}

[[nodiscard]] float MeasureTextPrefix(const std::string_view Text, const std::size_t ByteCount)
{
	const std::size_t ClampedByteCount = std::min(ByteCount, Text.size());
	const char* const TextBegin = Text.data();
	return ImGui::CalcTextSize(TextBegin, TextBegin + ClampedByteCount, false).x;
}

[[nodiscard]] std::size_t FindByteAtX(const std::string_view Text, const float LocalX)
{
	if (LocalX <= 0.0f)
	{
		return 0;
	}

	float TextX = 0.0f;
	for (std::size_t ByteOffset = 0; ByteOffset < Text.size();)
	{
		const std::size_t NextByteOffset = GetNextUtf8Boundary(Text, ByteOffset);
		const float CharacterWidth = ImGui::CalcTextSize(Text.data() + ByteOffset, Text.data() + NextByteOffset, false).x;
		if (LocalX < TextX + CharacterWidth * 0.5f)
		{
			return ByteOffset;
		}
		TextX += CharacterWidth;
		ByteOffset = NextByteOffset;
	}
	return Text.size();
}

[[nodiscard]] FLogTextPosition HitTestText(const std::span<const std::string> Lines, const ImVec2 TextOrigin, const float LineHeight, const ImVec2 MousePosition)
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
	return {LineIndex, FindByteAtX(Lines[LineIndex], MousePosition.x - TextOrigin.x)};
}

void CopyBuffer(std::span<char> Destination, const std::string_view Source)
{
	std::fill(Destination.begin(), Destination.end(), '\0');
	const std::size_t Count = std::min(Source.size(), Destination.size() - 1);
	std::ranges::copy(Source.substr(0, Count), Destination.begin());
}

[[nodiscard]] constexpr const char* GetPanelTransparencyLabel(const EPanelTransparency Mode) noexcept
{
	switch (Mode)
	{
		case EPanelTransparency::AllPanels:
			return "All panels";
		case EPanelTransparency::FloatingOnly:
			return "Floating panels";
		case EPanelTransparency::DockedOnly:
			return "Docked panels";
		case EPanelTransparency::Disabled:
			return "Opaque panels";
	}

	return "Unknown";
}
}

struct FEditorFramework::FImplementation
{
	FToolUIContext* ToolUI = nullptr;
	std::unique_ptr<FOutputLogModel> OutputLog;
	std::array<char, 512> SearchBuffer = {};
	std::array<char, 512> CommandBuffer = {};
	std::vector<std::string> Suggestions;
	int SuggestionIndex = -1;
	bool bOutputLogOpen = true;
	bool bReclaimCommandFocus = false;
	std::uint64_t ViewportTexture = 0;
	FExtent2D ViewportExtent{960, 540};

	[[nodiscard]] std::expected<void, FEditorFrameworkError> DrawOutputLog();
	void DrawStartPanel();
	void DrawViewport();
	void RebuildSuggestions();
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();
};

std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> FEditorFramework::Create(const FEditorFrameworkDescriptor Descriptor)
{
	if (Descriptor.Log == nullptr || Descriptor.Commands == nullptr || Descriptor.ToolUI == nullptr)
	{
		return std::unexpected(FEditorFrameworkError{"EditorFramework requires logging, commands, and ToolUI"});
	}

	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> OutputLog = FOutputLogModel::Create(*Descriptor.Log, *Descriptor.Commands);
	if (!OutputLog)
	{
		return std::unexpected(FEditorFrameworkError{std::move(OutputLog.error().Message)});
	}

	try
	{
		auto Implementation = std::make_unique<FImplementation>();
		Implementation->ToolUI = Descriptor.ToolUI;
		Implementation->OutputLog = std::move(*OutputLog);
		return std::unique_ptr<FEditorFramework>(new FEditorFramework(std::move(Implementation)));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorFrameworkError{Exception.what()});
	}
}

FEditorFramework::FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept
    : Implementation(std::move(Implementation))
{
}

FEditorFramework::~FEditorFramework() = default;

std::expected<void, FEditorFrameworkError> FEditorFramework::Draw()
{
	try
	{
		Implementation->ToolUI->DrawWorkspace("Herta Editor");
		Implementation->DrawStartPanel();
		Implementation->DrawViewport();
		return Implementation->DrawOutputLog();
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorFrameworkError{Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorFrameworkError{"Editor drawing failed due to an unknown error"});
	}
}

FOutputLogModel& FEditorFramework::GetOutputLog() noexcept
{
	return *Implementation->OutputLog;
}

const FOutputLogModel& FEditorFramework::GetOutputLog() const noexcept
{
	return *Implementation->OutputLog;
}

void FEditorFramework::SetViewportImage(const std::uint64_t TextureId) noexcept
{
	Implementation->ViewportTexture = TextureId;
}

FExtent2D FEditorFramework::GetViewportExtent() const noexcept
{
	return Implementation->ViewportExtent;
}

void FEditorFramework::FImplementation::DrawViewport()
{
	ImGui::SetNextWindowSize({960, 540}, ImGuiCond_FirstUseEver);
	if (ToolUI->BeginPanel("Viewport"))
	{
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		const float Scale = ImGui::GetWindowViewport()->DpiScale;
		ViewportExtent = {static_cast<std::uint32_t>(std::clamp(Size.x * Scale, 1.0f, 4096.0f)), static_cast<std::uint32_t>(std::clamp(Size.y * Scale, 1.0f, 4096.0f))};
		if (ViewportTexture != 0 && Size.x > 0 && Size.y > 0)
		{
			const ImVec2 ImageMinimum = ImGui::GetCursorScreenPos();
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(ViewportTexture)), Size);
			const std::string FpsText = std::format("{:.0f} FPS", ImGui::GetIO().Framerate);
			const ImVec2 TextSize = ImGui::CalcTextSize(FpsText.c_str());
			const ImVec2 TextPosition{ImageMinimum.x + 12.0f, ImageMinimum.y + 10.0f};
			FToolUIColor Background = ToolUITheme::Surface0;
			Background.Alpha = 210;
			ImDrawList* const DrawList = ImGui::GetWindowDrawList();
			DrawList->AddRectFilled({TextPosition.x - 6.0f, TextPosition.y - 4.0f}, {TextPosition.x + TextSize.x + 6.0f, TextPosition.y + TextSize.y + 4.0f}, PackColor(Background), 4.0f);
			DrawList->AddText(TextPosition, PackColor(ToolUITheme::TextPrimary), FpsText.c_str());
		}
	}
	else
	{
		ViewportExtent = {};
	}
	ToolUI->EndPanel();
}

void FEditorFramework::FImplementation::DrawStartPanel()
{
	if (!ToolUI->BeginPanel("Start"))
	{
		ToolUI->EndPanel();
		return;
	}

	ImGui::TextDisabled("HERTA / NATIVE C++23");
	ImGui::Spacing();
	ImGui::TextUnformatted("Build something remarkable.");
	ImGui::TextDisabled("The Viewport renders a textured mesh through Herta RHI and RenderGraph.");
	ImGui::Spacing();
	ImGui::SeparatorText("Workspace appearance");

	FEditorAppearance Appearance = ToolUI->GetAppearance();
	const char* const PanelMode = GetPanelTransparencyLabel(Appearance.PanelTransparency);
	if (ImGui::BeginCombo("Panel transparency", PanelMode))
	{
		constexpr std::array Modes = {
		    std::pair{EPanelTransparency::AllPanels, "All panels"},
		    std::pair{EPanelTransparency::FloatingOnly, "Floating panels"},
		    std::pair{EPanelTransparency::DockedOnly, "Docked panels"},
		    std::pair{EPanelTransparency::Disabled, "Opaque panels"}};
		for (const auto& [Mode, Label] : Modes)
		{
			if (ImGui::Selectable(Label, Appearance.PanelTransparency == Mode))
			{
				Appearance.PanelTransparency = Mode;
			}
		}
		ImGui::EndCombo();
	}

	int GradientHeightPercent = static_cast<int>(std::lround(Appearance.GradientHeight * 100.0f));
	int SaturationPercent = static_cast<int>(std::lround(Appearance.Saturation * 100.0f));
	int IntensityPercent = static_cast<int>(std::lround(Appearance.Intensity * 100.0f));
	if (ImGui::SliderInt("Gradient height", &GradientHeightPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.GradientHeight = static_cast<float>(GradientHeightPercent) / 100.0f;
	}
	if (ImGui::SliderInt("Color saturation", &SaturationPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.Saturation = static_cast<float>(SaturationPercent) / 100.0f;
	}
	if (ImGui::SliderInt("Color intensity", &IntensityPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.Intensity = static_cast<float>(IntensityPercent) / 100.0f;
	}
	std::array Color = {
	    static_cast<float>(Appearance.Accent.Red) / 255.0f,
	    static_cast<float>(Appearance.Accent.Green) / 255.0f,
	    static_cast<float>(Appearance.Accent.Blue) / 255.0f};
	if (ImGui::ColorEdit3("Color", Color.data()))
	{
		Appearance.Accent.Red = static_cast<std::uint8_t>(std::lround(Color[0] * 255.0f));
		Appearance.Accent.Green = static_cast<std::uint8_t>(std::lround(Color[1] * 255.0f));
		Appearance.Accent.Blue = static_cast<std::uint8_t>(std::lround(Color[2] * 255.0f));
	}
	for (std::size_t Index = 0; Index < ToolUITheme::Presets.size(); ++Index)
	{
		if (Index > 0)
		{
			ImGui::SameLine();
		}
		const FToolUIColorPreset& Preset = ToolUITheme::Presets[Index];
		ImGui::PushID(static_cast<int>(Index));
		if (ImGui::ColorButton("##Preset", ImGui::ColorConvertU32ToFloat4(PackColor(Preset.Color)), ImGuiColorEditFlags_NoTooltip, {22.0f, 22.0f}))
		{
			Appearance.Accent = Preset.Color;
		}
		if (ImGui::IsItemHovered())
		{
			const char* const PresetName = Preset.Name.data();
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(PresetName, PresetName + Preset.Name.size());
			ImGui::EndTooltip();
		}
		ImGui::PopID();
	}
	ToolUI->SetAppearance(Appearance);
	ToolUI->EndPanel();
}

void FEditorFramework::FImplementation::RebuildSuggestions()
{
	std::expected<std::vector<std::string>, FOutputLogError> Completion = OutputLog->CompleteCommand(CommandBuffer.data());
	Suggestions = Completion ? std::move(*Completion) : std::vector<std::string>{};
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
	bReclaimCommandFocus = true;
	return {};
}

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::DrawOutputLog()
{
	std::expected<bool, FOutputLogError> SynchronizeResult = OutputLog->Synchronize();
	if (!SynchronizeResult)
	{
		return std::unexpected(FEditorFrameworkError{std::move(SynchronizeResult.error().Message)});
	}
	const bool bReceivedRecords = *SynchronizeResult;

	if (!ToolUI->BeginPanel("Output Log", &bOutputLogOpen))
	{
		ToolUI->EndPanel();
		return {};
	}

	bool bCopyRequested = false;
	if (ImGui::Button("Clear"))
	{
		OutputLog->Clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy"))
	{
		bCopyRequested = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Filter"))
	{
		ImGui::OpenPopup("OutputLogFilter");
	}
	if (ImGui::BeginPopup("OutputLogFilter"))
	{
		for (const ELogLevel Level : {ELogLevel::Trace, ELogLevel::Debug, ELogLevel::Info, ELogLevel::Warning, ELogLevel::Error, ELogLevel::Critical})
		{
			bool bVisible = OutputLog->IsLevelVisible(Level);
			const std::string LevelName{GetLogLevelName(Level)};
			if (ImGui::MenuItem(LevelName.c_str(), nullptr, &bVisible))
			{
				std::expected<void, FOutputLogError> FilterResult = OutputLog->SetLevelVisible(Level, bVisible);
				if (!FilterResult)
				{
					ImGui::EndPopup();
					ToolUI->EndPanel();
					return std::unexpected(FEditorFrameworkError{std::move(FilterResult.error().Message)});
				}
			}
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Options"))
	{
		ImGui::OpenPopup("OutputLogOptions");
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
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputTextWithHint("##OutputLogSearch", "Search messages, categories, and verbosity", SearchBuffer.data(), SearchBuffer.size()))
	{
		std::expected<void, FOutputLogError> SearchResult = OutputLog->SetSearch(SearchBuffer.data());
		if (!SearchResult)
		{
			ToolUI->EndPanel();
			return std::unexpected(FEditorFrameworkError{std::move(SearchResult.error().Message)});
		}
	}

	if (bCopyRequested)
	{
		std::expected<std::string, FOutputLogError> Clipboard = OutputLog->CopySelectionOrVisible();
		if (!Clipboard)
		{
			ToolUI->EndPanel();
			return std::unexpected(FEditorFrameworkError{std::move(Clipboard.error().Message)});
		}
		ImGui::SetClipboardText(Clipboard->c_str());
	}

	const float InterfaceScale = ImGui::GetFontSize() / 15.0f;
	const float FooterHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f * InterfaceScale, 4.0f * InterfaceScale});
	if (ImGui::BeginChild("OutputLogEntries", {0.0f, -FooterHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const std::span<const FOutputLogLine> Lines = OutputLog->GetVisibleLines();
		const std::span<const std::string> TextLines = OutputLog->GetVisibleText();
		const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		const bool bShouldScroll = !Lines.empty() && ShouldScrollOutputLog(bReceivedRecords, OutputLog->IsAutoScroll(), bWasAtBottom, OutputLog->HasTailRequest());
		const float LineHeight = ImGui::GetFontSize() + 3.0f * InterfaceScale;
		const float TextOffsetY = (LineHeight - ImGui::GetFontSize()) * 0.5f;
		const ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		float ContentWidth = AvailableSize.x;
		for (const std::string& Text : TextLines)
		{
			ContentWidth = std::max(ContentWidth, ImGui::CalcTextSize(Text.data(), Text.data() + Text.size(), false).x + 8.0f * InterfaceScale);
		}

		const float ContentHeight = std::max(AvailableSize.y, static_cast<float>(TextLines.size()) * LineHeight);
		const ImVec2 TextOrigin = ImGui::GetCursorScreenPos();
		(void)ImGui::InvisibleButton("##OutputLogText", {ContentWidth, ContentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
		FLogTextSelection& Selection = OutputLog->GetSelection();
		if (ImGui::IsItemActivated())
		{
			Selection.Begin(HitTestText(TextLines, TextOrigin, LineHeight, ImGui::GetMousePos()), ImGui::GetIO().KeyShift);
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 MousePosition = ImGui::GetMousePos();
			Selection.Update(HitTestText(TextLines, TextOrigin, LineHeight, MousePosition));
			const ImVec2 WindowPosition = ImGui::GetWindowPos();
			const ImVec2 ContentMinimum = ImGui::GetWindowContentRegionMin();
			const ImVec2 ContentMaximum = ImGui::GetWindowContentRegionMax();
			const float ScrollStep = 360.0f * ImGui::GetIO().DeltaTime;
			if (MousePosition.y < WindowPosition.y + ContentMinimum.y)
			{
				ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - ScrollStep));
			}
			else if (MousePosition.y > WindowPosition.y + ContentMaximum.y)
			{
				ImGui::SetScrollY(ImGui::GetScrollY() + ScrollStep);
			}
		}
		if (bShouldScroll)
		{
			ImGui::SetScrollHereY(1.0f);
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
		const std::size_t LastVisibleLine = std::min(TextLines.size(), static_cast<std::size_t>(std::max(0.0f, (WindowBottom - TextOrigin.y) / LineHeight)) + 1);
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();
		for (std::size_t LineIndex = FirstVisibleLine; LineIndex < LastVisibleLine; ++LineIndex)
		{
			const std::string& Text = TextLines[LineIndex];
			const float LineY = TextOrigin.y + static_cast<float>(LineIndex) * LineHeight;
			if (bHasSelection && LineIndex >= SelectionFirst.Line && LineIndex <= SelectionLast.Line)
			{
				const std::size_t FirstByte = LineIndex == SelectionFirst.Line ? SelectionFirst.Byte : 0;
				const std::size_t LastByte = LineIndex == SelectionLast.Line ? SelectionLast.Byte : Text.size();
				const float SelectionX = TextOrigin.x + MeasureTextPrefix(Text, FirstByte);
				float SelectionEndX = TextOrigin.x + MeasureTextPrefix(Text, LastByte);
				if (LineIndex < SelectionLast.Line)
				{
					SelectionEndX += ImGui::GetFontSize() * 0.35f;
				}
				DrawList->AddRectFilled({SelectionX, LineY}, {std::max(SelectionX + 1.0f, SelectionEndX), LineY + LineHeight}, SelectionColor);
			}
			DrawList->AddText({TextOrigin.x, LineY + TextOffsetY}, ResolveLineColor(Lines[LineIndex], OutputLog->IsCategoryColorizationEnabled()), Text.data(), Text.data() + Text.size());
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();

	struct FInputCallbackContext
	{
		FImplementation* Editor;
	};
	FInputCallbackContext CallbackContext{this};
	const auto InputCallback = [](ImGuiInputTextCallbackData* const Data)
	{
		FImplementation& Editor = *static_cast<FInputCallbackContext*>(Data->UserData)->Editor;
		if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
		{
			Editor.SuggestionIndex = 0;
			Editor.RebuildSuggestions();
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
			std::expected<std::string, FOutputLogError> HistoryCommand = Editor.OutputLog->NavigateHistory(Direction);
			Data->DeleteChars(0, Data->BufTextLen);
			if (HistoryCommand && !HistoryCommand->empty())
			{
				Data->InsertChars(0, HistoryCommand->data(), HistoryCommand->data() + HistoryCommand->size());
			}
		}
		return 0;
	};

	constexpr ImGuiInputTextFlags CommandFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackHistory;
	constexpr const char* SubmitLabel = "Submit";
	const float SubmitWidth = ImGui::CalcTextSize(SubmitLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(-(SubmitWidth + ImGui::GetStyle().ItemSpacing.x));
	if (ImGui::InputTextWithHint("##OutputLogCommand", "Enter command, or type help", CommandBuffer.data(), CommandBuffer.size(), CommandFlags, InputCallback, &CallbackContext))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}
	const ImVec2 InputMinimum = ImGui::GetItemRectMin();
	const ImVec2 InputMaximum = ImGui::GetItemRectMax();
	if (bReclaimCommandFocus)
	{
		ImGui::SetKeyboardFocusHere(-1);
		bReclaimCommandFocus = false;
	}
	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
	if (ImGui::Button(SubmitLabel, {SubmitWidth, 0.0f}))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleVar();
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}
	ImGui::PopStyleVar();

	std::optional<std::string> ClickedSuggestion;
	if (!Suggestions.empty())
	{
		const std::size_t VisibleCount = std::min<std::size_t>(6, Suggestions.size());
		const float PopupPadding = 4.0f * InterfaceScale;
		const float PopupHeight = static_cast<float>(VisibleCount) * ImGui::GetFrameHeight() + PopupPadding * 2.0f;
		const ImGuiViewport* const Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({InputMinimum.x, std::max(Viewport->WorkPos.y, InputMinimum.y - PopupHeight)});
		ImGui::SetNextWindowSize({InputMaximum.x - InputMinimum.x, PopupHeight});
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PopupPadding, PopupPadding});
		constexpr ImGuiWindowFlags SuggestionFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("Command suggestions###OutputLogCommandSuggestions", nullptr, SuggestionFlags))
		{
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

	ToolUI->EndPanel();
	return {};
}
}
