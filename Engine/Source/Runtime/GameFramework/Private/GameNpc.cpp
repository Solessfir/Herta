#include "GameNpc.h"

#include "Herta/Math/AffineTransform.h"
#include "PlayerCharacter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float TurnRadiansPerSecond = 8.f;
constexpr float EyeHeight = 1.6f;
// A swing that never reports its end, such as one cut short by a hit reaction, stops counting after this long.
constexpr float MaximumAttackSeconds = 1.6f;

[[nodiscard]] float TurnToward(const float From, const float To, const float MaximumStep)
{
	const float Difference = std::remainder(To - From, 2.f * std::numbers::pi_v<float>);
	return From + std::clamp(Difference, -MaximumStep, MaximumStep);
}
}

FGameNpc::FGameNpc(FPlayerCharacter& InBody, const FAiAgentComponent& InSettings)
    : Body(InBody)
    , Settings(InSettings)
{
}

void FGameNpc::Start(FNavigationWorld* const InNavigation, const FMatrix4& InAuthored)
{
	Navigation = InNavigation;
	Authored = InAuthored;
	const auto Transform = TryDecomposeTransform(InAuthored, 1.e-3f);
	Scale = Transform ? Transform->Scale3D : FVector3::One();
	Foot = {InAuthored(0, 3), InAuthored(1, 3), InAuthored(2, 3)};
	PreviousFoot = Foot;
	const FVector3 Forward = Transform ? Transform->Rotation.RotateVector(FVector3::Forward()) : FVector3::Forward();
	Yaw = std::atan2(Forward.X, Forward.Z);
	PreviousYaw = Yaw;
	FacingYaw.reset();
	GroundSpeed = 0.f;
	bMoving = false;
	Health = 1.f;
	bAttacking = false;
	bSwingStarting = false;
	Agent = {};
	if (Navigation)
	{
		const auto Added = Navigation->AddAgent(Foot, {.Radius = 0.35f, .Height = 1.8f, .MaximumSpeed = Settings.WalkSpeed, .MaximumAcceleration = 10.f});
		Agent = Added ? *Added : FNavigationAgentId{};
	}
}

void FGameNpc::Leave()
{
	if (Navigation && Agent.Value != 0)
	{
		Navigation->RemoveAgent(Agent);
	}

	Agent = {};
	Navigation = nullptr;
}

void FGameNpc::Abandon() noexcept
{
	Agent = {};
	Navigation = nullptr;
}

void FGameNpc::Step(const float Seconds, const std::span<const std::string> BodyEvents)
{
	PreviousFoot = Foot;
	PreviousYaw = Yaw;
	bSwingStarting = false;
	GroundSpeed = 0.f;
	if (const auto State = Navigation && Agent.Value != 0 ? Navigation->GetAgent(Agent) : std::nullopt; State && State->bOnNavigation)
	{
		Foot = State->Position;
		GroundSpeed = std::hypot(State->DesiredVelocity.X, State->DesiredVelocity.Z);
		if (GroundSpeed > 0.2f)
		{
			Yaw = TurnToward(Yaw, std::atan2(State->DesiredVelocity.X, State->DesiredVelocity.Z), TurnRadiansPerSecond * Seconds);
		}
	}

	if (FacingYaw && GroundSpeed <= 0.2f)
	{
		Yaw = TurnToward(Yaw, *FacingYaw, TurnRadiansPerSecond * Seconds);
	}

	for (const std::string& Event : BodyEvents)
	{
		bSwingStarting = bSwingStarting || (bAttacking && Event == "SwingStart");
		bAttacking = bAttacking && Event != "SwingEnd";
	}

	AttackSeconds += Seconds;
	bAttacking = bAttacking && AttackSeconds < MaximumAttackSeconds;
}

void FGameNpc::Wound(const float Amount)
{
	Health = std::max(Health - Amount, 0.f);
	if (Health == 0.f && Navigation && Agent.Value != 0)
	{
		Navigation->RemoveAgent(Agent);
		Agent = {};
	}
}

FMatrix4 FGameNpc::GetModel(const float Alpha) const
{
	const FVector3 Position = PreviousFoot * (1.f - Alpha) + Foot * Alpha;
	const float Facing = PreviousYaw + std::remainder(Yaw - PreviousYaw, 2.f * std::numbers::pi_v<float>) * Alpha;
	return FTransform{Position, FQuaternion::FromAxisAngle(FVector3::Up(), Facing), Scale}.ToMatrix();
}

float FGameNpc::GetGroundSpeed() const noexcept
{
	return GroundSpeed;
}

bool FGameNpc::IsDead() const noexcept
{
	return Health <= 0.f;
}

bool FGameNpc::IsOnNavigation() const
{
	const auto State = Navigation && Agent.Value != 0 ? Navigation->GetAgent(Agent) : std::nullopt;
	return State && State->bOnNavigation;
}

bool FGameNpc::IsSwingStarting() const noexcept
{
	return bSwingStarting;
}

const FAiAgentComponent& FGameNpc::GetSettings() const noexcept
{
	return Settings;
}

FVector3 FGameNpc::GetFootPosition() const
{
	return Foot;
}

FVector3 FGameNpc::GetEyePosition() const
{
	return Foot + FVector3::Up() * EyeHeight;
}

float FGameNpc::GetYaw() const
{
	return Yaw;
}

float FGameNpc::GetHealth() const
{
	return Health;
}

void FGameNpc::MoveTo(const FVector3& Destination, const float Speed)
{
	bMoving = true;
	FacingYaw.reset();
	if (Navigation && Agent.Value != 0 && !bAttacking)
	{
		Navigation->SetAgentMaximumSpeed(Agent, Speed);
		Navigation->SetAgentTarget(Agent, Destination);
	}
}

void FGameNpc::Stop()
{
	bMoving = false;
	if (Navigation && Agent.Value != 0)
	{
		Navigation->StopAgent(Agent);
	}
}

EAiMoveStatus FGameNpc::GetMoveStatus() const
{
	if (!bMoving)
	{
		return EAiMoveStatus::Idle;
	}

	const auto State = Navigation && Agent.Value != 0 ? Navigation->GetAgent(Agent) : std::nullopt;
	if (!State || !State->bOnNavigation)
	{
		return EAiMoveStatus::Failed;
	}

	return State->bArrived ? EAiMoveStatus::Arrived : EAiMoveStatus::Moving;
}

void FGameNpc::Face(const FVector3& Point)
{
	FacingYaw = std::atan2(Point.X - Foot.X, Point.Z - Foot.Z);
}

void FGameNpc::Animate(const std::string_view Trigger)
{
	Body.FireTrigger(Trigger);
}

bool FGameNpc::StartAttack(const FVector3& Target)
{
	if (bAttacking || IsDead())
	{
		return false;
	}

	// A swing plants the feet; the tree moves again once it is over.
	Stop();
	Face(Target);
	Body.FireTrigger("Attack");
	bAttacking = true;
	AttackSeconds = 0.f;
	return true;
}

bool FGameNpc::IsAttacking() const
{
	return bAttacking;
}
}
