#include "Herta/Navigation/NavigationWorld.h"

#include "TileCacheSupport.h"

#include <DetourAlloc.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace Herta
{
namespace
{
constexpr int QueryNodes = 4096;
constexpr int MaximumStraightPoints = 256;
// Tiles rebuilt per Update under changed obstacles; the tile cache rebuilds one per call.
constexpr int TileRebuildsPerUpdate = 8;
// An obstacle re-cuts once it moves this far or turns this much.
constexpr float ObstacleMoveTolerance = 0.1f;
constexpr float ObstacleTurnTolerance = 0.05f;
constexpr float MaximumAgentRadius = 0.6f;

struct FNavMeshDeleter
{
	void operator()(dtNavMesh* const Mesh) const noexcept
	{
		dtFreeNavMesh(Mesh);
	}

	void operator()(dtTileCache* const Cache) const noexcept
	{
		dtFreeTileCache(Cache);
	}

	void operator()(dtNavMeshQuery* const Query) const noexcept
	{
		dtFreeNavMeshQuery(Query);
	}

	void operator()(dtCrowd* const Crowd) const noexcept
	{
		dtFreeCrowd(Crowd);
	}
};

void Copy(const FVector3& Vector, float* const Out)
{
	Out[0] = Vector.X;
	Out[1] = Vector.Y;
	Out[2] = Vector.Z;
}

[[nodiscard]] FVector3 ToVector(const float* const Values)
{
	return {Values[0], Values[1], Values[2]};
}

struct FTileKey
{
	auto operator<=>(const FTileKey&) const = default;

	std::int32_t X = 0;
	std::int32_t Z = 0;
};

[[nodiscard]] float HorizontalDistance(const FVector3& First, const FVector3& Second)
{
	return std::hypot(First.X - Second.X, First.Z - Second.Z);
}
}

struct FNavigationWorld::FImplementation
{
	// A source's layers by tile, pointing into its copy of the data, and the tile cache tiles of the ones it owns now.
	struct FSource
	{
		FNavigationSourceId Id{};
		FNavigationData Data{};
		std::map<FTileKey, std::vector<const std::vector<std::byte>*>> Layers{};
		std::map<FTileKey, std::vector<dtCompressedTileRef>> Loaded{};
	};

	struct FObstacleSlot
	{
		FNavigationObstacleId Id{};
		FNavigationObstacle Placement{};
		// The placement the tile cache cut, which differs from Placement until the change is applied.
		FNavigationObstacle Applied{};
		dtObstacleRef Ref = 0;
		bool bDirty = true;
		bool bRemoved = false;
	};

	struct FAgent
	{
		FNavigationAgentId Id{};
		int CrowdIndex = -1;
		FNavigationAgentParameters Parameters{};
		FVector3 Position{};
		FVector3 Velocity{};
		std::optional<FVector3> Target{};
		// The target has not been placed on the navigation yet, such as one on a region still streaming in.
		bool bTargetPending = false;
	};

	struct FPathRequest
	{
		FNavigationPathRequestId Id{};
		FVector3 Start{};
		FVector3 End{};
		dtPolyRef EndRef = 0;
		FNavigationPathResult Result{};
		bool bStarted = false;
	};

	[[nodiscard]] dtCrowdAgentParams MakeCrowdParameters(const FNavigationAgentParameters& Parameters) const;
	[[nodiscard]] dtPolyRef FindPolygon(const FVector3& Point, const FVector3& Extents, FVector3* Nearest = nullptr) const;
	[[nodiscard]] FNavigationPath FinishPath(const FVector3& Start, const FVector3& End, const dtPolyRef EndRef, const dtPolyRef* Polygons, int Count) const;
	void ApplyObstacles();
	void AdvancePaths();
	void PlaceAgents();
	void TargetAgent(FAgent& Agent);
	[[nodiscard]] std::expected<void, FNavigationError> LoadTile(FSource& Source, FTileKey Tile);
	void UnloadTile(FSource& Source, FTileKey Tile);
	[[nodiscard]] FSource* FindSource(FNavigationSourceId Id);
	[[nodiscard]] FAgent* FindAgent(FNavigationAgentId Id);
	[[nodiscard]] const FAgent* FindAgent(FNavigationAgentId Id) const;

	FNavigationSettings Settings{};
	FNavigationWorldSettings WorldSettings{};

	// The tile cache refers to these for its whole life.
	FNavigationCompressor Compressor{};
	FNavigationMeshProcess MeshProcess{};
	dtTileCacheAlloc Allocator{};

	std::unique_ptr<dtNavMesh, FNavMeshDeleter> Mesh{};
	std::unique_ptr<dtTileCache, FNavMeshDeleter> Cache{};
	std::unique_ptr<dtNavMeshQuery, FNavMeshDeleter> Query{};
	// Requests search one at a time through their own query, since a sliced search keeps state between Updates.
	std::unique_ptr<dtNavMeshQuery, FNavMeshDeleter> SlicedQuery{};
	std::unique_ptr<dtCrowd, FNavMeshDeleter> Crowd{};
	dtQueryFilter Filter{};

	// Sources keep their address, since their layer lists point into their own data.
	std::vector<std::unique_ptr<FSource>> Sources{};
	// Every source with a tile, oldest first; the last one's layers are loaded.
	std::map<FTileKey, std::vector<FNavigationSourceId>> Owners{};
	std::vector<FObstacleSlot> Obstacles{};
	std::vector<FAgent> Agents{};
	std::deque<FPathRequest> PathRequests{};
	std::uint32_t NextId = 1;
};

FNavigationWorld::FNavigationWorld(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FNavigationWorld::~FNavigationWorld()
{
	// The crowd and queries point into the mesh, and the tile cache's tiles are built into it.
	if (Implementation)
	{
		Implementation->Crowd.reset();
		Implementation->SlicedQuery.reset();
		Implementation->Query.reset();
		Implementation->Cache.reset();
		Implementation->Mesh.reset();
	}
}

std::expected<std::unique_ptr<FNavigationWorld>, FNavigationError> FNavigationWorld::Create(const FNavigationSettings& Settings, const FNavigationWorldSettings& WorldSettings)
{
	// Obstacle references keep 16 bits for the obstacle.
	if (WorldSettings.MaximumTiles == 0 || WorldSettings.MaximumLayers == 0 || WorldSettings.MaximumObstacles == 0 || WorldSettings.MaximumObstacles > 65535 || WorldSettings.MaximumAgents == 0 || WorldSettings.MaximumPathRequests == 0 || WorldSettings.PathIterationsPerUpdate == 0
	    || WorldSettings.MaximumPathPolygons < 2)
	{
		return std::unexpected(FNavigationError{"Navigation capacities must be positive, with at most 65535 obstacles"});
	}

	if (auto Valid = ValidateNavigationSettings(Settings); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Settings = Settings;
	Implementation->WorldSettings = WorldSettings;
	Implementation->Filter.setIncludeFlags(NavigationWalkableFlag);
	Implementation->Filter.setExcludeFlags(0);

	const float TileSize = Settings.GetTileSize();
	dtNavMeshParams MeshParameters{};
	MeshParameters.tileWidth = TileSize;
	MeshParameters.tileHeight = TileSize;
	MeshParameters.maxTiles = static_cast<int>(WorldSettings.MaximumLayers);
	MeshParameters.maxPolys = 1 << 14;
	Implementation->Mesh.reset(dtAllocNavMesh());
	if (!Implementation->Mesh || dtStatusFailed(Implementation->Mesh->init(&MeshParameters)))
	{
		return std::unexpected(FNavigationError{"Cannot create the navigation mesh"});
	}

	dtTileCacheParams CacheParameters{};
	CacheParameters.cs = Settings.CellSize;
	CacheParameters.ch = Settings.CellHeight;
	CacheParameters.width = static_cast<int>(Settings.TileCells);
	CacheParameters.height = static_cast<int>(Settings.TileCells);
	CacheParameters.walkableHeight = Settings.AgentHeight;
	CacheParameters.walkableRadius = Settings.AgentRadius;
	CacheParameters.walkableClimb = Settings.AgentMaxClimb;
	CacheParameters.maxSimplificationError = 1.3f;
	CacheParameters.maxTiles = static_cast<int>(WorldSettings.MaximumLayers);
	CacheParameters.maxObstacles = static_cast<int>(WorldSettings.MaximumObstacles);
	Implementation->Cache.reset(dtAllocTileCache());
	if (!Implementation->Cache || dtStatusFailed(Implementation->Cache->init(&CacheParameters, &Implementation->Allocator, &Implementation->Compressor, &Implementation->MeshProcess)))
	{
		return std::unexpected(FNavigationError{"Cannot create the navigation tile cache"});
	}

	Implementation->Query.reset(dtAllocNavMeshQuery());
	Implementation->SlicedQuery.reset(dtAllocNavMeshQuery());
	if (!Implementation->Query || !Implementation->SlicedQuery || dtStatusFailed(Implementation->Query->init(Implementation->Mesh.get(), QueryNodes)) || dtStatusFailed(Implementation->SlicedQuery->init(Implementation->Mesh.get(), QueryNodes)))
	{
		return std::unexpected(FNavigationError{"Cannot create navigation queries"});
	}

	Implementation->Crowd.reset(dtAllocCrowd());
	if (!Implementation->Crowd || !Implementation->Crowd->init(static_cast<int>(WorldSettings.MaximumAgents), MaximumAgentRadius, Implementation->Mesh.get()))
	{
		return std::unexpected(FNavigationError{"Cannot create the navigation crowd"});
	}

	Implementation->Crowd->getEditableFilter(0)->setIncludeFlags(NavigationWalkableFlag);
	return std::unique_ptr<FNavigationWorld>(new FNavigationWorld(std::move(Implementation)));
}

std::expected<void, FNavigationError> FNavigationWorld::FImplementation::LoadTile(FSource& Source, const FTileKey Tile)
{
	std::vector<dtCompressedTileRef>& Loaded = Source.Loaded[Tile];
	for (const std::vector<std::byte>* const Layer : Source.Layers[Tile])
	{
		auto* const Bytes = static_cast<unsigned char*>(dtAlloc(static_cast<int>(Layer->size()), DT_ALLOC_PERM));
		std::memcpy(Bytes, Layer->data(), Layer->size());
		// Only one source owns a tile at a time, so its layers number from zero.
		dtTileCacheLayerHeader Header{};
		std::memcpy(&Header, Bytes, sizeof(Header));
		Header.tlayer = static_cast<int>(Loaded.size());
		std::memcpy(Bytes, &Header, sizeof(Header));
		dtCompressedTileRef Ref = 0;
		if (dtStatusFailed(Cache->addTile(Bytes, static_cast<int>(Layer->size()), DT_COMPRESSEDTILE_FREE_DATA, &Ref)))
		{
			dtFree(Bytes);
			return std::unexpected(FNavigationError{"The navigation tile cache is full or a layer is invalid"});
		}

		Loaded.push_back(Ref);
		if (dtStatusFailed(Cache->buildNavMeshTile(Ref, Mesh.get())))
		{
			return std::unexpected(FNavigationError{"Cannot build a navigation tile; the navigation mesh may be full"});
		}
	}

	// Obstacles only cut the tiles that were loaded when they were placed, so the ones over this tile cut again.
	const float TileSize = Settings.GetTileSize();
	for (FObstacleSlot& Obstacle : Obstacles)
	{
		const float Reach = std::hypot(Obstacle.Placement.HalfExtents.X, Obstacle.Placement.HalfExtents.Z);
		const FVector3& Center = Obstacle.Placement.Center;
		if (Center.X + Reach >= static_cast<float>(Tile.X) * TileSize && Center.X - Reach <= static_cast<float>(Tile.X + 1) * TileSize && Center.Z + Reach >= static_cast<float>(Tile.Z) * TileSize && Center.Z - Reach <= static_cast<float>(Tile.Z + 1) * TileSize)
		{
			Obstacle.bDirty = true;
		}
	}

	return {};
}

void FNavigationWorld::FImplementation::UnloadTile(FSource& Source, const FTileKey Tile)
{
	const auto Found = Source.Loaded.find(Tile);
	if (Found == Source.Loaded.end())
	{
		return;
	}

	for (const dtCompressedTileRef Ref : Found->second)
	{
		const dtCompressedTile* const Compressed = Cache->getTileByRef(Ref);
		if (Compressed && Compressed->header)
		{
			Mesh->removeTile(Mesh->getTileRefAt(Compressed->header->tx, Compressed->header->ty, Compressed->header->tlayer), nullptr, nullptr);
		}

		Cache->removeTile(Ref, nullptr, nullptr);
	}

	Source.Loaded.erase(Found);
}

FNavigationWorld::FImplementation::FSource* FNavigationWorld::FImplementation::FindSource(const FNavigationSourceId Id)
{
	const auto Found = std::ranges::find(Sources, Id, [](const std::unique_ptr<FSource>& Source)
	{
		return Source->Id;
	});

	return Found != Sources.end() ? Found->get() : nullptr;
}

std::expected<FNavigationSourceId, FNavigationError> FNavigationWorld::AddData(const FNavigationData& Data)
{
	FImplementation& World = *Implementation;
	if (!(Data.Settings == World.Settings))
	{
		return std::unexpected(FNavigationError{"Navigation data was built with other settings than this world's"});
	}

	auto Source = std::make_unique<FImplementation::FSource>(FImplementation::FSource{.Id = {World.NextId++}, .Data = Data});
	for (const FNavigationLayer& Layer : Source->Data.Layers)
	{
		if (Layer.Data.size() < sizeof(dtTileCacheLayerHeader))
		{
			return std::unexpected(FNavigationError{"A navigation layer is truncated"});
		}

		Source->Layers[{Layer.TileX, Layer.TileZ}].push_back(&Layer.Data);
	}

	// The newest source covers the tiles it shares with older ones until it unloads.
	FImplementation::FSource& Added = *Source;
	World.Sources.push_back(std::move(Source));
	for (const auto& [Tile, Layers] : Added.Layers)
	{
		std::vector<FNavigationSourceId>& Owners = World.Owners[Tile];
		if (!Owners.empty())
		{
			World.UnloadTile(*World.FindSource(Owners.back()), Tile);
		}

		Owners.push_back(Added.Id);
		if (auto Loaded = World.LoadTile(Added, Tile); !Loaded)
		{
			RemoveData(Added.Id);
			return std::unexpected(Loaded.error());
		}
	}

	return Added.Id;
}

void FNavigationWorld::RemoveData(const FNavigationSourceId Source)
{
	FImplementation& World = *Implementation;
	FImplementation::FSource* const Removed = World.FindSource(Source);
	if (!Removed)
	{
		return;
	}

	for (const auto& [Tile, Layers] : Removed->Layers)
	{
		std::vector<FNavigationSourceId>& Owners = World.Owners[Tile];
		const bool bOwner = !Owners.empty() && Owners.back() == Source;
		std::erase(Owners, Source);
		if (!bOwner)
		{
			continue;
		}

		World.UnloadTile(*Removed, Tile);
		if (!Owners.empty())
		{
			// The tiles were loaded once already, so they fit again.
			static_cast<void>(World.LoadTile(*World.FindSource(Owners.back()), Tile));
		}
	}

	std::erase_if(World.Owners, [](const auto& Entry)
	{
		return Entry.second.empty();
	});

	std::erase_if(World.Sources, [Source](const std::unique_ptr<FImplementation::FSource>& Candidate)
	{
		return Candidate->Id == Source;
	});
}

std::expected<FNavigationObstacleId, FNavigationError> FNavigationWorld::AddObstacle(const FNavigationObstacle& Obstacle)
{
	FImplementation& World = *Implementation;
	const bool bFinite = std::isfinite(Obstacle.Center.X) && std::isfinite(Obstacle.Center.Y) && std::isfinite(Obstacle.Center.Z) && std::isfinite(Obstacle.YawRadians);
	if (!bFinite || !(Obstacle.HalfExtents.X > 0.f) || !(Obstacle.HalfExtents.Y > 0.f) || !(Obstacle.HalfExtents.Z > 0.f))
	{
		return std::unexpected(FNavigationError{"Navigation obstacles need a finite placement and positive half extents"});
	}

	if (World.Obstacles.size() >= World.WorldSettings.MaximumObstacles)
	{
		return std::unexpected(FNavigationError{"Too many navigation obstacles"});
	}

	const FNavigationObstacleId Id{World.NextId++};
	World.Obstacles.push_back({.Id = Id, .Placement = Obstacle});
	return Id;
}

void FNavigationWorld::MoveObstacle(const FNavigationObstacleId Obstacle, const FNavigationObstacle& Placement)
{
	const auto Found = std::ranges::find(Implementation->Obstacles, Obstacle, &FImplementation::FObstacleSlot::Id);
	if (Found == Implementation->Obstacles.end() || Found->bRemoved)
	{
		return;
	}

	const FNavigationObstacle& Applied = Found->bDirty ? Found->Placement : Found->Applied;
	const bool bMoved = (Placement.Center - Applied.Center).Length() > ObstacleMoveTolerance || (Placement.HalfExtents - Applied.HalfExtents).Length() > ObstacleMoveTolerance
	                    || std::abs(std::remainder(Placement.YawRadians - Applied.YawRadians, 6.2831853f)) > ObstacleTurnTolerance;
	if (bMoved)
	{
		Found->Placement = Placement;
		Found->bDirty = true;
	}
}

void FNavigationWorld::RemoveObstacle(const FNavigationObstacleId Obstacle)
{
	const auto Found = std::ranges::find(Implementation->Obstacles, Obstacle, &FImplementation::FObstacleSlot::Id);
	if (Found != Implementation->Obstacles.end())
	{
		Found->bRemoved = true;
	}
}

void FNavigationWorld::FImplementation::ApplyObstacles()
{
	// The tile cache takes a bounded number of requests per update; whatever does not fit waits for the next one.
	for (FObstacleSlot& Obstacle : Obstacles)
	{
		if ((!Obstacle.bDirty && !Obstacle.bRemoved) || (Obstacle.Ref != 0 && dtStatusFailed(Cache->removeObstacle(Obstacle.Ref))))
		{
			continue;
		}

		Obstacle.Ref = 0;
		if (Obstacle.bRemoved)
		{
			continue;
		}

		float Center[3];
		float HalfExtents[3];
		Copy(Obstacle.Placement.Center, Center);
		Copy(Obstacle.Placement.HalfExtents, HalfExtents);
		dtObstacleRef Ref = 0;
		if (dtStatusSucceed(Cache->addBoxObstacle(Center, HalfExtents, Obstacle.Placement.YawRadians, &Ref)))
		{
			Obstacle.Ref = Ref;
			Obstacle.Applied = Obstacle.Placement;
			Obstacle.bDirty = false;
		}
	}

	std::erase_if(Obstacles, [](const FObstacleSlot& Obstacle)
	{
		return Obstacle.bRemoved && Obstacle.Ref == 0;
	});

	for (int Rebuild = 0; Rebuild < TileRebuildsPerUpdate; ++Rebuild)
	{
		bool bUpToDate = true;
		Cache->update(0.f, Mesh.get(), &bUpToDate);
		if (bUpToDate)
		{
			break;
		}
	}
}

dtPolyRef FNavigationWorld::FImplementation::FindPolygon(const FVector3& Point, const FVector3& Extents, FVector3* const Nearest) const
{
	float Center[3];
	float HalfExtents[3];
	float Closest[3]{};
	Copy(Point, Center);
	Copy(Extents, HalfExtents);
	dtPolyRef Polygon = 0;
	if (dtStatusFailed(Query->findNearestPoly(Center, HalfExtents, &Filter, &Polygon, Closest)) || Polygon == 0)
	{
		return 0;
	}

	if (Nearest)
	{
		*Nearest = ToVector(Closest);
	}

	return Polygon;
}

FNavigationPath FNavigationWorld::FImplementation::FinishPath(const FVector3& Start, const FVector3& End, const dtPolyRef EndRef, const dtPolyRef* const Polygons, const int Count) const
{
	FNavigationPath Path{.bPartial = Polygons[Count - 1] != EndRef};
	float StartPoint[3];
	float EndPoint[3];
	Copy(Start, StartPoint);
	Copy(End, EndPoint);
	// A partial path ends at the point of its last polygon closest to the goal.
	if (Path.bPartial)
	{
		float Closest[3];
		if (dtStatusSucceed(Query->closestPointOnPoly(Polygons[Count - 1], EndPoint, Closest, nullptr)))
		{
			std::copy_n(Closest, 3, EndPoint);
		}
	}

	std::vector<float> Straight(MaximumStraightPoints * 3);
	int StraightCount = 0;
	if (dtStatusFailed(Query->findStraightPath(StartPoint, EndPoint, Polygons, Count, Straight.data(), nullptr, nullptr, &StraightCount, MaximumStraightPoints)))
	{
		return Path;
	}

	for (int Index = 0; Index < StraightCount; ++Index)
	{
		Path.Points.push_back(ToVector(&Straight[static_cast<std::size_t>(Index) * 3]));
	}

	return Path;
}

std::optional<FVector3> FNavigationWorld::FindNearestPoint(const FVector3& Point, const FVector3& Extents) const
{
	FVector3 Nearest;
	return Implementation->FindPolygon(Point, Extents, &Nearest) != 0 ? std::optional{Nearest} : std::nullopt;
}

std::expected<FNavigationPath, FNavigationError> FNavigationWorld::FindPath(const FVector3& Start, const FVector3& End) const
{
	const FImplementation& World = *Implementation;
	FVector3 StartPoint;
	FVector3 EndPoint;
	const dtPolyRef StartRef = World.FindPolygon(Start, {2.f, 4.f, 2.f}, &StartPoint);
	const dtPolyRef EndRef = World.FindPolygon(End, {2.f, 4.f, 2.f}, &EndPoint);
	if (StartRef == 0 || EndRef == 0)
	{
		return std::unexpected(FNavigationError{"The start or end of the path is not on loaded navigation"});
	}

	float StartValues[3];
	float EndValues[3];
	Copy(StartPoint, StartValues);
	Copy(EndPoint, EndValues);
	std::vector<dtPolyRef> Polygons(World.WorldSettings.MaximumPathPolygons);
	int Count = 0;
	if (dtStatusFailed(World.Query->findPath(StartRef, EndRef, StartValues, EndValues, &World.Filter, Polygons.data(), &Count, static_cast<int>(Polygons.size()))) || Count == 0)
	{
		return std::unexpected(FNavigationError{"No navigation path was found"});
	}

	return World.FinishPath(StartPoint, EndPoint, EndRef, Polygons.data(), Count);
}

std::expected<FNavigationPathRequestId, FNavigationError> FNavigationWorld::RequestPath(const FVector3& Start, const FVector3& End)
{
	FImplementation& World = *Implementation;
	if (World.PathRequests.size() >= World.WorldSettings.MaximumPathRequests)
	{
		return std::unexpected(FNavigationError{"Too many path requests are waiting"});
	}

	const FNavigationPathRequestId Id{World.NextId++};
	World.PathRequests.push_back({.Id = Id, .Start = Start, .End = End});
	return Id;
}

FNavigationPathResult FNavigationWorld::TakePathResult(const FNavigationPathRequestId Request)
{
	auto& Requests = Implementation->PathRequests;
	const auto Found = std::ranges::find(Requests, Request, &FImplementation::FPathRequest::Id);
	if (Found == Requests.end())
	{
		return {.State = ENavigationPathState::Failed};
	}

	if (Found->Result.State == ENavigationPathState::Pending)
	{
		return {};
	}

	FNavigationPathResult Result = std::move(Found->Result);
	Requests.erase(Found);
	return Result;
}

void FNavigationWorld::CancelPath(const FNavigationPathRequestId Request)
{
	std::erase_if(Implementation->PathRequests, [Request](const FImplementation::FPathRequest& Pending)
	{
		return Pending.Id == Request;
	});
}

void FNavigationWorld::FImplementation::AdvancePaths()
{
	int Budget = static_cast<int>(WorldSettings.PathIterationsPerUpdate);
	for (FPathRequest& Request : PathRequests)
	{
		if (Request.Result.State != ENavigationPathState::Pending)
		{
			continue;
		}

		if (Budget <= 0)
		{
			break;
		}

		if (!Request.bStarted)
		{
			// The search starts and ends on the navigation, so its path does too.
			const dtPolyRef StartRef = FindPolygon(Request.Start, {2.f, 4.f, 2.f}, &Request.Start);
			Request.EndRef = FindPolygon(Request.End, {2.f, 4.f, 2.f}, &Request.End);
			float StartValues[3];
			float EndValues[3];
			Copy(Request.Start, StartValues);
			Copy(Request.End, EndValues);
			if (StartRef == 0 || Request.EndRef == 0 || dtStatusFailed(SlicedQuery->initSlicedFindPath(StartRef, Request.EndRef, StartValues, EndValues, &Filter)))
			{
				Request.Result.State = ENavigationPathState::Failed;
				continue;
			}

			Request.bStarted = true;
		}

		int Done = 0;
		const dtStatus Status = SlicedQuery->updateSlicedFindPath(Budget, &Done);
		Budget -= std::max(Done, 1);
		if (dtStatusInProgress(Status))
		{
			// Keeps the sliced query for this request until it finishes.
			break;
		}

		std::vector<dtPolyRef> Polygons(WorldSettings.MaximumPathPolygons);
		int Count = 0;
		if (dtStatusFailed(Status) || dtStatusFailed(SlicedQuery->finalizeSlicedFindPath(Polygons.data(), &Count, static_cast<int>(Polygons.size()))) || Count == 0)
		{
			Request.Result.State = ENavigationPathState::Failed;
			continue;
		}

		Request.Result = {.State = ENavigationPathState::Succeeded, .Path = FinishPath(Request.Start, Request.End, Request.EndRef, Polygons.data(), Count)};
	}
}

dtCrowdAgentParams FNavigationWorld::FImplementation::MakeCrowdParameters(const FNavigationAgentParameters& Parameters) const
{
	dtCrowdAgentParams Result{};
	Result.radius = std::min(Parameters.Radius, MaximumAgentRadius);
	Result.height = Parameters.Height;
	Result.maxAcceleration = Parameters.MaximumAcceleration;
	Result.maxSpeed = Parameters.MaximumSpeed;
	Result.collisionQueryRange = std::max(Parameters.AvoidanceRange, Result.radius * 4.f);
	Result.pathOptimizationRange = Result.radius * 30.f;
	Result.separationWeight = 2.f;
	// An external agent is only something to avoid: the crowd never plans for it.
	Result.updateFlags = Parameters.bExternal ? 0 : DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
	Result.obstacleAvoidanceType = 3;
	Result.queryFilterType = 0;
	Result.userData = nullptr;
	return Result;
}

FNavigationWorld::FImplementation::FAgent* FNavigationWorld::FImplementation::FindAgent(const FNavigationAgentId Id)
{
	const auto Found = std::ranges::find(Agents, Id, &FAgent::Id);
	return Found != Agents.end() ? &*Found : nullptr;
}

const FNavigationWorld::FImplementation::FAgent* FNavigationWorld::FImplementation::FindAgent(const FNavigationAgentId Id) const
{
	const auto Found = std::ranges::find(Agents, Id, &FAgent::Id);
	return Found != Agents.end() ? &*Found : nullptr;
}

void FNavigationWorld::FImplementation::TargetAgent(FAgent& Agent)
{
	if (!Agent.Target || Agent.CrowdIndex < 0)
	{
		Agent.bTargetPending = Agent.Target.has_value();
		return;
	}

	FVector3 Nearest;
	const dtPolyRef Polygon = FindPolygon(*Agent.Target, {2.f, 4.f, 2.f}, &Nearest);
	if (Polygon == 0)
	{
		Agent.bTargetPending = true;
		return;
	}

	float Point[3];
	Copy(Nearest, Point);
	Agent.bTargetPending = !Crowd->requestMoveTarget(Agent.CrowdIndex, Polygon, Point);
}

void FNavigationWorld::FImplementation::PlaceAgents()
{
	// The crowd gives up on an agent whose ground unloads, so agents off the navigation rejoin once there is navigation under them again.
	for (FAgent& Agent : Agents)
	{
		if (Agent.CrowdIndex >= 0)
		{
			const dtCrowdAgent* const Crowded = Crowd->getAgent(Agent.CrowdIndex);
			if (Crowded && Crowded->active && Crowded->state != DT_CROWDAGENT_STATE_INVALID)
			{
				if (Agent.bTargetPending)
				{
					TargetAgent(Agent);
				}

				continue;
			}

			Crowd->removeAgent(Agent.CrowdIndex);
			Agent.CrowdIndex = -1;
		}

		const float Extents[3]{Agent.Parameters.Radius * 2.f, Agent.Parameters.Height * 0.5f + Settings.AgentMaxClimb, Agent.Parameters.Radius * 2.f};
		if (FindPolygon(Agent.Position, ToVector(Extents)) == 0)
		{
			continue;
		}

		float Position[3];
		Copy(Agent.Position, Position);
		const dtCrowdAgentParams Parameters = MakeCrowdParameters(Agent.Parameters);
		Agent.CrowdIndex = Crowd->addAgent(Position, &Parameters);
		TargetAgent(Agent);
	}
}

void FNavigationWorld::Update(const float DeltaSeconds)
{
	FImplementation& World = *Implementation;
	World.ApplyObstacles();
	World.AdvancePaths();
	World.PlaceAgents();
	for (const FImplementation::FAgent& Agent : World.Agents)
	{
		dtCrowdAgent* const Crowded = Agent.CrowdIndex >= 0 ? World.Crowd->getEditableAgent(Agent.CrowdIndex) : nullptr;
		if (Crowded && Agent.Parameters.bExternal)
		{
			Copy(Agent.Velocity, Crowded->vel);
		}
	}

	if (DeltaSeconds > 0.f)
	{
		World.Crowd->update(DeltaSeconds, nullptr);
	}

	for (FImplementation::FAgent& Agent : World.Agents)
	{
		// The crowd moves its own agents; external ones are wherever their owner last put them.
		const dtCrowdAgent* const Crowded = Agent.CrowdIndex >= 0 ? World.Crowd->getAgent(Agent.CrowdIndex) : nullptr;
		if (Crowded && Crowded->active && Crowded->state != DT_CROWDAGENT_STATE_INVALID && !Agent.Parameters.bExternal)
		{
			Agent.Position = ToVector(Crowded->npos);
			Agent.Velocity = ToVector(Crowded->vel);
		}
	}
}

std::expected<FNavigationAgentId, FNavigationError> FNavigationWorld::AddAgent(const FVector3& Position, const FNavigationAgentParameters& Parameters)
{
	FImplementation& World = *Implementation;
	if (World.Agents.size() >= World.WorldSettings.MaximumAgents)
	{
		return std::unexpected(FNavigationError{"Too many navigation agents"});
	}

	if (!(Parameters.Radius > 0.f) || !(Parameters.Height > 0.f) || !(Parameters.MaximumSpeed >= 0.f) || !(Parameters.MaximumAcceleration > 0.f) || !std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z))
	{
		return std::unexpected(FNavigationError{"Navigation agents need a finite position, a positive size and acceleration, and a speed of at least zero"});
	}

	const FNavigationAgentId Id{World.NextId++};
	World.Agents.push_back({.Id = Id, .Parameters = Parameters, .Position = Position});
	World.PlaceAgents();
	return Id;
}

void FNavigationWorld::RemoveAgent(const FNavigationAgentId Agent)
{
	FImplementation& World = *Implementation;
	const auto Found = std::ranges::find(World.Agents, Agent, &FImplementation::FAgent::Id);
	if (Found == World.Agents.end())
	{
		return;
	}

	if (Found->CrowdIndex >= 0)
	{
		World.Crowd->removeAgent(Found->CrowdIndex);
	}

	World.Agents.erase(Found);
}

void FNavigationWorld::SetAgentTarget(const FNavigationAgentId Agent, const FVector3& Target)
{
	if (FImplementation::FAgent* const Found = Implementation->FindAgent(Agent); Found && !Found->Parameters.bExternal)
	{
		Found->Target = Target;
		Implementation->TargetAgent(*Found);
	}
}

void FNavigationWorld::StopAgent(const FNavigationAgentId Agent)
{
	FImplementation::FAgent* const Found = Implementation->FindAgent(Agent);
	if (!Found)
	{
		return;
	}

	Found->Target.reset();
	Found->bTargetPending = false;
	if (Found->CrowdIndex >= 0)
	{
		Implementation->Crowd->resetMoveTarget(Found->CrowdIndex);
	}
}

void FNavigationWorld::SetAgentMaximumSpeed(const FNavigationAgentId Agent, const float MaximumSpeed)
{
	FImplementation::FAgent* const Found = Implementation->FindAgent(Agent);
	if (!Found || !std::isfinite(MaximumSpeed) || MaximumSpeed < 0.f || Found->Parameters.MaximumSpeed == MaximumSpeed)
	{
		return;
	}

	Found->Parameters.MaximumSpeed = MaximumSpeed;
	if (Found->CrowdIndex >= 0)
	{
		const dtCrowdAgentParams Parameters = Implementation->MakeCrowdParameters(Found->Parameters);
		Implementation->Crowd->updateAgentParameters(Found->CrowdIndex, &Parameters);
	}
}

void FNavigationWorld::SetAgentPosition(const FNavigationAgentId Agent, const FVector3& Position, const FVector3& Velocity)
{
	FImplementation::FAgent* const Found = Implementation->FindAgent(Agent);
	if (!Found || !std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z))
	{
		return;
	}

	Found->Position = Position;
	Found->Velocity = Velocity;
	if (dtCrowdAgent* const Crowded = Found->CrowdIndex >= 0 ? Implementation->Crowd->getEditableAgent(Found->CrowdIndex) : nullptr)
	{
		Copy(Position, Crowded->npos);
	}
}

std::optional<FNavigationAgentState> FNavigationWorld::GetAgent(const FNavigationAgentId Agent) const
{
	const FImplementation::FAgent* const Found = Implementation->FindAgent(Agent);
	if (!Found)
	{
		return std::nullopt;
	}

	FNavigationAgentState State{.Position = Found->Position, .bHasTarget = Found->Target.has_value()};
	const dtCrowdAgent* const Crowded = Found->CrowdIndex >= 0 ? Implementation->Crowd->getAgent(Found->CrowdIndex) : nullptr;
	if (!Crowded || !Crowded->active || Crowded->state == DT_CROWDAGENT_STATE_INVALID)
	{
		return State;
	}

	State.bOnNavigation = true;
	if (!Found->Parameters.bExternal)
	{
		State.DesiredVelocity = ToVector(Crowded->vel);
	}

	if (Found->Target && !Found->bTargetPending && Crowded->targetState == DT_CROWDAGENT_TARGET_VALID)
	{
		State.bPartial = Crowded->partial;
		const FVector3 End = ToVector(Crowded->corridor.getTarget());
		State.bArrived = HorizontalDistance(Found->Position, End) <= std::max(Found->Parameters.Radius, 0.25f) && std::abs(Found->Position.Y - End.Y) < Found->Parameters.Height;
	}

	return State;
}

const FNavigationSettings& FNavigationWorld::GetSettings() const noexcept
{
	return Implementation->Settings;
}

FNavigationStats FNavigationWorld::GetStats() const
{
	const FImplementation& World = *Implementation;
	FNavigationStats Stats{.Sources = static_cast<std::uint32_t>(World.Sources.size()), .Agents = static_cast<std::uint32_t>(World.Agents.size()), .PathRequests = static_cast<std::uint32_t>(World.PathRequests.size())};
	for (const std::unique_ptr<FImplementation::FSource>& Source : World.Sources)
	{
		for (const auto& [Tile, Loaded] : Source->Loaded)
		{
			Stats.Layers += static_cast<std::uint32_t>(Loaded.size());
		}
	}

	const dtNavMesh& Mesh = *World.Mesh;
	for (int Index = 0; Index < Mesh.getMaxTiles(); ++Index)
	{
		const dtMeshTile* const Tile = Mesh.getTile(Index);
		if (Tile && Tile->header)
		{
			++Stats.Tiles;
			Stats.Polygons += static_cast<std::uint32_t>(Tile->header->polyCount);
		}
	}

	for (const FImplementation::FObstacleSlot& Obstacle : World.Obstacles)
	{
		Stats.Obstacles += Obstacle.bRemoved ? 0u : 1u;
		Stats.PendingObstacles += Obstacle.bDirty || Obstacle.bRemoved ? 1u : 0u;
	}

	return Stats;
}

void FNavigationWorld::GetDebugTriangles(std::vector<FVector3>& OutTriangles) const
{
	const dtNavMesh& Mesh = *Implementation->Mesh;
	for (int TileIndex = 0; TileIndex < Mesh.getMaxTiles(); ++TileIndex)
	{
		const dtMeshTile* const Tile = Mesh.getTile(TileIndex);
		if (!Tile || !Tile->header)
		{
			continue;
		}

		for (int PolygonIndex = 0; PolygonIndex < Tile->header->polyCount; ++PolygonIndex)
		{
			const dtPoly& Polygon = Tile->polys[PolygonIndex];
			if (Polygon.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				continue;
			}

			const dtPolyDetail& Detail = Tile->detailMeshes[PolygonIndex];
			for (int Triangle = 0; Triangle < Detail.triCount; ++Triangle)
			{
				const unsigned char* const Corners = &Tile->detailTris[(Detail.triBase + static_cast<unsigned int>(Triangle)) * 4];
				for (int Corner = 0; Corner < 3; ++Corner)
				{
					const float* const Vertex = Corners[Corner] < Polygon.vertCount ? &Tile->verts[Polygon.verts[Corners[Corner]] * 3] : &Tile->detailVerts[(Detail.vertBase + Corners[Corner] - Polygon.vertCount) * 3];
					OutTriangles.push_back(ToVector(Vertex));
				}
			}
		}
	}
}
}
