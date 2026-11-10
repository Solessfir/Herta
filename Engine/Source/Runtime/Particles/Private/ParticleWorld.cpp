#include "Herta/Particles/ParticleWorld.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float GravityAcceleration = 9.81f;
// Below this speed after a bounce a particle comes to rest instead of jittering on the surface.
constexpr float RestSpeed = 0.25f;

[[nodiscard]] float NextUnit(std::uint32_t& State)
{
	State ^= State << 13;
	State ^= State >> 17;
	State ^= State << 5;
	return static_cast<float>(State >> 8) / static_cast<float>(1u << 24);
}

[[nodiscard]] FVector3 ToVector(const std::array<float, 3>& Values)
{
	return {Values[0], Values[1], Values[2]};
}

// Rotates a vector from +Y to Up along the shortest arc.
[[nodiscard]] FVector3 Orient(const FVector3& Vector, const FVector3& Up)
{
	const FVector3 Axis = FVector3{0.f, 1.f, 0.f}.Cross(Up);
	const float Cosine = Up.Y;
	const float Sine = Axis.Length();
	if (Sine < 1e-5f)
	{
		return Cosine >= 0.f ? Vector : FVector3{Vector.X, -Vector.Y, -Vector.Z};
	}

	const FVector3 Unit = Axis * (1.f / Sine);
	return Vector * Cosine + Unit.Cross(Vector) * Sine + Unit * (Unit.Dot(Vector) * (1.f - Cosine));
}

template <typename T>
void SwapRemove(std::vector<T>& Values, const std::size_t Index)
{
	Values[Index] = Values.back();
	Values.pop_back();
}
}

std::expected<void, FParticleError> FParticleWorld::AddSystem(const FAssetId& Id, std::shared_ptr<const FCookedParticleSystem> System)
{
	if (!Id.IsValid() || !System)
	{
		return std::unexpected(FParticleError{"A particle system needs an ID and data"});
	}

	if (auto Valid = ValidateParticleSystem(*System); !Valid)
	{
		return std::unexpected(FParticleError{Valid.error().Message});
	}

	// Effects already playing keep the version they started with.
	Systems[Id] = std::move(System);
	return {};
}

bool FParticleWorld::HasSystem(const FAssetId& Id) const
{
	return Systems.contains(Id);
}

std::optional<FParticleEffectId> FParticleWorld::Spawn(const FAssetId& System, const FVector3& Position, const FVector3& Up, const float Scale)
{
	const auto Found = Systems.find(System);
	if (Found == Systems.end())
	{
		return std::nullopt;
	}

	if (Effects.size() >= MaximumParticleEffects)
	{
		++Totals.Dropped;
		return std::nullopt;
	}

	FEffect Effect{
	    .Id = {++NextEffect},
	    .System = Found->second,
	    .Position = Position,
	    .Up = Up.LengthSquared() > 1e-8f ? Up.Normalized() : FVector3{0.f, 1.f, 0.f},
	    .Scale = std::isfinite(Scale) && Scale > 0.f ? Scale : 1.f,
	    .Random = (NextEffect * 2654435761u) ^ 0x6a09e667u,
	};

	Effect.Random = Effect.Random == 0 ? 1 : Effect.Random;
	for (const FParticleEmitterDesc& Desc : Effect.System->Emitters)
	{
		Effect.Emitters.push_back({.Desc = &Desc});
	}

	Effects.push_back(std::move(Effect));
	return Effects.back().Id;
}

void FParticleWorld::Stop(const FParticleEffectId Effect)
{
	for (FEffect& Each : Effects)
	{
		Each.bStopped = Each.bStopped || Each.Id == Effect;
	}
}

void FParticleWorld::Move(const FParticleEffectId Effect, const FVector3& Position)
{
	for (FEffect& Each : Effects)
	{
		if (Each.Id == Effect)
		{
			Each.Position = Position;
		}
	}
}

void FParticleWorld::Clear()
{
	Effects.clear();
	RenderParticles.clear();
	LiveParticles = 0;
}

bool FParticleWorld::IsAlive(const FParticleEffectId Effect) const
{
	return std::ranges::contains(Effects, Effect, &FEffect::Id);
}

// A burst spawns once; a rate spawns for the duration, or until stopped when the duration is zero.
bool FParticleWorld::IsSpawning(const FEffect& Effect, const FEmitterState& Emitter)
{
	const FParticleEmitterDesc& Desc = *Emitter.Desc;
	const bool bBurstPending = !Emitter.bBurstDone && Desc.Burst > 0;
	const bool bRate = Desc.SpawnRate > 0.f && (Desc.DurationSeconds == 0.f || Effect.Age < Desc.DurationSeconds);
	return !Effect.bStopped && (bBurstPending || bRate);
}

void FParticleWorld::SpawnParticles(FEffect& Effect, FEmitterState& Emitter, std::uint32_t Count)
{
	const FParticleEmitterDesc& Desc = *Emitter.Desc;
	const auto Room = static_cast<std::uint32_t>(Desc.MaximumParticles - std::min<std::size_t>(Emitter.Positions.size(), Desc.MaximumParticles));
	const std::uint32_t Allowed = std::min({Count, Room, MaximumLiveParticles - LiveParticles});
	Totals.Dropped += Count - Allowed;
	for (std::uint32_t Particle = 0; Particle < Allowed; ++Particle)
	{
		// A uniform point in a sphere and a uniform direction in a cone around the emitter's direction.
		FVector3 Offset{};
		do
		{
			Offset = FVector3{NextUnit(Effect.Random), NextUnit(Effect.Random), NextUnit(Effect.Random)} * 2.f - FVector3{1.f, 1.f, 1.f};
		} while (Offset.LengthSquared() > 1.f);

		const float CosAngle = 1.f - NextUnit(Effect.Random) * (1.f - std::cos(Desc.ConeAngle));
		const float SinAngle = std::sqrt(std::max(1.f - CosAngle * CosAngle, 0.f));
		const float Turn = NextUnit(Effect.Random) * 2.f * std::numbers::pi_v<float>;
		const FVector3 Local = Orient(FVector3{std::cos(Turn) * SinAngle, CosAngle, std::sin(Turn) * SinAngle}, ToVector(Desc.Direction));
		const float Speed = std::lerp(Desc.SpeedMinimum, Desc.SpeedMaximum, NextUnit(Effect.Random)) * Effect.Scale;
		Emitter.Positions.push_back(Effect.Position + Orient(Offset * (Desc.SpawnRadius * Effect.Scale), Effect.Up));
		Emitter.Velocities.push_back(Orient(Local, Effect.Up) * Speed);
		Emitter.Ages.push_back(0.f);
		Emitter.Lifetimes.push_back(std::lerp(Desc.LifetimeMinimum, Desc.LifetimeMaximum, NextUnit(Effect.Random)));
		Emitter.Resting.push_back(0);
	}

	LiveParticles += Allowed;
	Totals.Spawned += Allowed;
}

void FParticleWorld::Update(const float DeltaSeconds, const FParticleCollider& Collide)
{
	const float Step = std::isfinite(DeltaSeconds) ? std::clamp(DeltaSeconds, 0.f, 0.25f) : 0.f;
	// A paused frame changes nothing, so it sweeps nothing and keeps the last render list.
	if (Step == 0.f)
	{
		return;
	}
	std::uint32_t CollisionBudget = MaximumParticleCollisionsPerUpdate;
	for (FEffect& Effect : Effects)
	{
		for (FEmitterState& Emitter : Effect.Emitters)
		{
			const FParticleEmitterDesc& Desc = *Emitter.Desc;
			if (IsSpawning(Effect, Emitter))
			{
				std::uint32_t Count = 0;
				if (!Emitter.bBurstDone)
				{
					Count += Desc.Burst;
					Emitter.bBurstDone = true;
				}

				Emitter.SpawnCarry += Desc.SpawnRate * Step;
				const auto Whole = static_cast<std::uint32_t>(Emitter.SpawnCarry);
				Emitter.SpawnCarry -= static_cast<float>(Whole);
				SpawnParticles(Effect, Emitter, Count + Whole);
			}

			// The kernel: drag and gravity for everything, then a sweep for emitters that collide.
			const float Damping = std::exp(-Desc.Drag * Step);
			const FVector3 Fall{0.f, -GravityAcceleration * Desc.Gravity * Step, 0.f};
			const bool bCollide = Desc.bCollision && static_cast<bool>(Collide);
			for (std::size_t Index = 0; Index < Emitter.Positions.size(); ++Index)
			{
				if (Emitter.Resting[Index] != 0)
				{
					continue;
				}

				FVector3& Velocity = Emitter.Velocities[Index];
				Velocity = (Velocity + Fall) * Damping;
				const FVector3 Next = Emitter.Positions[Index] + Velocity * Step;
				if (bCollide && CollisionBudget > 0)
				{
					--CollisionBudget;
					++Totals.Collisions;
					if (const auto Hit = Collide(Emitter.Positions[Index], Next))
					{
						const FVector3 Normal = Hit->Normal;
						const FVector3 Into = Normal * Velocity.Dot(Normal);
						const FVector3 Along = Velocity - Into;
						Velocity = Along * (1.f - Desc.Friction) - Into * Desc.Restitution;
						Emitter.Positions[Index] = Hit->Position + Normal * 0.005f;
						if (Desc.bStick || Velocity.Length() < RestSpeed)
						{
							Velocity = {};
							Emitter.Resting[Index] = 1;
						}

						continue;
					}
				}

				Emitter.Positions[Index] = Next;
			}

			for (std::size_t Index = 0; Index < Emitter.Ages.size();)
			{
				Emitter.Ages[Index] += Step;
				if (Emitter.Ages[Index] < Emitter.Lifetimes[Index])
				{
					++Index;
					continue;
				}

				SwapRemove(Emitter.Positions, Index);
				SwapRemove(Emitter.Velocities, Index);
				SwapRemove(Emitter.Ages, Index);
				SwapRemove(Emitter.Lifetimes, Index);
				SwapRemove(Emitter.Resting, Index);
				--LiveParticles;
			}
		}

		Effect.Age += Step;
	}

	// An effect ends once it cannot spawn again and its last particle has died; its memory goes with it.
	const auto Finished = std::ranges::remove_if(Effects, [](const FEffect& Effect)
	{
		const bool bSpawning = std::ranges::any_of(Effect.Emitters, [&Effect](const FEmitterState& Emitter)
		{
			return IsSpawning(Effect, Emitter);
		});

		const bool bAlive = std::ranges::any_of(Effect.Emitters, [](const FEmitterState& Emitter)
		{
			return !Emitter.Positions.empty();
		});

		return !bSpawning && !bAlive;
	});

	Totals.EffectsRetired += static_cast<std::uint64_t>(std::ranges::distance(Finished));
	Effects.erase(Finished.begin(), Finished.end());

	RenderParticles.clear();
	for (const FEffect& Effect : Effects)
	{
		for (const FEmitterState& Emitter : Effect.Emitters)
		{
			const FParticleEmitterDesc& Desc = *Emitter.Desc;
			for (std::size_t Index = 0; Index < Emitter.Positions.size(); ++Index)
			{
				const float Alpha = std::clamp(Emitter.Ages[Index] / Emitter.Lifetimes[Index], 0.f, 1.f);
				FRenderParticle& Particle = RenderParticles.emplace_back();
				Particle.Position = Emitter.Positions[Index];
				Particle.Size = std::lerp(Desc.SizeStart, Desc.SizeEnd, Alpha) * Effect.Scale;
				for (std::size_t Channel = 0; Channel < 4; ++Channel)
				{
					Particle.Color[Channel] = std::lerp(Desc.ColorStart[Channel], Desc.ColorEnd[Channel], Alpha);
				}

				Particle.Emissive = Desc.Emissive;
			}
		}
	}
}

std::span<const FRenderParticle> FParticleWorld::GetRenderParticles() const noexcept
{
	return RenderParticles;
}

FParticleStats FParticleWorld::GetStats() const
{
	FParticleStats Stats = Totals;
	Stats.Effects = static_cast<std::uint32_t>(Effects.size());
	Stats.Particles = LiveParticles;
	return Stats;
}
}
