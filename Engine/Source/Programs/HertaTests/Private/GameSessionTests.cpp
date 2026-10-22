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

	// Clears the 0.6 m hurdle but not the 1.3 m one.
	CHECK(Highest > 0.65f);
	CHECK(Highest < 1.f);
	CHECK((*Session)->IsPlayerGrounded());

	// Walking off the floor edge falls below the respawn height and returns to the start.
	for (int Frame = 0; Frame < 900 && (*Session)->GetObjective().Respawns == 0; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {.Move = {1.f, 0.f}, .bSprint = true}));
	}

	CHECK((*Session)->GetObjective().Respawns == 1);
	CHECK((*Session)->GetPlayerFootPosition().X == doctest::Approx(0.f));
	CHECK((*Session)->GetObjective().State == EGameObjectiveState::Ready);
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
	// A 12 m slab 1.1 m above the floor, which a standing player cannot pass.
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Low ceiling", .Transform = {.Translation = {0., 1.35, 12.}, .Scale = {6.f, 0.5f, 12.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static});
	const auto Session = FGameSession::Create(Course, Assets, nullptr);
	REQUIRE(Session);
	FGameSession& Game = **Session;
	Advance(Game, {}, 20);
	Advance(Game, {.Move = {0.f, 1.f}, .bSprint = true}, 30);
	CHECK_FALSE(Game.IsPlayerSliding());
	REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bSprint = true, .bCrouch = true}));
	CHECK(Game.IsPlayerCrouched());
	CHECK(Game.IsPlayerSliding());
	// Entering a slide boosts the 7 m/s sprint once.
	const FVector3 Velocity = Game.GetPlayerVelocity();
	CHECK(std::hypot(Velocity.X, Velocity.Z) > 10.f);

	for (int Frame = 0; Frame < 600 && Game.IsPlayerSliding(); ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.bCrouch = true}));
	}

	// The slide carried the player under the slab, where standing up has no room.
	CHECK_FALSE(Game.IsPlayerSliding());
	CHECK(Game.GetPlayerFootPosition().Z > 7.f);
	CHECK(Game.GetPlayerFootPosition().Z < 17.f);
	Advance(Game, {}, 30);
	CHECK(Game.IsPlayerCrouched());
	CHECK(Game.GetPlayerFootPosition().Y == doctest::Approx(0.f).epsilon(0.05));

	// Crouch-walking out from under it lets the player stand.
	for (int Frame = 0; Frame < 600 && Game.IsPlayerCrouched(); ++Frame)
	{
		REQUIRE(Game.Update(1.f / 60.f, {.Move = {0.f, 1.f}}));
	}
	CHECK(Game.GetPlayerFootPosition().Z > 18.f);
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
	Advance(Game, {}, 20);
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
