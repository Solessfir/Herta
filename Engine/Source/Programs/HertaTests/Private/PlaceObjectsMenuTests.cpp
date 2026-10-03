#include "Herta/ToolUI/ToolUI.h"
#include "PlaceObjectsMenu.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace Herta
{
namespace
{
struct FPlaceObjectsMenuTestContext
{
	FPlaceObjectsMenuTestContext();
	~FPlaceObjectsMenuTestContext();
	FPlaceObjectsMenuTestContext(const FPlaceObjectsMenuTestContext&) = delete;
	FPlaceObjectsMenuTestContext& operator=(const FPlaceObjectsMenuTestContext&) = delete;
	FPlaceObjectsMenuTestContext(FPlaceObjectsMenuTestContext&&) = delete;
	FPlaceObjectsMenuTestContext& operator=(FPlaceObjectsMenuTestContext&&) = delete;

	bool Frame(bool bOpenRequested = false);

	ImGuiContext* Previous = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();
	FPlaceObjectsMenuState State;
	bool bSearchActive = false;
	bool bPopupOpen = false;
};

FPlaceObjectsMenuTestContext::FPlaceObjectsMenuTestContext()
{
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {640.f, 480.f};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.ConfigInputTrickleEventQueue = false;
	IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
}

FPlaceObjectsMenuTestContext::~FPlaceObjectsMenuTestContext()
{
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(Previous);
}

bool FPlaceObjectsMenuTestContext::Frame(const bool bOpenRequested)
{
	ImGui::NewFrame();
	ImGui::Begin("Place objects test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
	if (bOpenRequested)
	{
		OpenPlaceObjectsMenu(State);
	}

	bool bCubeRequested = false;
	if (ImGui::BeginPopup("Add###PlaceObjectsMenu"))
	{
		const bool bAppearing = ImGui::IsWindowAppearing();
		if (bAppearing)
		{
			State.Reset();
		}

		const FPlaceObjectsMenuNavigation Navigation = UpdatePlaceObjectsMenuNavigation(State);
		if (bAppearing || Navigation.bFocusSearch)
		{
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
		ImGui::InputTextWithHint("##PlaceObjectsSearch", "Search objects", State.Search.data(), State.Search.size());
		bSearchActive = ImGui::IsItemActive();
		if (bSearchActive)
		{
			State.bResultsFocused = false;
		}

		if (State.HasCubeMatch())
		{
			bCubeRequested = ToolUIMenuItem("Cube", EToolUIMenuIcon::Cube, nullptr, "Enter") || Navigation.bConfirm;
			if (Navigation.bFocusResult)
			{
				ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
			}
		}

		ImGui::PopItemFlag();
		if (Navigation.bCancel || bCubeRequested)
		{
			ImGui::ClearActiveID();
			ImGui::CloseCurrentPopup();
		}

		bCubeRequested &= !Navigation.bCancel;
		ImGui::EndPopup();
	}

	bPopupOpen = ImGui::IsPopupOpen("Add###PlaceObjectsMenu");
	ImGui::End();
	ImGui::Render();
	return bCubeRequested;
}

FPlaceObjectsMenuState MakeState(const std::string_view Query)
{
	FPlaceObjectsMenuState State;
	std::ranges::copy(Query, State.Search.begin());
	return State;
}
}

TEST_CASE("Place objects search matches Cube with case-insensitive subsequences")
{
	for (const std::string_view Query : std::array{"", "Cube", "cube", "CUBE", "cb", "ue", "  cB \t", " \t "})
	{
		CAPTURE(Query);
		CHECK(MakeState(Query).HasCubeMatch());
	}

	for (const std::string_view Query : std::array{"sphere", "plane", "camera", "light", "ec", "cubee", "c b"})
	{
		CAPTURE(Query);
		CHECK_FALSE(MakeState(Query).HasCubeMatch());
	}
}

TEST_CASE("Place objects keyboard focus cannot select a filtered-out result")
{
	FPlaceObjectsMenuState State = MakeState("cb");
	State.SetResultFocus(true);
	CHECK(State.bResultsFocused);
	State.SetResultFocus(false);
	CHECK_FALSE(State.bResultsFocused);
	State = MakeState("sphere");
	State.SetResultFocus(true);
	CHECK_FALSE(State.bResultsFocused);
}

TEST_CASE("Opening place objects clears the previous query and result focus")
{
	FPlaceObjectsMenuState State = MakeState("cb");
	State.SetResultFocus(true);
	State.Reset();
	CHECK(State.Search[0] == '\0');
	CHECK_FALSE(State.bResultsFocused);
	CHECK(State.HasCubeMatch());
}

TEST_CASE("Place objects Tab and Shift Tab switch an active search and the result")
{
	FPlaceObjectsMenuTestContext Test;
	Test.Frame(true);
	Test.Frame();
	Test.Frame();
	REQUIRE(Test.bSearchActive);
	ImGuiIO& IO = ImGui::GetIO();
	IO.AddKeyEvent(ImGuiKey_Tab, true);
	Test.Frame();
	CHECK(Test.State.bResultsFocused);
	CHECK_FALSE(Test.bSearchActive);
	IO.AddKeyEvent(ImGuiKey_Tab, false);
	Test.Frame();
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	IO.AddKeyEvent(ImGuiKey_Tab, true);
	Test.Frame();
	IO.AddKeyEvent(ImGuiKey_Tab, false);
	Test.Frame();
	CHECK_FALSE(Test.State.bResultsFocused);
	CHECK(Test.bSearchActive);
}

TEST_CASE("Place objects arrows navigate from fuzzy search and Enter adds the result")
{
	for (const ImGuiKey Arrow : {ImGuiKey_UpArrow, ImGuiKey_DownArrow})
	{
		FPlaceObjectsMenuTestContext Test;
		Test.Frame(true);
		Test.Frame();
		Test.Frame();
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddInputCharactersUTF8("cb");
		Test.Frame();
		REQUIRE(Test.State.HasCubeMatch());
		IO.AddKeyEvent(Arrow, true);
		Test.Frame();
		CHECK(Test.State.bResultsFocused);
		CHECK_FALSE(Test.bSearchActive);
		IO.AddKeyEvent(Arrow, false);
		Test.Frame();
		IO.AddKeyEvent(ImGuiKey_Enter, true);
		CHECK(Test.Frame());
		CHECK_FALSE(Test.bPopupOpen);
	}
}

TEST_CASE("Place objects Enter confirms from search but never adds a filtered-out shape")
{
	for (const std::string_view Query : {"cb", "sphere"})
	{
		FPlaceObjectsMenuTestContext Test;
		Test.Frame(true);
		Test.Frame();
		Test.Frame();
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddInputCharactersUTF8(Query.data());
		Test.Frame();
		IO.AddKeyEvent(ImGuiKey_Enter, true);
		CHECK(Test.Frame() == (Query == "cb"));
		CHECK(Test.bPopupOpen == (Query != "cb"));
	}
}

TEST_CASE("Place objects Escape closes active search without creating a Cube")
{
	FPlaceObjectsMenuTestContext Test;
	Test.Frame(true);
	Test.Frame();
	Test.Frame();
	REQUIRE(Test.bSearchActive);
	ImGui::GetIO().AddInputCharactersUTF8("cb");
	Test.Frame();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	CHECK_FALSE(Test.Frame());
	CHECK_FALSE(Test.bPopupOpen);
	CHECK(GImGui->ActiveId == 0);
}

TEST_CASE("Opening place objects consumes the Shift A chord character but preserves subsequent typing")
{
	FPlaceObjectsMenuTestContext Test;
	ImGuiIO& IO = ImGui::GetIO();
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	IO.AddKeyEvent(ImGuiKey_A, true);
	IO.AddInputCharactersUTF8("A");
	Test.Frame(true);
	CHECK(Test.State.Search[0] == '\0');
	IO.AddKeyEvent(ImGuiMod_Shift, false);
	IO.AddKeyEvent(ImGuiKey_A, false);
	Test.Frame();
	Test.Frame();
	IO.AddInputCharactersUTF8("a");
	Test.Frame();
	CHECK(std::string_view(Test.State.Search.data()) == "a");
}

TEST_CASE("Typing from a place objects result immediately resumes fuzzy search")
{
	FPlaceObjectsMenuTestContext Test;
	Test.Frame(true);
	Test.Frame();
	Test.Frame();
	ImGuiIO& IO = ImGui::GetIO();
	IO.AddKeyEvent(ImGuiKey_DownArrow, true);
	Test.Frame();
	REQUIRE(Test.State.bResultsFocused);
	IO.AddKeyEvent(ImGuiKey_DownArrow, false);
	Test.Frame();
	IO.AddInputCharactersUTF8("cb");
	Test.Frame();
	CHECK_FALSE(Test.State.bResultsFocused);
	CHECK(Test.bSearchActive);
	CHECK(std::string_view(Test.State.Search.data()) == "cb");
	CHECK(Test.State.HasCubeMatch());
}

TEST_CASE("Reopening place objects anchors to the pointer instead of stale keyboard focus")
{
	FPlaceObjectsMenuTestContext Test;
	ImGuiIO& IO = ImGui::GetIO();
	IO.AddMousePosEvent(180.f, 120.f);
	Test.Frame(true);
	Test.Frame();
	Test.Frame();
	IO.AddKeyEvent(ImGuiKey_DownArrow, true);
	Test.Frame();
	REQUIRE(Test.State.bResultsFocused);
	IO.AddKeyEvent(ImGuiKey_DownArrow, false);
	Test.Frame();
	IO.AddKeyEvent(ImGuiKey_Enter, true);
	REQUIRE(Test.Frame());
	IO.AddKeyEvent(ImGuiKey_Enter, false);
	Test.Frame();

	// Keyboard-only editing can restore navigation without moving the pointer.
	ImGui::SetNavCursorVisible(true);
	GImGui->NavHighlightItemUnderNav = true;
	Test.Frame(true);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	CHECK(GImGui->OpenPopupStack[0].OpenPopupPos.x == doctest::Approx(IO.MousePos.x + 1.f));
	CHECK(GImGui->OpenPopupStack[0].OpenPopupPos.y == doctest::Approx(IO.MousePos.y));
	Test.Frame();
	Test.Frame();
	CHECK(Test.bSearchActive);

	IO.AddKeyEvent(ImGuiKey_Escape, true);
	Test.Frame();
	IO.AddKeyEvent(ImGuiKey_Escape, false);
	IO.AddMousePosEvent(400.f, 160.f);
	Test.Frame();
	ImGui::SetNavCursorVisible(true);
	GImGui->NavHighlightItemUnderNav = true;
	Test.Frame(true);
	REQUIRE(GImGui->OpenPopupStack.Size == 1);
	CHECK(GImGui->OpenPopupStack[0].OpenPopupPos.x == doctest::Approx(401.f));
	CHECK(GImGui->OpenPopupStack[0].OpenPopupPos.y == doctest::Approx(160.f));
}
}
