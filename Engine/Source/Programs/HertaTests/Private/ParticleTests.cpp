#include "Herta/Assets/CookedAsset.h"
#include "Herta/Particles/ParticleWorld.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace Herta
{
namespace
{
constexpr FAssetId Puff{9, 1};
constexpr FAssetId Drops{9, 2};

[[nodiscard]] std::shared_ptr<FCookedParticleSystem> MakePuff()
{
	auto System = std::make_shared<FCookedParticleSystem>();
	System->Emitters.push_back({.Name = "Puff", .MaximumParticles = 64, .Burst = 20, .ConeAngle = 0.3f, .SpeedMinimum = 2.f, .SpeedMaximum = 3.f, .LifetimeMinimum = 0.5f, .LifetimeMaximum = 0.5f, .SizeStart = 0.1f, .SizeEnd = 0.5f, .ColorStart = {1.f, 0.5f, 0.25f, 1.f}, .ColorEnd = {1.f, 0.5f, 0.25f, 0.f}, .Gravity = 0.f});
	return System;
}

// Drops raining onto a floor at y = 0 that they stick to.
[[nodiscard]] std::shared_ptr<FCookedParticleSystem> MakeDrops(const bool bStick)
{
	auto System = std::make_shared<FCookedParticleSystem>();
	System->Emitters.push_back({.Name = "Drops", .MaximumParticles = 32, .Burst = 32, .ConeAngle = 0.f, .SpeedMinimum = 0.f, .SpeedMaximum = 0.f, .LifetimeMinimum = 5.f, .LifetimeMaximum = 5.f, .Gravity = 1.f, .bCollision = true, .Restitution = 0.5f, .Friction = 0.2f, .bStick = bStick});
	return System;
}

[[nodiscard]] std::optional<FParticleHit> HitFloor(const FVector3& From, const FVector3& To)
{
	if (From.Y >= 0.f && To.Y < 0.f)
	{
		const float Fraction = From.Y / (From.Y - To.Y);
		return FParticleHit{.Position = From + (To - From) * Fraction, .Normal = {0.f, 1.f, 0.f}};
	}

	return std::nullopt;
}
}

TEST_CASE("Particle systems round-trip as cooked assets and reject impossible emitters")
{
	const auto System = MakePuff();
	const auto Bytes = SerializeCookedAsset(*System);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	CHECK(std::get<FCookedParticleSystem>(*Loaded) == *System);

	FCookedParticleSystem Bad = *System;
	Bad.Emitters[0].Burst = 0;
	CHECK_FALSE(ValidateParticleSystem(Bad));
	Bad = *System;
	Bad.Emitters[0].Direction = {0.f, 2.f, 0.f};
	CHECK_FALSE(ValidateParticleSystem(Bad));
	Bad = *System;
	Bad.Emitters[0].LifetimeMaximum = 0.1f;
	CHECK_FALSE(ValidateParticleSystem(Bad));
	CHECK_FALSE(ValidateParticleSystem({}));
}

TEST_CASE("Ribbon emitters join their particles oldest first along the path the effect moved, spanning its edge")
{
	auto System = std::make_shared<FCookedParticleSystem>();
	System->Emitters.push_back({.Name = "Trail", .MaximumParticles = 64, .SpawnRate = 120.f, .ConeAngle = 0.f, .SpeedMinimum = 0.f, .SpeedMaximum = 0.f, .LifetimeMinimum = 0.1f, .LifetimeMaximum = 0.1f, .SizeStart = 1.f, .SizeEnd = 0.5f, .Gravity = 0.f, .bRibbon = true});
	const auto Bytes = SerializeCookedAsset(*System);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	CHECK(std::get<FCookedParticleSystem>(*Loaded).Emitters[0].bRibbon);

	constexpr FAssetId Trail{9, 3};
	FParticleWorld World;
	REQUIRE(World.AddSystem(Trail, System));
	const auto Effect = World.Spawn(Trail, {0.f, 0.f, 0.f});
	REQUIRE(Effect);
	World.Move(*Effect, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});

	// One point is not yet a strip.
	World.Update(1.f / 120.f);
	CHECK(World.GetRenderRibbons().empty());
	CHECK(World.GetRenderRibbonPoints().empty());
	CHECK(World.GetRenderParticles().empty());

	// Moving one meter in an update lays both of its points along the way, not all at the end.
	World.Update(1.f / 120.f);
	REQUIRE(World.GetRenderRibbons().size() == 1);
	World.Move(*Effect, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
	World.Update(1.f / 60.f);
	REQUIRE(World.GetRenderRibbons().size() == 1);
	const std::span<const FRenderRibbonPoint> Points = World.GetRenderRibbonPoints();
	REQUIRE(Points.size() == 4);
	CHECK(Points[1].Position.X == doctest::Approx(0.f));
	CHECK(Points[2].Position.X == doctest::Approx(0.5f));
	CHECK(Points[3].Position.X == doctest::Approx(1.f));
	CHECK(Points[3].Edge.Y == doctest::Approx(1.f));
	// The oldest points have aged the most, so sizes and order agree.
	CHECK(Points[0].Size < Points[3].Size);

	// Old points die first and the rest stay in order.
	World.Update(1.f / 60.f);
	World.Update(1.f / 60.f);
	World.Update(1.f / 60.f);
	const std::span<const FRenderRibbonPoint> Later = World.GetRenderRibbonPoints();
	REQUIRE(Later.size() >= 2);
	for (std::size_t Index = 1; Index < Later.size(); ++Index)
	{
		CHECK(Later[Index - 1].Size <= Later[Index].Size);
	}

	// A stopped ribbon fades out and frees itself.
	World.Stop(*Effect);
	for (int Frame = 0; Frame < 20; ++Frame)
	{
		World.Update(1.f / 60.f);
	}

	CHECK_FALSE(World.IsAlive(*Effect));
	CHECK(World.GetRenderRibbons().empty());
}

TEST_CASE("Particle effects burst, age, fade, and free themselves when they end")
{
	FParticleWorld World;
	REQUIRE(World.AddSystem(Puff, MakePuff()));
	CHECK_FALSE(World.Spawn(FAssetId{9, 99}, {}));
	const auto Effect = World.Spawn(Puff, {0.f, 1.f, 0.f});
	REQUIRE(Effect);
	World.Update(1.f / 60.f);
	REQUIRE(World.GetRenderParticles().size() == 20);
	CHECK(World.GetStats().Particles == 20);
	for (const FRenderParticle& Particle : World.GetRenderParticles())
	{
		CHECK(Particle.Position.Y > 1.f);
		CHECK(Particle.Color[3] > 0.9f);
	}

	for (int Frame = 0; Frame < 20; ++Frame)
	{
		World.Update(1.f / 60.f);
	}

	// Two thirds through their lives they have grown and faded.
	REQUIRE_FALSE(World.GetRenderParticles().empty());
	CHECK(World.GetRenderParticles()[0].Size > 0.3f);
	CHECK(World.GetRenderParticles()[0].Color[3] < 0.4f);
	for (int Frame = 0; Frame < 20; ++Frame)
	{
		World.Update(1.f / 60.f);
	}

	CHECK(World.GetRenderParticles().empty());
	CHECK_FALSE(World.IsAlive(*Effect));
	CHECK(World.GetStats().Effects == 0);
	CHECK(World.GetStats().EffectsRetired == 1);

	// Turned upside down, the cone points down.
	(void)World.Spawn(Puff, {0.f, 1.f, 0.f}, {0.f, -1.f, 0.f});
	World.Update(0.05f);
	CHECK(std::ranges::all_of(World.GetRenderParticles(), [](const FRenderParticle& Particle)
	{
		return Particle.Position.Y < 1.f;
	}));
}

TEST_CASE("Particle budgets drop what does not fit, and looping effects stop when told")
{
	FParticleWorld World;
	auto Fountain = std::make_shared<FCookedParticleSystem>();
	Fountain->Emitters.push_back({.Name = "Fountain", .MaximumParticles = 100, .SpawnRate = 600.f, .LifetimeMinimum = 10.f, .LifetimeMaximum = 10.f, .Gravity = 0.f});
	REQUIRE(World.AddSystem(Puff, Fountain));
	const auto Effect = World.Spawn(Puff, {});
	REQUIRE(Effect);
	for (int Frame = 0; Frame < 60; ++Frame)
	{
		World.Update(1.f / 60.f);
	}

	CHECK(World.GetStats().Particles == 100);
	CHECK(World.GetStats().Dropped >= 400);
	World.Stop(*Effect);
	for (int Frame = 0; Frame < 45; ++Frame)
	{
		World.Update(0.25f);
	}

	CHECK_FALSE(World.IsAlive(*Effect));

	for (std::uint32_t Index = 0; Index < MaximumParticleEffects; ++Index)
	{
		REQUIRE(World.Spawn(Puff, {}));
	}

	CHECK_FALSE(World.Spawn(Puff, {}));
	World.Clear();
	CHECK(World.GetStats().Effects == 0);
	CHECK(World.GetStats().Particles == 0);
}

TEST_CASE("Colliding particles bounce off or stick to what they hit, and replay the same")
{
	const auto Run = [](const bool bStick)
	{
		FParticleWorld World;
		REQUIRE(World.AddSystem(Drops, MakeDrops(bStick)));
		(void)World.Spawn(Drops, {0.f, 2.f, 0.f}, {0.f, 1.f, 0.f});
		for (int Frame = 0; Frame < 180; ++Frame)
		{
			World.Update(1.f / 60.f, HitFloor);
		}

		return std::vector<FRenderParticle>(World.GetRenderParticles().begin(), World.GetRenderParticles().end());
	};

	const auto Stuck = Run(true);
	REQUIRE(Stuck.size() == 32);
	CHECK(std::ranges::all_of(Stuck, [](const FRenderParticle& Particle)
	{
		return Particle.Position.Y >= 0.f && Particle.Position.Y < 0.02f;
	}));

	const auto Bounced = Run(false);
	CHECK(std::ranges::all_of(Bounced, [](const FRenderParticle& Particle)
	{
		return Particle.Position.Y >= 0.f;
	}));

	const auto Again = Run(true);
	REQUIRE(Again.size() == Stuck.size());
	for (std::size_t Index = 0; Index < Stuck.size(); ++Index)
	{
		CHECK(Again[Index].Position == Stuck[Index].Position);
	}
}
}
