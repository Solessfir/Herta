#pragma once

#include "Herta/Math/Quaternion.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Herta
{
struct FPhysicsError
{
	std::string Message;
};

struct FPhysicsBodyId
{
	std::uint32_t Value = 0xffffffffu;

	[[nodiscard]] constexpr bool operator==(const FPhysicsBodyId&) const = default;
};

enum class EPhysicsMotionType : std::uint8_t
{
	Static,
	Dynamic
};

struct FPhysicsBoxBodySettings
{
	FVector3 HalfExtents = FVector3::One();
	FVector3 Position = FVector3::Zero();
	FQuaternion Rotation = FQuaternion::Identity();
	EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
};

struct FPhysicsBodyTransform
{
	FVector3 Position;
	FQuaternion Rotation;
};

class FPhysicsWorld final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FPhysicsWorld>, FPhysicsError> Create();

	~FPhysicsWorld();

	FPhysicsWorld(const FPhysicsWorld&) = delete;
	FPhysicsWorld& operator=(const FPhysicsWorld&) = delete;
	FPhysicsWorld(FPhysicsWorld&&) = delete;
	FPhysicsWorld& operator=(FPhysicsWorld&&) = delete;

	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateBoxBody(const FPhysicsBoxBodySettings& Settings);
	[[nodiscard]] std::expected<void, FPhysicsError> Step(float FixedDeltaSeconds);
	[[nodiscard]] std::expected<FPhysicsBodyTransform, FPhysicsError> GetBodyTransform(FPhysicsBodyId BodyId) const;

private:
	struct FImplementation;
	explicit FPhysicsWorld(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
