#include "Herta/GameFramework/GameSession.h"

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
// Worn sticks rest up to about a tenth off centre.
constexpr float StickDeadZone = 0.15f;
// Radians per second at full tilt: about 200 degrees a second across and 140 up and down, which spans less.
constexpr float LookYawSpeed = 3.5f;
constexpr float LookPitchSpeed = 2.5f;

// Rescales past the dead zone, so the response starts at zero instead of jumping, and raises its length to Exponent along the same direction.
FVector2 ShapeStick(const FVector2 Stick, const float Exponent)
{
	const float Length = std::hypot(Stick.X, Stick.Y);
	if (!(Length > StickDeadZone))
	{
		return {};
	}

	const float Shaped = std::pow(std::min((Length - StickDeadZone) / (1.f - StickDeadZone), 1.f), Exponent) / Length;
	return {Stick.X * Shaped, Stick.Y * Shaped};
}
}

void FGameGamepadControls::Apply(FGameInput& Input, const FGameGamepad& Gamepad, const float DeltaSeconds)
{
	const FVector2 Move = ShapeStick(Gamepad.Move, 1.f);
	// Squared, so small corrections stay fine while a full push still turns quickly.
	const FVector2 Look = ShapeStick(Gamepad.Look, 2.f);
	Input.Move = {std::clamp(Input.Move.X + Move.X, -1.f, 1.f), std::clamp(Input.Move.Y + Move.Y, -1.f, 1.f)};
	Input.YawDelta -= Look.X * LookYawSpeed * DeltaSeconds;
	Input.PitchDelta += Look.Y * LookPitchSpeed * DeltaSeconds;
	Input.bJump |= Gamepad.bJump && !bJumpDown;
	bSprinting = (bSprinting != (Gamepad.bSprint && !bSprintDown)) && (Move.X != 0.f || Move.Y != 0.f);
	Input.bSprint |= bSprinting;
	Input.bCrouch |= Gamepad.bCrouch;
	bJumpDown = Gamepad.bJump;
	bSprintDown = Gamepad.bSprint;
}

void FGameGamepadControls::Reset() noexcept
{
	bJumpDown = true;
	bSprintDown = true;
	bSprinting = false;
}
}
