#pragma once

namespace Herta
{
inline constexpr int DefaultTitleBarHeight = 36;

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

struct FTitleBarLayout
{
	int WindowWidth = 0;
	int WindowHeight = 0;
	int TitleBarHeight = DefaultTitleBarHeight;
	int ButtonWidth = 46;
	int ResizeBorder = 6;
	bool bResizable = true;
	bool bMaximized = false;
};

struct FTitleBarHitTestState
{
	FTitleBarLayout Layout;
	bool bUiCapturesMouse = false;
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

[[nodiscard]] constexpr FTitleBarLayout MakeTitleBarLayout(const int WindowWidth, const int WindowHeight, const float ContentScale, const bool bResizable, const bool bMaximized) noexcept
{
	return {
	    .WindowWidth = WindowWidth,
	    .WindowHeight = WindowHeight,
	    .TitleBarHeight = ScaleTitleBarMetric(DefaultTitleBarHeight, ContentScale),
	    .ButtonWidth = ScaleTitleBarMetric(46, ContentScale),
	    .ResizeBorder = ScaleTitleBarMetric(6, ContentScale),
	    .bResizable = bResizable,
	    .bMaximized = bMaximized};
}

[[nodiscard]] constexpr ETitleBarHitRegion HitTestTitleBar(const FTitleBarLayout& Layout, const int X, const int Y, const bool bUiCapturesMouse) noexcept
{
	if (bUiCapturesMouse)
	{
		return ETitleBarHitRegion::Client;
	}

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

	if (X >= Layout.WindowWidth - Layout.ButtonWidth)
	{
		return ETitleBarHitRegion::CloseButton;
	}

	if (X >= Layout.WindowWidth - Layout.ButtonWidth * 2)
	{
		return ETitleBarHitRegion::MaximizeButton;
	}

	if (X >= Layout.WindowWidth - Layout.ButtonWidth * 3)
	{
		return ETitleBarHitRegion::MinimizeButton;
	}

	if (X < Layout.TitleBarHeight)
	{
		return ETitleBarHitRegion::SystemMenu;
	}

	if (X < Layout.TitleBarHeight * 2)
	{
		return ETitleBarHitRegion::ApplicationMenu;
	}

	return ETitleBarHitRegion::Caption;
}
}
