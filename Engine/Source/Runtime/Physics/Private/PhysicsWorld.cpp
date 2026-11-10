#include "Herta/Physics/PhysicsWorld.h"

#include "Herta/Core/Hash.h"
#include "Herta/Tasks/TaskSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr JPH::ObjectLayer StaticLayer = PhysicsLayerStatic;
constexpr JPH::ObjectLayer DynamicLayer = PhysicsLayerDynamic;
constexpr JPH::ObjectLayer CharacterLayer = PhysicsLayerCharacter;
constexpr std::size_t MaximumRagdollParts = 64;
// Hull points sit on the surface, so a small convex radius keeps thin fracture pieces valid while still rounding contacts.
constexpr float HullConvexRadius = 0.01f;

[[nodiscard]] constexpr std::size_t RequiredTempMemory(const FPhysicsWorldSettings& Settings)
{
	// Jolt 5.6: one discrete step, at most three constraints per body (a rope joint, its length limit, and an attached load), and no parallel island splitting.
	// Cover contact buffers, island and constraint arrays, CCD-index mapping, soft-body update contexts, and alignment padding before Jolt can abort.
	return sizeof(JPH::PhysicsUpdateContext::Step) + std::size_t{320} * 1024
	       + static_cast<std::size_t>(Settings.MaxBodies) * (192 + 3 * 64)
	       + static_cast<std::size_t>(Settings.MaxBodyPairs) * sizeof(JPH::BodyPair)
	       + static_cast<std::size_t>(Settings.MaxContactConstraints) * (JPH::ContactConstraintManager::cMaxConstraintSize + 32);
}

std::mutex RuntimeMutex;
std::size_t RuntimeUsers = 0;
JPH::TraceFunction PreviousTrace = nullptr;
#ifdef JPH_ENABLE_ASSERTS
JPH::AssertFailedFunction PreviousAssertFailed = nullptr;
#endif

void TraceJolt(const char* Format, ...)
{
	std::va_list Arguments;
	va_start(Arguments, Format);
	std::fputs("Jolt: ", stderr);
	std::vfprintf(stderr, Format, Arguments);
	std::fputc('\n', stderr);
	va_end(Arguments);
}

#ifdef JPH_ENABLE_ASSERTS
bool AssertJolt(const char* Expression, const char* Message, const char* File, JPH::uint Line)
{
	std::fprintf(stderr, "Jolt assertion at %s:%u: %s%s%s\n", File, Line, Expression, Message ? " - " : "", Message ? Message : "");

	// Jolt asserts whenever an update reports an error, but capacity overflow is a reported FPhysicsWorld::Step failure, not a programming error, so it must not trap.
	return std::string_view(Expression) != "errors == EPhysicsUpdateError::None";
}
#endif

// Returns false when something outside Herta Physics already registered Jolt.
[[nodiscard]] bool AcquireRuntime()
{
	std::scoped_lock Lock(RuntimeMutex);
	if (RuntimeUsers != 0)
	{
		++RuntimeUsers;
		return true;
	}

	if (JPH::Factory::sInstance != nullptr)
	{
		return false;
	}

	JPH::RegisterDefaultAllocator();
	auto Factory = std::make_unique<JPH::Factory>();
	PreviousTrace = JPH::Trace;
	JPH::Trace = TraceJolt;
#ifdef JPH_ENABLE_ASSERTS
	PreviousAssertFailed = JPH::AssertFailed;
	JPH::AssertFailed = AssertJolt;
#endif
	JPH::Factory::sInstance = Factory.release();
	JPH::RegisterTypes();
	RuntimeUsers = 1;
	return true;
}

void ReleaseRuntime()
{
	std::scoped_lock Lock(RuntimeMutex);
	if (--RuntimeUsers != 0)
	{
		return;
	}

	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;
	JPH::Trace = PreviousTrace;
#ifdef JPH_ENABLE_ASSERTS
	JPH::AssertFailed = PreviousAssertFailed;
#endif
}

[[nodiscard]] bool IsFinite(const FVector3& Value)
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
}

[[nodiscard]] bool IsFinite(const FQuaternion& Value)
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z) && std::isfinite(Value.W);
}

[[nodiscard]] bool IsInRange(const float Value, const float Minimum, const float Maximum)
{
	return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] JPH::Vec3 ToJolt(const FVector3& Value)
{
	return {Value.X, Value.Y, Value.Z};
}

[[nodiscard]] JPH::Quat ToJolt(const FQuaternion& Value)
{
	return {Value.X, Value.Y, Value.Z, Value.W};
}

[[nodiscard]] JPH::BodyID FindBody(const std::vector<JPH::BodyID>& BodyIds, const FPhysicsBodyId BodyId)
{
	const JPH::BodyID Id(BodyId.Value);
	if ((BodyId.Value & JPH::BodyID::cBroadPhaseBit) != 0 || Id.IsInvalid() || Id.GetIndex() >= BodyIds.size() || BodyIds[Id.GetIndex()] != Id)
	{
		return {};
	}

	return Id;
}

// Jolt calls listeners from its job threads, in no fixed order, so they only buffer under a lock; Step sorts what they gathered. Nothing here may
// touch the world: Jolt holds body locks while it calls back.
class FSensorListener final : public JPH::ContactListener
{
public:
	void OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold, JPH::ContactSettings& Settings) override;
	void OnContactRemoved(const JPH::SubShapeIDPair& Pair) override;

	// Written only between steps.
	std::unordered_set<std::uint32_t> Sensors;
	std::unordered_set<std::uint32_t> Reporters;
	std::mutex EventMutex;
	std::vector<FPhysicsSensorEvent> Events;
	std::vector<FPhysicsContactEvent> Contacts;
};

constexpr JPH::uint64 ReportContactsFlag = 1;

[[nodiscard]] FVector3 ToHerta(const JPH::Vec3 Value)
{
	return {Value.GetX(), Value.GetY(), Value.GetZ()};
}

[[nodiscard]] float InverseMass(const JPH::Body& Body)
{
	return Body.IsDynamic() ? Body.GetMotionPropertiesUnchecked()->GetInverseMassUnchecked() : 0.f;
}

void FSensorListener::OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold, JPH::ContactSettings& Settings)
{
	if (!Body1.IsSensor() && !Body2.IsSensor() && ((Body1.GetUserData() | Body2.GetUserData()) & ReportContactsFlag) != 0 && Manifold.mRelativeContactPointsOn1.size() != 0)
	{
		JPH::Vec3 Center = JPH::Vec3::sZero();
		for (JPH::uint Index = 0; Index < Manifold.mRelativeContactPointsOn1.size(); ++Index)
		{
			Center += Manifold.mRelativeContactPointsOn1[Index];
		}

		const JPH::RVec3 Point = Manifold.mBaseOffset + Center / static_cast<float>(Manifold.mRelativeContactPointsOn1.size());
		const JPH::Vec3 Velocity1 = Body1.GetPointVelocityCOM(JPH::Vec3(Point - Body1.GetCenterOfMassPosition()));
		const JPH::Vec3 Velocity2 = Body2.GetPointVelocityCOM(JPH::Vec3(Point - Body2.GetCenterOfMassPosition()));
		// Jolt's normal points from Body1 into Body2, so closing bodies have a positive approach speed along it.
		const float Approach = std::max((Velocity1 - Velocity2).Dot(Manifold.mWorldSpaceNormal), 0.f);
		const float InverseMasses = InverseMass(Body1) + InverseMass(Body2);
		const float Impulse = InverseMasses > 0.f ? Approach * (1.f + Settings.mCombinedRestitution) / InverseMasses : 0.f;
		const bool bSwap = Body2.GetID().GetIndexAndSequenceNumber() < Body1.GetID().GetIndexAndSequenceNumber();
		const FPhysicsContactEvent Event{
		    .First = FPhysicsBodyId{(bSwap ? Body2 : Body1).GetID().GetIndexAndSequenceNumber()},
		    .Second = FPhysicsBodyId{(bSwap ? Body1 : Body2).GetID().GetIndexAndSequenceNumber()},
		    .Phase = EPhysicsContactPhase::Began,
		    .Point = ToHerta(JPH::Vec3(Point)),
		    .Normal = ToHerta(bSwap ? -Manifold.mWorldSpaceNormal : Manifold.mWorldSpaceNormal),
		    .ApproachSpeed = Approach,
		    .Impulse = Impulse,
		};

		std::scoped_lock Lock(EventMutex);
		Contacts.push_back(Event);
	}

	if (Body1.IsSensor() != Body2.IsSensor())
	{
		const JPH::Body& Sensor = Body1.IsSensor() ? Body1 : Body2;
		const JPH::Body& Other = Body1.IsSensor() ? Body2 : Body1;
		std::scoped_lock Lock(EventMutex);
		Events.push_back({.Sensor = FPhysicsBodyId{Sensor.GetID().GetIndexAndSequenceNumber()}, .Body = FPhysicsBodyId{Other.GetID().GetIndexAndSequenceNumber()}, .bEntered = true});
	}
}

void FSensorListener::OnContactRemoved(const JPH::SubShapeIDPair& Pair)
{
	// Removal only names the bodies, which may already be gone, so sensors and reporters are remembered by ID.
	const std::uint32_t Body1 = Pair.GetBody1ID().GetIndexAndSequenceNumber();
	const std::uint32_t Body2 = Pair.GetBody2ID().GetIndexAndSequenceNumber();
	const bool bAnySensor = Sensors.contains(Body1) || Sensors.contains(Body2);
	if (!bAnySensor && (Reporters.contains(Body1) || Reporters.contains(Body2)))
	{
		std::scoped_lock Lock(EventMutex);
		Contacts.push_back({.First = FPhysicsBodyId{std::min(Body1, Body2)}, .Second = FPhysicsBodyId{std::max(Body1, Body2)}, .Phase = EPhysicsContactPhase::Ended});
	}

	const bool bSensor1 = Sensors.contains(Body1);
	if (bSensor1 != Sensors.contains(Body2))
	{
		std::scoped_lock Lock(EventMutex);
		Events.push_back({.Sensor = FPhysicsBodyId{bSensor1 ? Body1 : Body2}, .Body = FPhysicsBodyId{bSensor1 ? Body2 : Body1}, .bEntered = false});
	}
}

// Runs Jolt's jobs as Herta tasks. Every job Jolt waits for sits in a barrier that runs ready jobs on the waiting thread too, so a step finishes
// even when the task queue is full or every worker is busy; a job runs once whichever thread reaches it first.
class FTaskJobSystem final : public JPH::JobSystemWithBarrier
{
public:
	FTaskJobSystem(FTaskSystem& InTasks, std::unique_ptr<FTaskScope> InScope, std::uint32_t MaxJobs, std::uint32_t InMaxConcurrency);
	~FTaskJobSystem() override;
	FTaskJobSystem(const FTaskJobSystem&) = delete;
	FTaskJobSystem& operator=(const FTaskJobSystem&) = delete;
	FTaskJobSystem(FTaskJobSystem&&) = delete;
	FTaskJobSystem& operator=(FTaskJobSystem&&) = delete;

	int GetMaxConcurrency() const override;
	JPH::JobHandle CreateJob(const char* Name, JPH::ColorArg Color, const JobFunction& Function, JPH::uint32 Dependencies = 0) override;
	// Waits for the tasks a step submitted, so none outlives the step that queued it.
	void DrainSubmitted();

protected:
	void QueueJob(Job* InJob) override;
	void QueueJobs(Job** Jobs, JPH::uint Count) override;
	void FreeJob(Job* InJob) override;

private:
	FTaskSystem& Tasks;
	std::unique_ptr<FTaskScope> Scope;
	std::uint32_t MaxConcurrency = 1;
	JPH::FixedSizeFreeList<Job> Jobs;
	std::mutex SubmittedMutex;
	std::vector<FTaskHandle> Submitted;
};

FTaskJobSystem::FTaskJobSystem(FTaskSystem& InTasks, std::unique_ptr<FTaskScope> InScope, const std::uint32_t MaxJobs, const std::uint32_t InMaxConcurrency)
    : JobSystemWithBarrier(8)
    , Tasks(InTasks)
    , Scope(std::move(InScope))
    , MaxConcurrency(InMaxConcurrency)
{
	Jobs.Init(MaxJobs, MaxJobs);
}

FTaskJobSystem::~FTaskJobSystem()
{
	DrainSubmitted();
}

int FTaskJobSystem::GetMaxConcurrency() const
{
	return static_cast<int>(MaxConcurrency);
}

JPH::JobHandle FTaskJobSystem::CreateJob(const char* const Name, const JPH::ColorArg Color, const JobFunction& Function, const JPH::uint32 Dependencies)
{
	JPH::uint32 Index = Jobs.ConstructObject(Name, Color, this, Function, Dependencies);
	while (Index == decltype(Jobs)::cInvalidObjectIndex)
	{
		// MaxJobs bounds what Jolt queues per step; running out means a job is about to finish elsewhere.
		std::this_thread::yield();
		Index = Jobs.ConstructObject(Name, Color, this, Function, Dependencies);
	}

	Job* const Created = &Jobs.Get(Index);
	JPH::JobHandle Handle(Created);
	if (Dependencies == 0)
	{
		QueueJob(Created);
	}

	return Handle;
}

void FTaskJobSystem::QueueJob(Job* const InJob)
{
	InJob->AddRef();
	auto Task = Tasks.Submit(*Scope, {.Name = "Physics job", .Priority = ETaskPriority::High}, [InJob](FTaskContext&)
	{
		InJob->Execute();
		InJob->Release();
	});

	if (!Task)
	{
		// A full queue or an unregistered thread leaves the job to the barrier that waits for it.
		InJob->Release();
		return;
	}

	std::scoped_lock Lock(SubmittedMutex);
	Submitted.push_back(std::move(*Task));
}

void FTaskJobSystem::QueueJobs(Job** const Queued, const JPH::uint Count)
{
	for (JPH::uint Index = 0; Index < Count; ++Index)
	{
		QueueJob(Queued[Index]);
	}
}

void FTaskJobSystem::FreeJob(Job* const InJob)
{
	Jobs.DestructObject(InJob);
}

void FTaskJobSystem::DrainSubmitted()
{
	std::vector<FTaskHandle> Pending;
	{
		std::scoped_lock Lock(SubmittedMutex);
		Pending.swap(Submitted);
	}

	for (const FTaskHandle& Task : Pending)
	{
		(void)Task.Wait();
	}
}

// Jointed bodies overlap at the joint, and contacts there would push them apart. Each joined body uses its index as its group.
class FJoinedBodiesFilter final : public JPH::GroupFilter
{
public:
	bool CanCollide(const JPH::CollisionGroup& Group1, const JPH::CollisionGroup& Group2) const override;

	std::unordered_set<std::uint64_t> JoinedPairs;
};

[[nodiscard]] std::uint64_t PairKey(const std::uint32_t Group1, const std::uint32_t Group2)
{
	return static_cast<std::uint64_t>(std::min(Group1, Group2)) << 32 | std::max(Group1, Group2);
}

bool FJoinedBodiesFilter::CanCollide(const JPH::CollisionGroup& Group1, const JPH::CollisionGroup& Group2) const
{
	return !JoinedPairs.contains(PairKey(Group1.GetGroupID(), Group2.GetGroupID()));
}

// Characters walk through triggers; gameplay tests trigger overlap itself.
class FCharacterBodyFilter final : public JPH::BodyFilter
{
public:
	bool ShouldCollideLocked(const JPH::Body& Body) const override;
};

bool FCharacterBodyFilter::ShouldCollideLocked(const JPH::Body& Body) const
{
	return !Body.IsSensor();
}

// A light chain carrying a heavy load converges slowly; the island takes the largest step count of its constraints.
constexpr JPH::uint JointVelocitySteps = 24;
constexpr JPH::uint JointPositionSteps = 6;

struct FRopeState
{
	std::vector<JPH::BodyID> Segments{};
	float HalfLength = 0.f;
};

template <std::size_t Count>
[[nodiscard]] bool AreValidIndices(const std::span<const std::array<std::uint32_t, Count>> Elements, const std::size_t VertexCount)
{
	return std::ranges::all_of(Elements, [VertexCount](const std::array<std::uint32_t, Count>& Element)
	{
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			if (Element[Index] >= VertexCount || std::ranges::count(Element, Element[Index]) != 1)
			{
				return false;
			}
		}

		return true;
	});
}
}

struct FPhysicsWorld::FImplementation
{
	JPH::BroadPhaseLayerInterfaceTable BroadPhaseLayers{PhysicsLayerCount, 2};
	JPH::ObjectLayerPairFilterTable CollisionLayers{PhysicsLayerCount};
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> BroadPhaseFilter;
	// Declared before Physics so it outlives the system that holds a pointer to it.
	FSensorListener SensorListener;
	JPH::PhysicsSystem Physics;
	JPH::TempAllocatorImpl TempAllocator;
	// Exactly one is set: steps run on Herta tasks when the world has a task system.
	std::unique_ptr<JPH::JobSystemSingleThreaded> SingleThreaded;
	std::unique_ptr<FTaskJobSystem> TaskJobs;
	std::vector<JPH::BodyID> BodyIds;
	std::vector<JPH::Ref<JPH::Constraint>> Constraints;
	// Bodies a constraint references, which DestroyBody refuses so no constraint points at a missing body.
	std::unordered_set<std::uint32_t> ConstrainedBodies;
	std::vector<FRopeState> Ropes;

	struct FRagdollState
	{
		std::vector<JPH::BodyID> Parts;
		std::vector<JPH::Ref<JPH::Constraint>> Joints;
	};

	// Destroyed ragdolls leave an empty slot, so indices stay stable.
	std::vector<FRagdollState> Ragdolls;
	// Each character owns an inner body in Physics, so characters are released before the bodies.
	std::vector<JPH::Ref<JPH::CharacterVirtual>> Characters;

	struct FCharacterShape
	{
		float Radius = 0.f;
		float StepHeight = 0.f;
	};

	std::vector<FCharacterShape> CharacterShapes;
	JPH::Ref<FJoinedBodiesFilter> JoinedBodies = new FJoinedBodiesFilter;
	const FPhysicsWorldSettings Settings;
	FWorldPosition Origin{};
	std::string StepFailure;
	std::size_t BodyCount = 0;

	FImplementation(const FPhysicsWorldSettings& InSettings, std::unique_ptr<FTaskScope> Scope)
	    : TempAllocator(InSettings.TempMemoryBytes)
	    , Settings(InSettings)
	{
		if (Scope)
		{
			const auto Concurrency = InSettings.MaxConcurrency != 0 ? InSettings.MaxConcurrency : static_cast<std::uint32_t>(InSettings.Tasks->GetCpuWorkerCount() + 1);
			TaskJobs = std::make_unique<FTaskJobSystem>(*InSettings.Tasks, std::move(Scope), InSettings.MaxJobs, std::max(Concurrency, 1u));
		}
		else
		{
			SingleThreaded = std::make_unique<JPH::JobSystemSingleThreaded>(InSettings.MaxJobs);
		}

		// Two broad-phase trees, one for bodies that never move and one for everything else, which is what Jolt's broad phase is tuned for.
		for (JPH::ObjectLayer Layer = 0; Layer < PhysicsLayerCount; ++Layer)
		{
			BroadPhaseLayers.MapObjectToBroadPhaseLayer(Layer, JPH::BroadPhaseLayer(Layer == StaticLayer ? 0 : 1));
			for (JPH::ObjectLayer Other = Layer; Other < PhysicsLayerCount; ++Other)
			{
				if ((InSettings.LayerCollisions[Layer] >> Other & 1u) != 0)
				{
					CollisionLayers.EnableCollision(Layer, Other);
				}
			}
		}

		BroadPhaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(BroadPhaseLayers, 2, CollisionLayers, PhysicsLayerCount);
		Physics.Init(Settings.MaxBodies, 0, Settings.MaxBodyPairs, Settings.MaxContactConstraints, BroadPhaseLayers, *BroadPhaseFilter, CollisionLayers);
		JPH::PhysicsSettings PhysicsSettings = Physics.GetPhysicsSettings();
		PhysicsSettings.mMaxInFlightBodyPairs = static_cast<int>(Settings.MaxBodyPairs);
		PhysicsSettings.mUseLargeIslandSplitter = false;
		Physics.SetPhysicsSettings(PhysicsSettings);
		Physics.SetGravity(JPH::Vec3(0.f, -9.80665f, 0.f));
		Physics.SetContactListener(&SensorListener);
		BodyIds.resize(Settings.MaxBodies);
	}

	~FImplementation()
	{
		Characters.clear();
		for (const JPH::Ref<JPH::Constraint>& Constraint : Constraints)
		{
			Physics.RemoveConstraint(Constraint);
		}

		JPH::BodyInterface& Bodies = Physics.GetBodyInterface();
		for (const JPH::BodyID Id : BodyIds)
		{
			if (Id.IsInvalid())
			{
				continue;
			}

			Bodies.RemoveBody(Id);
			Bodies.DestroyBody(Id);
		}
	}

	FImplementation(const FImplementation&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	// An invalid Body1 fixes Body2 to the world; otherwise the two stop colliding with each other.
	void AddJoint(const JPH::TwoBodyConstraintSettings& Joint, JPH::BodyID Body1, JPH::BodyID Body2);
	[[nodiscard]] JPH::CharacterVirtual* FindCharacter(FPhysicsCharacterId Character) const;
	[[nodiscard]] FPhysicsCharacterState GetCharacterState(const JPH::CharacterVirtual& Character) const;
};

JPH::CharacterVirtual* FPhysicsWorld::FImplementation::FindCharacter(const FPhysicsCharacterId Character) const
{
	return Character.Value > 0 && Character.Value <= Characters.size() ? Characters[Character.Value - 1].GetPtr() : nullptr;
}

namespace
{
[[nodiscard]] JPH::ShapeSettings::ShapeResult MakeCharacterShape(const float Height, const float Radius)
{
	// The capsule is lifted so the character position is its feet.
	const JPH::RefConst<JPH::Shape> Capsule = new JPH::CapsuleShape(Height * 0.5f - Radius, Radius);
	return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.f, Height * 0.5f, 0.f), JPH::Quat::sIdentity(), Capsule).Create();
}
}

FPhysicsCharacterState FPhysicsWorld::FImplementation::GetCharacterState(const JPH::CharacterVirtual& Character) const
{
	const JPH::RVec3 Position = Character.GetPosition();
	const JPH::Vec3 Velocity = Character.GetLinearVelocity();
	const JPH::Vec3 Ground = Character.GetGroundVelocity();
	const bool bGrounded = Character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
	const JPH::Vec3 GroundNormal = bGrounded ? Character.GetGroundNormal() : JPH::Vec3::sAxisY();
	FPhysicsCharacterState State{
	    .FootPosition = {Position.GetX(), Position.GetY(), Position.GetZ()},
	    .Velocity = {Velocity.GetX(), Velocity.GetY(), Velocity.GetZ()},
	    .GroundVelocity = {Ground.GetX(), Ground.GetY(), Ground.GetZ()},
	    .GroundNormal = {GroundNormal.GetX(), GroundNormal.GetY(), GroundNormal.GetZ()},
	    .bGrounded = bGrounded,
	};

	// Contacts within the character's padding count as touching. A wall is steeper than about 70 degrees, and the closest one wins.
	float Closest = 0.05f;
	for (const JPH::CharacterContact& Contact : Character.GetActiveContacts())
	{
		const JPH::Vec3 Normal = Contact.mSurfaceNormal;
		const JPH::Vec3 Horizontal(Normal.GetX(), 0.f, Normal.GetZ());
		if (Contact.mIsSensorB || Contact.mWasDiscarded || std::abs(Normal.GetY()) > 0.35f || Contact.mDistance > Closest || Horizontal.LengthSq() < 0.01f)
		{
			continue;
		}

		const JPH::Vec3 Away = Horizontal.Normalized();
		Closest = Contact.mDistance;
		State.WallNormal = {Away.GetX(), 0.f, Away.GetZ()};
		State.bTouchingWall = true;
	}

	return State;
}

void FPhysicsWorld::FImplementation::AddJoint(const JPH::TwoBodyConstraintSettings& Joint, const JPH::BodyID Body1, const JPH::BodyID Body2)
{
	JPH::BodyInterface& Bodies = Physics.GetBodyInterface();
	const JPH::Ref<JPH::Constraint> Constraint = Bodies.CreateConstraint(&Joint, Body1, Body2);
	Constraint->SetNumVelocityStepsOverride(JointVelocitySteps);
	Constraint->SetNumPositionStepsOverride(JointPositionSteps);
	Physics.AddConstraint(Constraint);
	Constraints.push_back(Constraint);
	ConstrainedBodies.insert(Body2.GetIndexAndSequenceNumber());
	if (!Body1.IsInvalid())
	{
		ConstrainedBodies.insert(Body1.GetIndexAndSequenceNumber());
	}

	if (!Body1.IsInvalid())
	{
		for (const JPH::BodyID Id : {Body1, Body2})
		{
			const JPH::BodyLockWrite Lock(Physics.GetBodyLockInterface(), Id);
			Lock.GetBody().SetCollisionGroup(JPH::CollisionGroup(JoinedBodies, Id.GetIndex(), 0));
		}

		JoinedBodies->JoinedPairs.insert(PairKey(Body1.GetIndex(), Body2.GetIndex()));
	}

	Bodies.ActivateBody(Body2);
}

FPhysicsWorld::FPhysicsWorld(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FPhysicsWorld::~FPhysicsWorld()
{
	Implementation.reset();
	ReleaseRuntime();
}

std::expected<std::unique_ptr<FPhysicsWorld>, FPhysicsError> FPhysicsWorld::Create(const FPhysicsWorldSettings& Settings)
{
	if (Settings.MaxBodies == 0 || Settings.MaxBodies > JPH::BodyID::cMaxBodyIndex + 1
	    || Settings.MaxBodyPairs < 4 || Settings.MaxBodyPairs > JPH::ContactConstraintManager::cMaxBodyPairsLimit
	    || Settings.MaxContactConstraints < 4 || Settings.MaxContactConstraints > JPH::ContactConstraintManager::cMaxContactConstraintsLimit)
	{
		return std::unexpected(FPhysicsError{"Physics capacities exceed supported body, pair, or contact limits (pairs and contacts require at least 4)"});
	}

	const std::size_t RequiredBytes = RequiredTempMemory(Settings);
	if (Settings.TempMemoryBytes < RequiredBytes)
	{
		return std::unexpected(FPhysicsError{std::format("Physics temporary-memory capacity is {} bytes; configured body, pair, and contact limits require at least {} bytes", Settings.TempMemoryBytes, RequiredBytes)});
	}

	if (Settings.MaxJobs < 64 || Settings.MaxJobs > 65536)
	{
		return std::unexpected(FPhysicsError{"Physics job capacity must be between 64 and 65536"});
	}

	for (std::uint8_t Layer = 0; Layer < PhysicsLayerCount; ++Layer)
	{
		for (std::uint8_t Other = 0; Other < PhysicsLayerCount; ++Other)
		{
			if ((Settings.LayerCollisions[Layer] >> Other & 1u) != (Settings.LayerCollisions[Other] >> Layer & 1u))
			{
				return std::unexpected(FPhysicsError{std::format("Physics layer collisions must be symmetric: layer {} and layer {} disagree", Layer, Other)});
			}
		}
	}

	std::unique_ptr<FTaskScope> Scope;
	if (Settings.Tasks)
	{
		auto Created = Settings.Tasks->CreateScope("Physics");
		if (!Created)
		{
			return std::unexpected(FPhysicsError{"Cannot create the physics task scope: " + Created.error().Message});
		}

		Scope = std::move(*Created);
	}

	if (!AcquireRuntime())
	{
		return std::unexpected(FPhysicsError{"Jolt is already registered outside Herta Physics"});
	}

	return std::unique_ptr<FPhysicsWorld>(new FPhysicsWorld(std::make_unique<FImplementation>(Settings, std::move(Scope))));
}

std::expected<FPhysicsBodyId, FPhysicsError> FPhysicsWorld::CreateBody(const FPhysicsBodySettings& Settings)
{
	if (Implementation->BodyCount >= Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	if (Settings.Shape > EPhysicsShape::Compound)
	{
		return std::unexpected(FPhysicsError{"Unsupported body shape"});
	}

	if (Settings.MotionType != EPhysicsMotionType::Static && (Settings.Layer == StaticLayer || Settings.Layer >= PhysicsLayerCount))
	{
		return std::unexpected(FPhysicsError{"Moving bodies need a moving collision layer below PhysicsLayerCount"});
	}

	const auto IsValidHull = [](const std::span<const FVector3> Points)
	{
		return Points.size() >= 4 && Points.size() <= 256 && std::ranges::all_of(Points, [](const FVector3& Point)
		{
			return IsFinite(Point);
		});
	};

	if ((Settings.Shape == EPhysicsShape::ConvexHull && !IsValidHull(Settings.HullPoints)) || (Settings.Shape == EPhysicsShape::Compound && (Settings.CompoundHulls.empty() || Settings.CompoundHulls.size() > 1024 || !std::ranges::all_of(Settings.CompoundHulls, IsValidHull))))
	{
		return std::unexpected(FPhysicsError{"Convex hulls need 4 to 256 finite points, and compounds 1 to 1024 such hulls"});
	}

	if (Settings.Shape == EPhysicsShape::Box && (!IsFinite(Settings.HalfExtents) || Settings.HalfExtents.X <= 0.f || Settings.HalfExtents.Y <= 0.f || Settings.HalfExtents.Z <= 0.f))
	{
		return std::unexpected(FPhysicsError{"Box half extents must be finite and positive"});
	}

	if ((Settings.Shape == EPhysicsShape::Sphere || Settings.Shape == EPhysicsShape::Capsule) && (!IsInRange(Settings.Radius, 0.001f, 100000.f) || (Settings.Shape == EPhysicsShape::Capsule && !IsInRange(Settings.HalfHeight, 0.f, 100000.f))))
	{
		return std::unexpected(FPhysicsError{"Sphere and capsule radii must be between 1 mm and 100 km, with a finite, non-negative capsule half height"});
	}

	const float RotationLengthSquared = Settings.Rotation.LengthSquared();
	if (!IsFinite(Settings.Position) || !IsFinite(Settings.Rotation) || !std::isfinite(RotationLengthSquared) || RotationLengthSquared <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Body position and rotation must be finite, with a nonzero rotation"});
	}

	if (Settings.MotionType != EPhysicsMotionType::Static && Settings.MotionType != EPhysicsMotionType::Dynamic && Settings.MotionType != EPhysicsMotionType::Kinematic)
	{
		return std::unexpected(FPhysicsError{"Unsupported body motion type"});
	}

	const FPhysicsBodyProperties& Properties = Settings.Properties;
	if (!IsInRange(Properties.MassKg, 0.001f, 1000000.f))
	{
		return std::unexpected(FPhysicsError{"Body mass must be finite and between 0.001 and 1000000 kg"});
	}

	if (!IsInRange(Properties.Friction, 0.f, 1.f) || !IsInRange(Properties.Restitution, 0.f, 1.f) || !IsInRange(Properties.LinearDamping, 0.f, 1.f) || !IsInRange(Properties.AngularDamping, 0.f, 1.f))
	{
		return std::unexpected(FPhysicsError{"Body friction, restitution, and damping must be finite and between 0 and 1"});
	}

	if (!IsInRange(Properties.GravityScale, 0.f, 10.f))
	{
		return std::unexpected(FPhysicsError{"Body gravity scale must be finite and between 0 and 10"});
	}

	const auto HullSettings = [](const std::span<const FVector3> Points)
	{
		JPH::Array<JPH::Vec3> Converted;
		Converted.reserve(Points.size());
		for (const FVector3& Point : Points)
		{
			Converted.push_back(ToJolt(Point));
		}

		return JPH::Ref<JPH::ConvexHullShapeSettings>(new JPH::ConvexHullShapeSettings(Converted, HullConvexRadius));
	};

	JPH::ShapeSettings::ShapeResult Shape;
	if (Settings.Shape == EPhysicsShape::ConvexHull)
	{
		Shape = HullSettings(Settings.HullPoints)->Create();
	}
	else if (Settings.Shape == EPhysicsShape::Compound)
	{
		JPH::StaticCompoundShapeSettings Compound;
		for (const std::span<const FVector3> Hull : Settings.CompoundHulls)
		{
			Compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), HullSettings(Hull));
		}

		Shape = Compound.Create();
	}
	else
	{
		// A capsule without a cylinder is a sphere; Jolt rejects a zero half height.
		Shape = Settings.Shape == EPhysicsShape::Box                                    ? JPH::BoxShapeSettings(ToJolt(Settings.HalfExtents)).Create()
		        : Settings.Shape == EPhysicsShape::Sphere || Settings.HalfHeight <= 0.f ? JPH::SphereShapeSettings(Settings.Radius).Create()
		                                                                                : JPH::CapsuleShapeSettings(Settings.HalfHeight, Settings.Radius).Create();
	}

	if (Shape.HasError())
	{
		return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
	}

	const bool bDynamic = Settings.MotionType == EPhysicsMotionType::Dynamic;
	const bool bMoving = Settings.MotionType != EPhysicsMotionType::Static;
	const JPH::EMotionType MotionType = bDynamic ? JPH::EMotionType::Dynamic : bMoving ? JPH::EMotionType::Kinematic
	                                                                                   : JPH::EMotionType::Static;
	const FQuaternion Rotation = Settings.Rotation.NormalizedOrIdentity();
	JPH::BodyCreationSettings BodySettings(Shape.Get().GetPtr(), ToJolt(Settings.Position), ToJolt(Rotation), MotionType, bMoving ? JPH::ObjectLayer{Settings.Layer} : StaticLayer);
	BodySettings.mFriction = Properties.Friction;
	BodySettings.mRestitution = Properties.Restitution;
	BodySettings.mIsSensor = Settings.bSensor;
	BodySettings.mUserData = Settings.bReportContacts ? ReportContactsFlag : 0;
	BodySettings.mMotionQuality = Settings.bContinuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
	// A sleeping sensor stops reporting, and a resting body inside it would never be seen leaving.
	BodySettings.mAllowSleeping = !Settings.bSensor;

	if (bDynamic)
	{
		// Let Jolt scale the shape's inertia to the authored mass.
		BodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		BodySettings.mMassPropertiesOverride.mMass = Properties.MassKg;
		BodySettings.mLinearDamping = Properties.LinearDamping;
		BodySettings.mAngularDamping = Properties.AngularDamping;
		BodySettings.mGravityFactor = Properties.GravityScale;
	}

	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	const JPH::BodyID Id = Bodies.CreateAndAddBody(BodySettings, bMoving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Physics world body capacity reached"});
	}

	Implementation->BodyIds[Id.GetIndex()] = Id;
	++Implementation->BodyCount;
	if (Settings.bSensor)
	{
		Implementation->SensorListener.Sensors.insert(Id.GetIndexAndSequenceNumber());
	}

	if (Settings.bReportContacts)
	{
		Implementation->SensorListener.Reporters.insert(Id.GetIndexAndSequenceNumber());
	}

	return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
}

std::expected<void, FPhysicsError> FPhysicsWorld::DestroyBody(const FPhysicsBodyId BodyId)
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	if (Implementation->ConstrainedBodies.contains(BodyId.Value))
	{
		return std::unexpected(FPhysicsError{"Bodies held by constraints or ropes stay for the world's lifetime"});
	}

	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	Bodies.RemoveBody(Id);
	Bodies.DestroyBody(Id);
	Implementation->BodyIds[Id.GetIndex()] = {};
	Implementation->SensorListener.Sensors.erase(BodyId.Value);
	Implementation->SensorListener.Reporters.erase(BodyId.Value);
	--Implementation->BodyCount;
	return {};
}

std::size_t FPhysicsWorld::GetBodyCount() const noexcept
{
	return Implementation->BodyCount;
}

std::expected<FVector3, FPhysicsError> FPhysicsWorld::GetLinearVelocity(const FPhysicsBodyId BodyId) const
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	return ToHerta(Implementation->Physics.GetBodyInterface().GetLinearVelocity(Id));
}

std::expected<FVector3, FPhysicsError> FPhysicsWorld::GetAngularVelocity(const FPhysicsBodyId BodyId) const
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	return ToHerta(Implementation->Physics.GetBodyInterface().GetAngularVelocity(Id));
}

std::expected<void, FPhysicsError> FPhysicsWorld::SetBodyState(const FPhysicsBodyId BodyId, const FPhysicsBodyTransform& Transform, const FVector3& Linear, const FVector3& Angular)
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	if (Id.IsInvalid() || Bodies.GetMotionType(Id) != JPH::EMotionType::Dynamic || !IsFinite(Transform.Position) || !IsFinite(Linear) || !IsFinite(Angular))
	{
		return std::unexpected(FPhysicsError{"Only dynamic bodies take a finite state"});
	}

	Bodies.SetPositionAndRotation(Id, JPH::RVec3(ToJolt(Transform.Position)), ToJolt(Transform.Rotation.NormalizedOrIdentity()), JPH::EActivation::Activate);
	Bodies.SetLinearAndAngularVelocity(Id, ToJolt(Linear), ToJolt(Angular));
	return {};
}

std::expected<void, FPhysicsError> FPhysicsWorld::SetVelocity(const FPhysicsBodyId BodyId, const FVector3& Linear, const FVector3& Angular)
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	if (Id.IsInvalid() || Bodies.GetMotionType(Id) != JPH::EMotionType::Dynamic || !IsFinite(Linear) || !IsFinite(Angular))
	{
		return std::unexpected(FPhysicsError{"Only dynamic bodies take finite velocities"});
	}

	Bodies.SetLinearAndAngularVelocity(Id, ToJolt(Linear), ToJolt(Angular));
	return {};
}

std::expected<void, FPhysicsError> FPhysicsWorld::AddImpulse(const FPhysicsBodyId BodyId, const FVector3& Impulse, const FVector3& Point)
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	if (Id.IsInvalid() || Bodies.GetMotionType(Id) != JPH::EMotionType::Dynamic || !IsFinite(Impulse) || !IsFinite(Point))
	{
		return std::unexpected(FPhysicsError{"Only dynamic bodies take finite impulses"});
	}

	Bodies.AddImpulse(Id, ToJolt(Impulse), JPH::RVec3(ToJolt(Point)));
	return {};
}

void FPhysicsWorld::GetDebugLines(std::vector<FPhysicsDebugLine>& OutLines, const std::size_t MaxLines) const
{
	OutLines.clear();
	const JPH::BodyLockInterfaceNoLock& Locks = Implementation->Physics.GetBodyLockInterfaceNoLock();
	std::array<JPH::Float3, std::size_t{3} * JPH::Shape::cGetTrianglesMinTrianglesRequested> Triangles;
	JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> Leaves;
	for (const JPH::BodyID Id : Implementation->BodyIds)
	{
		if (Id.IsInvalid())
		{
			continue;
		}

		JPH::BodyLockRead Lock(Locks, Id);
		if (!Lock.Succeeded() || Lock.GetBody().GetBodyType() != JPH::EBodyType::RigidBody)
		{
			continue;
		}

		const JPH::Body& Body = Lock.GetBody();
		const EPhysicsMotionType MotionType = Body.IsStatic() ? EPhysicsMotionType::Static : Body.IsKinematic() ? EPhysicsMotionType::Kinematic
		                                                                                                        : EPhysicsMotionType::Dynamic;
		// Compound shapes cannot list triangles themselves, so walk their leaf shapes.
		Leaves.Reset();
		Body.GetTransformedShape().CollectTransformedShapes(JPH::AABox::sBiggest(), Leaves);
		for (const JPH::TransformedShape& Leaf : Leaves.mHits)
		{
			JPH::Shape::GetTrianglesContext Context;
			Leaf.GetTrianglesStart(Context, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
			for (int Count = Leaf.GetTrianglesNext(Context, JPH::Shape::cGetTrianglesMinTrianglesRequested, Triangles.data()); Count > 0; Count = Leaf.GetTrianglesNext(Context, JPH::Shape::cGetTrianglesMinTrianglesRequested, Triangles.data()))
			{
				for (int Triangle = 0; Triangle < Count; ++Triangle)
				{
					for (std::size_t Edge = 0; Edge < 3; ++Edge)
					{
						if (OutLines.size() >= MaxLines)
						{
							return;
						}

						const JPH::Float3& A = Triangles[(static_cast<std::size_t>(Triangle) * 3) + Edge];
						const JPH::Float3& B = Triangles[(static_cast<std::size_t>(Triangle) * 3) + ((Edge + 1) % 3)];
						OutLines.push_back({.Start = {A.x, A.y, A.z}, .End = {B.x, B.y, B.z}, .MotionType = MotionType, .bSensor = Body.IsSensor(), .bSleeping = !Body.IsActive()});
					}
				}
			}
		}
	}
}

std::expected<void, FPhysicsError> FPhysicsWorld::MoveKinematicBody(const FPhysicsBodyId BodyId, const FPhysicsBodyTransform& Target, const float DeltaSeconds)
{
	const JPH::BodyID Id = FindBody(Implementation->BodyIds, BodyId);
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	if (Id.IsInvalid() || Bodies.GetBodyType(Id) != JPH::EBodyType::RigidBody || Bodies.GetMotionType(Id) != JPH::EMotionType::Kinematic)
	{
		return std::unexpected(FPhysicsError{"Only kinematic rigid bodies can be moved"});
	}

	if (!IsFinite(Target.Position) || !IsFinite(Target.Rotation) || !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Kinematic targets must be finite with a positive step"});
	}

	Bodies.MoveKinematic(Id, ToJolt(Target.Position), ToJolt(Target.Rotation.NormalizedOrIdentity()), DeltaSeconds);
	return {};
}

std::span<const FPhysicsSensorEvent> FPhysicsWorld::GetSensorEvents() const
{
	return Implementation->SensorListener.Events;
}

std::expected<FPhysicsBodyId, FPhysicsError> FPhysicsWorld::CreateSoftBody(const FPhysicsSoftBodySettings& Settings)
{
	if (Implementation->BodyCount >= Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	const std::size_t VertexCount = Settings.Vertices.size();
	if (VertexCount < 2 || VertexCount > 65536 || !std::ranges::all_of(Settings.Vertices, [](const FVector3& Vertex)
	{
		return IsFinite(Vertex);
	}))
	{
		return std::unexpected(FPhysicsError{"Soft bodies need 2 to 65536 finite vertices"});
	}

	if (!AreValidIndices(Settings.StretchEdges, VertexCount) || !AreValidIndices(Settings.BendEdges, VertexCount) || !AreValidIndices(Settings.Faces, VertexCount) || (Settings.StretchEdges.empty() && Settings.Faces.empty()))
	{
		return std::unexpected(FPhysicsError{"Soft body edges and faces must reference distinct existing vertices, with at least one edge or face"});
	}

	std::vector<bool> Pinned(VertexCount);
	for (const std::uint32_t Vertex : Settings.PinnedVertices)
	{
		if (Vertex >= VertexCount)
		{
			return std::unexpected(FPhysicsError{"Pinned soft body vertex does not exist"});
		}

		Pinned[Vertex] = true;
	}

	const auto FreeCount = static_cast<std::size_t>(std::ranges::count(Pinned, false));
	if (FreeCount == 0)
	{
		return std::unexpected(FPhysicsError{"Soft bodies need at least one free vertex"});
	}

	if (!IsInRange(Settings.MassKg, 0.001f, 1000000.f) || !IsInRange(Settings.StretchCompliance, 0.f, 1000.f) || !IsInRange(Settings.BendCompliance, 0.f, 1000.f) || !IsInRange(Settings.VertexRadius, 0.f, 1.f) || !IsInRange(Settings.Pressure, 0.f, 1000000.f))
	{
		return std::unexpected(FPhysicsError{"Soft body mass, compliance, vertex radius, and pressure must be finite and within supported ranges"});
	}

	if (!IsInRange(Settings.Friction, 0.f, 1.f) || !IsInRange(Settings.Restitution, 0.f, 1.f) || !IsInRange(Settings.LinearDamping, 0.f, 1.f) || !IsInRange(Settings.GravityScale, 0.f, 10.f) || Settings.Iterations == 0 || Settings.Iterations > 64)
	{
		return std::unexpected(FPhysicsError{"Soft body friction, restitution, damping, gravity scale, or iteration count is out of range"});
	}

	// Jolt stores vertices relative to the body; centering them keeps single-precision positions small.
	FVector3 Origin;
	for (const FVector3& Vertex : Settings.Vertices)
	{
		Origin = Origin + Vertex;
	}

	Origin = Origin * (1.f / static_cast<float>(VertexCount));
	const JPH::Ref<JPH::SoftBodySharedSettings> Shared = new JPH::SoftBodySharedSettings;
	const float InverseMass = static_cast<float>(FreeCount) / Settings.MassKg;
	for (std::size_t Index = 0; Index < VertexCount; ++Index)
	{
		const FVector3 Local = Settings.Vertices[Index] - Origin;
		Shared->mVertices.emplace_back(JPH::Float3(Local.X, Local.Y, Local.Z), JPH::Float3(0.f, 0.f, 0.f), Pinned[Index] ? 0.f : InverseMass);
	}

	for (const std::array<std::uint32_t, 3>& Face : Settings.Faces)
	{
		Shared->AddFace(JPH::SoftBodySharedSettings::Face(Face[0], Face[1], Face[2]));
	}

	if (!Settings.Faces.empty())
	{
		const JPH::SoftBodySharedSettings::VertexAttributes Attributes(Settings.StretchCompliance, Settings.StretchCompliance, Settings.BendCompliance);
		Shared->CreateConstraints(&Attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
	}

	for (const std::array<std::uint32_t, 2>& Edge : Settings.StretchEdges)
	{
		Shared->mEdgeConstraints.emplace_back(Edge[0], Edge[1], Settings.StretchCompliance);
	}

	for (const std::array<std::uint32_t, 2>& Edge : Settings.BendEdges)
	{
		Shared->mEdgeConstraints.emplace_back(Edge[0], Edge[1], Settings.BendCompliance);
	}

	Shared->CalculateEdgeLengths();
	Shared->Optimize();
	JPH::SoftBodyCreationSettings Creation(Shared, ToJolt(Origin), JPH::Quat::sIdentity(), DynamicLayer);
	Creation.mNumIterations = Settings.Iterations;
	Creation.mLinearDamping = Settings.LinearDamping;
	Creation.mFriction = Settings.Friction;
	Creation.mRestitution = Settings.Restitution;
	Creation.mPressure = Settings.Pressure;
	Creation.mGravityFactor = Settings.GravityScale;
	Creation.mVertexRadius = Settings.VertexRadius;

	const JPH::BodyID Id = Implementation->Physics.GetBodyInterface().CreateAndAddSoftBody(Creation, JPH::EActivation::Activate);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Physics world body capacity reached"});
	}

	Implementation->BodyIds[Id.GetIndex()] = Id;
	++Implementation->BodyCount;
	return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
}

std::expected<FPhysicsRope, FPhysicsError> FPhysicsWorld::CreateRope(const FPhysicsRopeSettings& Settings)
{
	const FVector3 Span = Settings.End - Settings.Start;
	const float Length = Span.Length();
	if (!IsFinite(Settings.Start) || !IsFinite(Settings.End) || !IsInRange(Length, 0.01f, 1000.f) || Settings.Segments == 0 || Settings.Segments > 256)
	{
		return std::unexpected(FPhysicsError{"Ropes need finite ends 1 cm to 1000 m apart and 1 to 256 segments"});
	}

	if (!IsInRange(Settings.Radius, 0.001f, 1.f) || !IsInRange(Settings.MassKg, 0.001f, 1000000.f) || !IsInRange(Settings.Friction, 0.f, 1.f) || !IsInRange(Settings.AngularDamping, 0.f, 1.f))
	{
		return std::unexpected(FPhysicsError{"Rope radius, mass, friction, and damping must be finite and within supported ranges"});
	}

	if (Implementation->BodyCount + Settings.Segments > Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	const FVector3 Direction = Span * (1.f / Length);
	const float SegmentLength = Length / static_cast<float>(Settings.Segments);
	const JPH::Quat Rotation = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), ToJolt(Direction));
	const auto Joint = [&](const std::uint32_t Index)
	{
		return Settings.Start + Direction * (SegmentLength * static_cast<float>(Index));
	};

	FRopeState Rope{.HalfLength = SegmentLength * 0.5f};
	for (std::uint32_t Index = 0; Index < Settings.Segments; ++Index)
	{
		const auto Segment = CreateBody({
		    .Shape = EPhysicsShape::Capsule,
		    .Radius = Settings.Radius,
		    .HalfHeight = std::max(SegmentLength * 0.5f - Settings.Radius, 0.f),
		    .Position = (Joint(Index) + Joint(Index + 1)) * 0.5f,
		    .Rotation = {Rotation.GetX(), Rotation.GetY(), Rotation.GetZ(), Rotation.GetW()},
		    .MotionType = EPhysicsMotionType::Dynamic,
		    .Properties = {.MassKg = Settings.MassKg / static_cast<float>(Settings.Segments), .Friction = Settings.Friction, .AngularDamping = Settings.AngularDamping},
		});
		if (!Segment)
		{
			return std::unexpected(Segment.error());
		}

		Rope.Segments.push_back(FindBody(Implementation->BodyIds, *Segment));
	}

	// Joint 0 is the pin; the free end has none.
	for (std::uint32_t Index = Settings.bPinned ? 0 : 1; Index < Settings.Segments; ++Index)
	{
		JPH::PointConstraintSettings Point;
		Point.mPoint1 = Point.mPoint2 = ToJolt(Joint(Index));
		Implementation->AddJoint(Point, Index == 0 ? JPH::BodyID() : Rope.Segments[Index - 1], Rope.Segments[Index]);
	}

	if (Settings.bPinned)
	{
		// Long-range limits, as cloth solvers use, stop joint error from accumulating down the chain into visible stretch under load.
		for (std::uint32_t Index = 1; Index < Settings.Segments; ++Index)
		{
			JPH::DistanceConstraintSettings Limit;
			Limit.mPoint1 = ToJolt(Settings.Start);
			Limit.mPoint2 = ToJolt(Joint(Index + 1));
			Limit.mMinDistance = 0.f;
			Limit.mMaxDistance = SegmentLength * static_cast<float>(Index + 1);
			Implementation->AddJoint(Limit, JPH::BodyID(), Rope.Segments[Index]);
		}
	}

	const FPhysicsRope Result{.Index = static_cast<std::uint32_t>(Implementation->Ropes.size()), .First = FPhysicsBodyId{Rope.Segments.front().GetIndexAndSequenceNumber()}, .Last = FPhysicsBodyId{Rope.Segments.back().GetIndexAndSequenceNumber()}};
	Implementation->Ropes.push_back(std::move(Rope));
	return Result;
}

std::expected<FPhysicsRagdoll, FPhysicsError> FPhysicsWorld::CreateRagdoll(const FPhysicsRagdollSettings& Settings)
{
	const std::span<const FPhysicsRagdollPart> Parts = Settings.Parts;
	if (Parts.empty() || Parts.size() > MaximumRagdollParts || !IsFinite(Settings.LinearVelocity))
	{
		return std::unexpected(FPhysicsError{std::format("Ragdolls need 1 to {} parts and a finite velocity", MaximumRagdollParts)});
	}

	for (std::size_t Index = 0; Index < Parts.size(); ++Index)
	{
		const FPhysicsRagdollPart& Part = Parts[Index];
		const bool bParent = Part.Parent ? *Part.Parent < Index : Index == 0;
		const bool bJoint = !Part.Parent || (IsFinite(Part.Joint) && IsFinite(Part.TwistAxis) && Part.TwistAxis.LengthSquared() > 1.e-6f && IsInRange(Part.SwingRadians, 0.f, 3.1416f) && IsInRange(Part.TwistRadians, 0.f, 3.1416f));
		if (!bParent || !bJoint || !IsFinite(Part.Transform.Position) || !IsFinite(Part.Transform.Rotation) || !IsInRange(Part.Radius, 0.001f, 10.f) || !IsInRange(Part.HalfHeight, 0.f, 10.f) || !IsInRange(Part.MassKg, 0.001f, 100000.f))
		{
			return std::unexpected(FPhysicsError{"Ragdoll parts need finite capsules and masses, joints within half a turn, and only the first part without a parent, which must come earlier"});
		}
	}

	if (Implementation->BodyCount + Parts.size() > Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	FImplementation::FRagdollState Ragdoll;
	FPhysicsRagdoll Result{.Index = static_cast<std::uint32_t>(Implementation->Ragdolls.size())};
	for (const FPhysicsRagdollPart& Part : Parts)
	{
		const auto Body = CreateBody({
		    .Shape = EPhysicsShape::Capsule,
		    .Radius = Part.Radius,
		    .HalfHeight = Part.HalfHeight,
		    .Position = Part.Transform.Position,
		    .Rotation = Part.Transform.Rotation.NormalizedOrIdentity(),
		    .MotionType = EPhysicsMotionType::Dynamic,
		    .Properties = {.MassKg = Part.MassKg, .Friction = 0.6f, .LinearDamping = 0.05f, .AngularDamping = 0.3f},
		    .Layer = PhysicsLayerRagdoll,
		});
		if (!Body)
		{
			return std::unexpected(Body.error());
		}

		Result.Parts.push_back(*Body);
		Ragdoll.Parts.push_back(FindBody(Implementation->BodyIds, *Body));
		if (const auto Moved = SetVelocity(*Body, Settings.LinearVelocity); !Moved)
		{
			return std::unexpected(Moved.error());
		}
	}

	for (std::size_t Index = 1; Index < Parts.size(); ++Index)
	{
		const FPhysicsRagdollPart& Part = Parts[Index];
		const JPH::Vec3 Twist = ToJolt(Part.TwistAxis).Normalized();
		JPH::SwingTwistConstraintSettings Joint;
		Joint.mPosition1 = Joint.mPosition2 = JPH::RVec3(ToJolt(Part.Joint));
		Joint.mTwistAxis1 = Joint.mTwistAxis2 = Twist;
		Joint.mPlaneAxis1 = Joint.mPlaneAxis2 = Twist.GetNormalizedPerpendicular();
		Joint.mNormalHalfConeAngle = Joint.mPlaneHalfConeAngle = Part.SwingRadians;
		Joint.mTwistMinAngle = -Part.TwistRadians;
		Joint.mTwistMaxAngle = Part.TwistRadians;
		Implementation->AddJoint(Joint, Ragdoll.Parts[*Part.Parent], Ragdoll.Parts[Index]);
		Ragdoll.Joints.push_back(Implementation->Constraints.back());
	}

	Implementation->Ragdolls.push_back(std::move(Ragdoll));
	return Result;
}

std::expected<void, FPhysicsError> FPhysicsWorld::DestroyRagdoll(const FPhysicsRagdoll& Ragdoll)
{
	if (Ragdoll.Index >= Implementation->Ragdolls.size() || Implementation->Ragdolls[Ragdoll.Index].Parts.empty())
	{
		return std::unexpected(FPhysicsError{"Unknown ragdoll"});
	}

	FImplementation::FRagdollState State = std::exchange(Implementation->Ragdolls[Ragdoll.Index], {});
	for (const JPH::Ref<JPH::Constraint>& Joint : State.Joints)
	{
		Implementation->Physics.RemoveConstraint(Joint);
		std::erase(Implementation->Constraints, Joint);
	}

	// A later body may reuse a part's index, so its joined pairs go with it.
	for (const JPH::BodyID Part : State.Parts)
	{
		Implementation->ConstrainedBodies.erase(Part.GetIndexAndSequenceNumber());
		std::erase_if(Implementation->JoinedBodies->JoinedPairs, [Part](const std::uint64_t Pair)
		{
			return static_cast<std::uint32_t>(Pair >> 32) == Part.GetIndex() || static_cast<std::uint32_t>(Pair) == Part.GetIndex();
		});
	}

	for (const JPH::BodyID Part : State.Parts)
	{
		if (const auto Destroyed = DestroyBody(FPhysicsBodyId{Part.GetIndexAndSequenceNumber()}); !Destroyed)
		{
			return Destroyed;
		}
	}

	return {};
}

std::expected<void, FPhysicsError> FPhysicsWorld::CreatePointConstraint(const FPhysicsPointConstraintSettings& Settings)
{
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	const JPH::BodyID Body = FindBody(Implementation->BodyIds, Settings.Body);
	const JPH::BodyID Other = Settings.Other ? FindBody(Implementation->BodyIds, *Settings.Other) : JPH::BodyID();
	if (Body.IsInvalid() || Bodies.GetBodyType(Body) != JPH::EBodyType::RigidBody || Bodies.GetMotionType(Body) != JPH::EMotionType::Dynamic)
	{
		return std::unexpected(FPhysicsError{"Point constraints need a dynamic rigid body"});
	}

	if (Settings.Other && (Other.IsInvalid() || Other == Body || Bodies.GetBodyType(Other) != JPH::EBodyType::RigidBody))
	{
		return std::unexpected(FPhysicsError{"Point constraints join a body to the world or to a different rigid body"});
	}

	if (!IsFinite(Settings.Point))
	{
		return std::unexpected(FPhysicsError{"Point constraint position must be finite"});
	}

	JPH::PointConstraintSettings Point;
	Point.mPoint1 = Point.mPoint2 = ToJolt(Settings.Point);
	Implementation->AddJoint(Point, Other, Body);
	return {};
}

std::expected<void, FPhysicsError> FPhysicsWorld::Step(const float FixedDeltaSeconds)
{
	if (!std::isfinite(FixedDeltaSeconds) || FixedDeltaSeconds <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Physics step must be finite and positive"});
	}

	if (!Implementation->StepFailure.empty())
	{
		return std::unexpected(FPhysicsError{Implementation->StepFailure});
	}

	Implementation->SensorListener.Events.clear();
	Implementation->SensorListener.Contacts.clear();
	JPH::JobSystem& Jobs = Implementation->TaskJobs ? static_cast<JPH::JobSystem&>(*Implementation->TaskJobs) : *Implementation->SingleThreaded;
	const JPH::EPhysicsUpdateError Error = Implementation->Physics.Update(FixedDeltaSeconds, 1, &Implementation->TempAllocator, &Jobs);
	if (Implementation->TaskJobs)
	{
		Implementation->TaskJobs->DrainSubmitted();
	}

	std::ranges::sort(Implementation->SensorListener.Events, [](const FPhysicsSensorEvent& A, const FPhysicsSensorEvent& B)
	{
		return std::tuple(A.Sensor.Value, A.Body.Value, A.bEntered) < std::tuple(B.Sensor.Value, B.Body.Value, B.bEntered);
	});

	// Jolt reports each touching pair of sub-shapes, so the strongest report stands for its body pair; the tie-break on every field keeps the order
	// independent of which job thread reported first.
	std::vector<FPhysicsContactEvent>& Contacts = Implementation->SensorListener.Contacts;
	std::ranges::sort(Contacts, [](const FPhysicsContactEvent& A, const FPhysicsContactEvent& B)
	{
		return std::tuple(A.First.Value, A.Second.Value, A.Phase, -A.Impulse, -A.ApproachSpeed, A.Point.X, A.Point.Y, A.Point.Z) < std::tuple(B.First.Value, B.Second.Value, B.Phase, -B.Impulse, -B.ApproachSpeed, B.Point.X, B.Point.Y, B.Point.Z);
	});

	const auto Duplicates = std::ranges::unique(Contacts, [](const FPhysicsContactEvent& A, const FPhysicsContactEvent& B)
	{
		return A.First == B.First && A.Second == B.Second && A.Phase == B.Phase;
	});

	Contacts.erase(Duplicates.begin(), Duplicates.end());

	if (Error != JPH::EPhysicsUpdateError::None)
	{
		if ((Error & JPH::EPhysicsUpdateError::BodyPairCacheFull) != JPH::EPhysicsUpdateError::None)
		{
			Implementation->StepFailure = std::format("Physics body-pair capacity exceeded (MaxBodyPairs={}); ", Implementation->Settings.MaxBodyPairs);
		}

		if ((Error & (JPH::EPhysicsUpdateError::ManifoldCacheFull | JPH::EPhysicsUpdateError::ContactConstraintsFull)) != JPH::EPhysicsUpdateError::None)
		{
			Implementation->StepFailure += std::format("Physics contact capacity exceeded (MaxContactConstraints={}); ", Implementation->Settings.MaxContactConstraints);
		}

		Implementation->StepFailure += "recreate the world with larger capacities";
		return std::unexpected(FPhysicsError{Implementation->StepFailure});
	}

	return {};
}

std::span<const FPhysicsContactEvent> FPhysicsWorld::GetContactEvents() const
{
	return Implementation->SensorListener.Contacts;
}

const FWorldPosition& FPhysicsWorld::GetOrigin() const noexcept
{
	return Implementation->Origin;
}

namespace
{
class FSolidBodyFilter final : public JPH::BodyFilter
{
public:
	bool ShouldCollideLocked(const JPH::Body& Body) const override
	{
		return !Body.IsSensor();
	}
};
}

float FPhysicsWorld::CastSphere(const FWorldPosition& Start, const FWorldPosition& End, const float Radius) const
{
	const std::optional<FPhysicsCastHit> Hit = CastSphereHit(Start, End, Radius);
	return Hit ? Hit->Fraction : 1.f;
}

std::optional<FPhysicsCastHit> FPhysicsWorld::CastSphereHit(const FWorldPosition& Start, const FWorldPosition& End, const float Radius) const
{
	if (!std::isfinite(Radius) || Radius <= 0.f)
	{
		return std::nullopt;
	}

	const FVector3 From = ToPhysics(Start);
	const FVector3 Direction = ToPhysics(End) - From;
	if (!IsFinite(From) || !IsFinite(Direction) || Direction.LengthSquared() < 1.e-12f)
	{
		return std::nullopt;
	}

	JPH::SphereShape Sphere(Radius);
	// A stack shape must not be released by the reference count Jolt takes on it.
	Sphere.SetEmbedded();
	const JPH::RShapeCast Cast(&Sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(ToJolt(From))), ToJolt(Direction));
	JPH::ShapeCastSettings Settings;
	Settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
	JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> Collector;
	const FSolidBodyFilter BodyFilter;
	const JPH::PhysicsSystem& Physics = Implementation->Physics;
	Physics.GetNarrowPhaseQuery().CastShape(Cast, Settings, JPH::RVec3::sZero(), Collector, Physics.GetDefaultBroadPhaseLayerFilter(CharacterLayer), Physics.GetDefaultLayerFilter(CharacterLayer), BodyFilter);
	if (!Collector.HadHit())
	{
		return std::nullopt;
	}

	// The penetration axis points from the sphere into what it hit, so the surface faces the other way.
	const JPH::Vec3 Axis = Collector.mHit.mPenetrationAxis;
	const FVector3 Normal = Axis.LengthSq() > 1.e-12f ? -ToHerta(Axis.Normalized()) : -Direction.Normalized();
	return FPhysicsCastHit{.Fraction = std::clamp(Collector.mHit.mFraction, 0.f, 1.f), .Normal = Normal, .Body = FPhysicsBodyId{Collector.mHit.mBodyID2.GetIndexAndSequenceNumber()}};
}

FVector3 FPhysicsWorld::ToPhysics(const FWorldPosition& Position) const
{
	return WorldToOriginRelative(Position, Implementation->Origin);
}

FWorldPosition FPhysicsWorld::ToWorld(const FVector3& Position) const
{
	return OriginRelativeToWorld(Position, Implementation->Origin);
}

std::expected<void, FPhysicsError> FPhysicsWorld::ShiftOrigin(const FWorldPosition& NewOrigin)
{
	const FVector3d Delta = Implementation->Origin.Meters - NewOrigin.Meters;
	if (!std::isfinite(Delta.X) || !std::isfinite(Delta.Y) || !std::isfinite(Delta.Z))
	{
		return std::unexpected(FPhysicsError{"The physics origin must be finite"});
	}

	const JPH::Vec3 Offset(static_cast<float>(Delta.X), static_cast<float>(Delta.Y), static_cast<float>(Delta.Z));
	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	for (const JPH::BodyID Id : Implementation->BodyIds)
	{
		if (!Id.IsInvalid())
		{
			Bodies.SetPosition(Id, Bodies.GetPosition(Id) + Offset, JPH::EActivation::DontActivate);
		}
	}

	for (const JPH::Ref<JPH::CharacterVirtual>& Character : Implementation->Characters)
	{
		if (Character)
		{
			Character->SetPosition(Character->GetPosition() + Offset);
		}
	}

	// Every body moved at once, so the broad phase rebuilds its trees rather than patching them body by body during the next step.
	Implementation->Physics.OptimizeBroadPhase();
	Implementation->Origin = NewOrigin;
	return {};
}

namespace
{
// Jolt trusts its state stream: a truncated or foreign one reads past its end and corrupts the contact cache. Herta frames the stream so damaged data never reaches Jolt.
struct FStateHeader
{
	std::array<char, 4> Magic{'H', 'P', 'S', '1'};
	std::uint32_t BodyCount = 0;
	// Which bodies, constraints, and characters exist; a state restores only into a world holding the same ones.
	std::uint64_t Structure = 0;
	std::uint64_t PayloadBytes = 0;
	std::uint64_t PayloadHashLow = 0;
	std::uint64_t PayloadHashHigh = 0;
};

static_assert(std::is_trivially_copyable_v<FStateHeader> && sizeof(FStateHeader) == 40);

[[nodiscard]] std::uint64_t HashStructure(const JPH::PhysicsSystem& Physics, const std::span<const JPH::Ref<JPH::CharacterVirtual>> Characters)
{
	JPH::BodyIDVector Bodies;
	Physics.GetBodies(Bodies);
	std::vector<std::uint32_t> Words;
	Words.reserve(Bodies.size() + Characters.size() + 1);
	for (const JPH::BodyID Body : Bodies)
	{
		Words.push_back(Body.GetIndexAndSequenceNumber());
	}

	std::ranges::sort(Words);
	Words.push_back(static_cast<std::uint32_t>(Physics.GetConstraints().size()));
	for (const JPH::Ref<JPH::CharacterVirtual>& Character : Characters)
	{
		Words.push_back(Character ? 1u : 0u);
	}

	const FHash128 Hash = HashBytes(std::as_bytes(std::span(Words)));
	return Hash.Low ^ Hash.High;
}

// Jolt leaves virtual characters out of the system's state, so they follow it, each behind a flag for whether its slot is still in use. A
// world without characters writes nothing more, which keeps the hashes of character-free worlds as they were.
std::string SaveJoltState(const JPH::PhysicsSystem& Physics, const std::span<const JPH::Ref<JPH::CharacterVirtual>> Characters)
{
	JPH::StateRecorderImpl Recorder;
	Physics.SaveState(Recorder);
	for (const JPH::Ref<JPH::CharacterVirtual>& Character : Characters)
	{
		const bool bAlive = Character != nullptr;
		Recorder.Write(bAlive);
		if (bAlive)
		{
			Character->SaveState(Recorder);
		}
	}

	return Recorder.GetData();
}
}

std::vector<std::byte> FPhysicsWorld::SaveState() const
{
	const std::string Data = SaveJoltState(Implementation->Physics, Implementation->Characters);
	const auto Payload = std::as_bytes(std::span(Data));
	const FHash128 Hash = HashBytes(Payload);
	const FStateHeader Header{.BodyCount = Implementation->Physics.GetNumBodies(), .Structure = HashStructure(Implementation->Physics, Implementation->Characters), .PayloadBytes = Payload.size(), .PayloadHashLow = Hash.Low, .PayloadHashHigh = Hash.High};
	std::vector<std::byte> State(sizeof(Header) + Payload.size());
	std::memcpy(State.data(), &Header, sizeof(Header));
	std::ranges::copy(Payload, State.begin() + sizeof(Header));
	return State;
}

std::expected<void, FPhysicsError> FPhysicsWorld::RestoreState(const std::span<const std::byte> State)
{
	FStateHeader Header;
	if (State.size() < sizeof(Header))
	{
		return std::unexpected(FPhysicsError{"The physics state is truncated"});
	}

	std::memcpy(&Header, State.data(), sizeof(Header));
	const std::span<const std::byte> Payload = State.subspan(sizeof(Header));
	const FHash128 Hash = HashBytes(Payload);
	if (Header.Magic != FStateHeader{}.Magic || Header.PayloadBytes != Payload.size() || Header.PayloadHashLow != Hash.Low || Header.PayloadHashHigh != Hash.High)
	{
		return std::unexpected(FPhysicsError{"The physics state is truncated or damaged"});
	}

	if (Header.BodyCount != Implementation->Physics.GetNumBodies())
	{
		return std::unexpected(FPhysicsError{std::format("The physics state holds {} bodies but this world has {}", Header.BodyCount, Implementation->Physics.GetNumBodies())});
	}

	if (Header.Structure != HashStructure(Implementation->Physics, Implementation->Characters))
	{
		return std::unexpected(FPhysicsError{"The physics state was saved with other bodies, constraints, or characters than this world has"});
	}

	JPH::StateRecorderImpl Recorder;
	Recorder.WriteBytes(Payload.data(), Payload.size());
	if (!Implementation->Physics.RestoreState(Recorder) || Recorder.IsFailed())
	{
		return std::unexpected(FPhysicsError{"The physics state does not match this world's bodies and constraints"});
	}

	for (const JPH::Ref<JPH::CharacterVirtual>& Character : Implementation->Characters)
	{
		bool bAlive = false;
		Recorder.Read(bAlive);
		if (Recorder.IsFailed() || bAlive != (Character != nullptr))
		{
			return std::unexpected(FPhysicsError{"The physics state does not match this world's characters"});
		}

		if (Character)
		{
			Character->RestoreState(Recorder);
		}
	}

	// A stringstream reports its end only after reading past it, so the payload hash checked above stands in for an end check.
	if (Recorder.IsFailed())
	{
		return std::unexpected(FPhysicsError{"The physics state does not match this world's characters"});
	}

	// A restored state carries no events of its own.
	Implementation->SensorListener.Events.clear();
	Implementation->SensorListener.Contacts.clear();
	return {};
}

std::uint64_t FPhysicsWorld::ComputeStateHash() const
{
	// Hashes Jolt's stream alone, so the value matches across platforms whatever framing SaveState adds.
	const std::string Data = SaveJoltState(Implementation->Physics, Implementation->Characters);
	const FHash128 Hash = HashBytes(std::as_bytes(std::span(Data)));
	return Hash.Low ^ Hash.High;
}

std::expected<FPhysicsBodyTransform, FPhysicsError> FPhysicsWorld::GetBodyTransform(const FPhysicsBodyId BodyId) const
{
	if ((BodyId.Value & JPH::BodyID::cBroadPhaseBit) != 0)
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	const JPH::BodyID Id(BodyId.Value);
	if (Id.IsInvalid() || Id.GetIndex() >= Implementation->BodyIds.size() || Implementation->BodyIds[Id.GetIndex()] != Id)
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	JPH::RVec3 Position;
	JPH::Quat Rotation;
	Implementation->Physics.GetBodyInterface().GetPositionAndRotation(Id, Position, Rotation);
	return FPhysicsBodyTransform{.Position = {Position.GetX(), Position.GetY(), Position.GetZ()},
	    .Rotation = {Rotation.GetX(), Rotation.GetY(), Rotation.GetZ(), Rotation.GetW()}};
}

std::expected<void, FPhysicsError> FPhysicsWorld::GetSoftBodyVertices(const FPhysicsBodyId BodyId, std::vector<FVector3>& OutPositions) const
{
	const JPH::BodyID Id(BodyId.Value);
	if ((BodyId.Value & JPH::BodyID::cBroadPhaseBit) != 0 || Id.IsInvalid() || Id.GetIndex() >= Implementation->BodyIds.size() || Implementation->BodyIds[Id.GetIndex()] != Id)
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	const JPH::BodyLockRead Lock(Implementation->Physics.GetBodyLockInterface(), Id);
	if (!Lock.Succeeded() || !Lock.GetBody().IsSoftBody())
	{
		return std::unexpected(FPhysicsError{"Physics body is not a soft body"});
	}

	const JPH::Body& Body = Lock.GetBody();
	const auto& Motion = static_cast<const JPH::SoftBodyMotionProperties&>(*Body.GetMotionProperties());
	const JPH::RMat44 Transform = Body.GetCenterOfMassTransform();
	OutPositions.resize(Motion.GetVertices().size());
	for (std::size_t Index = 0; Index < OutPositions.size(); ++Index)
	{
		const JPH::RVec3 Position = Transform * Motion.GetVertex(static_cast<JPH::uint>(Index)).mPosition;
		OutPositions[Index] = {Position.GetX(), Position.GetY(), Position.GetZ()};
	}

	return {};
}

std::expected<void, FPhysicsError> FPhysicsWorld::GetRopePoints(const FPhysicsRope& Rope, std::vector<FVector3>& OutPoints) const
{
	if (Rope.Index >= Implementation->Ropes.size())
	{
		return std::unexpected(FPhysicsError{"Unknown rope"});
	}

	const FRopeState& State = Implementation->Ropes[Rope.Index];
	const JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	OutPoints.resize(State.Segments.size() + 1);
	for (std::size_t Index = 0; Index < State.Segments.size(); ++Index)
	{
		JPH::RVec3 Center;
		JPH::Quat Rotation;
		Bodies.GetPositionAndRotation(State.Segments[Index], Center, Rotation);
		const JPH::Vec3 HalfAxis = Rotation.RotateAxisY() * State.HalfLength;
		const JPH::RVec3 End = Center + HalfAxis;
		if (Index == 0)
		{
			const JPH::RVec3 Start = Center - HalfAxis;
			OutPoints[0] = {Start.GetX(), Start.GetY(), Start.GetZ()};
		}

		OutPoints[Index + 1] = {End.GetX(), End.GetY(), End.GetZ()};
	}

	return {};
}

std::expected<FPhysicsCharacterId, FPhysicsError> FPhysicsWorld::CreateCharacter(const FPhysicsCharacterSettings& Settings)
{
	if (Implementation->BodyCount >= Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	if (!IsFinite(Settings.FootPosition) || !IsInRange(Settings.Radius, 0.05f, 10.f) || !IsInRange(Settings.Height, Settings.Radius * 2.f, 20.f) || !IsInRange(Settings.MassKg, 1.f, 10000.f)
	    || !IsInRange(Settings.MaxSlopeRadians, 0.f, 1.5f) || !IsInRange(Settings.StepHeight, 0.f, Settings.Height) || !IsInRange(Settings.PushForce, 0.f, 1000000.f))
	{
		return std::unexpected(FPhysicsError{"Character dimensions, mass, slope, step height, and push force must be finite and in range"});
	}

	const JPH::ShapeSettings::ShapeResult Shape = MakeCharacterShape(Settings.Height, Settings.Radius);
	if (Shape.HasError())
	{
		return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
	}

	const JPH::Ref<JPH::CharacterVirtualSettings> CharacterSettings = new JPH::CharacterVirtualSettings;
	CharacterSettings->mShape = Shape.Get();
	CharacterSettings->mMaxSlopeAngle = Settings.MaxSlopeRadians;
	CharacterSettings->mMass = Settings.MassKg;
	CharacterSettings->mMaxStrength = Settings.PushForce;
	// Only the bottom hemisphere supports the character; contacts above it are walls.
	CharacterSettings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -Settings.Radius);
	// The inner body lets dynamic bodies collide with the character, which the virtual shape alone cannot.
	CharacterSettings->mInnerBodyShape = Shape.Get();
	CharacterSettings->mInnerBodyLayer = CharacterLayer;
	Implementation->Characters.emplace_back(new JPH::CharacterVirtual(CharacterSettings, ToJolt(Settings.FootPosition), JPH::Quat::sIdentity(), 0, &Implementation->Physics));
	Implementation->CharacterShapes.push_back({.Radius = Settings.Radius, .StepHeight = Settings.StepHeight});
	++Implementation->BodyCount;
	return FPhysicsCharacterId{static_cast<std::uint32_t>(Implementation->Characters.size())};
}

std::expected<void, FPhysicsError> FPhysicsWorld::DestroyCharacter(const FPhysicsCharacterId CharacterId)
{
	if (!Implementation->FindCharacter(CharacterId))
	{
		return std::unexpected(FPhysicsError{"Unknown character"});
	}

	// The slot stays empty, so later characters keep their ids; releasing the character removes its inner body.
	Implementation->Characters[CharacterId.Value - 1] = nullptr;
	--Implementation->BodyCount;
	return {};
}

std::expected<FPhysicsCharacterState, FPhysicsError> FPhysicsWorld::UpdateCharacterGround(const FPhysicsCharacterId CharacterId)
{
	JPH::CharacterVirtual* const Character = Implementation->FindCharacter(CharacterId);
	if (!Character)
	{
		return std::unexpected(FPhysicsError{"Unknown character"});
	}

	Character->UpdateGroundVelocity();
	return Implementation->GetCharacterState(*Character);
}

std::expected<FPhysicsCharacterState, FPhysicsError> FPhysicsWorld::MoveCharacter(const FPhysicsCharacterId CharacterId, const FVector3& Velocity, const float DeltaSeconds)
{
	JPH::CharacterVirtual* const Character = Implementation->FindCharacter(CharacterId);
	if (!Character)
	{
		return std::unexpected(FPhysicsError{"Unknown character"});
	}

	if (!IsFinite(Velocity) || !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Character velocity must be finite with a positive step"});
	}

	JPH::PhysicsSystem& Physics = Implementation->Physics;
	JPH::CharacterVirtual::ExtendedUpdateSettings Update;
	Update.mWalkStairsStepUp = JPH::Vec3(0.f, Implementation->CharacterShapes[CharacterId.Value - 1].StepHeight, 0.f);
	Character->SetLinearVelocity(ToJolt(Velocity));
	const FCharacterBodyFilter BodyFilter;
	Character->ExtendedUpdate(DeltaSeconds, Physics.GetGravity(), Update, Physics.GetDefaultBroadPhaseLayerFilter(CharacterLayer), Physics.GetDefaultLayerFilter(CharacterLayer), BodyFilter, {}, Implementation->TempAllocator);
	return Implementation->GetCharacterState(*Character);
}

std::expected<void, FPhysicsError> FPhysicsWorld::TeleportCharacter(const FPhysicsCharacterId CharacterId, const FVector3& FootPosition)
{
	JPH::CharacterVirtual* const Character = Implementation->FindCharacter(CharacterId);
	if (!Character || !IsFinite(FootPosition))
	{
		return std::unexpected(FPhysicsError{"Teleporting requires a known character and a finite position"});
	}

	Character->SetPosition(ToJolt(FootPosition));
	Character->SetLinearVelocity(JPH::Vec3::sZero());
	return {};
}

std::expected<bool, FPhysicsError> FPhysicsWorld::SetCharacterHeight(const FPhysicsCharacterId CharacterId, const float Height)
{
	JPH::CharacterVirtual* const Character = Implementation->FindCharacter(CharacterId);
	if (!Character)
	{
		return std::unexpected(FPhysicsError{"Unknown character"});
	}

	const float Radius = Implementation->CharacterShapes[CharacterId.Value - 1].Radius;
	if (!IsInRange(Height, Radius * 2.f, 20.f))
	{
		return std::unexpected(FPhysicsError{"Character height must be finite and at least the capsule diameter"});
	}

	const JPH::ShapeSettings::ShapeResult Shape = MakeCharacterShape(Height, Radius);
	if (Shape.HasError())
	{
		return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
	}

	// SetShape refuses a size that would sink deeper than the character padding into the world.
	JPH::PhysicsSystem& Physics = Implementation->Physics;
	const FCharacterBodyFilter BodyFilter;
	if (!Character->SetShape(Shape.Get(), 0.02f, Physics.GetDefaultBroadPhaseLayerFilter(CharacterLayer), Physics.GetDefaultLayerFilter(CharacterLayer), BodyFilter, {}, Implementation->TempAllocator))
	{
		return false;
	}

	Character->SetInnerBodyShape(Shape.Get());
	return true;
}
}
