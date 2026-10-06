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
};

enum class ELevelBodyType : std::uint8_t
{
	None,
	Static,
	Dynamic,
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
