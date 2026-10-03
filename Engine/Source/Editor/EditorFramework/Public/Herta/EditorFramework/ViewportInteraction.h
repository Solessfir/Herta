#pragma once

#include "Herta/EditorCore/ViewportCamera.h"

#include <array>
#include <cstddef>

namespace Herta
{
[[nodiscard]] constexpr FVector2 GetViewportProjectionCenter(const FVector2 CanvasMinimum, const FVector2 CanvasSize, const FVector2 ViewportMinimum, const FVector2 ViewportSize) noexcept
{
	return CanvasSize.X > 0.f && CanvasSize.Y > 0.f ? FVector2{(ViewportMinimum.X + ViewportSize.X * 0.5f - CanvasMinimum.X) / CanvasSize.X, (ViewportMinimum.Y + ViewportSize.Y * 0.5f - CanvasMinimum.Y) / CanvasSize.Y} : FVector2{0.5f, 0.5f};
}

[[nodiscard]] constexpr float GetViewportGizmoPixelScale(const float InterfaceScale, const float ImageHeight, const float FramebufferHeight) noexcept
{
	return ImageHeight > 0.f ? InterfaceScale * FramebufferHeight / ImageHeight : InterfaceScale;
}

struct FViewportInteractionInput
{
	bool bImageHovered = false;
	bool bImageActive = false;
	bool bWindowFocused = false;
	bool bApplicationFocused = true;
	bool bInputBlocked = false;
	bool bAlt = false;
	std::array<bool, 3> MouseClicked{};
	std::array<bool, 3> MouseDown{};
};

struct FViewportInteractionState
{
	EViewportCameraMode CameraMode = EViewportCameraMode::None;
	int DragButton = -1;
	bool bKeyboardFocus = false;

	[[nodiscard]] bool CanUseGizmo(const FViewportInteractionInput& Input) const noexcept
	{
		return Input.bApplicationFocused && !Input.bInputBlocked && CameraMode == EViewportCameraMode::None && (Input.bImageHovered || DragButton == 0);
	}

	void Cancel() noexcept
	{
		CameraMode = EViewportCameraMode::None;
		DragButton = -1;
		bKeyboardFocus = false;
	}

	void Update(const FViewportInteractionInput& Input) noexcept
	{
		if (!Input.bWindowFocused || Input.bInputBlocked)
		{
			Cancel();
			return;
		}

		if (DragButton >= 0 && (!Input.bImageActive || !Input.MouseDown[static_cast<std::size_t>(DragButton)]))
		{
			CameraMode = EViewportCameraMode::None;
			DragButton = -1;
		}

		if (DragButton < 0 && Input.bImageHovered && Input.bImageActive)
		{
			for (int Button = 0; Button < 3; ++Button)
			{
				if (!Input.MouseClicked[static_cast<std::size_t>(Button)])
				{
					continue;
				}

				DragButton = Button;
				bKeyboardFocus = true;
				CameraMode = Button == 2 ? EViewportCameraMode::Pan : Button == 1 ? (Input.bAlt ? EViewportCameraMode::Dolly : EViewportCameraMode::Fly)
				                                                  : Input.bAlt    ? EViewportCameraMode::Orbit
				                                                                  : EViewportCameraMode::None;
				break;
			}
		}
	}
};
}
