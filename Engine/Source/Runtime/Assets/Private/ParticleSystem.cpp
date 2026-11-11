#include "ParticleSerialization.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>

namespace Herta
{
namespace
{
[[nodiscard]] bool IsFinite(const float Value)
{
	return std::isfinite(Value);
}

void WriteFloats(FBinaryWriter& Writer, const std::span<const float> Values)
{
	for (const float Value : Values)
	{
		Writer.WriteFloat(Value);
	}
}

void ReadFloats(FBinaryReader& Reader, const std::span<float> Values)
{
	for (float& Value : Values)
	{
		Value = Reader.ReadFloat();
	}
}
}

std::expected<void, FAssetError> ValidateParticleSystem(const FCookedParticleSystem& System)
{
	if (System.Emitters.empty() || System.Emitters.size() > MaximumParticleEmitters)
	{
		return std::unexpected(FAssetError{std::format("A particle system has 1 to {} emitters", MaximumParticleEmitters)});
	}

	for (const FParticleEmitterDesc& Emitter : System.Emitters)
	{
		const auto Fail = [&Emitter](const std::string_view Problem)
		{
			return std::unexpected(FAssetError{std::format("Emitter '{}': {}", Emitter.Name, Problem)});
		};

		const std::array Numbers{Emitter.DurationSeconds, Emitter.SpawnRate, Emitter.SpawnRadius, Emitter.ConeAngle, Emitter.SpeedMinimum, Emitter.SpeedMaximum, Emitter.LifetimeMinimum, Emitter.LifetimeMaximum, Emitter.SizeStart, Emitter.SizeEnd, Emitter.Emissive, Emitter.Gravity, Emitter.Drag, Emitter.Restitution, Emitter.Friction};
		if (!std::ranges::all_of(Numbers, IsFinite) || !std::ranges::all_of(Emitter.Direction, IsFinite) || !std::ranges::all_of(Emitter.ColorStart, IsFinite) || !std::ranges::all_of(Emitter.ColorEnd, IsFinite))
		{
			return Fail("every value must be finite");
		}

		if (Emitter.Name.empty() || Emitter.Name.size() > 64 || Emitter.MaximumParticles == 0 || Emitter.MaximumParticles > MaximumEmitterParticles)
		{
			return Fail(std::format("needs a short name and 1 to {} particles", MaximumEmitterParticles));
		}

		const auto Index = static_cast<std::uint32_t>(&Emitter - System.Emitters.data());
		const bool bFed = std::ranges::any_of(System.Emitters, [Index](const FParticleEmitterDesc& Other)
		{
			return std::ranges::contains(Other.Events, Index, &FParticleEventDesc::Emitter);
		});

		if (Emitter.SpawnRate < 0.f || Emitter.SpawnRate > 10000.f || Emitter.Burst > MaximumEmitterParticles || (Emitter.SpawnRate == 0.f && Emitter.Burst == 0 && !bFed) || Emitter.DurationSeconds < 0.f || Emitter.SpawnRadius < 0.f)
		{
			return Fail("spawns at a rate from 0 to 10000 per second or in a burst, for a duration of zero or more");
		}

		const float Length = std::hypot(Emitter.Direction[0], Emitter.Direction[1], Emitter.Direction[2]);
		if (std::abs(Length - 1.f) > 1e-3f || Emitter.ConeAngle < 0.f || Emitter.ConeAngle > 3.1416f || Emitter.SpeedMinimum < 0.f || Emitter.SpeedMaximum < Emitter.SpeedMinimum)
		{
			return Fail("needs a unit direction, a cone half-angle up to pi, and speeds from a minimum to a maximum");
		}

		if (Emitter.LifetimeMinimum <= 0.f || Emitter.LifetimeMaximum < Emitter.LifetimeMinimum || Emitter.LifetimeMaximum > 60.f || Emitter.SizeStart < 0.f || Emitter.SizeEnd < 0.f || Emitter.Emissive < 0.f)
		{
			return Fail("lives 0 to 60 seconds with sizes and emissive of zero or more");
		}

		if (Emitter.Drag < 0.f || Emitter.Restitution < 0.f || Emitter.Restitution > 1.f || Emitter.Friction < 0.f || Emitter.Friction > 1.f)
		{
			return Fail("drag is zero or more, and restitution and friction are fractions");
		}

		const bool bStages = Emitter.Stages.size() <= MaximumParticleStages && std::ranges::all_of(Emitter.Stages, [](const FParticleStageDesc& Stage)
		{
			return Stage.Start >= 0.f && Stage.Start < Stage.End && Stage.End <= 1.f && Stage.Modules.size() <= MaximumStageModules && std::ranges::all_of(Stage.Modules, [](const FParticleModuleDesc& Module)
			{
				return Module.Type <= EParticleModuleType::Turbulence && std::ranges::all_of(Module.Vector, IsFinite) && IsFinite(Module.Strength) && Module.Strength >= 0.f && Module.Strength <= 1000.f && Module.Radius > 0.f && Module.Radius <= 1000.f;
			});
		});

		if (!bStages)
		{
			return Fail(std::format("has at most {} stages, each a stretch of life from 0 to 1 with at most {} modules of strength 0 to 1000 and a positive radius", MaximumParticleStages, MaximumStageModules));
		}

		const bool bEvents = Emitter.Events.size() <= MaximumParticleEvents && std::ranges::all_of(Emitter.Events, [&](const FParticleEventDesc& Event)
		{
			return Event.Type <= EParticleEventType::Collision && Event.Emitter < System.Emitters.size() && Event.Emitter != Index && !System.Emitters[Event.Emitter].bRibbon && Event.Count >= 1 && Event.Count <= MaximumEventParticles && Event.InheritVelocity >= 0.f && Event.InheritVelocity <= 1.f;
		});

		if (!bEvents)
		{
			return Fail(std::format("sends at most {} events, each 1 to {} particles into another emitter that is not a ribbon, inheriting a fraction of the velocity", MaximumParticleEvents, MaximumEventParticles));
		}
	}

	return {};
}

bool NeedsParticleStageFormat(const FCookedParticleSystem& System)
{
	return std::ranges::any_of(System.Emitters, [](const FParticleEmitterDesc& Emitter)
	{
		return !Emitter.Stages.empty() || !Emitter.Events.empty();
	});
}

void WriteParticleSystem(FBinaryWriter& Writer, const FCookedParticleSystem& System, const std::uint32_t Version)
{
	Writer.Write(static_cast<std::uint32_t>(System.Emitters.size()));
	for (const FParticleEmitterDesc& Emitter : System.Emitters)
	{
		Writer.WriteString(Emitter.Name);
		Writer.Write(Emitter.MaximumParticles);
		Writer.Write(Emitter.Burst);
		WriteFloats(Writer, std::array{Emitter.DurationSeconds, Emitter.SpawnRate, Emitter.SpawnRadius, Emitter.ConeAngle, Emitter.SpeedMinimum, Emitter.SpeedMaximum, Emitter.LifetimeMinimum, Emitter.LifetimeMaximum, Emitter.SizeStart, Emitter.SizeEnd, Emitter.Emissive, Emitter.Gravity, Emitter.Drag, Emitter.Restitution, Emitter.Friction});
		WriteFloats(Writer, Emitter.Direction);
		WriteFloats(Writer, Emitter.ColorStart);
		WriteFloats(Writer, Emitter.ColorEnd);
		Writer.Write(static_cast<std::uint8_t>((Emitter.bCollision ? 1 : 0) | (Emitter.bStick ? 2 : 0) | (Emitter.bRibbon ? 4 : 0)));
		if (Version < 10)
		{
			continue;
		}

		Writer.Write(static_cast<std::uint32_t>(Emitter.Stages.size()));
		for (const FParticleStageDesc& Stage : Emitter.Stages)
		{
			WriteFloats(Writer, std::array{Stage.Start, Stage.End});
			Writer.Write(static_cast<std::uint32_t>(Stage.Modules.size()));
			for (const FParticleModuleDesc& Module : Stage.Modules)
			{
				Writer.Write(static_cast<std::uint8_t>(Module.Type));
				WriteFloats(Writer, Module.Vector);
				WriteFloats(Writer, std::array{Module.Strength, Module.Radius});
			}
		}

		Writer.Write(static_cast<std::uint32_t>(Emitter.Events.size()));
		for (const FParticleEventDesc& Event : Emitter.Events)
		{
			Writer.Write(static_cast<std::uint8_t>(Event.Type));
			Writer.Write(Event.Emitter);
			Writer.Write(Event.Count);
			Writer.WriteFloat(Event.InheritVelocity);
		}
	}
}

FCookedParticleSystem ReadParticleSystem(FBinaryReader& Reader, const std::uint32_t Version)
{
	FCookedParticleSystem System;
	const auto Count = Reader.Read<std::uint32_t>();
	if (Count > MaximumParticleEmitters)
	{
		Reader.Invalidate();
		return System;
	}

	System.Emitters.resize(Count);
	for (FParticleEmitterDesc& Emitter : System.Emitters)
	{
		Emitter.Name = Reader.ReadString(64);
		Emitter.MaximumParticles = Reader.Read<std::uint32_t>();
		Emitter.Burst = Reader.Read<std::uint32_t>();
		std::array<float, 15> Numbers{};
		ReadFloats(Reader, Numbers);
		std::tie(Emitter.DurationSeconds, Emitter.SpawnRate, Emitter.SpawnRadius, Emitter.ConeAngle, Emitter.SpeedMinimum, Emitter.SpeedMaximum, Emitter.LifetimeMinimum, Emitter.LifetimeMaximum, Emitter.SizeStart, Emitter.SizeEnd, Emitter.Emissive, Emitter.Gravity, Emitter.Drag, Emitter.Restitution, Emitter.Friction) = std::tuple_cat(Numbers);
		ReadFloats(Reader, Emitter.Direction);
		ReadFloats(Reader, Emitter.ColorStart);
		ReadFloats(Reader, Emitter.ColorEnd);
		const auto Flags = Reader.Read<std::uint8_t>();
		Emitter.bCollision = (Flags & 1) != 0;
		Emitter.bStick = (Flags & 2) != 0;
		Emitter.bRibbon = (Flags & 4) != 0;
		if (Version < 10)
		{
			continue;
		}

		// Counts are bounded before anything is allocated for them.
		const auto Stages = Reader.Read<std::uint32_t>();
		if (Stages > MaximumParticleStages)
		{
			Reader.Invalidate();
			return System;
		}

		Emitter.Stages.resize(Stages);
		for (FParticleStageDesc& Stage : Emitter.Stages)
		{
			Stage.Start = Reader.ReadFloat();
			Stage.End = Reader.ReadFloat();
			const auto Modules = Reader.Read<std::uint32_t>();
			if (Modules > MaximumStageModules)
			{
				Reader.Invalidate();
				return System;
			}

			Stage.Modules.resize(Modules);
			for (FParticleModuleDesc& Module : Stage.Modules)
			{
				Module.Type = static_cast<EParticleModuleType>(Reader.Read<std::uint8_t>());
				ReadFloats(Reader, Module.Vector);
				Module.Strength = Reader.ReadFloat();
				Module.Radius = Reader.ReadFloat();
			}
		}

		const auto Events = Reader.Read<std::uint32_t>();
		if (Events > MaximumParticleEvents)
		{
			Reader.Invalidate();
			return System;
		}

		Emitter.Events.resize(Events);
		for (FParticleEventDesc& Event : Emitter.Events)
		{
			Event.Type = static_cast<EParticleEventType>(Reader.Read<std::uint8_t>());
			Event.Emitter = Reader.Read<std::uint32_t>();
			Event.Count = Reader.Read<std::uint32_t>();
			Event.InheritVelocity = Reader.ReadFloat();
		}
	}

	return System;
}
}
