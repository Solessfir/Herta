#pragma once

#include "Herta/Math/Vector.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// Mirror's Edge's movement numbers, in meters and seconds, from the game's TdGame.u class defaults and native code; the comments name the
// original fields. Values the game keeps in animations or code are tuned.
struct FMovementTuning
{
	// TdPawn's cylinder: 30 cm radius, 1.8 m tall, MaxWallStepHeight 35 cm. TdMove.ShrinkCollision crouches and slides at 1.22 m.
	float Radius = 0.3f;
	float Height = 1.8f;
	float CrouchHeight = 1.22f;
	float StepHeight = 0.35f;

	// SpeedCurve_LightWeapon: holding forward builds speed along seconds of running, linear between points, to GroundSpeed 7.2 m/s.
	std::vector<std::pair<float, float>> SprintCurve{{0.f, 0.f}, {0.4f, 4.f}, {1.f, 5.2f}, {3.5f, 6.5f}, {7.f, 7.2f}};
	// SpeedMaxBaseVelocity and SpeedMinBaseVelocity: walking tops out at the base speed; sprint energy is the speed above it.
	float BaseSpeed = 4.f;
	float MinimumBaseSpeed = 0.1f;
	float GroundSpeed = 7.2f;
	// AccelRate 6144 uu/s², GroundFriction 8, and TdPlayerPawn's BrakingFrictionStrength 0.5.
	float AccelerationRate = 61.44f;
	float GroundFriction = 8.f;
	float BrakingStrength = 0.5f;
	// Speed{Sprint,Walk,Strafe}VelocityAccelerationFactor, SpeedTurnDecelerationFactor, and SpeedEnergyDeceleration{Time,Exponent}.
	float SprintSteering = 30.f;
	float WalkAcceleration = 7.f;
	float StrafeAcceleration = 10.f;
	float TurnDeceleration = 10.f;
	float EnergyDecelerationSeconds = 3.f;
	float EnergyDecelerationExponent = 0.5f;
	// InputMaxSprintHeightLimit and RaduisLimit: how hard forward, and in all, the stick must push for sprint acceleration, which also needs the
	// sprint input held. Mirror's Edge sprints whenever forward is held; Herta keeps a sprint button, so running stays at the base speed.
	float SprintInputForward = 0.7f;
	float SprintInputSize = 0.7f;
	// TdMove_Crouch.SpeedModifier, of GroundSpeed.
	float CrouchSpeedModifier = 0.2f;

	// The pawn falls at 1600 uu/s after 780 cm, about twice the world gravity; TdMove_Jump.BaseJumpZ 630 and JumpAddXY 100.
	float Gravity = 16.4103f;
	float JumpSpeed = 6.3f;
	float JumpForwardBoost = 1.f;
	// AirControl 0.025 of AccelRate.
	float AirAcceleration = 1.536f;
	float CoyoteSeconds = 0.12f;
	// TdPlayerController.JumpTapTime.
	float JumpBufferSeconds = 0.15f;
	// TdMove_Landing.LandingSpeedReduction: landing a plain jump caps speed at the take-off speed less this, handing back the jump's boost.
	float LandingSpeedReduction = 0.65f;

	// TdMove_Slide: crouch at 3.5 m/s along the view; it brakes at FrictionModifier 0.1 of ground friction, ignores move input, steers
	// toward the view at 0.2 of the angle per second and 11 degrees per second with strafing, ends below 2.5 m/s or when pulling back,
	// lets go of crouch only after 0.5 s, and halves the speed however it ends.
	float SlideMinimumSpeed = 3.5f;
	float SlideAbortSpeed = 2.5f;
	float SlideFrictionModifier = 0.1f;
	float SlideMinimumSeconds = 0.5f;
	float SlideExitKeep = 0.5f;
	float SlideLookTurn = 0.2f;
	float SlideStrafeTurn = 0.19175f;

	// TdMove_WallRun: at least WallRunningMinSpeed, facing at most 57 degrees into the wall, which must be at least 1.92 m tall. It lifts
	// to 1.7 m above the jump under its own gravity, 8.2 m/s² rising and 5 falling, times one more for each run chained from a wall jump,
	// drags 5 percent of the speed along the wall per second, and ends falling faster than 5 m/s.
	float WallRunMinimumSpeed = 2.f;
	float WallRunMaximumApproachRadians = 0.9948f;
	float WallRunStrafeStartRadians = 1.0472f;
	float WallRunReach = 0.5f;
	float WallRunReachAtSprint = 1.8f;
	float WallRunMinimumWallHeight = 1.92f;
	float WallRunRiseHeight = 1.7f;
	float WallRunMinimumStartSpeed = 3.f;
	float WallRunRisingGravity = 8.2f;
	float WallRunFallingGravity = 5.f;
	float WallRunFriction = 0.05f;
	float WallRunStopFallSpeed = 5.f;
	// TdMove_WallrunJump: 1.2 m/s out plus 4 m/s more looking away from the wall, 1 m up plus 0.6 m looking away, keeping all the speed
	// along the wall looking along it and 10 percent looking straight out.
	float WallRunJumpOut = 1.2f;
	float WallRunJumpOutLookAdd = 4.f;
	float WallRunJumpHeight = 1.f;
	float WallRunJumpHeightLookAdd = 0.6f;
	float WallRunJumpForwardMinimum = 0.1f;

	// Herta's wall kick, which Mirror's Edge does not have: jumping while touching a wall in the air kicks this fast away from it.
	float WallKickSpeed = 5.5f;
	float WallKickCooldownSeconds = 0.2f;

	// Herta's mantle: moving forward into a wall in the air climbs onto its top when it is this high above the feet and the player fits.
	float MantleMinimumHeight = 0.5f;
	float MantleMaximumHeight = 1.9f;
	float MantleReach = 0.3f;
};

struct FMoverInput
{
	// [strafe right, forward] from -1 to 1.
	FVector2 Move{};
	// The view's facing; 0 faces +Z and positive turns toward +X.
	float Yaw = 0.f;
	// How far the view turned during this step, which sheds sprint speed.
	float TurnRadians = 0.f;
	bool bSprint = false;
	bool bCrouch = false;
};

// How the character left the ground, which landings and chained wall runs depend on.
enum class EMoverTakeoff : std::uint8_t
{
	Other,
	Jump,
	WallJump,
};

enum class EMoverEvent : std::uint8_t
{
	Jumped,
	WallJumped,
	Landed,
	Mantled,
	SlideStarted,
	SlideEnded,
	WallRunStarted,
};

// What every movement mode reads and writes, so one mode hands over to the next without losing speed or timers.
struct FMoverState
{
	FVector3 Foot{};
	FVector3 PreviousFoot{};
	// Horizontal speed is relative to the ground the character stands on, so moving platforms carry it; vertical speed is absolute.
	FVector3 Velocity{};
	FPhysicsCharacterState Ground{};
	bool bGrounded = false;
	bool bCrouched = false;
	bool bCrouchPressed = false;
	float AirborneSeconds = 0.f;
	float JumpBufferSeconds = 0.f;
	bool bJumpedSinceGround = false;
	FVector3 LastJumpFoot{};
	EMoverTakeoff Takeoff = EMoverTakeoff::Other;
	float TakeoffSpeed = 0.f;
	// The wall the last wall run used, which cannot be run again before landing, and how many runs in a row came from wall jumps.
	std::optional<FVector3> LastWallNormal{};
	// Set while running along a wall.
	std::optional<FVector3> WallRunNormal{};
	int ChainedWallRuns = 0;
	// TdPawn.SpeedSprintEnergy, and the controller's tap-stop: letting go within 0.15 s of starting stops dead for 0.25 s.
	float SprintEnergy = 0.f;
	float AccelerationSeconds = 0.f;
	float StoppingSeconds = 0.f;
};

struct FMoverContext
{
	FPhysicsWorld& World;
	FPhysicsCharacterId Character;
	const FMovementTuning& Tuning;
	const FMoverInput& Input;
	FMoverState& State;
	float StepSeconds = 0.f;
	std::vector<EMoverEvent>& Events;
};

enum class EMovementStep : std::uint8_t
{
	Continue,
	Ended,
	// Ended without moving, so walking or falling moves the character this same step, as after a wall jump.
	EndedBeforeMoving,
};

// One way of moving, such as walking, sliding, or running along a wall. The mover asks the modes in order whether one starts, then steps
// the active one; a mode that ends hands over to walking or falling.
class IMovementMode
{
public:
	IMovementMode() = default;
	virtual ~IMovementMode() = default;
	IMovementMode(const IMovementMode&) = delete;
	IMovementMode& operator=(const IMovementMode&) = delete;
	IMovementMode(IMovementMode&&) = delete;
	IMovementMode& operator=(IMovementMode&&) = delete;

	[[nodiscard]] virtual std::string_view GetName() const = 0;
	// Whether the mode takes over from Active this step, setting itself up if so. Walking and falling never start this way.
	[[nodiscard]] virtual std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) = 0;
	[[nodiscard]] virtual std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) = 0;
	// Forgets per-run state on a restart.
	virtual void Reset();
};

// The built-in modes a character uses; turn one off to play without it.
struct FMovementModeSet
{
	bool bSlide = true;
	bool bWallRun = true;
	bool bWallKick = true;
	bool bMantle = true;
};

using FMovementModeFactory = std::function<std::unique_ptr<IMovementMode>(const FMovementTuning& Tuning)>;

// A Mover-style character controller: a capsule in the physics world driven by a list of movement modes that share one state.
class FCharacterMover
{
public:
	// Custom modes come first, so they are asked before the built-in ones. MantleSeconds is how long the climb takes, normally the clip's.
	[[nodiscard]] static std::expected<FCharacterMover, FPhysicsError> Create(FPhysicsWorld& World, const FVector3& Foot, const FMovementTuning& Tuning, const FMovementModeSet& Modes, std::span<const FMovementModeFactory> CustomModes, float MantleSeconds);

	// Buffers a jump for the coming steps, as the game does for a tap.
	void PressJump();
	[[nodiscard]] std::expected<void, FPhysicsError> Step(FPhysicsWorld& World, const FMoverInput& Input, float StepSeconds);
	[[nodiscard]] std::expected<void, FPhysicsError> Teleport(FPhysicsWorld& World, const FVector3& Foot);

	[[nodiscard]] const FMoverState& GetState() const noexcept;
	[[nodiscard]] std::string_view GetActiveMode() const noexcept;
	[[nodiscard]] std::vector<EMoverEvent> TakeEvents();
	[[nodiscard]] const FMovementTuning& GetTuning() const noexcept;

private:
	FCharacterMover(FPhysicsCharacterId InCharacter, const FMovementTuning& InTuning, std::vector<std::unique_ptr<IMovementMode>> InModes);

	void Land(FMoverContext& Context);

	FPhysicsCharacterId Character{};
	FMovementTuning Tuning{};
	FMoverState State{};
	// Walking and falling are the last two modes; the active one is an index into the list.
	std::vector<std::unique_ptr<IMovementMode>> Modes{};
	std::size_t WalkingMode = 0;
	std::size_t FallingMode = 0;
	std::size_t Active = 0;
	bool bCrouchWasHeld = false;
	std::vector<EMoverEvent> Events{};
};

// Shared moves for modes: these keep the capsule, crouch, and ground state in step.
namespace Movement
{
// Moves the capsule by Velocity plus the ground's motion for one step and absorbs what walls and ceilings block.
[[nodiscard]] std::expected<void, FPhysicsError> Move(FMoverContext& Context, const FVector3& Velocity);
// Crouches or stands; standing fails without change when there is no room above.
[[nodiscard]] std::expected<bool, FPhysicsError> SetCrouched(FMoverContext& Context, bool bCrouched);
// TdMove_Jump.StartMove: remembers the take-off speed and adds the forward boost when already moving forward.
void Jump(FMoverContext& Context);
// How far up the capsule can rise from where it is, up to Limit.
[[nodiscard]] float Headroom(const FMoverContext& Context, float Limit);
[[nodiscard]] FVector3 Forward(float Yaw);
[[nodiscard]] FVector3 Right(float Yaw);
}
}
