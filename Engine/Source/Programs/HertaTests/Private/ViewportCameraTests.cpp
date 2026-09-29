#include "Herta/EditorCore/ViewportCamera.h"

#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <limits>

namespace Herta
{
namespace
{
constexpr FVector2 CameraViewportSize{1280.0f, 720.0f};
constexpr float CameraTolerance = 1.0e-4f;

void CheckCameraVector(const FVector3& Actual, const FVector3& Expected)
{
	CHECK(Actual.IsNearlyEqual(Expected, CameraTolerance));
}

FVector3 ProjectCameraPoint(const FViewportCameraSnapshot& Camera, const FVector3& Position)
{
	const FVector4 Clip = Camera.Projection * Camera.View * FVector4{Position, 1.0f};
	return {Clip.X / Clip.W, Clip.Y / Clip.W, Clip.Z / Clip.W};
}
}

TEST_CASE("Viewport camera starts looking at the origin with reversed infinite depth")
{
	const FViewportCameraController Camera;
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(1.0f);
	CHECK(Snapshot.Projection(1, 1) == doctest::Approx(1.56968558f));
	CheckCameraVector(Snapshot.Position, {0.0f, 2.0f, -10.0f});
	CheckCameraVector(Snapshot.View.TransformPosition(Snapshot.Position), FVector3::Zero());
	CheckCameraVector(Snapshot.View.TransformPosition(FVector3::Zero()), {0.0f, 0.0f, Snapshot.Position.Length()});
	const FVector3 LookDirection = Camera.MakePickingRay({0.5f, 0.5f}, 1.0f).Direction;
	CheckCameraVector(LookDirection, (FVector3::Zero() - Snapshot.Position).Normalized());
	CHECK(ProjectCameraPoint(Snapshot, Snapshot.Position + LookDirection * 0.1f).Z == doctest::Approx(1.0f));
	CHECK(ProjectCameraPoint(Snapshot, Snapshot.Position + LookDirection * 100'000.0f).Z == doctest::Approx(0.000001f));
}

TEST_CASE("Viewport camera does not jump on the first orbit frame")
{
	FViewportCameraController Camera;
	const FVector3 InitialPosition = Camera.GetSnapshot(1.0f).Position;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Orbit;
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.0f).Position, InitialPosition);
}

TEST_CASE("Viewport fly movement uses the camera basis and normalizes diagonal speed")
{
	FViewportCameraController Camera;
	const FVector3 InitialPosition = Camera.GetSnapshot(1.0f).Position;
	Camera.SetMovementSpeed(2.0f);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.Movement = {1.0f, 1.0f, 1.0f};
	Camera.Update(Input, 0.5f, CameraViewportSize);
	const FVector3 Offset = Camera.GetSnapshot(1.0f).Position - InitialPosition;
	CHECK(Offset.Length() == doctest::Approx(1.0f));
	CHECK(Offset.X > 0.0f);
	CHECK(Offset.Y > 0.0f);
	CHECK(Offset.Z > 0.0f);

	Input.Movement = FVector3::Up();
	Input.bFast = true;
	const FVector3 BeforeFast = Camera.GetSnapshot(1.0f).Position;
	Camera.Update(Input, 0.5f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.0f).View.TransformVector(Camera.GetSnapshot(1.0f).Position - BeforeFast), FVector3::Up() * 4.0f);
}

TEST_CASE("Viewport mouse look follows screen directions without frame-time scaling")
{
	FViewportCameraController Camera;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {100.0f, 100.0f};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	const FVector3 Direction = Camera.MakePickingRay({0.5f, 0.5f}, 1.0f).Direction;
	CHECK(Direction.X < 0.0f);
	CHECK(Direction.Y < 0.0f);
	CHECK(Direction.Z > 0.0f);
	CHECK(Direction.Length() == doctest::Approx(1.0f));

	Input.MouseDeltaPixels = {};
	Input.Movement = FVector3::Forward();
	const FVector3 Before = Camera.GetSnapshot(1.0f).Position;
	Camera.Update(Input, 0.25f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.0f).Position - Before, Direction * 1.25f);
}

TEST_CASE("Viewport orbit keeps the pivot centered and preserves distance")
{
	FViewportCameraController Camera;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {150.0f, -90.0f};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(1.0f);
	CHECK(Snapshot.Position.Length() == doctest::Approx(FVector3{0.0f, 2.0f, -10.0f}.Length()));
	CheckCameraVector(Snapshot.View.TransformPosition(FVector3::Zero()), {0.0f, 0.0f, Snapshot.Position.Length()});
	CHECK(Snapshot.Position.X > 0.0f);
	CHECK(Snapshot.Position.Y < 0.0f);

	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {50.0f, 30.0f};
	Input.Movement = FVector3::Forward();
	Camera.Update(Input, 0.1f, CameraViewportSize);
	const FVector3 BeforeOrbit = Camera.GetSnapshot(1.0f).Position;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {};
	Input.Movement = {};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.0f).Position, BeforeOrbit);
}

TEST_CASE("Viewport pan keeps the scene attached to the mouse at pivot depth")
{
	FViewportCameraController Camera;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Pan;
	Input.MouseDeltaPixels = {128.0f, 72.0f};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	const FVector3 ProjectedOrigin = ProjectCameraPoint(Camera.GetSnapshot(1280.0f / 720.0f), FVector3::Zero());
	CHECK(ProjectedOrigin.X == doctest::Approx(0.2f));
	CHECK(ProjectedOrigin.Y == doctest::Approx(-0.2f));
	const FVector3 BeforeOrbit = Camera.GetSnapshot(1.0f).Position;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.0f).Position, BeforeOrbit);
}

TEST_CASE("Viewport dolly moves toward the pivot and cannot pass it")
{
	FViewportCameraController Camera;
	const float InitialDistance = Camera.GetSnapshot(1.0f).Position.Length();
	FViewportCameraInput Input;
	Input.ScrollDelta = 1.0f;
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CHECK(Camera.GetSnapshot(1.0f).Position.Length() < InitialDistance);
	CHECK(Camera.GetSnapshot(1.0f).Position.Length() > 0.0f);

	Input.Mode = EViewportCameraMode::Dolly;
	Input.ScrollDelta = 0.0f;
	Input.MouseDeltaPixels = {0.0f, 100.0f};
	const float BeforeDrag = Camera.GetSnapshot(1.0f).Position.Length();
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CHECK(Camera.GetSnapshot(1.0f).Position.Length() > BeforeDrag);
	Input.MouseDeltaPixels = {0.0f, -100'000.0f};
	Camera.Update(Input, 0.0f, CameraViewportSize);
	CHECK(Camera.GetSnapshot(1.0f).Position.Length() == doctest::Approx(0.2f));
}

TEST_CASE("Viewport focus frames bounds at wide and tall aspect ratios")
{
	for (const float AspectRatio : std::array{0.25f, 1.0f, 4.0f})
	{
		FViewportCameraController Camera;
		FViewportCameraInput Input;
		Input.Mode = EViewportCameraMode::Orbit;
		Input.MouseDeltaPixels = {200.0f, -100.0f};
		Camera.Update(Input, 0.0f, CameraViewportSize);
		const FVector3 Center{10.0f, -2.0f, 7.0f};
		const FVector3 HalfExtent{2.0f, 3.0f, 4.0f};
		Camera.Focus(Center, HalfExtent, AspectRatio);
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio);
		const FVector3 ProjectedCenter = ProjectCameraPoint(Snapshot, Center);
		CHECK(std::abs(ProjectedCenter.X) < CameraTolerance);
		CHECK(std::abs(ProjectedCenter.Y) < CameraTolerance);
		for (const float X : std::array{-1.0f, 1.0f})
		{
			for (const float Y : std::array{-1.0f, 1.0f})
			{
				for (const float Z : std::array{-1.0f, 1.0f})
				{
					const FVector3 Ndc = ProjectCameraPoint(Snapshot, Center + HalfExtent.ComponentMultiply({X, Y, Z}));
					CHECK(std::abs(Ndc.X) <= 1.0f);
					CHECK(std::abs(Ndc.Y) <= 1.0f);
					CHECK(Ndc.Z > 0.0f);
					CHECK(Ndc.Z < 1.0f);
				}
			}
		}
	}
}

TEST_CASE("Viewport focus fits bounds inside an off-center visible pane")
{
	constexpr FVector2 ProjectionCenter{0.4f, 0.4f};
	const FVector3 Center{6.0f, -3.0f, 9.0f};
	const FVector3 BoundsHalfExtent{1.5f, 2.0f, 1.0f};
	for (const FVector2 VisibleSize : std::array{FVector2{0.8f, 0.8f}, FVector2{0.35f, 0.5f}})
	{
		FViewportCameraController Camera;
		Camera.Focus(Center, BoundsHalfExtent, 16.0f / 9.0f, VisibleSize);
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(16.0f / 9.0f, ProjectionCenter);
		const FVector3 ProjectedCenter = ProjectCameraPoint(Snapshot, Center);
		CHECK(ProjectedCenter.X == doctest::Approx(2.0f * ProjectionCenter.X - 1.0f).epsilon(CameraTolerance));
		CHECK(ProjectedCenter.Y == doctest::Approx(1.0f - 2.0f * ProjectionCenter.Y).epsilon(CameraTolerance));
		const float MinimumX = 2.0f * (ProjectionCenter.X - VisibleSize.X * 0.5f) - 1.0f;
		const float MaximumX = 2.0f * (ProjectionCenter.X + VisibleSize.X * 0.5f) - 1.0f;
		const float MinimumY = 1.0f - 2.0f * (ProjectionCenter.Y + VisibleSize.Y * 0.5f);
		const float MaximumY = 1.0f - 2.0f * (ProjectionCenter.Y - VisibleSize.Y * 0.5f);
		for (const float X : std::array{-1.0f, 1.0f})
		{
			for (const float Y : std::array{-1.0f, 1.0f})
			{
				for (const float Z : std::array{-1.0f, 1.0f})
				{
					const FVector3 Ndc = ProjectCameraPoint(Snapshot, Center + BoundsHalfExtent.ComponentMultiply({X, Y, Z}));
					CHECK(Ndc.X >= MinimumX - CameraTolerance);
					CHECK(Ndc.X <= MaximumX + CameraTolerance);
					CHECK(Ndc.Y >= MinimumY - CameraTolerance);
					CHECK(Ndc.Y <= MaximumY + CameraTolerance);
				}
			}
		}
	}
}

TEST_CASE("Viewport picking rays match top-left screen coordinates and camera projection")
{
	FViewportCameraController Camera;
	CheckCameraVector(Camera.MakePickingRay({0.5f, 0.5f}, 1.0f).Direction, (FVector3::Zero() - Camera.GetSnapshot(1.0f).Position).Normalized());
	CHECK(Camera.MakePickingRay({0.0f, 0.0f}, 1.0f).Direction.X > 0.0f);
	CHECK(Camera.MakePickingRay({0.0f, 0.0f}, 1.0f).Direction.Y > 0.0f);
	CHECK(Camera.MakePickingRay({1.0f, 1.0f}, 1.0f).Direction.X < 0.0f);
	CHECK(Camera.MakePickingRay({1.0f, 1.0f}, 1.0f).Direction.Y < 0.0f);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {100.0f, -150.0f};
	Input.Movement = FVector3::Left();
	Camera.Update(Input, 0.25f, CameraViewportSize);
	for (const float AspectRatio : std::array{0.25f, 1.0f, 2.0f, 4.0f})
	{
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio);
		CHECK(Snapshot.Projection(0, 0) == doctest::Approx(-Snapshot.Projection(1, 1) / AspectRatio));
		for (const FVector2 ScreenPosition : std::array{FVector2{0.0f, 0.0f}, FVector2{0.5f, 0.5f}, FVector2{0.8f, 0.2f}, FVector2{1.0f, 1.0f}})
		{
			const FViewportPickingRay Ray = Camera.MakePickingRay(ScreenPosition, AspectRatio);
			const FVector3 Ndc = ProjectCameraPoint(Snapshot, Ray.Origin + Ray.Direction * 10.0f);
			CheckCameraVector(Ray.Origin, Snapshot.Position);
			CHECK(Ray.Direction.Length() == doctest::Approx(1.0f));
			CHECK(Ndc.X == doctest::Approx(2.0f * ScreenPosition.X - 1.0f).epsilon(CameraTolerance));
			CHECK(Ndc.Y == doctest::Approx(1.0f - 2.0f * ScreenPosition.Y).epsilon(CameraTolerance));
		}
	}
}

TEST_CASE("Viewport camera projection center offsets the origin and picking center")
{
	const FViewportCameraController Camera;
	const FVector2 ProjectionCenter{0.4f, 0.41f};
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(16.0f / 9.0f, ProjectionCenter);
	const FVector3 OriginNdc = ProjectCameraPoint(Snapshot, FVector3::Zero());
	CHECK(OriginNdc.X == doctest::Approx(2.0f * ProjectionCenter.X - 1.0f));
	CHECK(OriginNdc.Y == doctest::Approx(1.0f - 2.0f * ProjectionCenter.Y));
	CheckCameraVector(Camera.MakePickingRay(ProjectionCenter, 16.0f / 9.0f, ProjectionCenter).Direction, (FVector3::Zero() - Snapshot.Position).Normalized());
}

TEST_CASE("Viewport picking rays project back to normalized positions for off-center projections")
{
	const FViewportCameraController Camera;
	const FVector2 ProjectionCenter{0.37f, 0.62f};
	for (const float AspectRatio : std::array{0.3f, 1.0f, 2.8f})
	{
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio, ProjectionCenter);
		for (const FVector2 ScreenPosition : std::array{FVector2{0.08f, 0.13f}, ProjectionCenter, FVector2{0.91f, 0.8f}})
		{
			const FViewportPickingRay Ray = Camera.MakePickingRay(ScreenPosition, AspectRatio, ProjectionCenter);
			const FVector3 Ndc = ProjectCameraPoint(Snapshot, Ray.Origin + Ray.Direction * 10.0f);
			CHECK(Ndc.X == doctest::Approx(2.0f * ScreenPosition.X - 1.0f).epsilon(CameraTolerance));
			CHECK(Ndc.Y == doctest::Approx(1.0f - 2.0f * ScreenPosition.Y).epsilon(CameraTolerance));
		}
	}
}

TEST_CASE("Viewport camera projection center defaults, sanitizes nonfinite values, and clamps bounds")
{
	const FViewportCameraController Camera;
	const FVector2 ScreenPosition{0.23f, 0.76f};
	const FViewportCameraSnapshot DefaultSnapshot = Camera.GetSnapshot(1.5f);
	CHECK(Camera.GetSnapshot(1.5f, {std::numeric_limits<float>::quiet_NaN(), 0.2f}).Projection == DefaultSnapshot.Projection);
	CHECK(Camera.GetSnapshot(1.5f, {0.2f, std::numeric_limits<float>::infinity()}).Projection == DefaultSnapshot.Projection);
	CheckCameraVector(Camera.MakePickingRay(ScreenPosition, 1.5f).Direction, Camera.MakePickingRay(ScreenPosition, 1.5f, {std::numeric_limits<float>::quiet_NaN(), 0.2f}).Direction);
	CHECK(Camera.GetSnapshot(1.5f, {-1.0f, 1.5f}).Projection == Camera.GetSnapshot(1.5f, {0.0f, 1.0f}).Projection);
	CheckCameraVector(Camera.MakePickingRay(ScreenPosition, 1.5f, {-1.0f, 1.5f}).Direction, Camera.MakePickingRay(ScreenPosition, 1.5f, {0.0f, 1.0f}).Direction);
}

TEST_CASE("Viewport camera rejects invalid state and clamps editable settings")
{
	FViewportCameraController Camera;
	const FViewportCameraSnapshot Before = Camera.GetSnapshot(1.0f);
	Camera.Focus({10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, {0.0f, 1.0f});
	Camera.Focus({10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, {1.0f, 1.01f});
	Camera.Focus({10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, {std::numeric_limits<float>::quiet_NaN(), 0.5f});
	CHECK(Camera.GetSnapshot(1.0f).Position == Before.Position);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.Movement = FVector3::Forward();
	Input.MouseDeltaPixels.X = std::numeric_limits<float>::quiet_NaN();
	Camera.Update(Input, 1.0f, CameraViewportSize);
	Camera.Focus({0.0f, 0.0f, 0.0f}, {-1.0f, 1.0f, 1.0f}, 1.0f);
	CHECK(Camera.GetSnapshot(1.0f).Position == Before.Position);
	CHECK(Camera.GetSnapshot(1.0f).View == Before.View);
	CHECK(Camera.GetSnapshot(0.0f).Projection == Before.Projection);
	Camera.SetMovementSpeed(std::numeric_limits<float>::infinity());
	CHECK(Camera.GetMovementSpeed() == 5.0f);
	Camera.SetMovementSpeed(-1.0f);
	CHECK(Camera.GetMovementSpeed() > 0.0f);
	Camera.SetMouseSensitivity(-1.0f);
	CHECK(Camera.GetMouseSensitivity() > 0.0f);
	Camera.Focus({}, {}, 1.0f);
	CHECK(Camera.GetSnapshot(1.0f).Position.Length() == doctest::Approx(0.2f));
}
}
