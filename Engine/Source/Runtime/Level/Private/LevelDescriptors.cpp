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

		if (Key == "sourceAngle")
		{
			return Visitor(Entity.Light->SourceAngle);
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
	}

	if (Type == ELevelComponentType::Destructible && Entity.Destructible)
	{
		if (Key == "cells")
		{
			return Visitor(Entity.Destructible->Cells);
		}

		if (Key == "seed")
		{
			return Visitor(Entity.Destructible->Seed);
		}

		if (Key == "strength")
		{
			return Visitor(Entity.Destructible->Strength);
		}

		if (Key == "mass")
		{
			return Visitor(Entity.Destructible->MassKg);
		}

		if (Key == "anchored")
		{
			return Visitor(Entity.Destructible->bAnchored);
		}

		if (Key == "impactSound")
		{
			return Visitor(Entity.Destructible->ImpactSound);
		}

		if (Key == "impactEffect")
		{
			return Visitor(Entity.Destructible->ImpactEffect);
		}

		if (Key == "breakEffect")
		{
			return Visitor(Entity.Destructible->BreakEffect);
		}
	}

	if (Type == ELevelComponentType::Character && Entity.Character)
	{
		if (Key == "maximumPieces")
		{
			return Visitor(Entity.Character->MaximumPieces);
		}

		if (Key == "severImpulse")
		{
			return Visitor(Entity.Character->SeverImpulse);
		}

		if (Key == "weapon")
		{
			return Visitor(Entity.Character->Weapon);
		}

		if (Key == "footstepSound")
		{
			return Visitor(Entity.Character->FootstepSound);
		}

		if (Key == "landSound")
		{
			return Visitor(Entity.Character->LandSound);
		}

		if (Key == "jumpSound")
		{
			return Visitor(Entity.Character->JumpSound);
		}

		if (Key == "slideSound")
		{
			return Visitor(Entity.Character->SlideSound);
		}

		if (Key == "swingSound")
		{
			return Visitor(Entity.Character->SwingSound);
		}

		if (Key == "hitSound")
		{
			return Visitor(Entity.Character->HitSound);
		}

		if (Key == "sliceSound")
		{
			return Visitor(Entity.Character->SliceSound);
		}

		if (Key == "severSound")
		{
			return Visitor(Entity.Character->SeverSound);
		}

		if (Key == "deflectSound")
		{
			return Visitor(Entity.Character->DeflectSound);
		}

		if (Key == "blockSound")
		{
			return Visitor(Entity.Character->BlockSound);
		}

		if (Key == "landEffect")
		{
			return Visitor(Entity.Character->LandEffect);
		}

		if (Key == "sliceEffect")
		{
			return Visitor(Entity.Character->SliceEffect);
		}

		if (Key == "severEffect")
		{
			return Visitor(Entity.Character->SeverEffect);
		}

		if (Key == "sparksEffect")
		{
			return Visitor(Entity.Character->SparksEffect);
		}

		if (Key == "trailEffect")
		{
			return Visitor(Entity.Character->TrailEffect);
		}
	}

	if (Type == ELevelComponentType::Player && Entity.Player && Key == "thirdPerson")
	{
		return Visitor(Entity.Player->bThirdPerson);
	}

	if (Type == ELevelComponentType::RigidBody && Entity.BodyType != ELevelBodyType::None)
	{
		if (Key == "impactSound")
		{
			return Visitor(Entity.BodySettings.ImpactSound);
		}

		if (Key == "impactEffect")
		{
			return Visitor(Entity.BodySettings.ImpactEffect);
		}
	}

	if (Type == ELevelComponentType::AudioEmitter && Entity.AudioEmitter)
	{
		if (Key == "sound")
		{
			return Visitor(Entity.AudioEmitter->Sound);
		}

		if (Key == "volume")
		{
			return Visitor(Entity.AudioEmitter->Volume);
		}

		if (Key == "pitch")
		{
			return Visitor(Entity.AudioEmitter->Pitch);
		}

		if (Key == "autoPlay")
		{
			return Visitor(Entity.AudioEmitter->bAutoPlay);
		}

		if (Key == "spatial")
		{
			return Visitor(Entity.AudioEmitter->bSpatial);
		}

		if (Key == "minimumDistance")
		{
			return Visitor(Entity.AudioEmitter->MinimumDistance);
		}

		if (Key == "maximumDistance")
		{
			return Visitor(Entity.AudioEmitter->MaximumDistance);
		}
	}

	if (Type == ELevelComponentType::NavigationBounds && Entity.NavigationBounds && Key == "size")
	{
		return Visitor(Entity.NavigationBounds->Size);
	}

	if (Type == ELevelComponentType::NavigationObstacle && Entity.NavigationObstacle && Key == "size")
	{
		return Visitor(Entity.NavigationObstacle->Size);
	}

	if (Type == ELevelComponentType::AiAgent && Entity.AiAgent)
	{
		if (Key == "stateTree")
		{
			return Visitor(Entity.AiAgent->StateTree);
		}

		if (Key == "sightRange")
		{
			return Visitor(Entity.AiAgent->SightRange);
		}

		if (Key == "sightHalfAngle")
		{
			return Visitor(Entity.AiAgent->SightHalfAngle);
		}

		if (Key == "hearingRange")
		{
			return Visitor(Entity.AiAgent->HearingRange);
		}

		if (Key == "walkSpeed")
		{
			return Visitor(Entity.AiAgent->WalkSpeed);
		}

		if (Key == "runSpeed")
		{
			return Visitor(Entity.AiAgent->RunSpeed);
		}

		if (Key == "attitude")
		{
			return Visitor(Entity.AiAgent->Attitude);
		}
	}

	if (Type == ELevelComponentType::GameplayGraph && Entity.GameplayGraph && Key == "graph")
	{
		return Visitor(Entity.GameplayGraph->Graph);
	}

	if (Type == ELevelComponentType::ParticleEmitter && Entity.ParticleEmitter)
	{
		if (Key == "system")
		{
			return Visitor(Entity.ParticleEmitter->System);
		}

		if (Key == "autoPlay")
		{
			return Visitor(Entity.ParticleEmitter->bAutoPlay);
		}

		if (Key == "scale")
		{
			return Visitor(Entity.ParticleEmitter->Scale);
		}
	}

	if (Type == ELevelComponentType::AudioListener && Entity.AudioListener)
	{
		if (Key == "active")
		{
			return Visitor(Entity.AudioListener->bActive);
		}
	}

	if (Type == ELevelComponentType::Mover && Entity.Mover)
	{
		if (Key == "offset")
		{
			return Visitor(Entity.Mover->Offset);
		}

		if (Key == "periodSeconds")
		{
			return Visitor(Entity.Mover->PeriodSeconds);
		}
	}

	if (Type == ELevelComponentType::Trigger && Entity.Trigger)
	{
		if (Key == "size")
		{
			return Visitor(Entity.Trigger->Size);
		}

		if (Key == "role")
		{
			return Visitor(Entity.Trigger->Role);
		}

		if (Key == "sound")
		{
			return Visitor(Entity.Trigger->Sound);
		}
	}

	if (Type == ELevelComponentType::Constraint && Entity.Constraint)
	{
		if (Key == "target")
		{
			return Visitor(Entity.Constraint->Target);
		}

		if (Key == "anchor")
		{
			return Visitor(Entity.Constraint->Anchor);
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
constexpr FMoverComponent MoverDefaults{};
constexpr FDestructibleComponent DestructibleDefaults{};
constexpr FCharacterComponent CharacterDefaults{};
constexpr FPlayerComponent PlayerDefaults{};
constexpr FAudioEmitterComponent EmitterDefaults{};
constexpr FAudioListenerComponent ListenerDefaults{};
constexpr FParticleEmitterComponent ParticleDefaults{};
constexpr FNavigationBoundsComponent NavigationBoundsDefaults{};
constexpr FNavigationObstacleComponent NavigationObstacleDefaults{};
constexpr FAiAgentComponent AiAgentDefaults{};
constexpr FGameplayGraphComponent GameplayGraphDefaults{};
constexpr FTriggerComponent TriggerDefaults{};
constexpr FConstraintComponent ConstraintDefaults{};

constexpr std::array TransformProperties{
    FLevelPropertyDescriptor{.Key = "translation", .Label = "Location", .Type = ELevelPropertyType::WorldPosition, .Unit = ELevelPropertyUnit::Meters, .Default = TransformDefaults.Translation},
    FLevelPropertyDescriptor{.Key = "rotation", .Label = "Rotation", .Type = ELevelPropertyType::Quaternion, .Default = TransformDefaults.Rotation},
    FLevelPropertyDescriptor{.Key = "scale", .Label = "Scale", .Type = ELevelPropertyType::Vector3, .Default = TransformDefaults.Scale},
};

// Not constexpr: MSVC debug builds give std::vector a heap-allocated iterator proxy, which a constant expression cannot keep.
const std::array MeshProperties{
    FLevelPropertyDescriptor{.Key = "asset", .Label = "Static Mesh", .Type = ELevelPropertyType::AssetReference, .Default = FStaticMeshComponent{}.Asset, .AssetKind = ELevelAssetKind::Model},
    FLevelPropertyDescriptor{.Key = "materials", .Label = "Materials", .Type = ELevelPropertyType::AssetReferences, .Default = std::vector<FAssetId>{}, .AssetKind = ELevelAssetKind::Material},
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
    FLevelPropertyDescriptor{.Key = "impactSound", .Label = "Impact sound", .Type = ELevelPropertyType::AssetReference, .Default = BodyDefaults.ImpactSound, .AssetKind = ELevelAssetKind::Sound},
    FLevelPropertyDescriptor{.Key = "impactEffect", .Label = "Impact effect", .Type = ELevelPropertyType::AssetReference, .Default = BodyDefaults.ImpactEffect, .AssetKind = ELevelAssetKind::Effect},
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
    FLevelPropertyDescriptor{.Key = "environment", .Label = "Environment", .Type = ELevelPropertyType::AssetReference, .Default = LightDefaults.Environment, .AssetKind = ELevelAssetKind::Environment},
    FLevelPropertyDescriptor{.Key = "ambientStrength", .Label = "Ambient strength", .Default = LightDefaults.AmbientStrength, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100.}},
    FLevelPropertyDescriptor{.Key = "environmentVisible", .Label = "Show environment", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bEnvironmentVisible},
    FLevelPropertyDescriptor{.Key = "enabled", .Label = "Enabled", .Type = ELevelPropertyType::Boolean, .Default = LightDefaults.bEnabled},
    FLevelPropertyDescriptor{.Key = "sourceAngle", .Label = "Source angle", .Unit = ELevelPropertyUnit::Radians, .Default = LightDefaults.SourceAngle, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 0.17453292f}},
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
    FLevelPropertyDescriptor{.Key = "material", .Label = "Material", .Type = ELevelPropertyType::AssetReference, .Default = SoftBodyDefaults.Material, .AssetKind = ELevelAssetKind::Material},
};

constexpr std::array MoverProperties{
    FLevelPropertyDescriptor{.Key = "offset", .Label = "Offset", .Type = ELevelPropertyType::Vector3, .Unit = ELevelPropertyUnit::Meters, .Default = MoverDefaults.Offset, .Range = FLevelPropertyRange{.Minimum = -1000., .Maximum = 1000.}},
    FLevelPropertyDescriptor{.Key = "periodSeconds", .Label = "Period", .Unit = ELevelPropertyUnit::Seconds, .Default = MoverDefaults.PeriodSeconds, .Range = FLevelPropertyRange{.Minimum = 0.5, .Maximum = 600.}},
};

constexpr std::array DestructibleProperties{
    FLevelPropertyDescriptor{.Key = "cells", .Label = "Cells", .Default = DestructibleDefaults.Cells, .Range = FLevelPropertyRange{.Minimum = 2., .Maximum = 64.}},
    FLevelPropertyDescriptor{.Key = "seed", .Label = "Seed", .Default = DestructibleDefaults.Seed, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 100000.}},
    FLevelPropertyDescriptor{.Key = "strength", .Label = "Strength", .Default = DestructibleDefaults.Strength, .Range = FLevelPropertyRange{.Minimum = 1., .Maximum = 1000000.}},
    FLevelPropertyDescriptor{.Key = "mass", .Label = "Mass", .Unit = ELevelPropertyUnit::Kilograms, .Default = DestructibleDefaults.MassKg, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 1000000.}},
    FLevelPropertyDescriptor{.Key = "anchored", .Label = "Anchored", .Type = ELevelPropertyType::Boolean, .Default = DestructibleDefaults.bAnchored},
    FLevelPropertyDescriptor{.Key = "impactSound", .Label = "Impact sound", .Type = ELevelPropertyType::AssetReference, .Default = DestructibleDefaults.ImpactSound, .AssetKind = ELevelAssetKind::Sound},
    FLevelPropertyDescriptor{.Key = "impactEffect", .Label = "Impact effect", .Type = ELevelPropertyType::AssetReference, .Default = DestructibleDefaults.ImpactEffect, .AssetKind = ELevelAssetKind::Effect},
    FLevelPropertyDescriptor{.Key = "breakEffect", .Label = "Break effect", .Type = ELevelPropertyType::AssetReference, .Default = DestructibleDefaults.BreakEffect, .AssetKind = ELevelAssetKind::Effect},
};

constexpr std::array TriggerProperties{
    FLevelPropertyDescriptor{.Key = "size", .Label = "Size", .Type = ELevelPropertyType::Vector3, .Unit = ELevelPropertyUnit::Meters, .Default = TriggerDefaults.Size, .Range = FLevelPropertyRange{.Minimum = 0.01, .Maximum = 1000.}},
    FLevelPropertyDescriptor{.Key = "role", .Label = "Role", .Type = ELevelPropertyType::TriggerRole, .Default = TriggerDefaults.Role},
    FLevelPropertyDescriptor{.Key = "sound", .Label = "Course sound", .Type = ELevelPropertyType::AssetReference, .Default = TriggerDefaults.Sound, .AssetKind = ELevelAssetKind::Sound},
};

constexpr std::array ConstraintProperties{
    FLevelPropertyDescriptor{.Key = "target", .Label = "Target", .Type = ELevelPropertyType::ObjectReference, .Default = ConstraintDefaults.Target},
    FLevelPropertyDescriptor{.Key = "anchor", .Label = "Anchor", .Type = ELevelPropertyType::Vector3, .Unit = ELevelPropertyUnit::Meters, .Default = ConstraintDefaults.Anchor, .Range = FLevelPropertyRange{.Minimum = -1000., .Maximum = 1000.}},
};

constexpr std::array CharacterProperties{
    FLevelPropertyDescriptor{.Key = "maximumPieces", .Label = "Maximum Pieces", .Default = CharacterDefaults.MaximumPieces, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 64.}},
    FLevelPropertyDescriptor{.Key = "severImpulse", .Label = "Sever Impulse", .Default = CharacterDefaults.SeverImpulse, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 100000.}},
    FLevelPropertyDescriptor{.Key = "weapon", .Label = "Weapon", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.Weapon, .AssetKind = ELevelAssetKind::Model},
    FLevelPropertyDescriptor{.Key = "footstepSound", .Label = "Footstep", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.FootstepSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "landSound", .Label = "Land", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.LandSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "jumpSound", .Label = "Jump", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.JumpSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "slideSound", .Label = "Slide", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SlideSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "swingSound", .Label = "Swing", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SwingSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "hitSound", .Label = "Hit", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.HitSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "sliceSound", .Label = "Slice", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SliceSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "severSound", .Label = "Sever", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SeverSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "deflectSound", .Label = "Deflect", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.DeflectSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "blockSound", .Label = "Block", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.BlockSound, .AssetKind = ELevelAssetKind::Sound, .Group = "Sounds"},
    FLevelPropertyDescriptor{.Key = "landEffect", .Label = "Land", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.LandEffect, .AssetKind = ELevelAssetKind::Effect, .Group = "Effects"},
    FLevelPropertyDescriptor{.Key = "sliceEffect", .Label = "Slice", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SliceEffect, .AssetKind = ELevelAssetKind::Effect, .Group = "Effects"},
    FLevelPropertyDescriptor{.Key = "severEffect", .Label = "Sever", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SeverEffect, .AssetKind = ELevelAssetKind::Effect, .Group = "Effects"},
    FLevelPropertyDescriptor{.Key = "sparksEffect", .Label = "Sparks", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.SparksEffect, .AssetKind = ELevelAssetKind::Effect, .Group = "Effects"},
    FLevelPropertyDescriptor{.Key = "trailEffect", .Label = "Blade trail", .Type = ELevelPropertyType::AssetReference, .Default = CharacterDefaults.TrailEffect, .AssetKind = ELevelAssetKind::Effect, .Group = "Effects"},
};

constexpr std::array PlayerProperties{
    FLevelPropertyDescriptor{.Key = "thirdPerson", .Label = "Third person", .Type = ELevelPropertyType::Boolean, .Default = PlayerDefaults.bThirdPerson},
};

constexpr std::array AudioEmitterProperties{
    FLevelPropertyDescriptor{.Key = "sound", .Label = "Sound", .Type = ELevelPropertyType::AssetReference, .Default = EmitterDefaults.Sound, .AssetKind = ELevelAssetKind::Sound},
    FLevelPropertyDescriptor{.Key = "volume", .Label = "Volume", .Default = EmitterDefaults.Volume, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 4.}},
    FLevelPropertyDescriptor{.Key = "pitch", .Label = "Pitch", .Default = EmitterDefaults.Pitch, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 4.}},
    FLevelPropertyDescriptor{.Key = "autoPlay", .Label = "Auto Play", .Type = ELevelPropertyType::Boolean, .Default = EmitterDefaults.bAutoPlay},
    FLevelPropertyDescriptor{.Key = "spatial", .Label = "Spatial", .Type = ELevelPropertyType::Boolean, .Default = EmitterDefaults.bSpatial},
    FLevelPropertyDescriptor{.Key = "minimumDistance", .Label = "Minimum Distance", .Unit = ELevelPropertyUnit::Meters, .Default = EmitterDefaults.MinimumDistance, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 1000.}},
    FLevelPropertyDescriptor{.Key = "maximumDistance", .Label = "Maximum Distance", .Unit = ELevelPropertyUnit::Meters, .Default = EmitterDefaults.MaximumDistance, .Range = FLevelPropertyRange{.Minimum = 0.2, .Maximum = 10000.}},
};

constexpr std::array AudioListenerProperties{
    FLevelPropertyDescriptor{.Key = "active", .Label = "Active", .Type = ELevelPropertyType::Boolean, .Default = ListenerDefaults.bActive},
};

constexpr std::array ParticleEmitterProperties{
    FLevelPropertyDescriptor{.Key = "system", .Label = "Particle System", .Type = ELevelPropertyType::AssetReference, .Default = ParticleDefaults.System, .AssetKind = ELevelAssetKind::Effect},
    FLevelPropertyDescriptor{.Key = "autoPlay", .Label = "Auto Play", .Type = ELevelPropertyType::Boolean, .Default = ParticleDefaults.bAutoPlay},
    FLevelPropertyDescriptor{.Key = "scale", .Label = "Scale", .Default = ParticleDefaults.Scale, .Range = FLevelPropertyRange{.Minimum = 0.01, .Maximum = 100.}},
};

constexpr std::array NavigationBoundsProperties{
    FLevelPropertyDescriptor{.Key = "size", .Label = "Size", .Type = ELevelPropertyType::Vector3, .Unit = ELevelPropertyUnit::Meters, .Default = NavigationBoundsDefaults.Size, .Range = FLevelPropertyRange{.Minimum = 0.01, .Maximum = 10000.}},
};

constexpr std::array NavigationObstacleProperties{
    FLevelPropertyDescriptor{.Key = "size", .Label = "Size", .Type = ELevelPropertyType::Vector3, .Unit = ELevelPropertyUnit::Meters, .Default = NavigationObstacleDefaults.Size, .Range = FLevelPropertyRange{.Minimum = 0.01, .Maximum = 1000.}},
};

constexpr std::array AiAgentProperties{
    FLevelPropertyDescriptor{.Key = "stateTree", .Label = "State Tree", .Type = ELevelPropertyType::AssetReference, .Default = AiAgentDefaults.StateTree, .AssetKind = ELevelAssetKind::StateTree},
    FLevelPropertyDescriptor{.Key = "sightRange", .Label = "Sight Range", .Unit = ELevelPropertyUnit::Meters, .Default = AiAgentDefaults.SightRange, .Range = FLevelPropertyRange{.Minimum = 0.5, .Maximum = 200.}},
    FLevelPropertyDescriptor{.Key = "sightHalfAngle", .Label = "Sight Half Angle", .Unit = ELevelPropertyUnit::Radians, .Default = AiAgentDefaults.SightHalfAngle, .Range = FLevelPropertyRange{.Minimum = 0.05, .Maximum = 3.1415927}},
    FLevelPropertyDescriptor{.Key = "hearingRange", .Label = "Hearing Range", .Unit = ELevelPropertyUnit::Meters, .Default = AiAgentDefaults.HearingRange, .Range = FLevelPropertyRange{.Minimum = 0., .Maximum = 200.}},
    FLevelPropertyDescriptor{.Key = "walkSpeed", .Label = "Walk Speed", .Default = AiAgentDefaults.WalkSpeed, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 10.}},
    FLevelPropertyDescriptor{.Key = "runSpeed", .Label = "Run Speed", .Default = AiAgentDefaults.RunSpeed, .Range = FLevelPropertyRange{.Minimum = 0.1, .Maximum = 15.}},
    FLevelPropertyDescriptor{.Key = "attitude", .Label = "Attitude", .Type = ELevelPropertyType::AiAttitude, .Default = AiAgentDefaults.Attitude},
};

constexpr std::array GameplayGraphProperties{
    FLevelPropertyDescriptor{.Key = "graph", .Label = "Gameplay Graph", .Type = ELevelPropertyType::AssetReference, .Default = GameplayGraphDefaults.Graph, .AssetKind = ELevelAssetKind::GameplayGraph},
};

constexpr std::array Components{
    FLevelComponentDescriptor{.Type = ELevelComponentType::Transform, .TypeId = "Herta.Level.Transform", .SerializationKey = "transform", .Label = "Transform", .Properties = TransformProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::StaticMesh, .TypeId = "Herta.Level.StaticMesh", .SerializationKey = "staticMesh", .Label = "Static Mesh", .Properties = MeshProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::RigidBody, .TypeId = "Herta.Level.RigidBody", .SerializationKey = "body", .Label = "Rigid Body", .Properties = BodyProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Light, .TypeId = "Herta.Level.Light", .SerializationKey = "light", .Label = "Light", .Properties = LightProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::SkyAtmosphere, .TypeId = "Herta.Level.SkyAtmosphere", .SerializationKey = "skyAtmosphere", .Label = "Sky Atmosphere", .Properties = AtmosphereProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::HeightFog, .TypeId = "Herta.Level.HeightFog", .SerializationKey = "heightFog", .Label = "Height Fog", .Properties = FogProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::SoftBody, .TypeId = "Herta.Level.SoftBody", .SerializationKey = "softBody", .Label = "Soft Body", .Properties = SoftBodyProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Mover, .TypeId = "Herta.Level.Mover", .SerializationKey = "mover", .Label = "Mover", .Properties = MoverProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Trigger, .TypeId = "Herta.Level.Trigger", .SerializationKey = "trigger", .Label = "Trigger", .Properties = TriggerProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Constraint, .TypeId = "Herta.Level.Constraint", .SerializationKey = "constraint", .Label = "Constraint", .Properties = ConstraintProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Destructible, .TypeId = "Herta.Level.Destructible", .SerializationKey = "destructible", .Label = "Destructible", .Properties = DestructibleProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Character, .TypeId = "Herta.Level.Character", .SerializationKey = "character", .Label = "Character", .Properties = CharacterProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::AudioEmitter, .TypeId = "Herta.Level.AudioEmitter", .SerializationKey = "audioEmitter", .Label = "Audio Emitter", .Properties = AudioEmitterProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::AudioListener, .TypeId = "Herta.Level.AudioListener", .SerializationKey = "audioListener", .Label = "Audio Listener", .Properties = AudioListenerProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::ParticleEmitter, .TypeId = "Herta.Level.ParticleEmitter", .SerializationKey = "particleEmitter", .Label = "Particle Emitter", .Properties = ParticleEmitterProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::NavigationBounds, .TypeId = "Herta.Level.NavigationBounds", .SerializationKey = "navigationBounds", .Label = "Navigation Bounds", .Properties = NavigationBoundsProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::NavigationObstacle, .TypeId = "Herta.Level.NavigationObstacle", .SerializationKey = "navigationObstacle", .Label = "Navigation Obstacle", .Properties = NavigationObstacleProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::AiAgent, .TypeId = "Herta.Level.AiAgent", .SerializationKey = "aiAgent", .Label = "AI Agent", .Properties = AiAgentProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::GameplayGraph, .TypeId = "Herta.Level.GameplayGraph", .SerializationKey = "gameplayGraph", .Label = "Gameplay Graph", .Properties = GameplayGraphProperties},
    FLevelComponentDescriptor{.Type = ELevelComponentType::Player, .TypeId = "Herta.Level.Player", .SerializationKey = "player", .Label = "Player", .Properties = PlayerProperties},
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

std::vector<FAssetId> GetLevelAssetReferences(const std::span<const FLevelEntity> Entities, const ELevelAssetKind Kind)
{
	std::vector<FAssetId> Assets;
	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.Mesh && Kind == ELevelAssetKind::Model)
		{
			Assets.push_back(Entity.Mesh->Asset);
		}

		if (Entity.Mesh && Kind == ELevelAssetKind::Material)
		{
			Assets.insert(Assets.end(), Entity.Mesh->Materials.begin(), Entity.Mesh->Materials.end());
		}

		for (const FLevelComponentDescriptor& Component : Components)
		{
			for (const FLevelPropertyDescriptor& Property : Component.Properties)
			{
				if (Property.AssetKind != Kind || Property.Type != ELevelPropertyType::AssetReference)
				{
					continue;
				}

				const auto Value = GetLevelVisualProperty(Entity, Component.Type, Property.Key);
				if (const FAssetId* const Asset = Value ? std::get_if<FAssetId>(&*Value) : nullptr)
				{
					Assets.push_back(*Asset);
				}
			}
		}
	}

	std::erase_if(Assets, [](const FAssetId& Asset)
	{
		return !Asset.IsValid();
	});

	std::ranges::sort(Assets);
	const auto Repeats = std::ranges::unique(Assets);
	Assets.erase(Repeats.begin(), Repeats.end());
	return Assets;
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

	const auto Validate = [](const auto& Component, const auto& Validator) -> std::expected<void, FLevelError>
	{
		if (!Component)
		{
			return std::unexpected(FLevelError{"Visual property component is absent"});
		}

		return Validator(*Component);
	};

	const auto Valid = Type == ELevelComponentType::Light                ? Validate(Candidate.Light, ValidateLightComponent)
	                   : Type == ELevelComponentType::SkyAtmosphere      ? Validate(Candidate.SkyAtmosphere, ValidateSkyAtmosphereComponent)
	                   : Type == ELevelComponentType::SoftBody           ? Validate(Candidate.SoftBody, ValidateSoftBodyComponent)
	                   : Type == ELevelComponentType::Mover              ? Validate(Candidate.Mover, ValidateMoverComponent)
	                   : Type == ELevelComponentType::Trigger            ? Validate(Candidate.Trigger, ValidateTriggerComponent)
	                   : Type == ELevelComponentType::Constraint         ? Validate(Candidate.Constraint, ValidateConstraintComponent)
	                   : Type == ELevelComponentType::Destructible       ? Validate(Candidate.Destructible, ValidateDestructibleComponent)
	                   : Type == ELevelComponentType::Character          ? Validate(Candidate.Character, ValidateCharacterComponent)
	                   : Type == ELevelComponentType::AudioEmitter       ? Validate(Candidate.AudioEmitter, ValidateAudioEmitterComponent)
	                   : Type == ELevelComponentType::ParticleEmitter    ? Validate(Candidate.ParticleEmitter, ValidateParticleEmitterComponent)
	                   : Type == ELevelComponentType::NavigationBounds   ? Validate(Candidate.NavigationBounds, ValidateNavigationBoundsComponent)
	                   : Type == ELevelComponentType::NavigationObstacle ? Validate(Candidate.NavigationObstacle, ValidateNavigationObstacleComponent)
	                   : Type == ELevelComponentType::AiAgent            ? Validate(Candidate.AiAgent, ValidateAiAgentComponent)
	                   : Type == ELevelComponentType::AudioListener      ? Validate(Candidate.AudioListener, [](const FAudioListenerComponent&) -> std::expected<void, FLevelError>
	     {
        return {};
    })
	                   : Type == ELevelComponentType::GameplayGraph      ? Validate(Candidate.GameplayGraph, [](const FGameplayGraphComponent&) -> std::expected<void, FLevelError>
	     {
        return {};
    })
	                   : Type == ELevelComponentType::Player             ? Validate(Candidate.Player, [](const FPlayerComponent&) -> std::expected<void, FLevelError>
	            {
        return {};
    })
	                   : Type == ELevelComponentType::RigidBody          ? std::expected<void, FLevelError>{}
	                                                                     : Validate(Candidate.HeightFog, ValidateHeightFogComponent);
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
