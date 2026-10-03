#pragma once

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/Scene/SceneSerialization.h"
#include "PreviewScene.h"

#include <filesystem>
#include <memory>

namespace Herta
{
// Im3d edits a transient float view. Saved data and stable identities belong to World.
class FEditorScene final
{
public:
	FEditorScene();
	[[nodiscard]] std::expected<void, FSceneError> Load(const std::filesystem::path& Path);
	[[nodiscard]] std::expected<void, FSceneError> Save(const std::filesystem::path& Path = {});
	[[nodiscard]] std::expected<void, FSceneError> CommitEdits();
	void SetPath(std::filesystem::path Path);
	void SetSimulationRunning(bool bRunning);
	std::optional<std::size_t> FindBody(ESceneBodyMotion Motion) const;
	std::vector<FPreviewObject>& GetObjects();
	const std::vector<FPreviewObject>& GetObjects() const;
	std::uint64_t GetGeneration() const;
	const std::filesystem::path& GetPath() const;
	const FWorld& GetWorld() const;

private:
	FWorld World;
	FObjectId Id;
	std::string Name;
	std::filesystem::path CurrentPath;
	std::vector<FPreviewObject> Objects;
	std::uint64_t Generation = 1;
	bool bSimulationRunning = false;
};

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterEditorSceneCommands(FEditorCommandRegistry& Commands, const std::shared_ptr<FEditorScene>& Scene);
}
