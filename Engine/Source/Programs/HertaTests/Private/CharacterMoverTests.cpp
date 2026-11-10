#include "Herta/GameFramework/CharacterMover.h"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>

namespace Herta
{
namespace
{
constexpr float StepSeconds = 1.f / 60.f;

// A 200 m floor, so runs never reach its edge.
[[nodiscard]] std::unique_ptr<FPhysicsWorld> MakeFloor()
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {100.f, 0.5f, 100.f}, .Position = {0.f, -0.5f, 0.f}}));
	return std::move(*World);
}

[[nodiscard]] FCharacterMover MakeMover(FPhysicsWorld& World, const FMovementModeSet& Modes = {}, std::span<const FMovementModeFactory> CustomModes = {})
{
	auto Mover = FCharacterMover::Create(World, {0.f, 0.f, 0.f}, FMovementTuning{}, Modes, CustomModes, 0.65f);
	REQUIRE(Mover);
	return std::move(*Mover);
}

void Run(FPhysicsWorld& World, FCharacterMover& Mover, const FMoverInput& Input, const int Steps)
{
	for (int Step = 0; Step < Steps; ++Step)
	{
		REQUIRE(Mover.Step(World, Input, StepSeconds));
		REQUIRE(World.Step(StepSeconds));
	}
}

[[nodiscard]] float HorizontalSpeed(const FCharacterMover& Mover)
{
	const FVector3& Velocity = Mover.GetState().Velocity;
	return std::hypot(Velocity.X, Velocity.Z);
}

// Counts how often it is asked, and takes over once told to.
class FProbeMode final : public IMovementMode
{
public:
	[[nodiscard]] std::string_view GetName() const override
	{
		return "Probe";
	}

	[[nodiscard]] std::expected<bool, FPhysicsError> TryStart(FMoverContext&, std::string_view) override
	{
		++Asked;
		return bStart;
	}

	[[nodiscard]] std::expected<EMovementStep, FPhysicsError> Step(FMoverContext&) override
	{
		return EMovementStep::Continue;
	}

	int Asked = 0;
	bool bStart = false;
};
}

TEST_CASE("Sprinting reaches the ground speed within a second, and running without it stays at the base speed")
{
	const auto World = MakeFloor();
	FCharacterMover Mover = MakeMover(*World);
	Run(*World, Mover, {}, 20);

	Run(*World, Mover, {.Move = {0.f, 1.f}, .bSprint = true}, 60);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(7.2f).epsilon(0.03));
	Run(*World, Mover, {.Move = {0.f, 1.f}}, 60);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(4.f).epsilon(0.03));
	Run(*World, Mover, {.Move = {0.f, 1.f}, .bSprint = true}, 60);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(7.2f).epsilon(0.03));

	// Letting go brakes to a stop well inside half a second.
	Run(*World, Mover, {}, 30);
	CHECK(HorizontalSpeed(Mover) < 0.01f);

	Run(*World, Mover, {.Move = {0.f, 1.f}}, 300);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(4.f).epsilon(0.05));
}

TEST_CASE("Mirror's Edge jumps hand their forward boost back on landing, and slides halve the speed they end with")
{
	const auto World = MakeFloor();
	FCharacterMover Mover = MakeMover(*World);
	Run(*World, Mover, {}, 20);
	Run(*World, Mover, {.Move = {0.f, 1.f}, .bSprint = true}, 120);
	const float Running = HorizontalSpeed(Mover);
	REQUIRE(Running > 5.5f);

	// The jump adds 1 m/s forward; landing caps the speed at the take-off speed less 0.65 m/s.
	Mover.PressJump();
	Run(*World, Mover, {.Move = {0.f, 1.f}}, 1);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(Running + 1.f).epsilon(0.03));
	float Landed = 0.f;
	for (int Step = 0; Step < 120 && Landed == 0.f; ++Step)
	{
		Run(*World, Mover, {.Move = {0.f, 1.f}}, 1);
		Landed = Mover.GetActiveMode() == "Walking" ? HorizontalSpeed(Mover) : 0.f;
	}

	CHECK(Landed <= Running - 0.65f + 0.15f);

	// Pressing crouch at a run slides; it only brakes, ends below 2.5 m/s, and halves the speed it ends with.
	Run(*World, Mover, {.Move = {0.f, 1.f}, .bSprint = true}, 60);
	Run(*World, Mover, {.Move = {0.f, 1.f}, .bSprint = true, .bCrouch = true}, 1);
	REQUIRE(std::string(Mover.GetActiveMode()) == "Slide");
	float Last = HorizontalSpeed(Mover);
	while (Mover.GetActiveMode() == "Slide")
	{
		Run(*World, Mover, {.bCrouch = true}, 1);
		CHECK(HorizontalSpeed(Mover) <= Last + 0.01f);
		Last = Mover.GetActiveMode() == "Slide" ? HorizontalSpeed(Mover) : Last;
	}

	CHECK(Last < 2.6f);
	CHECK(HorizontalSpeed(Mover) == doctest::Approx(Last * 0.5f).epsilon(0.2));
	CHECK(Mover.GetState().bCrouched);
}

TEST_CASE("Character movers leave out disabled modes and ask custom modes first")
{
	const auto World = MakeFloor();

	// Without the slide, pressing crouch at a run only crouches.
	FCharacterMover NoSlide = MakeMover(*World, {.bSlide = false});
	Run(*World, NoSlide, {}, 20);
	Run(*World, NoSlide, {.Move = {0.f, 1.f}}, 90);
	Run(*World, NoSlide, {.Move = {0.f, 1.f}, .bCrouch = true}, 5);
	CHECK(std::string(NoSlide.GetActiveMode()) == "Walking");
	CHECK(NoSlide.GetState().bCrouched);

	// A custom mode is asked every step, and when it takes over it owns the movement.
	FProbeMode* Probe = nullptr;
	const FMovementModeFactory Factory = [&Probe](const FMovementTuning&)
	{
		auto Mode = std::make_unique<FProbeMode>();
		Probe = Mode.get();
		return Mode;
	};

	FCharacterMover Custom = MakeMover(*World, {}, std::span(&Factory, 1));
	REQUIRE(Probe);
	Run(*World, Custom, {}, 10);
	CHECK(Probe->Asked == 10);
	Probe->bStart = true;
	Run(*World, Custom, {.Move = {0.f, 1.f}}, 10);
	CHECK(std::string(Custom.GetActiveMode()) == "Probe");
	CHECK(HorizontalSpeed(Custom) == 0.f);
}
}
