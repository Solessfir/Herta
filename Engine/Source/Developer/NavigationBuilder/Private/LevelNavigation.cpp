#include "Herta/NavigationBuilder/NavigationBuilder.h"

#include "Herta/Math/AffineTransform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace Herta
{
namespace
{
constexpr int ShapeSegments = 12;

[[nodiscard]] FVector3 TransformPoint(const FMatrix4& Matrix, const FVector3& Point)
{
	const FVector4 Result = Matrix * FVector4{Point.X, Point.Y, Point.Z, 1.f};
	return {Result.X, Result.Y, Result.Z};
}

// Appends a triangle of a convex shape wound to face away from its center, as Recast reads walkable tops.
void AddTriangle(FNavigationGeometry& Geometry, const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& Center)
{
	const auto Base = static_cast<std::uint32_t>(Geometry.Vertices.size());
	const bool bOutward = (B - A).Cross(C - A).Dot((A + B + C) * (1.f / 3.f) - Center) >= 0.f;
	Geometry.Vertices.insert(Geometry.Vertices.end(), {A, bOutward ? B : C, bOutward ? C : B});
	Geometry.Indices.insert(Geometry.Indices.end(), {Base, Base + 1, Base + 2});
}

void AddQuad(FNavigationGeometry& Geometry, const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D, const FVector3& Center)
{
	AddTriangle(Geometry, A, B, C, Center);
	AddTriangle(Geometry, A, C, D, Center);
}

void AddBox(FNavigationGeometry& Geometry, const FVector3& Center, const FQuaternion& Rotation, const FVector3& Half)
{
	const auto Corner = [&](const int Index)
	{
		return Center + Rotation.RotateVector({(Index & 1) != 0 ? Half.X : -Half.X, (Index & 2) != 0 ? Half.Y : -Half.Y, (Index & 4) != 0 ? Half.Z : -Half.Z});
	};

	constexpr std::array<std::array<int, 4>, 6> Faces{{{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}}};
	for (const std::array<int, 4>& Face : Faces)
	{
		AddQuad(Geometry, Corner(Face[0]), Corner(Face[1]), Corner(Face[2]), Corner(Face[3]), Center);
	}
}

// A sphere as rings of quads, and an upright capsule as the cylinder around it, which is all an agent walking past needs.
void AddSphere(FNavigationGeometry& Geometry, const FVector3& Center, const float Radius)
{
	constexpr int Rings = ShapeSegments / 2;
	const auto Point = [&](const int Ring, const int Segment)
	{
		const float Polar = std::numbers::pi_v<float> * static_cast<float>(Ring) / static_cast<float>(Rings);
		const float Azimuth = 2.f * std::numbers::pi_v<float> * static_cast<float>(Segment) / static_cast<float>(ShapeSegments);
		return Center + FVector3{std::sin(Polar) * std::cos(Azimuth), std::cos(Polar), std::sin(Polar) * std::sin(Azimuth)} * Radius;
	};

	for (int Ring = 0; Ring < Rings; ++Ring)
	{
		for (int Segment = 0; Segment < ShapeSegments; ++Segment)
		{
			const FVector3 A = Point(Ring, Segment);
			const FVector3 B = Point(Ring, Segment + 1);
			const FVector3 C = Point(Ring + 1, Segment + 1);
			const FVector3 D = Point(Ring + 1, Segment);
			if (Ring == 0)
			{
				AddTriangle(Geometry, A, C, D, Center);
			}
			else if (Ring == Rings - 1)
			{
				AddTriangle(Geometry, A, B, C, Center);
			}
			else
			{
				AddQuad(Geometry, A, B, C, D, Center);
			}
		}
	}
}

void AddCylinder(FNavigationGeometry& Geometry, const FVector3& Center, const FQuaternion& Rotation, const float Radius, const float HalfHeight)
{
	const auto Point = [&](const int Segment, const float Height)
	{
		const float Azimuth = 2.f * std::numbers::pi_v<float> * static_cast<float>(Segment) / static_cast<float>(ShapeSegments);
		return Center + Rotation.RotateVector({std::cos(Azimuth) * Radius, Height, std::sin(Azimuth) * Radius});
	};

	const FVector3 Top = Center + Rotation.RotateVector({0.f, HalfHeight, 0.f});
	const FVector3 Bottom = Center - Rotation.RotateVector({0.f, HalfHeight, 0.f});
	for (int Segment = 0; Segment < ShapeSegments; ++Segment)
	{
		AddQuad(Geometry, Point(Segment, -HalfHeight), Point(Segment + 1, -HalfHeight), Point(Segment + 1, HalfHeight), Point(Segment, HalfHeight), Center);
		AddTriangle(Geometry, Top, Point(Segment, HalfHeight), Point(Segment + 1, HalfHeight), Center);
		AddTriangle(Geometry, Bottom, Point(Segment + 1, -HalfHeight), Point(Segment, -HalfHeight), Center);
	}
}

[[nodiscard]] FVector3 Absolute(const FVector3& Vector)
{
	return {std::abs(Vector.X), std::abs(Vector.Y), std::abs(Vector.Z)};
}

void AppendGeometry(FNavigationGeometry& Target, const FNavigationGeometry& Source)
{
	const auto Base = static_cast<std::uint32_t>(Target.Vertices.size());
	Target.Vertices.insert(Target.Vertices.end(), Source.Vertices.begin(), Source.Vertices.end());
	for (const std::uint32_t Index : Source.Indices)
	{
		Target.Indices.push_back(Base + Index);
	}
}
}

FNavigationGeometry BuildLevelNavigationGeometry(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> World, const FNavigationMeshBounds& Bounds)
{
	FNavigationGeometry Geometry;
	for (std::size_t Index = 0; Index < std::min(Entities.size(), World.size()); ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		if (!Entity.Mesh || Entity.BodyType != ELevelBodyType::Static || Entity.Mover || Entity.Trigger || Entity.SoftBody || Entity.Destructible)
		{
			continue;
		}

		// Physics skips sheared bodies, so navigation does too.
		const auto Transform = TryDecomposeTransform(World[Index]);
		if (!Transform)
		{
			continue;
		}

		// Matches GetLevelBodyShape and FLevelSimulation's fitting: the mesh bounds, at least 1 mm thick, scaled and turned with the entity.
		const std::optional<FNavigationBox> Box = Bounds ? Bounds(Entity.Mesh->Asset) : std::nullopt;
		const FVector3 ModelCenter = Box ? (Box->Minimum + Box->Maximum) * 0.5f : FVector3{};
		const FVector3 ModelHalf = Box ? FVector3{std::max((Box->Maximum.X - Box->Minimum.X) * 0.5f, 0.001f), std::max((Box->Maximum.Y - Box->Minimum.Y) * 0.5f, 0.001f), std::max((Box->Maximum.Z - Box->Minimum.Z) * 0.5f, 0.001f)} : FVector3{0.5f, 0.5f, 0.5f};
		const FVector3 Half = Absolute(Transform->Scale3D.ComponentMultiply(ModelHalf));
		const FVector3 Center = Transform->Translation + Transform->Rotation.RotateVector(Transform->Scale3D.ComponentMultiply(ModelCenter));
		switch (Entity.BodySettings.Collision)
		{
			case ELevelCollisionShape::Box:
				AddBox(Geometry, Center, Transform->Rotation, Half);
				break;
			case ELevelCollisionShape::Sphere:
				AddSphere(Geometry, Center, std::max({Half.X, Half.Y, Half.Z}));
				break;
			case ELevelCollisionShape::Capsule:
				AddCylinder(Geometry, Center, Transform->Rotation, std::max(Half.X, Half.Z), Half.Y);
				break;
		}
	}

	return Geometry;
}

std::vector<FNavigationBox> GetLevelNavigationBoxes(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> World)
{
	std::vector<FNavigationBox> Boxes;
	for (std::size_t Index = 0; Index < std::min(Entities.size(), World.size()); ++Index)
	{
		if (!Entities[Index].NavigationBounds)
		{
			continue;
		}

		const FVector3 Half = Entities[Index].NavigationBounds->Size * 0.5f;
		FNavigationBox& Box = Boxes.emplace_back(FNavigationBox{.Minimum = {1.e30f, 1.e30f, 1.e30f}, .Maximum = {-1.e30f, -1.e30f, -1.e30f}});
		for (int Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 Point = TransformPoint(World[Index], {(Corner & 1) != 0 ? Half.X : -Half.X, (Corner & 2) != 0 ? Half.Y : -Half.Y, (Corner & 4) != 0 ? Half.Z : -Half.Z});
			Box.Minimum = {std::min(Box.Minimum.X, Point.X), std::min(Box.Minimum.Y, Point.Y), std::min(Box.Minimum.Z, Point.Z)};
			Box.Maximum = {std::max(Box.Maximum.X, Point.X), std::max(Box.Maximum.Y, Point.Y), std::max(Box.Maximum.Z, Point.Z)};
		}
	}

	return Boxes;
}

FHash128 GetRegionNavigationKey(const FObjectId& Region) noexcept
{
	return {.High = Region.GetHigh(), .Low = Region.GetLow()};
}

std::expected<std::optional<FNavigationFile>, FNavigationError> BuildLevelNavigation(const FLevelDocument& Level, const std::span<const std::optional<FLevelDocument>> Regions, const FNavigationMeshBounds& Bounds, const FNavigationSettings& Settings, FTaskSystem* const Tasks)
{
	const auto World = ComposeLevelRegionMatrices({}, Level.Entities);
	if (!World)
	{
		return std::unexpected(FNavigationError{std::format("The level is invalid: {}", World.error().Message)});
	}

	FNavigationGeometry Geometry = BuildLevelNavigationGeometry(Level.Entities, *World, Bounds);
	const std::vector<FNavigationBox> LevelBoxes = GetLevelNavigationBoxes(Level.Entities, *World);
	std::vector<FNavigationBox> Boxes = LevelBoxes;
	std::vector<std::pair<std::size_t, std::vector<FNavigationBox>>> RegionBoxes;
	for (std::size_t Index = 0; Index < std::min(Regions.size(), Level.Regions.size()); ++Index)
	{
		if (!Regions[Index])
		{
			continue;
		}

		const FLevelRegion& Region = Level.Regions[Index];
		const std::vector<FLevelEntity>& Entities = Regions[Index]->Entities;
		const auto Placed = ComposeLevelRegionMatrices(Region, Entities);
		if (!Placed)
		{
			return std::unexpected(FNavigationError{std::format("Region '{}' is invalid: {}", Region.Name, Placed.error().Message)});
		}

		// Only scenery streams in with a region, so only its scenery is walked on.
		std::vector<FLevelEntity> Scenery;
		std::vector<FMatrix4> SceneryWorld;
		for (std::size_t Entity = 0; Entity < Entities.size(); ++Entity)
		{
			if (IsLevelRegionScenery(Entities[Entity]))
			{
				Scenery.push_back(Entities[Entity]);
				SceneryWorld.push_back((*Placed)[Entity]);
			}
		}

		AppendGeometry(Geometry, BuildLevelNavigationGeometry(Scenery, SceneryWorld, Bounds));
		std::vector<FNavigationBox> Owned = GetLevelNavigationBoxes(Entities, *Placed);
		if (!Owned.empty())
		{
			Boxes.insert(Boxes.end(), Owned.begin(), Owned.end());
			RegionBoxes.emplace_back(Index, std::move(Owned));
		}
	}

	if (Boxes.empty())
	{
		return std::nullopt;
	}

	// Every tile is built once from the level and all its regions, so tiles meet wherever their ground does, as a world built in one piece
	// would. A tile then streams with the region whose bounds hold its center, or stays with the level.
	auto Built = BuildNavigationData(Geometry, Boxes, Settings, Tasks);
	if (!Built)
	{
		return std::unexpected(Built.error());
	}

	FNavigationFile File{.Base = {.Settings = Settings, .Source = Built->Source, .Boxes = LevelBoxes}};
	for (const auto& [Index, Owned] : RegionBoxes)
	{
		File.Patches.push_back({.Key = GetRegionNavigationKey(Level.Regions[Index].Id), .Data = {.Settings = Settings, .Source = Built->Source, .Boxes = Owned}});
	}

	const float TileSize = Settings.GetTileSize();
	for (FNavigationLayer& Layer : Built->Layers)
	{
		const float X = (static_cast<float>(Layer.TileX) + 0.5f) * TileSize;
		const float Z = (static_cast<float>(Layer.TileZ) + 0.5f) * TileSize;
		const auto Owner = std::ranges::find_if(File.Patches, [X, Z](const FNavigationPatch& Patch)
		{
			return std::ranges::any_of(Patch.Data.Boxes, [X, Z](const FNavigationBox& Box)
			{
				return X >= Box.Minimum.X && X <= Box.Maximum.X && Z >= Box.Minimum.Z && Z <= Box.Maximum.Z;
			});
		});

		(Owner != File.Patches.end() ? Owner->Data : File.Base).Layers.push_back(std::move(Layer));
	}

	return File;
}
}
