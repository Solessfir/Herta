#include "Herta/NavigationBuilder/NavigationBuilder.h"

#include "Herta/Core/BinaryStream.h"
#include "Herta/Tasks/TaskSystem.h"
#include "TileCacheSupport.h"

#include <DetourAlloc.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>

namespace Herta
{
namespace
{
// Part of every source hash, so navigation cooked by another Recast is rebuilt.
constexpr std::string_view RecastRevision = "9f4ce64458dfae86e1239c525ddc219c4e9e06f1";

struct FHeightfieldDeleter
{
	void operator()(rcHeightfield* const Heightfield) const noexcept
	{
		rcFreeHeightField(Heightfield);
	}

	void operator()(rcCompactHeightfield* const Heightfield) const noexcept
	{
		rcFreeCompactHeightfield(Heightfield);
	}

	void operator()(rcHeightfieldLayerSet* const Layers) const noexcept
	{
		rcFreeHeightfieldLayerSet(Layers);
	}
};

struct FRecastConfig
{
	int WalkableHeight = 0;
	int WalkableClimb = 0;
	int WalkableRadius = 0;
	int BorderSize = 0;
	int Size = 0;
	float SlopeDegrees = 45.f;
};

[[nodiscard]] FRecastConfig MakeConfig(const FNavigationSettings& Settings)
{
	FRecastConfig Config;
	Config.WalkableHeight = static_cast<int>(std::ceil(Settings.AgentHeight / Settings.CellHeight));
	Config.WalkableClimb = static_cast<int>(std::floor(Settings.AgentMaxClimb / Settings.CellHeight));
	Config.WalkableRadius = static_cast<int>(std::ceil(Settings.AgentRadius / Settings.CellSize));
	// Recast's padding: the agent's radius plus a few cells, so walkable edges match across tiles.
	Config.BorderSize = Config.WalkableRadius + 3;
	Config.Size = static_cast<int>(Settings.TileCells) + Config.BorderSize * 2;
	Config.SlopeDegrees = Settings.AgentMaxSlopeRadians * 180.f / std::numbers::pi_v<float>;
	return Config;
}

struct FTileKey
{
	auto operator<=>(const FTileKey&) const = default;

	std::int32_t X = 0;
	std::int32_t Z = 0;
};

[[nodiscard]] bool Overlaps(const FNavigationBox& Box, const FVector3& Minimum, const FVector3& Maximum)
{
	return Minimum.X <= Box.Maximum.X && Maximum.X >= Box.Minimum.X && Minimum.Y <= Box.Maximum.Y && Maximum.Y >= Box.Minimum.Y && Minimum.Z <= Box.Maximum.Z && Maximum.Z >= Box.Minimum.Z;
}

[[nodiscard]] std::expected<std::vector<FNavigationLayer>, FNavigationError> BuildTile(const std::vector<float>& Vertices, const std::vector<int>& Triangles, const FTileKey Tile, const float MinimumY, const float MaximumY, const FNavigationSettings& Settings)
{
	const FRecastConfig Config = MakeConfig(Settings);
	const float TileSize = Settings.GetTileSize();
	const float Border = static_cast<float>(Config.BorderSize) * Settings.CellSize;
	const float BoundsMinimum[3]{static_cast<float>(Tile.X) * TileSize - Border, MinimumY, static_cast<float>(Tile.Z) * TileSize - Border};
	const float BoundsMaximum[3]{static_cast<float>(Tile.X + 1) * TileSize + Border, MaximumY, static_cast<float>(Tile.Z + 1) * TileSize + Border};

	rcContext Context(false);
	const std::unique_ptr<rcHeightfield, FHeightfieldDeleter> Solid(rcAllocHeightfield());
	if (!Solid || !rcCreateHeightfield(&Context, *Solid, Config.Size, Config.Size, BoundsMinimum, BoundsMaximum, Settings.CellSize, Settings.CellHeight))
	{
		return std::unexpected(FNavigationError{"Cannot create a navigation heightfield"});
	}

	const int TriangleCount = static_cast<int>(Triangles.size() / 3);
	const int VertexCount = static_cast<int>(Vertices.size() / 3);
	std::vector<unsigned char> Areas(static_cast<std::size_t>(TriangleCount), 0);
	rcMarkWalkableTriangles(&Context, Config.SlopeDegrees, Vertices.data(), VertexCount, Triangles.data(), TriangleCount, Areas.data());
	if (!rcRasterizeTriangles(&Context, Vertices.data(), VertexCount, Triangles.data(), Areas.data(), TriangleCount, *Solid, Config.WalkableClimb))
	{
		return std::unexpected(FNavigationError{"Cannot rasterize navigation geometry"});
	}

	rcFilterLowHangingWalkableObstacles(&Context, Config.WalkableClimb, *Solid);
	rcFilterLedgeSpans(&Context, Config.WalkableHeight, Config.WalkableClimb, *Solid);
	rcFilterWalkableLowHeightSpans(&Context, Config.WalkableHeight, *Solid);

	const std::unique_ptr<rcCompactHeightfield, FHeightfieldDeleter> Compact(rcAllocCompactHeightfield());
	if (!Compact || !rcBuildCompactHeightfield(&Context, Config.WalkableHeight, Config.WalkableClimb, *Solid, *Compact) || !rcErodeWalkableArea(&Context, Config.WalkableRadius, *Compact))
	{
		return std::unexpected(FNavigationError{"Cannot build compact navigation voxels"});
	}

	const std::unique_ptr<rcHeightfieldLayerSet, FHeightfieldDeleter> LayerSet(rcAllocHeightfieldLayerSet());
	if (!LayerSet || !rcBuildHeightfieldLayers(&Context, *Compact, Config.BorderSize, Config.WalkableHeight, *LayerSet))
	{
		return std::unexpected(FNavigationError{"Cannot split navigation voxels into layers"});
	}

	FNavigationCompressor Compressor;
	std::vector<FNavigationLayer> Layers;
	for (int Index = 0; Index < LayerSet->nlayers; ++Index)
	{
		const rcHeightfieldLayer& Layer = LayerSet->layers[Index];
		dtTileCacheLayerHeader Header{};
		Header.magic = DT_TILECACHE_MAGIC;
		Header.version = DT_TILECACHE_VERSION;
		Header.tx = Tile.X;
		Header.ty = Tile.Z;
		Header.tlayer = Index;
		std::copy_n(Layer.bmin, 3, Header.bmin);
		std::copy_n(Layer.bmax, 3, Header.bmax);
		Header.width = static_cast<unsigned char>(Layer.width);
		Header.height = static_cast<unsigned char>(Layer.height);
		Header.minx = static_cast<unsigned char>(Layer.minx);
		Header.maxx = static_cast<unsigned char>(Layer.maxx);
		Header.miny = static_cast<unsigned char>(Layer.miny);
		Header.maxy = static_cast<unsigned char>(Layer.maxy);
		Header.hmin = static_cast<unsigned short>(Layer.hmin);
		Header.hmax = static_cast<unsigned short>(Layer.hmax);

		unsigned char* Data = nullptr;
		int Size = 0;
		if (dtStatusFailed(dtBuildTileCacheLayer(&Compressor, &Header, Layer.heights, Layer.areas, Layer.cons, &Data, &Size)))
		{
			return std::unexpected(FNavigationError{"Cannot compress a navigation layer"});
		}

		FNavigationLayer& Result = Layers.emplace_back(FNavigationLayer{.TileX = Tile.X, .TileZ = Tile.Z});
		Result.Data.resize(static_cast<std::size_t>(Size));
		std::memcpy(Result.Data.data(), Data, static_cast<std::size_t>(Size));
		dtFree(Data);
	}

	return Layers;
}

void WriteSettings(FBinaryWriter& Writer, const FNavigationSettings& Settings)
{
	Writer.WriteFloat(Settings.CellSize);
	Writer.WriteFloat(Settings.CellHeight);
	Writer.WriteFloat(Settings.AgentRadius);
	Writer.WriteFloat(Settings.AgentHeight);
	Writer.WriteFloat(Settings.AgentMaxClimb);
	Writer.WriteFloat(Settings.AgentMaxSlopeRadians);
	Writer.Write(Settings.TileCells);
}

void WriteBox(FBinaryWriter& Writer, const FNavigationBox& Box)
{
	for (const FVector3& Corner : {Box.Minimum, Box.Maximum})
	{
		Writer.WriteFloat(Corner.X);
		Writer.WriteFloat(Corner.Y);
		Writer.WriteFloat(Corner.Z);
	}
}
}

FHash128 HashNavigationSource(const FNavigationGeometry& Geometry, const std::span<const FNavigationBox> Boxes, const FNavigationSettings& Settings)
{
	FBinaryWriter Writer;
	Writer.Write(NavigationDataVersion);
	Writer.WriteString(RecastRevision);
	WriteSettings(Writer, Settings);
	Writer.Write(static_cast<std::uint32_t>(Boxes.size()));
	for (const FNavigationBox& Box : Boxes)
	{
		WriteBox(Writer, Box);
	}

	Writer.Write(static_cast<std::uint64_t>(Geometry.Vertices.size()));
	for (const FVector3& Vertex : Geometry.Vertices)
	{
		Writer.WriteFloat(Vertex.X);
		Writer.WriteFloat(Vertex.Y);
		Writer.WriteFloat(Vertex.Z);
	}

	Writer.Write(static_cast<std::uint64_t>(Geometry.Indices.size()));
	for (const std::uint32_t Index : Geometry.Indices)
	{
		Writer.Write(Index);
	}

	return HashBytes(Writer.GetBytes());
}

std::expected<FNavigationData, FNavigationError> BuildNavigationData(const FNavigationGeometry& Geometry, const std::span<const FNavigationBox> Boxes, const FNavigationSettings& Settings, FTaskSystem* const Tasks)
{
	if (auto Valid = ValidateNavigationSettings(Settings); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	if (Geometry.Indices.size() % 3 != 0 || std::ranges::any_of(Geometry.Indices, [&](const std::uint32_t Index)
	{
		return Index >= Geometry.Vertices.size();
	}))
	{
		return std::unexpected(FNavigationError{"Navigation geometry has an index past its vertices or a partial triangle"});
	}

	if (std::ranges::any_of(Geometry.Vertices, [](const FVector3& Vertex)
	{
		return !std::isfinite(Vertex.X) || !std::isfinite(Vertex.Y) || !std::isfinite(Vertex.Z);
	}))
	{
		return std::unexpected(FNavigationError{"Navigation geometry has a vertex that is not finite"});
	}

	FNavigationData Data{.Settings = Settings, .Source = HashNavigationSource(Geometry, Boxes, Settings), .Boxes = {Boxes.begin(), Boxes.end()}};
	std::vector<float> Vertices;
	Vertices.reserve(Geometry.Vertices.size() * 3);
	for (const FVector3& Vertex : Geometry.Vertices)
	{
		Vertices.insert(Vertices.end(), {Vertex.X, Vertex.Y, Vertex.Z});
	}

	// Each tile gathers the triangles that reach it through its border, so every tile rasterizes only its own neighbourhood.
	const FRecastConfig Config = MakeConfig(Settings);
	const float TileSize = Settings.GetTileSize();
	const float Border = static_cast<float>(Config.BorderSize) * Settings.CellSize;

	struct FTileInput
	{
		std::vector<int> Triangles{};
		float MinimumY = 0.f;
		float MaximumY = 0.f;
	};

	std::map<FTileKey, FTileInput> Tiles;
	for (std::size_t Triangle = 0; Triangle + 2 < Geometry.Indices.size(); Triangle += 3)
	{
		FVector3 Minimum = Geometry.Vertices[Geometry.Indices[Triangle]];
		FVector3 Maximum = Minimum;
		for (std::size_t Corner = 1; Corner < 3; ++Corner)
		{
			const FVector3& Vertex = Geometry.Vertices[Geometry.Indices[Triangle + Corner]];
			Minimum = {std::min(Minimum.X, Vertex.X), std::min(Minimum.Y, Vertex.Y), std::min(Minimum.Z, Vertex.Z)};
			Maximum = {std::max(Maximum.X, Vertex.X), std::max(Maximum.Y, Vertex.Y), std::max(Maximum.Z, Vertex.Z)};
		}

		if (std::ranges::none_of(Boxes, [&](const FNavigationBox& Box)
		{
			return Overlaps(Box, Minimum, Maximum);
		}))
		{
			continue;
		}

		const auto FirstX = static_cast<std::int32_t>(std::floor((Minimum.X - Border) / TileSize));
		const auto LastX = static_cast<std::int32_t>(std::floor((Maximum.X + Border) / TileSize));
		const auto FirstZ = static_cast<std::int32_t>(std::floor((Minimum.Z - Border) / TileSize));
		const auto LastZ = static_cast<std::int32_t>(std::floor((Maximum.Z + Border) / TileSize));
		for (std::int32_t Z = FirstZ; Z <= LastZ; ++Z)
		{
			for (std::int32_t X = FirstX; X <= LastX; ++X)
			{
				// Only tiles inside a box are built, though their triangles may come from just outside it.
				const FVector3 TileMinimum{static_cast<float>(X) * TileSize, -1.e30f, static_cast<float>(Z) * TileSize};
				const FVector3 TileMaximum{static_cast<float>(X + 1) * TileSize, 1.e30f, static_cast<float>(Z + 1) * TileSize};
				if (std::ranges::none_of(Boxes, [&](const FNavigationBox& Box)
				{
					return Overlaps(Box, TileMinimum, TileMaximum);
				}))
				{
					continue;
				}

				auto [Found, bInserted] = Tiles.try_emplace(FTileKey{X, Z}, FTileInput{.MinimumY = Minimum.Y, .MaximumY = Maximum.Y});
				FTileInput& Input = Found->second;
				Input.MinimumY = std::min(Input.MinimumY, Minimum.Y);
				Input.MaximumY = std::max(Input.MaximumY, Maximum.Y);
				Input.Triangles.insert(Input.Triangles.end(), {static_cast<int>(Geometry.Indices[Triangle]), static_cast<int>(Geometry.Indices[Triangle + 1]), static_cast<int>(Geometry.Indices[Triangle + 2])});
			}
		}
	}

	std::vector<std::pair<FTileKey, FTileInput>> Work(std::make_move_iterator(Tiles.begin()), std::make_move_iterator(Tiles.end()));
	std::vector<std::optional<std::expected<std::vector<FNavigationLayer>, FNavigationError>>> Results(Work.size());
	const auto Build = [&](const std::size_t Index)
	{
		const auto& [Key, Input] = Work[Index];
		// Room above the highest surface for the agent to stand.
		Results[Index] = BuildTile(Vertices, Input.Triangles, Key, Input.MinimumY - Settings.CellHeight, Input.MaximumY + Settings.AgentHeight, Settings);
	};

	std::unique_ptr<FTaskScope> Scope = Tasks ? Tasks->CreateScope("Navigation build").value_or(nullptr) : nullptr;
	const auto Parallel = Scope ? Tasks->ParallelFor(*Scope, {.Name = "Navigation tiles", .Lane = ETaskLane::Cpu}, Work.size(), 1, [&Build](const std::size_t Index, FTaskContext&)
	{
		Build(Index);
	})
	                            : std::unexpected(FTaskError{});

	// Tiles a failed or missing task system left unbuilt are built here instead.
	if (Parallel && Parallel->Wait().State != ETaskState::Succeeded)
	{
		Scope->Wait();
	}

	for (std::size_t Index = 0; Index < Work.size(); ++Index)
	{
		if (!Results[Index])
		{
			Build(Index);
		}
	}

	for (std::optional<std::expected<std::vector<FNavigationLayer>, FNavigationError>>& Result : Results)
	{
		if (!Result || !*Result)
		{
			return std::unexpected(Result ? Result->error() : FNavigationError{"A navigation tile was not built"});
		}

		std::ranges::move(**Result, std::back_inserter(Data.Layers));
	}

	return Data;
}
}
