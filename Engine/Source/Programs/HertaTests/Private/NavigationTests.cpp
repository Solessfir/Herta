#include "Herta/NavigationBuilder/NavigationBuilder.h"
#include "Herta/Navigation/NavigationWorld.h"
#include "Herta/Tasks/TaskSystem.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <set>

namespace Herta
{
namespace
{
// Adds a box whose faces wind counter-clockwise seen from outside, as Recast expects of walkable tops.
void AddBox(FNavigationGeometry& Geometry, const FVector3& Minimum, const FVector3& Maximum)
{
	const auto Corner = [&](const int Index)
	{
		return FVector3{(Index & 1) != 0 ? Maximum.X : Minimum.X, (Index & 2) != 0 ? Maximum.Y : Minimum.Y, (Index & 4) != 0 ? Maximum.Z : Minimum.Z};
	};

	constexpr std::array<std::array<int, 4>, 6> Faces{{{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}}};
	const FVector3 Center = (Minimum + Maximum) * 0.5f;
	for (const std::array<int, 4>& Face : Faces)
	{
		const auto Base = static_cast<std::uint32_t>(Geometry.Vertices.size());
		for (const int Index : Face)
		{
			Geometry.Vertices.push_back(Corner(Index));
		}

		const FVector3 Normal = (Geometry.Vertices[Base + 1] - Geometry.Vertices[Base]).Cross(Geometry.Vertices[Base + 2] - Geometry.Vertices[Base]);
		const bool bOutward = Normal.Dot(Geometry.Vertices[Base] - Center) > 0.f;
		Geometry.Indices.insert(Geometry.Indices.end(), bOutward ? std::initializer_list<std::uint32_t>{Base, Base + 1, Base + 2, Base, Base + 2, Base + 3} : std::initializer_list<std::uint32_t>{Base, Base + 2, Base + 1, Base, Base + 3, Base + 2});
	}
}

// A 20 m floor split by a wall with a gap at its +Z end, so going from -X to +X walks around it.
[[nodiscard]] FNavigationGeometry MakeWalledFloor()
{
	FNavigationGeometry Geometry;
	AddBox(Geometry, {-10.f, -0.5f, -10.f}, {10.f, 0.f, 10.f});
	AddBox(Geometry, {-0.25f, 0.f, -10.f}, {0.25f, 2.f, 6.f});
	return Geometry;
}

constexpr std::array FloorBox{FNavigationBox{.Minimum = {-10.f, -1.f, -10.f}, .Maximum = {10.f, 3.f, 10.f}}};

[[nodiscard]] float PathLength(const FNavigationPath& Path)
{
	float Length = 0.f;
	for (std::size_t Index = 1; Index < Path.Points.size(); ++Index)
	{
		Length += (Path.Points[Index] - Path.Points[Index - 1]).Length();
	}

	return Length;
}

[[nodiscard]] std::unique_ptr<FNavigationWorld> MakeWorld(const FNavigationWorldSettings& Settings = {})
{
	auto World = FNavigationWorld::Create({}, Settings);
	REQUIRE(World);
	return std::move(*World);
}
}

TEST_CASE("Navigation builds deterministic tiles that round trip and path around walls")
{
	const FNavigationGeometry Geometry = MakeWalledFloor();
	const auto Data = BuildNavigationData(Geometry, FloorBox);
	REQUIRE(Data);
	CHECK(!Data->Layers.empty());
	CHECK(Data->Source == HashNavigationSource(Geometry, FloorBox, {}));

	const auto Again = BuildNavigationData(Geometry, FloorBox);
	REQUIRE(Again);
	const auto Serialize = [](const FNavigationData& Built)
	{
		return SerializeNavigationFile({.Base = Built});
	};

	const std::vector<std::byte> Bytes = Serialize(*Data);
	CHECK(Serialize(*Again) == Bytes);

	auto Tasks = FTaskSystem::Create({.CpuWorkerCount = 4});
	REQUIRE(Tasks);
	const auto Parallel = BuildNavigationData(Geometry, FloorBox, {}, Tasks->get());
	REQUIRE(Parallel);
	CHECK(Serialize(*Parallel) == Bytes);

	const FNavigationFile File{.Base = *Data, .Patches = {{.Key = {1, 2}, .Data = *Again}}};
	const auto Parsed = ParseNavigationFile(SerializeNavigationFile(File));
	REQUIRE(Parsed);
	CHECK(*Parsed == File);

	auto World = MakeWorld();
	REQUIRE(World->AddData(Parsed->Base));
	const auto Path = World->FindPath({-5.f, 0.f, 0.f}, {5.f, 0.f, 0.f});
	REQUIRE(Path);
	CHECK(!Path->bPartial);
	REQUIRE(Path->Points.size() >= 3);
	CHECK(Path->Points.front().IsNearlyEqual({-5.f, 0.f, 0.f}, 0.3f));
	CHECK(Path->Points.back().IsNearlyEqual({5.f, 0.f, 0.f}, 0.3f));
	// Around the wall's end at z = 6, rather than the 10 m straight line through it.
	CHECK(PathLength(*Path) > 15.f);
	for (const FVector3& Point : Path->Points)
	{
		CHECK((std::abs(Point.X) > 0.4f || Point.Z > 6.f));
	}

	CHECK(World->GetStats().Polygons > 0);
	std::vector<FVector3> Triangles;
	World->GetDebugTriangles(Triangles);
	CHECK(!Triangles.empty());
	CHECK(Triangles.size() % 3 == 0);
}

TEST_CASE("Navigation data rejects truncation, corruption, and other settings")
{
	const auto Data = BuildNavigationData(MakeWalledFloor(), FloorBox);
	REQUIRE(Data);
	std::vector<std::byte> Bytes = SerializeNavigationFile({.Base = *Data});
	CHECK(!ParseNavigationFile(std::span(Bytes).first(Bytes.size() - 1)));
	CHECK(!ParseNavigationFile(std::span(Bytes).first(12)));

	std::vector<std::byte> Trailing = Bytes;
	Trailing.push_back(std::byte{0});
	CHECK(!ParseNavigationFile(Trailing));

	std::vector<std::byte> Corrupt = Bytes;
	Corrupt[0] = std::byte{0};
	CHECK(!ParseNavigationFile(Corrupt));

	// A layer whose tile cache header no longer matches its tile.
	std::vector<std::byte> Moved = Bytes;
	const auto Layer = std::ranges::search(Moved, Data->Layers.front().Data);
	REQUIRE(!Layer.empty());
	Layer.front() = std::byte{0};
	CHECK(!ParseNavigationFile(Moved));

	auto World = FNavigationWorld::Create({.CellSize = 0.2f});
	REQUIRE(World);
	CHECK(!(*World)->AddData(*Data));

	FNavigationGeometry Broken = MakeWalledFloor();
	Broken.Indices.push_back(9999);
	CHECK(!BuildNavigationData(Broken, FloorBox));
	CHECK(!BuildNavigationData(MakeWalledFloor(), FloorBox, {.TileCells = 300}));
}

TEST_CASE("Navigation patches stream in over a level's tiles and join it across the seam")
{
	const float TileSize = FNavigationSettings{}.GetTileSize();
	// The level's floor two tiles long, and a region's floor two more tiles east of it. Built on its own, the level's floor erodes back from
	// its east edge; the region's patch is built with the level's floor around it, so it replaces the tile beside the seam as well.
	FNavigationGeometry Level;
	AddBox(Level, {0.f, -0.5f, 0.f}, {TileSize * 2.f, 0.f, TileSize});
	FNavigationGeometry Joined = Level;
	AddBox(Joined, {TileSize * 2.f, -0.5f, 0.f}, {TileSize * 4.f, 0.f, TileSize});
	const std::array LevelBox{FNavigationBox{.Minimum = {0.f, -1.f, 0.f}, .Maximum = {TileSize * 2.f, 2.f, TileSize}}};
	const std::array RegionBox{FNavigationBox{.Minimum = {TileSize * 2.f - 1.f, -1.f, 0.f}, .Maximum = {TileSize * 4.f, 2.f, TileSize}}};
	const auto Base = BuildNavigationData(Level, LevelBox);
	const auto Patch = BuildNavigationData(Joined, RegionBox);
	REQUIRE(Base);
	REQUIRE(Patch);

	auto World = MakeWorld();
	REQUIRE(World->AddData(*Base));
	const FVector3 Start{1.f, 0.f, TileSize * 0.5f};
	const FVector3 Goal{TileSize * 3.5f, 0.f, TileSize * 0.5f};
	const FVector3 Near{TileSize * 1.5f, 0.f, TileSize * 0.5f};
	CHECK(!World->FindPath(Start, Goal));

	const auto Region = World->AddData(*Patch);
	REQUIRE(Region);
	const auto Path = World->FindPath(Start, Goal);
	REQUIRE(Path);
	CHECK(!Path->bPartial);
	CHECK(Path->Points.back().IsNearlyEqual(Goal, 0.3f));
	CHECK(World->GetStats().Sources == 2);

	World->RemoveData(*Region);
	CHECK(!World->FindPath(Start, Goal));
	const auto Restored = World->FindPath(Start, Near);
	REQUIRE(Restored);
	CHECK(!Restored->bPartial);
	CHECK(World->GetStats().Sources == 1);
	CHECK(!World->AddData(FNavigationData{.Settings = {.CellSize = 0.2f}}));
}

TEST_CASE("Navigation obstacles cut their tiles and apply to tiles that load later")
{
	// A 2 m corridor down Z, which a crate across it blocks.
	FNavigationGeometry Geometry;
	AddBox(Geometry, {-1.f, -0.5f, -8.f}, {1.f, 0.f, 8.f});
	const std::array Box{FNavigationBox{.Minimum = {-2.f, -1.f, -8.f}, .Maximum = {2.f, 2.f, 8.f}}};
	const auto Data = BuildNavigationData(Geometry, Box);
	REQUIRE(Data);

	auto World = MakeWorld();
	const auto Obstacle = World->AddObstacle({.Center = {0.f, 0.5f, 0.f}, .HalfExtents = {1.5f, 0.5f, 0.5f}});
	REQUIRE(Obstacle);
	REQUIRE(World->AddData(*Data));
	for (int Update = 0; Update < 8; ++Update)
	{
		World->Update(1.f / 60.f);
	}

	CHECK(World->GetStats().PendingObstacles == 0);
	const auto Blocked = World->FindPath({0.f, 0.f, -6.f}, {0.f, 0.f, 6.f});
	REQUIRE(Blocked);
	CHECK(Blocked->bPartial);

	World->RemoveObstacle(*Obstacle);
	for (int Update = 0; Update < 8; ++Update)
	{
		World->Update(1.f / 60.f);
	}

	const auto Open = World->FindPath({0.f, 0.f, -6.f}, {0.f, 0.f, 6.f});
	REQUIRE(Open);
	CHECK(!Open->bPartial);
	CHECK(World->GetStats().Obstacles == 0);
	CHECK(!World->AddObstacle({.HalfExtents = {0.f, 1.f, 1.f}}));
}

TEST_CASE("Navigation path requests search over several updates and can be cancelled")
{
	const auto Data = BuildNavigationData(MakeWalledFloor(), FloorBox);
	REQUIRE(Data);
	auto World = MakeWorld({.PathIterationsPerUpdate = 4});
	REQUIRE(World->AddData(*Data));

	const auto Request = World->RequestPath({-5.f, 0.f, 0.f}, {5.f, 0.f, 0.f});
	const auto Cancelled = World->RequestPath({-5.f, 0.f, 0.f}, {5.f, 0.f, -5.f});
	const auto Off = World->RequestPath({-5.f, 0.f, 0.f}, {50.f, 0.f, 0.f});
	REQUIRE(Request);
	REQUIRE(Cancelled);
	REQUIRE(Off);
	World->CancelPath(*Cancelled);
	CHECK(World->TakePathResult(*Request).State == ENavigationPathState::Pending);

	int Updates = 0;
	FNavigationPathResult Result;
	while ((Result = World->TakePathResult(*Request)).State == ENavigationPathState::Pending && Updates < 1000)
	{
		World->Update(1.f / 60.f);
		++Updates;
	}

	CHECK(Updates > 1);
	REQUIRE(Result.State == ENavigationPathState::Succeeded);
	const auto Direct = World->FindPath({-5.f, 0.f, 0.f}, {5.f, 0.f, 0.f});
	REQUIRE(Direct);
	CHECK(Result.Path.Points == Direct->Points);
	CHECK(World->TakePathResult(*Cancelled).State == ENavigationPathState::Failed);

	World->Update(1.f / 60.f);
	CHECK(World->TakePathResult(*Off).State == ENavigationPathState::Failed);
	CHECK(World->GetStats().PathRequests == 0);
}

TEST_CASE("Navigation crowds steer agents past each other and rejoin streamed navigation")
{
	FNavigationGeometry Geometry;
	AddBox(Geometry, {-10.f, -0.5f, -10.f}, {10.f, 0.f, 10.f});
	const auto Data = BuildNavigationData(Geometry, FloorBox);
	REQUIRE(Data);

	// Moves agents by the crowd's velocity, as a physics character would, and reports where they end up.
	const auto Run = [](FNavigationWorld& World, const std::span<const FNavigationAgentId> Agents, const int Steps)
	{
		constexpr float Step = 1.f / 60.f;
		for (int Index = 0; Index < Steps; ++Index)
		{
			for (const FNavigationAgentId Agent : Agents)
			{
				const auto State = World.GetAgent(Agent);
				if (State && State->bOnNavigation)
				{
					World.SetAgentPosition(Agent, State->Position + State->DesiredVelocity * Step, State->DesiredVelocity);
				}
			}

			World.Update(Step);
		}
	};

	auto World = MakeWorld();
	REQUIRE(World->AddData(*Data));
	const auto Left = World->AddAgent({-6.f, 0.f, 0.f}, {});
	const auto Right = World->AddAgent({6.f, 0.f, 0.f}, {});
	REQUIRE(Left);
	REQUIRE(Right);
	World->SetAgentTarget(*Left, {6.f, 0.f, 0.f});
	World->SetAgentTarget(*Right, {-6.f, 0.f, 0.f});

	float Closest = 100.f;
	for (int Second = 0; Second < 12; ++Second)
	{
		const std::array Agents{*Left, *Right};
		Run(*World, Agents, 60);
		Closest = std::min(Closest, (World->GetAgent(*Left)->Position - World->GetAgent(*Right)->Position).Length());
	}

	CHECK(Closest > 0.4f);
	CHECK(World->GetAgent(*Left)->bArrived);
	CHECK(World->GetAgent(*Right)->bArrived);
	CHECK(World->GetAgent(*Left)->Position.IsNearlyEqual({6.f, 0.f, 0.f}, 0.5f));

	// An agent placed before its ground loads waits off the navigation, then walks once it does.
	auto Streamed = MakeWorld();
	const auto Waiting = Streamed->AddAgent({0.f, 0.f, 0.f}, {});
	REQUIRE(Waiting);
	Streamed->SetAgentTarget(*Waiting, {5.f, 0.f, 5.f});
	Streamed->Update(1.f / 60.f);
	CHECK(!Streamed->GetAgent(*Waiting)->bOnNavigation);
	REQUIRE(Streamed->AddData(*Data));
	const std::array Agents{*Waiting};
	Run(*Streamed, Agents, 600);
	CHECK(Streamed->GetAgent(*Waiting)->bOnNavigation);
	CHECK(Streamed->GetAgent(*Waiting)->bArrived);
}
TEST_CASE("Levels build navigation from static bodies, with region tiles streaming as patches")
{
	constexpr FAssetId Cube{0x1111, 0x2222};
	const FNavigationMeshBounds Bounds = [Cube](const FAssetId& Mesh) -> std::optional<FNavigationBox>
	{
		return Mesh == Cube ? std::optional{FNavigationBox{.Minimum = {-0.5f, -0.5f, -0.5f}, .Maximum = {0.5f, 0.5f, 0.5f}}} : std::nullopt;
	};

	// A 20 m floor slab with a crate on it, a dynamic ball navigation must ignore, and bounds around the floor.
	const auto Floor = [&](const FObjectId Id, const FVector3d& Center)
	{
		return FLevelEntity{.Id = Id, .Name = "Floor", .Transform = {.Translation = FWorldPosition{Center}, .Scale = {20.f, 1.f, 20.f}}, .Mesh = FStaticMeshComponent{.Asset = Cube}, .BodyType = ELevelBodyType::Static};
	};

	FLevelDocument Level{.Id = FObjectId{1, 1}, .Name = "Navigation"};
	Level.Entities.push_back(Floor(FObjectId{1, 2}, {0., -0.5, 0.}));
	Level.Entities.push_back({.Id = FObjectId{1, 3}, .Name = "Crate", .Transform = {.Translation = FWorldPosition{{3., 1., 0.}}, .Scale = {2.f, 2.f, 2.f}}, .Mesh = FStaticMeshComponent{.Asset = Cube}, .BodyType = ELevelBodyType::Static});
	Level.Entities.push_back({.Id = FObjectId{1, 4}, .Name = "Ball", .Transform = {.Translation = FWorldPosition{{-3., 1., 0.}}, .Scale = {2.f, 2.f, 2.f}}, .Mesh = FStaticMeshComponent{.Asset = Cube}, .BodyType = ELevelBodyType::Dynamic});
	Level.Entities.push_back({.Id = FObjectId{1, 5}, .Name = "Bounds", .NavigationBounds = FNavigationBoundsComponent{.Size = {20.f, 6.f, 20.f}}});

	// A region with its own floor and bounds, placed so its floor continues the level's to the east.
	FLevelDocument Placed{.Id = FObjectId{2, 1}, .Name = "Region"};
	Placed.Entities.push_back(Floor(FObjectId{2, 2}, {0., -0.5, 0.}));
	Placed.Entities.push_back({.Id = FObjectId{2, 3}, .Name = "Bounds", .NavigationBounds = FNavigationBoundsComponent{.Size = {20.f, 6.f, 20.f}}});
	Level.Regions.push_back({.Id = FObjectId{3, 1}, .Name = "East", .Level = "Region.hlevel", .Offset = FWorldPosition{{20., 0., 0.}}});

	const auto Geometry = BuildLevelNavigationGeometry(Level.Entities, std::vector<FMatrix4>{FMatrix4{}, FMatrix4{}, FMatrix4{}, FMatrix4{}}, Bounds);
	CHECK(Geometry.Indices.size() == 2 * 36);

	const std::array<std::optional<FLevelDocument>, 1> Regions{Placed};
	const auto File = BuildLevelNavigation(Level, Regions, Bounds);
	REQUIRE(File);
	REQUIRE(*File);
	REQUIRE((*File)->Patches.size() == 1);
	CHECK((*File)->Patches[0].Key == GetRegionNavigationKey(FObjectId{3, 1}));
	CHECK(!(*File)->Base.Layers.empty());
	CHECK(!(*File)->Patches[0].Data.Layers.empty());

	// Each tile belongs to the level or the region, never both.
	std::set<std::pair<std::int32_t, std::int32_t>> BaseTiles;
	for (const FNavigationLayer& Layer : (*File)->Base.Layers)
	{
		BaseTiles.emplace(Layer.TileX, Layer.TileZ);
	}

	for (const FNavigationLayer& Layer : (*File)->Patches[0].Data.Layers)
	{
		CHECK(!BaseTiles.contains({Layer.TileX, Layer.TileZ}));
	}

	auto World = MakeWorld();
	REQUIRE(World->AddData((*File)->Base));
	const FVector3 Start{-8.f, 0.f, -8.f};
	const FVector3 Goal{25.f, 0.f, 5.f};
	const auto Before = World->FindPath(Start, Goal);
	CHECK((!Before || Before->bPartial));

	const auto Region = World->AddData((*File)->Patches[0].Data);
	REQUIRE(Region);
	const auto Joined = World->FindPath(Start, Goal);
	REQUIRE(Joined);
	CHECK(!Joined->bPartial);
	CHECK(Joined->Points.back().IsNearlyEqual(Goal, 0.3f));

	// The crate is solid, so a path through where it stands goes around it.
	const auto Around = World->FindPath({0.f, 0.f, 0.f}, {6.f, 0.f, 0.f});
	REQUIRE(Around);
	CHECK(PathLength(*Around) > 6.5f);

	// Rebuilding the same level gives the same bytes.
	const auto Again = BuildLevelNavigation(Level, Regions, Bounds);
	REQUIRE(Again);
	REQUIRE(*Again);
	CHECK(SerializeNavigationFile(**Again) == SerializeNavigationFile(**File));
	CHECK(!*BuildLevelNavigation(FLevelDocument{.Id = FObjectId{9, 9}, .Name = "Empty"}, {}, Bounds));
}
}
