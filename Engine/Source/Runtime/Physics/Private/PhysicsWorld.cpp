#include "Herta/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer DynamicLayer = 1;

[[nodiscard]] constexpr std::size_t RequiredTempMemory(const FPhysicsWorldSettings& Settings)
{
	// Jolt 5.6: one discrete step, at most three constraints per body (a rope joint, its length limit, and an attached load), and no parallel island splitting.
	// Cover contact buffers, island and constraint arrays, CCD-index mapping, soft-body update contexts, and alignment padding before Jolt can abort.
	return sizeof(JPH::PhysicsUpdateContext::Step) + 320 * 1024
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

// The single-threaded job system calls this on the stepping thread, so it needs no locking.
class FSensorListener final : public JPH::ContactListener
{
public:
	void OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold& Manifold, JPH::ContactSettings& Settings) override;
	void OnContactRemoved(const JPH::SubShapeIDPair& Pair) override;

	std::unordered_set<std::uint32_t> Sensors;
	std::vector<FPhysicsSensorEvent> Events;
};

void FSensorListener::OnContactAdded(const JPH::Body& Body1, const JPH::Body& Body2, const JPH::ContactManifold&, JPH::ContactSettings&)
{
	if (Body1.IsSensor() != Body2.IsSensor())
	{
		const JPH::Body& Sensor = Body1.IsSensor() ? Body1 : Body2;
		const JPH::Body& Other = Body1.IsSensor() ? Body2 : Body1;
		Events.push_back({.Sensor = FPhysicsBodyId{Sensor.GetID().GetIndexAndSequenceNumber()}, .Body = FPhysicsBodyId{Other.GetID().GetIndexAndSequenceNumber()}, .bEntered = true});
	}
}

void FSensorListener::OnContactRemoved(const JPH::SubShapeIDPair& Pair)
{
	// Removal only names the bodies, which may already be gone, so sensors are remembered by ID.
	const std::uint32_t Body1 = Pair.GetBody1ID().GetIndexAndSequenceNumber();
	const std::uint32_t Body2 = Pair.GetBody2ID().GetIndexAndSequenceNumber();
	const bool bSensor1 = Sensors.contains(Body1);
	if (bSensor1 != Sensors.contains(Body2))
	{
		Events.push_back({.Sensor = FPhysicsBodyId{bSensor1 ? Body1 : Body2}, .Body = FPhysicsBodyId{bSensor1 ? Body2 : Body1}, .bEntered = false});
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
	JPH::BroadPhaseLayerInterfaceTable BroadPhaseLayers{2, 2};
	JPH::ObjectLayerPairFilterTable CollisionLayers{2};
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> BroadPhaseFilter;
	// Declared before Physics so it outlives the system that holds a pointer to it.
	FSensorListener SensorListener;
	JPH::PhysicsSystem Physics;
	JPH::TempAllocatorImpl TempAllocator;
	JPH::JobSystemSingleThreaded JobSystem{2048};
	std::vector<JPH::BodyID> BodyIds;
	std::vector<JPH::Ref<JPH::Constraint>> Constraints;
	std::vector<FRopeState> Ropes;
	// Each character owns an inner body in Physics, so characters are released before the bodies.
	std::vector<JPH::Ref<JPH::CharacterVirtual>> Characters;
	std::vector<float> CharacterStepHeights;
	JPH::Ref<FJoinedBodiesFilter> JoinedBodies = new FJoinedBodiesFilter;
	const FPhysicsWorldSettings Settings;
	std::string StepFailure;
	std::size_t BodyCount = 0;

	explicit FImplementation(const FPhysicsWorldSettings& InSettings)
	    : TempAllocator(InSettings.TempMemoryBytes)
	    , Settings(InSettings)
	{
		BroadPhaseLayers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
		BroadPhaseLayers.MapObjectToBroadPhaseLayer(DynamicLayer, JPH::BroadPhaseLayer(1));
		CollisionLayers.EnableCollision(StaticLayer, DynamicLayer);
		CollisionLayers.EnableCollision(DynamicLayer, DynamicLayer);
		BroadPhaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(BroadPhaseLayers, 2, CollisionLayers, 2);
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
};

JPH::CharacterVirtual* FPhysicsWorld::FImplementation::FindCharacter(const FPhysicsCharacterId Character) const
{
	return Character.Value > 0 && Character.Value <= Characters.size() ? Characters[Character.Value - 1].GetPtr() : nullptr;
}

namespace
{
[[nodiscard]] FPhysicsCharacterState GetCharacterState(const JPH::CharacterVirtual& Character)
{
	const JPH::RVec3 Position = Character.GetPosition();
	const JPH::Vec3 Velocity = Character.GetLinearVelocity();
	const JPH::Vec3 Ground = Character.GetGroundVelocity();
	return {
	    .FootPosition = {Position.GetX(), Position.GetY(), Position.GetZ()},
	    .Velocity = {Velocity.GetX(), Velocity.GetY(), Velocity.GetZ()},
	    .GroundVelocity = {Ground.GetX(), Ground.GetY(), Ground.GetZ()},
	    .bGrounded = Character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround,
	};
}
}

void FPhysicsWorld::FImplementation::AddJoint(const JPH::TwoBodyConstraintSettings& Joint, const JPH::BodyID Body1, const JPH::BodyID Body2)
{
	JPH::BodyInterface& Bodies = Physics.GetBodyInterface();
	const JPH::Ref<JPH::Constraint> Constraint = Bodies.CreateConstraint(&Joint, Body1, Body2);
	Constraint->SetNumVelocityStepsOverride(JointVelocitySteps);
	Constraint->SetNumPositionStepsOverride(JointPositionSteps);
	Physics.AddConstraint(Constraint);
	Constraints.push_back(Constraint);
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

	if (!AcquireRuntime())
	{
		return std::unexpected(FPhysicsError{"Jolt is already registered outside Herta Physics"});
	}

	return std::unique_ptr<FPhysicsWorld>(new FPhysicsWorld(std::make_unique<FImplementation>(Settings)));
}

std::expected<FPhysicsBodyId, FPhysicsError> FPhysicsWorld::CreateBody(const FPhysicsBodySettings& Settings)
{
	if (Implementation->BodyCount >= Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	if (Settings.Shape != EPhysicsShape::Box && Settings.Shape != EPhysicsShape::Sphere && Settings.Shape != EPhysicsShape::Capsule)
	{
		return std::unexpected(FPhysicsError{"Unsupported body shape"});
	}

	if (Settings.Shape == EPhysicsShape::Box && (!IsFinite(Settings.HalfExtents) || Settings.HalfExtents.X <= 0.f || Settings.HalfExtents.Y <= 0.f || Settings.HalfExtents.Z <= 0.f))
	{
		return std::unexpected(FPhysicsError{"Box half extents must be finite and positive"});
	}

	if (Settings.Shape != EPhysicsShape::Box && (!IsInRange(Settings.Radius, 0.001f, 100000.f) || (Settings.Shape == EPhysicsShape::Capsule && !IsInRange(Settings.HalfHeight, 0.f, 100000.f))))
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

	// A capsule without a cylinder is a sphere; Jolt rejects a zero half height.
	const JPH::ShapeSettings::ShapeResult Shape = Settings.Shape == EPhysicsShape::Box                                    ? JPH::BoxShapeSettings(ToJolt(Settings.HalfExtents)).Create()
	                                              : Settings.Shape == EPhysicsShape::Sphere || Settings.HalfHeight <= 0.f ? JPH::SphereShapeSettings(Settings.Radius).Create()
	                                                                                                                      : JPH::CapsuleShapeSettings(Settings.HalfHeight, Settings.Radius).Create();
	if (Shape.HasError())
	{
		return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
	}

	const bool bDynamic = Settings.MotionType == EPhysicsMotionType::Dynamic;
	const bool bMoving = Settings.MotionType != EPhysicsMotionType::Static;
	const JPH::EMotionType MotionType = bDynamic ? JPH::EMotionType::Dynamic : bMoving ? JPH::EMotionType::Kinematic
	                                                                                   : JPH::EMotionType::Static;
	const FQuaternion Rotation = Settings.Rotation.NormalizedOrIdentity();
	JPH::BodyCreationSettings BodySettings(Shape.Get().GetPtr(), ToJolt(Settings.Position), ToJolt(Rotation), MotionType, bMoving ? DynamicLayer : StaticLayer);
	BodySettings.mFriction = Properties.Friction;
	BodySettings.mRestitution = Properties.Restitution;
	BodySettings.mIsSensor = Settings.bSensor;
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

	return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
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
	const JPH::EPhysicsUpdateError Error = Implementation->Physics.Update(FixedDeltaSeconds, 1, &Implementation->TempAllocator, &Implementation->JobSystem);
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

	// The capsule is lifted so the character position is its feet.
	const JPH::RefConst<JPH::Shape> Capsule = new JPH::CapsuleShape(Settings.Height * 0.5f - Settings.Radius, Settings.Radius);
	const JPH::ShapeSettings::ShapeResult Shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.f, Settings.Height * 0.5f, 0.f), JPH::Quat::sIdentity(), Capsule).Create();
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
	CharacterSettings->mInnerBodyLayer = DynamicLayer;
	Implementation->Characters.emplace_back(new JPH::CharacterVirtual(CharacterSettings, ToJolt(Settings.FootPosition), JPH::Quat::sIdentity(), 0, &Implementation->Physics));
	Implementation->CharacterStepHeights.push_back(Settings.StepHeight);
	++Implementation->BodyCount;
	return FPhysicsCharacterId{static_cast<std::uint32_t>(Implementation->Characters.size())};
}

std::expected<FPhysicsCharacterState, FPhysicsError> FPhysicsWorld::UpdateCharacterGround(const FPhysicsCharacterId CharacterId)
{
	JPH::CharacterVirtual* const Character = Implementation->FindCharacter(CharacterId);
	if (!Character)
	{
		return std::unexpected(FPhysicsError{"Unknown character"});
	}

	Character->UpdateGroundVelocity();
	return GetCharacterState(*Character);
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
	Update.mWalkStairsStepUp = JPH::Vec3(0.f, Implementation->CharacterStepHeights[CharacterId.Value - 1], 0.f);
	Character->SetLinearVelocity(ToJolt(Velocity));
	const FCharacterBodyFilter BodyFilter;
	Character->ExtendedUpdate(DeltaSeconds, Physics.GetGravity(), Update, Physics.GetDefaultBroadPhaseLayerFilter(DynamicLayer), Physics.GetDefaultLayerFilter(DynamicLayer), BodyFilter, {}, Implementation->TempAllocator);
	return GetCharacterState(*Character);
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
}
