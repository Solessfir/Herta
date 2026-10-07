#include "Herta/EditorCore/ViewportCamera.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
constexpr FVector2 CameraViewportSize{1280.f, 720.f};
constexpr float CameraTolerance = 1.0e-4f;

void CheckCameraVector(const FVector3& Actual, const FVector3& Expected)
{
	CHECK(Actual.IsNearlyEqual(Expected, CameraTolerance));
}

FVector3 ProjectCameraPoint(const FViewportCameraSnapshot& Camera, const FVector3& Position)
{
	const FVector4 Clip = Camera.Projection * Camera.View * FVector4{Position, 1.f};
	return {Clip.X / Clip.W, Clip.Y / Clip.W, Clip.Z / Clip.W};
}
}

TEST_CASE("Viewport camera starts looking down at a pivot above the origin with reversed infinite depth")
{
	const FViewportCameraController Camera;
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(1.f);
	const FVector3 Pivot = Camera.GetPivot();
	CHECK(Snapshot.Projection(1, 1) == doctest::Approx(1.56968558f));
	CHECK(Camera.GetPitch() == doctest::Approx(-17.5f * std::numbers::pi_v<float> / 180.f));
	CHECK(Camera.GetYaw() == 0.f);
	CheckCameraVector(Snapshot.Position, {0.f, 4.f, -10.f});
	CheckCameraVector(Pivot, {0.f, Pivot.Y, 0.f});
	CHECK(Pivot.Y > 0.f);
	CHECK(Pivot.Y < Snapshot.Position.Y);
	CheckCameraVector(Snapshot.View.TransformPosition(Snapshot.Position), FVector3::Zero());
	CheckCameraVector(Snapshot.View.TransformPosition(Pivot), {0.f, 0.f, (Pivot - Snapshot.Position).Length()});
	const FVector3 LookDirection = Camera.MakePickingRay({0.5f, 0.5f}, 1.f).Direction;
	CheckCameraVector(LookDirection, (Pivot - Snapshot.Position).Normalized());
	CHECK(ProjectCameraPoint(Snapshot, Snapshot.Position + LookDirection * 0.1f).Z == doctest::Approx(1.f));
	CHECK(ProjectCameraPoint(Snapshot, Snapshot.Position + LookDirection * 100'000.f).Z == doctest::Approx(0.000001f));
}

TEST_CASE("Viewport camera does not jump on the first orbit frame")
{
	FViewportCameraController Camera;
	const FVector3 InitialPosition = Camera.GetSnapshot(1.f).Position;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Orbit;
	Camera.Update(Input, 0.f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.f).Position, InitialPosition);
}

TEST_CASE("Viewport fly movement uses the camera basis and normalizes diagonal speed")
{
	FViewportCameraController Camera;
	const FVector3 InitialPosition = Camera.GetSnapshot(1.f).Position;
	Camera.SetMovementSpeed(2.f);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.Movement = {1.f, 1.f, 1.f};
	Camera.Update(Input, 0.5f, CameraViewportSize);
	const FVector3 Offset = Camera.GetSnapshot(1.f).Position - InitialPosition;
	CHECK(Offset.Length() == doctest::Approx(1.f));
	CHECK(Offset.X > 0.f);
	CHECK(Offset.Y > 0.f);
	CHECK(Offset.Z > 0.f);

	Input.Movement = FVector3::Up();
	Input.bFast = true;
	const FVector3 BeforeFast = Camera.GetSnapshot(1.f).Position;
	Camera.Update(Input, 0.5f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.f).View.TransformVector(Camera.GetSnapshot(1.f).Position - BeforeFast), FVector3::Up() * 4.f);
}

TEST_CASE("Viewport mouse look follows screen directions without frame-time scaling")
{
	FViewportCameraController Camera;
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {100.f, 100.f};
	Camera.Update(Input, 0.f, CameraViewportSize);
	const FVector3 Direction = Camera.MakePickingRay({0.5f, 0.5f}, 1.f).Direction;
	CHECK(Direction.X < 0.f);
	CHECK(Direction.Y < 0.f);
	CHECK(Direction.Z > 0.f);
	CHECK(Direction.Length() == doctest::Approx(1.f));

	Input.MouseDeltaPixels = {};
	Input.Movement = FVector3::Forward();
	const FVector3 Before = Camera.GetSnapshot(1.f).Position;
	Camera.Update(Input, 0.25f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.f).Position - Before, Direction * 1.25f);
}

TEST_CASE("Viewport orbit keeps the pivot centered and preserves distance")
{
	FViewportCameraController Camera;
	const FVector3 Pivot = Camera.GetPivot();
	const float InitialDistance = (Camera.GetSnapshot(1.f).Position - Pivot).Length();
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {150.f, -180.f};
	Camera.Update(Input, 0.f, CameraViewportSize);
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(1.f);
	CHECK((Snapshot.Position - Pivot).Length() == doctest::Approx(InitialDistance));
	CheckCameraVector(Snapshot.View.TransformPosition(Pivot), {0.f, 0.f, InitialDistance});
	CHECK(Snapshot.Position.X > Pivot.X);
	CHECK(Snapshot.Position.Y < Pivot.Y);

	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {50.f, 30.f};
	Input.Movement = FVector3::Forward();
	Camera.Update(Input, 0.1f, CameraViewportSize);
	const FVector3 BeforeOrbit = Camera.GetSnapshot(1.f).Position;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {};
	Input.Movement = {};
	Camera.Update(Input, 0.f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.f).Position, BeforeOrbit);
}

TEST_CASE("Viewport pan keeps the scene attached to the mouse at pivot depth")
{
	FViewportCameraController Camera;
	const FVector3 InitialPivot = Camera.GetPivot();
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Pan;
	Input.MouseDeltaPixels = {128.f, 72.f};
	Camera.Update(Input, 0.f, CameraViewportSize);
	const FVector3 ProjectedPivot = ProjectCameraPoint(Camera.GetSnapshot(1280.f / 720.f), InitialPivot);
	CHECK(ProjectedPivot.X == doctest::Approx(0.2f));
	CHECK(ProjectedPivot.Y == doctest::Approx(-0.2f));
	const FVector3 BeforeOrbit = Camera.GetSnapshot(1.f).Position;
	Input.Mode = EViewportCameraMode::Orbit;
	Input.MouseDeltaPixels = {};
	Camera.Update(Input, 0.f, CameraViewportSize);
	CheckCameraVector(Camera.GetSnapshot(1.f).Position, BeforeOrbit);
}

TEST_CASE("Viewport dolly moves toward the pivot and cannot pass it")
{
	FViewportCameraController Camera;
	const auto GetPivotDistance = [&Camera]
	{
		return (Camera.GetSnapshot(1.f).Position - Camera.GetPivot()).Length();
	};

	const float InitialDistance = GetPivotDistance();
	FViewportCameraInput Input;
	Input.ScrollDelta = 1.f;
	Camera.Update(Input, 0.f, CameraViewportSize);
	CHECK(GetPivotDistance() < InitialDistance);
	CHECK(GetPivotDistance() > 0.f);

	Input.Mode = EViewportCameraMode::Dolly;
	Input.ScrollDelta = 0.f;
	Input.MouseDeltaPixels = {0.f, 100.f};
	const float BeforeDrag = GetPivotDistance();
	Camera.Update(Input, 0.f, CameraViewportSize);
	CHECK(GetPivotDistance() > BeforeDrag);
	Input.MouseDeltaPixels = {0.f, -100'000.f};
	Camera.Update(Input, 0.f, CameraViewportSize);
	CHECK(GetPivotDistance() == doctest::Approx(0.2f));
}

TEST_CASE("Viewport focus frames bounds at wide and tall aspect ratios")
{
	for (const float AspectRatio : std::array{0.25f, 1.f, 4.f})
	{
		FViewportCameraController Camera;
		FViewportCameraInput Input;
		Input.Mode = EViewportCameraMode::Orbit;
		Input.MouseDeltaPixels = {200.f, -100.f};
		Camera.Update(Input, 0.f, CameraViewportSize);
		const FVector3 Center{10.f, -2.f, 7.f};
		const FVector3 HalfExtent{2.f, 3.f, 4.f};
		Camera.Focus(Center, HalfExtent, AspectRatio);
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio);
		const FVector3 ProjectedCenter = ProjectCameraPoint(Snapshot, Center);
		CHECK(std::abs(ProjectedCenter.X) < CameraTolerance);
		CHECK(std::abs(ProjectedCenter.Y) < CameraTolerance);
		for (const float X : std::array{-1.f, 1.f})
		{
			for (const float Y : std::array{-1.f, 1.f})
			{
				for (const float Z : std::array{-1.f, 1.f})
				{
					const FVector3 Ndc = ProjectCameraPoint(Snapshot, Center + HalfExtent.ComponentMultiply({X, Y, Z}));
					CHECK(std::abs(Ndc.X) <= 1.f);
					CHECK(std::abs(Ndc.Y) <= 1.f);
					CHECK(Ndc.Z > 0.f);
					CHECK(Ndc.Z < 1.f);
				}
			}
		}
	}
}

TEST_CASE("Viewport focus fits bounds inside an off-center visible pane")
{
	constexpr FVector2 ProjectionCenter{0.4f, 0.4f};
	const FVector3 Center{6.f, -3.f, 9.f};
	const FVector3 BoundsHalfExtent{1.5f, 2.f, 1.f};
	for (const FVector2 VisibleSize : std::array{FVector2{0.8f, 0.8f}, FVector2{0.35f, 0.5f}})
	{
		FViewportCameraController Camera;
		Camera.Focus(Center, BoundsHalfExtent, 16.f / 9.f, VisibleSize);
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(16.f / 9.f, ProjectionCenter);
		const FVector3 ProjectedCenter = ProjectCameraPoint(Snapshot, Center);
		CHECK(ProjectedCenter.X == doctest::Approx(2.f * ProjectionCenter.X - 1.f).epsilon(CameraTolerance));
		CHECK(ProjectedCenter.Y == doctest::Approx(1.f - 2.f * ProjectionCenter.Y).epsilon(CameraTolerance));
		const float MinimumX = 2.f * (ProjectionCenter.X - VisibleSize.X * 0.5f) - 1.f;
		const float MaximumX = 2.f * (ProjectionCenter.X + VisibleSize.X * 0.5f) - 1.f;
		const float MinimumY = 1.f - 2.f * (ProjectionCenter.Y + VisibleSize.Y * 0.5f);
		const float MaximumY = 1.f - 2.f * (ProjectionCenter.Y - VisibleSize.Y * 0.5f);
		for (const float X : std::array{-1.f, 1.f})
		{
			for (const float Y : std::array{-1.f, 1.f})
			{
				for (const float Z : std::array{-1.f, 1.f})
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
	CheckCameraVector(Camera.MakePickingRay({0.5f, 0.5f}, 1.f).Direction, (Camera.GetPivot() - Camera.GetSnapshot(1.f).Position).Normalized());
	CHECK(Camera.MakePickingRay({0.f, 0.f}, 1.f).Direction.X > 0.f);
	CHECK(Camera.MakePickingRay({0.f, 0.f}, 1.f).Direction.Y > 0.f);
	CHECK(Camera.MakePickingRay({1.f, 1.f}, 1.f).Direction.X < 0.f);
	CHECK(Camera.MakePickingRay({1.f, 1.f}, 1.f).Direction.Y < 0.f);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.MouseDeltaPixels = {100.f, -150.f};
	Input.Movement = FVector3::Left();
	Camera.Update(Input, 0.25f, CameraViewportSize);
	for (const float AspectRatio : std::array{0.25f, 1.f, 2.f, 4.f})
	{
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio);
		CHECK(Snapshot.Projection(0, 0) == doctest::Approx(-Snapshot.Projection(1, 1) / AspectRatio));
		for (const FVector2 ScreenPosition : std::array{FVector2{0.f, 0.f}, FVector2{0.5f, 0.5f}, FVector2{0.8f, 0.2f}, FVector2{1.f, 1.f}})
		{
			const FViewportPickingRay Ray = Camera.MakePickingRay(ScreenPosition, AspectRatio);
			const FVector3 Ndc = ProjectCameraPoint(Snapshot, Ray.Origin + Ray.Direction * 10.f);
			CheckCameraVector(Ray.Origin, Snapshot.Position);
			CHECK(Ray.Direction.Length() == doctest::Approx(1.f));
			CHECK(Ndc.X == doctest::Approx(2.f * ScreenPosition.X - 1.f).epsilon(CameraTolerance));
			CHECK(Ndc.Y == doctest::Approx(1.f - 2.f * ScreenPosition.Y).epsilon(CameraTolerance));
		}
	}
}

TEST_CASE("Viewport camera projection center offsets the pivot and picking center")
{
	const FViewportCameraController Camera;
	const FVector2 ProjectionCenter{0.4f, 0.41f};
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(16.f / 9.f, ProjectionCenter);
	const FVector3 PivotNdc = ProjectCameraPoint(Snapshot, Camera.GetPivot());
	CHECK(PivotNdc.X == doctest::Approx(2.f * ProjectionCenter.X - 1.f));
	CHECK(PivotNdc.Y == doctest::Approx(1.f - 2.f * ProjectionCenter.Y));
	CheckCameraVector(Camera.MakePickingRay(ProjectionCenter, 16.f / 9.f, ProjectionCenter).Direction, (Camera.GetPivot() - Snapshot.Position).Normalized());
}

TEST_CASE("Viewport picking rays project back to normalized positions for off-center projections")
{
	const FViewportCameraController Camera;
	const FVector2 ProjectionCenter{0.37f, 0.62f};
	for (const float AspectRatio : std::array{0.3f, 1.f, 2.8f})
	{
		const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio, ProjectionCenter);
		for (const FVector2 ScreenPosition : std::array{FVector2{0.08f, 0.13f}, ProjectionCenter, FVector2{0.91f, 0.8f}})
		{
			const FViewportPickingRay Ray = Camera.MakePickingRay(ScreenPosition, AspectRatio, ProjectionCenter);
			const FVector3 Ndc = ProjectCameraPoint(Snapshot, Ray.Origin + Ray.Direction * 10.f);
			CHECK(Ndc.X == doctest::Approx(2.f * ScreenPosition.X - 1.f).epsilon(CameraTolerance));
			CHECK(Ndc.Y == doctest::Approx(1.f - 2.f * ScreenPosition.Y).epsilon(CameraTolerance));
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
	CHECK(Camera.GetSnapshot(1.5f, {-1.f, 1.5f}).Projection == Camera.GetSnapshot(1.5f, {0.f, 1.f}).Projection);
	CheckCameraVector(Camera.MakePickingRay(ScreenPosition, 1.5f, {-1.f, 1.5f}).Direction, Camera.MakePickingRay(ScreenPosition, 1.5f, {0.f, 1.f}).Direction);
}

TEST_CASE("Viewport camera rejects invalid state and clamps editable settings")
{
	FViewportCameraController Camera;
	const FViewportCameraSnapshot Before = Camera.GetSnapshot(1.f);
	Camera.Focus({10.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, 1.f, {0.f, 1.f});
	Camera.Focus({10.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, 1.f, {1.f, 1.01f});
	Camera.Focus({10.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, 1.f, {std::numeric_limits<float>::quiet_NaN(), 0.5f});
	CHECK(Camera.GetSnapshot(1.f).Position == Before.Position);
	FViewportCameraInput Input;
	Input.Mode = EViewportCameraMode::Fly;
	Input.Movement = FVector3::Forward();
	Input.MouseDeltaPixels.X = std::numeric_limits<float>::quiet_NaN();
	Camera.Update(Input, 1.f, CameraViewportSize);
	Camera.Focus({0.f, 0.f, 0.f}, {-1.f, 1.f, 1.f}, 1.f);
	CHECK(Camera.GetSnapshot(1.f).Position == Before.Position);
	CHECK(Camera.GetSnapshot(1.f).View == Before.View);
	CHECK(Camera.GetSnapshot(0.f).Projection == Before.Projection);
	Camera.SetMovementSpeed(std::numeric_limits<float>::infinity());
	CHECK(Camera.GetMovementSpeed() == 5.f);
	Camera.SetMovementSpeed(-1.f);
	CHECK(Camera.GetMovementSpeed() > 0.f);
	Camera.SetMouseSensitivity(-1.f);
	CHECK(Camera.GetMouseSensitivity() > 0.f);
	Camera.Focus({}, {}, 1.f);
	CHECK(Camera.GetSnapshot(1.f).Position.Length() == doctest::Approx(0.2f));
}

TEST_CASE("Physical camera exposure follows aperture, shutter, and ISO")
{
	CHECK(GetPhysicalCameraExposureEV100({.Aperture = 8.f, .ShutterSeconds = 1.f / 250.f, .Iso = 100.f}) == doctest::Approx(std::log2(16000.f)));
	// Sunny 16 at ISO 100.
	CHECK(GetPhysicalCameraExposureEV100({.Aperture = 16.f, .ShutterSeconds = 1.f / 100.f, .Iso = 100.f}) == doctest::Approx(std::log2(25600.f)));
	const FPhysicalCamera Base{};
	CHECK(GetPhysicalCameraExposureEV100({.Aperture = Base.Aperture, .ShutterSeconds = Base.ShutterSeconds, .Iso = Base.Iso * 2.f}) == doctest::Approx(GetPhysicalCameraExposureEV100(Base) - 1.f));
	CHECK(GetPhysicalCameraExposureEV100({.Aperture = Base.Aperture, .ShutterSeconds = Base.ShutterSeconds * 2.f, .Iso = Base.Iso}) == doctest::Approx(GetPhysicalCameraExposureEV100(Base) - 1.f));
}

TEST_CASE("Physical camera focal length maps to the full-frame vertical field of view")
{
	const float FocalLength = 12.f / std::tan(65.f * std::numbers::pi_v<float> / 360.f);
	CHECK(GetPhysicalCameraVerticalFieldOfView(FocalLength) == doctest::Approx(65.f * std::numbers::pi_v<float> / 180.f));
	CHECK(GetPhysicalCameraVerticalFieldOfView(50.f) < GetPhysicalCameraVerticalFieldOfView(24.f));
	FViewportCameraController Camera;
	Camera.SetVerticalFieldOfView(GetPhysicalCameraVerticalFieldOfView(50.f));
	CHECK(Camera.GetVerticalFieldOfView() == doctest::Approx(GetPhysicalCameraVerticalFieldOfView(50.f)));
	const auto Snapshot = Camera.GetSnapshot(1.f);
	CHECK(Snapshot.Projection(1, 1) == doctest::Approx(1.f / std::tan(GetPhysicalCameraVerticalFieldOfView(50.f) * 0.5f)));
}
}
