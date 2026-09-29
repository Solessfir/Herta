#include "Herta/EditorCore/ViewportCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float CameraVerticalFieldOfView = 65.0f * std::numbers::pi_v<float> / 180.0f;
constexpr float CameraNearPlane = 0.1f;
constexpr float MinimumOrbitDistance = CameraNearPlane * 2.0f;
constexpr float MaximumOrbitDistance = 1'000'000.0f;
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
	return std::isfinite(AspectRatio) && AspectRatio > 0.0f ? AspectRatio : 1.0f;
}
}

void FViewportCameraController::Update(const FViewportCameraInput& Input, const float DeltaSeconds, const FVector2& ViewportSize)
{
	if (!IsFinite(Input.MouseDeltaPixels) || !IsFinite(Input.Movement) || !std::isfinite(Input.ScrollDelta) || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f || !IsFinite(ViewportSize) || ViewportSize.X <= 0.0f || ViewportSize.Y <= 0.0f)
	{
		return;
	}

	if (Input.Mode == EViewportCameraMode::Fly)
	{
		Rotate(Input.MouseDeltaPixels);
		const FVector3 Movement{std::clamp(Input.Movement.X, -1.0f, 1.0f), std::clamp(Input.Movement.Y, -1.0f, 1.0f), std::clamp(Input.Movement.Z, -1.0f, 1.0f)};
		const FVector3 Direction = Movement.LengthSquared() > 1.0f ? Movement.Normalized() : Movement;
		Translate(GetOrientation().RotateVector(Direction) * (MovementSpeed * (Input.bFast ? 4.0f : 1.0f) * DeltaSeconds));
		Pivot = Position + GetOrientation().RotateVector(FVector3::Forward()) * OrbitDistance;
	}
	else if (Input.Mode == EViewportCameraMode::Orbit)
	{
		Rotate(Input.MouseDeltaPixels);
		Position = Pivot - GetOrientation().RotateVector(FVector3::Forward()) * OrbitDistance;
	}
	else if (Input.Mode == EViewportCameraMode::Pan)
	{
		const float UnitsPerPixel = 2.0f * OrbitDistance * std::tan(CameraVerticalFieldOfView * 0.5f) / ViewportSize.Y;
		Translate(GetOrientation().RotateVector({Input.MouseDeltaPixels.X * UnitsPerPixel, Input.MouseDeltaPixels.Y * UnitsPerPixel, 0.0f}));
	}
	else if (Input.Mode == EViewportCameraMode::Dolly)
	{
		Dolly(Input.MouseDeltaPixels.Y * MouseSensitivity);
	}

	if (Input.ScrollDelta != 0.0f)
	{
		Dolly(-Input.ScrollDelta * 0.15f);
	}
}

void FViewportCameraController::Focus(const FVector3& Center, const FVector3& HalfExtent, const float AspectRatio)
{
	if (!IsFinite(Center) || !IsFinite(HalfExtent) || HalfExtent.X < 0.0f || HalfExtent.Y < 0.0f || HalfExtent.Z < 0.0f)
	{
		return;
	}

	const float Radius = std::hypot(HalfExtent.X, HalfExtent.Y, HalfExtent.Z);
	const float HalfVerticalFov = CameraVerticalFieldOfView * 0.5f;
	const float HalfHorizontalFov = std::atan(std::tan(HalfVerticalFov) * GetValidAspectRatio(AspectRatio));
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
		MovementSpeed = std::clamp(Speed, 0.01f, 10'000.0f);
	}
}

void FViewportCameraController::SetMouseSensitivity(const float RadiansPerPixel)
{
	if (std::isfinite(RadiansPerPixel))
	{
		MouseSensitivity = std::clamp(RadiansPerPixel, 0.00001f, 0.1f);
	}
}

FViewportCameraSnapshot FViewportCameraController::GetSnapshot(const float AspectRatio) const
{
	return {FMatrix4::Rotation(GetOrientation().Conjugated()) * FMatrix4::Translation(-Position), FMatrix4::PerspectiveReversedInfinite(CameraVerticalFieldOfView, GetValidAspectRatio(AspectRatio), CameraNearPlane), Position};
}

FViewportPickingRay FViewportCameraController::MakePickingRay(const FVector2& NormalizedPosition, const float AspectRatio) const
{
	const FVector2 ScreenPosition = IsFinite(NormalizedPosition) ? NormalizedPosition : FVector2{0.5f, 0.5f};
	const float HalfHeight = std::tan(CameraVerticalFieldOfView * 0.5f);
	const FVector3 ViewDirection{(1.0f - 2.0f * ScreenPosition.X) * HalfHeight * GetValidAspectRatio(AspectRatio), (1.0f - 2.0f * ScreenPosition.Y) * HalfHeight, 1.0f};
	return {Position, GetOrientation().RotateVector(ViewDirection.Normalized())};
}

FQuaternion FViewportCameraController::GetOrientation() const
{
	return FQuaternion::FromAxisAngle(FVector3::Up(), Yaw) * FQuaternion::FromAxisAngle(FVector3::Left(), -Pitch);
}

void FViewportCameraController::Rotate(const FVector2& MouseDeltaPixels)
{
	Yaw = std::remainder(Yaw - MouseDeltaPixels.X * MouseSensitivity, 2.0f * std::numbers::pi_v<float>);
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
	const float NewDistance = std::clamp(OrbitDistance * std::exp(std::clamp(LogDistanceDelta, -20.0f, 20.0f)), MinimumOrbitDistance, MaximumOrbitDistance);
	const FVector3 NewPosition = Pivot - GetOrientation().RotateVector(FVector3::Forward()) * NewDistance;
	if (IsFinite(NewPosition))
	{
		OrbitDistance = NewDistance;
		Position = NewPosition;
	}
}
}
