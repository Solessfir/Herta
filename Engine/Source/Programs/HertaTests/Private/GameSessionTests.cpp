#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

namespace Herta
{
namespace
{
class FNoGameAssets final : public IGameAssets
{
public:
	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId&) const override
	{
		return nullptr;
	}
};

constexpr FAssetId CubeAsset{0x59f13694df4844e5, 0x863555194493f242};

// A 40 m floor with the Start zone at the origin and the Finish zone 10 m ahead along +Z.
[[nodiscard]] std::vector<FLevelEntity> MakeCourse()
{
	return {
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Start", .Transform = {.Translation = {0., 1., 0.}}, .Trigger = FTriggerComponent{.Size = {2.f, 2.f, 2.f}, .Role = ETriggerRole::Start}},
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Finish", .Transform = {.Translation = {0., 1., 10.}}, .Trigger = FTriggerComponent{.Size = {4.f, 2.f, 2.f}, .Role = ETriggerRole::Finish}},
	};
}

void Advance(FGameSession& Session, const FGameInput& Input, const int Frames)
{
	for (int Frame = 0; Frame < Frames; ++Frame)
	{
		REQUIRE(Session.Update(1.f / 60.f, Input));
	}
}
}

TEST_CASE("Game sessions spawn in the Start zone and time a run to the Finish zone")
{
	const FNoGameAssets Assets;
	const auto Session = FGameSession::Create(MakeCourse(), Assets, nullptr);
	REQUIRE_MESSAGE(Session.has_value(), (Session ? "" : Session.error().Message));
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Ready);
	CHECK((*Session)->GetPlayerFootPosition().Y == doctest::Approx(0.05f));

	Advance(**Session, {}, 30);
	CHECK((*Session)->IsPlayerGrounded());
	CHECK((*Session)->GetPlayerFootPosition().Y == doctest::Approx(0.f).epsilon(0.05));
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Ready);

	Advance(**Session, {.Move = {0.f, 1.f}}, 30);
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Running);
	CHECK((*Session)->GetPlayerFootPosition().Z > 1.f);

	Advance(**Session, {.Move = {0.f, 1.f}}, 180);
	const FGameObjective& Objective = (*Session)->GetObjective();
	REQUIRE(Objective.State == EGameObjectiveState::Finished);
	REQUIRE(Objective.BestSeconds);
	// About 9 m at walking speed after accelerating.
	CHECK(*Objective.BestSeconds > 1.5);
	CHECK(*Objective.BestSeconds < 3.);
	const std::vector<std::string> Events = (*Session)->TakeEvents();
	CHECK(std::ranges::contains(Events, std::string("Run started")));

	REQUIRE((*Session)->Restart());
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Ready);
	CHECK((*Session)->GetObjective().BestSeconds == Objective.BestSeconds);
	CHECK((*Session)->GetPlayerFootPosition().Z == doctest::Approx(0.f));
}

TEST_CASE("Game players jump the Sandbox hurdle limits and respawn after falling")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	Advance(**Session, {}, 30);

	float Highest = 0.f;
	REQUIRE((*Session)->Update(1.f / 60.f, {.bJump = true}));
	for (int Frame = 0; Frame < 90; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		Highest = std::max(Highest, (*Session)->GetPlayerFootPosition().Y);
	}

	// Mirror's Edge's jump lifts the feet about 1.2 m: over the 0.6 m hurdle, but the 1.3 m one takes a mantle.
	CHECK(Highest > 1.15f);
	CHECK(Highest < 1.25f);
	CHECK((*Session)->IsPlayerGrounded());

	// Walking off the floor edge falls below the respawn height and returns to the start.
	for (int Frame = 0; Frame < 900 && (*Session)->GetObjective().Respawns == 0; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {.Move = {1.f, 0.f}}));
	}

	CHECK((*Session)->GetObjective().Respawns == 1);
	CHECK((*Session)->GetPlayerFootPosition().X == doctest::Approx(0.f));
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Ready);
}

TEST_CASE("Game players teleport standing and still, keeping the run, and refuse non-finite places")
{
	const FNoGameAssets Assets;
	const auto Session = FGameSession::Create(MakeCourse(), Assets, nullptr);
	REQUIRE(Session);
	FGameSession& Game = **Session;
	Advance(Game, {.Move = {0.f, 1.f}}, 60);
	REQUIRE(Game.GetObjective().State == EGameObjectiveState::Running);
	REQUIRE(Game.TeleportPlayer({3.f, 0.5f, -4.f}, 1.f));
	CHECK(Game.GetPlayerFootPosition().X == doctest::Approx(3.f));
	CHECK(Game.GetPlayerFootPosition().Z == doctest::Approx(-4.f));
	CHECK(Game.GetPlayerVelocity().Length() == 0.f);
	CHECK(Game.GetPlayerYaw() == doctest::Approx(1.f));
	CHECK(Game.GetObjective().State == EGameObjectiveState::Running);
	Advance(Game, {}, 30);
	CHECK(Game.IsPlayerGrounded());
	CHECK(Game.GetPlayerFootPosition().Y == doctest::Approx(0.f).epsilon(0.05));
	CHECK_FALSE(Game.TeleportPlayer({std::nanf(""), 0.f, 0.f}, 0.f));
}

TEST_CASE("Game sessions reject invalid frame input and levels without a start still play")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	Course.erase(Course.begin() + 1);
	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Exploring);
	CHECK_FALSE((*Session)->GetWarnings().empty());
	CHECK_FALSE((*Session)->Update(-1.f, {}));
	CHECK_FALSE((*Session)->Update(1.f / 60.f, {.YawDelta = std::nanf("")}));
	const FMeshRenderView& View = (*Session)->GetRenderView({1280, 720});
	CHECK(View.Models.size() == Course.size());
}

TEST_CASE("Game players slide under a low ceiling and stand once there is room")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	Course.erase(Course.begin() + 2);
	// A 6 m slab 1.3 m above the floor, which the 1.22 m crouched capsule fits under but a standing player cannot pass.
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Low ceiling", .Transform = {.Translation = {0., 1.55, 16.}, .Scale = {6.f, 0.5f, 6.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	FGameSession& Game = **Session;
	Advance(Game, {}, 20);
	for (int Frame = 0; Frame < 300 && Game.GetPlayerFootPosition().Z < 10.5f; ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}}));
	}

	CHECK_FALSE(Game.IsPlayerSliding());
	const FVector3 Before = Game.GetPlayerVelocity();
	REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bCrouch = true}));
	CHECK(Game.IsPlayerCrouched());
	CHECK(Game.IsPlayerSliding());
	// A slide keeps the run's speed and only brakes; it adds none.
	const FVector3 Velocity = Game.GetPlayerVelocity();
	CHECK(std::hypot(Velocity.X, Velocity.Z) <= std::hypot(Before.X, Before.Z) + 0.01f);
	CHECK(std::hypot(Velocity.X, Velocity.Z) > 5.f);

	for (int Frame = 0; Frame < 600 && Game.IsPlayerSliding(); ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.bCrouch = true}));
	}

	// The slide carried the player under the slab, where standing up has no room.
	CHECK_FALSE(Game.IsPlayerSliding());
	CHECK(Game.GetPlayerFootPosition().Z > 13.f);
	CHECK(Game.GetPlayerFootPosition().Z < 19.f);
	Advance(Game, {}, 30);
	CHECK(Game.IsPlayerCrouched());
	CHECK(Game.GetPlayerFootPosition().Y == doctest::Approx(0.f).epsilon(0.05));

	// Crouch-walking out from under it lets the player stand.
	for (int Frame = 0; Frame < 600 && Game.IsPlayerCrouched(); ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}}));
	}
	CHECK(Game.GetPlayerFootPosition().Z > 19.f);
	CHECK_FALSE(Game.IsPlayerCrouched());
}

TEST_CASE("Game players wall jump away from a wall they touch in the air")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	Course.erase(Course.begin() + 2);
	// A wall whose face is 1.5 m ahead of the spawn.
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Wall", .Transform = {.Translation = {0., 2., 1.75}, .Scale = {6.f, 4.f, 0.5f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	FGameSession& Game = **Session;
	// Air control is weak, as in Mirror's Edge, so the jump needs a few steps of run-up to reach the wall.
	Advance(Game, {}, 20);
	Advance(Game, {.Move = {0.f, 1.f}}, 10);
	REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bJump = true}));

	float Away = 0.f;
	float Rising = 0.f;
	for (int Frame = 0; Frame < 60; ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bJump = !Game.IsPlayerGrounded()}));
		if (Game.GetPlayerVelocity().Z < Away)
		{
			Away = Game.GetPlayerVelocity().Z;
			Rising = Game.GetPlayerVelocity().Y;
		}
	}

	// Holding forward into the wall still kicks the player back out and up.
	CHECK(Away < -3.f);
	CHECK(Rising > 2.f);
}

TEST_CASE("Game players climb a 2.5 m chimney by wall jumping between its walls")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	Course.erase(Course.begin() + 2);
	// The Sandbox chimney: 0.3 m walls 4.5 m tall with 2.5 m between them, around the spawn.
	for (const double X : {-1.4, 1.4})
	{
		Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Chimney wall", .Transform = {.Translation = {X, 2.25, 0.}, .Scale = {0.3f, 4.5f, 2.5f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
	}

	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	FGameSession& Game = **Session;
	Advance(Game, {}, 20);

	// Steer the way the last kick sent the player and jump whenever airborne, as a player mashing jump between the walls would.
	float Steer = 1.f;
	float Highest = 0.f;
	REQUIRE(Game.Update(1.f / 60.f, {.Move = {Steer, 0.f}, .bJump = true}));
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		const float VelocityX = Game.GetPlayerVelocity().X;
		Steer = VelocityX < -1.f ? 1.f : VelocityX > 1.f ? -1.f
		                                                 : Steer;
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {Steer, 0.f}, .bJump = !Game.IsPlayerGrounded()}));
		Highest = std::max(Highest, Game.GetPlayerFootPosition().Y);
	}

	// Above the 3.5 m ledge beside the Sandbox chimney.
	CHECK(Highest > 3.6f);
}
}

namespace Herta
{
TEST_CASE("Gamepad controls ignore stick drift, curve look, and act once per press")
{
	FGameGamepadControls Controls;
	FGameInput Input;
	Controls.Apply(Input, {.Move = {0.1f, -0.1f}, .Look = {0.1f, 0.f}, .bJump = true}, 0.5f);
	CHECK(Input.Move.X == 0.f);
	CHECK(Input.Move.Y == 0.f);
	CHECK(Input.YawDelta == 0.f);
	// A press held from before play started does not jump.
	CHECK_FALSE(Input.bJump);

	Input = {};
	Controls.Apply(Input, {.Move = {0.f, 1.f}, .Look = {1.f, 0.f}}, 0.5f);
	CHECK(Input.Move.Y == doctest::Approx(1.f));
	CHECK(Input.YawDelta < 0.f);
	const float FullTurn = Input.YawDelta;

	Input = {};
	Controls.Apply(Input, {.Look = {0.575f, 0.f}}, 0.5f);
	// Halfway past the dead zone turns a quarter as fast.
	CHECK(Input.YawDelta == doctest::Approx(FullTurn * 0.25f));

	Input = {};
	Controls.Apply(Input, {.Look = {0.f, 1.f}, .bJump = true}, 0.5f);
	CHECK(Input.PitchDelta > 0.f);
	CHECK(Input.bJump);
	Input = {};
	Controls.Apply(Input, {.bJump = true}, 0.5f);
	CHECK_FALSE(Input.bJump);
}

TEST_CASE("Gamepad controls add to keys and forget held buttons on reset")
{
	FGameGamepadControls Controls;
	FGameInput Input = {.Move = {1.f, 1.f}, .bCrouch = false};
	Controls.Apply(Input, {.Move = {1.f, 0.f}, .bCrouch = true}, 0.1f);
	CHECK(Input.Move.X == 1.f);
	CHECK(Input.Move.Y == 1.f);
	CHECK(Input.bCrouch);

	Controls.Reset();
	Input = {};
	Controls.Apply(Input, {.bJump = true}, 0.1f);
	CHECK_FALSE(Input.bJump);
}

TEST_CASE("Game players wall run along a wall they run at, but not from a standing jump beside it")
{
	const FNoGameAssets Assets;
	std::vector<FLevelEntity> Course = MakeCourse();
	Course.erase(Course.begin() + 2);
	// A 20 m wall whose face runs along +Z 2.5 m to the player's left.
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Long wall", .Transform = {.Translation = {2.7, 2.5, 12.}, .Scale = {0.4f, 5.f, 20.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});

	const auto Run = [&Assets, &Course](const bool bRunUp, int& AirFrames)
	{
		const auto Session = FGameSession::Create(Course, Assets, nullptr);
		REQUIRE(Session);
		FGameSession& Game = **Session;
		Advance(Game, {}, 20);
		// Face 30 degrees toward the wall and run at it, as a player would, then hold forward along it. Standing, the player only turns.
		REQUIRE(Game.Update(1.f / 60.f, {.YawDelta = 0.5236f}));
		Advance(Game, {.Move = {0.f, bRunUp ? 1.f : 0.f}}, 50);
		bool bWallRan = false;
		AirFrames = 0;
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bJump = true}));
		for (int Frame = 0; Frame < 240; ++Frame)
		{
			REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}}));
			bWallRan = bWallRan || Game.IsPlayerWallRunning();
			if (Game.IsPlayerGrounded())
			{
				break;
			}

			++AirFrames;
		}

		CHECK(Game.IsPlayerGrounded());
		CHECK_FALSE(Game.IsPlayerWallRunning());
		return bWallRan;
	};

	// A plain jump stays up about 0.77 s; the run's lift and lighter gravity keep the player up longer.
	int AirFrames = 0;
	CHECK(Run(true, AirFrames));
	CHECK(AirFrames > 60);
	CHECK_FALSE(Run(false, AirFrames));
	CHECK(AirFrames < 50);
}

TEST_CASE("Game players mantle onto a ledge they jump at, but not one without room on top")
{
	const FNoGameAssets Assets;
	// A 1.7 m, 3 m deep block whose face is 1.5 m ahead of the spawn: higher than a jump and its step-up reach.
	const auto MakeLedgeCourse = [](const bool bCovered)
	{
		std::vector<FLevelEntity> Course = MakeCourse();
		Course.erase(Course.begin() + 2);
		Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Ledge", .Transform = {.Translation = {0., 0.85, 3.}, .Scale = {4.f, 1.7f, 3.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
		if (bCovered)
		{
			// A 2 m slab 1 m above the top leaves no room to stand there, and is too high to climb itself.
			Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Cover", .Transform = {.Translation = {0., 3.7, 3.}, .Scale = {4.f, 2.f, 3.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
		}

		return Course;
	};

	const auto Climb = [&Assets](const std::vector<FLevelEntity>& Course, bool& bMantled)
	{
		const auto Session = FGameSession::Create(Course, Assets, nullptr);
		REQUIRE(Session);
		FGameSession& Game = **Session;
		Advance(Game, {}, 20);
		Advance(Game, {.Move = {0.f, 1.f}}, 10);
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bJump = true}));
		// Letting go once the climb starts leaves the player standing near the edge instead of walking off the far side.
		bMantled = false;
		for (int Frame = 0; Frame < 120; ++Frame)
		{
			bMantled = bMantled || Game.IsPlayerMantling();
			REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, bMantled ? 0.f : 1.f}}));
		}

		return Game.GetPlayerFootPosition();
	};

	bool bMantled = false;
	const FVector3 OnTop = Climb(MakeLedgeCourse(false), bMantled);
	CHECK(bMantled);
	CHECK(OnTop.Y == doctest::Approx(1.7f).epsilon(0.03));
	CHECK(OnTop.Z > 1.6f);

	const FVector3 Blocked = Climb(MakeLedgeCourse(true), bMantled);
	CHECK_FALSE(bMantled);
	CHECK(Blocked.Y < 0.1f);
	CHECK(Blocked.Z < 1.5f);
}
}
