#include "Herta/Level/World.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <exception>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumEntityCount = 1'000'000;
constexpr std::size_t MaximumNameBytes = 1024;

struct FObjectIdHash
{
	std::size_t operator()(FObjectId Id) const noexcept;
};

constexpr std::uint64_t MixObjectIdBits(std::uint64_t Value) noexcept
{
	Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9;
	Value = (Value ^ (Value >> 27)) * 0x94d049bb133111eb;
	return Value ^ (Value >> 31);
}

std::size_t FObjectIdHash::operator()(const FObjectId Id) const noexcept
{
	// Mixing each half before ordered combination avoids collisions for IDs with identical halves.
	return static_cast<std::size_t>(MixObjectIdBits(MixObjectIdBits(Id.GetHigh()) ^ std::rotl(MixObjectIdBits(Id.GetLow()), 32)));
}

struct FEntityGeneration
{
	std::uint64_t Value;
};

struct FEntityName
{
	std::string Value;
};

struct FEntityParent
{
	FObjectId Value;
};

std::uint64_t AllocateWorldToken()
{
	static std::atomic<std::uint64_t> NextToken{1};
	const std::uint64_t Token = NextToken.fetch_add(1, std::memory_order_relaxed);
	if (Token == 0)
	{
		std::terminate();
	}

	return Token;
}

bool IsValidUtf8(const std::string_view Text)
{
	for (std::size_t Index = 0; Index < Text.size();)
	{
		const auto First = static_cast<unsigned char>(Text[Index++]);
		if (First < 0x20 || First == 0x7f)
		{
			return false;
		}

		if (First < 0x80)
		{
			continue;
		}

		std::uint32_t CodePoint = 0;
		std::size_t Remaining = 0;
		std::uint32_t Minimum = 0;
		if (First >= 0xc2 && First <= 0xdf)
		{
			CodePoint = First & 0x1fu;
			Remaining = 1;
			Minimum = 0x80;
		}
		else if (First >= 0xe0 && First <= 0xef)
		{
			CodePoint = First & 0x0fu;
			Remaining = 2;
			Minimum = 0x800;
		}
		else if (First >= 0xf0 && First <= 0xf4)
		{
			CodePoint = First & 0x07u;
			Remaining = 3;
			Minimum = 0x10000;
		}
		else
		{
			return false;
		}

		if (Remaining > Text.size() - Index)
		{
			return false;
		}

		for (std::size_t Byte = 0; Byte < Remaining; ++Byte)
		{
			const auto Continuation = static_cast<unsigned char>(Text[Index++]);
			if ((Continuation & 0xc0u) != 0x80u)
			{
				return false;
			}

			CodePoint = (CodePoint << 6) | (Continuation & 0x3fu);
		}

		if (CodePoint < Minimum || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff) || (CodePoint >= 0x80 && CodePoint <= 0x9f))
		{
			return false;
		}
	}

	return true;
}

std::expected<void, FLevelError> ValidateEntityProperties(const FLevelEntity& Entity)
{
	if (!Entity.Id.IsValid())
	{
		return std::unexpected(FLevelError{"Entity has an invalid object ID"});
	}

	if (Entity.Name.size() > MaximumNameBytes || !IsValidUtf8(Entity.Name))
	{
		return std::unexpected(FLevelError{"Entity name must be valid UTF-8 without control characters and at most 1024 bytes"});
	}

	const FVector3d& Position = Entity.Transform.Translation.Meters;
	const FVector3& Scale = Entity.Transform.Scale;
	const FQuaternion& Rotation = Entity.Transform.Rotation;
	const float RotationLengthSquared = Rotation.LengthSquared();
	if (!std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z)
	    || !std::isfinite(Scale.X) || !std::isfinite(Scale.Y) || !std::isfinite(Scale.Z)
	    || Scale.X <= 0.f || Scale.Y <= 0.f || Scale.Z <= 0.f
	    || !std::isfinite(Rotation.X) || !std::isfinite(Rotation.Y) || !std::isfinite(Rotation.Z) || !std::isfinite(Rotation.W)
	    || !std::isfinite(RotationLengthSquared) || RotationLengthSquared <= std::numeric_limits<float>::epsilon())
	{
		return std::unexpected(FLevelError{"Entity transform must be finite, with positive scale and a nondegenerate quaternion"});
	}

	if (Entity.Mesh && !Entity.Mesh->Asset.IsValid())
	{
		return std::unexpected(FLevelError{"Static mesh component has an invalid asset ID"});
	}

	if (Entity.Mesh && Entity.Mesh->Materials.size() > 256)
	{
		return std::unexpected(FLevelError{"Static mesh exceeds the limit of 256 material slots"});
	}

	if (Entity.Light)
	{
		const auto Valid = ValidateLightComponent(*Entity.Light);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.SkyAtmosphere)
	{
		const auto Valid = ValidateSkyAtmosphereComponent(*Entity.SkyAtmosphere);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.HeightFog)
	{
		const auto Valid = ValidateHeightFogComponent(*Entity.HeightFog);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.SoftBody)
	{
		const auto Valid = ValidateSoftBodyComponent(*Entity.SoftBody);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.Mover)
	{
		const auto Valid = ValidateMoverComponent(*Entity.Mover);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.Trigger)
	{
		const auto Valid = ValidateTriggerComponent(*Entity.Trigger);
		if (!Valid)
		{
			return Valid;
		}
	}

	if (Entity.BodyType != ELevelBodyType::None && Entity.BodyType != ELevelBodyType::Static && Entity.BodyType != ELevelBodyType::Dynamic)
	{
		return std::unexpected(FLevelError{"Entity has an unknown body type"});
	}

	const auto ValidBodySettings = ValidateLevelRigidBodySettings(Entity.BodySettings);
	if (!ValidBodySettings)
	{
		return ValidBodySettings;
	}

	if (Entity.BodyType == ELevelBodyType::None && Entity.BodySettings != FLevelRigidBodySettings{})
	{
		return std::unexpected(FLevelError{"Entity without a rigid body must use default body settings"});
	}

	return {};
}

TMatrix4<double> MakeLocalMatrix(const FLevelTransform& Transform)
{
	const FQuaternion& Rotation = Transform.Rotation;
	return TMatrix4<double>::Transform(Transform.Translation.Meters, TQuaternion<double>{Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}, FVector3d{Transform.Scale});
}
}

FObjectId FObjectId::Generate()
{
	const FAssetId Generated = FAssetId::Generate();
	return {Generated.GetHigh(), Generated.GetLow()};
}

std::optional<FObjectId> FObjectId::Parse(const std::string_view Text) noexcept
{
	const auto Parsed = FAssetId::Parse(Text);
	return Parsed ? std::optional<FObjectId>{{Parsed->GetHigh(), Parsed->GetLow()}} : std::nullopt;
}

std::string FObjectId::ToString() const
{
	return FAssetId{High, Low}.ToString();
}

std::expected<void, FLevelError> ValidateLevelRigidBodySettings(const FLevelRigidBodySettings& Settings)
{
	if (!std::isfinite(Settings.MassKg) || Settings.MassKg < 0.001f || Settings.MassKg > 1'000'000.f)
	{
		return std::unexpected(FLevelError{"Rigid body mass must be finite and between 0.001 and 1000000 kg"});
	}

	if (!std::isfinite(Settings.Friction) || Settings.Friction < 0.f || Settings.Friction > 1.f
	    || !std::isfinite(Settings.Restitution) || Settings.Restitution < 0.f || Settings.Restitution > 1.f
	    || !std::isfinite(Settings.LinearDamping) || Settings.LinearDamping < 0.f || Settings.LinearDamping > 1.f
	    || !std::isfinite(Settings.AngularDamping) || Settings.AngularDamping < 0.f || Settings.AngularDamping > 1.f)
	{
		return std::unexpected(FLevelError{"Rigid body friction, restitution, and damping must be finite and between 0 and 1"});
	}

	if (!std::isfinite(Settings.GravityScale) || Settings.GravityScale < 0.f || Settings.GravityScale > 10.f)
	{
		return std::unexpected(FLevelError{"Rigid body gravity scale must be finite and between 0 and 10"});
	}

	if (Settings.Collision > ELevelCollisionShape::Capsule)
	{
		return std::unexpected(FLevelError{"Unknown rigid body collision shape"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateLightComponent(const FLightComponent& Light)
{
	const auto InRange = [](const float Value, const float Minimum, const float Maximum)
	{
		return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
	};

	if (Light.Type > ELightType::Rect || !InRange(Light.Color.X, 0.f, 1.f) || !InRange(Light.Color.Y, 0.f, 1.f) || !InRange(Light.Color.Z, 0.f, 1.f)
	    || !InRange(Light.Intensity, 0.f, 1'000'000'000.f) || !InRange(Light.TemperatureKelvin, 1000.f, 40'000.f)
	    || !InRange(Light.ShadowBias, 0.f, 1.f) || !InRange(Light.ShadowNormalBias, 0.f, 10.f)
	    || !InRange(Light.Range, 0.001f, 1'000'000.f) || !InRange(Light.InnerConeAngle, 0.f, 1.553343f)
	    || !InRange(Light.OuterConeAngle, 0.001f, 1.553343f) || Light.InnerConeAngle > Light.OuterConeAngle
	    || !InRange(Light.Width, 0.001f, 10'000.f) || !InRange(Light.Height, 0.001f, 10'000.f) || !InRange(Light.AmbientStrength, 0.f, 100.f))
	{
		return std::unexpected(FLevelError{"Light settings contain an unknown type, nonfinite value, or out-of-range property"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateSkyAtmosphereComponent(const FSkyAtmosphereComponent& Atmosphere)
{
	if (!std::isfinite(Atmosphere.RayleighScattering) || Atmosphere.RayleighScattering < 0.f || Atmosphere.RayleighScattering > 100.f
	    || !std::isfinite(Atmosphere.MieScattering) || Atmosphere.MieScattering < 0.f || Atmosphere.MieScattering > 100.f
	    || !std::isfinite(Atmosphere.MieAnisotropy) || Atmosphere.MieAnisotropy < -0.99f || Atmosphere.MieAnisotropy > 0.99f
	    || !std::isfinite(Atmosphere.PlanetRadius) || Atmosphere.PlanetRadius < 1000.f || Atmosphere.PlanetRadius > 100'000'000.f
	    || !std::isfinite(Atmosphere.AtmosphereHeight) || Atmosphere.AtmosphereHeight < 1.f || Atmosphere.AtmosphereHeight > 1'000'000.f
	    || Atmosphere.AtmosphereHeight > Atmosphere.PlanetRadius)
	{
		return std::unexpected(FLevelError{"Atmosphere settings must be finite, in range, with atmosphere height no greater than planet radius"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateHeightFogComponent(const FHeightFogComponent& Fog)
{
	if (!std::isfinite(Fog.Density) || Fog.Density < 0.f || Fog.Density > 10.f
	    || !std::isfinite(Fog.HeightFalloff) || Fog.HeightFalloff < 0.f || Fog.HeightFalloff > 100.f
	    || !std::isfinite(Fog.Albedo.X) || Fog.Albedo.X < 0.f || Fog.Albedo.X > 1.f
	    || !std::isfinite(Fog.Albedo.Y) || Fog.Albedo.Y < 0.f || Fog.Albedo.Y > 1.f
	    || !std::isfinite(Fog.Albedo.Z) || Fog.Albedo.Z < 0.f || Fog.Albedo.Z > 1.f
	    || !std::isfinite(Fog.Anisotropy) || Fog.Anisotropy < -0.99f || Fog.Anisotropy > 0.99f
	    || !std::isfinite(Fog.MaxDistance) || Fog.MaxDistance < 0.1f || Fog.MaxDistance > 100'000.f || Fog.Quality > EFogQuality::High)
	{
		return std::unexpected(FLevelError{"Fog settings contain a nonfinite value, unknown quality, or out-of-range property"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateSoftBodyComponent(const FSoftBodyComponent& SoftBody)
{
	const auto InRange = [](const float Value, const float Minimum, const float Maximum)
	{
		return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
	};

	if (SoftBody.Shape > ESoftBodyShape::Ball || !InRange(SoftBody.Length, 0.1f, 50.f) || !InRange(SoftBody.Height, 0.1f, 50.f) || !InRange(SoftBody.Thickness, 0.005f, 1.f)
	    || !InRange(SoftBody.MassKg, 0.001f, 10'000.f) || !InRange(SoftBody.Stiffness, 0.f, 1.f) || !InRange(SoftBody.Pressure, 0.f, 1'000'000.f) || !InRange(SoftBody.Friction, 0.f, 1.f))
	{
		return std::unexpected(FLevelError{"Soft body settings contain a nonfinite value, unknown shape, or out-of-range property"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateMoverComponent(const FMoverComponent& Mover)
{
	const auto InRange = [](const float Value, const float Minimum, const float Maximum)
	{
		return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
	};

	if (!InRange(Mover.Offset.X, -1000.f, 1000.f) || !InRange(Mover.Offset.Y, -1000.f, 1000.f) || !InRange(Mover.Offset.Z, -1000.f, 1000.f) || !InRange(Mover.PeriodSeconds, 0.5f, 600.f))
	{
		return std::unexpected(FLevelError{"Mover offsets must be within 1000 m and periods between 0.5 and 600 s"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateTriggerComponent(const FTriggerComponent& Trigger)
{
	const auto InRange = [](const float Value)
	{
		return std::isfinite(Value) && Value >= 0.01f && Value <= 1000.f;
	};

	if (!InRange(Trigger.Size.X) || !InRange(Trigger.Size.Y) || !InRange(Trigger.Size.Z))
	{
		return std::unexpected(FLevelError{"Trigger sizes must be between 0.01 and 1000 m"});
	}

	return {};
}

std::expected<void, FLevelError> ValidateLevelEntities(const std::span<const FLevelEntity> Entities)
{
	if (Entities.size() > MaximumEntityCount)
	{
		return std::unexpected(FLevelError{"Level exceeds the limit of 1000000 entities"});
	}

	std::unordered_map<FObjectId, std::size_t, FObjectIdHash> Indices;
	Indices.reserve(Entities.size());

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		const auto Valid = ValidateEntityProperties(Entity);
		if (!Valid)
		{
			return Valid;
		}

		if (!Indices.emplace(Entity.Id, Index).second)
		{
			return std::unexpected(FLevelError{std::format("Duplicate object ID '{}'", Entity.Id.ToString())});
		}
	}

	std::vector<std::size_t> Parents(Entities.size(), Entities.size());

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const FObjectId Parent = Entities[Index].Parent;
		if (!Parent.IsValid())
		{
			continue;
		}

		const auto Found = Indices.find(Parent);
		if (Found == Indices.end())
		{
			return std::unexpected(FLevelError{std::format("Entity '{}' references missing parent '{}'", Entities[Index].Id.ToString(), Parent.ToString())});
		}

		Parents[Index] = Found->second;
	}

	// Iterative traversal also handles deep imported hierarchies without recursive stack growth.
	std::vector<std::uint8_t> States(Entities.size());

	for (std::size_t Start = 0; Start < Entities.size(); ++Start)
	{
		std::size_t Current = Start;

		while (Current != Entities.size() && States[Current] == 0)
		{
			States[Current] = 1;
			Current = Parents[Current];
		}

		if (Current != Entities.size() && States[Current] == 1)
		{
			return std::unexpected(FLevelError{"Level hierarchy contains a parent cycle"});
		}

		Current = Start;

		while (Current != Entities.size() && States[Current] == 1)
		{
			States[Current] = 2;
			Current = Parents[Current];
		}
	}

	return {};
}

struct FWorld::FImplementation
{
	bool IsValid(FEntityId Entity) const;
	FLevelEntity Snapshot(entt::entity Entity) const;
	void Assign(entt::entity Entity, const FLevelEntity& Snapshot);
	void Insert(const FLevelEntity& Snapshot);
	void ClearPending();

	entt::registry Registry;
	std::unordered_map<FObjectId, entt::entity, FObjectIdHash> Objects;
	std::uint64_t Token = AllocateWorldToken();
	std::uint64_t NextGeneration = 1;

	std::vector<FLevelEntity> PendingCreates;
	std::unordered_set<FObjectId, FObjectIdHash> PendingIds;
	std::unordered_set<std::uint32_t> PendingDestroys;
};

bool FWorld::FImplementation::IsValid(const FEntityId Entity) const
{
	const auto StorageEntity = static_cast<entt::entity>(Entity.Value);
	return Entity.World == Token && Entity.Generation != 0 && Registry.valid(StorageEntity) && Registry.get<FEntityGeneration>(StorageEntity).Value == Entity.Generation;
}

FLevelEntity FWorld::FImplementation::Snapshot(const entt::entity Entity) const
{
	const FStaticMeshComponent* Mesh = Registry.try_get<FStaticMeshComponent>(Entity);
	const FLightComponent* Light = Registry.try_get<FLightComponent>(Entity);
	const FSkyAtmosphereComponent* Atmosphere = Registry.try_get<FSkyAtmosphereComponent>(Entity);
	const FHeightFogComponent* Fog = Registry.try_get<FHeightFogComponent>(Entity);
	const FSoftBodyComponent* SoftBody = Registry.try_get<FSoftBodyComponent>(Entity);
	const FMoverComponent* Mover = Registry.try_get<FMoverComponent>(Entity);
	const FTriggerComponent* Trigger = Registry.try_get<FTriggerComponent>(Entity);
	return {
	    .Id = Registry.get<FObjectId>(Entity),
	    .Name = Registry.get<FEntityName>(Entity).Value,
	    .Parent = Registry.get<FEntityParent>(Entity).Value,
	    .Transform = Registry.get<FLevelTransform>(Entity),
	    .Mesh = Mesh ? std::optional<FStaticMeshComponent>{*Mesh} : std::nullopt,
	    .BodyType = Registry.get<ELevelBodyType>(Entity),
	    .BodySettings = Registry.get<FLevelRigidBodySettings>(Entity),
	    .Light = Light ? std::optional{*Light} : std::nullopt,
	    .SkyAtmosphere = Atmosphere ? std::optional{*Atmosphere} : std::nullopt,
	    .HeightFog = Fog ? std::optional{*Fog} : std::nullopt,
	    .SoftBody = SoftBody ? std::optional{*SoftBody} : std::nullopt,
	    .Mover = Mover ? std::optional{*Mover} : std::nullopt,
	    .Trigger = Trigger ? std::optional{*Trigger} : std::nullopt,
	};
}

void FWorld::FImplementation::Assign(const entt::entity Entity, const FLevelEntity& Snapshot)
{
	Registry.emplace_or_replace<FObjectId>(Entity, Snapshot.Id);
	Registry.emplace_or_replace<FEntityName>(Entity, Snapshot.Name);
	Registry.emplace_or_replace<FEntityParent>(Entity, Snapshot.Parent);
	Registry.emplace_or_replace<FLevelTransform>(Entity, Snapshot.Transform);
	Registry.emplace_or_replace<ELevelBodyType>(Entity, Snapshot.BodyType);
	Registry.emplace_or_replace<FLevelRigidBodySettings>(Entity, Snapshot.BodySettings);

	if (Snapshot.Mesh)
	{
		Registry.emplace_or_replace<FStaticMeshComponent>(Entity, *Snapshot.Mesh);
	}
	else
	{
		Registry.remove<FStaticMeshComponent>(Entity);
	}

	if (Snapshot.Light)
	{
		Registry.emplace_or_replace<FLightComponent>(Entity, *Snapshot.Light);
	}
	else
	{
		Registry.remove<FLightComponent>(Entity);
	}

	if (Snapshot.SkyAtmosphere)
	{
		Registry.emplace_or_replace<FSkyAtmosphereComponent>(Entity, *Snapshot.SkyAtmosphere);
	}
	else
	{
		Registry.remove<FSkyAtmosphereComponent>(Entity);
	}

	if (Snapshot.HeightFog)
	{
		Registry.emplace_or_replace<FHeightFogComponent>(Entity, *Snapshot.HeightFog);
	}
	else
	{
		Registry.remove<FHeightFogComponent>(Entity);
	}

	if (Snapshot.SoftBody)
	{
		Registry.emplace_or_replace<FSoftBodyComponent>(Entity, *Snapshot.SoftBody);
	}
	else
	{
		Registry.remove<FSoftBodyComponent>(Entity);
	}

	if (Snapshot.Mover)
	{
		Registry.emplace_or_replace<FMoverComponent>(Entity, *Snapshot.Mover);
	}
	else
	{
		Registry.remove<FMoverComponent>(Entity);
	}

	if (Snapshot.Trigger)
	{
		Registry.emplace_or_replace<FTriggerComponent>(Entity, *Snapshot.Trigger);
	}
	else
	{
		Registry.remove<FTriggerComponent>(Entity);
	}
}

void FWorld::FImplementation::Insert(const FLevelEntity& Snapshot)
{
	const std::uint64_t Generation = NextGeneration++;
	if (Generation == 0)
	{
		std::terminate();
	}

	const entt::entity Entity = Registry.create();
	Registry.emplace<FEntityGeneration>(Entity, Generation);
	Assign(Entity, Snapshot);
	Objects.emplace(Snapshot.Id, Entity);
}

void FWorld::FImplementation::ClearPending()
{
	PendingCreates.clear();
	PendingIds.clear();
	PendingDestroys.clear();
}

FWorld::FWorld()
    : Implementation(std::make_unique<FImplementation>())
{
}

FWorld::~FWorld() = default;

std::expected<FObjectId, FLevelError> FWorld::QueueCreateEntity(FLevelEntity Entity)
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Systems must queue structural changes through their execution context"});
	}

	if (!Entity.Id.IsValid())
	{
		Entity.Id = FObjectId::Generate();
	}

	const auto Valid = ValidateEntityProperties(Entity);
	if (!Valid)
	{
		return std::unexpected(Valid.error());
	}

	if (Implementation->Objects.contains(Entity.Id) || Implementation->PendingIds.contains(Entity.Id))
	{
		return std::unexpected(FLevelError{"Object ID already exists or is queued for creation"});
	}

	if (Implementation->Objects.size() + Implementation->PendingCreates.size() - Implementation->PendingDestroys.size() >= MaximumEntityCount)
	{
		return std::unexpected(FLevelError{"World exceeds the limit of 1000000 entities"});
	}

	const FObjectId Id = Entity.Id;
	Implementation->PendingIds.insert(Id);
	Implementation->PendingCreates.push_back(std::move(Entity));
	return Id;
}

std::expected<void, FLevelError> FWorld::QueueDestroyEntity(const FEntityId Entity)
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Systems must queue structural changes through their execution context"});
	}

	if (!Implementation->IsValid(Entity))
	{
		return std::unexpected(FLevelError{"Entity handle is stale or belongs to another world"});
	}

	if (!Implementation->PendingDestroys.insert(Entity.Value).second)
	{
		return std::unexpected(FLevelError{"Entity is already queued for destruction"});
	}

	return {};
}

std::expected<void, FLevelError> FWorld::FlushStructuralChanges()
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Structural barriers cannot run inside a system"});
	}

	if (Implementation->PendingCreates.empty() && Implementation->PendingDestroys.empty())
	{
		return {};
	}

	std::vector<FLevelEntity> Candidate;
	Candidate.reserve(Implementation->Objects.size() + Implementation->PendingCreates.size());

	for (const auto& [Id, Entity] : Implementation->Objects)
	{
		if (!Implementation->PendingDestroys.contains(entt::to_integral(Entity)))
		{
			Candidate.push_back(Implementation->Snapshot(Entity));
		}
	}

	Candidate.insert(Candidate.end(), Implementation->PendingCreates.begin(), Implementation->PendingCreates.end());
	const auto Valid = ValidateLevelEntities(Candidate);
	if (!Valid)
	{
		Implementation->ClearPending();
		return std::unexpected(FLevelError{std::format("Structural batch rejected: {}. Reparent or destroy children before destroying their parent", Valid.error().Message)});
	}

	for (const std::uint32_t Value : Implementation->PendingDestroys)
	{
		const auto Entity = static_cast<entt::entity>(Value);
		Implementation->Objects.erase(Implementation->Registry.get<FObjectId>(Entity));
		Implementation->Registry.destroy(Entity);
	}

	for (const FLevelEntity& Entity : Implementation->PendingCreates)
	{
		Implementation->Insert(Entity);
	}

	Implementation->ClearPending();
	return {};
}

std::expected<void, FLevelError> FWorld::ReplaceEntities(const std::span<const FLevelEntity> Entities)
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"World replacement cannot run inside a system"});
	}

	const auto Valid = ValidateLevelEntities(Entities);
	if (!Valid)
	{
		return Valid;
	}

	auto Replacement = std::make_unique<FImplementation>();
	Replacement->Objects.reserve(Entities.size());

	for (const FLevelEntity& Entity : Entities)
	{
		Replacement->Insert(Entity);
	}

	Implementation = std::move(Replacement);
	return {};
}

std::expected<void, FLevelError> FWorld::ApplyEntityChanges(const std::span<const FLevelEntityChange> Changes)
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Entity patches cannot publish inside a system"});
	}

	if (!Implementation->PendingCreates.empty() || !Implementation->PendingDestroys.empty())
	{
		return std::unexpected(FLevelError{"Flush pending structural changes before applying an entity patch"});
	}

	if (Changes.empty())
	{
		return {};
	}

	std::unordered_map<FObjectId, std::size_t, FObjectIdHash> ChangedObjects;
	ChangedObjects.reserve(Changes.size());
	std::size_t InsertCount = 0;
	std::size_t RemoveCount = 0;

	for (std::size_t Index = 0; Index < Changes.size(); ++Index)
	{
		const FLevelEntityChange& Change = Changes[Index];
		if (!Change.Before && !Change.After)
		{
			return std::unexpected(FLevelError{"Entity change must contain a before or after snapshot"});
		}

		const FObjectId Id = Change.Before ? Change.Before->Id : Change.After->Id;
		if (!Id.IsValid() || (Change.Before && Change.After && Change.Before->Id != Change.After->Id))
		{
			return std::unexpected(FLevelError{"Entity change must preserve a valid stable object ID"});
		}

		if (!ChangedObjects.emplace(Id, Index).second)
		{
			return std::unexpected(FLevelError{"Entity patch contains duplicate object IDs"});
		}

		const auto Existing = Implementation->Objects.find(Id);
		if (Change.Before)
		{
			if (Existing == Implementation->Objects.end() || Implementation->Snapshot(Existing->second) != *Change.Before)
			{
				return std::unexpected(FLevelError{"Entity patch before snapshot does not match the current world"});
			}
		}
		else if (Existing != Implementation->Objects.end())
		{
			return std::unexpected(FLevelError{"Entity patch inserts an object ID that already exists"});
		}

		InsertCount += !Change.Before;
		RemoveCount += !Change.After;
	}

	if (InsertCount > MaximumEntityCount || Implementation->Objects.size() - RemoveCount > MaximumEntityCount - InsertCount)
	{
		return std::unexpected(FLevelError{"Entity patch exceeds the limit of 1000000 entities"});
	}

	std::vector<FLevelEntity> Candidate;
	Candidate.reserve(Implementation->Objects.size() - RemoveCount + InsertCount);

	for (const auto& [Id, Entity] : Implementation->Objects)
	{
		const auto Changed = ChangedObjects.find(Id);
		if (Changed == ChangedObjects.end())
		{
			Candidate.push_back(Implementation->Snapshot(Entity));
		}
		else if (Changes[Changed->second].After)
		{
			Candidate.push_back(*Changes[Changed->second].After);
		}
	}

	for (const FLevelEntityChange& Change : Changes)
	{
		if (!Change.Before)
		{
			Candidate.push_back(*Change.After);
		}
	}

	const auto Valid = ValidateLevelEntities(Candidate);
	if (!Valid)
	{
		return Valid;
	}

	// Retire removals first so a full-capacity patch can reuse storage for its insertions.
	for (const FLevelEntityChange& Change : Changes)
	{
		if (!Change.After)
		{
			const auto Existing = Implementation->Objects.find(Change.Before->Id);
			Implementation->Registry.destroy(Existing->second);
			Implementation->Objects.erase(Existing);
		}
	}

	for (const FLevelEntityChange& Change : Changes)
	{
		if (Change.Before && Change.After)
		{
			Implementation->Assign(Implementation->Objects.find(Change.Before->Id)->second, *Change.After);
		}
		else if (Change.After)
		{
			Implementation->Insert(*Change.After);
		}
	}

	return {};
}

std::expected<void, FLevelError> FWorld::SetEntity(const FEntityId Entity, const FLevelEntity& Snapshot)
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Systems must write components through their execution context"});
	}

	return SetEntityForSystem(Entity, Snapshot);
}

std::expected<void, FLevelError> FWorld::SetEntityForSystem(const FEntityId Entity, const FLevelEntity& Snapshot)
{
	if (!Implementation->IsValid(Entity))
	{
		return std::unexpected(FLevelError{"Entity handle is stale or belongs to another world"});
	}

	const auto StorageEntity = static_cast<entt::entity>(Entity.Value);
	if (Snapshot.Id != Implementation->Registry.get<FObjectId>(StorageEntity))
	{
		return std::unexpected(FLevelError{"An existing entity's stable object ID cannot change"});
	}

	const auto Valid = ValidateEntityProperties(Snapshot);
	if (!Valid)
	{
		return Valid;
	}

	FObjectId Parent = Snapshot.Parent;

	while (Parent.IsValid())
	{
		if (Parent == Snapshot.Id)
		{
			return std::unexpected(FLevelError{"Reparenting would create a hierarchy cycle"});
		}

		const auto Found = Implementation->Objects.find(Parent);
		if (Found == Implementation->Objects.end())
		{
			return std::unexpected(FLevelError{"Entity references a missing parent"});
		}

		Parent = Implementation->Registry.get<FEntityParent>(Found->second).Value;
	}

	Implementation->Assign(StorageEntity, Snapshot);
	return {};
}

std::optional<FEntityId> FWorld::FindEntity(const FObjectId Object) const
{
	const auto Found = Implementation->Objects.find(Object);
	return Found == Implementation->Objects.end() ? std::nullopt : std::optional<FEntityId>{{.World = Implementation->Token, .Value = entt::to_integral(Found->second), .Generation = Implementation->Registry.get<FEntityGeneration>(Found->second).Value}};
}

std::optional<FLevelEntity> FWorld::GetEntity(const FEntityId Entity) const
{
	return bExecutingSystems ? std::nullopt : GetEntityForSystem(Entity);
}

std::optional<FLevelEntity> FWorld::GetEntityForSystem(const FEntityId Entity) const
{
	return Implementation->IsValid(Entity) ? std::optional<FLevelEntity>{Implementation->Snapshot(static_cast<entt::entity>(Entity.Value))} : std::nullopt;
}

std::vector<FLevelEntity> FWorld::SnapshotEntities() const
{
	return bExecutingSystems ? std::vector<FLevelEntity>{} : SnapshotEntitiesForSystem();
}

std::vector<FLevelEntity> FWorld::SnapshotEntitiesForSystem() const
{
	std::vector<FLevelEntity> Entities;
	Entities.reserve(Implementation->Objects.size());

	for (const auto& [Id, Entity] : Implementation->Objects)
	{
		Entities.push_back(Implementation->Snapshot(Entity));
	}

	std::ranges::sort(Entities, {}, &FLevelEntity::Id);
	return Entities;
}

std::size_t FWorld::GetEntityCount() const
{
	return Implementation->Objects.size();
}

bool FWorld::HasPendingStructuralChanges() const
{
	return !Implementation->PendingCreates.empty() || !Implementation->PendingDestroys.empty();
}

bool FWorld::BeginSystemExecution()
{
	if (bExecutingSystems || HasPendingStructuralChanges())
	{
		return false;
	}

	bExecutingSystems = true;
	return true;
}

void FWorld::EndSystemExecution()
{
	bExecutingSystems = false;
}

std::expected<TMatrix4<double>, FLevelError> FWorld::GetWorldMatrix(const FEntityId Entity) const
{
	if (bExecutingSystems)
	{
		return std::unexpected(FLevelError{"Systems must read components through their execution context"});
	}

	return GetWorldMatrixForSystem(Entity);
}

std::expected<TMatrix4<double>, FLevelError> FWorld::GetWorldMatrixForSystem(const FEntityId Entity) const
{
	if (!Implementation->IsValid(Entity))
	{
		return std::unexpected(FLevelError{"Entity handle is stale or belongs to another world"});
	}

	auto Current = static_cast<entt::entity>(Entity.Value);
	TMatrix4<double> Matrix = MakeLocalMatrix(Implementation->Registry.get<FLevelTransform>(Current));
	FObjectId Parent = Implementation->Registry.get<FEntityParent>(Current).Value;

	while (Parent.IsValid())
	{
		Current = Implementation->Objects.find(Parent)->second;
		Matrix = MakeLocalMatrix(Implementation->Registry.get<FLevelTransform>(Current)) * Matrix;
		Parent = Implementation->Registry.get<FEntityParent>(Current).Value;
	}

	return Matrix;
}
}
