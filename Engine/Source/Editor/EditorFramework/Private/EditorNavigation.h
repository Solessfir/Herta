#pragma once

#include "Herta/Level/LevelSerialization.h"
#include "Herta/NavigationBuilder/NavigationBuilder.h"
#include "Herta/Tasks/TaskSystem.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
struct FEditorNavigationInputs
{
	FLevelDocument Level{};
	// Parallel to Level.Regions.
	std::vector<std::optional<FLevelDocument>> Regions{};
	std::map<FAssetId, FNavigationBox> Bounds{};
};

// Builds the open level's navigation on a worker, as staging would cook it, so the viewport can show it and Play can walk it. A newer
// request while one builds waits for it and then builds once, so dragging a wall never queues a build per frame.
class FEditorNavigation final
{
public:
	[[nodiscard]] static std::unique_ptr<FEditorNavigation> Create(FTaskSystem& Tasks);

	~FEditorNavigation();

	FEditorNavigation(const FEditorNavigation&) = delete;
	FEditorNavigation& operator=(const FEditorNavigation&) = delete;
	FEditorNavigation(FEditorNavigation&&) = delete;
	FEditorNavigation& operator=(FEditorNavigation&&) = delete;

	// Builds for Key unless that is the key already built or building.
	void Request(std::uint64_t Key, FEditorNavigationInputs Inputs);
	// Publishes a finished build and starts the next waiting one; true when a new build was published.
	bool Poll();

	[[nodiscard]] std::optional<std::uint64_t> GetKey() const noexcept;
	[[nodiscard]] bool IsBuilding() const noexcept;
	// Null when the level has no Navigation Bounds, or before the first build.
	[[nodiscard]] const std::shared_ptr<const FNavigationFile>& GetFile() const noexcept;
	// The built level and every region patch, three points per triangle.
	[[nodiscard]] std::span<const FVector3> GetDebugTriangles() const noexcept;
	[[nodiscard]] const std::string& GetError() const noexcept;
	[[nodiscard]] double GetBuildMilliseconds() const noexcept;

private:
	struct FBuild
	{
		std::uint64_t Key = 0;
		std::expected<std::optional<FNavigationFile>, FNavigationError> Result{};
		std::vector<FVector3> Triangles{};
		double Milliseconds = 0.;
	};

	FEditorNavigation(FTaskSystem& InTasks, std::unique_ptr<FTaskScope> InScope);
	void Start(std::uint64_t Key, FEditorNavigationInputs Inputs);

	FTaskSystem& Tasks;
	std::unique_ptr<FTaskScope> Scope;
	FTaskHandle Running{};
	std::shared_ptr<FBuild> Building{};
	std::optional<std::pair<std::uint64_t, FEditorNavigationInputs>> Waiting{};

	std::optional<std::uint64_t> Key{};
	std::optional<std::uint64_t> BuildingKey{};
	std::shared_ptr<const FNavigationFile> File{};
	std::vector<FVector3> Triangles{};
	std::string Error{};
	double Milliseconds = 0.;
};
}
