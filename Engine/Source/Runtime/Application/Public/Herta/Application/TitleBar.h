#pragma once

#include <array>
#include <cstddef>

namespace Herta
{
inline constexpr int DefaultTitleBarHeight = 36;
inline constexpr std::size_t MaxTitleBarUiCaptureRegions = 16;

enum class ETitleBarHitRegion
{
	Client,
	Caption,
	ResizeLeft,
	ResizeRight,
	ResizeTop,
	ResizeBottom,
	ResizeTopLeft,
	ResizeTopRight,
	ResizeBottomLeft,
	ResizeBottomRight,
	SystemMenu,
	ApplicationMenu,
	MinimizeButton,
	MaximizeButton,
	CloseButton
};

struct FWindowActionCapabilities
{
	bool bMinimize = true;
	bool bMaximize = true;
	bool bWindowMenu = true;

	[[nodiscard]] constexpr bool operator==(const FWindowActionCapabilities&) const noexcept = default;
};

struct FWindowActionPolicy
{
	bool bAllowClose = true;
	bool bAllowMinimize = true;
	bool bAllowMaximize = true;
	bool bAllowWindowMenu = true;

	[[nodiscard]] constexpr bool operator==(const FWindowActionPolicy&) const noexcept = default;
};

struct FTitleBarControlBounds
{
	int MinimumX = 0;
	int MaximumX = 0;
	bool bVisible = false;

	[[nodiscard]] constexpr bool Contains(const int X) const noexcept
	{
		return bVisible && X >= MinimumX && X < MaximumX;
	}
};

struct FTitleBarLayout
{
	int WindowWidth = 0;
	int WindowHeight = 0;
	int TitleBarHeight = DefaultTitleBarHeight;
	int ButtonWidth = 46;
	int ResizeBorder = 6;
	bool bResizable = true;
	bool bMaximized = false;
	bool bCloseVisible = true;
	bool bMinimizeVisible = true;
	bool bMaximizeVisible = true;
	bool bSystemMenuEnabled = true;
};

struct FTitleBarUiCaptureRegion
{
	int MinimumX = 0;
	int MinimumY = 0;
	int MaximumX = 0;
	int MaximumY = 0;

	[[nodiscard]] constexpr bool Contains(const int X, const int Y) const noexcept
	{
		return X >= MinimumX && X < MaximumX && Y >= MinimumY && Y < MaximumY;
	}
};

struct FTitleBarHitTestState
{
	FTitleBarLayout Layout;
	std::array<FTitleBarUiCaptureRegion, MaxTitleBarUiCaptureRegions> UiCaptureRegions{};
	std::size_t UiCaptureRegionCount = 0;
	bool bUiCapturesEntireTitleBar = false;
};

[[nodiscard]] constexpr float ResolveTitleBarUiScale(const bool bWayland, const float ContentScale) noexcept
{
	return bWayland || ContentScale <= 0.0f ? 1.0f : ContentScale;
}

[[nodiscard]] constexpr int ScaleTitleBarMetric(const int Value, const float ContentScale) noexcept
{
	const float ScaledValue = static_cast<float>(Value) * ContentScale;
	const int WholeValue = static_cast<int>(ScaledValue);
	const float Fraction = ScaledValue - static_cast<float>(WholeValue);

	// MSVC does not yet provide constexpr lround in every supported toolset.
	if (Fraction >= 0.5f)
	{
		return WholeValue + 1;
	}

	if (Fraction <= -0.5f)
	{
		return WholeValue - 1;
	}

	return WholeValue;
}

[[nodiscard]] constexpr FTitleBarLayout MakeTitleBarLayout(const int WindowWidth, const int WindowHeight, const float ContentScale, const bool bResizable, const bool bMaximized, const FWindowActionCapabilities Capabilities = {}, const FWindowActionPolicy Policy = {}) noexcept
{
	return {
	    .WindowWidth = WindowWidth,
	    .WindowHeight = WindowHeight,
	    .TitleBarHeight = ScaleTitleBarMetric(DefaultTitleBarHeight, ContentScale),
	    .ButtonWidth = ScaleTitleBarMetric(46, ContentScale),
	    .ResizeBorder = ScaleTitleBarMetric(6, ContentScale),
	    .bResizable = bResizable,
	    .bMaximized = bMaximized,
	    .bCloseVisible = Policy.bAllowClose,
	    .bMinimizeVisible = Capabilities.bMinimize && Policy.bAllowMinimize,
	    .bMaximizeVisible = Capabilities.bMaximize && Policy.bAllowMaximize && bResizable,
	    .bSystemMenuEnabled = Capabilities.bWindowMenu && Policy.bAllowWindowMenu};
}

[[nodiscard]] constexpr FTitleBarControlBounds GetTitleBarControlBounds(const FTitleBarLayout& Layout, const ETitleBarHitRegion Region) noexcept
{
	int MaximumX = Layout.WindowWidth;
	if (Layout.bCloseVisible)
	{
		const FTitleBarControlBounds CloseBounds{MaximumX - Layout.ButtonWidth, MaximumX, true};
		if (Region == ETitleBarHitRegion::CloseButton)
		{
			return CloseBounds;
		}
		MaximumX = CloseBounds.MinimumX;
	}

	if (Layout.bMaximizeVisible)
	{
		const FTitleBarControlBounds MaximizeBounds{MaximumX - Layout.ButtonWidth, MaximumX, true};
		if (Region == ETitleBarHitRegion::MaximizeButton)
		{
			return MaximizeBounds;
		}
		MaximumX = MaximizeBounds.MinimumX;
	}

	if (Layout.bMinimizeVisible)
	{
		const FTitleBarControlBounds MinimizeBounds{MaximumX - Layout.ButtonWidth, MaximumX, true};
		if (Region == ETitleBarHitRegion::MinimizeButton)
		{
			return MinimizeBounds;
		}
	}

	return {};
}

[[nodiscard]] constexpr ETitleBarHitRegion HitTestTitleBar(const FTitleBarLayout& Layout, const int X, const int Y) noexcept
{
	if (X < 0 || Y < 0 || X >= Layout.WindowWidth || Y >= Layout.WindowHeight)
	{
		return ETitleBarHitRegion::Client;
	}

	if (Layout.bResizable && !Layout.bMaximized)
	{
		const bool bLeft = X < Layout.ResizeBorder;
		const bool bRight = X >= Layout.WindowWidth - Layout.ResizeBorder;
		const bool bTop = Y < Layout.ResizeBorder;
		const bool bBottom = Y >= Layout.WindowHeight - Layout.ResizeBorder;

		if (bTop && bLeft)
		{
			return ETitleBarHitRegion::ResizeTopLeft;
		}

		if (bTop && bRight)
		{
			return ETitleBarHitRegion::ResizeTopRight;
		}

		if (bBottom && bLeft)
		{
			return ETitleBarHitRegion::ResizeBottomLeft;
		}

		if (bBottom && bRight)
		{
			return ETitleBarHitRegion::ResizeBottomRight;
		}

		if (bLeft)
		{
			return ETitleBarHitRegion::ResizeLeft;
		}

		if (bRight)
		{
			return ETitleBarHitRegion::ResizeRight;
		}

		if (bTop)
		{
			return ETitleBarHitRegion::ResizeTop;
		}

		if (bBottom)
		{
			return ETitleBarHitRegion::ResizeBottom;
		}
	}

	if (Y >= Layout.TitleBarHeight)
	{
		return ETitleBarHitRegion::Client;
	}

	if (GetTitleBarControlBounds(Layout, ETitleBarHitRegion::CloseButton).Contains(X))
	{
		return ETitleBarHitRegion::CloseButton;
	}

	if (GetTitleBarControlBounds(Layout, ETitleBarHitRegion::MaximizeButton).Contains(X))
	{
		return ETitleBarHitRegion::MaximizeButton;
	}

	if (GetTitleBarControlBounds(Layout, ETitleBarHitRegion::MinimizeButton).Contains(X))
	{
		return ETitleBarHitRegion::MinimizeButton;
	}

	if (Layout.bSystemMenuEnabled && X < Layout.TitleBarHeight)
	{
		return ETitleBarHitRegion::SystemMenu;
	}

	const int ApplicationMenuMinimumX = Layout.TitleBarHeight;
	if (X >= ApplicationMenuMinimumX && X < ApplicationMenuMinimumX + Layout.TitleBarHeight)
	{
		return ETitleBarHitRegion::ApplicationMenu;
	}

	return ETitleBarHitRegion::Caption;
}

[[nodiscard]] constexpr ETitleBarHitRegion HitTestTitleBar(const FTitleBarHitTestState& State, const int X, const int Y) noexcept
{
	if (State.bUiCapturesEntireTitleBar)
	{
		return ETitleBarHitRegion::Client;
	}

	const std::size_t RegionCount = State.UiCaptureRegionCount < State.UiCaptureRegions.size() ? State.UiCaptureRegionCount : State.UiCaptureRegions.size();
	for (std::size_t RegionIndex = 0; RegionIndex < RegionCount; ++RegionIndex)
	{
		if (State.UiCaptureRegions[RegionIndex].Contains(X, Y))
		{
			return ETitleBarHitRegion::Client;
		}
	}

	return HitTestTitleBar(State.Layout, X, Y);
}
}
