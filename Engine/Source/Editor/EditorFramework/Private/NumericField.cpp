#include "NumericField.h"

#include "Herta/EditorCore/TransformText.h"

#include <imgui_internal.h>

#include <algorithm>
#include <format>
#include <string>
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
	int LastFrame = -1;
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

template <typename DrawWidget>
bool DrawNumericField(const char* const Label, float* const Value, const float Minimum, const float Maximum, const bool bAlwaysClamp, const bool bClickToEdit, DrawWidget&& DrawWidgetFunction)
{
	const ImGuiID Id = ImGui::GetID(Label);
	FNumericEditState& State = GetEditState();
	if (State.Id != 0 && ImGui::GetFrameCount() > State.LastFrame + 1)
	{
		State = {};
	}

	const float Before = *Value;
	const auto ApplyValue = [&](const float Candidate)
	{
		*Value = bAlwaysClamp && Minimum < Maximum ? std::clamp(Candidate, Minimum, Maximum) : Candidate;
		if (*Value != Before)
		{
			ImGui::MarkItemEdited(Id);
		}

		return *Value != Before;
	};

	const auto BeginTextEditing = [&](const bool bMouseActivation)
	{
		*Value = Before;
		ImGui::ClearActiveID();
		State = {};
		State.Id = Id;
		State.LastFrame = ImGui::GetFrameCount();
		const std::string Text = std::format("{:.9g}", Before);
		std::ranges::copy(Text, State.Text.begin());
		State.bFocusRequested = true;
		State.bMouseActivation = bMouseActivation;
		ImGui::ActivateItemByID(Id);
	};

	const bool bEditing = State.Id == Id;
	bool bChanged = false;
	bool bEntered = false;
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
		bChanged = DrawWidgetFunction();
		const ImGuiIO& Io = ImGui::GetIO();
		const bool bClickReleased = bClickToEdit && bActiveBeforeDraw && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !Io.KeyShift && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, Io.MouseDragThreshold * 0.5f);
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
		State = {};
		if (bShiftCopy)
		{
			ImGui::SetClipboardText(std::format("{:.9g}", Before).c_str());
		}
		else if (const char* const Clipboard = ImGui::GetClipboardText(); Clipboard != nullptr)
		{
			if (const auto Parsed = EvaluateNumericExpression(Clipboard))
			{
				return ApplyValue(*Parsed);
			}
		}

		return false;
	}

	if (!bEditing)
	{
		const bool bMouseText = bHovered && ((Io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
		const bool bKeyboardText = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || (GImGui->NavActivateId == Id && (GImGui->NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0));
		if (bMouseText || bKeyboardText)
		{
			BeginTextEditing(bMouseText);
			return false;
		}

		return bChanged;
	}

	if (bEntered)
	{
		if (const auto Parsed = EvaluateNumericExpression(State.Text.data()))
		{
			State = {};
			return ApplyValue(*Parsed);
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
		State = {};
	}

	return false;
}
}

bool DrawNumericDragFloat(const char* const Label, float* const Value, const float Speed, const float Minimum, const float Maximum, const char* const Format, const ImGuiSliderFlags Flags)
{
	return DrawNumericField(Label, Value, Minimum, Maximum, (Flags & ImGuiSliderFlags_AlwaysClamp) != 0, true, [&]
	{
		return ImGui::DragFloat(Label, Value, Speed, Minimum, Maximum, Format, Flags | ImGuiSliderFlags_NoInput);
	});
}

bool DrawNumericSliderFloat(const char* const Label, float* const Value, const float Minimum, const float Maximum, const char* const Format, const ImGuiSliderFlags Flags)
{
	return DrawNumericField(Label, Value, Minimum, Maximum, true, true, [&]
	{
		return ImGui::SliderFloat(Label, Value, Minimum, Maximum, Format, Flags | ImGuiSliderFlags_NoInput);
	});
}
}
