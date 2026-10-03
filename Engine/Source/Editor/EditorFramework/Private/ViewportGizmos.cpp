/*
Translation handle behavior adapted from im3d.
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

#include "ViewportGizmos.h"

#include <im3d.h>
#include <im3d_math.h>

#include <array>

namespace Herta
{
bool DrawPreviewTranslationGizmo(Im3d::Vec3& Translation, const Im3d::Mat3& Rotation, const bool bLocal)
{
	Im3d::Context& Context = Im3d::GetContext();
	Im3d::AppData& AppData = Context.getAppData();
	const Im3d::Vec3 DrawAt = Translation;
	const Im3d::Mat3 Basis = bLocal ? Rotation : Im3d::Mat3(1.f);
	const Im3d::Mat3 InverseBasis = Im3d::Transpose(Basis);
	std::array Axes{Basis.getCol(0), Basis.getCol(1), Basis.getCol(2)};
	constexpr std::array PlaneColors{Im3d::Color_Red, Im3d::Color_Green, Im3d::Color_Blue};
	std::array AxisColors = PlaneColors;
	const Im3d::Vec3 ViewDirection = AppData.m_projOrtho ? -AppData.m_viewDirection : Im3d::Normalize(AppData.m_viewOrigin - DrawAt);
	for (Im3d::Vec3& Axis : Axes)
	{
		if (AppData.m_flipGizmoWhenBehind && Im3d::Dot(Axis, ViewDirection) < 0.f)
		{
			Axis = -Axis;
		}
	}

	const float WorldHeight = Context.pixelsToWorldSize(DrawAt, Context.m_gizmoHeightPixels);
	const float WorldSize = Context.pixelsToWorldSize(DrawAt, Context.m_gizmoSizePixels);
	const float PlaneHalfSize = WorldHeight * (6.f / 80.f);
	const float PlaneOffset = WorldHeight * 0.6f;
	const Im3d::Id AppId = Im3d::MakeId("PreviewCube");
	Context.pushId(AppId);
	Context.m_appId = AppId;
	Context.pushMatrix(Im3d::Mat4(1.f));
	Context.pushEnableSorting(true);
	bool bChanged = false;
	constexpr std::array PlaneNames{"planeYZ", "planeXZ", "planeXY"};
	for (std::size_t Index = 0; Index < Axes.size(); ++Index)
	{
		const std::size_t First = (Index + 1) % 3;
		const std::size_t Second = (Index + 2) % 3;
		const Im3d::Id PlaneId = Im3d::MakeId(PlaneNames[Index]);
		const Im3d::Vec3 Center = DrawAt + (Axes[First] + Axes[Second]) * PlaneOffset;
		const Im3d::Vec3 RayOrigin = AppData.m_cursorRayOrigin;
		const Im3d::Vec3 RayDirection = AppData.m_cursorRayDirection;
		Im3d::Vec3 LocalTranslation = InverseBasis * Translation;
		// im3d's plane bounds are axis-aligned, so local squares need a matching ray basis.
		AppData.m_cursorRayOrigin = InverseBasis * RayOrigin;
		AppData.m_cursorRayDirection = InverseBasis * RayDirection;
		const bool bPlaneChanged = Context.gizmoPlaneTranslation_Behavior(PlaneId, InverseBasis * Center, InverseBasis * Axes[Index], AppData.m_snapTranslation, PlaneHalfSize, &LocalTranslation);
		AppData.m_cursorRayOrigin = RayOrigin;
		AppData.m_cursorRayDirection = RayDirection;
		if (bPlaneChanged)
		{
			Translation = Basis * LocalTranslation;
			bChanged = true;
		}

		const bool bHighlighted = PlaneId == Context.m_hotId || PlaneId == Context.m_activeId;
		Im3d::PushColor(bHighlighted ? Im3d::Color_Yellow : PlaneColors[Index]);
		const Im3d::Vec3 SideA = Axes[First] * PlaneHalfSize;
		const Im3d::Vec3 SideB = Axes[Second] * PlaneHalfSize;
		Im3d::DrawQuadFilled(Center - SideA - SideB, Center + SideA - SideB, Center + SideA + SideB, Center - SideA + SideB);
		Im3d::PopColor();
		if (bHighlighted)
		{
			AxisColors[First] = AxisColors[Second] = Im3d::Color_Yellow;
		}
	}

	const Im3d::Id CenterId = Im3d::MakeId("planeV");
	const Im3d::Id PreviousActiveId = Context.m_activeId;
	const Im3d::Vec3 CenterNormal = CenterId == PreviousActiveId ? Context.m_gizmoStateMat3.getCol(0) : AppData.m_viewDirection;
	bChanged |= Context.gizmoPlaneTranslation_Behavior(CenterId, DrawAt, CenterNormal, AppData.m_snapTranslation, WorldSize, &Translation);
	if (Context.m_activeId == CenterId && PreviousActiveId != CenterId)
	{
		Context.m_gizmoStateMat3.setCol(0, CenterNormal);
	}

	Im3d::DrawPoint(DrawAt, Context.m_gizmoSizePixels * 2.f, CenterId == Context.m_hotId || CenterId == Context.m_activeId ? Im3d::Color_Yellow : Im3d::Color_White);
	constexpr std::array AxisNames{"axisX", "axisY", "axisZ"};
	for (std::size_t Index = 0; Index < Axes.size(); ++Index)
	{
		const Im3d::Id AxisId = Im3d::MakeId(AxisNames[Index]);
		bChanged |= Context.gizmoAxisTranslation_Behavior(AxisId, DrawAt, Axes[Index], AppData.m_snapTranslation, WorldHeight, WorldSize, &Translation);
		const bool bHighlighted = AxisId == Context.m_hotId || AxisId == Context.m_activeId;
		Im3d::PushColor(bHighlighted ? Im3d::Color_Yellow : AxisColors[Index]);
		Im3d::PushSize(Context.m_gizmoSizePixels);
		Im3d::DrawArrow(DrawAt + Axes[Index] * (0.2f * WorldHeight), DrawAt + Axes[Index] * WorldHeight);
		Im3d::PopSize();
		Im3d::PopColor();
	}

	Context.popEnableSorting();
	Context.popMatrix();
	Context.popId();
	return bChanged;
}
}
