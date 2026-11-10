#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace Herta
{
namespace
{
class FNoEffectGameAssets final : public IGameAssets
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

constexpr FAssetId Dust{8, 1};
constexpr FAssetId Embers{8, 2};

[[nodiscard]] std::shared_ptr<const FCookedParticleSystem> MakeBurst(const bool bCollide)
{
	auto System = std::make_shared<FCookedParticleSystem>();
	System->Emitters.push_back({.Name = "Burst", .MaximumParticles = 64, .Burst = 16, .ConeAngle = 0.6f, .SpeedMinimum = 1.f, .SpeedMaximum = 2.f, .LifetimeMinimum = 1.f, .LifetimeMaximum = 1.5f, .Gravity = 1.f, .bCollision = bCollide});
	return System;
}

[[nodiscard]] std::shared_ptr<const FCookedParticleSystem> MakeLoop()
{
	auto System = std::make_shared<FCookedParticleSystem>();
	System->Emitters.push_back({.Name = "Loop", .MaximumParticles = 128, .SpawnRate = 30.f, .LifetimeMinimum = 0.5f, .LifetimeMaximum = 0.5f, .Emissive = 50.f, .Gravity = -0.2f});
	return System;
}

[[nodiscard]] std::vector<FLevelEntity> MakeEffectLevel()
{
	return {
	    FLevelEntity{.Id = FObjectId{8, 10}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{8, 11}, .Name = "Crate", .Transform = {.Translation = {2., 4., 2.}, .Scale = {0.5f, 0.5f, 0.5f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Dynamic, .BodySettings = {.MassKg = 30.f}},
	    FLevelEntity{.Id = FObjectId{8, 12}, .Name = "Brazier", .Transform = {.Translation = {-3., 1., 3.}}, .ParticleEmitter = FParticleEmitterComponent{.System = Embers}},
	};
}

[[nodiscard]] std::vector<FRenderParticle> PlayEffects()
{
	const FNoEffectGameAssets Assets;
	auto Session = FGameSession::Create(MakeEffectLevel(), Assets, nullptr);
	REQUIRE(Session);
	FLevelEffects Effects{};
	Effects[static_cast<std::size_t>(ELevelEffect::Impact)] = Dust;
	(*Session)->SetLevelEffects(Effects);
	CHECK((*Session)->GetParticleSystems() == std::vector{Dust, Embers});
	REQUIRE((*Session)->AddParticleSystem(Dust, MakeBurst(true)));
	REQUIRE((*Session)->AddParticleSystem(Embers, MakeLoop()));
	for (int Frame = 0; Frame < 70; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
	}

	const FParticleStats Stats = (*Session)->GetParticleStats();
	CHECK(Stats.Effects >= 2);
	CHECK(Stats.Spawned > 16);
	CHECK(Stats.Collisions > 0);
	const FMeshRenderView& View = (*Session)->GetRenderView({320, 180});
	return std::vector<FRenderParticle>(View.Particles.begin(), View.Particles.end());
}
}

TEST_CASE("Game sessions spawn effects from impacts and emitters, collide them with the world, and replay them the same")
{
	const std::vector<FRenderParticle> Particles = PlayEffects();
	REQUIRE_FALSE(Particles.empty());
	// The brazier's embers rise around it, and the crate's landing raises dust near the floor.
	CHECK(std::ranges::any_of(Particles, [](const FRenderParticle& Particle)
	{
		return Particle.Emissive > 0.f && std::abs(Particle.Position.X + 3.f) < 1.f;
	}));

	CHECK(std::ranges::any_of(Particles, [](const FRenderParticle& Particle)
	{
		return Particle.Emissive == 0.f && Particle.Position.Y > -0.05f && std::abs(Particle.Position.X - 2.f) < 2.f;
	}));

	const std::vector<FRenderParticle> Again = PlayEffects();
	REQUIRE(Again.size() == Particles.size());
	for (std::size_t Index = 0; Index < Particles.size(); ++Index)
	{
		CHECK(Again[Index].Position == Particles[Index].Position);
	}
}
}
