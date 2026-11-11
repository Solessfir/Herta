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
inline constexpr std::uint32_t MaximumParticleStages = 4;
inline constexpr std::uint32_t MaximumStageModules = 8;
inline constexpr std::uint32_t MaximumParticleEvents = 4;
inline constexpr std::uint32_t MaximumEventParticles = 64;

enum class EParticleModuleType : std::uint8_t
{
	// Accelerates particles along Vector, in the effect's space, by Strength meters per second squared.
	Force,
	// Slows particles by Strength per second, on top of the emitter's drag.
	Drag,
	// Swirls particles about the axis Vector through the effect's origin, faster the farther out, by Strength per second squared.
	Vortex,
	// Pulls particles toward Vector from the effect's origin by Strength meters per second squared.
	Attract,
	// Pushes particles by a smooth field that changes every Radius meters, by up to Strength meters per second squared.
	Turbulence,
};

struct FParticleModuleDesc
{
	bool operator==(const FParticleModuleDesc&) const = default;

	EParticleModuleType Type = EParticleModuleType::Force;
	std::array<float, 3> Vector{0.f, 1.f, 0.f};
	float Strength = 1.f;
	float Radius = 1.f;
};

// A stretch of a particle's life, from Start to End as fractions of its lifetime, that runs its modules in order each update.
struct FParticleStageDesc
{
	bool operator==(const FParticleStageDesc&) const = default;

	float Start = 0.f;
	float End = 1.f;
	std::vector<FParticleModuleDesc> Modules{};
};

enum class EParticleEventType : std::uint8_t
{
	Death,
	Collision,
};

// When a particle dies or hits something, Count particles start in another emitter of the same effect where it was, carrying InheritVelocity
// of its velocity on top of their own.
struct FParticleEventDesc
{
	bool operator==(const FParticleEventDesc&) const = default;

	EParticleEventType Type = EParticleEventType::Death;
	std::uint32_t Emitter = 0;
	std::uint32_t Count = 1;
	float InheritVelocity = 0.f;
};

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

	// The emitter's module stack: stages run after gravity and drag, before particles move. An emitter that other emitters' events feed may
	// spawn nothing on its own.
	std::vector<FParticleStageDesc> Stages{};
	std::vector<FParticleEventDesc> Events{};
};

struct FCookedParticleSystem
{
	bool operator==(const FCookedParticleSystem&) const = default;

	std::vector<FParticleEmitterDesc> Emitters{};
};

[[nodiscard]] std::expected<void, FAssetError> ValidateParticleSystem(const FCookedParticleSystem& System);
}
