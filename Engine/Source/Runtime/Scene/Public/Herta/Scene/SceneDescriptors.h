#pragma once

#include "Herta/Scene/World.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace Herta
{
enum class ESceneComponentType : std::uint8_t
{
	Transform,
	StaticMesh,
	RigidBody,
};

enum class EScenePropertyType : std::uint8_t
{
	WorldPosition,
	Quaternion,
	Vector3,
	AssetReference,
	BodyType,
	Float,
};

enum class EScenePropertyUnit : std::uint8_t
{
	None,
	Meters,
	Kilograms,
	InverseSeconds,
};

using FScenePropertyValue = std::variant<FWorldPosition, FQuaternion, FVector3, FAssetId, ESceneBodyType, float>;

struct FScenePropertyRange
{
	double Minimum = 0.;
	double Maximum = 0.;
};

struct FScenePropertyDescriptor
{
	// Keys are serialized identity, independent of C++ member names and display labels.
	std::string_view Key;
	std::string_view Label;
	EScenePropertyType Type = EScenePropertyType::Float;
	EScenePropertyUnit Unit = EScenePropertyUnit::None;
	FScenePropertyValue Default = 0.f;
	// Scalar component bounds. Unbounded transforms remain limited by their storage type.
	std::optional<FScenePropertyRange> Range{};
};

struct FSceneComponentDescriptor
{
	ESceneComponentType Type = ESceneComponentType::Transform;
	std::string_view TypeId;
	std::string_view SerializationKey;
	std::string_view Label;
	std::span<const FScenePropertyDescriptor> Properties;
};

std::span<const FSceneComponentDescriptor> GetSceneComponentDescriptors();
const FSceneComponentDescriptor& GetSceneComponentDescriptor(ESceneComponentType Type);
const FSceneComponentDescriptor* FindSceneComponentDescriptor(std::string_view TypeId);
const FScenePropertyDescriptor* FindScenePropertyDescriptor(const FSceneComponentDescriptor& Component, std::string_view Key);
}
