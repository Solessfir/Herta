#include "Herta/Level/SystemScheduler.h"

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

struct FBufferedLevelEvent
{
	ELevelSystemPhase Phase;
	FLevelEvent Event;
};

struct FRegisteredLevelSystem
{
	FLevelSystemDescriptor Descriptor;
	FLevelSystemFunction Function;
};

bool IsValidPhase(const ELevelSystemPhase Phase)
{
	return Phase == ELevelSystemPhase::FixedUpdate || Phase == ELevelSystemPhase::Update || Phase == ELevelSystemPhase::Extract;
}

bool IsValidComponents(const ELevelComponent Components)
{
	return HasLevelComponents(ELevelComponent::All, Components);
}

ELevelComponent PresentComponents(const FLevelEntity& Entity)
{
	return ELevelComponent::Name | ELevelComponent::Hierarchy | ELevelComponent::Transform
	       | (Entity.Mesh ? ELevelComponent::StaticMesh : ELevelComponent::None)
	       | (Entity.BodyType != ELevelBodyType::None ? ELevelComponent::RigidBody : ELevelComponent::None)
	       | (Entity.Light ? ELevelComponent::Light : ELevelComponent::None)
	       | (Entity.SkyAtmosphere ? ELevelComponent::SkyAtmosphere : ELevelComponent::None)
	       | (Entity.HeightFog ? ELevelComponent::HeightFog : ELevelComponent::None)
	       | (Entity.SoftBody ? ELevelComponent::SoftBody : ELevelComponent::None)
	       | (Entity.Mover ? ELevelComponent::Mover : ELevelComponent::None)
	       | (Entity.Trigger ? ELevelComponent::Trigger : ELevelComponent::None);
}

FLevelQueryEntity ProjectEntity(const FEntityId Handle, const FLevelEntity& Entity, const ELevelComponent Components)
{
	FLevelQueryEntity Result{.Entity = Handle, .Object = Entity.Id};

	if (HasLevelComponents(Components, ELevelComponent::Name))
	{
		Result.Name = Entity.Name;
	}

	if (HasLevelComponents(Components, ELevelComponent::Hierarchy))
	{
		Result.Parent = Entity.Parent;
	}

	if (HasLevelComponents(Components, ELevelComponent::Transform))
	{
		Result.Transform = Entity.Transform;
	}

	if (HasLevelComponents(Components, ELevelComponent::StaticMesh))
	{
		Result.Mesh = Entity.Mesh;
	}

	if (HasLevelComponents(Components, ELevelComponent::RigidBody) && Entity.BodyType != ELevelBodyType::None)
	{
		Result.RigidBody = FLevelRigidBodyComponent{.Type = Entity.BodyType, .Settings = Entity.BodySettings};
	}

	if (HasLevelComponents(Components, ELevelComponent::Light))
	{
		Result.Light = Entity.Light;
	}

	if (HasLevelComponents(Components, ELevelComponent::SkyAtmosphere))
	{
		Result.SkyAtmosphere = Entity.SkyAtmosphere;
	}

	if (HasLevelComponents(Components, ELevelComponent::HeightFog))
	{
		Result.HeightFog = Entity.HeightFog;
	}

	if (HasLevelComponents(Components, ELevelComponent::SoftBody))
	{
		Result.SoftBody = Entity.SoftBody;
	}

	if (HasLevelComponents(Components, ELevelComponent::Mover))
	{
		Result.Mover = Entity.Mover;
	}

	if (HasLevelComponents(Components, ELevelComponent::Trigger))
	{
		Result.Trigger = Entity.Trigger;
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

struct FLevelSystemContext::FImplementation
{
	FImplementation(FWorld& InWorld, const FLevelSystemDescriptor& InDescriptor, double InDeltaSeconds, std::map<FObjectId, FLevelEntityChange>& InChanges, const std::vector<FLevelEvent>& InInputEvents, std::vector<FBufferedLevelEvent>& InOutputEvents, std::size_t InRetainedEventCount);

	FWorld& World;
	const FLevelSystemDescriptor& Descriptor;
	double DeltaSeconds;
	std::map<FObjectId, FLevelEntityChange>& Changes;
	const std::vector<FLevelEvent>& InputEvents;
	std::vector<FBufferedLevelEvent>& OutputEvents;
	std::size_t RetainedEventCount;
	std::optional<FLevelError> Failure{};
};

struct FLevelSystemScheduler::FImplementation
{
	explicit FImplementation(FWorld& InWorld);

	FWorld& World;
	std::map<std::string, FRegisteredLevelSystem, std::less<>> Systems;
	std::vector<FBufferedLevelEvent> Events;
	bool bRunning = false;
};

FLevelSystemContext::FImplementation::FImplementation(FWorld& InWorld, const FLevelSystemDescriptor& InDescriptor, const double InDeltaSeconds, std::map<FObjectId, FLevelEntityChange>& InChanges, const std::vector<FLevelEvent>& InInputEvents, std::vector<FBufferedLevelEvent>& InOutputEvents, const std::size_t InRetainedEventCount)
    : World(InWorld)
    , Descriptor(InDescriptor)
    , DeltaSeconds(InDeltaSeconds)
    , Changes(InChanges)
    , InputEvents(InInputEvents)
    , OutputEvents(InOutputEvents)
    , RetainedEventCount(InRetainedEventCount)
{
}

FLevelSystemScheduler::FImplementation::FImplementation(FWorld& InWorld)
    : World(InWorld)
{
}

FLevelSystemContext::FLevelSystemContext(FImplementation& InImplementation)
    : Implementation(InImplementation)
{
}

double FLevelSystemContext::GetDeltaSeconds() const
{
	return Implementation.DeltaSeconds;
}

std::unexpected<FLevelError> FLevelSystemContext::Fail(std::string Message)
{
	if (!Implementation.Failure)
	{
		Implementation.Failure = FLevelError{std::move(Message)};
	}

	return std::unexpected(*Implementation.Failure);
}

std::expected<void, FLevelError> FLevelSystemContext::CheckAccess(const FLevelComponentAccess Access)
{
	if (!IsValidComponents(Access.Read | Access.Write)
	    || !HasLevelComponents(Implementation.Descriptor.Access.Read | Implementation.Descriptor.Access.Write, Access.Read)
	    || !HasLevelComponents(Implementation.Descriptor.Access.Write, Access.Write))
	{
		return Fail("System attempted undeclared component access");
	}

	return {};
}

std::expected<std::vector<FLevelQueryEntity>, FLevelError> FLevelSystemContext::Query(const FLevelQuery& Query)
{
	const auto Allowed = CheckAccess(Query.Access);
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	const ELevelComponent Components = Query.Access.Read | Query.Access.Write;
	if (!IsValidComponents(Query.Require | Query.Exclude) || (Query.Require & Query.Exclude) != ELevelComponent::None
	    || !HasLevelComponents(Components, Query.Require | Query.Exclude))
	{
		return Fail("Query filters require declared component access and cannot overlap");
	}

	// ponytail: owned, sorted snapshots cost O(n log n); use private pool views if gameplay profiling requires them.
	const std::vector<FLevelEntity> Entities = Implementation.World.SnapshotEntitiesForSystem();
	std::vector<FLevelQueryEntity> Results;
	Results.reserve(Entities.size());

	for (const FLevelEntity& Entity : Entities)
	{
		const ELevelComponent Present = PresentComponents(Entity);
		if (HasLevelComponents(Present, Query.Require) && (Present & Query.Exclude) == ELevelComponent::None)
		{
			Results.push_back(ProjectEntity(*Implementation.World.FindEntity(Entity.Id), Entity, Components));
		}
	}

	return Results;
}

std::expected<FLevelQueryEntity, FLevelError> FLevelSystemContext::ReadEntity(const FEntityId Entity, const FLevelComponentAccess Access)
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

std::expected<void, FLevelError> FLevelSystemContext::UpdateEntity(const FEntityId Entity, const FLevelComponentUpdate& Update)
{
	const ELevelComponent Writes = (Update.Name ? ELevelComponent::Name : ELevelComponent::None)
	                               | (Update.Parent ? ELevelComponent::Hierarchy : ELevelComponent::None)
	                               | (Update.Transform ? ELevelComponent::Transform : ELevelComponent::None)
	                               | (Update.Mesh ? ELevelComponent::StaticMesh : ELevelComponent::None)
	                               | (Update.RigidBody ? ELevelComponent::RigidBody : ELevelComponent::None)
	                               | (Update.Light ? ELevelComponent::Light : ELevelComponent::None)
	                               | (Update.SkyAtmosphere ? ELevelComponent::SkyAtmosphere : ELevelComponent::None)
	                               | (Update.HeightFog ? ELevelComponent::HeightFog : ELevelComponent::None)
	                               | (Update.SoftBody ? ELevelComponent::SoftBody : ELevelComponent::None)
	                               | (Update.Mover ? ELevelComponent::Mover : ELevelComponent::None)
	                               | (Update.Trigger ? ELevelComponent::Trigger : ELevelComponent::None);
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

	FLevelEntity Candidate = Pending == Implementation.Changes.end() ? *Live : *Pending->second.After;
	const bool bStructural = Update.Parent.has_value()
	                         || (Update.Mesh && Update.Mesh->has_value() != Candidate.Mesh.has_value())
	                         || (Update.RigidBody && (Update.RigidBody->Type != ELevelBodyType::None) != (Candidate.BodyType != ELevelBodyType::None))
	                         || (Update.Light && Update.Light->has_value() != Candidate.Light.has_value())
	                         || (Update.SkyAtmosphere && Update.SkyAtmosphere->has_value() != Candidate.SkyAtmosphere.has_value())
	                         || (Update.HeightFog && Update.HeightFog->has_value() != Candidate.HeightFog.has_value())
	                         || (Update.SoftBody && Update.SoftBody->has_value() != Candidate.SoftBody.has_value())
	                         || (Update.Mover && Update.Mover->has_value() != Candidate.Mover.has_value())
	                         || (Update.Trigger && Update.Trigger->has_value() != Candidate.Trigger.has_value());
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

	if (Update.Light)
	{
		Candidate.Light = *Update.Light;
	}

	if (Update.SkyAtmosphere)
	{
		Candidate.SkyAtmosphere = *Update.SkyAtmosphere;
	}

	if (Update.HeightFog)
	{
		Candidate.HeightFog = *Update.HeightFog;
	}

	if (Update.SoftBody)
	{
		Candidate.SoftBody = *Update.SoftBody;
	}

	if (Update.Mover)
	{
		Candidate.Mover = *Update.Mover;
	}

	if (Update.Trigger)
	{
		Candidate.Trigger = *Update.Trigger;
	}

	if (bStructural || Pending != Implementation.Changes.end())
	{
		Implementation.Changes[Live->Id] = {.Before = *Live, .After = std::move(Candidate)};
		return {};
	}

	const auto Result = Implementation.World.SetEntityForSystem(Entity, Candidate);
	return Result ? Result : std::expected<void, FLevelError>{Fail(Result.error().Message)};
}

std::expected<TMatrix4<double>, FLevelError> FLevelSystemContext::GetWorldMatrix(const FEntityId Entity)
{
	const auto Allowed = CheckAccess({.Read = ELevelComponent::Transform | ELevelComponent::Hierarchy});
	if (!Allowed)
	{
		return std::unexpected(Allowed.error());
	}

	const auto Result = Implementation.World.GetWorldMatrixForSystem(Entity);
	return Result ? Result : std::expected<TMatrix4<double>, FLevelError>{Fail(Result.error().Message)};
}

std::expected<FObjectId, FLevelError> FLevelSystemContext::QueueCreateEntity(FLevelEntity Entity)
{
	const auto Allowed = CheckAccess({.Write = ELevelComponent::All});
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
	Implementation.Changes.emplace(Id, FLevelEntityChange{.After = std::move(Entity)});
	return Id;
}

std::expected<void, FLevelError> FLevelSystemContext::QueueDestroyEntity(const FEntityId Entity)
{
	const auto Allowed = CheckAccess({.Write = ELevelComponent::All});
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

std::expected<std::vector<FLevelEvent>, FLevelError> FLevelSystemContext::ReadEvents(const std::string_view Type)
{
	if (!ContainsName(Implementation.Descriptor.ConsumeEvents, Type))
	{
		return Fail("System attempted undeclared event consumption");
	}

	std::vector<FLevelEvent> Events;

	for (const FLevelEvent& Event : Implementation.InputEvents)
	{
		if (Event.Type == Type)
		{
			Events.push_back(Event);
		}
	}

	return Events;
}

std::expected<void, FLevelError> FLevelSystemContext::PublishEvent(const ELevelSystemPhase ConsumePhase, FLevelEvent Event)
{
	if (!IsValidPhase(ConsumePhase) || !ContainsName(Implementation.Descriptor.ProduceEvents, Event.Type))
	{
		return Fail("System attempted undeclared event publication or used an invalid phase");
	}

	if (Event.Payload.size() > MaximumEventPayloadBytes || Implementation.RetainedEventCount + Implementation.OutputEvents.size() >= MaximumBufferedEvents)
	{
		return Fail("Level event payload or buffered event capacity exceeded");
	}

	Implementation.OutputEvents.push_back({.Phase = ConsumePhase, .Event = std::move(Event)});
	return {};
}

FLevelSystemScheduler::FLevelSystemScheduler(FWorld& InWorld)
    : Implementation(std::make_unique<FImplementation>(InWorld))
{
}

FLevelSystemScheduler::~FLevelSystemScheduler() = default;

std::expected<void, FLevelError> FLevelSystemScheduler::AddSystem(FLevelSystemDescriptor Descriptor, FLevelSystemFunction Function)
{
	if (Implementation->bRunning)
	{
		return std::unexpected(FLevelError{"Systems cannot be registered during execution"});
	}

	if (Descriptor.Name.empty() || !Function || !IsValidPhase(Descriptor.Phase) || !IsValidComponents(Descriptor.Access.Read | Descriptor.Access.Write)
	    || !ValidNames(Descriptor.After) || !ValidNames(Descriptor.ConsumeEvents) || !ValidNames(Descriptor.ProduceEvents)
	    || ContainsName(Descriptor.After, Descriptor.Name))
	{
		return std::unexpected(FLevelError{"System descriptor or callback is invalid"});
	}

	const std::string Name = Descriptor.Name;
	if (!Implementation->Systems.emplace(Name, FRegisteredLevelSystem{.Descriptor = std::move(Descriptor), .Function = std::move(Function)}).second)
	{
		return std::unexpected(FLevelError{"System name is already registered"});
	}

	return {};
}

std::expected<void, FLevelError> FLevelSystemScheduler::RemoveSystem(const std::string_view Name)
{
	if (Implementation->bRunning || Implementation->Systems.erase(std::string{Name}) == 0)
	{
		return std::unexpected(FLevelError{"System cannot be removed during execution or is not registered"});
	}

	return {};
}

std::expected<void, FLevelError> FLevelSystemScheduler::Clear()
{
	if (Implementation->bRunning)
	{
		return std::unexpected(FLevelError{"Scheduler cannot be cleared during execution"});
	}

	Implementation->Systems.clear();
	Implementation->Events.clear();
	return {};
}

std::expected<void, FLevelError> FLevelSystemScheduler::RunPhase(const ELevelSystemPhase Phase, const double DeltaSeconds)
{
	if (Implementation->bRunning || !IsValidPhase(Phase) || !std::isfinite(DeltaSeconds) || DeltaSeconds < 0.)
	{
		return std::unexpected(FLevelError{"Invalid phase, delta time, or recursive system execution"});
	}

	std::vector<FRegisteredLevelSystem*> Order;
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
				return std::unexpected(FLevelError{std::format("System '{}' has missing or cross-phase dependency '{}'", Name, Dependency)});
			}
		}
	}

	// ponytail: small system lists use O(systems squared) topological selection; cache a graph if registration scales.
	while (Order.size() < PhaseSystemCount)
	{
		const auto Ready = std::ranges::find_if(Implementation->Systems, [&](const auto& Pair)
		{
			const FLevelSystemDescriptor& Descriptor = Pair.second.Descriptor;
			return Descriptor.Phase == Phase && !Scheduled.contains(Pair.first)
			       && std::ranges::all_of(Descriptor.After, [&](const std::string& Name)
			{
				return Scheduled.contains(Name);
			});
		});

		if (Ready == Implementation->Systems.end())
		{
			return std::unexpected(FLevelError{"System dependencies contain a cycle"});
		}

		Scheduled.insert(Ready->first);
		Order.push_back(&Ready->second);
	}

	if (!Implementation->World.BeginSystemExecution())
	{
		return std::unexpected(FLevelError{"World is executing systems or has unflushed structural changes"});
	}

	Implementation->bRunning = true;
	const auto Unlock = [&](FWorld* World)
	{
		World->EndSystemExecution();
		Implementation->bRunning = false;
	};
	std::unique_ptr<FWorld, decltype(Unlock)> ExecutionGuard(&Implementation->World, Unlock);
	std::map<FObjectId, FLevelEntityChange> Changes;
	std::vector<FLevelEvent> InputEvents;
	std::vector<FBufferedLevelEvent> OutputEvents;

	for (const FBufferedLevelEvent& Event : Implementation->Events)
	{
		if (Event.Phase == Phase)
		{
			InputEvents.push_back(Event.Event);
		}
	}

	for (FRegisteredLevelSystem* System : Order)
	{
		FLevelSystemContext::FImplementation State(Implementation->World, System->Descriptor, DeltaSeconds, Changes, InputEvents, OutputEvents, Implementation->Events.size() - InputEvents.size());
		FLevelSystemContext Context(State);
		std::expected<void, FLevelError> Result;
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
			return std::unexpected(FLevelError{std::format("System '{}' failed: {}", System->Descriptor.Name, State.Failure ? State.Failure->Message : Result.error().Message)});
		}
	}

	ExecutionGuard.reset();
	std::vector<FLevelEntityChange> Batch;
	Batch.reserve(Changes.size());

	for (auto& [Id, Change] : Changes)
	{
		Batch.push_back(std::move(Change));
	}

	const auto Published = Implementation->World.ApplyEntityChanges(Batch);
	if (!Published)
	{
		return std::unexpected(FLevelError{std::format("System phase structural barrier rejected: {}", Published.error().Message)});
	}

	std::erase_if(Implementation->Events, [&](const FBufferedLevelEvent& Event)
	{
		return Event.Phase == Phase;
	});
	Implementation->Events.insert(Implementation->Events.end(), std::make_move_iterator(OutputEvents.begin()), std::make_move_iterator(OutputEvents.end()));
	return {};
}

std::size_t FLevelSystemScheduler::GetSystemCount() const
{
	return Implementation->Systems.size();
}

std::size_t FLevelSystemScheduler::GetBufferedEventCount() const
{
	return Implementation->Events.size();
}
}
