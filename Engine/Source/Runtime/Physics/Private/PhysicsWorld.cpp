#include "Herta/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
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
	// Jolt 5.6: one discrete step, at most one soft-body tether joint per body, and no parallel island splitting.
	// Cover contact buffers, island and joint arrays, CCD-index mapping, soft-body update contexts, and alignment padding before Jolt can abort.
	return sizeof(JPH::PhysicsUpdateContext::Step) + 320 * 1024
	       + static_cast<std::size_t>(Settings.MaxBodies) * 192
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
	return true;
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

struct FSoftBodyAttachmentState
{
	JPH::BodyID SoftBody;
	JPH::BodyID Body;
	std::uint32_t Vertex = 0;
	JPH::Vec3 LocalPoint;
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
	JPH::PhysicsSystem Physics;
	JPH::TempAllocatorImpl TempAllocator;
	JPH::JobSystemSingleThreaded JobSystem{2048};
	std::vector<JPH::BodyID> BodyIds;
	std::vector<FSoftBodyAttachmentState> Attachments;
	std::vector<JPH::Ref<JPH::Constraint>> Constraints;
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
		BodyIds.resize(Settings.MaxBodies);
	}

	~FImplementation()
	{
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
};

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

std::expected<FPhysicsBodyId, FPhysicsError> FPhysicsWorld::CreateBoxBody(const FPhysicsBoxBodySettings& Settings)
{
	if (Implementation->BodyCount >= Implementation->Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Physics world body capacity reached (MaxBodies={})", Implementation->Settings.MaxBodies)});
	}

	if (!IsFinite(Settings.HalfExtents) || Settings.HalfExtents.X <= 0.f || Settings.HalfExtents.Y <= 0.f || Settings.HalfExtents.Z <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Box half extents must be finite and positive"});
	}

	const float RotationLengthSquared = Settings.Rotation.LengthSquared();
	if (!IsFinite(Settings.Position) || !IsFinite(Settings.Rotation) || !std::isfinite(RotationLengthSquared) || RotationLengthSquared <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Body position and rotation must be finite, with a nonzero rotation"});
	}

	if (Settings.MotionType != EPhysicsMotionType::Static && Settings.MotionType != EPhysicsMotionType::Dynamic)
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

	const JPH::BoxShapeSettings ShapeSettings(ToJolt(Settings.HalfExtents));
	const JPH::ShapeSettings::ShapeResult Shape = ShapeSettings.Create();
	if (Shape.HasError())
	{
		return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
	}

	const bool bDynamic = Settings.MotionType == EPhysicsMotionType::Dynamic;
	const FQuaternion Rotation = Settings.Rotation.NormalizedOrIdentity();
	JPH::BodyCreationSettings BodySettings(Shape.Get().GetPtr(), ToJolt(Settings.Position), ToJolt(Rotation), bDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, bDynamic ? DynamicLayer : StaticLayer);
	BodySettings.mFriction = Properties.Friction;
	BodySettings.mRestitution = Properties.Restitution;

	if (bDynamic)
	{
		// Let Jolt scale the box's inertia to the authored mass.
		BodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		BodySettings.mMassPropertiesOverride.mMass = Properties.MassKg;
		BodySettings.mLinearDamping = Properties.LinearDamping;
		BodySettings.mAngularDamping = Properties.AngularDamping;
		BodySettings.mGravityFactor = Properties.GravityScale;
	}

	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	const JPH::BodyID Id = Bodies.CreateAndAddBody(BodySettings, bDynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Physics world body capacity reached"});
	}

	Implementation->BodyIds[Id.GetIndex()] = Id;
	++Implementation->BodyCount;
	return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
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

	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	JPH::BodyID AttachedBody;
	if (Settings.Attachment)
	{
		const FPhysicsSoftBodyAttachment& Attachment = *Settings.Attachment;
		AttachedBody = FindBody(Implementation->BodyIds, Attachment.Body);
		if (AttachedBody.IsInvalid() || Bodies.GetBodyType(AttachedBody) != JPH::EBodyType::RigidBody || Bodies.GetMotionType(AttachedBody) != JPH::EMotionType::Dynamic)
		{
			return std::unexpected(FPhysicsError{"Soft body attachments need an existing dynamic rigid body"});
		}

		const bool bValidTether = !Attachment.TetherVertex || (*Attachment.TetherVertex < VertexCount && Pinned[*Attachment.TetherVertex] && IsInRange(Attachment.TetherLength, 0.f, 1000.f));
		if (Attachment.Vertex >= VertexCount || Pinned[Attachment.Vertex] || !IsFinite(Attachment.Point) || !bValidTether)
		{
			return std::unexpected(FPhysicsError{"Soft body attachments need a free vertex, a finite point, and a pinned tether vertex with a length up to 1000 m"});
		}

		Pinned[Attachment.Vertex] = true;
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

	const JPH::BodyID Id = Bodies.CreateAndAddSoftBody(Creation, JPH::EActivation::Activate);
	if (Id.IsInvalid())
	{
		return std::unexpected(FPhysicsError{"Physics world body capacity reached"});
	}

	Implementation->BodyIds[Id.GetIndex()] = Id;
	++Implementation->BodyCount;
	if (Settings.Attachment)
	{
		const FPhysicsSoftBodyAttachment& Attachment = *Settings.Attachment;
		const JPH::RVec3 Point = ToJolt(Attachment.Point);
		Implementation->Attachments.push_back({.SoftBody = Id, .Body = AttachedBody, .Vertex = Attachment.Vertex, .LocalPoint = JPH::Vec3(Bodies.GetWorldTransform(AttachedBody).InversedRotationTranslation() * Point)});
		if (Attachment.TetherVertex)
		{
			// A distance limit with no minimum behaves like a rope: taut at full length and slack when the body swings closer.
			JPH::DistanceConstraintSettings Tether;
			Tether.mPoint1 = ToJolt(Settings.Vertices[*Attachment.TetherVertex]);
			Tether.mPoint2 = Point;
			Tether.mMinDistance = 0.f;
			Tether.mMaxDistance = std::max(Attachment.TetherLength, (Attachment.Point - Settings.Vertices[*Attachment.TetherVertex]).Length());
			const JPH::Ref<JPH::Constraint> Constraint = Bodies.CreateConstraint(&Tether, JPH::BodyID(), AttachedBody);
			Implementation->Physics.AddConstraint(Constraint);
			Implementation->Constraints.push_back(Constraint);
		}
	}

	return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
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

	JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
	for (const FSoftBodyAttachmentState& Attachment : Implementation->Attachments)
	{
		// Aim for where the attachment point will be after this step, so the vertex keeps pace instead of trailing a step behind.
		const JPH::RVec3 Point = Bodies.GetWorldTransform(Attachment.Body) * Attachment.LocalPoint;
		const JPH::RVec3 Target = Point + Bodies.GetPointVelocity(Attachment.Body, Point) * FixedDeltaSeconds;
		if (Bodies.IsActive(Attachment.Body))
		{
			Bodies.ActivateBody(Attachment.SoftBody);
		}

		const JPH::BodyLockWrite Lock(Implementation->Physics.GetBodyLockInterface(), Attachment.SoftBody);
		if (Lock.Succeeded())
		{
			JPH::Body& SoftBody = Lock.GetBody();
			JPH::SoftBodyVertex& Vertex = static_cast<JPH::SoftBodyMotionProperties&>(*SoftBody.GetMotionProperties()).GetVertex(Attachment.Vertex);
			Vertex.mVelocity = (JPH::Vec3(SoftBody.GetInverseCenterOfMassTransform() * Target) - Vertex.mPosition) / FixedDeltaSeconds;
		}
	}

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
}
