#include "ViewportScaleGizmo.h"

#include <doctest/doctest.h>
#include <im3d_math.h>

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
struct FScaleGizmoTestContext
{
	Im3d::Context Context;
	Im3d::Context& Previous = Im3d::GetContext();
	Im3d::Vec3 Translation{0.f};
	Im3d::Mat3 Rotation{1.f};
	Im3d::Vec3 Scale{2.f, 3.f, 4.f};
	FViewportScaleGizmoState State;
	FViewportScaleGizmoFeedback Feedback;

	FScaleGizmoTestContext()
	{
		Im3d::SetContext(Context);
		Context.m_gizmoHeightPixels = 80.f;
		Context.m_gizmoSizePixels = 4.f;
		auto& AppData = Context.getAppData();
		AppData.m_viewOrigin = {0.f, 0.f, -5.f};
		AppData.m_viewDirection = {0.f, 0.f, 1.f};
		AppData.m_cursorRayOrigin = AppData.m_viewOrigin;
		AppData.m_viewportSize = {960.f, 540.f};
		AppData.m_projScaleY = 1.1547005f;
		AppData.m_snapScale = 0.25f;
	}

	~FScaleGizmoTestContext()
	{
		Im3d::SetContext(Previous);
	}

	FScaleGizmoTestContext(const FScaleGizmoTestContext&) = delete;
	FScaleGizmoTestContext& operator=(const FScaleGizmoTestContext&) = delete;
	FScaleGizmoTestContext(FScaleGizmoTestContext&&) = delete;
	FScaleGizmoTestContext& operator=(FScaleGizmoTestContext&&) = delete;

	float WorldHeight()
	{
		return Context.pixelsToWorldSize(Translation, Context.m_gizmoHeightPixels);
	}

	bool Frame(const Im3d::Vec3& Target, const bool bSelect)
	{
		auto& AppData = Context.getAppData();
		AppData.m_cursorRayDirection = Im3d::Normalize(Target - AppData.m_cursorRayOrigin);
		AppData.m_keyDown[Im3d::Mouse_Left] = bSelect;
		Im3d::NewFrame();
		const bool bChanged = DrawPreviewScaleGizmo(Translation, Rotation, Scale, State, Feedback);
		Im3d::EndFrame();
		return bChanged;
	}
};
}

TEST_CASE("Scale plane handles hit drag snap release and preserve starting ratio")
{
	FScaleGizmoTestContext Gizmo;
	const float Offset = Gizmo.WorldHeight() * 0.6f;
	const Im3d::Vec3 Center{Offset, Offset, 0.f};
	Gizmo.Frame({Gizmo.WorldHeight() * 0.45f, Gizmo.WorldHeight() * 0.45f, 0.f}, false);
	CHECK(Im3d::GetHotId() == Im3d::Id_Invalid);
	Gizmo.Frame(Center, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.Frame(Center, true);
	REQUIRE(Gizmo.Feedback.bActive);
	CHECK(Gizmo.Feedback.Handle == EViewportScaleHandle::XY);
	CHECK(Gizmo.Frame(Center * 1.4f, true));
	CHECK(Gizmo.Scale.x == doctest::Approx(3.f));
	CHECK(Gizmo.Scale.y == doctest::Approx(4.5f));
	CHECK(Gizmo.Scale.z == doctest::Approx(4.f));
	CHECK(Gizmo.Feedback.Factors.x == doctest::Approx(1.5f));
	CHECK(Gizmo.Feedback.Factors.y == doctest::Approx(1.5f));
	CHECK(Gizmo.Feedback.Factors.z == doctest::Approx(1.f));
	CHECK(Gizmo.Feedback.HandlePosition.x == doctest::Approx(Center.x * 1.5f));
	Im3d::U32 DrawnVertices = 0;
	for (Im3d::U32 Index = 0; Index < Im3d::GetDrawListCount(); ++Index)
	{
		DrawnVertices += Im3d::GetDrawLists()[Index].m_vertexCount;
	}

	CHECK(DrawnVertices == 6);
	Gizmo.Frame(Center * 1.4f, false);
	CHECK_FALSE(Gizmo.Feedback.bActive);
	CHECK(Gizmo.State.ActiveId == Im3d::Id_Invalid);
	CHECK(Im3d::GetActiveId() == Im3d::Id_Invalid);
	CHECK(Gizmo.Scale.x == doctest::Approx(3.f));
}

TEST_CASE("Scale plane hit bounds match rotated local squares")
{
	FScaleGizmoTestContext Gizmo;
	const float Diagonal = std::sqrt(0.5f);
	Gizmo.Rotation.setCol(0, {Diagonal, Diagonal, 0.f});
	Gizmo.Rotation.setCol(1, {-Diagonal, Diagonal, 0.f});
	const float Offset = Gizmo.WorldHeight() * 0.6f;
	const float HalfSize = Gizmo.WorldHeight() * (6.f / 80.f);
	const Im3d::Vec3 Center = Gizmo.Rotation * Im3d::Vec3{Offset, Offset, 0.f};
	const Im3d::Vec3 Inside = Center + Gizmo.Rotation * Im3d::Vec3{HalfSize * 0.9f, HalfSize * 0.9f, 0.f};
	Gizmo.Frame(Inside, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.Frame(Inside, true);
	CHECK(Gizmo.Feedback.Handle == EViewportScaleHandle::XY);
	Gizmo.Frame(Inside, false);
	Gizmo.Context.resetId();
	Gizmo.State.Reset();
	Gizmo.Frame(Center + Gizmo.Rotation * Im3d::Vec3{HalfSize * 1.1f, 0.f, 0.f}, false);
	CHECK(Im3d::GetHotId() == Im3d::Id_Invalid);
}

TEST_CASE("Scale plane clamps one factor for nonuniform components")
{
	FScaleGizmoTestContext Gizmo;
	Gizmo.Scale = {2.f, 500.f, 4.f};
	const float Offset = Gizmo.WorldHeight() * 0.6f;
	const Im3d::Vec3 Center{Offset, Offset, 0.f};
	Gizmo.Frame(Center, false);
	Gizmo.Frame(Center, true);
	REQUIRE(Gizmo.Feedback.Handle == EViewportScaleHandle::XY);
	Gizmo.Frame(Center * 5.f, true);
	CHECK(Gizmo.Scale.x == doctest::Approx(4.f));
	CHECK(Gizmo.Scale.y == doctest::Approx(1000.f));
	CHECK(Gizmo.Scale.z == doctest::Approx(4.f));
	CHECK(Gizmo.Feedback.Factors.x == doctest::Approx(2.f));
	CHECK(Gizmo.Feedback.Factors.y == doctest::Approx(2.f));
	Gizmo.Frame(Center * -5.f, true);
	CHECK(Gizmo.Scale.x == doctest::Approx(0.001f));
	CHECK(Gizmo.Scale.y == doctest::Approx(0.25f));
	CHECK(Gizmo.Feedback.Factors.x == doctest::Approx(Gizmo.Feedback.Factors.y));
}

TEST_CASE("Scale axis uses a relative factor and finite cube drawing without guides")
{
	FScaleGizmoTestContext Gizmo;
	const float Height = Gizmo.WorldHeight();
	Gizmo.Frame({Height, 0.f, 0.f}, false);
	Gizmo.Frame({Height, 0.f, 0.f}, true);
	REQUIRE(Gizmo.Feedback.Handle == EViewportScaleHandle::X);
	Gizmo.Frame({Height * 1.4f, 0.f, 0.f}, true);
	CHECK(Gizmo.Scale.x == doctest::Approx(3.f));
	CHECK(Gizmo.Scale.y == doctest::Approx(3.f));
	CHECK(Gizmo.Feedback.Factors.x == doctest::Approx(1.5f));
	CHECK(Gizmo.Feedback.HandlePosition.x == doctest::Approx(Height * 1.5f));
	for (Im3d::U32 ListIndex = 0; ListIndex < Im3d::GetDrawListCount(); ++ListIndex)
	{
		const Im3d::DrawList& List = Im3d::GetDrawLists()[ListIndex];
		for (Im3d::U32 VertexIndex = 0; VertexIndex < List.m_vertexCount; ++VertexIndex)
		{
			CHECK(List.m_vertexData[VertexIndex].m_color.v == Im3d::Color_Yellow.v);
			const Im3d::Vec4 Position = List.m_vertexData[VertexIndex].m_positionSize;
			CHECK(std::abs(Position.x) < 10.f);
			CHECK(std::abs(Position.y) < 10.f);
			CHECK(std::abs(Position.z) < 10.f);
		}
	}
}

TEST_CASE("Scale center preserves nonuniform ratios and clears feedback on cancellation")
{
	FScaleGizmoTestContext Gizmo;
	Gizmo.Frame({0.f}, false);
	Gizmo.Frame({0.f}, true);
	REQUIRE(Gizmo.Feedback.Handle == EViewportScaleHandle::Uniform);
	const float Distance = Gizmo.WorldHeight() * 0.4f / std::sqrt(2.f);
	Gizmo.Frame({-Distance, Distance, 0.f}, true);
	CHECK(Gizmo.Scale.x == doctest::Approx(3.f));
	CHECK(Gizmo.Scale.y == doctest::Approx(4.5f));
	CHECK(Gizmo.Scale.z == doctest::Approx(6.f));
	CHECK(Gizmo.Feedback.Factors.x == doctest::Approx(1.5f));
	CHECK(Gizmo.Feedback.Factors.z == doctest::Approx(1.5f));
	Gizmo.Context.resetId();
	Gizmo.State.Reset();
	Gizmo.Frame({-Distance, Distance, 0.f}, false);
	CHECK_FALSE(Gizmo.Feedback.bActive);
	CHECK(Gizmo.State.ActiveId == Im3d::Id_Invalid);
	CHECK(Gizmo.Scale.z == doctest::Approx(6.f));
}
}
