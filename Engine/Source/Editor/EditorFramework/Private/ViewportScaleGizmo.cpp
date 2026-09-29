/*
Scale handle behavior adapted from im3d.
Copyright (c) 2016-2025 John Chapman

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "ViewportScaleGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <im3d_math.h>

namespace Herta
{
namespace
{
constexpr float MinimumPreviewScale = 0.001f;
constexpr float MaximumPreviewScale = 1000.0f;

float GetScaleIncrement(const float Increment, const float Snap)
{
	return std::isfinite(Snap) && Snap > 0.0f ? std::round(Increment / Snap) * Snap : Increment;
}

float ApplyScaleFactor(Im3d::Vec3& Scale, const Im3d::Vec3& StartScale, const unsigned int AxisMask, const float RequestedFactor)
{
	float MinimumFactor = 0.0f;
	float MaximumFactor = MaximumPreviewScale / MinimumPreviewScale;
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		if ((AxisMask & (1u << Axis)) != 0)
		{
			MinimumFactor = std::max(MinimumFactor, MinimumPreviewScale / StartScale[Axis]);
			MaximumFactor = std::min(MaximumFactor, MaximumPreviewScale / StartScale[Axis]);
		}
	}
	const float Factor = std::clamp(std::isfinite(RequestedFactor) ? RequestedFactor : 1.0f, MinimumFactor, MaximumFactor);
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		if ((AxisMask & (1u << Axis)) != 0)
		{
			Scale[Axis] = StartScale[Axis] * Factor;
		}
	}
	return Factor;
}

void DrawScaleCube(const Im3d::Vec3& Position, const Im3d::Mat3& Rotation, const float HalfSize, const Im3d::Color Color)
{
	Im3d::PushMatrix(Im3d::Mat4(Position, Rotation, Im3d::Vec3(1.0f)));
	Im3d::PushColor(Color);
	Im3d::DrawAlignedBoxFilled(Im3d::Vec3(-HalfSize), Im3d::Vec3(HalfSize));
	Im3d::PopColor();
	Im3d::PopMatrix();
}
}

void FViewportScaleGizmoState::Reset() noexcept
{
	*this = {};
}

bool DrawPreviewScaleGizmo(const Im3d::Vec3& Translation, const Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FViewportScaleGizmoState& State, FViewportScaleGizmoFeedback& Feedback)
{
	Im3d::Context& Context = Im3d::GetContext();
	Im3d::AppData& AppData = Context.getAppData();
	Feedback = {};
	if (State.ActiveId != Context.m_activeId || !Context.isKeyDown(Im3d::Action_Select))
	{
		if (State.ActiveId != Im3d::Id_Invalid && State.ActiveId == Context.m_activeId)
		{
			Context.makeActive(Im3d::Id_Invalid);
		}
		State.Reset();
	}
	const float WorldHeight = State.ActiveId == Im3d::Id_Invalid ? Context.pixelsToWorldSize(Translation, Context.m_gizmoHeightPixels) : State.WorldHeight;
	if (!std::isfinite(WorldHeight) || WorldHeight <= 0.0f || !std::isfinite(Scale.x) || !std::isfinite(Scale.y) || !std::isfinite(Scale.z) || Scale.x < MinimumPreviewScale || Scale.y < MinimumPreviewScale || Scale.z < MinimumPreviewScale || Scale.x > MaximumPreviewScale || Scale.y > MaximumPreviewScale || Scale.z > MaximumPreviewScale)
	{
		State.Reset();
		Context.resetId();
		return false;
	}
	const float WorldSize = Context.pixelsToWorldSize(Translation, Context.m_gizmoSizePixels);
	const float PlaneHalfSize = WorldHeight * (6.0f / 80.0f);
	const float PlaneOffset = WorldHeight * 0.60f;
	const Im3d::Mat3 InverseBasis = Im3d::Transpose(Rotation);
	std::array Axes{Rotation.getCol(0), Rotation.getCol(1), Rotation.getCol(2)};
	constexpr std::array Colors{Im3d::Color_Red, Im3d::Color_Green, Im3d::Color_Blue};
	std::array AxisColors = Colors;
	const Im3d::Vec3 ViewDirection = AppData.m_projOrtho ? -AppData.m_viewDirection : Im3d::Normalize(AppData.m_viewOrigin - Translation);
	for (Im3d::Vec3& Axis : Axes)
	{
		if (AppData.m_flipGizmoWhenBehind && Im3d::Dot(Axis, ViewDirection) < 0.0f)
		{
			Axis = -Axis;
		}
	}
	const Im3d::Id AppId = Im3d::MakeId("PreviewCube");
	Context.pushId(AppId);
	Context.m_appId = AppId;
	Context.pushMatrix(Im3d::Mat4(1.0f));
	Context.pushEnableSorting(true);
	bool bChanged = false;
	const auto BeginDrag = [&](const Im3d::Id Id, const EViewportScaleHandle Handle, const Im3d::Vec3& HandlePosition, const Im3d::Vec3& DragPosition)
	{
		if (Context.m_activeId == Id && State.ActiveId != Id)
		{
			State.ActiveId = Id;
			State.Handle = Handle;
			State.StartScale = Scale;
			State.StartHandlePosition = HandlePosition;
			State.DragPosition = DragPosition;
			State.WorldHeight = WorldHeight;
		}
	};
	constexpr std::array PlaneNames{"scaleYZ", "scaleXZ", "scaleXY"};
	constexpr std::array PlaneHandles{EViewportScaleHandle::YZ, EViewportScaleHandle::XZ, EViewportScaleHandle::XY};
	for (std::size_t Index = 0; Index < Axes.size(); ++Index)
	{
		const std::size_t First = (Index + 1) % 3;
		const std::size_t Second = (Index + 2) % 3;
		const Im3d::Id PlaneId = Im3d::MakeId(PlaneNames[Index]);
		if (Context.m_activeId != Im3d::Id_Invalid && Context.m_activeId != PlaneId)
		{
			continue;
		}
		const Im3d::Vec3 Center = Translation + (Axes[First] + Axes[Second]) * PlaneOffset;
		Im3d::Vec3 DragPosition = State.ActiveId == PlaneId ? State.DragPosition : InverseBasis * Center;
		const Im3d::Vec3 RayOrigin = AppData.m_cursorRayOrigin;
		const Im3d::Vec3 RayDirection = AppData.m_cursorRayDirection;
		AppData.m_cursorRayOrigin = InverseBasis * RayOrigin;
		AppData.m_cursorRayDirection = InverseBasis * RayDirection;
		const bool bPlaneChanged = Context.gizmoPlaneTranslation_Behavior(PlaneId, InverseBasis * Center, InverseBasis * Axes[Index], 0.0f, PlaneHalfSize, &DragPosition);
		AppData.m_cursorRayOrigin = RayOrigin;
		AppData.m_cursorRayDirection = RayDirection;
		BeginDrag(PlaneId, PlaneHandles[Index], Center, DragPosition);
		float Factor = 1.0f;
		if (State.ActiveId == PlaneId)
		{
			State.DragPosition = DragPosition;
			if (bPlaneChanged)
			{
				const Im3d::Vec3 Diagonal = Im3d::Normalize(Axes[First] + Axes[Second]);
				const Im3d::Vec3 Delta = Rotation * DragPosition - State.StartHandlePosition;
				Factor = ApplyScaleFactor(Scale, State.StartScale, (1u << First) | (1u << Second), 1.0f + GetScaleIncrement(Im3d::Dot(Delta, Diagonal) / Im3d::Length(Center - Translation), AppData.m_snapScale));
				bChanged = true;
			}
			else
			{
				Factor = Scale[static_cast<int>(First)] / State.StartScale[static_cast<int>(First)];
			}
			Feedback.HandlePosition = Translation + (Center - Translation) * Factor;
		}
		const bool bHighlighted = PlaneId == Context.m_hotId || PlaneId == Context.m_activeId;
		const Im3d::Vec3 DrawCenter = Translation + (Center - Translation) * Factor;
		const Im3d::Vec3 SideA = Axes[First] * PlaneHalfSize;
		const Im3d::Vec3 SideB = Axes[Second] * PlaneHalfSize;
		Im3d::PushColor(bHighlighted ? Im3d::Color_Yellow : Colors[Index]);
		Im3d::DrawQuadFilled(DrawCenter - SideA - SideB, DrawCenter + SideA - SideB, DrawCenter + SideA + SideB, DrawCenter - SideA + SideB);
		Im3d::PopColor();
		if (bHighlighted)
		{
			AxisColors[First] = AxisColors[Second] = Im3d::Color_Yellow;
		}
	}
	const Im3d::Id CenterId = Im3d::MakeId("scaleUniform");
	if (Context.m_activeId == Im3d::Id_Invalid || Context.m_activeId == CenterId)
	{
		Im3d::Vec3 CenterDragPosition = State.ActiveId == CenterId ? State.DragPosition : InverseBasis * Translation;
		const Im3d::Vec3 CenterRayOrigin = AppData.m_cursorRayOrigin;
		const Im3d::Vec3 CenterRayDirection = AppData.m_cursorRayDirection;
		AppData.m_cursorRayOrigin = InverseBasis * CenterRayOrigin;
		AppData.m_cursorRayDirection = InverseBasis * CenterRayDirection;
		const bool bCenterChanged = Context.gizmoPlaneTranslation_Behavior(CenterId, InverseBasis * Translation, InverseBasis * AppData.m_viewDirection, 0.0f, WorldSize, &CenterDragPosition);
		AppData.m_cursorRayOrigin = CenterRayOrigin;
		AppData.m_cursorRayDirection = CenterRayDirection;
		BeginDrag(CenterId, EViewportScaleHandle::Uniform, Translation, CenterDragPosition);
		if (State.ActiveId == CenterId)
		{
			State.DragPosition = CenterDragPosition;
			if (bCenterChanged)
			{
				const Im3d::Vec3 CameraRight = Im3d::Normalize(Im3d::Cross(AppData.m_viewDirection, AppData.m_worldUp));
				const Im3d::Vec3 CameraUp = Im3d::Normalize(Im3d::Cross(CameraRight, AppData.m_viewDirection));
				const Im3d::Vec3 Delta = Rotation * CenterDragPosition - Translation;
				ApplyScaleFactor(Scale, State.StartScale, 7u, 1.0f + GetScaleIncrement(Im3d::Dot(Delta, Im3d::Normalize(CameraRight + CameraUp)) / WorldHeight, AppData.m_snapScale));
				bChanged = true;
			}
			Feedback.HandlePosition = Rotation * CenterDragPosition;
		}
		DrawScaleCube(State.ActiveId == CenterId ? Feedback.HandlePosition : Translation, Rotation, WorldSize, CenterId == Context.m_hotId || CenterId == Context.m_activeId ? Im3d::Color_Yellow : Im3d::Color_White);
	}
	constexpr std::array AxisNames{"scaleX", "scaleY", "scaleZ"};
	constexpr std::array AxisHandles{EViewportScaleHandle::X, EViewportScaleHandle::Y, EViewportScaleHandle::Z};
	for (std::size_t Index = 0; Index < Axes.size(); ++Index)
	{
		const Im3d::Id AxisId = Im3d::MakeId(AxisNames[Index]);
		if (Context.m_activeId != Im3d::Id_Invalid && Context.m_activeId != AxisId)
		{
			continue;
		}
		if (Context.m_activeId != AxisId && std::abs(Im3d::Dot(Axes[Index], ViewDirection)) > 0.99f)
		{
			continue;
		}
		const int AxisIndex = static_cast<int>(Index);
		float AxisScale = Scale[AxisIndex];
		const bool bAxisChanged = Context.gizmoAxisScale_Behavior(AxisId, Translation, Axes[Index], 0.0f, WorldHeight, WorldSize, &AxisScale);
		BeginDrag(AxisId, AxisHandles[Index], Translation + Axes[Index] * WorldHeight, Translation);
		float Factor = 1.0f;
		if (State.ActiveId == AxisId)
		{
			Factor = ApplyScaleFactor(Scale, State.StartScale, 1u << Index, 1.0f + GetScaleIncrement(AxisScale / State.StartScale[AxisIndex] - 1.0f, AppData.m_snapScale));
			Feedback.HandlePosition = Translation + Axes[Index] * (WorldHeight * Factor);
			bChanged |= bAxisChanged;
		}
		const bool bHighlighted = AxisId == Context.m_hotId || AxisId == Context.m_activeId;
		const Im3d::Color Color = bHighlighted ? Im3d::Color_Yellow : AxisColors[Index];
		const Im3d::Vec3 Endpoint = Translation + Axes[Index] * (WorldHeight * Factor);
		Im3d::DrawLine(Translation + Axes[Index] * (0.2f * WorldHeight), Endpoint, Context.m_gizmoSizePixels, Color);
		DrawScaleCube(Endpoint, Rotation, WorldSize, Color);
	}
	if (State.ActiveId != Im3d::Id_Invalid && State.ActiveId == Context.m_activeId)
	{
		Feedback.bActive = true;
		Feedback.Handle = State.Handle;
		Feedback.Factors = Scale / State.StartScale;
	}
	else
	{
		State.Reset();
	}
	Context.popEnableSorting();
	Context.popMatrix();
	Context.popId();
	return bChanged;
}
}
