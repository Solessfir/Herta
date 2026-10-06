#include "Herta/Scene/SystemScheduler.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <map>
#include <set>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumBufferedEvents = 65'536;
constexpr std::size_t MaximumEventPayloadBytes = 65'536;

struct FBufferedSceneEvent
{
	ESceneSystemPhase Phase;
	FSceneEvent Event;
};

struct FRegisteredSceneSystem
{
	FSceneSystemDescriptor Descriptor;
	FSceneSystemFunction Function;
};

bool IsValidPhase(const ESceneSystemPhase Phase)
{
	return Phase == ESceneSystemPhase::FixedUpdate || Phase == ESceneSystemPhase::Update || Phase == ESceneSystemPhase::Extract;
}

bool IsValidComponents(const ESceneComponent Components)
{
	return HasSceneComponents(ESceneComponent::All, Components);
}

ESceneComponent PresentComponents(const FSceneEntity& Entity)
{
	return ESceneComponent::Name | ESceneComponent::Hierarchy | ESceneComponent::Transform
	       | (Entity.Mesh ? ESceneComponent::StaticMesh : ESceneComponent::None)
	       | (Entity.BodyType != ESceneBodyType::None ? ESceneComponent::RigidBody : ESceneComponent::None);
}

FSceneQueryEntity ProjectEntity(const FEntityId Handle, const FSceneEntity& Entity, const ESceneComponent Components)
{
	FSceneQueryEntity Result{.Entity = Handle, .Object = Entity.Id};

	if (HasSceneComponents(Components, ESceneComponent::Name))
	{
		Result.Name = Entity.Name;
	}

	if (HasSceneComponents(Components, ESceneComponent::Hierarchy))
	{
		Result.Parent = Entity.Parent;
	}

	if (HasSceneComponents(Components, ESceneComponent::Transform))
	{
		Result.Transform = Entity.Transform;
	}

	if (HasSceneComponents(Components, ESceneComponent::StaticMesh))
	{
		Result.Mesh = Entity.Mesh;
	}

	if (HasSceneComponents(Components, ESceneComponent::RigidBody) && Entity.BodyType != ESceneBodyType::None)
	{
		Result.RigidBody = FSceneRigidBodyComponent{.Type = Entity.BodyType, .Settings = Entity.BodySettings};
	}

	return Result;
}

bool ContainsName(const std::vector<std::string>& Names, const std::string_view Name)
{
	return std::ranges::find(Names, Name) != Names.end();
}

bool ValidNames(const std::vector<std::string>& Names)
{
	std::set<std::string_view> Unique;

	for (const std::string& Name : Names)
	{
		if (Name.empty() || !Unique.insert(Name).second)
		{
			return false;
		}
	}

	return true;
}
}

struct FSceneSystemContext::FImplementation
{
	FImplementation(FWorld& InWorld, const FSceneSystemDescriptor& InDescriptor, double InDeltaSeconds, std::map<FObjectId, FSceneEntityChange>& InChanges, const std::vector<FSceneEvent>& InInputEvents, std::vector<FBufferedSceneEvent>& InOutputEvents, std::size_t InRetainedEventCount);

	FWorld& World;
	const FSceneSystemDescriptor& Descriptor;
	double DeltaSeconds;
	std::map<FObjectId, FSceneEntityChange>& Changes;
	const std::vector<FSceneEvent>& InputEvents;
	std::vector<FBufferedSceneEvent>& OutputEvents;
	std::size_t RetainedEventCount;
	std::optional<FSceneError> Failure{};
};

struct FSceneSystemScheduler::FImplementation
{
	explicit FImplementation(FWorld& InWorld);

	FWorld& World;
	std::map<std::string, FRegisteredSceneSystem, std::less<>> Systems;
	std::vector<FBufferedSceneEvent> Events;
	bool bRunning = false;
};

FSceneSystemContext::FImplementation::FImplementation(FWorld& InWorld, const FSceneSystemDescriptor& InDescriptor, const double InDeltaSeconds, std::map<FObjectId, FSceneEntityChange>& InChanges, const std::vector<FSceneEvent>& InInputEvents, std::vector<FBufferedSceneEvent>& InOutputEvents, const std::size_t InRetainedEventCount)
    : World(InWorld)
    , Descriptor(InDescriptor)
    , DeltaSeconds(InDeltaSeconds)
    , Changes(InChanges)
    , InputEvents(InInputEvents)
    , OutputEvents(InOutputEvents)
    , RetainedEventCount(InRetainedEventCount)
{
}

FSceneSystemScheduler::FImplementation::FImplementation(FWorld& InWorld)
    : World(InWorld)
{
}

FSceneSystemContext::FSceneSystemContext(FImplementation& InImplementation)
    : Implementation(InImplementation)
{
}

double FSceneSystemContext::GetDeltaSeconds() const
{
	return Implementation.DeltaSeconds;
}

std::unexpected<FSceneError> FSceneSystemContext::Fail(std::string Message)
{
	if (!Implementation.Failure)
	{
		Implementation.Failure = FSceneError{std::move(Message)};
	}

	return std::unexpected(*Implementation.Failure);
}

std::expected<void, FSceneError> FSceneSystemContext::CheckAccess(const FSceneComponentAccess Access)
{
	if (!IsValidComponents(Access.Read | Access.Write)
	    || !HasSceneComponents(Implementation.Descriptor.Access.Read | Implementation.Descriptor.Access.Write, Access.Read)
	    || !HasSceneComponents(Implementation.Descriptor.Access.Write, Access.Write))
	{
		return Fail("System attempted undeclared component access");
	}

	return {};
}

std::expected<std::vector<FSceneQueryEntity>, FSceneError> FSceneSystemContext::Query(const FSceneQuery& Query)
{
	const auto Allowed = CheckAccess(Query.Access);
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	const ESceneComponent Components = Query.Access.Read | Query.Access.Write;
	if (!IsValidComponents(Query.Require | Query.Exclude) || (Query.Require & Query.Exclude) != ESceneComponent::None
	    || !HasSceneComponents(Components, Query.Require | Query.Exclude))
	{
		return Fail("Query filters require declared component access and cannot overlap");
	}

	// ponytail: owned, sorted snapshots cost O(n log n); use private pool views if gameplay profiling requires them.
	const std::vector<FSceneEntity> Entities = Implementation.World.SnapshotEntitiesForSystem();
	std::vector<FSceneQueryEntity> Results;
	Results.reserve(Entities.size());

	for (const FSceneEntity& Entity : Entities)
	{
		const ESceneComponent Present = PresentComponents(Entity);
		if (HasSceneComponents(Present, Query.Require) && (Present & Query.Exclude) == ESceneComponent::None)
		{
			Results.push_back(ProjectEntity(*Implementation.World.FindEntity(Entity.Id), Entity, Components));
		}
	}

	return Results;
}

std::expected<FSceneQueryEntity, FSceneError> FSceneSystemContext::ReadEntity(const FEntityId Entity, const FSceneComponentAccess Access)
{
	const auto Allowed = CheckAccess(Access);
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	const auto Snapshot = Implementation.World.GetEntityForSystem(Entity);
	if (!Snapshot)
	{
		return Fail("Entity handle is stale or belongs to another world");
	}

	return ProjectEntity(Entity, *Snapshot, Access.Read | Access.Write);
}

std::expected<void, FSceneError> FSceneSystemContext::UpdateEntity(const FEntityId Entity, const FSceneComponentUpdate& Update)
{
	const ESceneComponent Writes = (Update.Name ? ESceneComponent::Name : ESceneComponent::None)
	                               | (Update.Parent ? ESceneComponent::Hierarchy : ESceneComponent::None)
	                               | (Update.Transform ? ESceneComponent::Transform : ESceneComponent::None)
	                               | (Update.Mesh ? ESceneComponent::StaticMesh : ESceneComponent::None)
	                               | (Update.RigidBody ? ESceneComponent::RigidBody : ESceneComponent::None);
	const auto Allowed = CheckAccess({.Write = Writes});
	if (!Allowed)
	{
		return Allowed;
	}

	const auto Live = Implementation.World.GetEntityForSystem(Entity);
	if (!Live)
	{
		return Fail("Entity handle is stale or belongs to another world");
	}

	const auto Pending = Implementation.Changes.find(Live->Id);
	if (Pending != Implementation.Changes.end() && !Pending->second.After)
	{
		return Fail("Entity is already queued for destruction");
	}

	FSceneEntity Candidate = Pending == Implementation.Changes.end() ? *Live : *Pending->second.After;
	const bool bStructural = Update.Parent.has_value()
	                         || (Update.Mesh && Update.Mesh->has_value() != Candidate.Mesh.has_value())
	                         || (Update.RigidBody && (Update.RigidBody->Type != ESceneBodyType::None) != (Candidate.BodyType != ESceneBodyType::None));
	if (bStructural && !Implementation.Descriptor.bStructuralChanges)
	{
		return Fail("System attempted undeclared structural mutation");
	}

	if (Update.Name)
	{
		Candidate.Name = *Update.Name;
	}

	if (Update.Parent)
	{
		Candidate.Parent = *Update.Parent;
	}

	if (Update.Transform)
	{
		Candidate.Transform = *Update.Transform;
	}

	if (Update.Mesh)
	{
		Candidate.Mesh = *Update.Mesh;
	}

	if (Update.RigidBody)
	{
		Candidate.BodyType = Update.RigidBody->Type;
		Candidate.BodySettings = Update.RigidBody->Settings;
	}

	if (bStructural || Pending != Implementation.Changes.end())
	{
		Implementation.Changes[Live->Id] = {.Before = *Live, .After = std::move(Candidate)};
		return {};
	}

	const auto Result = Implementation.World.SetEntityForSystem(Entity, Candidate);
	return Result ? Result : std::expected<void, FSceneError>{Fail(Result.error().Message)};
}

std::expected<TMatrix4<double>, FSceneError> FSceneSystemContext::GetWorldMatrix(const FEntityId Entity)
{
	const auto Allowed = CheckAccess({.Read = ESceneComponent::Transform | ESceneComponent::Hierarchy});
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	const auto Result = Implementation.World.GetWorldMatrixForSystem(Entity);
	return Result ? Result : std::expected<TMatrix4<double>, FSceneError>{Fail(Result.error().Message)};
}

std::expected<FObjectId, FSceneError> FSceneSystemContext::QueueCreateEntity(FSceneEntity Entity)
{
	const auto Allowed = CheckAccess({.Write = ESceneComponent::All});
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	if (!Implementation.Descriptor.bStructuralChanges)
	{
		return Fail("System attempted undeclared structural mutation");
	}

	if (!Entity.Id.IsValid())
	{
		Entity.Id = FObjectId::Generate();
	}

	if (Implementation.World.FindEntity(Entity.Id) || Implementation.Changes.contains(Entity.Id))
	{
		return Fail("Object ID already exists or is queued for creation");
	}

	const FObjectId Id = Entity.Id;
	Implementation.Changes.emplace(Id, FSceneEntityChange{.After = std::move(Entity)});
	return Id;
}

std::expected<void, FSceneError> FSceneSystemContext::QueueDestroyEntity(const FEntityId Entity)
{
	const auto Allowed = CheckAccess({.Write = ESceneComponent::All});
	if (!Allowed)
	{
		return Allowed;
	}

	if (!Implementation.Descriptor.bStructuralChanges)
	{
		return Fail("System attempted undeclared structural mutation");
	}

	const auto Live = Implementation.World.GetEntityForSystem(Entity);
	if (!Live)
	{
		return Fail("Entity handle is stale or belongs to another world");
	}

	const auto Pending = Implementation.Changes.find(Live->Id);
	if (Pending != Implementation.Changes.end() && !Pending->second.After)
	{
		return Fail("Entity is already queued for destruction");
	}

	Implementation.Changes[Live->Id] = {.Before = *Live};
	return {};
}

std::expected<std::vector<FSceneEvent>, FSceneError> FSceneSystemContext::ReadEvents(const std::string_view Type)
{
	if (!ContainsName(Implementation.Descriptor.ConsumeEvents, Type))
	{
		return Fail("System attempted undeclared event consumption");
	}

	std::vector<FSceneEvent> Events;

	for (const FSceneEvent& Event : Implementation.InputEvents)
	{
		if (Event.Type == Type)
		{
			Events.push_back(Event);
		}
	}

	return Events;
}

std::expected<void, FSceneError> FSceneSystemContext::PublishEvent(const ESceneSystemPhase ConsumePhase, FSceneEvent Event)
{
	if (!IsValidPhase(ConsumePhase) || !ContainsName(Implementation.Descriptor.ProduceEvents, Event.Type))
	{
		return Fail("System attempted undeclared event publication or used an invalid phase");
	}

	if (Event.Payload.size() > MaximumEventPayloadBytes || Implementation.RetainedEventCount + Implementation.OutputEvents.size() >= MaximumBufferedEvents)
	{
		return Fail("Scene event payload or buffered event capacity exceeded");
	}

	Implementation.OutputEvents.push_back({.Phase = ConsumePhase, .Event = std::move(Event)});
	return {};
}

FSceneSystemScheduler::FSceneSystemScheduler(FWorld& InWorld)
    : Implementation(std::make_unique<FImplementation>(InWorld))
{
}

FSceneSystemScheduler::~FSceneSystemScheduler() = default;

std::expected<void, FSceneError> FSceneSystemScheduler::AddSystem(FSceneSystemDescriptor Descriptor, FSceneSystemFunction Function)
{
	if (Implementation->bRunning)
	{
		return std::unexpected(FSceneError{"Systems cannot be registered during execution"});
	}

	if (Descriptor.Name.empty() || !Function || !IsValidPhase(Descriptor.Phase) || !IsValidComponents(Descriptor.Access.Read | Descriptor.Access.Write)
	    || !ValidNames(Descriptor.After) || !ValidNames(Descriptor.ConsumeEvents) || !ValidNames(Descriptor.ProduceEvents)
	    || ContainsName(Descriptor.After, Descriptor.Name))
	{
		return std::unexpected(FSceneError{"System descriptor or callback is invalid"});
	}

	const std::string Name = Descriptor.Name;
	if (!Implementation->Systems.emplace(Name, FRegisteredSceneSystem{.Descriptor = std::move(Descriptor), .Function = std::move(Function)}).second)
	{
		return std::unexpected(FSceneError{"System name is already registered"});
	}

	return {};
}

std::expected<void, FSceneError> FSceneSystemScheduler::RemoveSystem(const std::string_view Name)
{
	if (Implementation->bRunning || Implementation->Systems.erase(std::string{Name}) == 0)
	{
		return std::unexpected(FSceneError{"System cannot be removed during execution or is not registered"});
	}

	return {};
}

std::expected<void, FSceneError> FSceneSystemScheduler::Clear()
{
	if (Implementation->bRunning)
	{
		return std::unexpected(FSceneError{"Scheduler cannot be cleared during execution"});
	}

	Implementation->Systems.clear();
	Implementation->Events.clear();
	return {};
}

std::expected<void, FSceneError> FSceneSystemScheduler::RunPhase(const ESceneSystemPhase Phase, const double DeltaSeconds)
{
	if (Implementation->bRunning || !IsValidPhase(Phase) || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.)
	{
		return std::unexpected(FSceneError{"Invalid phase, delta time, or recursive system execution"});
	}

	std::vector<FRegisteredSceneSystem*> Order;
	std::set<std::string_view> Scheduled;
	std::size_t PhaseSystemCount = 0;

	for (auto& [Name, System] : Implementation->Systems)
	{
		if (System.Descriptor.Phase != Phase)
		{
			continue;
		}

		++PhaseSystemCount;

		for (const std::string& Dependency : System.Descriptor.After)
		{
			const auto Found = Implementation->Systems.find(Dependency);
			if (Found == Implementation->Systems.end() || Found->second.Descriptor.Phase != Phase)
			{
				return std::unexpected(FSceneError{std::format("System '{}' has missing or cross-phase dependency '{}'", Name, Dependency)});
			}
		}
	}

	// ponytail: small system lists use O(systems squared) topological selection; cache a graph if registration scales.
	while (Order.size() < PhaseSystemCount)
	{
		const auto Ready = std::ranges::find_if(Implementation->Systems, [&](const auto& Pair)
		{
			const FSceneSystemDescriptor& Descriptor = Pair.second.Descriptor;
			return Descriptor.Phase == Phase && !Scheduled.contains(Pair.first)
			       && std::ranges::all_of(Descriptor.After, [&](const std::string& Name)
			{
				return Scheduled.contains(Name);
			});
		});

		if (Ready == Implementation->Systems.end())
		{
			return std::unexpected(FSceneError{"System dependencies contain a cycle"});
		}

		Scheduled.insert(Ready->first);
		Order.push_back(&Ready->second);
	}

	if (!Implementation->World.BeginSystemExecution())
	{
		return std::unexpected(FSceneError{"World is executing systems or has unflushed structural changes"});
	}

	Implementation->bRunning = true;
	const auto Unlock = [&](FWorld* World)
	{
		World->EndSystemExecution();
		Implementation->bRunning = false;
	};
	std::unique_ptr<FWorld, decltype(Unlock)> ExecutionGuard(&Implementation->World, Unlock);
	std::map<FObjectId, FSceneEntityChange> Changes;
	std::vector<FSceneEvent> InputEvents;
	std::vector<FBufferedSceneEvent> OutputEvents;

	for (const FBufferedSceneEvent& Event : Implementation->Events)
	{
		if (Event.Phase == Phase)
		{
			InputEvents.push_back(Event.Event);
		}
	}

	for (FRegisteredSceneSystem* System : Order)
	{
		FSceneSystemContext::FImplementation State(Implementation->World, System->Descriptor, DeltaSeconds, Changes, InputEvents, OutputEvents, Implementation->Events.size() - InputEvents.size());
		FSceneSystemContext Context(State);
		std::expected<void, FSceneError> Result;
		try
		{
			Result = System->Function(Context);
		}
		catch (...)
		{
			std::terminate();
		}

		if (!Result || State.Failure)
		{
			return std::unexpected(FSceneError{std::format("System '{}' failed: {}", System->Descriptor.Name, State.Failure ? State.Failure->Message : Result.error().Message)});
		}
	}

	ExecutionGuard.reset();
	std::vector<FSceneEntityChange> Batch;
	Batch.reserve(Changes.size());

	for (auto& [Id, Change] : Changes)
	{
		Batch.push_back(std::move(Change));
	}

	const auto Published = Implementation->World.ApplyEntityChanges(Batch);
	if (!Published)
	{
		return std::unexpected(FSceneError{std::format("System phase structural barrier rejected: {}", Published.error().Message)});
	}

	std::erase_if(Implementation->Events, [&](const FBufferedSceneEvent& Event)
	{
		return Event.Phase == Phase;
	});
	Implementation->Events.insert(Implementation->Events.end(), std::make_move_iterator(OutputEvents.begin()), std::make_move_iterator(OutputEvents.end()));
	return {};
}

std::size_t FSceneSystemScheduler::GetSystemCount() const
{
	return Implementation->Systems.size();
}

std::size_t FSceneSystemScheduler::GetBufferedEventCount() const
{
	return Implementation->Events.size();
}
}
