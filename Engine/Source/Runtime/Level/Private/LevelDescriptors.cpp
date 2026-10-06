#include "Herta/Level/LevelDescriptors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Herta
{
namespace
{
constexpr FLevelTransform TransformDefaults{};
constexpr FLevelRigidBodySettings BodyDefaults{};

constexpr std::array TransformProperties{
    FLevelPropertyDescriptor{.Key = "translation", .Label = "Location", .Type = ELevelPropertyType::WorldPosition, .Unit = ELevelPropertyUnit::Meters, .Default = TransformDefaults.Translation},
    FLevelPropertyDescriptor{.Key = "rotation", .Label = "Rotation", .Type = ELevelPropertyType::Quaternion, .Default = TransformDefaults.Rotation},
    FLevelPropertyDescriptor{.Key = "scale", .Label = "Scale", .Type = ELevelPropertyType::Vector3, .Default = TransformDefaults.Scale},
};

constexpr std::array MeshProperties{
    FLevelPropertyDescriptor{.Key = "asset", .Label = "Static Mesh", .Type = ELevelPropertyType::AssetReference, .Default = FStaticMeshComponent{}.Asset},
};

constexpr std::array BodyProperties{
    FLevelPropertyDescriptor{.Key = "type", .Label = "Body type", .Type = ELevelPropertyType::BodyType, .Default = ELevelBodyType::Dynamic},
    FLevelPropertyDescriptor{.Key = "massKg", .Label = "Mass", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::Kilograms, .Default = BodyDefaults.MassKg, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 1'000'000.f}},
    FLevelPropertyDescriptor{.Key = "friction", .Label = "Friction", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.Friction, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "restitution", .Label = "Bounciness", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.Restitution, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "linearDamping", .Label = "Linear damping", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::InverseSeconds, .Default = BodyDefaults.LinearDamping, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "angularDamping", .Label = "Angular damping", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::InverseSeconds, .Default = BodyDefaults.AngularDamping, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "gravityScale", .Label = "Gravity scale", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.GravityScale, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 10.}},
};

constexpr std::array Components{
    FLevelComponentDescriptor{.Type = ELevelComponentType::Transform, .TypeId = "Herta.Level.Transform", .SerializationKey = "transform", .Label = "Transform", .Properties = TransformProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::StaticMesh, .TypeId = "Herta.Level.StaticMesh", .SerializationKey = "staticMesh", .Label = "Static Mesh", .Properties = MeshProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::RigidBody, .TypeId = "Herta.Level.RigidBody", .SerializationKey = "body", .Label = "Rigid Body", .Properties = BodyProperties},
};
}

std::span<const FLevelComponentDescriptor> GetLevelComponentDescriptors()
{
	return Components;
}

const FLevelComponentDescriptor& GetLevelComponentDescriptor(const ELevelComponentType Type)
{
	for (const FLevelComponentDescriptor& Component : Components)
	{
		if (Component.Type == Type)
		{
			return Component;
		}
	}

	std::unreachable();
}

const FLevelComponentDescriptor* FindLevelComponentDescriptor(const std::string_view TypeId)
{
	const auto Found = std::ranges::find(Components, TypeId, &FLevelComponentDescriptor::TypeId);
	return Found == Components.end() ? nullptr : &*Found;
}

const FLevelPropertyDescriptor* FindLevelPropertyDescriptor(const FLevelComponentDescriptor& Component, const std::string_view Key)
{
	const auto Found = std::ranges::find(Component.Properties, Key, &FLevelPropertyDescriptor::Key);
	return Found == Component.Properties.end() ? nullptr : &*Found;
}
}
