#include "Herta/EditorCore/ViewportCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float CameraNearPlane = 0.1f;
constexpr float MinimumOrbitDistance = CameraNearPlane * 2.f;
constexpr float MaximumOrbitDistance = 1'000'000.f;
constexpr float MaximumPitch = std::numbers::pi_v<float> * 0.5f - 0.01f;

bool IsFinite(const FVector2& Value)
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y);
}

bool IsFinite(const FVector3& Value)
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
}

float GetValidAspectRatio(const float AspectRatio)
{
	return std::isfinite(AspectRatio) && AspectRatio > 0.f ? AspectRatio : 1.f;
}

FVector2 GetValidProjectionCenter(const FVector2& ProjectionCenter)
{
	return IsFinite(ProjectionCenter) ? FVector2{std::clamp(ProjectionCenter.X, 0.f, 1.f), std::clamp(ProjectionCenter.Y, 0.f, 1.f)} : FVector2{0.5f, 0.5f};
}
}

void FViewportCameraController::Update(const FViewportCameraInput& Input, const float DeltaSeconds, const FVector2& ViewportSize)
{
	if (!IsFinite(Input.MouseDeltaPixels) || !IsFinite(Input.Movement) || !std::isfinite(Input.ScrollDelta) || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.f || !IsFinite(ViewportSize) || ViewportSize.X <= 0.f || ViewportSize.Y <= 0.f)
	{
		return;
	}

	if (Input.Mode == EViewportCameraMode::Fly)
	{
		Rotate(Input.MouseDeltaPixels);
		const FVector3 Movement{std::clamp(Input.Movement.X, -1.f, 1.f), std::clamp(Input.Movement.Y, -1.f, 1.f), std::clamp(Input.Movement.Z, -1.f, 1.f)};
		const FVector3 Direction = Movement.LengthSquared() > 1.f ? Movement.Normalized() : Movement;
		Translate(GetOrientation().RotateVector(Direction) * (MovementSpeed * (Input.bFast ? 4.f : 1.f) * DeltaSeconds));
		Pivot = Position + GetOrientation().RotateVector(FVector3::Forward()) * OrbitDistance;
	}
	else if (Input.Mode == EViewportCameraMode::Orbit)
	{
		Rotate(Input.MouseDeltaPixels);
		Position = Pivot - GetOrientation().RotateVector(FVector3::Forward()) * OrbitDistance;
	}
	else if (Input.Mode == EViewportCameraMode::Pan)
	{
		const float UnitsPerPixel = 2.f * OrbitDistance * std::tan(VerticalFieldOfView * 0.5f) / ViewportSize.Y;
		Translate(GetOrientation().RotateVector({Input.MouseDeltaPixels.X * UnitsPerPixel, Input.MouseDeltaPixels.Y * UnitsPerPixel, 0.f}));
	}
	else if (Input.Mode == EViewportCameraMode::Dolly)
	{
		Dolly(Input.MouseDeltaPixels.Y * MouseSensitivity);
	}

	if (Input.ScrollDelta != 0.f)
	{
		Dolly(-Input.ScrollDelta * 0.15f);
	}
}

void FViewportCameraController::Focus(const FVector3& Center, const FVector3& HalfExtent, const float AspectRatio, const FVector2 VisibleSize)
{
	if (!IsFinite(Center) || !IsFinite(HalfExtent) || !IsFinite(VisibleSize) || HalfExtent.X < 0.f || HalfExtent.Y < 0.f || HalfExtent.Z < 0.f || VisibleSize.X <= 0.f || VisibleSize.X > 1.f || VisibleSize.Y <= 0.f || VisibleSize.Y > 1.f)
	{
		return;
	}

	const float Radius = std::hypot(HalfExtent.X, HalfExtent.Y, HalfExtent.Z);
	const float TanHalfVerticalFov = std::tan(VerticalFieldOfView * 0.5f);
	const float HalfVerticalFov = std::atan(TanHalfVerticalFov * VisibleSize.Y);
	const float HalfHorizontalFov = std::atan(TanHalfVerticalFov * GetValidAspectRatio(AspectRatio) * VisibleSize.X);
	const float Distance = std::max(Radius / std::sin(std::min(HalfVerticalFov, HalfHorizontalFov)), Radius + MinimumOrbitDistance);
	const FVector3 NewPosition = Center - GetOrientation().RotateVector(FVector3::Forward()) * Distance;
	if (!std::isfinite(Distance) || Distance > MaximumOrbitDistance || !IsFinite(NewPosition))
	{
		return;
	}

	Pivot = Center;
	OrbitDistance = Distance;
	Position = NewPosition;
}

void FViewportCameraController::SetMovementSpeed(const float Speed)
{
	if (std::isfinite(Speed))
	{
		MovementSpeed = std::clamp(Speed, 0.01f, 10'000.f);
	}
}

void FViewportCameraController::SetMouseSensitivity(const float RadiansPerPixel)
{
	if (std::isfinite(RadiansPerPixel))
	{
		MouseSensitivity = std::clamp(RadiansPerPixel, 0.00001f, 0.1f);
	}
}

void FViewportCameraController::SetVerticalFieldOfView(const float Radians)
{
	if (std::isfinite(Radians))
	{
		VerticalFieldOfView = std::clamp(Radians, 0.01f, 3.f);
	}
}

float FViewportCameraController::GetVerticalFieldOfView() const
{
	return VerticalFieldOfView;
}

float GetPhysicalCameraVerticalFieldOfView(const float FocalLengthMillimeters)
{
	return 2.f * std::atan(PhysicalCameraSensorHeightMillimeters * 0.5f / std::max(FocalLengthMillimeters, 1.f));
}

float GetPhysicalCameraExposureEV100(const FPhysicalCamera& Camera)
{
	const float Aperture = std::max(Camera.Aperture, 0.5f);
	return std::log2(Aperture * Aperture / std::max(Camera.ShutterSeconds, 1e-6f)) - std::log2(std::max(Camera.Iso, 1.f) / 100.f);
}

FViewportCameraSnapshot FViewportCameraController::GetSnapshot(const float AspectRatio, const FVector2 ProjectionCenter) const
{
	FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(VerticalFieldOfView, GetValidAspectRatio(AspectRatio), CameraNearPlane);
	const FVector2 Center = GetValidProjectionCenter(ProjectionCenter);
	Projection(0, 2) = 2.f * Center.X - 1.f;
	Projection(1, 2) = 1.f - 2.f * Center.Y;
	return {.View = FMatrix4::Rotation(GetOrientation().Conjugated()) * FMatrix4::Translation(-Position), .Projection = Projection, .Position = Position};
}

FViewportPickingRay FViewportCameraController::MakePickingRay(const FVector2& NormalizedPosition, const float AspectRatio, const FVector2 ProjectionCenter) const
{
	const FVector2 ScreenPosition = IsFinite(NormalizedPosition) ? NormalizedPosition : FVector2{0.5f, 0.5f};
	const FVector2 Center = GetValidProjectionCenter(ProjectionCenter);
	const float HalfHeight = std::tan(VerticalFieldOfView * 0.5f);
	const FVector3 ViewDirection{2.f * (Center.X - ScreenPosition.X) * HalfHeight * GetValidAspectRatio(AspectRatio), 2.f * (Center.Y - ScreenPosition.Y) * HalfHeight, 1.f};
	return {.Origin = Position, .Direction = GetOrientation().RotateVector(ViewDirection.Normalized())};
}

FQuaternion FViewportCameraController::GetOrientation() const
{
	return FQuaternion::FromAxisAngle(FVector3::Up(), Yaw) * FQuaternion::FromAxisAngle(FVector3::Left(), -Pitch);
}

void FViewportCameraController::Rotate(const FVector2& MouseDeltaPixels)
{
	Yaw = std::remainder(Yaw - MouseDeltaPixels.X * MouseSensitivity, 2.f * std::numbers::pi_v<float>);
	Pitch = std::clamp(Pitch - MouseDeltaPixels.Y * MouseSensitivity, -MaximumPitch, MaximumPitch);
}

void FViewportCameraController::Translate(const FVector3& Offset)
{
	const FVector3 NewPosition = Position + Offset;
	const FVector3 NewPivot = Pivot + Offset;
	if (IsFinite(NewPosition) && IsFinite(NewPivot))
	{
		Position = NewPosition;
		Pivot = NewPivot;
	}
}

void FViewportCameraController::Dolly(const float LogDistanceDelta)
{
	const float NewDistance = std::clamp(OrbitDistance * std::exp(std::clamp(LogDistanceDelta, -20.f, 20.f)), MinimumOrbitDistance, MaximumOrbitDistance);
	const FVector3 NewPosition = Pivot - GetOrientation().RotateVector(FVector3::Forward()) * NewDistance;
	if (IsFinite(NewPosition))
	{
		OrbitDistance = NewDistance;
		Position = NewPosition;
	}
}
}
