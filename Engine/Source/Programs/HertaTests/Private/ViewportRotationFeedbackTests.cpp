#include "ViewportRotationFeedback.h"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <im3d_math.h>
#include <numbers>
#include <utility>

namespace Herta
{
namespace
{
struct FRotationFeedbackTestContext final
{
	Im3d::Context Context;
	Im3d::Context& Previous = Im3d::GetContext();
	Im3d::Mat3 Rotation{1.0f};
	Im3d::Mat3 InitialBasis{1.0f};
	FViewportRotationFeedbackState Feedback;
	bool bLocal = false;

	FRotationFeedbackTestContext()
	{
		Im3d::SetContext(Context);
		Context.m_gizmoMode = Im3d::GizmoMode_Rotation;
		Context.m_gizmoHeightPixels = 80.0f;
		Context.m_gizmoSizePixels = 4.0f;
		Im3d::AppData& AppData = Context.getAppData();
		AppData.m_viewOrigin = {0.0f, 0.0f, -5.0f};
		AppData.m_viewDirection = {0.0f, 0.0f, 1.0f};
		AppData.m_cursorRayOrigin = AppData.m_viewOrigin;
		AppData.m_viewportSize = {960.0f, 540.0f};
		AppData.m_projScaleY = 1.1547005f;
	}

	~FRotationFeedbackTestContext()
	{
		Im3d::SetContext(Previous);
	}

	void Frame(const float DragDegrees, const bool bSelect)
	{
		const float Angle = (45.0f + DragDegrees) * std::numbers::pi_v<float> / 180.0f;
		const float Radius = Context.pixelsToWorldSize(Im3d::Vec3(0.0f), Context.m_gizmoHeightPixels) * 0.9f;
		const Im3d::Vec3 Target = InitialBasis * Im3d::Vec3(std::cos(Angle) * Radius, std::sin(Angle) * Radius, 0.0f);
		Context.getAppData().m_cursorRayDirection = Im3d::Normalize(Target - Context.getAppData().m_cursorRayOrigin);
		Context.getAppData().m_keyDown[Im3d::Mouse_Left] = bSelect;
		Im3d::NewFrame();
		DrawPreviewRotationGizmo(Im3d::Vec3(0.0f), Rotation, bLocal, Feedback);
		Im3d::EndFrame();
	}

	void BeginDrag()
	{
		Frame(0.0f, false);
		REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
		Frame(0.0f, true);
		REQUIRE(Im3d::GetActiveId() != Im3d::Id_Invalid);
		REQUIRE(Feedback.AngleDegrees.has_value());
		CHECK(*Feedback.AngleDegrees == doctest::Approx(0.0f));
	}
};

void CheckFiniteRotationFeedbackGeometry()
{
	for (std::uint32_t ListIndex = 0; ListIndex < Im3d::GetDrawListCount(); ++ListIndex)
	{
		const Im3d::DrawList& List = Im3d::GetDrawLists()[ListIndex];
		for (std::uint32_t VertexIndex = 0; VertexIndex < List.m_vertexCount; ++VertexIndex)
		{
			const Im3d::Vec4 Position = List.m_vertexData[VertexIndex].m_positionSize;
			CHECK(std::isfinite(Position.x));
			CHECK(std::isfinite(Position.y));
			CHECK(std::isfinite(Position.z));
			CHECK(std::abs(Position.x) < 2.0f);
			CHECK(std::abs(Position.y) < 2.0f);
			CHECK(std::abs(Position.z) < 2.0f);
		}
	}
}
}

TEST_CASE("Viewport rotation feedback reports signed native drag angles and bounded translucent wedges")
{
	for (const float Sign : {-1.0f, 1.0f})
	{
		FRotationFeedbackTestContext Gizmo;
		Gizmo.BeginDrag();
		Gizmo.Frame(Sign * 90.0f, true);
		REQUIRE(Gizmo.Feedback.AngleDegrees.has_value());
		CHECK(*Gizmo.Feedback.AngleDegrees == doctest::Approx(Sign * 90.0f));
		CHECK(Gizmo.Rotation(1, 0) == doctest::Approx(Sign));
		bool bHasTranslucentWedge = false;
		for (std::uint32_t Index = 0; Index < Im3d::GetDrawListCount(); ++Index)
		{
			const Im3d::DrawList& List = Im3d::GetDrawLists()[Index];
			bHasTranslucentWedge |= List.m_primType == Im3d::DrawPrimitive_Triangles && List.m_vertexCount > 0 && List.m_vertexData[0].m_color.getA() > 0.0f && List.m_vertexData[0].m_color.getA() < 0.5f;
		}
		CHECK(bHasTranslucentWedge);
		CheckFiniteRotationFeedbackGeometry();
		Gizmo.Frame(Sign * 90.0f, false);
		CHECK_FALSE(Gizmo.Feedback.AngleDegrees.has_value());
		CHECK(Gizmo.Feedback.ActiveId == Im3d::Id_Invalid);
	}
}

TEST_CASE("Viewport rotation feedback uses native snapping and principal angle wrap")
{
	for (const auto& [Input, Expected] : {std::pair{22.0f, 15.0f}, std::pair{-22.0f, -30.0f}})
	{
		FRotationFeedbackTestContext Gizmo;
		Gizmo.Context.getAppData().m_snapRotation = 15.0f * std::numbers::pi_v<float> / 180.0f;
		Gizmo.BeginDrag();
		Gizmo.Frame(Input, true);
		REQUIRE(Gizmo.Feedback.AngleDegrees.has_value());
		CHECK(*Gizmo.Feedback.AngleDegrees == doctest::Approx(Expected));
		CHECK(Gizmo.Rotation(1, 0) == doctest::Approx(std::sin(Expected * std::numbers::pi_v<float> / 180.0f)));
	}
	FRotationFeedbackTestContext Gizmo;
	Gizmo.BeginDrag();
	Gizmo.Frame(170.0f, true);
	REQUIRE(Gizmo.Feedback.AngleDegrees.has_value());
	CHECK(*Gizmo.Feedback.AngleDegrees == doctest::Approx(170.0f));
	Gizmo.Frame(190.0f, true);
	REQUIRE(Gizmo.Feedback.AngleDegrees.has_value());
	CHECK(*Gizmo.Feedback.AngleDegrees == doctest::Approx(-170.0f));
	Gizmo.Context.resetId();
	Gizmo.Frame(190.0f, false);
	CHECK_FALSE(Gizmo.Feedback.AngleDegrees.has_value());
}

TEST_CASE("Viewport rotation feedback tracks local axes and remains finite in edge-on views")
{
	FRotationFeedbackTestContext Gizmo;
	Gizmo.bLocal = true;
	Gizmo.InitialBasis = Im3d::Rotation({1.0f, 0.0f, 0.0f}, 0.5f);
	Gizmo.Rotation = Gizmo.InitialBasis;
	const Im3d::Vec3 Axis = Gizmo.InitialBasis.getCol(2);
	Gizmo.Context.getAppData().m_viewOrigin = Axis * -5.0f;
	Gizmo.Context.getAppData().m_cursorRayOrigin = Gizmo.Context.getAppData().m_viewOrigin;
	Gizmo.Context.getAppData().m_viewDirection = Axis;
	Gizmo.BeginDrag();
	Gizmo.Frame(90.0f, true);
	REQUIRE(Gizmo.Feedback.AngleDegrees.has_value());
	CHECK(*Gizmo.Feedback.AngleDegrees == doctest::Approx(90.0f));
	CheckFiniteRotationFeedbackGeometry();
	Gizmo.Context.resetId();
	Gizmo.bLocal = false;
	Gizmo.Context.getAppData().m_viewOrigin = {5.0f, 0.0f, 0.0f};
	Gizmo.Context.getAppData().m_cursorRayOrigin = Gizmo.Context.getAppData().m_viewOrigin;
	Gizmo.Context.getAppData().m_viewDirection = {-1.0f, 0.0f, 0.0f};
	Gizmo.Frame(0.0f, false);
	CHECK_FALSE(Gizmo.Feedback.AngleDegrees.has_value());
	CheckFiniteRotationFeedbackGeometry();
}
}
