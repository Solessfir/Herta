#include "Herta/Level/SystemScheduler.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>

namespace Herta
{
namespace
{
FLevelEntity MakeSystemEntity(const std::uint64_t Id)
{
	return {.Id = FObjectId{0, Id}, .Name = "Entity"};
}

std::expected<void, FLevelError> EmptySystem(FLevelSystemContext&)
{
	return {};
}
}

TEST_CASE("Level systems extract visual components and guard their structural changes")
{
	FWorld World;
	FLevelEntity Entity{.Id = FObjectId{1, 1}, .Name = "Light", .Light = FLightComponent{}, .SkyAtmosphere = FSkyAtmosphereComponent{}, .HeightFog = FHeightFogComponent{}};
	REQUIRE(World.ReplaceEntities(std::array{Entity}));
	FLevelSystemScheduler Scheduler(World);
	const ELevelComponent Visual = ELevelComponent::Light | ELevelComponent::SkyAtmosphere | ELevelComponent::HeightFog;

	REQUIRE(Scheduler.AddSystem({.Name = "ExtractVisual", .Phase = ELevelSystemPhase::Extract, .Access = {.Read = Visual, .Write = ELevelComponent::Light}}, [](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Rows = Context.Query({.Access = {.Read = Visual}, .Require = Visual});
		REQUIRE(Rows);
		REQUIRE(Rows->size() == 1);
		const auto& Row = Rows->front();
		CHECK_FALSE(Row.Transform);
		REQUIRE(Row.Light);
		REQUIRE(Row.SkyAtmosphere);
		REQUIRE(Row.HeightFog);
		FLightComponent Light = *Row.Light;
		Light.Intensity = 25.f;
		return Context.UpdateEntity(Row.Entity, {.Light = std::optional{Light}});
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Extract, 0.));
	CHECK(World.GetEntity(*World.FindEntity(Entity.Id))->Light->Intensity == 25.f);
	REQUIRE(Scheduler.Clear());

	REQUIRE(Scheduler.AddSystem({.Name = "RemoveUndeclared", .Access = {.Read = ELevelComponent::Light, .Write = ELevelComponent::Light}}, [](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Rows = Context.Query({.Access = {.Read = ELevelComponent::Light}, .Require = ELevelComponent::Light});
		REQUIRE(Rows);
		return Context.UpdateEntity(Rows->front().Entity, {.Light = std::optional<FLightComponent>{}});
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(World.GetEntity(*World.FindEntity(Entity.Id))->Light.has_value());
}

TEST_CASE("Level systems validate lifecycle and deterministic dependency ordering")
{
	FWorld World;
	FLevelSystemScheduler Scheduler(World);
	std::vector<std::string> Order;
	const auto Record = [&](const std::string& Name)
	{
		return [&, Name](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
		{
			CHECK(Context.GetDeltaSeconds() == doctest::Approx(0.25));
			Order.push_back(Name);
			CHECK_FALSE(Scheduler.Clear());
			CHECK_FALSE(Scheduler.RemoveSystem(Name));
			CHECK_FALSE(Scheduler.AddSystem({.Name = "Nested"}, EmptySystem));
			CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.25));
			return {};
		};
	};

	REQUIRE(Scheduler.AddSystem({.Name = "C", .After = {"B"}}, Record("C")));
	REQUIRE(Scheduler.AddSystem({.Name = "B", .After = {"A"}}, Record("B")));
	REQUIRE(Scheduler.AddSystem({.Name = "D"}, Record("D")));
	REQUIRE(Scheduler.AddSystem({.Name = "A"}, Record("A")));
	REQUIRE(Scheduler.AddSystem({.Name = "Fixed", .Phase = ELevelSystemPhase::FixedUpdate}, EmptySystem));
	CHECK(Scheduler.GetSystemCount() == 5);
	CHECK_FALSE(Scheduler.AddSystem({.Name = "A"}, EmptySystem));
	CHECK_FALSE(Scheduler.AddSystem({}, EmptySystem));
	CHECK_FALSE(Scheduler.AddSystem({.Name = "Invalid"}, {}));
	CHECK_FALSE(Scheduler.AddSystem({.Name = "Self", .After = {"Self"}}, EmptySystem));
	CHECK_FALSE(Scheduler.AddSystem({.Name = "Repeated", .After = {"A", "A"}}, EmptySystem));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, -1.));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, std::numeric_limits<double>::quiet_NaN()));
	CHECK_FALSE(Scheduler.RunPhase(static_cast<ELevelSystemPhase>(255), 0.));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.25));
	CHECK(Order == std::vector<std::string>{"A", "B", "C", "D"});
	Order.clear();
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.25));
	CHECK(Order == std::vector<std::string>{"A", "B", "C", "D"});
	REQUIRE(Scheduler.RemoveSystem("D"));
	CHECK_FALSE(Scheduler.RemoveSystem("Missing"));
	REQUIRE(Scheduler.Clear());
	CHECK(Scheduler.GetSystemCount() == 0);
}

TEST_CASE("Level dependency failures run no callbacks")
{
	FWorld World;
	FLevelSystemScheduler Scheduler(World);
	int Calls = 0;
	const auto Count = [&](FLevelSystemContext&) -> std::expected<void, FLevelError>
	{
		++Calls;
		return {};
	};

	SUBCASE("Missing dependency")
	{
		REQUIRE(Scheduler.AddSystem({.Name = "A", .After = {"Missing"}}, Count));
	}

	SUBCASE("Cross-phase dependency")
	{
		REQUIRE(Scheduler.AddSystem({.Name = "Fixed", .Phase = ELevelSystemPhase::FixedUpdate}, Count));
		REQUIRE(Scheduler.AddSystem({.Name = "A", .After = {"Fixed"}}, Count));
	}

	SUBCASE("Cycle")
	{
		REQUIRE(Scheduler.AddSystem({.Name = "A", .After = {"B"}}, Count));
		REQUIRE(Scheduler.AddSystem({.Name = "B", .After = {"A"}}, Count));
	}

	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(Calls == 0);
}

TEST_CASE("Level queries project declared components and expose validated writes in order")
{
	FWorld World;
	FLevelEntity Mesh = MakeSystemEntity(2);
	Mesh.Mesh = FStaticMeshComponent{FAssetId{1, 2}};
	Mesh.BodyType = ELevelBodyType::Dynamic;
	const std::array Entities{Mesh, MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Entities));
	const FEntityId MeshHandle = *World.FindEntity(Mesh.Id);
	FLevelSystemScheduler Scheduler(World);
	std::vector<FLevelQueryEntity> Retained;
	REQUIRE(Scheduler.AddSystem({.Name = "Move", .Access = {.Write = ELevelComponent::Transform}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Rows = Context.Query({.Access = {.Write = ELevelComponent::Transform}});
		REQUIRE(Rows);
		REQUIRE(Rows->size() == 2);
		CHECK((*Rows)[0].Object == FObjectId{0, 1});
		CHECK((*Rows)[1].Object == FObjectId{0, 2});
		CHECK_FALSE((*Rows)[0].Name);
		CHECK_FALSE((*Rows)[0].Parent);
		CHECK_FALSE((*Rows)[1].Mesh);
		CHECK_FALSE((*Rows)[1].RigidBody);
		Retained = *Rows;

		for (const FLevelQueryEntity& Row : *Rows)
		{
			FLevelTransform Transform = *Row.Transform;
			Transform.Translation.Meters.X += 10.;
			REQUIRE(Context.UpdateEntity(Row.Entity, {.Transform = Transform}));
		}

		return {};
	}));
	REQUIRE(Scheduler.AddSystem({.Name = "Inspect", .Access = {.Read = ELevelComponent::All}, .After = {"Move"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Meshes = Context.Query({.Access = {.Read = ELevelComponent::All}, .Require = ELevelComponent::StaticMesh | ELevelComponent::RigidBody});
		REQUIRE(Meshes);
		REQUIRE(Meshes->size() == 1);
		CHECK(Meshes->front().Entity == MeshHandle);
		CHECK(Meshes->front().Transform->Translation.Meters.X == doctest::Approx(10.));
		CHECK(Meshes->front().RigidBody->Type == ELevelBodyType::Dynamic);
		const auto Matrix = Context.GetWorldMatrix(MeshHandle);
		REQUIRE(Matrix);
		CHECK(Matrix->TransformPosition(FVector3d::Zero()).X == doctest::Approx(10.));
		const auto Empty = Context.Query({.Access = {.Read = ELevelComponent::StaticMesh}, .Exclude = ELevelComponent::StaticMesh});
		REQUIRE(Empty);
		CHECK(Empty->size() == 1);
		return {};
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 1.));
	CHECK(World.GetEntity(MeshHandle)->Transform.Translation.Meters.X == doctest::Approx(10.));
	CHECK(Retained.back().Transform->Translation.Meters.X == doctest::Approx(0.));
}

TEST_CASE("Level systems enforce access even when a callback ignores a rejected operation")
{
	FWorld World;
	const std::array Initial{MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Initial.front().Id);
	FLevelSystemScheduler Scheduler(World);
	int LaterCalls = 0;
	REQUIRE(Scheduler.AddSystem({.Name = "Denied", .Access = {.Read = ELevelComponent::Transform}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		SUBCASE("Write through read-only declaration")
		{
			CHECK_FALSE(Context.UpdateEntity(Handle, {.Transform = FLevelTransform{}}));
		}

		SUBCASE("Undeclared read")
		{
			CHECK_FALSE(Context.ReadEntity(Handle, {.Read = ELevelComponent::Name}));
		}

		SUBCASE("Undeclared query filter")
		{
			CHECK_FALSE(Context.Query({.Access = {.Read = ELevelComponent::Transform}, .Require = ELevelComponent::StaticMesh}));
		}

		SUBCASE("Undeclared event")
		{
			CHECK_FALSE(Context.PublishEvent(ELevelSystemPhase::Extract, {.Type = "Hit"}));
		}

		SUBCASE("Hierarchy-dependent read requires hierarchy access")
		{
			CHECK_FALSE(Context.GetWorldMatrix(Handle));
		}

		SUBCASE("Overlapping query filters")
		{
			CHECK_FALSE(Context.Query({.Access = {.Read = ELevelComponent::Transform}, .Require = ELevelComponent::Transform, .Exclude = ELevelComponent::Transform}));
		}

		SUBCASE("Undeclared event consumption")
		{
			CHECK_FALSE(Context.ReadEvents("Hit"));
		}

		SUBCASE("Unknown component bit")
		{
			CHECK_FALSE(Context.Query({.Access = {.Read = static_cast<ELevelComponent>(256)}}));
		}

		return {};
	}));
	REQUIRE(Scheduler.AddSystem({.Name = "Later", .After = {"Denied"}}, [&](FLevelSystemContext&) -> std::expected<void, FLevelError>
	{
		++LaterCalls;
		return {};
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(LaterCalls == 0);
	CHECK(World.SnapshotEntities() == std::vector<FLevelEntity>{Initial.front()});
	CHECK(Scheduler.GetBufferedEventCount() == 0);
}

TEST_CASE("Level systems cannot bypass their context through a captured world")
{
	FWorld World;
	const std::array Initial{MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Initial.front().Id);
	FLevelSystemScheduler Scheduler(World);
	FLevelSystemScheduler OtherScheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Access"}, [&](FLevelSystemContext&) -> std::expected<void, FLevelError>
	{
		CHECK_FALSE(World.GetEntity(Handle));
		CHECK(World.SnapshotEntities().empty());
		CHECK_FALSE(World.GetWorldMatrix(Handle));
		CHECK_FALSE(World.SetEntity(Handle, Initial.front()));
		CHECK_FALSE(World.ReplaceEntities(Initial));
		CHECK_FALSE(World.ApplyEntityChanges({}));
		CHECK_FALSE(World.QueueCreateEntity(MakeSystemEntity(2)));
		CHECK_FALSE(World.QueueDestroyEntity(Handle));
		CHECK_FALSE(World.FlushStructuralChanges());
		CHECK_FALSE(OtherScheduler.RunPhase(ELevelSystemPhase::Update, 0.));
		return {};
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(World.GetEntity(Handle));
	REQUIRE(World.QueueCreateEntity(MakeSystemEntity(2)));
	CHECK(World.HasPendingStructuralChanges());
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	REQUIRE(World.FlushStructuralChanges());
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
}

TEST_CASE("Level structural barriers atomically publish component membership and hierarchy")
{
	FWorld World;
	FLevelEntity Child = MakeSystemEntity(2);
	Child.Parent = FObjectId{0, 1};
	const std::array Initial{MakeSystemEntity(1), Child};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId ParentHandle = *World.FindEntity(Initial.front().Id);
	const FEntityId ChildHandle = *World.FindEntity(Child.Id);
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Structural", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		REQUIRE(Context.QueueCreateEntity(MakeSystemEntity(3)));
		REQUIRE(Context.UpdateEntity(ChildHandle, {.Parent = FObjectId{0, 3}, .Mesh = std::optional<FStaticMeshComponent>{FStaticMeshComponent{FAssetId{1, 2}}}, .RigidBody = FLevelRigidBodyComponent{.Type = ELevelBodyType::Dynamic}}));
		REQUIRE(Context.QueueDestroyEntity(ParentHandle));
		return {};
	}));
	REQUIRE(Scheduler.AddSystem({.Name = "Observe", .Access = {.Read = ELevelComponent::All}, .After = {"Structural"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Rows = Context.Query({.Access = {.Read = ELevelComponent::All}});
		REQUIRE(Rows);
		CHECK(Rows->size() == 2);
		const auto ExistingChild = Context.ReadEntity(ChildHandle, {.Read = ELevelComponent::All});
		REQUIRE(ExistingChild);
		CHECK(ExistingChild->Parent == FObjectId{0, 1});
		CHECK_FALSE(ExistingChild->Mesh);
		CHECK_FALSE(ExistingChild->RigidBody);
		REQUIRE(Context.UpdateEntity(ChildHandle, {}));
		return {};
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK_FALSE(World.GetEntity(ParentHandle));
	const auto Updated = World.GetEntity(ChildHandle);
	REQUIRE(Updated);
	CHECK(Updated->Parent == FObjectId{0, 3});
	CHECK(Updated->Mesh);
	CHECK(Updated->BodyType == ELevelBodyType::Dynamic);
	CHECK(World.FindEntity(FObjectId{0, 3}));
	REQUIRE(Scheduler.Clear());
	REQUIRE(Scheduler.AddSystem({.Name = "Remove", .Access = {.Write = ELevelComponent::StaticMesh | ELevelComponent::RigidBody}, .bStructuralChanges = true}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		return Context.UpdateEntity(ChildHandle, {.Mesh = std::optional<FStaticMeshComponent>{}, .RigidBody = FLevelRigidBodyComponent{}});
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK_FALSE(World.GetEntity(ChildHandle)->Mesh);
	CHECK(World.GetEntity(ChildHandle)->BodyType == ELevelBodyType::None);
}

TEST_CASE("Level rejected barriers preserve hierarchy and discard deferred events")
{
	FWorld World;
	FLevelEntity Child = MakeSystemEntity(2);
	Child.Parent = FObjectId{0, 1};
	const std::array Initial{MakeSystemEntity(1), Child};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId ParentHandle = *World.FindEntity(Initial.front().Id);
	const FEntityId ChildHandle = *World.FindEntity(Child.Id);
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Invalid", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true, .ProduceEvents = {"Changed"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		REQUIRE(Context.PublishEvent(ELevelSystemPhase::Extract, {.Type = "Changed"}));

		SUBCASE("Surviving child")
		{
			REQUIRE(Context.QueueDestroyEntity(ParentHandle));
		}

		SUBCASE("Hierarchy cycle")
		{
			REQUIRE(Context.UpdateEntity(ParentHandle, {.Parent = Child.Id}));
		}

		SUBCASE("Invalid deferred transform")
		{
			FLevelTransform Invalid;
			Invalid.Scale.X = -1.f;
			REQUIRE(Context.UpdateEntity(ChildHandle, {.Parent = FObjectId{}, .Transform = Invalid}));
		}

		SUBCASE("Invalid created entity")
		{
			FLevelEntity Invalid = MakeSystemEntity(3);
			Invalid.Parent = FObjectId{0, 99};
			REQUIRE(Context.QueueCreateEntity(Invalid));
		}

		return {};
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(World.SnapshotEntities() == std::vector<FLevelEntity>{Initial.begin(), Initial.end()});
	CHECK(World.GetEntity(ParentHandle));
	CHECK(World.GetEntity(ChildHandle));
	CHECK_FALSE(World.HasPendingStructuralChanges());
	CHECK(Scheduler.GetBufferedEventCount() == 0);
	REQUIRE(Scheduler.Clear());
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
}

TEST_CASE("Level stale handles and invalid mutable components fail without bypassing validation")
{
	FWorld World;
	FWorld Foreign;
	const std::array Initial{MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial));
	REQUIRE(Foreign.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Initial.front().Id);
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Invalid", .Access = {.Write = ELevelComponent::All}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		SUBCASE("Foreign read")
		{
			CHECK_FALSE(Context.ReadEntity(*Foreign.FindEntity(Initial.front().Id), {.Read = ELevelComponent::Transform}));
		}

		SUBCASE("Stale read")
		{
			CHECK_FALSE(Context.ReadEntity({}, {.Read = ELevelComponent::Transform}));
		}

		SUBCASE("Foreign write")
		{
			CHECK_FALSE(Context.UpdateEntity(*Foreign.FindEntity(Initial.front().Id), {.Name = "Foreign"}));
		}

		SUBCASE("Invalid transform")
		{
			FLevelTransform Invalid;
			Invalid.Scale.X = 0.f;
			CHECK_FALSE(Context.UpdateEntity(Handle, {.Transform = Invalid}));
		}

		SUBCASE("Membership without structural permission")
		{
			CHECK_FALSE(Context.UpdateEntity(Handle, {.Mesh = std::optional<FStaticMeshComponent>{FStaticMeshComponent{FAssetId{1, 2}}}}));
		}

		return {};
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(World.GetEntity(Handle) == Initial.front());
}

TEST_CASE("Level events broadcast only at their explicit consuming phase")
{
	FWorld World;
	FLevelSystemScheduler Scheduler(World);
	int UpdateEvents = 0;
	int ExtractEvents = 0;
	REQUIRE(Scheduler.AddSystem({.Name = "Producer", .ProduceEvents = {"Hit"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		REQUIRE(Context.PublishEvent(ELevelSystemPhase::Update, {.Type = "Hit", .Subject = FObjectId{0, 1}, .Payload = {std::byte{42}}}));
		return Context.PublishEvent(ELevelSystemPhase::Extract, {.Type = "Hit", .Subject = FObjectId{0, 2}});
	}));
	REQUIRE(Scheduler.AddSystem({.Name = "UpdateConsumer", .After = {"Producer"}, .ConsumeEvents = {"Hit"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Events = Context.ReadEvents("Hit");
		REQUIRE(Events);
		UpdateEvents += static_cast<int>(Events->size());

		if (!Events->empty())
		{
			CHECK(Events->front().Subject == FObjectId{0, 1});
			CHECK(Events->front().Payload == std::vector<std::byte>{std::byte{42}});
		}

		return {};
	}));
	REQUIRE(Scheduler.AddSystem({.Name = "ExtractConsumer", .Phase = ELevelSystemPhase::Extract, .ConsumeEvents = {"Hit"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Events = Context.ReadEvents("Hit");
		REQUIRE(Events);
		ExtractEvents += static_cast<int>(Events->size());
		return {};
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(UpdateEvents == 0);
	CHECK(ExtractEvents == 0);
	CHECK(Scheduler.GetBufferedEventCount() == 2);
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Extract, 0.));
	CHECK(ExtractEvents == 1);
	CHECK(Scheduler.GetBufferedEventCount() == 1);
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Extract, 0.));
	CHECK(ExtractEvents == 1);
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(UpdateEvents == 1);
	REQUIRE(Scheduler.Clear());
	CHECK(Scheduler.GetBufferedEventCount() == 0);
}

TEST_CASE("Level callback failure preserves immediate writes but discards deferred output")
{
	FWorld World;
	const std::array Initial{MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Initial.front().Id);
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Fail", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true, .ProduceEvents = {"Done"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		REQUIRE(Context.UpdateEntity(Handle, {.Name = "Applied"}));
		REQUIRE(Context.QueueCreateEntity(MakeSystemEntity(2)));
		REQUIRE(Context.PublishEvent(ELevelSystemPhase::Extract, {.Type = "Done"}));
		return std::unexpected(FLevelError{"Expected gameplay failure"});
	}));
	const auto Result = Scheduler.RunPhase(ELevelSystemPhase::Update, 0.);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message.find("Expected gameplay failure") != std::string::npos);
	CHECK(World.GetEntity(Handle)->Name == "Applied");
	CHECK_FALSE(World.FindEntity(FObjectId{0, 2}));
	CHECK(Scheduler.GetBufferedEventCount() == 0);
	REQUIRE(World.SetEntity(Handle, Initial.front()));
	REQUIRE(Scheduler.Clear());
}

TEST_CASE("Level event consumption is retried after phase failure and payloads are bounded")
{
	FWorld World;
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Publish", .ProduceEvents = {"Input"}}, [](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		return Context.PublishEvent(ELevelSystemPhase::Extract, {.Type = "Input"});
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	REQUIRE(Scheduler.RemoveSystem("Publish"));
	bool bFail = true;
	int Observations = 0;
	REQUIRE(Scheduler.AddSystem({.Name = "Consume", .Phase = ELevelSystemPhase::Extract, .ConsumeEvents = {"Input"}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		const auto Events = Context.ReadEvents("Input");
		REQUIRE(Events);
		Observations += static_cast<int>(Events->size());

		if (bFail)
		{
			return std::unexpected(FLevelError{"Retry"});
		}

		return {};
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Extract, 0.));
	CHECK(Scheduler.GetBufferedEventCount() == 1);
	bFail = false;
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Extract, 0.));
	CHECK(Observations == 2);
	CHECK(Scheduler.GetBufferedEventCount() == 0);
	REQUIRE(Scheduler.AddSystem({.Name = "Oversized", .ProduceEvents = {"Input"}}, [](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		FLevelEvent Event{.Type = "Input", .Payload = std::vector<std::byte>(65'537)};
		return Context.PublishEvent(ELevelSystemPhase::Extract, std::move(Event));
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(Scheduler.GetBufferedEventCount() == 0);
}

TEST_CASE("Level system handles remain stale after a structural barrier")
{
	FWorld World;
	const std::array Initial{MakeSystemEntity(1)};
	REQUIRE(World.ReplaceEntities(Initial));
	const FEntityId Handle = *World.FindEntity(Initial.front().Id);
	FLevelSystemScheduler Scheduler(World);
	REQUIRE(Scheduler.AddSystem({.Name = "Destroy", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		return Context.QueueDestroyEntity(Handle);
	}));
	REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	REQUIRE(Scheduler.Clear());
	REQUIRE(World.ReplaceEntities(Initial));
	REQUIRE(Scheduler.AddSystem({.Name = "Stale", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
	{
		CHECK_FALSE(Context.ReadEntity(Handle, {.Read = ELevelComponent::Transform}));
		CHECK_FALSE(Context.GetWorldMatrix(Handle));
		CHECK_FALSE(Context.UpdateEntity(Handle, {.Name = "Wrong generation"}));
		CHECK_FALSE(Context.QueueDestroyEntity(Handle));
		return {};
	}));
	CHECK_FALSE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
	CHECK(World.GetEntity(*World.FindEntity(Initial.front().Id)) == Initial.front());
}

TEST_CASE("Level system queries and barriers support the scaling checkpoints")
{
	for (const std::size_t Count : {1000u, 5000u, 10000u})
	{
		CAPTURE(Count);
		std::vector<FLevelEntity> Initial;
		Initial.reserve(Count);

		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			Initial.push_back(MakeSystemEntity(Index + 1));
		}

		FWorld World;
		REQUIRE(World.ReplaceEntities(Initial));
		FLevelSystemScheduler Scheduler(World);
		std::vector<FEntityId> Handles;
		REQUIRE(Scheduler.AddSystem({.Name = "Move", .Access = {.Write = ELevelComponent::Transform}}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
		{
			const auto Rows = Context.Query({.Access = {.Write = ELevelComponent::Transform}});
			REQUIRE(Rows);
			REQUIRE(Rows->size() == Count);
			Handles.clear();

			for (const FLevelQueryEntity& Row : *Rows)
			{
				FLevelTransform Transform = *Row.Transform;
				Transform.Translation.Meters.Z += Context.GetDeltaSeconds();
				REQUIRE(Context.UpdateEntity(Row.Entity, {.Transform = Transform}));
				Handles.push_back(Row.Entity);
			}

			return {};
		}));
		REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.5));
		REQUIRE(Scheduler.RemoveSystem("Move"));
		REQUIRE(Scheduler.AddSystem({.Name = "Destroy", .Access = {.Write = ELevelComponent::All}, .bStructuralChanges = true}, [&](FLevelSystemContext& Context) -> std::expected<void, FLevelError>
		{
			for (const FEntityId Handle : Handles)
			{
				const auto Row = Context.ReadEntity(Handle, {.Read = ELevelComponent::Transform});
				REQUIRE(Row);
				CHECK(Row->Transform->Translation.Meters.Z == doctest::Approx(0.5));
				REQUIRE(Context.QueueDestroyEntity(Handle));
			}

			return {};
		}));
		REQUIRE(Scheduler.RunPhase(ELevelSystemPhase::Update, 0.));
		CHECK(World.GetEntityCount() == 0);
		CHECK_FALSE(World.GetEntity(Handles.front()));
		REQUIRE(Scheduler.Clear());
		REQUIRE(World.ReplaceEntities(Initial));
		CHECK_FALSE(World.GetEntity(Handles.front()));
	}
}
}
