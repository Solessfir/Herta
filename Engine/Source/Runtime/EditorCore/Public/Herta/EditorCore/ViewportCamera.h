#pragma once

#include "Herta/Math/Matrix.h"

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
	float ScrollDelta = 0.0f;
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
	void Focus(const FVector3& Center, const FVector3& HalfExtent, float AspectRatio);
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
	[[nodiscard]] FViewportCameraSnapshot GetSnapshot(float AspectRatio) const;
	// Normalized screen coordinates use a top-left origin; positions outside the viewport remain valid.
	[[nodiscard]] FViewportPickingRay MakePickingRay(const FVector2& NormalizedPosition, float AspectRatio) const;

private:
	[[nodiscard]] FQuaternion GetOrientation() const;
	void Rotate(const FVector2& MouseDeltaPixels);
	void Translate(const FVector3& Offset);
	void Dolly(float LogDistanceDelta);

	FVector3 Position{0.0f, 0.0f, -5.0f};
	FVector3 Pivot;
	float Yaw = 0.0f;
	float Pitch = 0.0f;
	float OrbitDistance = 5.0f;
	float MovementSpeed = 5.0f;
	float MouseSensitivity = 0.003f;
};
}
