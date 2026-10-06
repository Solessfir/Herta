#pragma once

#include "Herta/Level/World.h"

#include <functional>
#include <utility>

namespace Herta
{
enum class ELevelComponent : std::uint8_t
{
	None = 0,
	Name = 1 << 0,
	Hierarchy = 1 << 1,
	Transform = 1 << 2,
	StaticMesh = 1 << 3,
	RigidBody = 1 << 4,
	All = (1 << 5) - 1,
};

constexpr ELevelComponent operator|(const ELevelComponent Left, const ELevelComponent Right)
{
	return static_cast<ELevelComponent>(std::to_underlying(Left) | std::to_underlying(Right));
}

constexpr ELevelComponent operator&(const ELevelComponent Left, const ELevelComponent Right)
{
	return static_cast<ELevelComponent>(std::to_underlying(Left) & std::to_underlying(Right));
}

constexpr bool HasLevelComponents(const ELevelComponent Available, const ELevelComponent Required)
{
	return (Available & Required) == Required;
}

enum class ELevelSystemPhase : std::uint8_t
{
	FixedUpdate,
	Update,
	Extract,
};

struct FLevelComponentAccess
{
	ELevelComponent Read = ELevelComponent::None;
	ELevelComponent Write = ELevelComponent::None;
};

struct FLevelQuery
{
	FLevelComponentAccess Access{};
	ELevelComponent Require = ELevelComponent::None;
	ELevelComponent Exclude = ELevelComponent::None;
};

struct FLevelRigidBodyComponent
{
	ELevelBodyType Type = ELevelBodyType::None;
	FLevelRigidBodySettings Settings{};
};

// Owned component copies. Only requested fields are populated; identity is always available.
struct FLevelQueryEntity
{
	FEntityId Entity{};
	FObjectId Object{};
	std::optional<std::string> Name{};
	std::optional<FObjectId> Parent{};
	std::optional<FLevelTransform> Transform{};
	std::optional<FStaticMeshComponent> Mesh{};
	std::optional<FLevelRigidBodyComponent> RigidBody{};
};

struct FLevelComponentUpdate
{
	std::optional<std::string> Name{};
	std::optional<FObjectId> Parent{};
	std::optional<FLevelTransform> Transform{};
	// Outer optional means update; an empty inner optional removes the component.
	std::optional<std::optional<FStaticMeshComponent>> Mesh{};
	std::optional<FLevelRigidBodyComponent> RigidBody{};
};

struct FLevelEvent
{
	std::string Type{};
	FObjectId Subject{};
	std::vector<std::byte> Payload{};
};

struct FLevelSystemDescriptor
{
	std::string Name{};
	ELevelSystemPhase Phase = ELevelSystemPhase::Update;
	FLevelComponentAccess Access{};
	// Dependencies name systems in this phase. Independent systems run in name order.
	std::vector<std::string> After{};
	bool bStructuralChanges = false;
	std::vector<std::string> ConsumeEvents{};
	std::vector<std::string> ProduceEvents{};
};

// Contexts are callback-scoped. Query and event results are owned copies.
class FLevelSystemContext final
{
public:
	~FLevelSystemContext() = default;
	FLevelSystemContext(const FLevelSystemContext&) = delete;
	FLevelSystemContext& operator=(const FLevelSystemContext&) = delete;
	FLevelSystemContext(FLevelSystemContext&&) = delete;
	FLevelSystemContext& operator=(FLevelSystemContext&&) = delete;

	double GetDeltaSeconds() const;
	[[nodiscard]] std::expected<std::vector<FLevelQueryEntity>, FLevelError> Query(const FLevelQuery& Query);
	[[nodiscard]] std::expected<FLevelQueryEntity, FLevelError> ReadEntity(FEntityId Entity, FLevelComponentAccess Access);
	[[nodiscard]] std::expected<TMatrix4<double>, FLevelError> GetWorldMatrix(FEntityId Entity);
	[[nodiscard]] std::expected<void, FLevelError> UpdateEntity(FEntityId Entity, const FLevelComponentUpdate& Update);
	[[nodiscard]] std::expected<FObjectId, FLevelError> QueueCreateEntity(FLevelEntity Entity);
	[[nodiscard]] std::expected<void, FLevelError> QueueDestroyEntity(FEntityId Entity);
	[[nodiscard]] std::expected<std::vector<FLevelEvent>, FLevelError> ReadEvents(std::string_view Type);
	// Events never become visible within the current invocation, even to later systems.
	[[nodiscard]] std::expected<void, FLevelError> PublishEvent(ELevelSystemPhase ConsumePhase, FLevelEvent Event);

private:
	friend class FLevelSystemScheduler;
	struct FImplementation;
	explicit FLevelSystemContext(FImplementation& InImplementation);
	[[nodiscard]] std::expected<void, FLevelError> CheckAccess(FLevelComponentAccess Access);
	std::unexpected<FLevelError> Fail(std::string Message);

	FImplementation& Implementation;
};

// Callbacks report ordinary failures through expected. Throwing violates the module boundary and is fatal.
using FLevelSystemFunction = std::function<std::expected<void, FLevelError>(FLevelSystemContext&)>;

// Single-owner serial executor. The world must outlive this scheduler.
class FLevelSystemScheduler final
{
public:
	explicit FLevelSystemScheduler(FWorld& InWorld);
	~FLevelSystemScheduler();
	FLevelSystemScheduler(const FLevelSystemScheduler&) = delete;
	FLevelSystemScheduler& operator=(const FLevelSystemScheduler&) = delete;
	FLevelSystemScheduler(FLevelSystemScheduler&&) = delete;
	FLevelSystemScheduler& operator=(FLevelSystemScheduler&&) = delete;

	// Registration owns the descriptor and callable until removal or Clear.
	[[nodiscard]] std::expected<void, FLevelError> AddSystem(FLevelSystemDescriptor Descriptor, FLevelSystemFunction Function);
	[[nodiscard]] std::expected<void, FLevelError> RemoveSystem(std::string_view Name);
	[[nodiscard]] std::expected<void, FLevelError> Clear();
	// Failure discards this phase's deferred changes and emitted events. Earlier validated component writes remain applied.
	[[nodiscard]] std::expected<void, FLevelError> RunPhase(ELevelSystemPhase Phase, double DeltaSeconds);
	std::size_t GetSystemCount() const;
	std::size_t GetBufferedEventCount() const;

private:
	struct FImplementation;
	std::unique_ptr<FImplementation> Implementation;
};
}
