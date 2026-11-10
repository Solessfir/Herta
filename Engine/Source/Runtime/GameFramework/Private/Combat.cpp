#include "Herta/GameFramework/Combat.h"

#include <algorithm>
#include <array>

namespace Herta
{
namespace
{
constexpr std::array DeflectWindowSeconds{0.2f, 0.1f, 1.f / 15.f, 0.f};
// Sekiro reopens the full window once a guard animation plays out; a second's pause between presses stands in for that.
constexpr float MashSeconds = 1.f;
constexpr float DeflectRunSeconds = 2.f;
}

void FHitStop::Start(const float Seconds)
{
	Length = Seconds;
	Elapsed = 0.f;
}

float FHitStop::Advance(const float Seconds)
{
	if (!IsActive())
	{
		return Seconds;
	}

	constexpr float HoldSeconds = 0.06f;
	Elapsed += Seconds;
	if (Elapsed >= Length)
	{
		// The part of the step past the end runs at full speed.
		const float Over = Elapsed - Length;
		Length = 0.f;
		return std::min(Over, Seconds);
	}

	// Each step advances by the square of how far through the ease it is.
	const float Ease = Length > HoldSeconds ? std::clamp((Elapsed - HoldSeconds) / (Length - HoldSeconds), 0.f, 1.f) : 0.f;
	return Seconds * Ease * Ease;
}

bool FHitStop::IsActive() const noexcept
{
	return Length > 0.f;
}

FCombatSettings GetPlayerCombatSettings() noexcept
{
	return {.MaximumHealth = 100.f, .MaximumPosture = 120.f, .PostureRecoveryPerSecond = 30.f, .GuardRecoveryScale = 2.f, .StaggerSeconds = 1.2f, .DeathblowSeconds = 0.f};
}

FCombatSettings GetGuardCombatSettings() noexcept
{
	return {.MaximumHealth = 100.f, .MaximumPosture = 75.f, .PostureRecoveryPerSecond = 20.f, .GuardRecoveryScale = 1.f, .bHealthSlowsRecovery = true};
}

FCombatant::FCombatant(const FCombatSettings& InSettings)
    : Settings(InSettings)
    , Health(InSettings.MaximumHealth)
{
}

void FCombatant::Tick(const float Seconds)
{
	SincePressSeconds += Seconds;
	SinceDeflectedSeconds += Seconds;
	DeflectSeconds = std::max(DeflectSeconds - Seconds, 0.f);
	DeflectedInRow = SinceDeflectedSeconds > DeflectRunSeconds ? 0 : DeflectedInRow;
	if (StaggerSeconds > 0.f)
	{
		// A broken posture resets once the stagger ends, as Sekiro's does.
		StaggerSeconds = std::max(StaggerSeconds - Seconds, 0.f);
		Posture = StaggerSeconds == 0.f ? 0.f : Posture;
		return;
	}

	float Rate = Settings.PostureRecoveryPerSecond * (bGuardHeld ? Settings.GuardRecoveryScale : 1.f);
	if (Settings.bHealthSlowsRecovery)
	{
		const float Fraction = GetHealthFraction();
		Rate *= (Fraction < 0.8f ? 0.6f : 1.f) * (Fraction < 0.6f ? 0.5f : 1.f) * (Fraction < 0.4f ? 1.f / 3.f : 1.f);
	}

	Posture = std::max(Posture - Rate * Seconds, 0.f);
}

void FCombatant::PressGuard()
{
	Presses = SincePressSeconds < MashSeconds ? Presses + 1 : 0;
	SincePressSeconds = 0.f;
	DeflectSeconds = DeflectWindowSeconds[std::min<std::size_t>(Presses, DeflectWindowSeconds.size() - 1)];
	bGuardHeld = true;
}

void FCombatant::SetGuardHeld(const bool bHeld)
{
	bGuardHeld = bHeld;
	DeflectSeconds = bHeld ? DeflectSeconds : 0.f;
}

EGuardOutcome FCombatant::Receive(const FCombatBlow& Blow, const bool bFacingAttacker)
{
	const bool bGuard = bGuardHeld && bFacingAttacker && !Blow.bPerilous && StaggerSeconds == 0.f && !IsDead();
	if (bGuard && DeflectSeconds > 0.f)
	{
		// A deflect never breaks the defender, even at a full bar; the next block or hit does.
		AddPosture(Blow.PostureOnDeflect, false);
		return EGuardOutcome::Deflected;
	}

	if (bGuard)
	{
		AddPosture(Blow.PostureOnBlock, true);
		return StaggerSeconds > 0.f ? EGuardOutcome::GuardBroken : EGuardOutcome::Blocked;
	}

	Wound(Blow.Health, Blow.PostureOnHit);
	return EGuardOutcome::Hit;
}

void FCombatant::TakeDeflectRecoil(const FCombatBlow& Blow)
{
	const float Rate = 1.f / static_cast<float>(1u << std::min<std::uint32_t>(DeflectedInRow, 3));
	++DeflectedInRow;
	SinceDeflectedSeconds = 0.f;
	AddPosture(Blow.AttackerPostureOnDeflect * Rate, true);
}

void FCombatant::Wound(const float Amount, const float PostureAmount)
{
	Health = std::max(Health - Amount, 0.f);
	AddPosture(PostureAmount, true);
}

void FCombatant::Restore()
{
	*this = FCombatant(Settings);
}

void FCombatant::AddPosture(const float Amount, const bool bCanBreak)
{
	if (StaggerSeconds > 0.f)
	{
		return;
	}

	Posture = std::min(Posture + Amount, bCanBreak ? Settings.MaximumPosture : Settings.MaximumPosture - 1.f);
	if (Posture >= Settings.MaximumPosture)
	{
		StaggerSeconds = Settings.StaggerSeconds;
		DeflectSeconds = 0.f;
	}
}

float FCombatant::GetHealth() const noexcept
{
	return Health;
}

float FCombatant::GetHealthFraction() const noexcept
{
	return Health / Settings.MaximumHealth;
}

float FCombatant::GetPostureFraction() const noexcept
{
	return Posture / Settings.MaximumPosture;
}

bool FCombatant::IsStaggered() const noexcept
{
	return StaggerSeconds > 0.f;
}

bool FCombatant::IsDeathblowOpen() const noexcept
{
	return StaggerSeconds > Settings.StaggerSeconds - Settings.DeathblowSeconds;
}

bool FCombatant::IsDead() const noexcept
{
	return Health <= 0.f;
}

bool FCombatant::IsGuarding() const noexcept
{
	return bGuardHeld && StaggerSeconds == 0.f;
}

bool FCombatant::IsDeflecting() const noexcept
{
	return IsGuarding() && DeflectSeconds > 0.f;
}

const FCombatSettings& FCombatant::GetSettings() const noexcept
{
	return Settings;
}
}
