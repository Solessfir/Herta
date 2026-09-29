#pragma once

#include <im3d.h>
#include <optional>

namespace Herta
{
struct FViewportRotationFeedbackState
{
	std::optional<float> AngleDegrees;
	Im3d::Id ActiveId = Im3d::Id_Invalid;
	Im3d::Vec3 InitialRadial{1.0f, 0.0f, 0.0f};

	void Reset() noexcept
	{
		AngleDegrees.reset();
		ActiveId = Im3d::Id_Invalid;
	}
};

bool DrawPreviewRotationGizmo(const Im3d::Vec3& Translation, Im3d::Mat3& Rotation, bool bLocal, FViewportRotationFeedbackState& State);
}
