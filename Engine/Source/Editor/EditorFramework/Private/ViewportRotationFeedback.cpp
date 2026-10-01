/*
Rotation handle behavior adapted from im3d.
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

#include "ViewportRotationFeedback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <im3d_math.h>
#include <numbers>

namespace Herta
{
bool DrawPreviewRotationGizmo(const Im3d::Vec3& Translation, Im3d::Mat3& Rotation, const bool bLocal, FViewportRotationFeedbackState& State)
{
	Im3d::Context& Context = Im3d::GetContext();
	const Im3d::AppData& AppData = Context.getAppData();
	const Im3d::Id PreviousActiveId = Context.m_activeId;
	const Im3d::Id AppId = Im3d::MakeId("PreviewCube");
	Context.pushId(AppId);
	Context.m_appId = AppId;
	Context.pushMatrix(Im3d::Mat4(1.0f));
	Context.pushEnableSorting(true);
	Im3d::PushSize(Context.m_gizmoSizePixels);
	const float WorldRadius = Context.pixelsToWorldSize(Translation, Context.m_gizmoHeightPixels);
	const float WorldSize = Context.pixelsToWorldSize(Translation, Context.m_gizmoSizePixels);
	const Im3d::Vec3 Euler = Im3d::ToEulerXYZ(Rotation);
	constexpr std::array AxisNames{"axisX", "axisY", "axisZ", "axisV"};
	constexpr std::array Colors{Im3d::Color_Red, Im3d::Color_Green, Im3d::Color_Blue, Im3d::Color_White};
	Im3d::Vec3 ActiveAxis(0.0f, 0.0f, 1.0f);
	float ActiveRadius = WorldRadius;
	float AppliedAngle = State.AngleDegrees.value_or(0.0f) * std::numbers::pi_v<float> / 180.0f;
	bool bChanged = false;
	for (std::size_t Index = 0; Index < AxisNames.size(); ++Index)
	{
		const Im3d::Id AxisId = Im3d::MakeId(AxisNames[Index]);
		if (Context.m_activeId != Im3d::Id_Invalid && Context.m_activeId != AxisId)
		{
			continue;
		}
		Im3d::Vec3 Axis(0.0f);
		if (Index == 3)
		{
			Axis = AppData.m_viewDirection;
		}
		else if (bLocal)
		{
			Axis = (Context.m_activeId == AxisId ? Context.m_gizmoStateMat3 : Rotation).getCol(static_cast<int>(Index));
		}
		else
		{
			Axis[static_cast<int>(Index)] = 1.0f;
		}
		if (!std::isfinite(Axis.x) || !std::isfinite(Axis.y) || !std::isfinite(Axis.z) || Im3d::Length2(Axis) < 0.000001f)
		{
			Axis = Im3d::Vec3(0.0f);
			Axis[static_cast<int>(Index == 3 ? 2 : Index)] = 1.0f;
		}
		Axis = Im3d::Normalize(Axis);
		const float Radius = WorldRadius * (Index == 3 ? 1.0f : 0.9f);
		float Angle = Index == 3 ? 0.0f : Euler[static_cast<int>(Index)];
		if (Context.gizmoAxislAngle_Behavior(AxisId, Translation, Axis, AppData.m_snapRotation, Radius, WorldSize, &Angle))
		{
			AppliedAngle = Index == 3 ? Angle : Angle - Context.m_gizmoStateFloat;
			if (std::isfinite(AppliedAngle))
			{
				Rotation = Im3d::Rotation(Axis, AppliedAngle) * Context.m_gizmoStateMat3;
				bChanged = true;
			}
			else
			{
				Context.resetId();
			}
		}
		Im3d::PushColor(AxisId == Context.m_hotId || AxisId == Context.m_activeId ? Im3d::Color_Yellow : Colors[Index]);
		Im3d::DrawCircle(Translation, Axis, Radius, 96);
		Im3d::PopColor();
		if (Context.m_activeId == AxisId)
		{
			ActiveAxis = Axis;
			ActiveRadius = Radius;
		}
	}
	if (Context.m_activeId != Im3d::Id_Invalid && PreviousActiveId != Context.m_activeId)
	{
		Context.m_gizmoStateMat3 = Rotation;
	}
	if (Context.m_activeId == Im3d::Id_Invalid)
	{
		State.Reset();
	}
	else
	{
		if (State.ActiveId != Context.m_activeId)
		{
			State.ActiveId = Context.m_activeId;
			const Im3d::Vec3 Radial = Context.m_gizmoStateVec3 - ActiveAxis * Im3d::Dot(Context.m_gizmoStateVec3, ActiveAxis);
			State.InitialRadial = Im3d::Length2(Radial) > 0.000001f ? Im3d::Normalize(Radial) : Im3d::AlignZ(ActiveAxis).getCol(0);
			AppliedAngle = 0.0f;
		}
		State.AngleDegrees = AppliedAngle * 180.0f / std::numbers::pi_v<float>;
		const Im3d::Vec3 Start = Translation + State.InitialRadial * ActiveRadius;
		const Im3d::Vec3 Current = Translation + Im3d::Rotation(ActiveAxis, AppliedAngle) * State.InitialRadial * ActiveRadius;
		Im3d::DrawLine(Translation, Start, Context.m_gizmoSizePixels * 0.5f, Im3d::Color_White);
		Im3d::DrawLine(Translation, Current, Context.m_gizmoSizePixels * 0.5f, Im3d::Color_Yellow);
		const int Segments = std::clamp(static_cast<int>(std::ceil(std::abs(AppliedAngle) * 36.0f / std::numbers::pi_v<float>)), 1, 72);
		Im3d::PushColor(Im3d::Color(1.0f, 1.0f, 0.0f, 0.18f));
		Im3d::BeginTriangles();
		Im3d::Vec3 Previous = Start;
		for (int Segment = 1; Segment <= Segments; ++Segment)
		{
			const float Angle = AppliedAngle * static_cast<float>(Segment) / static_cast<float>(Segments);
			const Im3d::Vec3 Next = Translation + Im3d::Rotation(ActiveAxis, Angle) * State.InitialRadial * ActiveRadius;
			Im3d::Vertex(Translation);
			Im3d::Vertex(Previous);
			Im3d::Vertex(Next);
			Previous = Next;
		}
		Im3d::End();
		Im3d::PopColor();
	}
	Im3d::PopSize();
	Context.popEnableSorting();
	Context.popMatrix();
	Context.popId();
	return bChanged;
}
}
