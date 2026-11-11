#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/ParticleSystem.h"
#include "Herta/Math/Vector.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
struct FParticleError
{
	std::string Message{};
};

struct FParticleHit
{
	FVector3 Position{};
	// Points away from the surface, back toward where the particle came from.
	FVector3 Normal{};
};

// Sweeps a particle from From to To against the world, such as a physics cast; empty when nothing is in the way.
using FParticleCollider = std::function<std::optional<FParticleHit>(const FVector3& From, const FVector3& To)>;

struct FParticleEffectId
{
	auto operator<=>(const FParticleEffectId&) const = default;

	std::uint32_t Value = 0;
};

inline constexpr std::uint32_t MaximumLiveParticles = 16384;
inline constexpr std::uint32_t MaximumParticleEffects = 256;
// Collision sweeps per update, so a burst of colliding particles never stalls a frame; the rest wait for the next update.
inline constexpr std::uint32_t MaximumParticleCollisionsPerUpdate = 512;

struct FParticleStats
{
	std::uint32_t Effects = 0;
	std::uint32_t Particles = 0;
	std::uint64_t Spawned = 0;
	// Particles and effects that did not start because a budget was full.
	std::uint64_t Dropped = 0;
	std::uint64_t EffectsRetired = 0;
	std::uint64_t Collisions = 0;
};

// Simulates particle effects on the CPU within fixed budgets. Each emitter runs one of a few fixed update kernels, chosen by whether it
// collides, instead of an interpreted per-particle program. Finished effects free their particles the update they end. Randomness comes
// from each effect's identity, so the same spawns and updates give the same particles.
class FParticleWorld final
{
public:
	[[nodiscard]] std::expected<void, FParticleError> AddSystem(const FAssetId& Id, std::shared_ptr<const FCookedParticleSystem> System);
	[[nodiscard]] bool HasSystem(const FAssetId& Id) const;
	// Starts an effect with its emitters' directions turned from +Y to Up and its speeds, sizes, and spawn radius scaled. Empty when the
	// system is unknown or the effect budget is full.
	std::optional<FParticleEffectId> Spawn(const FAssetId& System, const FVector3& Position, const FVector3& Up = {0.f, 1.f, 0.f}, float Scale = 1.f);
	// Stops spawning; particles already alive finish their lives.
	void Stop(FParticleEffectId Effect);
	// Ribbons lay new points along the way from the last position. Edge spans them across the path, such as half a sword's blade; zero faces
	// them to the camera.
	void Move(FParticleEffectId Effect, const FVector3& Position, const FVector3& Edge = {});
	void Clear();
	void Update(float DeltaSeconds, const FParticleCollider& Collide = {});

	[[nodiscard]] bool IsAlive(FParticleEffectId Effect) const;
	// Rebuilt by Update; stays valid until the next Update or Clear.
	[[nodiscard]] std::span<const FRenderParticle> GetRenderParticles() const noexcept;
	[[nodiscard]] std::span<const FRenderRibbon> GetRenderRibbons() const noexcept;
	[[nodiscard]] std::span<const FRenderRibbonPoint> GetRenderRibbonPoints() const noexcept;
	[[nodiscard]] FParticleStats GetStats() const;

private:
	struct FEmitterState
	{
		const FParticleEmitterDesc* Desc = nullptr;
		float SpawnCarry = 0.f;
		bool bBurstDone = false;
		std::vector<FVector3> Positions{};
		std::vector<FVector3> Velocities{};
		std::vector<float> Ages{};
		std::vector<float> Lifetimes{};
		std::vector<std::uint8_t> Resting{};
		std::vector<FVector3> Edges{};
	};

	struct FEffect
	{
		FParticleEffectId Id{};
		std::shared_ptr<const FCookedParticleSystem> System{};
		FVector3 Position{};
		// Where the last update left it, which ribbons lay this update's points from.
		FVector3 PreviousPosition{};
		FVector3 Edge{};
		FVector3 PreviousEdge{};
		FVector3 Up{0.f, 1.f, 0.f};
		float Scale = 1.f;
		float Age = 0.f;
		bool bStopped = false;
		std::uint32_t Random = 1;
		std::vector<FEmitterState> Emitters{};
	};

	[[nodiscard]] static bool IsSpawning(const FEffect& Effect, const FEmitterState& Emitter);
	void SpawnParticles(FEffect& Effect, FEmitterState& Emitter, std::uint32_t Count);

	std::map<FAssetId, std::shared_ptr<const FCookedParticleSystem>> Systems{};
	std::vector<FEffect> Effects{};
	std::vector<FRenderParticle> RenderParticles{};
	std::vector<FRenderRibbon> RenderRibbons{};
	std::vector<FRenderRibbonPoint> RenderRibbonPoints{};
	std::uint32_t NextEffect = 0;
	std::uint32_t LiveParticles = 0;
	FParticleStats Totals{};
};
}
