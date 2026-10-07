#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Math/Matrix.h"
#include "Herta/Math/WorldPosition.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FLevelSystemContext;
class FLevelSystemScheduler;

class FObjectId final
{
public:
	constexpr FObjectId() noexcept = default;
	constexpr FObjectId(std::uint64_t InHigh, std::uint64_t InLow) noexcept;
	[[nodiscard]] static FObjectId Generate();
	static std::optional<FObjectId> Parse(std::string_view Text) noexcept;
	std::string ToString() const;
	constexpr bool IsValid() const noexcept;
	constexpr std::uint64_t GetHigh() const noexcept;
	constexpr std::uint64_t GetLow() const noexcept;
	constexpr auto operator<=>(const FObjectId&) const noexcept = default;

private:
	std::uint64_t High = 0;
	std::uint64_t Low = 0;
};

constexpr FObjectId::FObjectId(const std::uint64_t InHigh, const std::uint64_t InLow) noexcept
    : High(InHigh)
    , Low(InLow)
{
}

constexpr bool FObjectId::IsValid() const noexcept
{
	return High != 0 || Low != 0;
}

constexpr std::uint64_t FObjectId::GetHigh() const noexcept
{
	return High;
}

constexpr std::uint64_t FObjectId::GetLow() const noexcept
{
	return Low;
}

struct FEntityId
{
	constexpr bool operator==(const FEntityId&) const noexcept = default;

	std::uint64_t World = 0;
	std::uint32_t Value = std::numeric_limits<std::uint32_t>::max();
	std::uint64_t Generation = 0;
};

struct FLevelTransform
{
	constexpr bool operator==(const FLevelTransform&) const = default;

	// Translation is parent-local, or world-space for root entities.
	FWorldPosition Translation{};
	FQuaternion Rotation{};
	FVector3 Scale = FVector3::One();
};

struct FStaticMeshComponent
{
	constexpr bool operator==(const FStaticMeshComponent&) const = default;

	FAssetId Asset;
	// An invalid slot reference uses the material imported with the mesh.
	std::vector<FAssetId> Materials{};
};

enum class ELightType : std::uint8_t
{
	Directional,
	Sky,
	Point,
	Spot,
	Rect,
};

struct FLightComponent
{
	constexpr bool operator==(const FLightComponent&) const = default;

	ELightType Type = ELightType::Point;
	FVector3 Color = FVector3::One();
	float Intensity = 1.f;
	bool bUseTemperature = false;
	float TemperatureKelvin = 6500.f;
	bool bCastShadows = true;
	float ShadowBias = 0.001f;
	float ShadowNormalBias = 0.02f;
	float Range = 10.f;
	float InnerConeAngle = 0.34906585f;
	float OuterConeAngle = 0.61086524f;
	float Width = 1.f;
	float Height = 1.f;
	FAssetId Environment{};
	float AmbientStrength = 1.f;
	bool bEnvironmentVisible = true;
	bool bEnabled = true;
};

struct FSkyAtmosphereComponent
{
	constexpr bool operator==(const FSkyAtmosphereComponent&) const = default;

	FObjectId Sun{};
	float RayleighScattering = 1.f;
	float MieScattering = 1.f;
	float MieAnisotropy = 0.8f;
	float PlanetRadius = 6'360'000.f;
	float AtmosphereHeight = 80'000.f;
	bool bEnabled = true;
};

enum class EFogQuality : std::uint8_t
{
	Low,
	Medium,
	High,
};

struct FHeightFogComponent
{
	constexpr bool operator==(const FHeightFogComponent&) const = default;

	float Density = 0.005f;
	float HeightFalloff = 0.1f;
	FVector3 Albedo = FVector3::One();
	float Anisotropy = 0.2f;
	float MaxDistance = 100.f;
	EFogQuality Quality = EFogQuality::Medium;
	bool bEnabled = true;
	bool bVolumetric = true;
};

enum class ESoftBodyShape : std::uint8_t
{
	Rope,
	Cloth,
	Ball,
};

// Simulated deformable geometry generated from a few physical dimensions; the editor and physics derive their vertices at 10 cm spacing.
struct FSoftBodyComponent
{
	constexpr bool operator==(const FSoftBodyComponent&) const = default;

	ESoftBodyShape Shape = ESoftBodyShape::Rope;
	// Rope length, cloth width, or ball diameter.
	float Length = 3.f;
	// Cloth only.
	float Height = 2.f;
	// Rope radius, and the collision radius of cloth and ball vertices.
	float Thickness = 0.03f;
	float MassKg = 1.f;
	// 0 is slack and floppy, 1 is as stiff as the solver allows.
	float Stiffness = 0.9f;
	// Inflates the ball, in Pa * m^3.
	float Pressure = 400.f;
	float Friction = 0.4f;
	// Pins the rope's top end or the cloth's top corners in place.
	bool bPinned = true;
	FAssetId Material{};
	// Dynamic rigid body hung from the rope's free end; a soft reference like the atmosphere's sun.
	FObjectId Attachment{};
};

enum class ELevelBodyType : std::uint8_t
{
	None,
	Static,
	Dynamic,
};

// Moves back and forth between its authored pose and that pose plus Offset, easing in and out at both ends.
struct FMoverComponent
{
	constexpr bool operator==(const FMoverComponent&) const = default;

	// World-space meters at the far end of the path.
	FVector3 Offset{0.f, 0.f, 4.f};
	// One full round trip.
	float PeriodSeconds = 6.f;
};

// Reports bodies entering and leaving a box centered on the entity origin while simulating.
struct FTriggerComponent
{
	constexpr bool operator==(const FTriggerComponent&) const = default;

	// Full box dimensions in meters, before the entity's scale.
	FVector3 Size{2.f, 2.f, 2.f};
};

// Collision fitted to the mesh bounds: a sphere encloses the largest half extent, and a capsule stands along local +Y with the larger horizontal half extent as its radius.
enum class ELevelCollisionShape : std::uint8_t
{
	Box,
	Sphere,
	Capsule,
};

struct FLevelRigidBodySettings
{
	constexpr bool operator==(const FLevelRigidBodySettings&) const = default;

	float MassKg = 1.f;
	float Friction = 0.2f;
	float Restitution = 0.f;
	float LinearDamping = 0.05f;
	float AngularDamping = 0.05f;
	float GravityScale = 1.f;
	ELevelCollisionShape Collision = ELevelCollisionShape::Box;
};

struct FLevelEntity
{
	bool operator==(const FLevelEntity&) const = default;

	FObjectId Id{};
	std::string Name{};
	FObjectId Parent{};
	FLevelTransform Transform{};
	std::optional<FStaticMeshComponent> Mesh{};
	ELevelBodyType BodyType = ELevelBodyType::None;
	FLevelRigidBodySettings BodySettings{};
	std::optional<FLightComponent> Light{};
	std::optional<FSkyAtmosphereComponent> SkyAtmosphere{};
	std::optional<FHeightFogComponent> HeightFog{};
	std::optional<FSoftBodyComponent> SoftBody{};
	std::optional<FMoverComponent> Mover{};
	std::optional<FTriggerComponent> Trigger{};
};

struct FLevelEntityChange
{
	std::optional<FLevelEntity> Before{};
	std::optional<FLevelEntity> After{};
};

struct FLevelError
{
	std::string Message;
};

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelEntities(std::span<const FLevelEntity> Entities);
[[nodiscard]] std::expected<void, FLevelError> ValidateLevelRigidBodySettings(const FLevelRigidBodySettings& Settings);
[[nodiscard]] std::expected<void, FLevelError> ValidateLightComponent(const FLightComponent& Light);
[[nodiscard]] std::expected<void, FLevelError> ValidateSkyAtmosphereComponent(const FSkyAtmosphereComponent& Atmosphere);
[[nodiscard]] std::expected<void, FLevelError> ValidateHeightFogComponent(const FHeightFogComponent& Fog);
[[nodiscard]] std::expected<void, FLevelError> ValidateSoftBodyComponent(const FSoftBodyComponent& SoftBody);
[[nodiscard]] std::expected<void, FLevelError> ValidateMoverComponent(const FMoverComponent& Mover);
[[nodiscard]] std::expected<void, FLevelError> ValidateTriggerComponent(const FTriggerComponent& Trigger);

// Single-owner world. Structural changes become visible only at an explicit barrier.
class FWorld final
{
public:
	FWorld();
	~FWorld();
	FWorld(const FWorld&) = delete;
	FWorld& operator=(const FWorld&) = delete;
	FWorld(FWorld&&) = delete;
	FWorld& operator=(FWorld&&) = delete;

	[[nodiscard]] std::expected<FObjectId, FLevelError> QueueCreateEntity(FLevelEntity Entity);
	[[nodiscard]] std::expected<void, FLevelError> QueueDestroyEntity(FEntityId Entity);
	// A rejected batch is discarded without changing live entities or their handles.
	[[nodiscard]] std::expected<void, FLevelError> FlushStructuralChanges();
	// Successful replacement discards pending changes and invalidates every old handle.
	[[nodiscard]] std::expected<void, FLevelError> ReplaceEntities(std::span<const FLevelEntity> Entities);
	// Applies a validated batch at a structural barrier while preserving surviving handles.
	[[nodiscard]] std::expected<void, FLevelError> ApplyEntityChanges(std::span<const FLevelEntityChange> Changes);
	[[nodiscard]] std::expected<void, FLevelError> SetEntity(FEntityId Entity, const FLevelEntity& Snapshot);
	std::optional<FEntityId> FindEntity(FObjectId Object) const;
	std::optional<FLevelEntity> GetEntity(FEntityId Entity) const;
	std::vector<FLevelEntity> SnapshotEntities() const;
	std::size_t GetEntityCount() const;
	bool HasPendingStructuralChanges() const;
	[[nodiscard]] std::expected<TMatrix4<double>, FLevelError> GetWorldMatrix(FEntityId Entity) const;

private:
	friend class FLevelSystemContext;
	friend class FLevelSystemScheduler;

	[[nodiscard]] std::expected<void, FLevelError> SetEntityForSystem(FEntityId Entity, const FLevelEntity& Snapshot);
	std::optional<FLevelEntity> GetEntityForSystem(FEntityId Entity) const;
	std::vector<FLevelEntity> SnapshotEntitiesForSystem() const;
	[[nodiscard]] std::expected<TMatrix4<double>, FLevelError> GetWorldMatrixForSystem(FEntityId Entity) const;
	bool BeginSystemExecution();
	void EndSystemExecution();

	struct FImplementation;
	std::unique_ptr<FImplementation> Implementation;
	bool bExecutingSystems = false;
};
}
