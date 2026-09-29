#pragma once

#include "Herta/EditorCore/ViewportCamera.h"

#include <array>
#include <cstddef>

namespace Herta
{
[[nodiscard]] constexpr float GetViewportGizmoPixelScale(const float InterfaceScale, const float ImageHeight, const float FramebufferHeight) noexcept
{
	return ImageHeight > 0.0f ? InterfaceScale * FramebufferHeight / ImageHeight : InterfaceScale;
}

struct FViewportInteractionInput
{
	bool bImageHovered = false;
	bool bImageActive = false;
	bool bWindowFocused = false;
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
