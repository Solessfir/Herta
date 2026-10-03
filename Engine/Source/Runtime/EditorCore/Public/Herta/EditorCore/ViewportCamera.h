#pragma once

#include "Herta/Math/Matrix.h"

#include <cmath>
#include <numbers>

namespace Herta
{
enum class EViewportCameraMode
{
	None,
	Fly,
	Orbit,
	Pan,
	Dolly
};

struct FViewportCameraInput
{
	EViewportCameraMode Mode = EViewportCameraMode::None;
	FVector2 MouseDeltaPixels;
	// Movement follows the camera's Left-Up-Forward basis.
	FVector3 Movement;
	float ScrollDelta = 0.f;
	bool bFast = false;
};

struct FViewportCameraSnapshot
{
	FMatrix4 View;
	FMatrix4 Projection;
	FVector3 Position;
};

struct FViewportPickingRay
{
	FVector3 Origin;
	FVector3 Direction;
};

class FViewportCameraController
{
public:
	void Update(const FViewportCameraInput& Input, float DeltaSeconds, const FVector2& ViewportSize);
	void Focus(const FVector3& Center, const FVector3& HalfExtent, float AspectRatio, FVector2 VisibleSize = {1.f, 1.f});
	void SetMovementSpeed(float Speed);
	void SetMouseSensitivity(float RadiansPerPixel);

	[[nodiscard]] constexpr float GetMovementSpeed() const
	{
		return MovementSpeed;
	}

	[[nodiscard]] constexpr float GetMouseSensitivity() const
	{
		return MouseSensitivity;
	}

	// Radians; negative pitch looks down.
	[[nodiscard]] constexpr float GetPitch() const
	{
		return Pitch;
	}

	[[nodiscard]] constexpr float GetYaw() const
	{
		return Yaw;
	}

	[[nodiscard]] constexpr const FVector3& GetPivot() const
	{
		return Pivot;
	}

	// ProjectionCenter uses full-target normalized coordinates with a top-left origin.
	[[nodiscard]] FViewportCameraSnapshot GetSnapshot(float AspectRatio, FVector2 ProjectionCenter = {0.5f, 0.5f}) const;
	// Normalized screen coordinates use a top-left origin; positions outside the viewport remain valid.
	[[nodiscard]] FViewportPickingRay MakePickingRay(const FVector2& NormalizedPosition, float AspectRatio, FVector2 ProjectionCenter = {0.5f, 0.5f}) const;

private:
	[[nodiscard]] FQuaternion GetOrientation() const;
	void Rotate(const FVector2& MouseDeltaPixels);
	void Translate(const FVector3& Offset);
	void Dolly(float LogDistanceDelta);

	// Starts above and behind the origin, looking 17.5 degrees down at a pivot above it so the floor and the cube over it both fit.
	FVector3 Position{0.f, 4.f, -10.f};
	float Yaw = 0.f;
	float Pitch = -17.5f * std::numbers::pi_v<float> / 180.f;
	float OrbitDistance = -Position.Z / std::cos(Pitch);
	FVector3 Pivot = Position + GetOrientation().RotateVector(FVector3::Forward()) * OrbitDistance;
	float MovementSpeed = 5.f;
	float MouseSensitivity = 0.003f;
};
}
