#include "Herta/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/RegisterTypes.h>

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
	// Jolt 5.6: one discrete rigid-body step, no joints, soft bodies, or parallel island splitting.
	// Cover contact buffers, island arrays, CCD-index mapping, and alignment padding before Jolt can abort.
	return sizeof(JPH::PhysicsUpdateContext::Step) + 64 * 1024
	       + static_cast<std::size_t>(Settings.MaxBodies) * 128
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
}
