#include "Herta/GameFramework/Combat.h"

#include <doctest/doctest.h>

using namespace Herta;

TEST_CASE("A guard pressed just before a blow deflects it, and mashing the guard shrinks the window")
{
	FCombatant Player(GetPlayerCombatSettings());
	const FCombatBlow Blow{};

	Player.PressGuard();
	Player.Tick(0.15f);
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::Deflected);
	Player.Tick(0.1f);
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::Blocked);

	// Each press within a second of the last opens a shorter window: 0.2 s, 0.1 s, 0.067 s, then none.
	FCombatant Masher(GetPlayerCombatSettings());
	Masher.PressGuard();
	Masher.Tick(0.3f);
	Masher.PressGuard();
	Masher.Tick(0.12f);
	CHECK(Masher.Receive(Blow, true) == EGuardOutcome::Blocked);
	Masher.Tick(0.3f);
	Masher.PressGuard();
	Masher.Tick(0.05f);
	CHECK(Masher.Receive(Blow, true) == EGuardOutcome::Deflected);
	Masher.Tick(0.3f);
	Masher.PressGuard();
	CHECK_FALSE(Masher.IsDeflecting());

	// A pause resets it.
	Masher.Tick(1.5f);
	Masher.PressGuard();
	Masher.Tick(0.19f);
	CHECK(Masher.IsDeflecting());

	// Released, turned away, or against a perilous attack, the guard does nothing.
	FCombatant Open(GetPlayerCombatSettings());
	Open.PressGuard();
	CHECK(Open.Receive(Blow, false) == EGuardOutcome::Hit);
	Open.PressGuard();
	CHECK(Open.Receive({.bPerilous = true}, true) == EGuardOutcome::Hit);
	Open.SetGuardHeld(false);
	CHECK(Open.Receive(Blow, true) == EGuardOutcome::Hit);
	CHECK(Open.GetHealth() == doctest::Approx(40.f));
}

TEST_CASE("Blocks fill the posture bar until the guard breaks, but a deflect never breaks the defender")
{
	FCombatant Player(GetPlayerCombatSettings());
	Player.SetGuardHeld(true);
	const FCombatBlow Blow{.PostureOnBlock = 50.f, .PostureOnDeflect = 200.f};
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::Blocked);
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::Blocked);
	Player.PressGuard();
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::Deflected);
	CHECK_FALSE(Player.IsStaggered());
	CHECK(Player.GetPostureFraction() < 1.f);
	Player.SetGuardHeld(false);
	Player.SetGuardHeld(true);
	CHECK(Player.Receive(Blow, true) == EGuardOutcome::GuardBroken);
	CHECK(Player.IsStaggered());
	CHECK_FALSE(Player.IsDeathblowOpen());
	CHECK_FALSE(Player.IsGuarding());

	// The stagger ends with the bar empty.
	Player.Tick(GetPlayerCombatSettings().StaggerSeconds + 0.01f);
	CHECK_FALSE(Player.IsStaggered());
	CHECK(Player.GetPostureFraction() == 0.f);
}

TEST_CASE("Deflects wear the attacker down by half each time in a row, and a broken posture opens a deathblow")
{
	FCombatant Guard(GetGuardCombatSettings());
	const FCombatBlow Blow{.AttackerPostureOnDeflect = 40.f};
	Guard.TakeDeflectRecoil(Blow);
	CHECK(Guard.GetPostureFraction() * 75.f == doctest::Approx(40.f));
	Guard.TakeDeflectRecoil(Blow);
	CHECK(Guard.GetPostureFraction() * 75.f == doctest::Approx(60.f));
	Guard.TakeDeflectRecoil(Blow);
	CHECK(Guard.GetPostureFraction() * 75.f == doctest::Approx(70.f));
	Guard.TakeDeflectRecoil(Blow);
	CHECK(Guard.IsStaggered());
	CHECK(Guard.IsDeathblowOpen());
	Guard.Tick(2.6f);
	CHECK(Guard.IsStaggered());
	CHECK_FALSE(Guard.IsDeathblowOpen());

	// After a pause the falloff starts over.
	FCombatant Fresh(GetGuardCombatSettings());
	Fresh.TakeDeflectRecoil(Blow);
	Fresh.Tick(2.5f);
	const float Before = Fresh.GetPostureFraction();
	Fresh.TakeDeflectRecoil(Blow);
	CHECK((Fresh.GetPostureFraction() - Before) * 75.f == doctest::Approx(40.f));
}

TEST_CASE("Posture recovers faster behind a guard and slower as a guard's vitality falls")
{
	FCombatant Player(GetPlayerCombatSettings());
	Player.Wound(0.f, 90.f);
	Player.Tick(1.f);
	CHECK(Player.GetPostureFraction() * 120.f == doctest::Approx(60.f));
	Player.SetGuardHeld(true);
	Player.Tick(0.5f);
	CHECK(Player.GetPostureFraction() * 120.f == doctest::Approx(30.f));

	FCombatant Healthy(GetGuardCombatSettings());
	FCombatant Hurt(GetGuardCombatSettings());
	Healthy.Wound(0.f, 50.f);
	Hurt.Wound(70.f, 50.f);
	Healthy.Tick(1.f);
	Hurt.Tick(1.f);
	CHECK(Healthy.GetPostureFraction() * 75.f == doctest::Approx(30.f));
	CHECK(Hurt.GetPostureFraction() * 75.f == doctest::Approx(50.f - 20.f * 0.6f * 0.5f / 3.f));
}

TEST_CASE("Hitstop holds the clock still, eases it back, and gives the rest of a step back")
{
	FHitStop Stop;
	CHECK(Stop.Advance(0.016f) == doctest::Approx(0.016f));
	Stop.Start(0.1f);
	CHECK(Stop.Advance(0.05f) == 0.f);
	const float Easing = Stop.Advance(0.03f);
	CHECK(Easing > 0.f);
	CHECK(Easing < 0.03f);
	CHECK(Stop.Advance(0.03f) == doctest::Approx(0.01f));
	CHECK_FALSE(Stop.IsActive());
}
