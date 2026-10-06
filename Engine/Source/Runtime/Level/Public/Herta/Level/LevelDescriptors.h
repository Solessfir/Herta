#pragma once

#include "Herta/Level/World.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace Herta
{
enum class ELevelComponentType : std::uint8_t
{
	Transform,
	StaticMesh,
	RigidBody,
};

enum class ELevelPropertyType : std::uint8_t
{
	WorldPosition,
	Quaternion,
	Vector3,
	AssetReference,
	BodyType,
	Float,
};

enum class ELevelPropertyUnit : std::uint8_t
{
	None,
	Meters,
	Kilograms,
	InverseSeconds,
};

using FLevelPropertyValue = std::variant<FWorldPosition, FQuaternion, FVector3, FAssetId, ELevelBodyType, float>;

struct FLevelPropertyRange
{
	double Minimum = 0.;
	double Maximum = 0.;
};

struct FLevelPropertyDescriptor
{
	// Keys are serialized identity, independent of C++ member names and display labels.
	std::string_view Key;
	std::string_view Label;
	ELevelPropertyType Type = ELevelPropertyType::Float;
	ELevelPropertyUnit Unit = ELevelPropertyUnit::None;
	FLevelPropertyValue Default = 0.f;
	// Scalar component bounds. Unbounded transforms remain limited by their storage type.
	std::optional<FLevelPropertyRange> Range{};
};

struct FLevelComponentDescriptor
{
	ELevelComponentType Type = ELevelComponentType::Transform;
	std::string_view TypeId;
	std::string_view SerializationKey;
	std::string_view Label;
	std::span<const FLevelPropertyDescriptor> Properties;
};

std::span<const FLevelComponentDescriptor> GetLevelComponentDescriptors();
const FLevelComponentDescriptor& GetLevelComponentDescriptor(ELevelComponentType Type);
const FLevelComponentDescriptor* FindLevelComponentDescriptor(std::string_view TypeId);
const FLevelPropertyDescriptor* FindLevelPropertyDescriptor(const FLevelComponentDescriptor& Component, std::string_view Key);
}
