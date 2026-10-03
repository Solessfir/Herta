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
	float Minimum = 0.f;
	float Maximum = 100.f;
	ImGuiSliderFlags Flags = ImGuiSliderFlags_AlwaysClamp;
	FNumericEditLifecycle Edit;
	int Begins = 0;
	int Finishes = 0;
	int Cancels = 0;
	int Flushes = 0;
	float ValueAtBegin = 0.f;
	float SecondValue = 7.f;
	ImVec2 SecondFieldCenter{};
	bool bDrawSecondFirst = false;
	bool bLastFlushCanceled = false;
	bool bTextEditing = false;
	bool bRequestFocus = false;
	bool bSlider = false;

	FNumericFieldTestContext()
	{
		ImGui::SetCurrentContext(Context);
		ImGuiIO& Io = ImGui::GetIO();
		Io.DisplaySize = {400.f, 200.f};
		Io.DeltaTime = 1.f / 60.f;
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

	FNumericFieldTestContext(const FNumericFieldTestContext&) = delete;
	FNumericFieldTestContext& operator=(const FNumericFieldTestContext&) = delete;
	FNumericFieldTestContext(FNumericFieldTestContext&&) = delete;
	FNumericFieldTestContext& operator=(FNumericFieldTestContext&&) = delete;

	bool Frame(float& Value, const bool bDisabled = false)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({400.f, 200.f}, ImGuiCond_Always);
		ImGui::Begin("Numeric field test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
		if (bDrawSecondFirst)
		{
			ImGui::SetCursorScreenPos({40.f, 90.f});
			ImGui::SetNextItemWidth(160.f);
			FNumericEditLifecycle SecondEdit{
			    .Begin = [&]
			{
				++Begins;
				ValueAtBegin = SecondValue;
			},
			    .Flush = [&](const bool bCanceled)
			{
				++Flushes;
				bLastFlushCanceled = bCanceled;
			},
			};
			DrawNumericDragFloat("##SecondValue", &SecondValue, 0.1f, Minimum, Maximum, "%.3f", Flags, &SecondEdit);
			Finishes += SecondEdit.bFinished ? 1 : 0;
			Cancels += SecondEdit.bCanceled ? 1 : 0;
			const ImVec2 SecondMinimum = ImGui::GetItemRectMin();
			const ImVec2 SecondMaximum = ImGui::GetItemRectMax();
			SecondFieldCenter = {(SecondMinimum.x + SecondMaximum.x) * 0.5f, (SecondMinimum.y + SecondMaximum.y) * 0.5f};
		}

		ImGui::SetCursorScreenPos({40.f, 40.f});
		ImGui::SetNextItemWidth(160.f);
		if (bRequestFocus)
		{
			ImGui::SetKeyboardFocusHere();
			bRequestFocus = false;
		}

		if (bDisabled)
		{
			ImGui::BeginDisabled();
		}

		Edit.Begin = [&]
		{
			++Begins;
			ValueAtBegin = Value;
		};

		const bool bChanged = bSlider ? DrawNumericSliderFloat("##Value", &Value, Minimum, Maximum, "%.3f", Flags, &Edit) : DrawNumericDragFloat("##Value", &Value, 0.1f, Minimum, Maximum, "%.3f", Flags, &Edit);
		Finishes += Edit.bFinished ? 1 : 0;
		Cancels += Edit.bCanceled ? 1 : 0;
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
	float Value = 2.f;
	BeginExpression(Test, Value);
	CHECK(Value == doctest::Approx(2.f));
	ImGui::GetIO().AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.f));
}

TEST_CASE("Numeric field opens text entry on single click release without a mouse nav outline")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
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
	CHECK(Value == doctest::Approx(5.f));
}

TEST_CASE("Numeric slider opens expression entry on single click release")
{
	FNumericFieldTestContext Test;
	Test.bSlider = true;
	float Value = 50.f;
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
	Io.AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	Io.AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.f));
}

TEST_CASE("Numeric slider still drags without entering text")
{
	FNumericFieldTestContext Test;
	Test.bSlider = true;
	float Value = 50.f;
	Test.Frame(Value);
	Test.MoveToField();
	Test.Frame(Value);
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	Io.AddMousePosEvent(Test.FieldCenter.x + 30.f, Test.FieldCenter.y);
	CHECK(Test.Frame(Value));
	CHECK(Value > 50.f);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	CHECK_FALSE(Test.bTextEditing);
}

TEST_CASE("Unbounded location-style numeric field drags in either direction without entering text")
{
	for (const float Delta : {30.f, -30.f})
	{
		FNumericFieldTestContext Test;
		Test.Minimum = 0.f;
		Test.Maximum = 0.f;
		Test.Flags = 0;
		float Value = 0.f;
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
		CHECK(Value * Delta > 0.f);
		Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		Test.Frame(Value);
		Test.Frame(Value);
		CHECK_FALSE(Test.bTextEditing);
	}
}

TEST_CASE("Numeric field keeps invalid expression and restores its temporary style")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	const ImU32 FrameColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
	const ImU32 BorderColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Border]);
	BeginExpression(Test, Value);
	ImGui::GetIO().AddInputCharactersUTF8("1/0");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.f));
	CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]) == FrameColor);
	CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Border]) == BorderColor);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.f));
	ImGui::GetIO().AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.f));
}

TEST_CASE("Numeric field does not evaluate a truncated oversized expression")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	BeginExpression(Test, Value);
	ImGui::GetIO().AddInputCharactersUTF8(("5" + std::string(255, ' ') + "+1").c_str());
	Test.Frame(Value);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	CHECK_FALSE(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.f));
}

TEST_CASE("Numeric field supports keyboard expression activation")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
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
	CHECK(Value == doctest::Approx(5.f));
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
	CHECK(Value == doctest::Approx(5.f));
	Io.AddKeyEvent(ImGuiMod_Shift, false);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	CHECK_FALSE(Test.bTextEditing);
	Test.Clipboard = "7";
	Io.AddKeyEvent(ImGuiMod_Shift, true);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	CHECK_FALSE(Test.Frame(Value, true));
	CHECK(Value == doctest::Approx(5.f));
}

TEST_CASE("Numeric drag reports one gesture and begins before changing its value")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	Test.Frame(Value);
	Test.MoveToField();
	Test.Frame(Value);
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	CHECK(Test.Begins == 1);
	CHECK(Test.ValueAtBegin == doctest::Approx(2.f));
	Io.AddMousePosEvent(Test.FieldCenter.x + 30.f, Test.FieldCenter.y);
	CHECK(Test.Frame(Value));
	CHECK(Value > 2.f);
	CHECK(Test.Begins == 1);
	CHECK(Test.Finishes == 0);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	CHECK(Test.Finishes == 1);
	Test.Frame(Value);
	CHECK(Test.Finishes == 1);
	CHECK(Test.Cancels == 0);
}

TEST_CASE("Escape cancels a numeric drag and restores its initial value")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	Test.Frame(Value);
	Test.MoveToField();
	Test.Frame(Value);
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	Io.AddMousePosEvent(Test.FieldCenter.x + 30.f, Test.FieldCenter.y);
	CHECK(Test.Frame(Value));
	CHECK(Value > 2.f);
	Io.AddKeyEvent(ImGuiKey_Escape, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(2.f));
	CHECK(Test.Begins == 1);
	CHECK(Test.Cancels == 1);
	CHECK(Test.Finishes == 0);
	Io.AddKeyEvent(ImGuiKey_Escape, false);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	CHECK(Test.Cancels == 1);
}

TEST_CASE("Click-to-text keeps the numeric gesture open until expression commit")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	Test.Frame(Value);
	Test.MoveToField();
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	CHECK(Test.bTextEditing);
	CHECK(Test.Begins == 1);
	CHECK(Test.Finishes == 0);
	Io.AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	Io.AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Value == doctest::Approx(5.f));
	CHECK(Test.Begins == 1);
	CHECK(Test.Finishes == 1);
}

TEST_CASE("Numeric expression starts its transaction only on valid commit")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	BeginExpression(Test, Value);
	CHECK(Test.Begins == 0);
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddInputCharactersUTF8("1/0");
	Test.Frame(Value);
	Io.AddKeyEvent(ImGuiKey_Enter, true);
	CHECK_FALSE(Test.Frame(Value));
	Io.AddKeyEvent(ImGuiKey_Enter, false);
	Test.Frame(Value);
	CHECK(Test.bTextEditing);
	CHECK(Test.Begins == 0);
	CHECK(Test.Finishes == 0);
	Io.AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	Io.AddKeyEvent(ImGuiKey_Enter, true);
	CHECK(Test.Frame(Value));
	CHECK(Test.ValueAtBegin == doctest::Approx(2.f));
	CHECK(Test.Begins == 1);
	CHECK(Test.Finishes == 1);
}

TEST_CASE("Canceled and unfocused numeric expressions do not mutate the value")
{
	for (const bool bEscape : {false, true})
	{
		FNumericFieldTestContext Test;
		float Value = 2.f;
		BeginExpression(Test, Value);
		ImGuiIO& Io = ImGui::GetIO();
		Io.AddInputCharactersUTF8("10/2");
		Test.Frame(Value);

		if (bEscape)
		{
			Io.AddKeyEvent(ImGuiKey_Escape, true);
		}
		else
		{
			Io.AddMousePosEvent(300.f, 150.f);
			Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		}

		CHECK_FALSE(Test.Frame(Value));
		CHECK(Value == doctest::Approx(2.f));
		CHECK(Test.Begins == 0);
		CHECK(Test.Finishes == 0);
	}
}

TEST_CASE("Numeric paste reports a discrete transaction before changing its value")
{
	FNumericFieldTestContext Test;
	float Value = 2.f;
	Test.Frame(Value);
	Test.MoveToField();
	Test.Clipboard = "10/2";
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddKeyEvent(ImGuiMod_Shift, true);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	CHECK(Test.Frame(Value));
	CHECK(Test.ValueAtBegin == doctest::Approx(2.f));
	CHECK(Value == doctest::Approx(5.f));
	CHECK(Test.Begins == 1);
	CHECK(Test.Finishes == 1);
}

TEST_CASE("Numeric field flushes an earlier text gesture before a reverse-order field activates")
{
	FNumericFieldTestContext Test;
	Test.bDrawSecondFirst = true;
	float Value = 2.f;
	Test.Frame(Value);
	Test.MoveToField();
	ImGuiIO& Io = ImGui::GetIO();
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	Test.Frame(Value);
	REQUIRE(Test.bTextEditing);
	REQUIRE(Test.Begins == 1);
	Io.AddInputCharactersUTF8("10/2");
	Test.Frame(Value);
	Io.AddMousePosEvent(Test.SecondFieldCenter.x, Test.SecondFieldCenter.y);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	Test.Frame(Value);
	CHECK(Test.Flushes == 1);
	CHECK(Test.bLastFlushCanceled);
	CHECK(Test.Begins == 2);
	CHECK(Test.Finishes == 0);
	CHECK(Test.Cancels == 0);
	CHECK(Value == doctest::Approx(2.f));
	Io.AddMousePosEvent(Test.SecondFieldCenter.x + 30.f, Test.SecondFieldCenter.y);
	Test.Frame(Value);
	CHECK(Test.SecondValue > 7.f);
	Io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	Test.Frame(Value);
	CHECK(Test.Finishes == 1);
}
}
