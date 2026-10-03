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

struct FSceneTransform
{
	constexpr bool operator==(const FSceneTransform&) const = default;

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

enum class ESceneBodyMotion : std::uint8_t
{
	None,
	Static,
	Dynamic,
};

struct FSceneEntity
{
	bool operator==(const FSceneEntity&) const = default;

	FObjectId Id{};
	std::string Name{};
	FObjectId Parent{};
	FSceneTransform Transform{};
	std::optional<FStaticMeshComponent> Mesh{};
	ESceneBodyMotion BodyMotion = ESceneBodyMotion::None;
};

struct FSceneError
{
	std::string Message;
};

[[nodiscard]] std::expected<void, FSceneError> ValidateSceneEntities(std::span<const FSceneEntity> Entities);

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

	[[nodiscard]] std::expected<FObjectId, FSceneError> QueueCreateEntity(FSceneEntity Entity);
	[[nodiscard]] std::expected<void, FSceneError> QueueDestroyEntity(FEntityId Entity);
	// A rejected batch is discarded without changing live entities or their handles.
	[[nodiscard]] std::expected<void, FSceneError> FlushStructuralChanges();
	// Successful replacement discards pending changes and invalidates every old handle.
	[[nodiscard]] std::expected<void, FSceneError> ReplaceEntities(std::span<const FSceneEntity> Entities);
	[[nodiscard]] std::expected<void, FSceneError> SetEntity(FEntityId Entity, const FSceneEntity& Snapshot);
	std::optional<FEntityId> FindEntity(FObjectId Object) const;
	std::optional<FSceneEntity> GetEntity(FEntityId Entity) const;
	std::vector<FSceneEntity> SnapshotEntities() const;
	std::size_t GetEntityCount() const;
	[[nodiscard]] std::expected<TMatrix4<double>, FSceneError> GetWorldMatrix(FEntityId Entity) const;

private:
	struct FImplementation;
	std::unique_ptr<FImplementation> Implementation;
};
}
