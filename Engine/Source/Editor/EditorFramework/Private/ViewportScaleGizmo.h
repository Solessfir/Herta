#pragma once

#include <im3d.h>

namespace Herta
{
enum class EViewportScaleHandle
{
	None,
	X,
	Y,
	Z,
	YZ,
	XZ,
	XY,
	Uniform
};

struct FViewportScaleGizmoState
{
	Im3d::Id ActiveId = Im3d::Id_Invalid;
	EViewportScaleHandle Handle = EViewportScaleHandle::None;
	Im3d::Vec3 StartScale{1.f};
	Im3d::Vec3 StartHandlePosition{0.f};
	Im3d::Vec3 DragPosition{0.f};
	float WorldHeight = 0.f;

	void Reset() noexcept;
};

struct FViewportScaleGizmoFeedback
{
	bool bActive = false;
	EViewportScaleHandle Handle = EViewportScaleHandle::None;
	Im3d::Vec3 HandlePosition{0.f};
	Im3d::Vec3 Factors{1.f};
};

bool DrawPreviewScaleGizmo(const Im3d::Vec3& Translation, const Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FViewportScaleGizmoState& State, FViewportScaleGizmoFeedback& Feedback);
}
