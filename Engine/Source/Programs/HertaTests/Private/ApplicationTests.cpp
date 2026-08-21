#include "Herta/Application/Application.h"
#include "Herta/Application/WindowPlacement.h"

#include <doctest/doctest.h>
#include <utility>

namespace Herta
{
TEST_CASE("Title bar hit testing routes native regions")
{
	const FTitleBarLayout Layout = MakeTitleBarLayout(1280, 720, 1.0f, true, false);

	CHECK(HitTestTitleBar(Layout, 1270, 10) == ETitleBarHitRegion::CloseButton);
	CHECK(HitTestTitleBar(Layout, 1220, 10) == ETitleBarHitRegion::MaximizeButton);
	CHECK(HitTestTitleBar(Layout, 1170, 10) == ETitleBarHitRegion::MinimizeButton);
	CHECK(HitTestTitleBar(Layout, 10, 10) == ETitleBarHitRegion::SystemMenu);
	CHECK(HitTestTitleBar(Layout, 50, 10) == ETitleBarHitRegion::ApplicationMenu);
	CHECK(HitTestTitleBar(Layout, 300, 10) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(Layout, 300, 100) == ETitleBarHitRegion::Client);
}

TEST_CASE("Title bar controls follow per-window action capabilities")
{
	const FTitleBarLayout WaylandLayout = MakeTitleBarLayout(1280, 720, 1.0f, true, false, {.bMinimize = false, .bMaximize = true, .bWindowMenu = false});
	CHECK_FALSE(WaylandLayout.bMinimizeVisible);
	CHECK(WaylandLayout.bMaximizeVisible);
	CHECK_FALSE(WaylandLayout.bSystemMenuEnabled);
	CHECK(HitTestTitleBar(WaylandLayout, 1270, 10) == ETitleBarHitRegion::CloseButton);
	CHECK(HitTestTitleBar(WaylandLayout, 1220, 10) == ETitleBarHitRegion::MaximizeButton);
	CHECK(HitTestTitleBar(WaylandLayout, 1170, 10) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(WaylandLayout, 10, 10) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(WaylandLayout, 50, 10) == ETitleBarHitRegion::ApplicationMenu);

	const FTitleBarLayout MinimizeOnly = MakeTitleBarLayout(1280, 720, 1.0f, true, false, {.bMinimize = true, .bMaximize = false, .bWindowMenu = false});
	CHECK(HitTestTitleBar(MinimizeOnly, 1220, 10) == ETitleBarHitRegion::MinimizeButton);
	CHECK(HitTestTitleBar(MinimizeOnly, 1170, 10) == ETitleBarHitRegion::Caption);

	const FTitleBarLayout FixedSize = MakeTitleBarLayout(1280, 720, 1.0f, false, false, {.bMinimize = false, .bMaximize = true, .bWindowMenu = false});
	CHECK_FALSE(FixedSize.bMaximizeVisible);
	CHECK(HitTestTitleBar(FixedSize, 1220, 10) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(FixedSize, 1270, 10) == ETitleBarHitRegion::CloseButton);
}

TEST_CASE("Window action policy can suppress every title bar control")
{
	constexpr FWindowActionCapabilities Advertised;
	constexpr FWindowActionPolicy NoControls{.bAllowClose = false, .bAllowMinimize = false, .bAllowMaximize = false, .bAllowWindowMenu = false};
	const FTitleBarLayout Layout = MakeTitleBarLayout(1280, 720, 1.0f, true, false, Advertised, NoControls);
	CHECK_FALSE(Layout.bCloseVisible);
	CHECK_FALSE(Layout.bMinimizeVisible);
	CHECK_FALSE(Layout.bMaximizeVisible);
	CHECK_FALSE(Layout.bSystemMenuEnabled);
	CHECK(HitTestTitleBar(Layout, 1270, 10) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(Layout, 1220, 10) == ETitleBarHitRegion::Caption);
}

TEST_CASE("Title bar UI capture is limited to cached overlapping regions")
{
	FTitleBarHitTestState State;
	State.Layout = MakeTitleBarLayout(1280, 720, 1.0f, true, false);
	State.UiCaptureRegions[0] = {.MinimumX = 200, .MinimumY = 0, .MaximumX = 500, .MaximumY = 36};
	State.UiCaptureRegionCount = 1;

	CHECK(HitTestTitleBar(State, 200, 0) == ETitleBarHitRegion::Client);
	CHECK(HitTestTitleBar(State, 499, 35) == ETitleBarHitRegion::Client);
	CHECK(HitTestTitleBar(State, 500, 35) == ETitleBarHitRegion::Caption);
	CHECK(HitTestTitleBar(State, 1270, 10) == ETitleBarHitRegion::CloseButton);

	State.bUiCapturesEntireTitleBar = true;
	CHECK(HitTestTitleBar(State, 1270, 10) == ETitleBarHitRegion::Client);
}

TEST_CASE("Title bar resizing is disabled while maximized")
{
	const FTitleBarLayout Restored = MakeTitleBarLayout(1280, 720, 1.0f, true, false);
	const FTitleBarLayout Maximized = MakeTitleBarLayout(1280, 720, 1.0f, true, true);

	CHECK(HitTestTitleBar(Restored, 0, 0) == ETitleBarHitRegion::ResizeTopLeft);
	CHECK(HitTestTitleBar(Maximized, 0, 0) == ETitleBarHitRegion::SystemMenu);
}

TEST_CASE("Title bar scale follows the native coordinate space")
{
	CHECK(ResolveTitleBarUiScale(false, 1.5f) == doctest::Approx(1.5f));
	CHECK(ResolveTitleBarUiScale(true, 1.5f) == doctest::Approx(1.0f));
	CHECK(ScaleTitleBarMetric(36, 1.25f) == 45);
}

TEST_CASE("Default window placement uses eighty percent of the work area")
{
	const FWindowPlacement Placement = ResolveCenteredWindowPlacement(100, 50, 2000, 1000);

	CHECK(Placement.X == 300);
	CHECK(Placement.Y == 150);
	CHECK(Placement.Width == 1600);
	CHECK(Placement.Height == 800);
}

TEST_CASE("Window placement rejects unusable work areas and clamps percentages")
{
	CHECK(ResolveCenteredWindowPlacement(10, 20, 0, 1080).Width == 0);
	CHECK(ResolveCenteredWindowPlacement(10, 20, 1920, 1080, 0).Width == 19);
	CHECK(ResolveCenteredWindowPlacement(10, 20, 1920, 1080, 200).Width == 1920);
}

TEST_CASE("Modifier flags compose without exposing GLFW values")
{
	const EModifierFlags Flags = EModifierFlags::Shift | EModifierFlags::Control;

	CHECK(HasModifier(Flags, EModifierFlags::Shift));
	CHECK(HasModifier(Flags, EModifierFlags::Control));
	CHECK_FALSE(HasModifier(Flags, EModifierFlags::Alt));
}

TEST_CASE("Window behavior policy preserves unsupported compositor behavior")
{
	FWindowDescriptor Descriptor;
	Descriptor.bShowInTaskbar = false;
	Descriptor.bTopMost = true;
	Descriptor.bFocusOnShow = false;

	CHECK(ResolveWindowBehaviorPolicy(EWindowSystem::Win32, Descriptor) == FWindowBehaviorPolicy{false, true, false});
	CHECK(ResolveWindowBehaviorPolicy(EWindowSystem::X11, Descriptor) == FWindowBehaviorPolicy{false, true, false});
	CHECK(ResolveWindowBehaviorPolicy(EWindowSystem::Wayland, Descriptor) == FWindowBehaviorPolicy{true, false, false});
	CHECK(ResolveWindowBehaviorPolicy(EWindowSystem::Null, Descriptor) == FWindowBehaviorPolicy{true, false, false});
}

TEST_CASE("GLFW null platform supports a complete application lifecycle")
{
	FApplicationDescriptor ApplicationDescriptor;
	ApplicationDescriptor.PreferredWindowSystem = EWindowSystem::Null;
	std::expected<std::unique_ptr<FApplication>, FApplicationError> ApplicationResult = FApplication::Create(ApplicationDescriptor);
	REQUIRE(ApplicationResult.has_value());
	std::unique_ptr<FApplication> Application = std::move(*ApplicationResult);

	CHECK(Application->GetCapabilities().WindowSystem == EWindowSystem::Null);
	CHECK_FALSE(Application->GetCapabilities().bCustomTitleBars);
	CHECK_FALSE(Application->GetCapabilities().bProgrammaticWindowPosition);
	CHECK_FALSE(Application->GetCapabilities().bTaskbarVisibility);
	CHECK_FALSE(Application->GetCapabilities().bTopMostWindows);

	FWindowDescriptor WindowDescriptor;
	WindowDescriptor.Title = "Herta null platform test";
	WindowDescriptor.Width = 640;
	WindowDescriptor.Height = 360;
	std::expected<FWindow*, FApplicationError> WindowResult = Application->CreateWindow(std::move(WindowDescriptor));
	REQUIRE(WindowResult.has_value());
	CHECK((*WindowResult)->GetWidth() == 640);
	CHECK((*WindowResult)->GetHeight() == 360);
	CHECK((*WindowResult)->IsShownInTaskbar());
	CHECK_FALSE((*WindowResult)->IsTopMost());
	CHECK((*WindowResult)->WillFocusOnShow());
	CHECK((*WindowResult)->GetActionCapabilities() == FWindowActionCapabilities{});
	(*WindowResult)->SetActionPolicy({.bAllowClose = false, .bAllowMinimize = false, .bAllowMaximize = false, .bAllowWindowMenu = false});
	CHECK_FALSE((*WindowResult)->GetTitleBarHitTestState().Layout.bCloseVisible);
	CHECK_FALSE((*WindowResult)->GetTitleBarHitTestState().Layout.bMinimizeVisible);
	CHECK_FALSE((*WindowResult)->GetTitleBarHitTestState().Layout.bMaximizeVisible);
	CHECK_FALSE((*WindowResult)->GetTitleBarHitTestState().Layout.bSystemMenuEnabled);
	CHECK(Application->PumpEvents() == EEventPumpMode::Waited);
}

#ifdef HERTA_PLATFORM_WINDOWS
TEST_CASE("Win32 application creates a custom-title-bar window")
{
	std::expected<std::unique_ptr<FApplication>, FApplicationError> ApplicationResult = FApplication::Create();
	REQUIRE(ApplicationResult.has_value());
	std::unique_ptr<FApplication> Application = std::move(*ApplicationResult);
	REQUIRE(Application->GetCapabilities().WindowSystem == EWindowSystem::Win32);
	REQUIRE(Application->GetCapabilities().bCustomTitleBars);
	REQUIRE(Application->GetCapabilities().bTaskbarVisibility);
	REQUIRE(Application->GetCapabilities().bTopMostWindows);

	FWindowDescriptor WindowDescriptor;
	WindowDescriptor.Title = "Herta Win32 platform test";
	WindowDescriptor.Width = 640;
	WindowDescriptor.Height = 360;
	WindowDescriptor.bCustomTitleBar = true;
	WindowDescriptor.bShowInTaskbar = false;
	WindowDescriptor.bTopMost = true;
	WindowDescriptor.bFocusOnShow = false;
	std::expected<FWindow*, FApplicationError> WindowResult = Application->CreateWindow(std::move(WindowDescriptor));
	REQUIRE(WindowResult.has_value());
	CHECK((*WindowResult)->GetBackendHandle().Value != nullptr);
	CHECK_FALSE((*WindowResult)->IsShownInTaskbar());
	CHECK((*WindowResult)->IsTopMost());
	CHECK_FALSE((*WindowResult)->WillFocusOnShow());
	CHECK(Application->PumpEvents() == EEventPumpMode::Waited);
}
#endif
}
