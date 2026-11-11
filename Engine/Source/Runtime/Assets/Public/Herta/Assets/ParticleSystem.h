#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Herta
{
inline constexpr std::uint32_t MaximumParticleEmitters = 4;
inline constexpr std::uint32_t MaximumEmitterParticles = 4096;

// One emitter of a cooked particle system. Its features choose among fixed update kernels instead of running a per-particle program.
struct FParticleEmitterDesc
{
	bool operator==(const FParticleEmitterDesc&) const = default;

	std::string Name{};
	std::uint32_t MaximumParticles = 256;
	// Seconds the emitter spawns for; zero spawns until the effect is stopped.
	float DurationSeconds = 0.f;
	// Particles per second while spawning, and how many spawn at once when it starts.
	float SpawnRate = 0.f;
	std::uint32_t Burst = 0;
	// Meters around the effect's origin that particles start within.
	float SpawnRadius = 0.f;

	// Velocity within a cone around Direction, in the effect's space; Direction is a unit vector and the angle a half-angle in radians.
	std::array<float, 3> Direction{0.f, 1.f, 0.f};
	float ConeAngle = 0.5f;
	float SpeedMinimum = 1.f;
	float SpeedMaximum = 2.f;

	float LifetimeMinimum = 1.f;
	float LifetimeMaximum = 1.f;
	// Diameter in meters at birth and death, eased linearly.
	float SizeStart = 0.1f;
	float SizeEnd = 0.1f;
	// Linear albedo and opacity at birth and death. Emissive is luminance in nits added on top of the lit albedo, scaled by the color.
	std::array<float, 4> ColorStart{1.f, 1.f, 1.f, 1.f};
	std::array<float, 4> ColorEnd{1.f, 1.f, 1.f, 0.f};
	float Emissive = 0.f;

	// Scale of gravity, and the linear drag that slows particles, per second.
	float Gravity = 1.f;
	float Drag = 0.f;

	bool bCollision = false;
	// Fraction of the speed into a surface that bounces back, and of the speed along it that remains.
	float Restitution = 0.3f;
	float Friction = 0.5f;
	// Particles that hit something stay where they landed, such as blood drops.
	bool bStick = false;
	// Joins the emitter's particles, oldest first, into one continuous strip instead of drawing discs. Size is the strip's width, or a
	// multiple of the edge the effect is moved with.
	bool bRibbon = false;
};

struct FCookedParticleSystem
{
	bool operator==(const FCookedParticleSystem&) const = default;

	std::vector<FParticleEmitterDesc> Emitters{};
};

[[nodiscard]] std::expected<void, FAssetError> ValidateParticleSystem(const FCookedParticleSystem& System);
}
