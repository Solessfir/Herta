#pragma once

#include "Herta/Navigation/NavigationData.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// Capacities, fixed at creation. Tiles and layers bound how much navigation can be loaded at once, not how large the world is.
struct FNavigationWorldSettings
{
	std::uint32_t MaximumTiles = 4096;
	std::uint32_t MaximumLayers = 8192;
	std::uint32_t MaximumObstacles = 1024;
	std::uint32_t MaximumAgents = 128;
	std::uint32_t MaximumPathRequests = 64;
	// Search nodes path requests may expand per Update, shared in request order, so long searches spread over frames.
	std::uint32_t PathIterationsPerUpdate = 1024;
	// Polygons on one path; longer paths come back partial toward the goal.
	std::uint32_t MaximumPathPolygons = 512;
};

struct FNavigationSourceId
{
	auto operator<=>(const FNavigationSourceId&) const = default;

	std::uint32_t Value = 0;
};

struct FNavigationObstacleId
{
	auto operator<=>(const FNavigationObstacleId&) const = default;

	std::uint32_t Value = 0;
};

struct FNavigationAgentId
{
	auto operator<=>(const FNavigationAgentId&) const = default;

	std::uint32_t Value = 0;
};

struct FNavigationPathRequestId
{
	auto operator<=>(const FNavigationPathRequestId&) const = default;

	std::uint32_t Value = 0;
};

// A box that cuts walkable area out of the tiles it touches, rotated about the vertical axis, such as a crate or an unbroken wall.
struct FNavigationObstacle
{
	bool operator==(const FNavigationObstacle&) const = default;

	FVector3 Center{};
	FVector3 HalfExtents{0.5f, 0.5f, 0.5f};
	float YawRadians = 0.f;
};

struct FNavigationPath
{
	// Corners from the start to the end, both on the navigation, where a straight walk between each pair stays on it.
	std::vector<FVector3> Points{};
	// The goal could not be reached, so the path ends at the closest point to it.
	bool bPartial = false;
};

enum class ENavigationPathState : std::uint8_t
{
	Pending,
	Succeeded,
	Failed,
};

struct FNavigationPathResult
{
	ENavigationPathState State = ENavigationPathState::Pending;
	FNavigationPath Path{};
};

struct FNavigationAgentParameters
{
	float Radius = 0.3f;
	float Height = 1.8f;
	float MaximumSpeed = 3.5f;
	float MaximumAcceleration = 8.f;
	// How far ahead the agent steers around others, in meters.
	float AvoidanceRange = 2.5f;
	// Moved by its owner through SetAgentPosition, such as the player: crowd agents avoid it but navigation never steers it.
	bool bExternal = false;
};

struct FNavigationAgentState
{
	// Where the crowd moved the agent, or where its owner last reported it.
	FVector3 Position{};
	// The agent's velocity this update, steering around others; an owner that moves the agent itself, such as a physics character, moves it
	// by this and reports where it ended up.
	FVector3 DesiredVelocity{};
	// Off the loaded navigation, such as on a region that streamed out; the agent rejoins once navigation under it loads.
	bool bOnNavigation = false;
	bool bHasTarget = false;
	// Within the arrival distance of its target.
	bool bArrived = false;
	// The target cannot be reached; the agent heads for the closest point.
	bool bPartial = false;
};

struct FNavigationStats
{
	std::uint32_t Sources = 0;
	std::uint32_t Layers = 0;
	std::uint32_t Tiles = 0;
	std::uint32_t Polygons = 0;
	std::uint32_t Obstacles = 0;
	// Obstacle changes whose tiles have not been rebuilt yet.
	std::uint32_t PendingObstacles = 0;
	std::uint32_t Agents = 0;
	std::uint32_t PathRequests = 0;
};

// Loaded navigation: tiles from any number of sources joined on one grid, temporary obstacles that rebuild only the tiles they touch, path
// queries, and a crowd that steers agents around each other. Everything advances in Update, so with the same calls in the same order the
// results are the same on every run.
class FNavigationWorld final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FNavigationWorld>, FNavigationError> Create(const FNavigationSettings& Settings = {}, const FNavigationWorldSettings& WorldSettings = {});

	~FNavigationWorld();

	FNavigationWorld(const FNavigationWorld&) = delete;
	FNavigationWorld& operator=(const FNavigationWorld&) = delete;
	FNavigationWorld(FNavigationWorld&&) = delete;
	FNavigationWorld& operator=(FNavigationWorld&&) = delete;

	// Adds cooked tiles built with the world's settings. Where sources share a tile, the newest covers the older ones until it is removed, as a
	// streamed region's patch covers its level's tiles.
	[[nodiscard]] std::expected<FNavigationSourceId, FNavigationError> AddData(const FNavigationData& Data);
	void RemoveData(FNavigationSourceId Source);

	// Obstacles cut their tiles over the next Updates, a few tiles at a time, and apply to tiles that load later too.
	[[nodiscard]] std::expected<FNavigationObstacleId, FNavigationError> AddObstacle(const FNavigationObstacle& Obstacle);
	// Re-cuts only when the obstacle moved or turned enough to matter, so a body settling in place does not rebuild tiles every frame.
	void MoveObstacle(FNavigationObstacleId Obstacle, const FNavigationObstacle& Placement);
	void RemoveObstacle(FNavigationObstacleId Obstacle);

	// Rebuilds tiles under changed obstacles, advances path requests, and steps the crowd.
	void Update(float DeltaSeconds);

	// The closest point on the navigation within Extents of Point.
	[[nodiscard]] std::optional<FVector3> FindNearestPoint(const FVector3& Point, const FVector3& Extents = {2.f, 4.f, 2.f}) const;
	// A path searched all at once. Prefer RequestPath for anything that runs often.
	[[nodiscard]] std::expected<FNavigationPath, FNavigationError> FindPath(const FVector3& Start, const FVector3& End) const;
	// Searches over the next Updates within PathIterationsPerUpdate. Tiles that unload under a search fail it.
	[[nodiscard]] std::expected<FNavigationPathRequestId, FNavigationError> RequestPath(const FVector3& Start, const FVector3& End);
	// The request's result; a finished request is forgotten once read.
	[[nodiscard]] FNavigationPathResult TakePathResult(FNavigationPathRequestId Request);
	void CancelPath(FNavigationPathRequestId Request);

	[[nodiscard]] std::expected<FNavigationAgentId, FNavigationError> AddAgent(const FVector3& Position, const FNavigationAgentParameters& Parameters);
	void RemoveAgent(FNavigationAgentId Agent);
	void SetAgentTarget(FNavigationAgentId Agent, const FVector3& Target);
	void StopAgent(FNavigationAgentId Agent);
	void SetAgentMaximumSpeed(FNavigationAgentId Agent, float MaximumSpeed);
	// Where the agent really is after its owner moved it, such as by a physics character.
	void SetAgentPosition(FNavigationAgentId Agent, const FVector3& Position, const FVector3& Velocity);
	[[nodiscard]] std::optional<FNavigationAgentState> GetAgent(FNavigationAgentId Agent) const;

	[[nodiscard]] const FNavigationSettings& GetSettings() const noexcept;
	[[nodiscard]] FNavigationStats GetStats() const;
	// Every loaded polygon as triangles, three points each, for debug drawing.
	void GetDebugTriangles(std::vector<FVector3>& OutTriangles) const;

private:
	struct FImplementation;
	explicit FNavigationWorld(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
