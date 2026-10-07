#include "Herta/Level/LevelDescriptors.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace Herta
{
namespace
{
template <typename TEntity, typename TVisitor> auto VisitVisualProperty(TEntity& Entity, const ELevelComponentType Type, const std::string_view Key, TVisitor&& Visitor) -> decltype(Visitor(Entity.Light->Intensity))
{
	if (Type == ELevelComponentType::Light && Entity.Light)
	{
		if (Key == "type")
		{
			return Visitor(Entity.Light->Type);
		}

		if (Key == "color")
		{
			return Visitor(Entity.Light->Color);
		}

		if (Key == "intensity")
		{
			return Visitor(Entity.Light->Intensity);
		}

		if (Key == "useTemperature")
		{
			return Visitor(Entity.Light->bUseTemperature);
		}

		if (Key == "temperatureKelvin")
		{
			return Visitor(Entity.Light->TemperatureKelvin);
		}

		if (Key == "castShadows")
		{
			return Visitor(Entity.Light->bCastShadows);
		}

		if (Key == "shadowBias")
		{
			return Visitor(Entity.Light->ShadowBias);
		}

		if (Key == "shadowNormalBias")
		{
			return Visitor(Entity.Light->ShadowNormalBias);
		}

		if (Key == "range")
		{
			return Visitor(Entity.Light->Range);
		}

		if (Key == "innerConeAngle")
		{
			return Visitor(Entity.Light->InnerConeAngle);
		}

		if (Key == "outerConeAngle")
		{
			return Visitor(Entity.Light->OuterConeAngle);
		}

		if (Key == "width")
		{
			return Visitor(Entity.Light->Width);
		}

		if (Key == "height")
		{
			return Visitor(Entity.Light->Height);
		}

		if (Key == "environment")
		{
			return Visitor(Entity.Light->Environment);
		}

		if (Key == "ambientStrength")
		{
			return Visitor(Entity.Light->AmbientStrength);
		}

		if (Key == "environmentVisible")
		{
			return Visitor(Entity.Light->bEnvironmentVisible);
		}

		if (Key == "enabled")
		{
			return Visitor(Entity.Light->bEnabled);
		}
	}

	if (Type == ELevelComponentType::SkyAtmosphere && Entity.SkyAtmosphere)
	{
		if (Key == "sun")
		{
			return Visitor(Entity.SkyAtmosphere->Sun);
		}

		if (Key == "rayleighScattering")
		{
			return Visitor(Entity.SkyAtmosphere->RayleighScattering);
		}

		if (Key == "mieScattering")
		{
			return Visitor(Entity.SkyAtmosphere->MieScattering);
		}

		if (Key == "mieAnisotropy")
		{
			return Visitor(Entity.SkyAtmosphere->MieAnisotropy);
		}

		if (Key == "planetRadius")
		{
			return Visitor(Entity.SkyAtmosphere->PlanetRadius);
		}

		if (Key == "atmosphereHeight")
		{
			return Visitor(Entity.SkyAtmosphere->AtmosphereHeight);
		}

		if (Key == "enabled")
		{
			return Visitor(Entity.SkyAtmosphere->bEnabled);
		}
	}

	if (Type == ELevelComponentType::HeightFog && Entity.HeightFog)
	{
		if (Key == "density")
		{
			return Visitor(Entity.HeightFog->Density);
		}

		if (Key == "heightFalloff")
		{
			return Visitor(Entity.HeightFog->HeightFalloff);
		}

		if (Key == "albedo")
		{
			return Visitor(Entity.HeightFog->Albedo);
		}

		if (Key == "anisotropy")
		{
			return Visitor(Entity.HeightFog->Anisotropy);
		}

		if (Key == "maxDistance")
		{
			return Visitor(Entity.HeightFog->MaxDistance);
		}

		if (Key == "quality")
		{
			return Visitor(Entity.HeightFog->Quality);
		}

		if (Key == "enabled")
		{
			return Visitor(Entity.HeightFog->bEnabled);
		}

		if (Key == "volumetric")
		{
			return Visitor(Entity.HeightFog->bVolumetric);
		}
	}

	if (Type == ELevelComponentType::SoftBody && Entity.SoftBody)
	{
		if (Key == "shape")
		{
			return Visitor(Entity.SoftBody->Shape);
		}

		if (Key == "length")
		{
			return Visitor(Entity.SoftBody->Length);
		}

		if (Key == "height")
		{
			return Visitor(Entity.SoftBody->Height);
		}

		if (Key == "thickness")
		{
			return Visitor(Entity.SoftBody->Thickness);
		}

		if (Key == "massKg")
		{
			return Visitor(Entity.SoftBody->MassKg);
		}

		if (Key == "stiffness")
		{
			return Visitor(Entity.SoftBody->Stiffness);
		}

		if (Key == "pressure")
		{
			return Visitor(Entity.SoftBody->Pressure);
		}

		if (Key == "friction")
		{
			return Visitor(Entity.SoftBody->Friction);
		}

		if (Key == "pinned")
		{
			return Visitor(Entity.SoftBody->bPinned);
		}

		if (Key == "material")
		{
			return Visitor(Entity.SoftBody->Material);
		}

		if (Key == "attachment")
		{
			return Visitor(Entity.SoftBody->Attachment);
		}
	}

	return {};
}

constexpr FLevelTransform TransformDefaults{};
constexpr FLevelRigidBodySettings BodyDefaults{};
constexpr FLightComponent LightDefaults{};
constexpr FSkyAtmosphereComponent AtmosphereDefaults{};
constexpr FHeightFogComponent FogDefaults{};
constexpr FSoftBodyComponent SoftBodyDefaults{};

constexpr std::array TransformProperties{
    FLevelPropertyDescriptor{.Key = "translation", .Label = "Location", .Type = ELevelPropertyType::WorldPosition, .Unit = ELevelPropertyUnit::Meters, .Default = TransformDefaults.Translation},
    FLevelPropertyDescriptor{.Key = "rotation", .Label = "Rotation", .Type = ELevelPropertyType::Quaternion, .Default = TransformDefaults.Rotation},
    FLevelPropertyDescriptor{.Key = "scale", .Label = "Scale", .Type = ELevelPropertyType::Vector3, .Default = TransformDefaults.Scale},
};

constexpr std::array MeshProperties{
    FLevelPropertyDescriptor{.Key = "asset", .Label = "Static Mesh", .Type = ELevelPropertyType::AssetReference, .Default = FStaticMeshComponent{}.Asset},
    FLevelPropertyDescriptor{.Key = "materials", .Label = "Materials", .Type = ELevelPropertyType::AssetReferences, .Default = std::vector<FAssetId>{}},
};

constexpr std::array BodyProperties{
    FLevelPropertyDescriptor{.Key = "type", .Label = "Body type", .Type = ELevelPropertyType::BodyType, .Default = ELevelBodyType::Dynamic},
    FLevelPropertyDescriptor{.Key = "massKg", .Label = "Mass", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::Kilograms, .Default = BodyDefaults.MassKg, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 1'000'000.f}},
    FLevelPropertyDescriptor{.Key = "friction", .Label = "Friction", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.Friction, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "restitution", .Label = "Bounciness", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.Restitution, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "linearDamping", .Label = "Linear damping", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::InverseSeconds, .Default = BodyDefaults.LinearDamping, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "angularDamping", .Label = "Angular damping", .Type = ELevelPropertyType::Float, .Unit = ELevelPropertyUnit::InverseSeconds, .Default = BodyDefaults.AngularDamping, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "gravityScale", .Label = "Gravity scale", .Type = ELevelPropertyType::Float, .Default = BodyDefaults.GravityScale, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 10.}},
    FLevelPropertyDescriptor{.Key = "collision", .Label = "Collision", .Type = ELevelPropertyType::CollisionShape, .Default = BodyDefaults.Collision},
};

constexpr std::array LightProperties{
    FLevelPropertyDescriptor{.Key = "type", .Label = "Light type", .Type = ELevelPropertyType::LightType, .Default = LightDefaults.Type},
    FLevelPropertyDescriptor{.Key = "color", .Label = "Color", .Type = ELevelPropertyType::Vector3, .Default = LightDefaults.Color, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "intensity", .Label = "Intensity", .Default = LightDefaults.Intensity, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1'000'000'000.}},
    FLevelPropertyDescriptor{.Key = "useTemperature", .Label = "Use temperature", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bUseTemperature},
    FLevelPropertyDescriptor{.Key = "temperatureKelvin", .Label = "Temperature", .Unit = ELevelPropertyUnit::Kelvin, .Default = LightDefaults.TemperatureKelvin, .Range = FLevelPropertyRange{.Minimum = 1000., .Maximum = 40'000.}},
    FLevelPropertyDescriptor{.Key = "castShadows", .Label = "Cast shadows", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bCastShadows},
    FLevelPropertyDescriptor{.Key = "shadowBias", .Label = "Shadow bias", .Default = LightDefaults.ShadowBias, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "shadowNormalBias", .Label = "Normal bias", .Unit = ELevelPropertyUnit::Meters, .Default = LightDefaults.ShadowNormalBias, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 10.}},
    FLevelPropertyDescriptor{.Key = "range", .Label = "Range", .Unit = ELevelPropertyUnit::Meters, .Default = LightDefaults.Range, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 1'000'000.}},
    FLevelPropertyDescriptor{.Key = "innerConeAngle", .Label = "Inner cone", .Unit = ELevelPropertyUnit::Radians, .Default = LightDefaults.InnerConeAngle, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.553343f}},
    FLevelPropertyDescriptor{.Key = "outerConeAngle", .Label = "Outer cone", .Unit = ELevelPropertyUnit::Radians, .Default = LightDefaults.OuterConeAngle, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 1.553343f}},
    FLevelPropertyDescriptor{.Key = "width", .Label = "Width", .Unit = ELevelPropertyUnit::Meters, .Default = LightDefaults.Width, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 10'000.}},
    FLevelPropertyDescriptor{.Key = "height", .Label = "Height", .Unit = ELevelPropertyUnit::Meters, .Default = LightDefaults.Height, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 10'000.}},
    FLevelPropertyDescriptor{.Key = "environment", .Label = "Environment", .Type = ELevelPropertyType::AssetReference, .Default = LightDefaults.Environment},
    FLevelPropertyDescriptor{.Key = "ambientStrength", .Label = "Ambient strength", .Default = LightDefaults.AmbientStrength, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100.}},
    FLevelPropertyDescriptor{.Key = "environmentVisible", .Label = "Show environment", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bEnvironmentVisible},
    FLevelPropertyDescriptor{.Key = "enabled", .Label = "Enabled", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bEnabled},
};

constexpr std::array AtmosphereProperties{
    FLevelPropertyDescriptor{.Key = "sun", .Label = "Sun", .Type = ELevelPropertyType::ObjectReference, .Default = AtmosphereDefaults.Sun},
    FLevelPropertyDescriptor{.Key = "rayleighScattering", .Label = "Rayleigh scattering", .Default = AtmosphereDefaults.RayleighScattering, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100.}},
    FLevelPropertyDescriptor{.Key = "mieScattering", .Label = "Mie scattering", .Default = AtmosphereDefaults.MieScattering, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100.}},
    FLevelPropertyDescriptor{.Key = "mieAnisotropy", .Label = "Mie anisotropy", .Default = AtmosphereDefaults.MieAnisotropy, .Range = FLevelPropertyRange{.Minimum = -0.99f, .Maximum = 0.99f}},
    FLevelPropertyDescriptor{.Key = "planetRadius", .Label = "Planet radius", .Unit = ELevelPropertyUnit::Meters, .Default = AtmosphereDefaults.PlanetRadius, .Range = FLevelPropertyRange{.Minimum = 1000., .Maximum = 100'000'000.}},
    FLevelPropertyDescriptor{.Key = "atmosphereHeight", .Label = "Atmosphere height", .Unit = ELevelPropertyUnit::Meters, .Default = AtmosphereDefaults.AtmosphereHeight, .Range = FLevelPropertyRange{.Minimum = 1., .Maximum = 1'000'000.}},
    FLevelPropertyDescriptor{.Key = "enabled", .Label = "Enabled", .Type = ELevelPropertyType::Boolean, .Default = AtmosphereDefaults.bEnabled},
};

constexpr std::array FogProperties{
    FLevelPropertyDescriptor{.Key = "density", .Label = "Density", .Unit = ELevelPropertyUnit::InverseMeters, .Default = FogDefaults.Density, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 10.}},
    FLevelPropertyDescriptor{.Key = "heightFalloff", .Label = "Height falloff", .Unit = ELevelPropertyUnit::InverseMeters, .Default = FogDefaults.HeightFalloff, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100.}},
    FLevelPropertyDescriptor{.Key = "albedo", .Label = "Albedo", .Type = ELevelPropertyType::Vector3, .Default = FogDefaults.Albedo, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "anisotropy", .Label = "Anisotropy", .Default = FogDefaults.Anisotropy, .Range = FLevelPropertyRange{.Minimum = -0.99f, .Maximum = 0.99f}},
    FLevelPropertyDescriptor{.Key = "maxDistance", .Label = "View distance", .Unit = ELevelPropertyUnit::Meters, .Default = FogDefaults.MaxDistance, .Range = FLevelPropertyRange{.Minimum = 0.1f, .Maximum = 100'000.}},
    FLevelPropertyDescriptor{.Key = "quality", .Label = "Quality", .Type = ELevelPropertyType::FogQuality, .Default = FogDefaults.Quality},
    FLevelPropertyDescriptor{.Key = "enabled", .Label = "Enabled", .Type = ELevelPropertyType::Boolean, .Default = FogDefaults.bEnabled},
    FLevelPropertyDescriptor{.Key = "volumetric", .Label = "Volumetric", .Type = ELevelPropertyType::Boolean, .Default = FogDefaults.bVolumetric},
};

// Serialized key order is the component's JSON field order.
constexpr std::array SoftBodyProperties{
    FLevelPropertyDescriptor{.Key = "shape", .Label = "Shape", .Type = ELevelPropertyType::SoftBodyShape, .Default = SoftBodyDefaults.Shape},
    FLevelPropertyDescriptor{.Key = "length", .Label = "Length", .Unit = ELevelPropertyUnit::Meters, .Default = SoftBodyDefaults.Length, .Range = FLevelPropertyRange{.Minimum = 0.1f, .Maximum = 50.}},
    FLevelPropertyDescriptor{.Key = "height", .Label = "Height", .Unit = ELevelPropertyUnit::Meters, .Default = SoftBodyDefaults.Height, .Range = FLevelPropertyRange{.Minimum = 0.1f, .Maximum = 50.}},
    FLevelPropertyDescriptor{.Key = "thickness", .Label = "Thickness", .Unit = ELevelPropertyUnit::Meters, .Default = SoftBodyDefaults.Thickness, .Range = FLevelPropertyRange{.Minimum = 0.005f, .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "massKg", .Label = "Mass", .Unit = ELevelPropertyUnit::Kilograms, .Default = SoftBodyDefaults.MassKg, .Range = FLevelPropertyRange{.Minimum = 0.001f, .Maximum = 10'000.}},
    FLevelPropertyDescriptor{.Key = "stiffness", .Label = "Stiffness", .Default = SoftBodyDefaults.Stiffness, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "pressure", .Label = "Pressure", .Default = SoftBodyDefaults.Pressure, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1'000'000.}},
    FLevelPropertyDescriptor{.Key = "friction", .Label = "Friction", .Default = SoftBodyDefaults.Friction, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 1.}},
    FLevelPropertyDescriptor{.Key = "pinned", .Label = "Pinned", .Type = ELevelPropertyType::Boolean, .Default = SoftBodyDefaults.bPinned},
    FLevelPropertyDescriptor{.Key = "material", .Label = "Material", .Type = ELevelPropertyType::AssetReference, .Default = SoftBodyDefaults.Material},
    FLevelPropertyDescriptor{.Key = "attachment", .Label = "Attached body", .Type = ELevelPropertyType::ObjectReference, .Default = SoftBodyDefaults.Attachment},
};

constexpr std::array Components{
    FLevelComponentDescriptor{.Type = ELevelComponentType::Transform, .TypeId = "Herta.Level.Transform", .SerializationKey = "transform", .Label = "Transform", .Properties = TransformProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::StaticMesh, .TypeId = "Herta.Level.StaticMesh", .SerializationKey = "staticMesh", .Label = "Static Mesh", .Properties = MeshProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::RigidBody, .TypeId = "Herta.Level.RigidBody", .SerializationKey = "body", .Label = "Rigid Body", .Properties = BodyProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Light, .TypeId = "Herta.Level.Light", .SerializationKey = "light", .Label = "Light", .Properties = LightProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::SkyAtmosphere, .TypeId = "Herta.Level.SkyAtmosphere", .SerializationKey = "skyAtmosphere", .Label = "Sky Atmosphere", .Properties = AtmosphereProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::HeightFog, .TypeId = "Herta.Level.HeightFog", .SerializationKey = "heightFog", .Label = "Height Fog", .Properties = FogProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::SoftBody, .TypeId = "Herta.Level.SoftBody", .SerializationKey = "softBody", .Label = "Soft Body", .Properties = SoftBodyProperties},
};
}

std::span<const FLevelComponentDescriptor> GetLevelComponentDescriptors()
{
	return Components;
}

std::optional<FLevelPropertyValue> GetLevelVisualProperty(const FLevelEntity& Entity, const ELevelComponentType Type, const std::string_view Key)
{
	return VisitVisualProperty(Entity, Type, Key, [](const auto& Value) -> std::optional<FLevelPropertyValue>
	{
		return FLevelPropertyValue{Value};
	});
}

std::expected<void, FLevelError> SetLevelVisualProperty(FLevelEntity& Entity, const ELevelComponentType Type, const std::string_view Key, const FLevelPropertyValue& Value)
{
	FLevelEntity Candidate = Entity;
	const bool bWritten = VisitVisualProperty(Candidate, Type, Key, [&Value](auto& Property)
	{
		using T = std::decay_t<decltype(Property)>;
		if (const T* const Typed = std::get_if<T>(&Value))
		{
			Property = *Typed;
			return true;
		}

		return false;
	});

	if (!bWritten)
	{
		return std::unexpected(FLevelError{"Visual property is unknown, absent, or has the wrong value type"});
	}

	const auto Valid = Type == ELevelComponentType::Light           ? ValidateLightComponent(*Candidate.Light)
	                   : Type == ELevelComponentType::SkyAtmosphere ? ValidateSkyAtmosphereComponent(*Candidate.SkyAtmosphere)
	                   : Type == ELevelComponentType::SoftBody      ? ValidateSoftBodyComponent(*Candidate.SoftBody)
	                                                                : ValidateHeightFogComponent(*Candidate.HeightFog);
	if (!Valid)
	{
		return Valid;
	}

	Entity = std::move(Candidate);
	return {};
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
