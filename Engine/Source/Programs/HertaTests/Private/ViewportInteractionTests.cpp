#include "Herta/EditorFramework/ViewportInteraction.h"
#include "ViewportGizmos.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <im3d.h>
#include <im3d_math.h>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
struct FViewportGizmoTestContext final
{
	Im3d::Context Context;
	Im3d::Context& Previous = Im3d::GetContext();
	Im3d::Vec3 Translation{0.0f};
	Im3d::Mat3 Rotation{1.0f};
	Im3d::Vec3 Scale{1.0f};
	bool bCustomTranslation = false;
	bool bLocal = false;

	FViewportGizmoTestContext()
	{
		Im3d::SetContext(Context);
		Context.m_gizmoMode = Im3d::GizmoMode_Translation;
		Im3d::AppData& AppData = Im3d::GetAppData();
		AppData.m_viewOrigin = {0.0f, 0.0f, -5.0f};
		AppData.m_viewDirection = {0.0f, 0.0f, 1.0f};
		AppData.m_viewportSize = {960.0f, 540.0f};
		AppData.m_projScaleY = 1.1547005f;
		AppData.m_snapTranslation = 0.5f;
		AppData.m_cursorRayOrigin = AppData.m_viewOrigin;
	}

	~FViewportGizmoTestContext()
	{
		Im3d::SetContext(Previous);
	}

	void Frame(const float CursorX, const bool bSelect)
	{
		FrameRay({CursorX, 0.0f, 5.0f}, bSelect);
	}

	void FrameRay(const Im3d::Vec3 Direction, const bool bSelect)
	{
		const float Magnitude = std::hypot(Direction.x, Direction.y, Direction.z);
		Im3d::GetAppData().m_cursorRayDirection = {Direction.x / Magnitude, Direction.y / Magnitude, Direction.z / Magnitude};
		Im3d::GetAppData().m_keyDown[Im3d::Mouse_Left] = bSelect;
		Im3d::NewFrame();
		if (bCustomTranslation)
		{
			DrawPreviewTranslationGizmo(Translation, Rotation, bLocal);
		}
		else
		{
			Im3d::Gizmo("PreviewCube", Translation, Rotation, Scale);
		}
		Im3d::EndFrame();
	}
};
}

TEST_CASE("Viewport drags require the image and keep their original owner")
{
	FViewportInteractionState State;
	FViewportInteractionInput Input;
	Input.bWindowFocused = true;
	Input.MouseClicked[1] = true;
	Input.MouseDown[1] = true;
	State.Update(Input);
	CHECK(State.DragButton == -1);
	CHECK_FALSE(State.bKeyboardFocus);

	Input.bImageHovered = true;
	Input.bImageActive = true;
	State.Update(Input);
	CHECK(State.CameraMode == EViewportCameraMode::Fly);
	CHECK(State.DragButton == 1);
	CHECK(State.bKeyboardFocus);

	Input.bImageHovered = false;
	Input.bAlt = true;
	Input.MouseClicked.fill(false);
	State.Update(Input);
	CHECK(State.CameraMode == EViewportCameraMode::Fly);

	Input.MouseDown.fill(false);
	State.Update(Input);
	CHECK(State.CameraMode == EViewportCameraMode::None);
	CHECK(State.DragButton == -1);
	CHECK(State.bKeyboardFocus);
}

TEST_CASE("Viewport focus loss popups and competing widgets cancel ownership")
{
	for (const int Cancellation : {0, 1, 2})
	{
		FViewportInteractionState State;
		FViewportInteractionInput Input;
		Input.bImageHovered = true;
		Input.bImageActive = true;
		Input.bWindowFocused = true;
		Input.MouseClicked[0] = true;
		Input.MouseDown[0] = true;
		Input.bAlt = true;
		State.Update(Input);
		REQUIRE(State.CameraMode == EViewportCameraMode::Orbit);
		Input.MouseClicked.fill(false);
		Input.bWindowFocused = Cancellation != 0;
		Input.bInputBlocked = Cancellation == 1;
		Input.bImageActive = Cancellation != 2;
		State.Update(Input);
		CHECK(State.CameraMode == EViewportCameraMode::None);
		CHECK(State.DragButton == -1);
		if (Cancellation != 2)
		{
			CHECK_FALSE(State.bKeyboardFocus);
		}
		Input.bWindowFocused = true;
		Input.bInputBlocked = false;
		Input.bImageActive = true;
		State.Update(Input);
		CHECK(State.DragButton == -1);
	}
}

TEST_CASE("Viewport camera chords do not steal an owned gizmo drag")
{
	FViewportInteractionState State;
	FViewportInteractionInput Input;
	Input.bWindowFocused = true;
	Input.bImageHovered = true;
	Input.bImageActive = true;
	Input.MouseClicked[0] = true;
	Input.MouseDown[0] = true;
	State.Update(Input);
	CHECK(State.DragButton == 0);
	CHECK(State.CameraMode == EViewportCameraMode::None);
	Input.MouseClicked.fill(false);
	Input.MouseClicked[1] = true;
	Input.MouseDown[1] = true;
	Input.bAlt = true;
	State.Update(Input);
	CHECK(State.DragButton == 0);
	CHECK(State.CameraMode == EViewportCameraMode::None);

	State.Cancel();
	Input.MouseClicked.fill(false);
	Input.MouseDown.fill(false);
	Input.MouseClicked[2] = true;
	Input.MouseDown[2] = true;
	State.Update(Input);
	CHECK(State.CameraMode == EViewportCameraMode::Pan);
	State.Cancel();
	Input.MouseClicked.fill(false);
	Input.MouseDown.fill(false);
	Input.MouseClicked[1] = true;
	Input.MouseDown[1] = true;
	State.Update(Input);
	CHECK(State.CameraMode == EViewportCameraMode::Dolly);
}

TEST_CASE("Viewport gizmo pixels preserve UI scale after image downsampling")
{
	for (const float InterfaceScale : {1.0f, 1.5f, 2.0f})
	{
		constexpr float ImageHeight = 600.0f;
		const float FramebufferHeight = ImageHeight * InterfaceScale;
		const float GizmoScale = GetViewportGizmoPixelScale(InterfaceScale, ImageHeight, FramebufferHeight);
		CHECK(80.0f * GizmoScale * ImageHeight / FramebufferHeight == doctest::Approx(80.0f * InterfaceScale));
		CHECK(4.0f * GizmoScale * ImageHeight / FramebufferHeight == doctest::Approx(4.0f * InterfaceScale));
	}
	CHECK(80.0f * GetViewportGizmoPixelScale(2.0f, 3000.0f, 4096.0f) * 3000.0f / 4096.0f == doctest::Approx(160.0f));
}

TEST_CASE("Viewport im3d gizmo hits drags snaps and releases")
{
	FViewportGizmoTestContext Gizmo;
	Gizmo.Frame(0.6f, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.Frame(0.6f, true);
	REQUIRE(Im3d::GetActiveId() != Im3d::Id_Invalid);
	Gizmo.Frame(1.13f, true);
	CHECK(Gizmo.Translation.x == doctest::Approx(0.5f));
	CHECK(Gizmo.Translation.y == doctest::Approx(0.0f));
	CHECK(Gizmo.Translation.z == doctest::Approx(0.0f));
	Gizmo.Frame(1.13f, false);
	CHECK(Im3d::GetActiveId() == Im3d::Id_Invalid);

	Gizmo.Context.resetId();
	// The moved X handle flips toward the camera.
	Gizmo.Frame(-0.1f, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.Frame(-0.1f, true);
	REQUIRE(Im3d::GetActiveId() != Im3d::Id_Invalid);
	Gizmo.Context.resetId();
	Gizmo.Frame(2.0f, false);
	CHECK(Im3d::GetActiveId() == Im3d::Id_Invalid);
	CHECK(Gizmo.Translation.x == doctest::Approx(0.5f));
}

TEST_CASE("Viewport im3d stale hot plane does not own a parallel empty click")
{
	FViewportGizmoTestContext Gizmo;
	Im3d::GetAppData().m_viewOrigin = {0.0f, 1.0f, -5.0f};
	Im3d::GetAppData().m_cursorRayOrigin = Im3d::GetAppData().m_viewOrigin;
	Gizmo.FrameRay({0.43f, -1.0f, 4.57f}, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	const Im3d::Id PreviouslyHotId = Gizmo.Context.m_hotId;

	Gizmo.FrameRay({2.0f, 0.0f, 1.0f}, true);
	CHECK(Im3d::GetActiveId() == Im3d::Id_Invalid);
	CHECK(Gizmo.Context.m_hotId == PreviouslyHotId);
	CHECK(Im3d::GetHotId() != Im3d::Id_Invalid);
	CHECK(Gizmo.Translation.x == doctest::Approx(0.0f));
	CHECK(Gizmo.Translation.y == doctest::Approx(0.0f));
	CHECK(Gizmo.Translation.z == doctest::Approx(0.0f));
}

TEST_CASE("Viewport translation plane is a small solid square with matching world hit bounds")
{
	FViewportGizmoTestContext Gizmo;
	Gizmo.bCustomTranslation = true;
	Gizmo.Context.m_gizmoHeightPixels = 80.0f;
	Gizmo.Context.m_gizmoSizePixels = 4.0f;
	const float Center = Gizmo.Context.pixelsToWorldSize(Im3d::Vec3(0.0f), 80.0f) * 0.60f;
	Gizmo.FrameRay({Center + 0.1f, Center, 5.0f}, false);
	CHECK(Im3d::GetHotId() == Im3d::Id_Invalid);
	float MinimumX = std::numeric_limits<float>::max();
	float MaximumX = std::numeric_limits<float>::lowest();
	std::size_t BluePlaneVertices = 0;
	for (std::uint32_t Index = 0; Index < Im3d::GetDrawListCount(); ++Index)
	{
		const Im3d::DrawList& List = Im3d::GetDrawLists()[Index];
		if (List.m_primType != Im3d::DrawPrimitive_Triangles)
		{
			continue;
		}
		for (std::uint32_t VertexIndex = 0; VertexIndex < List.m_vertexCount; ++VertexIndex)
		{
			const Im3d::VertexData& Vertex = List.m_vertexData[VertexIndex];
			if (Vertex.m_color.v == Im3d::Color_Blue.v && Vertex.m_positionSize.x > 0.2f && Vertex.m_positionSize.y > 0.2f)
			{
				MinimumX = std::min(MinimumX, Vertex.m_positionSize.x);
				MaximumX = std::max(MaximumX, Vertex.m_positionSize.x);
				++BluePlaneVertices;
			}
		}
	}
	REQUIRE(BluePlaneVertices == 6);
	CHECK(Gizmo.Context.worldSizeToPixels(Im3d::Vec3(0.0f), MaximumX - MinimumX) == doctest::Approx(12.0f));
	Gizmo.FrameRay({Center, Center, 5.0f}, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.FrameRay({Center, Center, 5.0f}, true);
	REQUIRE(Im3d::GetActiveId() != Im3d::Id_Invalid);
	Gizmo.FrameRay({Center + 0.53f, Center + 0.53f, 5.0f}, true);
	CHECK(Gizmo.Translation.x == doctest::Approx(0.5f));
	CHECK(Gizmo.Translation.y == doctest::Approx(0.5f));
	CHECK(Gizmo.Translation.z == doctest::Approx(0.0f));
	Gizmo.FrameRay({Center + 0.53f, Center + 0.53f, 5.0f}, false);
	CHECK(Im3d::GetActiveId() == Im3d::Id_Invalid);
}

TEST_CASE("Viewport local translation plane hit bounds rotate with the drawn square")
{
	FViewportGizmoTestContext Gizmo;
	Gizmo.bCustomTranslation = true;
	Gizmo.bLocal = true;
	Gizmo.Context.m_gizmoHeightPixels = 80.0f;
	Gizmo.Context.m_gizmoSizePixels = 4.0f;
	Gizmo.Rotation = Im3d::Rotation({0.0f, 0.0f, 1.0f}, std::numbers::pi_v<float> * 0.25f);
	const float Center = Gizmo.Context.pixelsToWorldSize(Im3d::Vec3(0.0f), 80.0f) * 0.60f;
	const Im3d::Vec3 Inside = Gizmo.Rotation * Im3d::Vec3(Center + 0.06f, Center + 0.06f, 0.0f);
	Gizmo.FrameRay({Inside.x, Inside.y, 5.0f}, false);
	REQUIRE(Im3d::GetHotId() != Im3d::Id_Invalid);
	Gizmo.Context.resetId();
	const Im3d::Vec3 Outside = Gizmo.Rotation * Im3d::Vec3(Center + 0.09f, Center, 0.0f);
	Gizmo.FrameRay({Outside.x, Outside.y, 5.0f}, false);
	CHECK(Im3d::GetHotId() == Im3d::Id_Invalid);
}

TEST_CASE("Viewport translation drag keeps geometry local instead of drawing an infinite guide")
{
	FViewportGizmoTestContext Gizmo;
	Gizmo.bCustomTranslation = true;
	Gizmo.Context.m_gizmoHeightPixels = 80.0f;
	Gizmo.Context.m_gizmoSizePixels = 4.0f;
	Gizmo.Frame(0.6f, false);
	Gizmo.Frame(0.6f, true);
	REQUIRE(Im3d::GetActiveId() != Im3d::Id_Invalid);
	Gizmo.Frame(1.13f, true);
	CHECK(Gizmo.Translation.x == doctest::Approx(0.5f));
	for (std::uint32_t Index = 0; Index < Im3d::GetDrawListCount(); ++Index)
	{
		const Im3d::DrawList& List = Im3d::GetDrawLists()[Index];
		for (std::uint32_t VertexIndex = 0; VertexIndex < List.m_vertexCount; ++VertexIndex)
		{
			const auto& Position = List.m_vertexData[VertexIndex].m_positionSize;
			CHECK(std::hypot(Position.x, Position.y, Position.z) < 5.0f);
		}
	}
}
}
