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
	    .PreviousPosition = Position,
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

void FParticleWorld::Move(const FParticleEffectId Effect, const FVector3& Position, const FVector3& Edge)
{
	for (FEffect& Each : Effects)
	{
		if (Each.Id == Effect)
		{
			Each.Position = Position;
			Each.Edge = Edge;
			// Before its first update an effect has no path yet, so a ribbon starts at full width where it is placed.
			if (Each.Age == 0.f)
			{
				Each.PreviousPosition = Position;
				Each.PreviousEdge = Edge;
			}
		}
	}
}

void FParticleWorld::Clear()
{
	Effects.clear();
	RenderParticles.clear();
	RenderRibbons.clear();
	RenderRibbonPoints.clear();
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

void FParticleWorld::SpawnParticles(FEffect& Effect, FEmitterState& Emitter, std::uint32_t Count, const FPendingEvent* const Event)
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
		// A ribbon's points spread along the way the effect moved this update, so a fast source leaves no gaps.
		const float Along = Desc.bRibbon ? static_cast<float>(Particle + 1) / static_cast<float>(Allowed) : 1.f;
		const FVector3 Origin = Event ? Event->Position : Effect.PreviousPosition + (Effect.Position - Effect.PreviousPosition) * Along;
		Emitter.Positions.push_back(Origin + Orient(Offset * (Desc.SpawnRadius * Effect.Scale), Effect.Up));
		Emitter.Edges.push_back(Effect.PreviousEdge + (Effect.Edge - Effect.PreviousEdge) * Along);
		Emitter.Velocities.push_back(Orient(Local, Effect.Up) * Speed + (Event ? Event->Velocity : FVector3{}));
		Emitter.Ages.push_back(0.f);
		Emitter.Lifetimes.push_back(std::lerp(Desc.LifetimeMinimum, Desc.LifetimeMaximum, NextUnit(Effect.Random)));
		Emitter.Resting.push_back(0);
	}

	LiveParticles += Allowed;
	Totals.Spawned += Allowed;
}

void FParticleWorld::RunStages(const FEffect& Effect, FEmitterState& Emitter, const float Step)
{
	for (const FParticleStageDesc& Stage : Emitter.Desc->Stages)
	{
		// One module at a time over every particle in the stage, so each is a tight loop rather than a per-particle program.
		for (const FParticleModuleDesc& Module : Stage.Modules)
		{
			const FVector3 Vector = Orient(ToVector(Module.Vector), Effect.Up);
			const FVector3 Axis = Vector.LengthSquared() > 1e-8f ? Vector.Normalized() : FVector3{0.f, 1.f, 0.f};
			const float Damping = std::exp(-Module.Strength * Step);
			const float Frequency = 1.f / (Module.Radius * Effect.Scale);
			for (std::size_t Index = 0; Index < Emitter.Positions.size(); ++Index)
			{
				const float Life = Emitter.Ages[Index] / Emitter.Lifetimes[Index];
				if (Emitter.Resting[Index] != 0 || Life < Stage.Start || Life >= Stage.End)
				{
					continue;
				}

				FVector3& Velocity = Emitter.Velocities[Index];
				const FVector3 Offset = Emitter.Positions[Index] - Effect.Position;
				switch (Module.Type)
				{
					case EParticleModuleType::Force:
						Velocity += Axis * (Module.Strength * Step);
						break;
					case EParticleModuleType::Drag:
						Velocity = Velocity * Damping;
						break;
					case EParticleModuleType::Vortex:
						Velocity += Axis.Cross(Offset) * (Module.Strength * Step);
						break;
					case EParticleModuleType::Attract:
					{
						const FVector3 Toward = Effect.Position + Vector * Effect.Scale - Emitter.Positions[Index];
						Velocity += Toward.LengthSquared() > 1e-8f ? Toward.Normalized() * (Module.Strength * Step) : FVector3{};
						break;
					}
					case EParticleModuleType::Turbulence:
					{
						// Sines of mixed coordinates: smooth, cheap, deterministic, and different on each axis.
						const FVector3 P = Emitter.Positions[Index] * Frequency;
						const float Time = Effect.Age;
						const FVector3 Push{std::sin(1.7f * P.Y + 2.3f * P.Z + 0.5f * Time), std::sin(2.9f * P.Z + 1.3f * P.X + 0.7f * Time), std::sin(2.1f * P.X + 1.9f * P.Y + 0.3f * Time)};
						Velocity += Push * (Module.Strength * Step);
						break;
					}
				}
			}
		}
	}
}

void FParticleWorld::QueueEvents(const FEmitterState& Emitter, const EParticleEventType Type, const FVector3& Position, const FVector3& Velocity)
{
	for (const FParticleEventDesc& Event : Emitter.Desc->Events)
	{
		if (Event.Type != Type)
		{
			continue;
		}

		if (EventBudget == 0)
		{
			++Totals.Dropped;
			continue;
		}

		--EventBudget;
		++Totals.Events;
		PendingEvents.push_back({.Emitter = Event.Emitter, .Count = Event.Count, .Position = Position, .Velocity = Velocity * Event.InheritVelocity});
	}
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
	EventBudget = MaximumParticleEventsPerUpdate;
	for (FEffect& Effect : Effects)
	{
		PendingEvents.clear();
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

			RunStages(Effect, Emitter, Step);

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
						QueueEvents(Emitter, EParticleEventType::Collision, Emitter.Positions[Index], Velocity);
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

				QueueEvents(Emitter, EParticleEventType::Death, Emitter.Positions[Index], Emitter.Velocities[Index]);

				// A ribbon's points stay in spawn order, which is the strip's order; particles can swap, which is cheaper.
				const auto Remove = [Index, bKeepOrder = Desc.bRibbon](auto& Values)
				{
					if (bKeepOrder)
					{
						Values.erase(Values.begin() + static_cast<std::ptrdiff_t>(Index));
					}
					else
					{
						SwapRemove(Values, Index);
					}
				};

				Remove(Emitter.Positions);
				Remove(Emitter.Velocities);
				Remove(Emitter.Ages);
				Remove(Emitter.Lifetimes);
				Remove(Emitter.Resting);
				Remove(Emitter.Edges);
				--LiveParticles;
			}
		}

		// Events spawn after every emitter has moved, so a particle born from one first moves next update.
		for (const FPendingEvent& Event : PendingEvents)
		{
			if (Event.Emitter < Effect.Emitters.size())
			{
				SpawnParticles(Effect, Effect.Emitters[Event.Emitter], Event.Count, &Event);
			}
		}

		Effect.Age += Step;
		Effect.PreviousPosition = Effect.Position;
		Effect.PreviousEdge = Effect.Edge;
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
	RenderRibbons.clear();
	RenderRibbonPoints.clear();
	for (const FEffect& Effect : Effects)
	{
		for (const FEmitterState& Emitter : Effect.Emitters)
		{
			const FParticleEmitterDesc& Desc = *Emitter.Desc;
			const auto FirstPoint = static_cast<std::uint32_t>(RenderRibbonPoints.size());
			for (std::size_t Index = 0; Index < Emitter.Positions.size(); ++Index)
			{
				const float Alpha = std::clamp(Emitter.Ages[Index] / Emitter.Lifetimes[Index], 0.f, 1.f);
				const float Size = std::lerp(Desc.SizeStart, Desc.SizeEnd, Alpha) * Effect.Scale;
				std::array<float, 4> Color{};
				for (std::size_t Channel = 0; Channel < 4; ++Channel)
				{
					Color[Channel] = std::lerp(Desc.ColorStart[Channel], Desc.ColorEnd[Channel], Alpha);
				}

				if (Desc.bRibbon)
				{
					RenderRibbonPoints.push_back({.Position = Emitter.Positions[Index], .Edge = Emitter.Edges[Index], .Size = Size, .Color = Color, .Emissive = Desc.Emissive});
				}
				else
				{
					RenderParticles.push_back({.Position = Emitter.Positions[Index], .Size = Size, .Color = Color, .Emissive = Desc.Emissive});
				}
			}

			const auto Points = static_cast<std::uint32_t>(RenderRibbonPoints.size()) - FirstPoint;
			if (Points >= 2)
			{
				RenderRibbons.push_back({.FirstPoint = FirstPoint, .PointCount = Points});
			}
			else
			{
				RenderRibbonPoints.resize(FirstPoint);
			}
		}
	}
}

std::span<const FRenderParticle> FParticleWorld::GetRenderParticles() const noexcept
{
	return RenderParticles;
}

std::span<const FRenderRibbon> FParticleWorld::GetRenderRibbons() const noexcept
{
	return RenderRibbons;
}

std::span<const FRenderRibbonPoint> FParticleWorld::GetRenderRibbonPoints() const noexcept
{
	return RenderRibbonPoints;
}

FParticleStats FParticleWorld::GetStats() const
{
	FParticleStats Stats = Totals;
	Stats.Effects = static_cast<std::uint32_t>(Effects.size());
	Stats.Particles = LiveParticles;
	return Stats;
}
}
