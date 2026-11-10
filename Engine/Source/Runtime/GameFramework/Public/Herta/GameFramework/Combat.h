#pragma once

#include <cstdint>

namespace Herta
{
// Sword combat in the manner of Sekiro: blows wear down posture as well as vitality, a guard pressed just before a blow deflects it, and a
// broken posture opens a deathblow. Numbers follow the game's data where it is known: Wolf's 120 posture recovering at 30 per second, a
// soldier's 75, a 0.2 s deflect window that shrinks when the guard is mashed, and deflects that wear the attacker down by half each time.
struct FCombatSettings
{
	float MaximumHealth = 100.f;
	float MaximumPosture = 100.f;
	float PostureRecoveryPerSecond = 30.f;
	// Recovery multiplies by this while the guard is up.
	float GuardRecoveryScale = 2.f;
	// Recovery slows as vitality falls, as a soldier's does: x0.6 under 80 %, x0.5 under 60 %, x1/3 under 40 %.
	bool bHealthSlowsRecovery = false;
	// How long a broken posture staggers, and how much of it allows a deathblow.
	float StaggerSeconds = 3.f;
	float DeathblowSeconds = 2.5f;
};

// What one blow does, by how it is met.
struct FCombatBlow
{
	float Health = 20.f;
	float PostureOnHit = 18.f;
	float PostureOnBlock = 17.f;
	float PostureOnDeflect = 8.f;
	// Posture a deflect takes from the attacker before the falloff over consecutive deflects.
	float AttackerPostureOnDeflect = 45.f;
	// A perilous attack cannot be guarded at all.
	bool bPerilous = false;
};

enum class EGuardOutcome : std::uint8_t
{
	Hit,
	Blocked,
	Deflected,
	// The block that filled the posture bar; it staggers the defender.
	GuardBroken,
};

// Freezes a fighter's animation for a moment when blades meet, as Sekiro does: the clock holds still for 0.06 s, then eases back over the rest.
class FHitStop
{
public:
	// A new hitstop replaces one still running.
	void Start(float Seconds);
	// How far animation advances in Seconds of game time.
	[[nodiscard]] float Advance(float Seconds);
	[[nodiscard]] bool IsActive() const noexcept;

private:
	float Length = 0.f;
	float Elapsed = 0.f;
};

[[nodiscard]] FCombatSettings GetPlayerCombatSettings() noexcept;
[[nodiscard]] FCombatSettings GetGuardCombatSettings() noexcept;

// One fighter's vitality and posture. Posture counts damage taken, from zero up to the maximum, where it breaks.
class FCombatant
{
public:
	FCombatant() = default;
	explicit FCombatant(const FCombatSettings& InSettings);

	// Recovers posture and counts down a stagger and the guard's windows.
	void Tick(float Seconds);
	// Raises the guard: each press within the mashing window of the last deflects for a shorter time, 0.2, 0.1, 0.067 s, then not at all.
	void PressGuard();
	void SetGuardHeld(bool bHeld);
	// Meets a blow from the front with the guard, or takes it.
	EGuardOutcome Receive(const FCombatBlow& Blow, bool bFacingAttacker);
	// Posture this fighter loses for having its blow deflected; it halves with each deflect in a row.
	void TakeDeflectRecoil(const FCombatBlow& Blow);
	// A blow that cannot be guarded, such as a cut while staggered.
	void Wound(float Health, float Posture);
	void Restore();

	[[nodiscard]] float GetHealth() const noexcept;
	[[nodiscard]] float GetHealthFraction() const noexcept;
	// How full the posture bar is, from 0 to 1.
	[[nodiscard]] float GetPostureFraction() const noexcept;
	[[nodiscard]] bool IsStaggered() const noexcept;
	// Whether a cut now is a deathblow.
	[[nodiscard]] bool IsDeathblowOpen() const noexcept;
	[[nodiscard]] bool IsDead() const noexcept;
	[[nodiscard]] bool IsGuarding() const noexcept;
	[[nodiscard]] bool IsDeflecting() const noexcept;
	[[nodiscard]] const FCombatSettings& GetSettings() const noexcept;

private:
	void AddPosture(float Amount, bool bCanBreak);

	FCombatSettings Settings{};
	float Health = Settings.MaximumHealth;
	float Posture = 0.f;
	float StaggerSeconds = 0.f;

	bool bGuardHeld = false;
	float DeflectSeconds = 0.f;
	// Time since the last guard press, and how many presses in a row came within the mashing window.
	float SincePressSeconds = 1.e9f;
	std::uint32_t Presses = 0;

	// Deflects suffered in a row, each halving the posture the next one takes; the run ends after a pause.
	std::uint32_t DeflectedInRow = 0;
	float SinceDeflectedSeconds = 1.e9f;
};
}
