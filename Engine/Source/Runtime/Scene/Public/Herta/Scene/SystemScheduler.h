#pragma once

#include "Herta/Scene/World.h"

#include <functional>
#include <utility>

namespace Herta
{
enum class ESceneComponent : std::uint8_t
{
	None = 0,
	Name = 1 << 0,
	Hierarchy = 1 << 1,
	Transform = 1 << 2,
	StaticMesh = 1 << 3,
	RigidBody = 1 << 4,
	All = (1 << 5) - 1,
};

constexpr ESceneComponent operator|(const ESceneComponent Left, const ESceneComponent Right)
{
	return static_cast<ESceneComponent>(std::to_underlying(Left) | std::to_underlying(Right));
}

constexpr ESceneComponent operator&(const ESceneComponent Left, const ESceneComponent Right)
{
	return static_cast<ESceneComponent>(std::to_underlying(Left) & std::to_underlying(Right));
}

constexpr bool HasSceneComponents(const ESceneComponent Available, const ESceneComponent Required)
{
	return (Available & Required) == Required;
}

enum class ESceneSystemPhase : std::uint8_t
{
	FixedUpdate,
	Update,
	Extract,
};

struct FSceneComponentAccess
{
	ESceneComponent Read = ESceneComponent::None;
	ESceneComponent Write = ESceneComponent::None;
};

struct FSceneQuery
{
	FSceneComponentAccess Access{};
	ESceneComponent Require = ESceneComponent::None;
	ESceneComponent Exclude = ESceneComponent::None;
};

struct FSceneRigidBodyComponent
{
	ESceneBodyType Type = ESceneBodyType::None;
	FSceneRigidBodySettings Settings{};
};

// Owned component copies. Only requested fields are populated; identity is always available.
struct FSceneQueryEntity
{
	FEntityId Entity{};
	FObjectId Object{};
	std::optional<std::string> Name{};
	std::optional<FObjectId> Parent{};
	std::optional<FSceneTransform> Transform{};
	std::optional<FStaticMeshComponent> Mesh{};
	std::optional<FSceneRigidBodyComponent> RigidBody{};
};

struct FSceneComponentUpdate
{
	std::optional<std::string> Name{};
	std::optional<FObjectId> Parent{};
	std::optional<FSceneTransform> Transform{};
	// Outer optional means update; an empty inner optional removes the component.
	std::optional<std::optional<FStaticMeshComponent>> Mesh{};
	std::optional<FSceneRigidBodyComponent> RigidBody{};
};

struct FSceneEvent
{
	std::string Type{};
	FObjectId Subject{};
	std::vector<std::byte> Payload{};
};

struct FSceneSystemDescriptor
{
	std::string Name{};
	ESceneSystemPhase Phase = ESceneSystemPhase::Update;
	FSceneComponentAccess Access{};
	// Dependencies name systems in this phase. Independent systems run in name order.
	std::vector<std::string> After{};
	bool bStructuralChanges = false;
	std::vector<std::string> ConsumeEvents{};
	std::vector<std::string> ProduceEvents{};
};

// Contexts are callback-scoped. Query and event results are owned copies.
class FSceneSystemContext final
{
public:
	~FSceneSystemContext() = default;
	FSceneSystemContext(const FSceneSystemContext&) = delete;
	FSceneSystemContext& operator=(const FSceneSystemContext&) = delete;
	FSceneSystemContext(FSceneSystemContext&&) = delete;
	FSceneSystemContext& operator=(FSceneSystemContext&&) = delete;

	double GetDeltaSeconds() const;
	[[nodiscard]] std::expected<std::vector<FSceneQueryEntity>, FSceneError> Query(const FSceneQuery& Query);
	[[nodiscard]] std::expected<FSceneQueryEntity, FSceneError> ReadEntity(FEntityId Entity, FSceneComponentAccess Access);
	[[nodiscard]] std::expected<TMatrix4<double>, FSceneError> GetWorldMatrix(FEntityId Entity);
	[[nodiscard]] std::expected<void, FSceneError> UpdateEntity(FEntityId Entity, const FSceneComponentUpdate& Update);
	[[nodiscard]] std::expected<FObjectId, FSceneError> QueueCreateEntity(FSceneEntity Entity);
	[[nodiscard]] std::expected<void, FSceneError> QueueDestroyEntity(FEntityId Entity);
	[[nodiscard]] std::expected<std::vector<FSceneEvent>, FSceneError> ReadEvents(std::string_view Type);
	// Events never become visible within the current invocation, even to later systems.
	[[nodiscard]] std::expected<void, FSceneError> PublishEvent(ESceneSystemPhase ConsumePhase, FSceneEvent Event);

private:
	friend class FSceneSystemScheduler;
	struct FImplementation;
	explicit FSceneSystemContext(FImplementation& InImplementation);
	[[nodiscard]] std::expected<void, FSceneError> CheckAccess(FSceneComponentAccess Access);
	std::unexpected<FSceneError> Fail(std::string Message);

	FImplementation& Implementation;
};

// Callbacks report ordinary failures through expected. Throwing violates the module boundary and is fatal.
using FSceneSystemFunction = std::function<std::expected<void, FSceneError>(FSceneSystemContext&)>;

// Single-owner serial executor. The world must outlive this scheduler.
class FSceneSystemScheduler final
{
public:
	explicit FSceneSystemScheduler(FWorld& InWorld);
	~FSceneSystemScheduler();
	FSceneSystemScheduler(const FSceneSystemScheduler&) = delete;
	FSceneSystemScheduler& operator=(const FSceneSystemScheduler&) = delete;
	FSceneSystemScheduler(FSceneSystemScheduler&&) = delete;
	FSceneSystemScheduler& operator=(FSceneSystemScheduler&&) = delete;

	// Registration owns the descriptor and callable until removal or Clear.
	[[nodiscard]] std::expected<void, FSceneError> AddSystem(FSceneSystemDescriptor Descriptor, FSceneSystemFunction Function);
	[[nodiscard]] std::expected<void, FSceneError> RemoveSystem(std::string_view Name);
	[[nodiscard]] std::expected<void, FSceneError> Clear();
	// Failure discards this phase's deferred changes and emitted events. Earlier validated component writes remain applied.
	[[nodiscard]] std::expected<void, FSceneError> RunPhase(ESceneSystemPhase Phase, double DeltaSeconds);
	std::size_t GetSystemCount() const;
	std::size_t GetBufferedEventCount() const;

private:
	struct FImplementation;
	std::unique_ptr<FImplementation> Implementation;
};
}
