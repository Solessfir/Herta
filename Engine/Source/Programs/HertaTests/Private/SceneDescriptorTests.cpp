#include "Herta/Scene/SceneDescriptors.h"
#include "Herta/Scene/SceneSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

namespace Herta
{
TEST_CASE("Scene descriptors register stable built-in component and property identities")
{
	const auto Components = GetSceneComponentDescriptors();
	REQUIRE(Components.size() == 3);
	const std::array<std::string_view, 3> TypeIds{"Herta.Scene.Transform", "Herta.Scene.StaticMesh", "Herta.Scene.RigidBody"};
	const std::array<std::string_view, 3> Keys{"transform", "staticMesh", "body"};

	for (std::size_t Index = 0; Index < Components.size(); ++Index)
	{
		const auto& Component = Components[Index];
		CHECK(Component.TypeId == TypeIds[Index]);
		CHECK(Component.SerializationKey == Keys[Index]);
		CHECK(FindSceneComponentDescriptor(Component.TypeId) == &Component);
		CHECK(&GetSceneComponentDescriptor(Component.Type) == &Component);
		CHECK_FALSE(Component.Label.empty());

		for (const auto& Property : Component.Properties)
		{
			CHECK_FALSE(Property.Key.empty());
			CHECK_FALSE(Property.Label.empty());
			CHECK(FindScenePropertyDescriptor(Component, Property.Key) == &Property);
			CHECK(std::ranges::count(Component.Properties, Property.Key, &FScenePropertyDescriptor::Key) == 1);
		}

		CHECK(FindScenePropertyDescriptor(Component, "missing") == nullptr);
	}

	CHECK(FindSceneComponentDescriptor("missing") == nullptr);
}

TEST_CASE("Scene descriptors preserve typed storage defaults and physical units")
{
	const auto& Transform = GetSceneComponentDescriptor(ESceneComponentType::Transform);
	REQUIRE(Transform.Properties.size() == 3);
	CHECK(Transform.Properties[0].Type == EScenePropertyType::WorldPosition);
	CHECK(Transform.Properties[0].Unit == EScenePropertyUnit::Meters);
	CHECK(std::get<FWorldPosition>(Transform.Properties[0].Default) == FSceneTransform{}.Translation);
	CHECK(Transform.Properties[1].Type == EScenePropertyType::Quaternion);
	CHECK(std::get<FQuaternion>(Transform.Properties[1].Default) == FSceneTransform{}.Rotation);
	CHECK(Transform.Properties[2].Type == EScenePropertyType::Vector3);
	CHECK(std::get<FVector3>(Transform.Properties[2].Default) == FSceneTransform{}.Scale);
	CHECK_FALSE(Transform.Properties[0].Range);
	CHECK_FALSE(Transform.Properties[2].Range);

	const auto& Mesh = GetSceneComponentDescriptor(ESceneComponentType::StaticMesh);
	REQUIRE(Mesh.Properties.size() == 1);
	CHECK(Mesh.Properties[0].Type == EScenePropertyType::AssetReference);
	CHECK(std::get<FAssetId>(Mesh.Properties[0].Default) == FStaticMeshComponent{}.Asset);

	const auto& Body = GetSceneComponentDescriptor(ESceneComponentType::RigidBody);
	REQUIRE(Body.Properties.size() == 7);
	CHECK(std::get<ESceneBodyType>(Body.Properties[0].Default) == ESceneBodyType::Dynamic);
	const std::array Members{&FSceneRigidBodySettings::MassKg, &FSceneRigidBodySettings::Friction, &FSceneRigidBodySettings::Restitution, &FSceneRigidBodySettings::LinearDamping, &FSceneRigidBodySettings::AngularDamping, &FSceneRigidBodySettings::GravityScale};

	for (std::size_t Index = 0; Index < Members.size(); ++Index)
	{
		const auto& Property = Body.Properties[Index + 1];
		CHECK(Property.Type == EScenePropertyType::Float);
		const float Default = std::get<float>(Property.Default);
		CHECK(Default == FSceneRigidBodySettings{}.*Members[Index]);
		REQUIRE(Property.Range);
		CHECK(Property.Range->Minimum <= Default);
		CHECK(Property.Range->Maximum >= Default);
		FSceneRigidBodySettings Settings;
		Settings.*Members[Index] = static_cast<float>(Property.Range->Minimum);
		CHECK(ValidateSceneRigidBodySettings(Settings));
		Settings.*Members[Index] = static_cast<float>(Property.Range->Maximum);
		CHECK(ValidateSceneRigidBodySettings(Settings));
	}

	CHECK(Body.Properties[1].Unit == EScenePropertyUnit::Kilograms);
	CHECK(Body.Properties[4].Unit == EScenePropertyUnit::InverseSeconds);
	CHECK(Body.Properties[5].Unit == EScenePropertyUnit::InverseSeconds);
}

TEST_CASE("Scene serialization consumes every registered built-in property key without changing schema")
{
	const FSceneDocument Document{
	    .Id = FObjectId{1, 1},
	    .Name = "Descriptors",
	    .Entities = {FSceneEntity{.Id = FObjectId{1, 2}, .Name = "Cube", .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 3}}, .BodyType = ESceneBodyType::Dynamic}},
	};
	const auto Text = SerializeScene(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"engineSchemaVersion\": 2") != std::string::npos);

	for (const auto& Component : GetSceneComponentDescriptors())
	{
		CHECK(Text->find("\"" + std::string(Component.SerializationKey) + "\":") != std::string::npos);

		for (const auto& Property : Component.Properties)
		{
			const std::string Key = "\"" + std::string(Property.Key) + "\":";
			const auto Position = Text->find(Key);
			REQUIRE(Position != std::string::npos);
			std::string Corrupted = *Text;
			Corrupted.replace(Position, Key.size(), "\"unregistered\":");
			CHECK_FALSE(ParseScene(Corrupted));
		}
	}

	const auto Parsed = ParseScene(*Text);
	REQUIRE(Parsed);
	CHECK(Parsed->Entities == Document.Entities);
	const auto Canonical = SerializeScene(*Parsed);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
}
}
