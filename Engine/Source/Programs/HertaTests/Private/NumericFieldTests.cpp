#include "NumericField.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <initializer_list>
#include <string>

namespace Herta
{
namespace
{
struct FNumericFieldTestContext
{
	ImGuiContext* PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();
	std::string Clipboard;
	ImVec2 FieldCenter{};
	float Minimum = 0.0f;
	float Maximum = 100.0f;
	ImGuiSliderFlags Flags = ImGuiSliderFlags_AlwaysClamp;
	bool bTextEditing = false;
	bool bRequestFocus = false;

	FNumericFieldTestContext()
	{
		ImGui::SetCurrentContext(Context);
		ImGuiIO& Io = ImGui::GetIO();
		Io.DisplaySize = {400.0f, 200.0f};
		Io.DeltaTime = 1.0f / 60.0f;
		Io.IniFilename = nullptr;
		Io.ConfigInputTrickleEventQueue = false;
		Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		Io.Fonts->AddFontDefault();
		unsigned char* Pixels = nullptr;
		int Width = 0;
		int Height = 0;
		Io.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
		ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
		Platform.Platform_ClipboardUserData = this;
		Platform.Platform_GetClipboardTextFn = [](ImGuiContext*)
		{
			return static_cast<FNumericFieldTestContext*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->Clipboard.c_str();
		};
		Platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* const Text)
		{
			static_cast<FNumericFieldTestContext*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->Clipboard = Text;
		};
	}

	~FNumericFieldTestContext()
	{
		ImGui::DestroyContext(Context);
		ImGui::SetCurrentContext(PreviousContext);
	}

	bool Frame(float& Value, const bool bDisabled = false)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({400.0f, 200.0f}, ImGuiCond_Always);
		ImGui::Begin("Numeric field test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
		ImGui::SetCursorScreenPos({40.0f, 40.0f});
		ImGui::SetNextItemWidth(160.0f);
		if (bRequestFocus)
		{
			ImGui::SetKeyboardFocusHere();
			bRequestFocus = false;
		}
		if (bDisabled)
		{
			ImGui::BeginDisabled();
		}
		const bool bChanged = DrawNumericDragFloat("##Value", &Value, 0.1f, Minimum, Maximum, "%.3f", Flags);
		bTextEditing = ImGui::IsItemActive() && GImGui->InputTextState.ID == ImGui::GetItemID();
		if (bDisabled)
		{
			ImGui::EndDisabled();
		}
		const ImVec2 FieldMinimum = ImGui::GetItemRectMin();
		const ImVec2 FieldMaximum = ImGui::GetItemRectMax();
		FieldCenter = {(FieldMinimum.x + FieldMaximum.x) * 0.5f, (FieldMinimum.y + FieldMaximum.y) * 0.5f};
		ImGui::End();
		ImGui::Render();
		return bChanged;
	}

	void MoveToField() const
	{
		ImGui::GetIO().AddMousePosEvent(FieldCenter.x, FieldCenter.y);
	}
};

void BeginExpression(FNumericFieldTestContext& Test, float& Value)
{
	Test.Frame(Value);
	Test.MoveToField();
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddKeyEvent(ImGuiMod_Ctrl, true);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Io.AddKeyEvent(ImGuiMod_Ctrl, false);
	Test.Frame(Value);
}
}

TEST_CASE("Numeric field evaluates expression entry without changing the drag value first")
{
	FNumericFieldTestContext Test;
	float Value = 2.0f;
	BeginExpression(Test, Value);
	CHECK(Value == doctest::Approx(2.0f));
	ImGui::GetIO().AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.0f));
}

TEST_CASE("Numeric field opens text entry on single click release without a mouse nav outline")
{
	FNumericFieldTestContext Test;
	float Value = 2.0f;
	const ImU32 NavColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]);
	Test.Frame(Value);
	Test.MoveToField();
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	CHECK_FALSE(Test.bTextEditing);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	CHECK(Test.bTextEditing);
	CHECK_FALSE(GImGui->NavCursorVisible);
	CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_NavCursor]) == NavColor);
	Io.AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	Io.AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.0f));
}

TEST_CASE("Unbounded location-style numeric field drags in either direction without entering text")
{
	for (const float Delta : {30.0f, -30.0f})
	{
		FNumericFieldTestContext Test;
		Test.Minimum = 0.0f;
		Test.Maximum = 0.0f;
		Test.Flags = 0;
		float Value = 0.0f;
		Test.Frame(Value);
		Test.MoveToField();
		Test.Frame(Value);
		Test.Frame(Value);
		ImGuiIO& Io = ImGui::GetIO();
		Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		Test.Frame(Value);
		Test.Frame(Value);
		Io.AddMousePosEvent(Test.FieldCenter.x + Delta, Test.FieldCenter.y);
		CHECK(Test.Frame(Value));
		CHECK(Value * Delta > 0.0f);
		Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		Test.Frame(Value);
		Test.Frame(Value);
		CHECK_FALSE(Test.bTextEditing);
	}
}

TEST_CASE("Numeric field keeps invalid expression and restores its temporary style")
{
	FNumericFieldTestContext Test;
	float Value = 2.0f;
	const ImU32 FrameColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
	const ImU32 BorderColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Border]);
	BeginExpression(Test, Value);
	ImGui::GetIO().AddInputCharactersUTF8("1/0");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.0f));
	CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]) == FrameColor);
	CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Border]) == BorderColor);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.0f));
	ImGui::GetIO().AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.0f));
}

TEST_CASE("Numeric field does not evaluate a truncated oversized expression")
{
	FNumericFieldTestContext Test;
	float Value = 2.0f;
	BeginExpression(Test, Value);
	ImGui::GetIO().AddInputCharactersUTF8(("5" + std::string(255, ' ') + "+1").c_str());
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.0f));
}

TEST_CASE("Numeric field supports keyboard expression activation")
{
	FNumericFieldTestContext Test;
	float Value = 2.0f;
	Test.bRequestFocus = true;
	Test.Frame(Value);
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	Test.Frame(Value);
	ImGui::GetIO().AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.0f));
}

TEST_CASE("Numeric field copies and pastes expressions, but ignores paste when disabled")
{
	FNumericFieldTestContext Test;
	float Value = 2.5f;
	Test.Frame(Value);
	Test.MoveToField();
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddKeyEvent(ImGuiMod_Shift, true);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Test.Clipboard == "2.5");
	Io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
	Test.Frame(Value);
	Test.Clipboard = "10/2";
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.0f));
	Io.AddKeyEvent(ImGuiMod_Shift, false);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	CHECK_FALSE(Test.bTextEditing);
	Test.Clipboard = "7";
	Io.AddKeyEvent(ImGuiMod_Shift, true);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	CHECK_FALSE(Test.Frame(Value, true));
	CHECK(Value == doctest::Approx(5.0f));
}
}
