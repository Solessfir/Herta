#include "NumericField.h"

#include "Herta/EditorCore/TransformText.h"

#include <imgui_internal.h>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Herta
{
namespace
{
struct FNumericEditState
{
	ImGuiID Id = 0;
	std::vector<char> Text = std::vector<char>(257);
	// Enter on unchanged text keeps the exact value instead of the rounded text.
	std::string InitialText;
	int LastFrame = -1;
	float InitialValue = 0.f;
	bool bTextEditing = false;
	bool bGestureStarted = false;
	bool bHadFocus = false;
	bool bFocusRequested = false;
	bool bMouseActivation = false;
	bool bInvalid = false;
};

std::unordered_map<ImGuiContext*, FNumericEditState> NumericEditStates;

FNumericEditState& GetEditState()
{
	ImGuiContext* const Context = ImGui::GetCurrentContext();
	const auto [Entry, bInserted] = NumericEditStates.try_emplace(Context);
	if (bInserted)
	{
		ImGuiContextHook Hook;
		Hook.Type = ImGuiContextHookType_Shutdown;

		Hook.Callback = [](ImGuiContext* const DestroyedContext, ImGuiContextHook*)
		{
			NumericEditStates.erase(DestroyedContext);
		};

		ImGui::AddContextHook(Context, &Hook);
	}

	return Entry->second;
}

int UpdateInputState(ImGuiInputTextCallbackData* const Data)
{
	FNumericEditState& State = *static_cast<FNumericEditState*>(Data->UserData);
	if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		// Preserve oversized input so validation rejects it instead of evaluating a truncated prefix.
		State.Text.resize(static_cast<std::size_t>(Data->BufSize));
		Data->Buf = State.Text.data();
	}
	else if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
	{
		State.bInvalid = false;
	}

	return 0;
}

// The value as the field displays it, without surrounding units or labels. A display with no numeric conversion, such as a clock time, is edited as shown only when the field can parse it back.
std::optional<std::string> FormatEditableValue(const float Value, const char* const Format, const bool bHasParser)
{
	const char* const Conversion = ImParseFormatFindStart(Format);
	if (*Conversion == '\0')
	{
		return bHasParser ? std::optional<std::string>{Format} : std::nullopt;
	}

	const std::string Specifier(Conversion, ImParseFormatFindEnd(Conversion));
	std::string Text = FormatCompactNumericValue(Value, Specifier.c_str());
	if (Text.starts_with('+'))
	{
		Text.erase(0, 1);
	}

	return Text;
}

template <typename DrawWidget>
bool DrawNumericField(const char* const Label, float* const Value, const float Minimum, const float Maximum, const char* const Format, const bool bAlwaysClamp, const bool bClickToEdit, FNumericEditLifecycle* const Edit, const FNumericTextParser& ParseText, DrawWidget&& DrawWidgetFunction)
{
	if (Edit != nullptr)
	{
		Edit->bStarted = false;
		Edit->bFinished = false;
		Edit->bCanceled = false;
	}

	const ImGuiID Id = ImGui::GetID(Label);
	FNumericEditState& State = GetEditState();
	if (State.Id != 0 && ImGui::GetFrameCount() > State.LastFrame + 1)
	{
		State = {};
	}

	float Before = *Value;
	const auto BeginGesture = [&]
	{
		if (State.bGestureStarted)
		{
			return;
		}

		State.bGestureStarted = true;
		if (Edit != nullptr)
		{
			Edit->bStarted = true;
			if (Edit->Begin)
			{
				Edit->Begin();
			}
		}

		State.InitialValue = *Value;
	};

	const auto FinishGesture = [&](const bool bCanceled)
	{
		if (Edit != nullptr && State.bGestureStarted)
		{
			Edit->bFinished = !bCanceled;
			Edit->bCanceled = bCanceled;
		}

		State = {};
	};

	const auto FlushGesture = [&](const bool bCanceled)
	{
		if (State.bGestureStarted && Edit != nullptr && Edit->Flush)
		{
			State = {};
			Edit->Flush(bCanceled);
			Before = *Value;
		}
		else
		{
			FinishGesture(bCanceled);
		}
	};

	const auto ApplyValue = [&](const float Candidate)
	{
		*Value = bAlwaysClamp && Minimum < Maximum ? std::clamp(Candidate, Minimum, Maximum) : Candidate;
		if (*Value != Before)
		{
			ImGui::MarkItemEdited(Id);
		}

		return *Value != Before;
	};

	const std::optional<std::string> EditableText = FormatEditableValue(Before, Format, static_cast<bool>(ParseText));
	const auto BeginTextEditing = [&](const bool bMouseActivation)
	{
		if (State.Id != 0 && State.Id != Id)
		{
			FlushGesture(State.bTextEditing);
		}

		*Value = Before;
		ImGui::ClearActiveID();
		const bool bGestureStarted = State.Id == Id && State.bGestureStarted;
		const float InitialValue = bGestureStarted ? State.InitialValue : Before;
		State = {};
		State.Id = Id;
		State.InitialValue = InitialValue;
		State.bGestureStarted = bGestureStarted;
		State.bTextEditing = true;
		State.LastFrame = ImGui::GetFrameCount();
		State.InitialText = *EditableText;
		std::ranges::copy(State.InitialText, State.Text.begin());
		State.bFocusRequested = true;
		State.bMouseActivation = bMouseActivation;
		ImGui::ActivateItemByID(Id);
	};

	const bool bEditing = State.Id == Id && State.bTextEditing;
	bool bChanged = false;
	bool bEntered = false;
	float Candidate = Before;
	if (bEditing)
	{
		State.LastFrame = ImGui::GetFrameCount();
		const bool bMouseFocusRequested = State.bFocusRequested && State.bMouseActivation;
		if (State.bFocusRequested)
		{
			if (bMouseFocusRequested)
			{
				ImGui::SetNavCursorVisible(false);
			}

			State.bFocusRequested = false;
		}

		if (bMouseFocusRequested)
		{
			ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
		}

		bEntered = ImGui::InputText(Label, State.Text.data(), State.Text.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit, UpdateInputState, &State);
		if (bMouseFocusRequested)
		{
			ImGui::PopStyleColor();
			ImGui::SetNavCursorVisible(false);
		}
	}
	else
	{
		const bool bActiveBeforeDraw = GImGui->ActiveId == Id;
		bChanged = DrawWidgetFunction(&Candidate);
		const ImGuiIO& Io = ImGui::GetIO();
		const bool bWithinClick = !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, Io.MouseDragThreshold * 0.5f);

		// A slider jumps to the pressed position; until the press becomes a drag it may still be a click, so the value waits.
		if (bChanged && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && bWithinClick && GImGui->ActiveIdSource == ImGuiInputSource_Mouse)
		{
			bChanged = false;
			Candidate = Before;
		}

		const bool bClickReleased = bClickToEdit && EditableText && bActiveBeforeDraw && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !Io.KeyShift && bWithinClick;
		if (bClickReleased && ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))
		{
			BeginTextEditing(true);
			return false;
		}
	}

	const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
	const ImGuiIO& Io = ImGui::GetIO();
	const bool bShiftCopy = bHovered && Io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
	const bool bShiftPaste = bHovered && Io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	if (bShiftCopy || bShiftPaste)
	{
		*Value = Before;
		ImGui::ClearActiveID();
		FlushGesture(State.bTextEditing);
		if (bShiftCopy)
		{
			ImGui::SetClipboardText(std::format("{:.9g}", Before).c_str());
		}
		else if (const char* const Clipboard = ImGui::GetClipboardText(); Clipboard != nullptr)
		{
			if (const auto Parsed = EvaluateNumericExpression(Clipboard))
			{
				BeginGesture();
				const bool bApplied = ApplyValue(*Parsed);
				FinishGesture(false);
				return bApplied;
			}
		}

		return false;
	}

	if (!bEditing)
	{
		const bool bMouseText = bHovered && ((Io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
		const bool bKeyboardText = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || (GImGui->NavActivateId == Id && (GImGui->NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0));
		if (EditableText && (bMouseText || bKeyboardText))
		{
			BeginTextEditing(bMouseText);
			return false;
		}

		if (ImGui::IsItemActivated())
		{
			if (State.Id != 0 && State.Id != Id)
			{
				FlushGesture(State.bTextEditing);
			}

			State = {};
			State.Id = Id;
			State.InitialValue = Before;
			BeginGesture();
		}

		if (State.Id == Id)
		{
			State.LastFrame = ImGui::GetFrameCount();
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				const float InitialValue = State.InitialValue;
				ImGui::ClearActiveID();
				FinishGesture(true);
				return ApplyValue(InitialValue);
			}
		}

		if (bChanged)
		{
			bChanged = ApplyValue(Candidate);
		}

		if (State.Id == Id && !ImGui::IsItemActive())
		{
			FinishGesture(false);
		}

		return bChanged;
	}

	if (bEntered && State.InitialText == State.Text.data())
	{
		FinishGesture(false);
		return false;
	}

	if (bEntered)
	{
		const std::optional<float> Typed = ParseText ? ParseText(State.Text.data()) : std::nullopt;
		if (const auto Parsed = Typed ? Typed : EvaluateNumericExpression(State.Text.data()))
		{
			BeginGesture();
			const bool bApplied = ApplyValue(*Parsed);
			FinishGesture(false);
			return bApplied;
		}

		State.bInvalid = true;
		State.bFocusRequested = true;
		ImGui::ActivateItemByID(Id);
	}

	if (State.bInvalid)
	{
		ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(217, 64, 64, 220), ImGui::GetStyle().FrameRounding);
		if (bHovered)
		{
			ImGui::SetTooltip("Invalid expression");
		}
	}

	const bool bActive = ImGui::IsItemActive();
	const bool bLostFocus = State.bHadFocus && !bActive && !bEntered;
	State.bHadFocus = bActive || State.bFocusRequested;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) || bLostFocus)
	{
		if (GImGui->ActiveId == Id)
		{
			ImGui::ClearActiveID();
		}

		FinishGesture(true);
	}

	return false;
}
}

std::string FormatCompactNumericValue(const float Value, const char* const Format)
{
	char Buffer[128];
	ImGui::DataTypeFormatString(Buffer, static_cast<int>(sizeof(Buffer)), ImGuiDataType_Float, &Value, Format);
	std::string Text = Buffer;
	const std::size_t Decimal = Text.find('.');
	std::size_t NumberEnd = Text.find_first_not_of("-0123456789.");
	if (NumberEnd == std::string::npos)
	{
		NumberEnd = Text.size();
	}

	if (Decimal < NumberEnd)
	{
		std::size_t TrimmedEnd = NumberEnd;
		while (TrimmedEnd > Decimal + 1 && Text[TrimmedEnd - 1] == '0')
		{
			--TrimmedEnd;
		}

		if (TrimmedEnd == Decimal + 1)
		{
			--TrimmedEnd;
		}

		Text.erase(TrimmedEnd, NumberEnd - TrimmedEnd);
		NumberEnd = TrimmedEnd;
	}

	if (std::string_view(Text).substr(0, NumberEnd) == "-0")
	{
		Text.erase(0, 1);
	}

	return Text;
}

bool DrawNumericDragFloat(const char* const Label, float* const Value, const float Speed, const float Minimum, const float Maximum, const char* const Format, const ImGuiSliderFlags Flags, FNumericEditLifecycle* const Edit, const bool bCompactDisplay)
{
	return DrawNumericField(Label, Value, Minimum, Maximum, Format, (Flags & ImGuiSliderFlags_AlwaysClamp) != 0, true, Edit, {}, [&](float* const Candidate)
	{
		if (bCompactDisplay)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, {0.f, 0.f, 0.f, 0.f});
		}

		const bool bChanged = ImGui::DragFloat(Label, Candidate, Speed, Minimum, Maximum, Format, Flags | ImGuiSliderFlags_NoInput);
		if (bCompactDisplay)
		{
			ImGui::PopStyleColor();
			const std::string Text = FormatCompactNumericValue(*Candidate, Format);
			ImGui::RenderTextClipped(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Text.data(), Text.data() + Text.size(), nullptr, {0.5f, 0.5f});
		}

		return bChanged;
	});
}

bool DrawNumericSliderFloat(const char* const Label, float* const Value, const float Minimum, const float Maximum, const char* const Format, const ImGuiSliderFlags Flags, FNumericEditLifecycle* const Edit, const FNumericTextParser& ParseText)
{
	return DrawNumericField(Label, Value, Minimum, Maximum, Format, true, true, Edit, ParseText, [&](float* const Candidate)
	{
		return ImGui::SliderFloat(Label, Candidate, Minimum, Maximum, Format, Flags | ImGuiSliderFlags_NoInput);
	});
}
}
