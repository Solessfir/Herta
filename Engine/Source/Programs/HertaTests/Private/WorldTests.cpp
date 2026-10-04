#include "Herta/Scene/World.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
FSceneEntity MakeEntity(const std::uint64_t Value, std::string Name = "Entity")
{
	return {.Id = FObjectId{0, Value}, .Name = std::move(Name)};
}
}

TEST_CASE("Scene object IDs are strong canonical UUID values")
{
	constexpr FObjectId Id{0x0011223344556677, 0x8899aabbccddeeff};
	CHECK(Id.ToString() == "00112233-4455-6677-8899-aabbccddeeff");
	CHECK(FObjectId::Parse(Id.ToString()) == Id);
	CHECK_FALSE(FObjectId::Parse("00112233-4455-6677-8899-AABBCCDDEEFF").has_value());
	CHECK_FALSE(FObjectId::Parse("00000000-0000-0000-0000-000000000000").has_value());
	const FObjectId Generated = FObjectId::Generate();
	CHECK(Generated.IsValid());
	CHECK(FObjectId::Parse(Generated.ToString()) == Generated);
}

TEST_CASE("World creation and destruction occur at structural barriers")
{
	FWorld World;
	const auto Id = World.QueueCreateEntity(MakeEntity(1));
	REQUIRE(Id.has_value());
	CHECK(World.GetEntityCount() == 0);
	CHECK_FALSE(World.FindEntity(*Id).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	const auto Handle = World.FindEntity(*Id);
	REQUIRE(Handle.has_value());
	CHECK(World.GetEntityCount() == 1);
	CHECK(World.GetEntity(*Handle)->Name == "Entity");
	REQUIRE(World.QueueDestroyEntity(*Handle).has_value());
	CHECK_FALSE(World.QueueDestroyEntity(*Handle).has_value());
	CHECK(World.GetEntity(*Handle).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntityCount() == 0);
	CHECK_FALSE(World.GetEntity(*Handle).has_value());
	CHECK_FALSE(World.QueueDestroyEntity(*Handle).has_value());

	REQUIRE(World.QueueCreateEntity(MakeEntity(2)).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	CHECK_FALSE(World.GetEntity(*Handle).has_value());
	CHECK(World.FindEntity(FObjectId{0, 2}) != Handle);
}

TEST_CASE("World rejects foreign handles and replacement invalidates old handles")
{
	FWorld First;
	FWorld Second;
	const std::array Entities{MakeEntity(1)};
	REQUIRE(First.ReplaceEntities(Entities).has_value());
	REQUIRE(Second.ReplaceEntities(Entities).has_value());
	const FEntityId Handle = *First.FindEntity(Entities[0].Id);
	CHECK_FALSE(Second.GetEntity(Handle).has_value());
	CHECK_FALSE(Second.QueueDestroyEntity(Handle).has_value());
	CHECK_FALSE(Second.SetEntity(Handle, Entities[0]).has_value());
	CHECK_FALSE(Second.GetWorldMatrix(Handle).has_value());
	CHECK_FALSE(First.GetEntity({}).has_value());
	REQUIRE(First.ReplaceEntities(Entities).has_value());
	CHECK_FALSE(First.GetEntity(Handle).has_value());
	CHECK(First.FindEntity(Entities[0].Id)->World != Handle.World);
}

TEST_CASE("World handles remain stale after the private storage generation wraps")
{
	FWorld World;
	REQUIRE(World.QueueCreateEntity(MakeEntity(1)).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	const FEntityId Original = *World.FindEntity(FObjectId{0, 1});
	FEntityId Current = Original;
	bool bStorageGenerationWrapped = false;

	for (std::uint64_t Index = 2; Index <= 5000; ++Index)
	{
		REQUIRE(World.QueueDestroyEntity(Current).has_value());
		REQUIRE(World.FlushStructuralChanges().has_value());
		REQUIRE(World.QueueCreateEntity(MakeEntity(Index)).has_value());
		REQUIRE(World.FlushStructuralChanges().has_value());
		Current = *World.FindEntity(FObjectId{0, Index});
		REQUIRE(Current.Generation > Original.Generation);
		CHECK_FALSE(World.GetEntity(Original).has_value());
		CHECK_FALSE(World.QueueDestroyEntity(Original).has_value());
		CHECK_FALSE(World.SetEntity(Original, MakeEntity(1)).has_value());
		CHECK_FALSE(World.GetWorldMatrix(Original).has_value());

		if (Current.Value == Original.Value)
		{
			bStorageGenerationWrapped = true;
			CHECK(Current.Generation != Original.Generation);
		}
	}

	CHECK(bStorageGenerationWrapped);
	FEntityId InvalidGeneration = Current;
	InvalidGeneration.Generation = 0;
	CHECK_FALSE(World.GetEntity(InvalidGeneration).has_value());
	CHECK(World.GetEntity(Current).has_value());
}

TEST_CASE("World UUID lookups handle identifiers with identical halves")
{
	std::vector<FSceneEntity> Entities;
	Entities.reserve(5000);

	for (std::uint64_t Index = 1; Index <= 5000; ++Index)
	{
		FSceneEntity Entity = MakeEntity(Index);
		Entity.Id = FObjectId{Index, Index};
		Entity.Parent = Index == 1 ? FObjectId{} : FObjectId{Index - 1, Index - 1};
		Entities.push_back(std::move(Entity));
	}

	REQUIRE(ValidateSceneEntities(Entities).has_value());
	FWorld World;
	REQUIRE(World.ReplaceEntities(Entities).has_value());
	CHECK(World.SnapshotEntities() == Entities);

	for (const FSceneEntity& Entity : Entities)
	{
		const auto Handle = World.FindEntity(Entity.Id);
		REQUIRE(Handle.has_value());
		CHECK(World.GetEntity(*Handle) == Entity);
	}
}

TEST_CASE("World snapshots preserve components and have canonical object ordering")
{
	FWorld World;
	FSceneEntity Mesh = MakeEntity(3, "Mesh");
	Mesh.Mesh = FStaticMeshComponent{FAssetId{1, 2}};
	Mesh.BodyType = ESceneBodyType::Dynamic;
	Mesh.BodySettings = {
	    .MassKg = 42.f,
	    .Friction = 0.7f,
	    .Restitution = 0.3f,
	    .LinearDamping = 0.1f,
	    .AngularDamping = 0.2f,
	    .GravityScale = 2.f,
	};
	Mesh.Transform.Translation = FWorldPosition{1234567890.125, 2., 3.};
	Mesh.Transform.Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), 0.25f);
	Mesh.Transform.Scale = {1.f, 2.f, 3.f};
	const std::array Input{Mesh, MakeEntity(1), MakeEntity(2)};
	REQUIRE(World.ReplaceEntities(Input).has_value());
	const auto Snapshot = World.SnapshotEntities();
	REQUIRE(Snapshot.size() == 3);
	CHECK(Snapshot[0].Id == Input[1].Id);
	CHECK(Snapshot[1].Id == Input[2].Id);
	CHECK(Snapshot[2] == Mesh);

	FWorld Clone;
	REQUIRE(Clone.ReplaceEntities(Snapshot).has_value());
	CHECK(Clone.SnapshotEntities() == Snapshot);
	Mesh.Mesh.reset();
	Mesh.BodyType = ESceneBodyType::None;
	Mesh.BodySettings = {};
	REQUIRE(World.SetEntity(*World.FindEntity(Mesh.Id), Mesh).has_value());
	CHECK(World.GetEntity(*World.FindEntity(Mesh.Id)) == Mesh);
}

TEST_CASE("Rigid body settings validate ranges and update atomically")
{
	static_assert(FSceneRigidBodySettings{} == FSceneRigidBodySettings{});

	FSceneRigidBodySettings Settings{
	    .MassKg = 0.001f,
	    .Friction = 0.f,
	    .Restitution = 1.f,
	    .LinearDamping = 0.f,
	    .AngularDamping = 1.f,
	    .GravityScale = 10.f,
	};
	CHECK(ValidateSceneRigidBodySettings(Settings).has_value());

	Settings.MassKg = 1'000'000.f;
	CHECK(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings.MassKg = 0.f;
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings = {};
	Settings.Friction = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings = {};
	Settings.Restitution = 1.01f;
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings = {};
	Settings.LinearDamping = -0.01f;
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings = {};
	Settings.AngularDamping = std::numeric_limits<float>::infinity();
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());
	Settings = {};
	Settings.GravityScale = 10.01f;
	CHECK_FALSE(ValidateSceneRigidBodySettings(Settings).has_value());

	FSceneEntity Entity = MakeEntity(1);
	Entity.BodySettings.MassKg = 2.f;
	CHECK_FALSE(ValidateSceneEntities(std::span{&Entity, 1}).has_value());
	Entity.BodyType = ESceneBodyType::Dynamic;
	REQUIRE(ValidateSceneEntities(std::span{&Entity, 1}).has_value());

	FWorld World;
	REQUIRE(World.ReplaceEntities(std::span{&Entity, 1}));
	const FEntityId Handle = *World.FindEntity(Entity.Id);
	FSceneEntity Invalid = Entity;
	Invalid.BodySettings.GravityScale = -1.f;
	CHECK_FALSE(World.SetEntity(Handle, Invalid));
	CHECK(World.GetEntity(Handle) == Entity);
}

TEST_CASE("Rejected structural batches leave the live world unchanged")
{
	FWorld World;
	const std::array Initial{MakeEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial).has_value());
	const FEntityId Handle = *World.FindEntity(Initial[0].Id);
	FSceneEntity Orphan = MakeEntity(2);
	Orphan.Parent = FObjectId{0, 99};
	REQUIRE(World.QueueCreateEntity(Orphan).has_value());
	REQUIRE(World.QueueDestroyEntity(Handle).has_value());
	CHECK_FALSE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntity(Handle) == Initial[0]);
	CHECK(World.GetEntityCount() == 1);
	REQUIRE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntity(Handle).has_value());
	REQUIRE(World.QueueCreateEntity(MakeEntity(2)).has_value());
	CHECK_FALSE(World.QueueCreateEntity(MakeEntity(2)).has_value());
	CHECK_FALSE(World.QueueCreateEntity(MakeEntity(1)).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntity(Handle).has_value());
	CHECK(World.GetEntityCount() == 2);

	const std::array Duplicate{MakeEntity(3), MakeEntity(3)};
	CHECK_FALSE(World.ReplaceEntities(Duplicate).has_value());
	CHECK(World.GetEntity(Handle).has_value());
	CHECK(World.GetEntityCount() == 2);
}

TEST_CASE("Scene hierarchy composes local matrices and rejects unsafe parent edits")
{
	FWorld World;
	FSceneEntity Parent = MakeEntity(1, "Parent");
	Parent.Transform.Translation = FWorldPosition{1000000000., 2., 3.};
	Parent.Transform.Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), std::numbers::pi_v<float> / 2.f);
	Parent.Transform.Scale = {2.f, 2.f, 2.f};
	FSceneEntity Child = MakeEntity(2, "Child");
	Child.Parent = Parent.Id;
	Child.Transform.Translation = FWorldPosition{0., 0., 1.};
	// A child can be queued before its parent because the barrier validates the complete batch.
	REQUIRE(World.QueueCreateEntity(Child).has_value());
	REQUIRE(World.QueueCreateEntity(Parent).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	const FEntityId ParentHandle = *World.FindEntity(Parent.Id);
	const FEntityId ChildHandle = *World.FindEntity(Child.Id);
	const auto Matrix = World.GetWorldMatrix(ChildHandle);
	REQUIRE(Matrix.has_value());
	const FVector3d Position = Matrix->TransformPosition(FVector3d::Zero());
	CHECK(Position.X == doctest::Approx(1000000002.).epsilon(1e-12));
	CHECK(Position.Y == doctest::Approx(2.));
	CHECK(Position.Z == doctest::Approx(3.).epsilon(1e-6));

	Parent.Parent = Child.Id;
	CHECK_FALSE(World.SetEntity(ParentHandle, Parent).has_value());
	CHECK_FALSE(World.GetEntity(ParentHandle)->Parent.IsValid());
	Parent.Parent = {};
	Parent.Id = FObjectId{0, 9};
	CHECK_FALSE(World.SetEntity(ParentHandle, Parent).has_value());
	REQUIRE(World.QueueDestroyEntity(ParentHandle).has_value());
	CHECK_FALSE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntity(ParentHandle).has_value());
	CHECK(World.GetEntity(ChildHandle).has_value());
	REQUIRE(World.QueueDestroyEntity(ParentHandle).has_value());
	REQUIRE(World.QueueDestroyEntity(ChildHandle).has_value());
	REQUIRE(World.FlushStructuralChanges().has_value());
	CHECK(World.GetEntityCount() == 0);
}

TEST_CASE("Scene validation rejects cycles invalid names and invalid components")
{
	FSceneEntity Entity = MakeEntity(1);
	const auto IsValid = [&Entity]
	{
		return ValidateSceneEntities(std::span{&Entity, 1}).has_value();
	};

	CHECK(IsValid());
	Entity.Id = {};
	CHECK_FALSE(IsValid());
	Entity.Id = FObjectId{0, 1};
	Entity.Parent = Entity.Id;
	CHECK_FALSE(IsValid());
	Entity.Parent = FObjectId{0, 99};
	CHECK_FALSE(IsValid());
	Entity.Parent = {};
	Entity.Name = std::string("a\0b", 3);
	CHECK_FALSE(IsValid());
	Entity.Name = "\xc0\xaf";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xed\xa0\x80";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xf4\x90\x80\x80";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xe2\x82";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xc2\x80";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xc2\x9f";
	CHECK_FALSE(IsValid());
	Entity.Name = "\xc2\xa0";
	CHECK(IsValid());
	Entity.Name = std::string(1025, 'a');
	CHECK_FALSE(IsValid());
	Entity.Name = "\xe4\xb8\x96\xe7\x95\x8c";
	CHECK(IsValid());
	Entity.Transform.Translation.Meters.X = std::numeric_limits<double>::infinity();
	CHECK_FALSE(IsValid());
	Entity.Transform.Translation = {};
	Entity.Transform.Scale.X = 0.f;
	CHECK_FALSE(IsValid());
	Entity.Transform.Scale.X = -1.f;
	CHECK_FALSE(IsValid());
	Entity.Transform.Scale = FVector3::One();
	Entity.Transform.Rotation = {0.f, 0.f, 0.f, 0.f};
	CHECK_FALSE(IsValid());
	Entity.Transform.Rotation = {std::numeric_limits<float>::max(), 0.f, 0.f, 1.f};
	CHECK_FALSE(IsValid());
	Entity.Transform.Rotation = FQuaternion::Identity();
	Entity.Mesh = FStaticMeshComponent{};
	CHECK_FALSE(IsValid());
	Entity.Mesh.reset();
	Entity.BodyType = static_cast<ESceneBodyType>(255);
	CHECK_FALSE(IsValid());

	std::array Cycle{MakeEntity(1), MakeEntity(2), MakeEntity(3)};
	Cycle[0].Parent = Cycle[1].Id;
	Cycle[1].Parent = Cycle[2].Id;
	Cycle[2].Parent = Cycle[0].Id;
	CHECK_FALSE(ValidateSceneEntities(Cycle).has_value());
}

TEST_CASE("Scene validation handles deep hierarchies without recursion")
{
	std::vector<FSceneEntity> Entities;
	Entities.reserve(10000);

	for (std::uint64_t Index = 1; Index <= 10000; ++Index)
	{
		FSceneEntity Entity = MakeEntity(Index);
		Entity.Parent = Index == 10000 ? FObjectId{} : FObjectId{0, Index + 1};
		Entities.push_back(std::move(Entity));
	}

	CHECK(ValidateSceneEntities(Entities).has_value());
	Entities.back().Parent = Entities.front().Id;
	CHECK_FALSE(ValidateSceneEntities(Entities).has_value());
}

TEST_CASE("World entity patches preserve surviving handles and retire restored handles")
{
	FWorld World;
	const std::array Initial{MakeEntity(1, "Updated"), MakeEntity(2, "Removed"), MakeEntity(3, "Unchanged")};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId UpdatedHandle = *World.FindEntity(Initial[0].Id);
	const FEntityId RemovedHandle = *World.FindEntity(Initial[1].Id);
	const FEntityId UnchangedHandle = *World.FindEntity(Initial[2].Id);
	FSceneEntity Updated = Initial[0];
	Updated.Name = "Renamed";
	Updated.Transform.Translation = FWorldPosition{1., 2., 3.};
	Updated.Mesh = FStaticMeshComponent{FAssetId{7, 8}};
	Updated.BodyType = ESceneBodyType::Dynamic;
	const FSceneEntity Inserted = MakeEntity(4, "Inserted");
	const std::array Changes{
	    FSceneEntityChange{.Before = Initial[0], .After = Updated},
	    FSceneEntityChange{.Before = Initial[1]},
	    FSceneEntityChange{.After = Inserted},
	};

	REQUIRE(World.ApplyEntityChanges(Changes));
	CHECK(World.FindEntity(Updated.Id) == UpdatedHandle);
	CHECK(World.GetEntity(UpdatedHandle) == Updated);
	CHECK(World.FindEntity(Initial[2].Id) == UnchangedHandle);
	CHECK(World.GetEntity(UnchangedHandle) == Initial[2]);
	CHECK_FALSE(World.GetEntity(RemovedHandle).has_value());
	CHECK(World.GetEntity(*World.FindEntity(Inserted.Id)) == Inserted);
	const std::array Restore{FSceneEntityChange{.After = Initial[1]}};
	REQUIRE(World.ApplyEntityChanges(Restore));
	const FEntityId RestoredHandle = *World.FindEntity(Initial[1].Id);
	CHECK(RestoredHandle != RemovedHandle);
	CHECK_FALSE(World.GetEntity(RemovedHandle).has_value());
	CHECK(World.GetEntity(RestoredHandle) == Initial[1]);
}

TEST_CASE("World entity patches validate the complete hierarchy independent of change order")
{
	FWorld World;
	const FSceneEntity Parent = MakeEntity(1, "Parent");
	FSceneEntity Child = MakeEntity(2, "Child");
	Child.Parent = Parent.Id;
	const std::array Initial{Parent, Child};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId ChildHandle = *World.FindEntity(Child.Id);
	const FSceneEntity ReplacementParent = MakeEntity(3, "Replacement parent");
	FSceneEntity Reparented = Child;
	Reparented.Parent = ReplacementParent.Id;
	const std::array Changes{
	    FSceneEntityChange{.Before = Parent},
	    FSceneEntityChange{.Before = Child, .After = Reparented},
	    FSceneEntityChange{.After = ReplacementParent},
	};

	REQUIRE(World.ApplyEntityChanges(Changes));
	CHECK(World.GetEntity(ChildHandle) == Reparented);
	CHECK_FALSE(World.FindEntity(Parent.Id).has_value());
	CHECK(World.FindEntity(ReplacementParent.Id).has_value());
}

TEST_CASE("Rejected world entity patches never modify snapshots or handles")
{
	FWorld World;
	FSceneEntity Parent = MakeEntity(1, "Parent");
	FSceneEntity Child = MakeEntity(2, "Child");
	Child.Parent = Parent.Id;
	const std::array Initial{Parent, Child};
	REQUIRE(World.ReplaceEntities(Initial));
	const auto Snapshot = World.SnapshotEntities();
	const FEntityId ParentHandle = *World.FindEntity(Parent.Id);
	const FEntityId ChildHandle = *World.FindEntity(Child.Id);
	FSceneEntity Updated = Parent;
	Updated.Name = "Valid update";
	std::vector<FSceneEntityChange> Changes{{.Before = Parent, .After = Updated}};

	SUBCASE("empty change")
	{
		Changes.push_back({});
	}

	SUBCASE("invalid stable ID")
	{
		Changes.push_back({.After = FSceneEntity{}});
	}

	SUBCASE("changed stable ID")
	{
		Updated.Id = FObjectId{0, 3};
		Changes[0].After = Updated;
	}

	SUBCASE("duplicate patch ID")
	{
		Changes.push_back({.Before = Parent, .After = Updated});
	}

	SUBCASE("insertion already exists")
	{
		Changes.push_back({.After = Child});
	}

	SUBCASE("before snapshot is stale")
	{
		Changes[0].Before->Name = "Stale";
	}

	SUBCASE("before entity does not exist")
	{
		Changes.push_back({.Before = MakeEntity(3)});
	}

	SUBCASE("invalid final transform")
	{
		Changes.push_back({.Before = Child, .After = Child});
		Changes.back().After->Transform.Scale.X = 0.f;
	}

	SUBCASE("parent cycle")
	{
		Changes[0].After->Parent = Child.Id;
	}

	SUBCASE("surviving orphan")
	{
		Changes[0].After.reset();
	}

	CHECK_FALSE(World.ApplyEntityChanges(Changes).has_value());
	CHECK(World.SnapshotEntities() == Snapshot);
	CHECK(World.FindEntity(Parent.Id) == ParentHandle);
	CHECK(World.FindEntity(Child.Id) == ChildHandle);
}

TEST_CASE("World entity patches reject pending queues without consuming them")
{
	FWorld World;
	const FSceneEntity Entity = MakeEntity(1);
	const std::array Initial{Entity};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Entity.Id);
	FSceneEntity Updated = Entity;
	Updated.Name = "Updated";
	const std::array Changes{FSceneEntityChange{.Before = Entity, .After = Updated}};
	REQUIRE(World.QueueCreateEntity(MakeEntity(2)));
	CHECK_FALSE(World.ApplyEntityChanges(Changes).has_value());
	CHECK(World.GetEntity(Handle) == Entity);
	REQUIRE(World.FlushStructuralChanges());
	CHECK(World.FindEntity(FObjectId{0, 2}).has_value());
	REQUIRE(World.QueueDestroyEntity(Handle));
	CHECK_FALSE(World.ApplyEntityChanges({}).has_value());
	CHECK(World.GetEntity(Handle).has_value());
	REQUIRE(World.FlushStructuralChanges());
	CHECK_FALSE(World.GetEntity(Handle).has_value());
	REQUIRE(World.ApplyEntityChanges({}));
	CHECK(World.GetEntityCount() == 1);
}
}
