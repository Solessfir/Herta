#include "Herta/Scene/SceneDescriptors.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Herta
{
namespace
{
constexpr FSceneTransform TransformDefaults{};
constexpr FSceneRigidBodySettings BodyDefaults{};

constexpr std::array TransformProperties{
    FScenePropertyDescriptor{.Key = "translation", .Label = "Location", .Type = EScenePropertyType::WorldPosition, .Unit = EScenePropertyUnit::Meters, .Default = TransformDefaults.Translation},
    FScenePropertyDescriptor{.Key = "rotation", .Label = "Rotation", .Type = EScenePropertyType::Quaternion, .Default = TransformDefaults.Rotation},
    FScenePropertyDescriptor{.Key = "scale", .Label = "Scale", .Type = EScenePropertyType::Vector3, .Default = TransformDefaults.Scale},
};

constexpr std::array MeshProperties{
    FScenePropertyDescriptor{.Key = "asset", .Label = "Static Mesh", .Type = EScenePropertyType::AssetReference, .Default = FStaticMeshComponent{}.Asset},
};

constexpr std::array BodyProperties{
    FScenePropertyDescriptor{.Key = "type", .Label = "Body type", .Type = EScenePropertyType::BodyType, .Default = ESceneBodyType::Dynamic},
    FScenePropertyDescriptor{.Key = "massKg", .Label = "Mass", .Type = EScenePropertyType::Float, .Unit = EScenePropertyUnit::Kilograms, .Default = BodyDefaults.MassKg, .Range = FScenePropertyRange{.Minimum = 0.001f, .Maximum = 1'000'000.f}},
    FScenePropertyDescriptor{.Key = "friction", .Label = "Friction", .Type = EScenePropertyType::Float, .Default = BodyDefaults.Friction, .Range = FScenePropertyRange{.Minimum = 0., .Maximum = 1.}},
    FScenePropertyDescriptor{.Key = "restitution", .Label = "Bounciness", .Type = EScenePropertyType::Float, .Default = BodyDefaults.Restitution, .Range = FScenePropertyRange{.Minimum = 0., .Maximum = 1.}},
    FScenePropertyDescriptor{.Key = "linearDamping", .Label = "Linear damping", .Type = EScenePropertyType::Float, .Unit = EScenePropertyUnit::InverseSeconds, .Default = BodyDefaults.LinearDamping, .Range = FScenePropertyRange{.Minimum = 0., .Maximum = 1.}},
    FScenePropertyDescriptor{.Key = "angularDamping", .Label = "Angular damping", .Type = EScenePropertyType::Float, .Unit = EScenePropertyUnit::InverseSeconds, .Default = BodyDefaults.AngularDamping, .Range = FScenePropertyRange{.Minimum = 0., .Maximum = 1.}},
    FScenePropertyDescriptor{.Key = "gravityScale", .Label = "Gravity scale", .Type = EScenePropertyType::Float, .Default = BodyDefaults.GravityScale, .Range = FScenePropertyRange{.Minimum = 0., .Maximum = 10.}},
};

constexpr std::array Components{
    FSceneComponentDescriptor{.Type = ESceneComponentType::Transform, .TypeId = "Herta.Scene.Transform", .SerializationKey = "transform", .Label = "Transform", .Properties = TransformProperties},
    FSceneComponentDescriptor{.Type = ESceneComponentType::StaticMesh, .TypeId = "Herta.Scene.StaticMesh", .SerializationKey = "staticMesh", .Label = "Static Mesh", .Properties = MeshProperties},
    FSceneComponentDescriptor{.Type = ESceneComponentType::RigidBody, .TypeId = "Herta.Scene.RigidBody", .SerializationKey = "body", .Label = "Rigid Body", .Properties = BodyProperties},
};
}

std::span<const FSceneComponentDescriptor> GetSceneComponentDescriptors()
{
	return Components;
}

const FSceneComponentDescriptor& GetSceneComponentDescriptor(const ESceneComponentType Type)
{
	for (const FSceneComponentDescriptor& Component : Components)
	{
		if (Component.Type == Type)
		{
			return Component;
		}
	}

	std::unreachable();
}

const FSceneComponentDescriptor* FindSceneComponentDescriptor(const std::string_view TypeId)
{
	const auto Found = std::ranges::find(Components, TypeId, &FSceneComponentDescriptor::TypeId);
	return Found == Components.end() ? nullptr : &*Found;
}

const FScenePropertyDescriptor* FindScenePropertyDescriptor(const FSceneComponentDescriptor& Component, const std::string_view Key)
{
	const auto Found = std::ranges::find(Component.Properties, Key, &FScenePropertyDescriptor::Key);
	return Found == Component.Properties.end() ? nullptr : &*Found;
}
}
