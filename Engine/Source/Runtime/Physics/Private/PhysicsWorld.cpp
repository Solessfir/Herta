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
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer DynamicLayer = 1;
constexpr JPH::uint MaxBodies = 1024;
constexpr JPH::uint MaxBodyPairs = 4096;
constexpr JPH::uint MaxContactConstraints = 1024;

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

void AcquireRuntime()
{
	std::scoped_lock Lock(RuntimeMutex);
	if (RuntimeUsers != 0)
	{
		++RuntimeUsers;
		return;
	}

	if (JPH::Factory::sInstance != nullptr)
	{
		throw std::runtime_error("Jolt is already registered outside Herta Physics");
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
	try
	{
		JPH::RegisterTypes();
	}
	catch (...)
	{
		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
		JPH::Trace = PreviousTrace;
#ifdef JPH_ENABLE_ASSERTS
		JPH::AssertFailed = PreviousAssertFailed;
#endif
		throw;
	}
	RuntimeUsers = 1;
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
	JPH::TempAllocatorImpl TempAllocator{4 * 1024 * 1024};
	JPH::JobSystemSingleThreaded JobSystem{2048};
	std::vector<JPH::BodyID> BodyIds;

	FImplementation()
	{
		BroadPhaseLayers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
		BroadPhaseLayers.MapObjectToBroadPhaseLayer(DynamicLayer, JPH::BroadPhaseLayer(1));
		CollisionLayers.EnableCollision(StaticLayer, DynamicLayer);
		CollisionLayers.EnableCollision(DynamicLayer, DynamicLayer);
		BroadPhaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(BroadPhaseLayers, 2, CollisionLayers, 2);
		Physics.Init(MaxBodies, 0, MaxBodyPairs, MaxContactConstraints, BroadPhaseLayers, *BroadPhaseFilter, CollisionLayers);
		Physics.SetGravity(JPH::Vec3(0.f, -9.80665f, 0.f));
		BodyIds.reserve(MaxBodies);
	}

	~FImplementation()
	{
		JPH::BodyInterface& Bodies = Physics.GetBodyInterface();
		for (const JPH::BodyID Id : BodyIds)
		{
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

std::expected<std::unique_ptr<FPhysicsWorld>, FPhysicsError> FPhysicsWorld::Create()
{
	bool bRuntimeAcquired = false;
	try
	{
		AcquireRuntime();
		bRuntimeAcquired = true;
		return std::unique_ptr<FPhysicsWorld>(new FPhysicsWorld(std::make_unique<FImplementation>()));
	}
	catch (const std::exception& Error)
	{
		if (bRuntimeAcquired)
		{
			ReleaseRuntime();
		}

		return std::unexpected(FPhysicsError{Error.what()});
	}
	catch (...)
	{
		if (bRuntimeAcquired)
		{
			ReleaseRuntime();
		}

		return std::unexpected(FPhysicsError{"Physics world initialization failed"});
	}
}

std::expected<FPhysicsBodyId, FPhysicsError> FPhysicsWorld::CreateBoxBody(const FPhysicsBoxBodySettings& Settings)
{
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

	try
	{
		const JPH::BoxShapeSettings ShapeSettings(ToJolt(Settings.HalfExtents));
		const JPH::ShapeSettings::ShapeResult Shape = ShapeSettings.Create();
		if (Shape.HasError())
		{
			return std::unexpected(FPhysicsError{Shape.GetError().c_str()});
		}

		const bool bDynamic = Settings.MotionType == EPhysicsMotionType::Dynamic;
		const FQuaternion Rotation = Settings.Rotation.NormalizedOrIdentity();
		const JPH::BodyCreationSettings BodySettings(Shape.Get().GetPtr(), ToJolt(Settings.Position), ToJolt(Rotation), bDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, bDynamic ? DynamicLayer : StaticLayer);
		JPH::BodyInterface& Bodies = Implementation->Physics.GetBodyInterface();
		const JPH::BodyID Id = Bodies.CreateAndAddBody(BodySettings, bDynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		if (Id.IsInvalid())
		{
			return std::unexpected(FPhysicsError{"Physics world body capacity reached"});
		}

		Implementation->BodyIds.push_back(Id);
		return FPhysicsBodyId{Id.GetIndexAndSequenceNumber()};
	}
	catch (const std::exception& Error)
	{
		return std::unexpected(FPhysicsError{Error.what()});
	}
	catch (...)
	{
		return std::unexpected(FPhysicsError{"Physics body creation failed"});
	}
}

std::expected<void, FPhysicsError> FPhysicsWorld::Step(const float FixedDeltaSeconds)
{
	if (!std::isfinite(FixedDeltaSeconds) || FixedDeltaSeconds <= 0.f)
	{
		return std::unexpected(FPhysicsError{"Physics step must be finite and positive"});
	}

	try
	{
		if (Implementation->Physics.Update(FixedDeltaSeconds, 1, &Implementation->TempAllocator, &Implementation->JobSystem) != JPH::EPhysicsUpdateError::None)
		{
			return std::unexpected(FPhysicsError{"Physics contact capacity exceeded"});
		}

		return {};
	}
	catch (const std::exception& Error)
	{
		return std::unexpected(FPhysicsError{Error.what()});
	}
	catch (...)
	{
		return std::unexpected(FPhysicsError{"Physics step failed"});
	}
}

std::expected<FPhysicsBodyTransform, FPhysicsError> FPhysicsWorld::GetBodyTransform(const FPhysicsBodyId BodyId) const
{
	const auto Match = std::ranges::find_if(Implementation->BodyIds, [BodyId](const JPH::BodyID Id)
	{
		return Id.GetIndexAndSequenceNumber() == BodyId.Value;
	});

	if (Match == Implementation->BodyIds.end())
	{
		return std::unexpected(FPhysicsError{"Unknown physics body"});
	}

	JPH::RVec3 Position;
	JPH::Quat Rotation;
	Implementation->Physics.GetBodyInterface().GetPositionAndRotation(*Match, Position, Rotation);
	return FPhysicsBodyTransform{.Position = {Position.GetX(), Position.GetY(), Position.GetZ()},
	    .Rotation = {Rotation.GetX(), Rotation.GetY(), Rotation.GetZ(), Rotation.GetW()}};
}
}
