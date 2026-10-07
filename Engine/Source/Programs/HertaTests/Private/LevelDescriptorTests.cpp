#include "Herta/Level/LevelDescriptors.h"
#include "Herta/Level/LevelSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

namespace Herta
{
TEST_CASE("Level descriptors register stable built-in component and property identities")
{
	const auto Components = GetLevelComponentDescriptors();
	REQUIRE(Components.size() == 7);
	const std::array<std::string_view, 7> TypeIds{"Herta.Level.Transform", "Herta.Level.StaticMesh", "Herta.Level.RigidBody", "Herta.Level.Light", "Herta.Level.SkyAtmosphere", "Herta.Level.HeightFog", "Herta.Level.SoftBody"};
	const std::array<std::string_view, 7> Keys{"transform", "staticMesh", "body", "light", "skyAtmosphere", "heightFog", "softBody"};

	for (std::size_t Index = 0; Index < Components.size(); ++Index)
	{
		const auto& Component = Components[Index];
		CHECK(Component.TypeId == TypeIds[Index]);
		CHECK(Component.SerializationKey == Keys[Index]);
		CHECK(FindLevelComponentDescriptor(Component.TypeId) == &Component);
		CHECK(&GetLevelComponentDescriptor(Component.Type) == &Component);
		CHECK_FALSE(Component.Label.empty());

		for (const auto& Property : Component.Properties)
		{
			CHECK_FALSE(Property.Key.empty());
			CHECK_FALSE(Property.Label.empty());
			CHECK(FindLevelPropertyDescriptor(Component, Property.Key) == &Property);
			CHECK(std::ranges::count(Component.Properties, Property.Key, &FLevelPropertyDescriptor::Key) == 1);
		}

		CHECK(FindLevelPropertyDescriptor(Component, "missing") == nullptr);
	}

	CHECK(FindLevelComponentDescriptor("missing") == nullptr);
}

TEST_CASE("Visual descriptor property access preserves types and rejects invalid values atomically")
{
	FLevelEntity Entity{.Id = FObjectId{1, 1}, .Light = FLightComponent{}, .SkyAtmosphere = FSkyAtmosphereComponent{}, .HeightFog = FHeightFogComponent{}, .SoftBody = FSoftBodyComponent{}};
	for (const ELevelComponentType Type : {ELevelComponentType::Light, ELevelComponentType::SkyAtmosphere, ELevelComponentType::HeightFog, ELevelComponentType::SoftBody})
	{
		const auto& Descriptor = GetLevelComponentDescriptor(Type);
		for (const auto& Property : Descriptor.Properties)
		{
			const auto Value = GetLevelVisualProperty(Entity, Type, Property.Key);
			REQUIRE(Value);
			CHECK(*Value == Property.Default);
			CHECK(SetLevelVisualProperty(Entity, Type, Property.Key, Property.Default));
		}
	}

	const auto Before = Entity;
	CHECK_FALSE(SetLevelVisualProperty(Entity, ELevelComponentType::Light, "intensity", true));
	CHECK_FALSE(SetLevelVisualProperty(Entity, ELevelComponentType::HeightFog, "density", -1.f));
	CHECK_FALSE(SetLevelVisualProperty(Entity, ELevelComponentType::SoftBody, "stiffness", 1.5f));
	CHECK_FALSE(SetLevelVisualProperty(Entity, ELevelComponentType::SoftBody, "shape", static_cast<ESoftBodyShape>(9)));
	CHECK_FALSE(SetLevelVisualProperty(Entity, ELevelComponentType::Light, "missing", 1.f));
	CHECK(Entity == Before);
	CHECK_FALSE(GetLevelVisualProperty(Entity, ELevelComponentType::Light, "missing"));
}

TEST_CASE("Level descriptors preserve typed storage defaults and physical units")
{
	const auto& Transform = GetLevelComponentDescriptor(ELevelComponentType::Transform);
	REQUIRE(Transform.Properties.size() == 3);
	CHECK(Transform.Properties[0].Type == ELevelPropertyType::WorldPosition);
	CHECK(Transform.Properties[0].Unit == ELevelPropertyUnit::Meters);
	CHECK(std::get<FWorldPosition>(Transform.Properties[0].Default) == FLevelTransform{}.Translation);
	CHECK(Transform.Properties[1].Type == ELevelPropertyType::Quaternion);
	CHECK(std::get<FQuaternion>(Transform.Properties[1].Default) == FLevelTransform{}.Rotation);
	CHECK(Transform.Properties[2].Type == ELevelPropertyType::Vector3);
	CHECK(std::get<FVector3>(Transform.Properties[2].Default) == FLevelTransform{}.Scale);
	CHECK_FALSE(Transform.Properties[0].Range);
	CHECK_FALSE(Transform.Properties[2].Range);

	const auto& Mesh = GetLevelComponentDescriptor(ELevelComponentType::StaticMesh);
	REQUIRE(Mesh.Properties.size() == 2);
	CHECK(Mesh.Properties[0].Type == ELevelPropertyType::AssetReference);
	CHECK(std::get<FAssetId>(Mesh.Properties[0].Default) == FStaticMeshComponent{}.Asset);

	const auto& Body = GetLevelComponentDescriptor(ELevelComponentType::RigidBody);
	REQUIRE(Body.Properties.size() == 7);
	CHECK(std::get<ELevelBodyType>(Body.Properties[0].Default) == ELevelBodyType::Dynamic);
	const std::array Members{&FLevelRigidBodySettings::MassKg, &FLevelRigidBodySettings::Friction, &FLevelRigidBodySettings::Restitution, &FLevelRigidBodySettings::LinearDamping, &FLevelRigidBodySettings::AngularDamping, &FLevelRigidBodySettings::GravityScale};

	for (std::size_t Index = 0; Index < Members.size(); ++Index)
	{
		const auto& Property = Body.Properties[Index + 1];
		CHECK(Property.Type == ELevelPropertyType::Float);
		const float Default = std::get<float>(Property.Default);
		CHECK(Default == FLevelRigidBodySettings{}.*Members[Index]);
		REQUIRE(Property.Range);
		CHECK(Property.Range->Minimum <= Default);
		CHECK(Property.Range->Maximum >= Default);
		FLevelRigidBodySettings Settings;
		Settings.*Members[Index] = static_cast<float>(Property.Range->Minimum);
		CHECK(ValidateLevelRigidBodySettings(Settings));
		Settings.*Members[Index] = static_cast<float>(Property.Range->Maximum);
		CHECK(ValidateLevelRigidBodySettings(Settings));
	}

	CHECK(Body.Properties[1].Unit == ELevelPropertyUnit::Kilograms);
	CHECK(Body.Properties[4].Unit == ELevelPropertyUnit::InverseSeconds);
	CHECK(Body.Properties[5].Unit == ELevelPropertyUnit::InverseSeconds);
}

TEST_CASE("Level serialization consumes every registered built-in property key")
{
	const FLevelDocument Document{
	    .Id = FObjectId{1, 1},
	    .Name = "Descriptors",
	    .Entities = {FLevelEntity{.Id = FObjectId{1, 2}, .Name = "Cube", .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 3}}, .BodyType = ELevelBodyType::Dynamic, .Light = FLightComponent{}, .SkyAtmosphere = FSkyAtmosphereComponent{}, .HeightFog = FHeightFogComponent{}, .SoftBody = FSoftBodyComponent{}}},
	};
	const auto Text = SerializeLevel(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"engineSchemaVersion\": 5") != std::string::npos);

	for (const auto& Component : GetLevelComponentDescriptors())
	{
		CHECK(Text->find("\"" + std::string(Component.SerializationKey) + "\":") != std::string::npos);

		for (const auto& Property : Component.Properties)
		{
			const std::string Key = "\"" + std::string(Property.Key) + "\":";
			const auto Position = Text->find(Key);
			REQUIRE(Position != std::string::npos);
			std::string Corrupted = *Text;
			Corrupted.replace(Position, Key.size(), "\"unregistered\":");
			CHECK_FALSE(ParseLevel(Corrupted));
		}
	}

	const auto Parsed = ParseLevel(*Text);
	REQUIRE(Parsed);
	CHECK(Parsed->Entities == Document.Entities);
	const auto Canonical = SerializeLevel(*Parsed);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
}
}
