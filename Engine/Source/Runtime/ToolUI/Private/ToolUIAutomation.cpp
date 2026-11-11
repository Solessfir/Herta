// ImGui's test-engine hooks, which record every item a frame lays out so automation can find and click it by label. ImGui only calls them
// while ImGuiContext::TestEngineHookItems is set, which FToolUIContext::SetItemRecording turns on.
#include "ToolUIAutomation.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cctype>
#include <ranges>

namespace Herta
{
namespace
{
FToolUIItemRecorder* Recorder = nullptr;

struct FNamedKey
{
	std::string_view Name;
	ImGuiKey Key = ImGuiKey_None;
};

constexpr std::array NamedKeys{
    FNamedKey{"Enter", ImGuiKey_Enter},
    FNamedKey{"Escape", ImGuiKey_Escape},
    FNamedKey{"Tab", ImGuiKey_Tab},
    FNamedKey{"Backspace", ImGuiKey_Backspace},
    FNamedKey{"Delete", ImGuiKey_Delete},
    FNamedKey{"Space", ImGuiKey_Space},
    FNamedKey{"Up", ImGuiKey_UpArrow},
    FNamedKey{"Down", ImGuiKey_DownArrow},
    FNamedKey{"Left", ImGuiKey_LeftArrow},
    FNamedKey{"Right", ImGuiKey_RightArrow},
    FNamedKey{"Home", ImGuiKey_Home},
    FNamedKey{"End", ImGuiKey_End},
    FNamedKey{"PageUp", ImGuiKey_PageUp},
    FNamedKey{"PageDown", ImGuiKey_PageDown},
    FNamedKey{"F1", ImGuiKey_F1},
    FNamedKey{"F2", ImGuiKey_F2},
    FNamedKey{"F5", ImGuiKey_F5},
    FNamedKey{"F11", ImGuiKey_F11},
};

[[nodiscard]] bool EqualsIgnoringCase(const std::string_view Left, const std::string_view Right)
{
	return Left.size() == Right.size() && std::ranges::equal(Left, Right, [](const unsigned char A, const unsigned char B)
	{
		return std::tolower(A) == std::tolower(B);
	});
}

[[nodiscard]] ImGuiKey FindKey(const std::string_view Name)
{
	if (Name.size() == 1)
	{
		const auto Character = static_cast<char>(std::toupper(static_cast<unsigned char>(Name.front())));
		if (Character >= 'A' && Character <= 'Z')
		{
			return static_cast<ImGuiKey>(ImGuiKey_A + (Character - 'A'));
		}

		if (Character >= '0' && Character <= '9')
		{
			return static_cast<ImGuiKey>(ImGuiKey_0 + (Character - '0'));
		}
	}

	const auto Found = std::ranges::find_if(NamedKeys, [Name](const FNamedKey& Key)
	{
		return EqualsIgnoringCase(Key.Name, Name);
	});

	return Found == NamedKeys.end() ? ImGuiKey_None : Found->Key;
}
}

void FToolUIItemRecorder::BeginFrame(ImGuiContext& Context, const bool bEnabled)
{
	Recorder = bEnabled ? this : nullptr;
	Context.TestEngineHookItems = bEnabled;
	Items = std::move(Pending);
	Pending.clear();
	Indices.clear();
}

void FToolUIItemRecorder::Add(const std::uint32_t Id, const FToolUICanvasBounds& Bounds, const std::string_view Window, const bool bDisabled)
{
	const auto [Found, bAdded] = Indices.try_emplace(Id, Pending.size());
	if (bAdded)
	{
		Pending.push_back({.Id = Id, .Window = std::string(Window)});
	}

	FToolUIItem& Item = Pending[Found->second];
	Item.Bounds = Bounds;
	Item.bDisabled = bDisabled;
}

void FToolUIItemRecorder::Describe(const std::uint32_t Id, const std::string_view Label, const bool bCheckable, const bool bChecked, const bool bOpenable, const bool bOpened, const bool bInput)
{
	const auto Found = Indices.find(Id);
	if (Found == Indices.end())
	{
		return;
	}

	FToolUIItem& Item = Pending[Found->second];
	// Labels end in "##id" or "###id" when they need a unique ID; automation matches what the user reads. Widgets that draw their own text,
	// such as menu items with icons, hide all of it behind "##", so that text is the label.
	const std::string_view Visible = Label.substr(0, Label.find("##"));
	const std::size_t Hidden = Label.find_first_not_of('#');
	Item.Label = std::string(Visible.empty() && Label.starts_with("##") && Hidden != std::string_view::npos ? Label.substr(Hidden) : Visible);
	Item.bCheckable = bCheckable;
	Item.bChecked = bChecked;
	Item.bOpenable = bOpenable;
	Item.bOpened = bOpened;
	Item.bInput = bInput;
}

void QueueToolUIClick(const float X, const float Y, const int Button, const bool bDouble)
{
	ImGuiIO& Input = ImGui::GetIO();
	Input.AddMousePosEvent(X, Y);
	for (int Press = 0; Press < (bDouble ? 2 : 1); ++Press)
	{
		Input.AddMouseButtonEvent(Button, true);
		Input.AddMouseButtonEvent(Button, false);
	}
}

void QueueToolUIText(const std::string_view Text)
{
	ImGui::GetIO().AddInputCharactersUTF8(std::string(Text).c_str());
}

bool QueueToolUIKey(const std::string_view Chord)
{
	// "Ctrl+Shift+S" holds the modifiers around one key press.
	std::vector<ImGuiKey> Modifiers;
	ImGuiKey Key = ImGuiKey_None;
	for (const auto Part : std::views::split(Chord, '+'))
	{
		const std::string_view Name(Part.begin(), Part.end());
		if (EqualsIgnoringCase(Name, "Ctrl"))
		{
			Modifiers.push_back(ImGuiMod_Ctrl);
		}
		else if (EqualsIgnoringCase(Name, "Shift"))
		{
			Modifiers.push_back(ImGuiMod_Shift);
		}
		else if (EqualsIgnoringCase(Name, "Alt"))
		{
			Modifiers.push_back(ImGuiMod_Alt);
		}
		else if (Key != ImGuiKey_None || (Key = FindKey(Name)) == ImGuiKey_None)
		{
			return false;
		}
	}

	if (Key == ImGuiKey_None)
	{
		return false;
	}

	ImGuiIO& Input = ImGui::GetIO();
	for (const ImGuiKey Modifier : Modifiers)
	{
		Input.AddKeyEvent(Modifier, true);
	}

	Input.AddKeyEvent(Key, true);
	Input.AddKeyEvent(Key, false);
	for (const ImGuiKey Modifier : Modifiers)
	{
		Input.AddKeyEvent(Modifier, false);
	}

	return true;
}
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* const Context, const ImGuiID Id, const ImRect& Bounds, const ImGuiLastItemData* const Data)
{
	if (Herta::Recorder != nullptr && Id != 0)
	{
		const char* const Window = Context->CurrentWindow != nullptr ? Context->CurrentWindow->Name : "";
		const bool bDisabled = Data != nullptr && (Data->ItemFlags & ImGuiItemFlags_Disabled) != 0;
		Herta::Recorder->Add(Id, {.X = Bounds.Min.x, .Y = Bounds.Min.y, .Width = Bounds.GetWidth(), .Height = Bounds.GetHeight()}, Window, bDisabled);
	}
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, const ImGuiID Id, const char* const Label, const ImGuiItemStatusFlags Flags)
{
	if (Herta::Recorder != nullptr && Label != nullptr)
	{
		Herta::Recorder->Describe(Id, Label, (Flags & ImGuiItemStatusFlags_Checkable) != 0, (Flags & ImGuiItemStatusFlags_Checked) != 0, (Flags & ImGuiItemStatusFlags_Openable) != 0, (Flags & ImGuiItemStatusFlags_Opened) != 0, (Flags & ImGuiItemStatusFlags_Inputable) != 0);
	}
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...)
{
}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID)
{
	return nullptr;
}
