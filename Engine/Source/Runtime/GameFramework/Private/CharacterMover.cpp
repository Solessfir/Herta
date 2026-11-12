#include "Herta/GameFramework/CharacterMover.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

// The walking, falling, slide, and wall run rules follow Mirror's Edge's scripts and native code. The wall kick and the mantle are Herta's own.

namespace Herta
{
namespace
{
using FCurve = std::vector<std::pair<float, float>>;

// FInterpCurve::Eval for linear points: clamped at both ends.
[[nodiscard]] float Evaluate(const FCurve& Curve, const float X)
{
	if (Curve.empty())
	{
		return 0.f;
	}

	if (X <= Curve.front().first)
	{
		return Curve.front().second;
	}

	for (std::size_t Index = 1; Index < Curve.size(); ++Index)
	{
		const auto [X0, Y0] = Curve[Index - 1];
		const auto [X1, Y1] = Curve[Index];
		if (X < X1)
		{
			return X1 > X0 ? Y0 + (Y1 - Y0) * (X - X0) / (X1 - X0) : Y0;
		}
	}

	return Curve.back().second;
}

// The game turns the speed curve into acceleration against speed on load: at eleven speeds, the speed gained over the next 0.05 s of
// running. Sprinting straight then follows the speed curve exactly.
[[nodiscard]] FCurve MakeAccelerationCurve(const FCurve& Speed)
{
	if (Speed.size() < 2)
	{
		return {};
	}

	constexpr int Steps = 10;
	const auto [LastSeconds, TopSpeed] = Speed.back();
	const int Samples = static_cast<int>(Speed.size()) * Steps;
	FCurve SecondsAtSpeed;
	for (int Sample = 0; Sample <= Samples; ++Sample)
	{
		const float Seconds = static_cast<float>(Sample) * LastSeconds / static_cast<float>(Samples);
		SecondsAtSpeed.emplace_back(Evaluate(Speed, Seconds), Seconds);
	}

	constexpr float Delta = 0.5f / static_cast<float>(Steps);
	FCurve Acceleration;
	for (int Step = 0; Step <= Steps; ++Step)
	{
		const float Velocity = static_cast<float>(Step) * TopSpeed / static_cast<float>(Steps);
		const float Seconds = Evaluate(SecondsAtSpeed, Velocity);
		Acceleration.emplace_back(Velocity, (Evaluate(Speed, Seconds + Delta) - Evaluate(Speed, Seconds)) / Delta);
	}

	return Acceleration;
}

[[nodiscard]] FVector3 Horizontal(const FVector3& Value)
{
	return {Value.X, 0.f, Value.Z};
}

[[nodiscard]] FVector3 ClampLength(const FVector3& Value, const float Maximum)
{
	const float Length = Value.Length();
	return Length > Maximum && Length > 0.f ? Value * (Maximum / Length) : Value;
}

// Mirror's Edge's ground movement, from the native ATdPawn::GetSprintAcceleration, GetWalkAcceleration, and CalcVelocity and the player
// controller that picks between them.
class FLocomotion
{
public:
	explicit FLocomotion(const FMovementTuning& Tuning);

	// The game's sprint acceleration, which its air control still steers with.
	[[nodiscard]] FVector3 SprintAcceleration(const FMovementTuning& Tuning, const FMoverInput& Input, const FVector3& Velocity, float StepSeconds, bool bFalling) const;
	// bSkidStarted reports the step the feet plant against the run.
	[[nodiscard]] FVector3 ControllerAcceleration(const FMovementTuning& Tuning, FMoverState& State, const FMoverInput& Input, FVector3& Velocity, float StepSeconds, bool& bSkidStarted) const;
	[[nodiscard]] static FVector3 CalcVelocity(const FMovementTuning& Tuning, const FVector3& Velocity, const FVector3& Acceleration, float StepSeconds, float SpeedModifier, float Friction);

private:
	[[nodiscard]] static FVector3 InputDirection(const FMoverInput& Input);
	[[nodiscard]] static FVector3 WalkAcceleration(const FMovementTuning& Tuning, const FMoverInput& Input, const FVector3& Velocity, bool bSprint);

	FCurve AccelerationCurve;
};

FLocomotion::FLocomotion(const FMovementTuning& Tuning)
    : AccelerationCurve(MakeAccelerationCurve(Tuning.SprintCurve))
{
}

FVector3 FLocomotion::InputDirection(const FMoverInput& Input)
{
	return (Movement::Forward(Input.Yaw) * Input.Move.Y + Movement::Right(Input.Yaw) * Input.Move.X).Normalized();
}

FVector3 FLocomotion::SprintAcceleration(const FMovementTuning& Tuning, const FMoverInput& Input, const FVector3& Velocity, const float StepSeconds, const bool bFalling) const
{
	const FVector3 Direction = InputDirection(Input);
	if (Direction.LengthSquared() < 1.e-8f)
	{
		return {};
	}

	const float Speed = Horizontal(Velocity).Length();
	FVector3 Acceleration = Direction * Evaluate(AccelerationCurve, Speed);
	if (!bFalling)
	{
		// Steer the velocity onto the input direction at its current speed, and hand back what friction takes every step.
		const FVector3 Difference = Horizontal(Direction * Speed - Velocity);
		const float Distance = Difference.Length();
		const float Steer = std::min(Tuning.SprintSteering * Distance, StepSeconds > 0.f ? Distance / StepSeconds : 0.f);
		Acceleration += Difference.Normalized() * Steer + Horizontal(Velocity) * (Tuning.GroundFriction * 0.1f);
	}

	// Turning the view sheds speed: the game scales velocity by the turn in rotation units, 32768 to half a turn.
	return Acceleration - Horizontal(Velocity) * (Tuning.TurnDeceleration * std::abs(Input.TurnRadians) / std::numbers::pi_v<float>);
}

FVector3 FLocomotion::WalkAcceleration(const FMovementTuning& Tuning, const FMoverInput& Input, const FVector3& Velocity, const bool bSprint)
{
	const FVector3 Direction = InputDirection(Input);
	if (Direction.LengthSquared() < 1.e-8f)
	{
		return {};
	}

	// Herta sprints like most games: the run's target speed rises from the base to the ground speed while sprint is held, reached at the
	// walk's acceleration in under half a second.
	const FVector3 Forward = Movement::Forward(Input.Yaw);
	const FVector3 Right = Movement::Right(Input.Yaw);
	const float Top = (bSprint ? Tuning.GroundSpeed : Tuning.BaseSpeed) - Tuning.MinimumBaseSpeed;
	const float WantForward = Direction.Dot(Forward) * Tuning.MinimumBaseSpeed + Top * Input.Move.Y;
	const float WantRight = Direction.Dot(Right) * Tuning.MinimumBaseSpeed + Top * Input.Move.X;
	return Forward * ((WantForward - Velocity.Dot(Forward)) * Tuning.WalkAcceleration) + Right * ((WantRight - Velocity.Dot(Right)) * Tuning.StrafeAcceleration) + Horizontal(Velocity) * (Tuning.GroundFriction * 0.1f);
}

FVector3 FLocomotion::ControllerAcceleration(const FMovementTuning& Tuning, FMoverState& State, const FMoverInput& Input, FVector3& Velocity, const float StepSeconds, bool& bSkidStarted) const
{
	bSkidStarted = false;
	if (State.StoppingSeconds > 0.f)
	{
		State.StoppingSeconds -= StepSeconds;
		Velocity = Horizontal(Velocity).Normalized() * 0.35f;
		return Velocity;
	}

	const FVector3 Direction = InputDirection(Input);
	if (Direction.LengthSquared() > 0.f)
	{
		State.AccelerationSeconds += StepSeconds;
		const bool bSprint = Input.bSprint && Input.Move.Y > Tuning.SprintInputForward && Input.Move.Length() > Tuning.SprintInputSize;
		FVector3 Acceleration = WalkAcceleration(Tuning, Input, Velocity, bSprint);

		// Weight: a body moving faster than a run cannot change direction at once. Its path swings toward the input at the turn rate its
		// speed allows while the speed itself eases to what the input asks for, and pushing back against it brakes straight on planted feet
		// instead of swinging round in an arc.
		const float Speed = Horizontal(Velocity).Length();
		const bool bWasSkidding = State.bSkidding;
		State.bSkidding = false;
		if (Speed > Tuning.BaseSpeed && StepSeconds > 0.f)
		{
			const FVector3 Along = Horizontal(Velocity) / Speed;
			const float Facing = Direction.Dot(Along);
			State.bSkidding = Facing < -0.5f;
			if (State.bSkidding)
			{
				Acceleration = Along * -Tuning.PivotDeceleration;
			}
			else
			{
				const float Fraction = std::clamp((Speed - Tuning.BaseSpeed) / std::max(Tuning.GroundSpeed - Tuning.BaseSpeed, 1.e-3f), 0.f, 1.f);
				const float SteerRate = Tuning.SteerRateAtBase + (Tuning.SteerRateAtSprint - Tuning.SteerRateAtBase) * Fraction;
				const FVector3 Side = Direction - Along * Facing;
				const float Angle = std::atan2(Side.Length(), Facing);
				const float Turn = std::min(Angle, SteerRate * StepSeconds);
				const float Want = (bSprint ? Tuning.GroundSpeed : Tuning.BaseSpeed) * std::min(Input.Move.Length(), 1.f);
				const float Parallel = std::max((Want - Speed) * Tuning.WalkAcceleration, -Tuning.PivotDeceleration) + Speed * Tuning.GroundFriction * 0.1f;
				Acceleration = Along * Parallel + Side.Normalized() * (Speed * Turn / StepSeconds);
			}
		}

		bSkidStarted = State.bSkidding && !bWasSkidding;
		return State.LandingSeconds > 0.f ? Acceleration * Tuning.LandingRecoveryAcceleration : Acceleration;
	}

	State.bSkidding = false;

	// A tap: letting go within 0.15 s of starting stops dead for a moment.
	if (State.AccelerationSeconds > 0.f && State.AccelerationSeconds < 0.15f)
	{
		State.StoppingSeconds = 0.25f;
		Velocity = Horizontal(Velocity).Normalized() * 0.35f;
	}

	State.AccelerationSeconds = 0.f;
	return {};
}

FVector3 FLocomotion::CalcVelocity(const FMovementTuning& Tuning, const FVector3& Velocity, const FVector3& Acceleration, const float StepSeconds, const float SpeedModifier, const float Friction)
{
	const FVector3 Applied = ClampLength(Acceleration, Tuning.AccelerationRate * SpeedModifier);
	FVector3 Result = Velocity;
	if (Applied.LengthSquared() > 0.f)
	{
		Result -= Result * (StepSeconds * Friction * 0.1f);
	}
	else
	{
		// Braking in slices of at most 0.03 s, averaged over the step, stopping dead once reversed or nearly still.
		const FVector3 Before = Result;
		FVector3 Average{};
		for (float Left = StepSeconds; Left > 0.f;)
		{
			const float Slice = std::min(Left, 0.03f);
			Left -= Slice;
			Result -= Result * (2.f * Slice * Friction * Tuning.BrakingStrength);
			if (Result.Dot(Before) > 0.f && StepSeconds > 0.f)
			{
				Average += Result * (Slice / StepSeconds);
			}
		}

		Result = Average.Dot(Before) <= 0.f || Average.LengthSquared() < 0.01f ? FVector3{} : Average;
	}

	return ClampLength(Result + Applied * StepSeconds, Tuning.GroundSpeed * SpeedModifier);
}

class FWalkingMode final : public IMovementMode
{
public:
	explicit FWalkingMode(const FMovementTuning& Tuning);

	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;

private:
	FLocomotion Locomotion;
};

FWalkingMode::FWalkingMode(const FMovementTuning& Tuning)
    : Locomotion(Tuning)
{
}

std::string_view FWalkingMode::GetName() const
{
	return "Walking";
}

std::expected<bool, FPhysicsError> FWalkingMode::TryStart(FMoverContext&, std::string_view)
{
	return false;
}

std::expected<EMovementStep, FPhysicsError> FWalkingMode::Step(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	if (!State.bGrounded)
	{
		return EMovementStep::EndedBeforeMoving;
	}

	State.AirborneSeconds = 0.f;
	State.bJumpedSinceGround = false;
	State.LastWallNormal.reset();
	State.Velocity.Y = 0.f;
	if (Context.Input.bCrouch != State.bCrouched)
	{
		if (auto Crouched = Movement::SetCrouched(Context, Context.Input.bCrouch); !Crouched)
		{
			return std::unexpected(Crouched.error());
		}
	}

	if (State.JumpBufferSeconds > 0.f)
	{
		auto Stood = State.bCrouched ? Movement::SetCrouched(Context, false) : std::expected<bool, FPhysicsError>(true);
		if (!Stood)
		{
			return std::unexpected(Stood.error());
		}

		if (*Stood)
		{
			Movement::Jump(Context);
			return EMovementStep::EndedBeforeMoving;
		}
	}

	FVector3 Velocity = Horizontal(State.Velocity);
	bool bSkidStarted = false;
	const FVector3 Acceleration = Locomotion.ControllerAcceleration(Tuning, State, Context.Input, Velocity, Context.StepSeconds, bSkidStarted);
	if (bSkidStarted)
	{
		Context.Events.push_back(EMoverEvent::Skidded);
	}

	Velocity = FLocomotion::CalcVelocity(Tuning, Velocity, Acceleration, Context.StepSeconds, State.bCrouched ? Tuning.CrouchSpeedModifier : 1.f, Tuning.GroundFriction);
	if (auto Moved = Movement::Move(Context, Velocity); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	return State.bGrounded ? EMovementStep::Continue : EMovementStep::Ended;
}

class FFallingMode final : public IMovementMode
{
public:
	explicit FFallingMode(const FMovementTuning& Tuning);

	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;

private:
	FLocomotion Locomotion;
};

FFallingMode::FFallingMode(const FMovementTuning& Tuning)
    : Locomotion(Tuning)
{
}

std::string_view FFallingMode::GetName() const
{
	return "Falling";
}

std::expected<bool, FPhysicsError> FFallingMode::TryStart(FMoverContext&, std::string_view)
{
	return false;
}

std::expected<EMovementStep, FPhysicsError> FFallingMode::Step(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	State.AirborneSeconds += Context.StepSeconds;
	if (State.JumpBufferSeconds > 0.f && !State.bJumpedSinceGround && State.AirborneSeconds < Tuning.CoyoteSeconds && !State.bCrouched)
	{
		Movement::Jump(Context);
	}

	// Air control as APawn::physFalling clamps the sprint acceleration: AirControl of AccelRate, a little more when nearly still, and
	// almost none at top speed.
	const FVector3 Current = Horizontal(State.Velocity);
	const float Speed = Current.Length();
	float Maximum = Tuning.AirAcceleration;
	if (Speed < 0.1f && Context.StepSeconds > 0.f)
	{
		Maximum += (0.1f - Speed) / Context.StepSeconds;
	}
	else if (Speed >= Tuning.GroundSpeed && Tuning.AirAcceleration <= 0.05f * Tuning.AccelerationRate)
	{
		Maximum = 0.01f;
	}

	const FVector3 Acceleration = ClampLength(Horizontal(Locomotion.SprintAcceleration(Tuning, Context.Input, Current, Context.StepSeconds, true)), Maximum);
	const FVector3 Velocity = Current + Acceleration * Context.StepSeconds + FVector3::Up() * (State.Velocity.Y - Tuning.Gravity * Context.StepSeconds);
	State.FallSpeed = std::max(State.FallSpeed, -Velocity.Y);
	if (auto Moved = Movement::Move(Context, Velocity); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	return State.bGrounded && State.Velocity.Y <= 0.f ? EMovementStep::Ended : EMovementStep::Continue;
}

class FSlideMode final : public IMovementMode
{
public:
	[[nodiscard]] std::vector<float> SaveState() const override;
	void LoadState(std::span<const float> Saved) override;
	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;

private:
	// The slide's own heading, which follows the view slowly, so looking around does not steer it straight away.
	float SlideYaw = 0.f;
	// Where the slide ran relative to the view when it began, so a slide to the side or backwards is not drawn round to the front.
	float LookOffset = 0.f;
	float ElapsedSeconds = 0.f;
};

std::vector<float> FSlideMode::SaveState() const
{
	return {SlideYaw, LookOffset, ElapsedSeconds};
}

void FSlideMode::LoadState(const std::span<const float> Saved)
{
	if (Saved.size() == 3)
	{
		SlideYaw = Saved[0];
		LookOffset = Saved[1];
		ElapsedSeconds = Saved[2];
	}
}

std::string_view FSlideMode::GetName() const
{
	return "Slide";
}

std::expected<bool, FPhysicsError> FSlideMode::TryStart(FMoverContext& Context, const std::string_view Active)
{
	const FMoverState& State = Context.State;
	const FVector3 Velocity = Horizontal(State.Velocity);
	const bool bWanted = State.bCrouchPressed || (Context.Input.bCrouch && State.SlideWindowSeconds > 0.f);
	if (Active != "Walking" || !State.bGrounded || !bWanted || State.bCrouched || Velocity.Length() < Context.Tuning.SlideMinimumSpeed)
	{
		return false;
	}

	auto Crouched = Movement::SetCrouched(Context, true);
	if (!Crouched)
	{
		return std::unexpected(Crouched.error());
	}

	if (!*Crouched)
	{
		return false;
	}

	SlideYaw = std::atan2(Velocity.X, Velocity.Z);
	LookOffset = std::remainder(SlideYaw - Context.Input.Yaw, 2.f * std::numbers::pi_v<float>);
	ElapsedSeconds = 0.f;
	Context.Events.push_back(EMoverEvent::SlideStarted);
	return true;
}

std::expected<EMovementStep, FPhysicsError> FSlideMode::Step(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	ElapsedSeconds += Context.StepSeconds;
	if (State.JumpBufferSeconds > 0.f)
	{
		auto Stood = Movement::SetCrouched(Context, false);
		if (!Stood)
		{
			return std::unexpected(Stood.error());
		}

		if (*Stood)
		{
			Movement::Jump(Context);
			Context.Events.push_back(EMoverEvent::SlideEnded);
			return EMovementStep::EndedBeforeMoving;
		}
	}

	if (!State.bGrounded)
	{
		State.Velocity = State.Velocity * Tuning.SlideExitKeep;
		Context.Events.push_back(EMoverEvent::SlideEnded);
		return EMovementStep::EndedBeforeMoving;
	}

	// Input across the slide turns it, input against it stops it, and the view draws it round slowly.
	const FVector3 Wanted = Movement::Forward(Context.Input.Yaw) * Context.Input.Move.Y + Movement::Right(Context.Input.Yaw) * Context.Input.Move.X;
	const float Across = Wanted.Dot(Movement::Right(SlideYaw));
	const float Side = Across > 0.3f ? 1.f : Across < -0.3f ? -1.f
	                                                        : 0.f;
	const bool bAbort = (Side == 0.f && Wanted.Dot(Movement::Forward(SlideYaw)) < -0.8f) || State.Velocity.Length() < Tuning.SlideAbortSpeed;
	const float Difference = std::remainder(Context.Input.Yaw + LookOffset - SlideYaw, 2.f * std::numbers::pi_v<float>);
	SlideYaw += Context.StepSeconds * (Tuning.SlideLookTurn * Difference - Side * Tuning.SlideStrafeTurn);
	const float Speed = Horizontal(State.Velocity).Length();
	// A slide out of a dash runs faster than the ground speed, which is the limit only for running.
	FVector3 Velocity = FLocomotion::CalcVelocity(Tuning, Movement::Forward(SlideYaw) * Speed, {}, Context.StepSeconds, std::max(1.f, Speed / Tuning.GroundSpeed), Tuning.GroundFriction * Tuning.SlideFrictionModifier);

	const bool bStandUp = !Context.Input.bCrouch && ElapsedSeconds >= Tuning.SlideMinimumSeconds;
	if (bAbort || bStandUp)
	{
		if (bStandUp)
		{
			if (auto Stood = Movement::SetCrouched(Context, false); !Stood)
			{
				return std::unexpected(Stood.error());
			}
		}

		Velocity = Velocity * Tuning.SlideExitKeep;
		Context.Events.push_back(EMoverEvent::SlideEnded);
	}

	if (auto Moved = Movement::Move(Context, Velocity); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	return bAbort || bStandUp ? EMovementStep::Ended : EMovementStep::Continue;
}

struct FWallHit
{
	FVector3 Normal{};
	FVector3 Point{};
	// From the capsule's surface to the wall.
	float Gap = 0.f;
};

class FDashMode final : public IMovementMode
{
public:
	[[nodiscard]] std::vector<float> SaveState() const override;
	void LoadState(std::span<const float> Saved) override;
	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;
	void Reset() override;

private:
	FVector3 Direction{};
	float ElapsedSeconds = 0.f;
	float CooldownSeconds = 0.f;
	float TapSeconds = 0.f;
	bool bAirDashUsed = false;
};

std::vector<float> FDashMode::SaveState() const
{
	return {Direction.X, Direction.Y, Direction.Z, ElapsedSeconds, CooldownSeconds, TapSeconds, bAirDashUsed ? 1.f : 0.f};
}

void FDashMode::LoadState(const std::span<const float> Saved)
{
	if (Saved.size() == 7)
	{
		Direction = {Saved[0], Saved[1], Saved[2]};
		ElapsedSeconds = Saved[3];
		CooldownSeconds = Saved[4];
		TapSeconds = Saved[5];
		bAirDashUsed = Saved[6] != 0.f;
	}
}

std::string_view FDashMode::GetName() const
{
	return "Dash";
}

void FDashMode::Reset()
{
	ElapsedSeconds = CooldownSeconds = TapSeconds = 0.f;
	bAirDashUsed = false;
}

std::expected<bool, FPhysicsError> FDashMode::TryStart(FMoverContext& Context, const std::string_view Active)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	CooldownSeconds = std::max(CooldownSeconds - Context.StepSeconds, 0.f);
	TapSeconds = std::max(TapSeconds - Context.StepSeconds, 0.f);
	bAirDashUsed = bAirDashUsed && !State.bGrounded && Active != "WallRun";

	const FVector3 Forward = Movement::Forward(Context.Input.Yaw);
	const FVector3 Wanted = Forward * Context.Input.Move.Y + Movement::Right(Context.Input.Yaw) * Context.Input.Move.X;
	const bool bMoving = Wanted.LengthSquared() > 0.01f;
	std::optional<FVector3> Chosen;
	if (State.bSprintPressed && !bMoving)
	{
		// Held on, this is the start of a sprint, so only letting go in time makes it a dash.
		TapSeconds = Tuning.DashTapSeconds;
	}
	else if (bMoving && (State.bSprintPressed || (TapSeconds > 0.f && Context.Input.bSprint)))
	{
		Chosen = Wanted.Normalized();
	}
	else if (State.bSprintReleased && TapSeconds > 0.f)
	{
		Chosen = -Forward;
	}

	const bool bAir = Active == "Falling";
	if (!Chosen || (Active != "Walking" && !bAir && Active != "Slide") || CooldownSeconds > 0.f || (bAir && bAirDashUsed))
	{
		return false;
	}

	if (State.bCrouched)
	{
		auto Stood = Movement::SetCrouched(Context, false);
		if (!Stood)
		{
			return std::unexpected(Stood.error());
		}

		if (!*Stood)
		{
			return false;
		}
	}

	Direction = *Chosen;
	ElapsedSeconds = 0.f;
	TapSeconds = 0.f;
	CooldownSeconds = Tuning.DashCooldownSeconds;
	bAirDashUsed = bAirDashUsed || bAir;
	// An air dash spends the jump's speed cap with it, so landing does not take the dash back.
	State.Takeoff = EMoverTakeoff::Other;
	Context.Events.push_back(EMoverEvent::Dashed);
	return true;
}

std::expected<EMovementStep, FPhysicsError> FDashMode::Step(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	ElapsedSeconds += Context.StepSeconds;
	if (auto Moved = Movement::Move(Context, Direction * Tuning.DashSpeed); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	if (ElapsedSeconds < Tuning.DashSeconds)
	{
		return EMovementStep::Continue;
	}

	// A wall that stopped the dash leaves nothing to carry on with.
	State.Velocity = Direction * std::min(Tuning.DashExitSpeed, Horizontal(State.Velocity).Length());
	State.SlideWindowSeconds = Tuning.SlideLandSeconds;
	return EMovementStep::Ended;
}

class FWallRunMode final : public IMovementMode
{
public:
	[[nodiscard]] std::vector<float> SaveState() const override;
	void LoadState(std::span<const float> Saved) override;
	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;

private:
	[[nodiscard]] static std::optional<FWallHit> TraceWall(const FMoverContext& Context, const FVector3& Direction, float Reach, float Height);
	[[nodiscard]] static std::optional<FWallHit> FindWall(const FMoverContext& Context);

	FVector3 Normal{};
	float ElapsedSeconds = 0.f;
};

std::vector<float> FWallRunMode::SaveState() const
{
	return {Normal.X, Normal.Y, Normal.Z, ElapsedSeconds};
}

void FWallRunMode::LoadState(const std::span<const float> Saved)
{
	if (Saved.size() == 4)
	{
		Normal = {Saved[0], Saved[1], Saved[2]};
		ElapsedSeconds = Saved[3];
	}
}

std::string_view FWallRunMode::GetName() const
{
	return "WallRun";
}

// A sweep from the capsule's axis toward a wall at a height above the feet, reaching that far past the capsule's surface.
std::optional<FWallHit> FWallRunMode::TraceWall(const FMoverContext& Context, const FVector3& Direction, const float Reach, const float Height)
{
	constexpr float ProbeRadius = 0.1f;
	const FVector3 Start = Context.State.Foot + FVector3::Up() * Height;
	const float Distance = Context.Tuning.Radius - ProbeRadius + Reach;
	const FPhysicsWorld& World = Context.World;
	const auto Hit = World.CastSphereHit(World.ToWorld(Start), World.ToWorld(Start + Direction * Distance), ProbeRadius);
	if (!Hit || Hit->Fraction <= 0.f || std::abs(Hit->Normal.Y) > 0.3f)
	{
		return std::nullopt;
	}

	const FVector3 Normal = Horizontal(Hit->Normal).Normalized();
	// The gap is measured square to the wall, however obliquely the sweep reached it.
	const FVector3 Point = Start + Direction * (Distance * Hit->Fraction) - Normal * ProbeRadius;
	return FWallHit{.Normal = Normal, .Point = Point, .Gap = std::max(Horizontal(Point - Start).Dot(-Normal) - Context.Tuning.Radius, 0.f)};
}

// TdMove_WallRun.CanDoMove's wall search: FindWallSide when strafing toward a wall right after jumping, otherwise FindWallForward.
std::optional<FWallHit> FWallRunMode::FindWall(const FMoverContext& Context)
{
	const FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	const FVector3 Forward = Movement::Forward(Context.Input.Yaw);
	const FVector3 Velocity = Horizontal(State.Velocity);
	const float Speed = Velocity.Length();
	const float Top = Tuning.WallRunMinimumWallHeight - 0.02f;
	const auto IsSameWall = [&State](const FVector3& Normal)
	{
		return State.LastWallNormal && State.LastWallNormal->Dot(Normal) > 0.9f;
	};

	const auto Along = [&Forward](const FVector3& Normal)
	{
		const FVector3 Tangent{Normal.Z, 0.f, -Normal.X};
		return Tangent.Dot(Forward) < 0.f ? -Tangent : Tangent;
	};

	std::optional<FWallHit> Hit;
	const bool bNearJump = (State.Foot - State.LastJumpFoot).Length() < 0.3f && std::abs(Context.Input.Move.X) > 0.3f;
	if (bNearJump)
	{
		const FVector3 Side = Movement::Right(Context.Input.Yaw) * (Context.Input.Move.X > 0.f ? 1.f : -1.f);
		const auto High = TraceWall(Context, Side, Tuning.WallRunReach, Top);
		const auto Low = TraceWall(Context, Side, Tuning.WallRunReach, Tuning.StepHeight + 0.02f);
		if (High && Low && std::abs(High->Normal.Dot(Side)) >= std::cos(Tuning.WallRunStrafeStartRadians) && !IsSameWall(High->Normal) && Low->Normal.Dot(High->Normal) > 0.99f && State.Velocity.Dot(Along(High->Normal)) >= 0.f)
		{
			Hit = High;
		}
	}

	// Off a wall jump the next wall lies out to the side, across the gap, rather than ahead.
	if (!Hit && State.Takeoff == EMoverTakeoff::WallJump && State.LastWallNormal)
	{
		const FVector3 Side = *State.LastWallNormal;
		const auto High = TraceWall(Context, Side, Tuning.WallRunReach, Top);
		const auto Low = TraceWall(Context, Side, Tuning.WallRunReach, Tuning.StepHeight + 0.02f);
		if (High && Low && High->Normal.Dot(Side) < -0.5f && Low->Normal.Dot(High->Normal) > 0.99f && State.Velocity.Dot(Along(High->Normal)) >= 0.f)
		{
			Hit = High;
		}
	}

	if (!Hit)
	{
		// The reach grows from the base distance at run speed to the multiplier times it at full sprint.
		const float Fraction = std::max(Speed - Tuning.BaseSpeed, 0.f) / (Tuning.GroundSpeed - Tuning.BaseSpeed);
		const float Reach = ((Tuning.WallRunReachAtSprint - 1.f) * Fraction + 1.f) * Tuning.WallRunReach;
		const auto Middle = TraceWall(Context, Forward, Reach, Tuning.Height * 0.5f);
		if (!Middle)
		{
			return std::nullopt;
		}

		const float Into = -Middle->Normal.Dot(Forward);
		if (Into > std::cos(std::numbers::pi_v<float> * 0.5f - Tuning.WallRunMaximumApproachRadians) || IsSameWall(Middle->Normal) || !TraceWall(Context, Forward, Reach, Top - (1.f - Into) * 0.5f) || State.Velocity.Dot(Along(Middle->Normal)) < 0.f)
		{
			return std::nullopt;
		}

		Hit = Middle;
	}

	if (Velocity.Dot(Hit->Normal) >= 0.f)
	{
		return std::nullopt;
	}

	// Falling, there is no wall run when ground lies just under where it would go, half a second ahead.
	if (State.Velocity.Y < 0.f)
	{
		const FVector3 Start = FVector3{Hit->Point.X, State.Foot.Y - Tuning.Height * 0.5f, Hit->Point.Z} + Hit->Normal * Tuning.Radius;
		const FVector3 End = Start + Along(Hit->Normal) * (Speed * 0.5f) - FVector3::Up() * 0.45f;
		if (Context.World.CastSphere(Context.World.ToWorld(Start), Context.World.ToWorld(End), 0.01f) < 1.f)
		{
			return std::nullopt;
		}
	}

	return Hit;
}

std::expected<bool, FPhysicsError> FWallRunMode::TryStart(FMoverContext& Context, const std::string_view Active)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	const FVector3 Velocity = Horizontal(State.Velocity);
	const float Speed = Velocity.Length();
	if (Active != "Falling" || Speed < Tuning.WallRunMinimumSpeed || Velocity.Dot(Movement::Forward(Context.Input.Yaw)) < 0.f || Context.Input.Move.Y <= 0.3f || State.Velocity.Y <= -Tuning.WallRunStopFallSpeed || State.bCrouched)
	{
		return false;
	}

	const std::optional<FWallHit> Hit = FindWall(Context);
	if (!Hit)
	{
		return false;
	}

	// A run straight out of the last one's jump is chained: no lift, and a harder pull for each in a row.
	// Leaving the air resets how the character took off, so landing after the run does not pay a plain jump's landing cost.
	const bool bChained = State.Takeoff == EMoverTakeoff::WallJump;
	State.ChainedWallRuns = bChained ? State.ChainedWallRuns + 1 : 0;
	State.Takeoff = EMoverTakeoff::Other;
	const float Pull = Tuning.WallRunRisingGravity * static_cast<float>(1 + std::min(State.ChainedWallRuns, Tuning.WallRunMaximumChain));
	// The push off the last wall is spent crossing the gap, so it does not add to the speed along this one.
	const float Carried = bChained ? (Velocity - Hit->Normal * Velocity.Dot(Hit->Normal)).Length() : Speed;
	const float Begin = State.Velocity.Y > 0.f ? std::max(Carried, Tuning.WallRunMinimumStartSpeed) : Carried;
	float Rise = State.Velocity.Y;
	if (State.Velocity.Y > 0.f && !bChained)
	{
		const float Height = Movement::Headroom(Context, Tuning.WallRunRiseHeight) - (State.Foot.Y - State.LastJumpFoot.Y);
		Rise = std::sqrt(2.f * std::max(Height, 0.f) * Pull);
	}

	// The wall was found up to a reach ahead, so the capsule steps onto it, as the game moves the feet by the gap.
	const FVector3 OnWall = State.Foot - Hit->Normal * std::max(Hit->Gap - 0.01f, 0.f);
	if (auto Placed = Context.World.TeleportCharacter(Context.Character, OnWall); !Placed)
	{
		return std::unexpected(Placed.error());
	}

	State.Foot = OnWall;
	Normal = Hit->Normal;
	ElapsedSeconds = 0.f;
	State.Velocity = (Velocity - Normal * Velocity.Dot(Normal)).Normalized() * Begin + FVector3::Up() * Rise;
	State.JumpBufferSeconds = 0.f;
	State.LastWallNormal = Normal;
	State.WallRunNormal = Normal;
	Context.Events.push_back(EMoverEvent::WallRunStarted);
	return true;
}

std::expected<EMovementStep, FPhysicsError> FWallRunMode::Step(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	ElapsedSeconds += Context.StepSeconds;
	const float Chain = static_cast<float>(1 + std::min(State.ChainedWallRuns, Tuning.WallRunMaximumChain));
	const FVector3 Velocity = Horizontal(State.Velocity);
	const FVector3 Along = Velocity - Normal * Velocity.Dot(Normal);

	if (State.JumpBufferSeconds > 0.f && ElapsedSeconds > 0.1f)
	{
		// TdMove_WallrunJump: looking away from the wall pushes out harder and higher, trading the speed along it.
		const float Push = std::max(Movement::Forward(Context.Input.Yaw).Dot(Normal), 0.f);
		const float Height = Movement::Headroom(Context, Tuning.WallRunJumpHeight + Tuning.WallRunJumpHeightLookAdd * Push);
		const float Keep = Tuning.WallRunJumpForwardMinimum + (1.f - Tuning.WallRunJumpForwardMinimum) * (1.f - Push);
		State.Velocity = Normal * (Tuning.WallRunJumpOut + Tuning.WallRunJumpOutLookAdd * Push) + Along * Keep + FVector3::Up() * (std::sqrt(2.f * Tuning.Gravity * Height) / Chain);
		State.LastJumpFoot = State.Foot;
		State.AirborneSeconds = 0.f;
		State.bJumpedSinceGround = true;
		State.JumpBufferSeconds = 0.f;
		State.Takeoff = EMoverTakeoff::WallJump;
		State.WallRunNormal.reset();
		Context.Events.push_back(EMoverEvent::WallJumped);
		return EMovementStep::EndedBeforeMoving;
	}

	const auto Wall = TraceWall(Context, -Normal, 0.12f, Tuning.Height * 0.5f);
	const bool bOnWall = Wall && Wall->Normal.Dot(Normal) > 0.9f;
	const float Speed = Along.Length() * (1.f - Tuning.WallRunFriction * Context.StepSeconds);
	if (!bOnWall || State.Velocity.Y < -Tuning.WallRunStopFallSpeed || State.bCrouchPressed || Context.Input.Move.Y < -0.3f || Speed < 0.1f || State.bGrounded)
	{
		State.WallRunNormal.reset();
		return EMovementStep::EndedBeforeMoving;
	}

	// The wall run's own gravity, lighter while rising, and a slight push into the wall to keep the capsule on it.
	const float Pull = (State.Velocity.Y > 0.f ? Tuning.WallRunRisingGravity : Tuning.WallRunFallingGravity) * Chain;
	const FVector3 Next = Along.Normalized() * Speed - Normal * 0.5f + FVector3::Up() * (State.Velocity.Y - Pull * Context.StepSeconds);
	if (auto Moved = Movement::Move(Context, Next); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	if (State.bGrounded)
	{
		State.WallRunNormal.reset();
		return EMovementStep::Ended;
	}

	return EMovementStep::Continue;
}

class FWallKickMode final : public IMovementMode
{
public:
	[[nodiscard]] std::vector<float> SaveState() const override;
	void LoadState(std::span<const float> Saved) override;
	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;
	void Reset() override;

private:
	float SecondsSinceKick = 1.f;
};

std::vector<float> FWallKickMode::SaveState() const
{
	return {SecondsSinceKick};
}

void FWallKickMode::LoadState(const std::span<const float> Saved)
{
	if (Saved.size() == 1)
	{
		SecondsSinceKick = Saved[0];
	}
}

std::string_view FWallKickMode::GetName() const
{
	return "WallKick";
}

std::expected<bool, FPhysicsError> FWallKickMode::TryStart(FMoverContext& Context, const std::string_view Active)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	SecondsSinceKick += Context.StepSeconds;
	if (Active != "Falling" || State.JumpBufferSeconds <= 0.f || !State.Ground.bTouchingWall || SecondsSinceKick < Tuning.WallKickCooldownSeconds)
	{
		return false;
	}

	// The kick rises like a jump and pushes away from the wall, keeping the speed along it.
	const FVector3 Wall = State.Ground.WallNormal;
	const float Into = State.Velocity.Dot(Wall);
	State.Velocity = Horizontal(State.Velocity) + Wall * (Tuning.WallKickSpeed - std::min(Into, 0.f)) + FVector3::Up() * Tuning.JumpSpeed;
	State.JumpBufferSeconds = 0.f;
	State.bJumpedSinceGround = true;
	State.LastJumpFoot = State.Foot;
	State.Takeoff = EMoverTakeoff::WallJump;
	SecondsSinceKick = 0.f;
	Context.Events.push_back(EMoverEvent::WallJumped);
	return true;
}

std::expected<EMovementStep, FPhysicsError> FWallKickMode::Step(FMoverContext&)
{
	return EMovementStep::EndedBeforeMoving;
}

void FWallKickMode::Reset()
{
	SecondsSinceKick = 1.f;
}

class FMantleMode final : public IMovementMode
{
public:
	[[nodiscard]] std::vector<float> SaveState() const override;
	void LoadState(std::span<const float> Saved) override;
	explicit FMantleMode(float InDurationSeconds);

	[[nodiscard]] std::string_view GetName() const override;
	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext& Context, std::string_view Active) override;
	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext& Context) override;

private:
	float DurationSeconds = 0.f;
	FVector3 Start{};
	FVector3 Target{};
	float ElapsedSeconds = 0.f;
};

FMantleMode::FMantleMode(const float InDurationSeconds)
    : DurationSeconds(std::max(InDurationSeconds, 0.1f))
{
}

std::vector<float> FMantleMode::SaveState() const
{
	return {DurationSeconds, Start.X, Start.Y, Start.Z, Target.X, Target.Y, Target.Z, ElapsedSeconds};
}

void FMantleMode::LoadState(const std::span<const float> Saved)
{
	if (Saved.size() == 4)
	{
		DurationSeconds = Saved[0];
		Start = {Saved[1], Saved[2], Saved[3]};
		Target = {Saved[4], Saved[5], Saved[6]};
		ElapsedSeconds = Saved[7];
	}
}

std::string_view FMantleMode::GetName() const
{
	return "Mantle";
}

std::expected<bool, FPhysicsError> FMantleMode::TryStart(FMoverContext& Context, const std::string_view Active)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	const FVector3 Forward = Movement::Forward(Context.Input.Yaw);
	if (Active != "Falling" || State.bCrouched || Context.Input.Move.Y <= 0.5f || !State.Ground.bTouchingWall || State.Ground.WallNormal.Dot(Forward) >= -0.5f)
	{
		return false;
	}

	// A small sphere swept down just past the wall finds the top, down to just above the feet; a sweep that starts inside the wall means it
	// is too tall to climb.
	constexpr float ProbeRadius = 0.1f;
	const FPhysicsWorld& World = Context.World;
	const FVector3 Ahead = State.Foot + Forward * (Tuning.Radius + Tuning.MantleReach);
	const FVector3 Above = Ahead + FVector3::Up() * (Tuning.MantleMaximumHeight + ProbeRadius);
	const FVector3 Below = Ahead + FVector3::Up() * (ProbeRadius + 0.02f);
	const float Hit = World.CastSphere(World.ToWorld(Above), World.ToWorld(Below), ProbeRadius);
	if (Hit <= 0.f || Hit >= 1.f)
	{
		return false;
	}

	const FVector3 Top = Above + (Below - Above) * Hit - FVector3::Up() * (ProbeRadius - 0.01f);
	const float Rise = Top.Y - State.Foot.Y;

	// A top too low to climb is vaulted: the feet rise just enough to clear it and air control carries the player over. Its jump press is
	// spent here, so it never kicks off the obstacle backwards.
	if (Rise < Tuning.MantleMinimumHeight)
	{
		State.Velocity.Y = std::max(State.Velocity.Y, std::sqrt(2.f * Tuning.Gravity * (std::max(Rise, 0.f) + 0.1f)));
		State.JumpBufferSeconds = 0.f;
		return false;
	}

	// The standing capsule must fit on top, and nothing may hang over the player on the way up.
	const float Inset = Tuning.Radius - 0.02f;
	const auto IsClear = [&World, Inset](const FVector3& From, const FVector3& To)
	{
		return World.CastSphere(World.ToWorld(From), World.ToWorld(To), Inset) >= 1.f;
	};

	const FVector3 Head = FVector3::Up() * (Tuning.Height - Tuning.Radius);
	if (!IsClear(Top + FVector3::Up() * (Tuning.Radius + 0.05f), Top + Head) || !IsClear(State.Foot + Head, State.Foot + Head + FVector3::Up() * Rise))
	{
		return false;
	}

	Start = State.Foot;
	Target = Top;
	ElapsedSeconds = 0.f;
	Context.State.JumpBufferSeconds = 0.f;
	Context.Events.push_back(EMoverEvent::Mantled);
	return true;
}

std::expected<EMovementStep, FPhysicsError> FMantleMode::Step(FMoverContext& Context)
{
	// The path cuts through the ledge's corner, which a swept move would stop at, so the capsule is placed along it; both ends were
	// checked clear. The straight line matches the climb clip's root.
	FMoverState& State = Context.State;
	ElapsedSeconds += Context.StepSeconds;
	const float Progress = std::min(ElapsedSeconds / DurationSeconds, 1.f);
	const FVector3 Foot = Start + (Target - Start) * Progress;
	if (auto Placed = Context.World.TeleportCharacter(Context.Character, Foot); !Placed)
	{
		return std::unexpected(Placed.error());
	}

	State.Foot = Foot;
	State.Velocity = {};
	if (Progress < 1.f)
	{
		return EMovementStep::Continue;
	}

	// The target sits a centimetre above the top so the checks start clear. Settling onto it touches the ground, so walking finds it on the
	// next step instead of falling for one, which the animation would show as a fall and a landing.
	if (auto Settled = Movement::Move(Context, {0.f, -1.f, 0.f}); !Settled)
	{
		return std::unexpected(Settled.error());
	}

	State.Velocity = {};
	State.Takeoff = EMoverTakeoff::Other;
	return EMovementStep::Ended;
}
}

void IMovementMode::Reset()
{
}

std::vector<float> IMovementMode::SaveState() const
{
	return {};
}

void IMovementMode::LoadState(std::span<const float>)
{
}

namespace Movement
{
FVector3 Forward(const float Yaw)
{
	return {std::sin(Yaw), 0.f, std::cos(Yaw)};
}

FVector3 Right(const float Yaw)
{
	return Forward(Yaw).Cross(FVector3::Up());
}

std::expected<void, FPhysicsError> Move(FMoverContext& Context, const FVector3& Velocity)
{
	FMoverState& State = Context.State;
	const FVector3 Carried = State.bGrounded ? State.Ground.GroundVelocity : FVector3{};
	const auto Moved = Context.World.MoveCharacter(Context.Character, Velocity + Carried, Context.StepSeconds);
	if (!Moved)
	{
		return std::unexpected(Moved.error());
	}

	// Walls and ceilings take the velocity they block, so it does not build up against them.
	const FVector3 Resolved = Moved->Velocity - Carried;
	float Vertical = Velocity.Y;
	if (!Moved->bGrounded && Vertical > 0.f)
	{
		Vertical = std::min(Vertical, Resolved.Y);
	}
	else if (Moved->bGrounded && Vertical <= 0.f)
	{
		Vertical = 0.f;
	}

	State.Velocity = {Resolved.X, Vertical, Resolved.Z};
	// Leaving the ground keeps its motion, so jumping off a moving shuttle carries its speed.
	if (State.bGrounded && !Moved->bGrounded)
	{
		State.Velocity += Carried;
	}

	State.Foot = Moved->FootPosition;
	State.Ground = *Moved;
	State.bGrounded = Moved->bGrounded;
	return {};
}

std::expected<bool, FPhysicsError> SetCrouched(FMoverContext& Context, const bool bCrouched)
{
	if (Context.State.bCrouched == bCrouched)
	{
		return true;
	}

	const auto Resized = Context.World.SetCharacterHeight(Context.Character, bCrouched ? Context.Tuning.CrouchHeight : Context.Tuning.Height);
	if (!Resized)
	{
		return std::unexpected(Resized.error());
	}

	if (*Resized)
	{
		Context.State.bCrouched = bCrouched;
	}

	return *Resized;
}

void Jump(FMoverContext& Context)
{
	FMoverState& State = Context.State;
	const FMovementTuning& Tuning = Context.Tuning;
	const FVector3 Forward = Movement::Forward(Context.Input.Yaw);
	State.TakeoffSpeed = Horizontal(State.Velocity).Length();
	if (Forward.Dot(State.Velocity) > 0.1f)
	{
		State.Velocity += Forward * Tuning.JumpForwardBoost;
	}

	if (State.bGrounded)
	{
		State.Velocity += State.Ground.GroundVelocity;
		State.bGrounded = false;
	}

	State.Velocity.Y = Tuning.JumpSpeed + std::max(State.Velocity.Y, 0.f);
	State.LastJumpFoot = State.Foot;
	State.bJumpedSinceGround = true;
	State.JumpBufferSeconds = 0.f;
	State.AirborneSeconds = Tuning.CoyoteSeconds;
	State.Takeoff = EMoverTakeoff::Jump;
	Context.Events.push_back(EMoverEvent::Jumped);
}

float Headroom(const FMoverContext& Context, const float Limit)
{
	const FPhysicsWorld& World = Context.World;
	const float Inset = Context.Tuning.Radius - 0.02f;
	const float Height = Context.State.bCrouched ? Context.Tuning.CrouchHeight : Context.Tuning.Height;
	const FVector3 Head = Context.State.Foot + FVector3::Up() * (Height - Context.Tuning.Radius);
	return World.CastSphere(World.ToWorld(Head), World.ToWorld(Head + FVector3::Up() * Limit), Inset) * Limit;
}
}

FCharacterMover::FCharacterMover(const FPhysicsCharacterId InCharacter, const FMovementTuning& InTuning, std::vector<std::unique_ptr<IMovementMode>> InModes)
    : Character(InCharacter)
    , Tuning(InTuning)
    , Modes(std::move(InModes))
{
}

std::expected<FCharacterMover, FPhysicsError> FCharacterMover::Create(FPhysicsWorld& World, const FVector3& Foot, const FMovementTuning& Tuning, const FMovementModeSet& ModeSet, const std::span<const FMovementModeFactory> CustomModes, const float MantleSeconds)
{
	const auto Character = World.CreateCharacter({.FootPosition = Foot, .Height = Tuning.Height, .Radius = Tuning.Radius, .StepHeight = Tuning.StepHeight});
	if (!Character)
	{
		return std::unexpected(Character.error());
	}

	// Earlier modes are asked first, so custom modes can take over from the built-in ones.
	std::vector<std::unique_ptr<IMovementMode>> Modes;
	for (const FMovementModeFactory& Factory : CustomModes)
	{
		if (std::unique_ptr<IMovementMode> Mode = Factory ? Factory(Tuning) : nullptr)
		{
			Modes.push_back(std::move(Mode));
		}
	}

	if (ModeSet.bMantle)
	{
		Modes.push_back(std::make_unique<FMantleMode>(MantleSeconds));
	}

	if (ModeSet.bDash)
	{
		Modes.push_back(std::make_unique<FDashMode>());
	}

	if (ModeSet.bWallRun)
	{
		Modes.push_back(std::make_unique<FWallRunMode>());
	}

	if (ModeSet.bWallKick)
	{
		Modes.push_back(std::make_unique<FWallKickMode>());
	}

	if (ModeSet.bSlide)
	{
		Modes.push_back(std::make_unique<FSlideMode>());
	}

	Modes.push_back(std::make_unique<FWalkingMode>(Tuning));
	Modes.push_back(std::make_unique<FFallingMode>(Tuning));
	FCharacterMover Mover(*Character, Tuning, std::move(Modes));
	Mover.WalkingMode = Mover.Modes.size() - 2;
	Mover.FallingMode = Mover.Modes.size() - 1;
	Mover.Active = Mover.FallingMode;
	Mover.State.Foot = Mover.State.PreviousFoot = Foot;
	return Mover;
}

void FCharacterMover::PressJump()
{
	State.JumpBufferSeconds = Tuning.JumpBufferSeconds;
}

void FCharacterMover::Land(FMoverContext& Context)
{
	// TdMove_Landing.SubtractLandingSpeed: landing a plain jump caps the speed at the take-off speed, less the reduction, so hopping costs
	// speed. The game does not floor the cap at zero, which would push a slow landing backwards.
	if (State.Takeoff == EMoverTakeoff::Jump && !Context.Input.bCrouch)
	{
		const float Cap = std::max(State.TakeoffSpeed - Tuning.LandingSpeedReduction, 0.f);
		const FVector3 Current = Horizontal(State.Velocity);
		if (Current.Length() > Cap)
		{
			const FVector3 Capped = Current.Normalized() * Cap;
			State.Velocity = {Capped.X, State.Velocity.Y, Capped.Z};
		}
	}

	// Weight: a long drop takes speed and a moment to push off again, unless crouch slides out of it.
	const float Impact = std::exchange(State.FallSpeed, 0.f);
	if (Impact > Tuning.HeavyLandingSpeed && !Context.Input.bCrouch)
	{
		const float Hardness = std::clamp((Impact - Tuning.HeavyLandingSpeed) / std::max(Tuning.HardLandingSpeed - Tuning.HeavyLandingSpeed, 1.e-3f), 0.f, 1.f);
		const float Keep = Tuning.HeavyLandingKeep + (Tuning.HardLandingKeep - Tuning.HeavyLandingKeep) * Hardness;
		State.Velocity = {State.Velocity.X * Keep, State.Velocity.Y, State.Velocity.Z * Keep};
		State.LandingSeconds = Tuning.HeavyLandingRecoverySeconds + (Tuning.LandingRecoverySeconds - Tuning.HeavyLandingRecoverySeconds) * Hardness;
		Context.Events.push_back(EMoverEvent::LandedHeavily);
	}

	State.Takeoff = EMoverTakeoff::Other;
	State.SlideWindowSeconds = Tuning.SlideLandSeconds;
	Context.Events.push_back(EMoverEvent::Landed);
}

std::expected<void, FPhysicsError> FCharacterMover::Step(FPhysicsWorld& World, const FMoverInput& Input, const float StepSeconds)
{
	const auto Ground = World.UpdateCharacterGround(Character);
	if (!Ground)
	{
		return std::unexpected(Ground.error());
	}

	State.PreviousFoot = State.Foot;
	State.Ground = *Ground;
	State.bGrounded = Ground->bGrounded;
	State.bCrouchPressed = Input.bCrouch && !bCrouchWasHeld;
	bCrouchWasHeld = Input.bCrouch;
	State.bSprintPressed = Input.bSprint && !bSprintWasHeld;
	State.bSprintReleased = !Input.bSprint && bSprintWasHeld;
	bSprintWasHeld = Input.bSprint;
	FMoverContext Context{.World = World, .Character = Character, .Tuning = Tuning, .Input = Input, .State = State, .StepSeconds = StepSeconds, .Events = Events};
	// Only a plain fall builds up a landing; a wall run, dash, or mantle on the way starts it over.
	if (Active != FallingMode)
	{
		State.FallSpeed = 0.f;
	}

	// Walking and falling swap by ground contact at the start of a step, as when a platform rises under a falling player.
	if (Active == FallingMode && State.bGrounded && State.Velocity.Y <= 0.f)
	{
		Land(Context);
		Active = WalkingMode;
	}
	else if (Active == WalkingMode && !State.bGrounded)
	{
		State.Velocity += State.Ground.GroundVelocity;
		Active = FallingMode;
	}

	for (std::size_t Index = 0; Index < WalkingMode; ++Index)
	{
		if (Index == Active)
		{
			continue;
		}

		const auto Started = Modes[Index]->TryStart(Context, Modes[Active]->GetName());
		if (!Started)
		{
			return std::unexpected(Started.error());
		}

		if (*Started)
		{
			Active = Index;
			break;
		}
	}

	// A mode that ends before moving hands this step to walking or falling, once.
	for (int Attempt = 0; Attempt < 2; ++Attempt)
	{
		const std::size_t Stepped = Active;
		const auto Result = Modes[Stepped]->Step(Context);
		if (!Result)
		{
			return std::unexpected(Result.error());
		}

		if (*Result == EMovementStep::Continue)
		{
			break;
		}

		Active = State.bGrounded && State.Velocity.Y <= 0.f ? WalkingMode : FallingMode;
		if (Stepped == FallingMode && Active == WalkingMode)
		{
			Land(Context);
		}

		if (*Result == EMovementStep::Ended || Active == Stepped)
		{
			break;
		}
	}

	State.JumpBufferSeconds = std::max(0.f, State.JumpBufferSeconds - StepSeconds);
	State.SlideWindowSeconds = std::max(0.f, State.SlideWindowSeconds - StepSeconds);
	State.LandingSeconds = std::max(0.f, State.LandingSeconds - StepSeconds);
	return {};
}

std::expected<void, FPhysicsError> FCharacterMover::Teleport(FPhysicsWorld& World, const FVector3& Foot)
{
	if (auto Placed = World.TeleportCharacter(Character, Foot); !Placed)
	{
		return Placed;
	}

	const bool bCrouched = State.bCrouched;
	State = {};
	State.bCrouched = bCrouched;
	State.Foot = State.PreviousFoot = Foot;
	Active = FallingMode;
	for (const std::unique_ptr<IMovementMode>& Mode : Modes)
	{
		Mode->Reset();
	}

	return {};
}

FMoverSnapshot FCharacterMover::SaveSnapshot() const
{
	FMoverSnapshot Snapshot{.State = State, .Active = Active, .bCrouchWasHeld = bCrouchWasHeld, .bSprintWasHeld = bSprintWasHeld};
	for (const std::unique_ptr<IMovementMode>& Mode : Modes)
	{
		Snapshot.Modes.push_back(Mode->SaveState());
	}

	return Snapshot;
}

std::expected<void, FPhysicsError> FCharacterMover::RestoreSnapshot(FPhysicsWorld& World, const FMoverSnapshot& Snapshot)
{
	if (Snapshot.Active >= Modes.size() || Snapshot.Modes.size() != Modes.size())
	{
		return std::unexpected(FPhysicsError{"The mover snapshot was taken from a mover with other modes"});
	}

	// Height first, so the capsule fits where it is put back.
	if (auto Resized = World.SetCharacterHeight(Character, Snapshot.State.bCrouched ? Tuning.CrouchHeight : Tuning.Height); !Resized)
	{
		return std::unexpected(Resized.error());
	}

	if (auto Placed = World.TeleportCharacter(Character, Snapshot.State.Foot); !Placed)
	{
		return Placed;
	}

	State = Snapshot.State;
	Active = Snapshot.Active;
	bCrouchWasHeld = Snapshot.bCrouchWasHeld;
	bSprintWasHeld = Snapshot.bSprintWasHeld;
	for (std::size_t Index = 0; Index < Modes.size(); ++Index)
	{
		Modes[Index]->LoadState(Snapshot.Modes[Index]);
	}

	return {};
}

const FMoverState& FCharacterMover::GetState() const noexcept
{
	return State;
}

std::string_view FCharacterMover::GetActiveMode() const noexcept
{
	return Modes[Active]->GetName();
}

std::vector<EMoverEvent> FCharacterMover::TakeEvents()
{
	return std::exchange(Events, {});
}

const FMovementTuning& FCharacterMover::GetTuning() const noexcept
{
	return Tuning;
}

FPhysicsCharacterId FCharacterMover::GetCharacter() const noexcept
{
	return Character;
}
}
