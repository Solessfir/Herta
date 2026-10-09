#pragma once

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/TransactionHistory.h"
#include "Herta/Level/LevelDescriptors.h"
#include "Herta/Level/LevelSerialization.h"
#include "PreviewLevel.h"

#include <filesystem>
#include <memory>

namespace Herta
{
// Im3d edits a transient float view. Saved data and stable identities belong to World.
class FEditorLevel final
{
public:
	explicit FEditorLevel(std::size_t MaximumTransactions = 256, std::size_t MaximumMemoryCost = 64 * 1024 * 1024);
	[[nodiscard]] std::expected<void, FLevelError> Load(const std::filesystem::path& Path);
	[[nodiscard]] std::expected<void, FLevelError> LoadDocument(FLevelDocument Document, const std::filesystem::path& Path);
	[[nodiscard]] std::expected<void, FLevelError> Save(const std::filesystem::path& Path = {});
	[[nodiscard]] std::expected<void, FLevelError> BeginEdit(std::string_view Label);
	[[nodiscard]] std::expected<void, FLevelError> EndEdit();
	[[nodiscard]] std::expected<void, FLevelError> CancelEdit();
	bool HasActiveEdit() const;
	[[nodiscard]] std::expected<void, FLevelError> CommitEdits(std::string_view Label = "Edit objects");
	[[nodiscard]] std::expected<void, FLevelError> Undo();
	[[nodiscard]] std::expected<void, FLevelError> Redo();
	bool CanUndo() const;
	bool CanRedo() const;
	bool IsDirty() const;
	std::string_view GetUndoLabel() const;
	std::string_view GetRedoLabel() const;
	void SetSelection(std::span<const FObjectId> Selected, std::optional<FObjectId> Active = std::nullopt);
	std::span<const FObjectId> GetSelection() const;
	std::optional<FObjectId> GetActiveObject() const;
	std::span<const FLevelFolder> GetFolders() const;
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateFolder(std::optional<FObjectId> Parent = {}, std::span<const FObjectId> Entities = {});
	[[nodiscard]] std::expected<void, FLevelError> RenameFolder(FObjectId Folder, std::string_view Name);
	[[nodiscard]] std::expected<void, FLevelError> DeleteFolder(FObjectId Folder);
	[[nodiscard]] std::expected<void, FLevelError> MoveFolder(FObjectId Folder, std::optional<FObjectId> Parent);
	[[nodiscard]] std::expected<void, FLevelError> MoveEntitiesToFolder(std::span<const FObjectId> Entities, std::optional<FObjectId> Folder);
	std::span<const FLevelCameraBookmark> GetCameraBookmarks() const;
	// Adds the bookmark or replaces the one in its slot.
	[[nodiscard]] std::expected<void, FLevelError> SetCameraBookmark(FLevelCameraBookmark Bookmark);
	[[nodiscard]] std::expected<void, FLevelError> RemoveCameraBookmark(std::uint32_t Slot);
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateMeshEntity(FAssetId Asset, std::string_view Label, const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateEmptyEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<void, FLevelError> AddStaticMeshToSelected(FAssetId Asset);
	[[nodiscard]] std::expected<void, FLevelError> RemoveStaticMeshFromSelected();
	[[nodiscard]] std::expected<void, FLevelError> AddRigidBodyToSelected(ELevelBodyType Type = ELevelBodyType::Dynamic);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedBodyType(ELevelBodyType Type);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedBodyProperty(float FLevelRigidBodySettings::*Property, float Value);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedCollisionShape(ELevelCollisionShape Shape);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedMaterial(std::size_t Slot, FAssetId Asset);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedVisualProperty(ELevelComponentType Type, std::string_view Key, const FLevelPropertyValue& Value);
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateLightEntity(ELightType Type, const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateSkyAtmosphereEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateHeightFogEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<void, FLevelError> AddLightToSelected(ELightType Type);
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedLight(std::optional<FLightComponent> Light);
	[[nodiscard]] std::expected<void, FLevelError> AddSkyAtmosphereToSelected();
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedSkyAtmosphere(std::optional<FSkyAtmosphereComponent> Atmosphere);
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateSoftBodyEntity(ESoftBodyShape Shape, const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<void, FLevelError> AddSoftBodyToSelected();
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedSoftBody(std::optional<FSoftBodyComponent> SoftBody);
	[[nodiscard]] std::expected<FObjectId, FLevelError> CreateTriggerEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<void, FLevelError> AddMoverToSelected();
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedMover(std::optional<FMoverComponent> Mover);
	[[nodiscard]] std::expected<void, FLevelError> AddTriggerToSelected();
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedTrigger(std::optional<FTriggerComponent> Trigger);
	[[nodiscard]] std::expected<void, FLevelError> AddHeightFogToSelected();
	[[nodiscard]] std::expected<void, FLevelError> SetSelectedHeightFog(std::optional<FHeightFogComponent> Fog);
	[[nodiscard]] std::expected<void, FLevelError> DuplicateSelected(bool bWithinActiveEdit = false, const FVector3d& WorldOffset = {});
	[[nodiscard]] std::expected<void, FLevelError> DeleteSelected();
	[[nodiscard]] std::expected<void, FLevelError> ReparentEntities(std::span<const FObjectId> Entities, std::optional<FObjectId> Parent);
	[[nodiscard]] std::expected<void, FLevelError> ReparentSelected(std::optional<FObjectId> Parent);
	[[nodiscard]] std::expected<std::string, FLevelError> CopySelected() const;
	[[nodiscard]] std::expected<void, FLevelError> PasteEntities(std::string_view Text);
	void SetPath(std::filesystem::path Path);
	void SetSimulationRunning(bool bRunning);
	[[nodiscard]] std::expected<void, FLevelError> UpdatePreviewHierarchy(std::span<const FObjectId> OverrideWorldPoses);
	std::vector<std::size_t> FindBodies(ELevelBodyType Type) const;
	std::vector<FPreviewObject>& GetObjects();
	const std::vector<FPreviewObject>& GetObjects() const;
	std::uint64_t GetGeneration() const;
	const std::filesystem::path& GetPath() const;
	std::string_view GetName() const;
	const FWorld& GetWorld() const;

private:
	struct FActiveEdit
	{
		std::string Label;
		std::vector<FLevelEntity> Before;
		std::vector<FLevelFolder> BeforeFolders;
		std::vector<FObjectId> Selection;
		std::optional<FObjectId> Active;
		bool bDuplicated = false;
	};

	void RebuildObjects();
	void RestoreObjects(bool bNotify = true);
	FEditorTransaction MakeTransaction(std::string_view Label, const std::vector<FLevelEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, std::optional<FObjectId> BeforeActive, const std::vector<FObjectId>& AfterSelection, std::optional<FObjectId> AfterActive, std::optional<std::vector<FLevelFolder>> BeforeFolders = {}, std::optional<std::vector<FLevelFolder>> AfterFolders = {});
	[[nodiscard]] std::expected<void, FLevelError> RecordChanges(std::string_view Label, const std::vector<FLevelEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, std::optional<FObjectId> BeforeActive, std::optional<std::vector<FLevelFolder>> BeforeFolders = {});
	[[nodiscard]] std::expected<void, FLevelError> ApplyStructuralChanges(std::string_view Label, const std::vector<FLevelEntityChange>& Changes, const std::vector<FObjectId>& AfterSelection, std::optional<FObjectId> AfterActive = std::nullopt, std::optional<std::vector<FLevelFolder>> AfterFolders = {});
	[[nodiscard]] std::expected<void, FLevelError> ApplyAuthoringChanges(std::span<const FLevelEntityChange> Changes, const std::optional<std::vector<FLevelFolder>>& AfterFolders);
	[[nodiscard]] std::expected<FObjectId, FLevelError> InsertEntity(FLevelEntity Entity);
	[[nodiscard]] std::expected<void, FLevelError> ApplySelectedMesh(std::optional<FStaticMeshComponent> Mesh);
	[[nodiscard]] std::expected<void, FLevelError> ApplySelectedBodyType(ELevelBodyType Type, bool bOnlyAbsent);
	template <typename T> [[nodiscard]] std::expected<void, FLevelError> ApplySelectedComponent(std::optional<T> FLevelEntity::*Member, std::optional<T> Value, std::string_view Label, bool bOnlyAbsent = false);
	[[nodiscard]] std::expected<void, FLevelError> CheckAuthoringAllowed(bool bAllowActiveEdit = false) const;
	[[nodiscard]] std::expected<void, FLevelError> ApplyCameraBookmarks(std::string_view Label, std::vector<FLevelCameraBookmark> After);

	FWorld World;
	FObjectId Id;
	std::string Name;
	std::filesystem::path CurrentPath;
	std::vector<FLevelFolder> Folders;
	std::vector<FLevelCameraBookmark> CameraBookmarks;
	std::vector<FPreviewObject> Objects;
	std::vector<FPreviewObject> AuthoredObjects;
	std::uint64_t Generation = 1;
	bool bSimulationRunning = false;

	std::vector<FObjectId> Selection;
	std::optional<FObjectId> ActiveObject;
	std::optional<FActiveEdit> ActiveEdit;

	// Transactions capture this object, so their callbacks are destroyed before the world and editing state.
	FEditorTransactionHistory History;
};

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterEditorLevelCommands(FEditorCommandRegistry& Commands, const std::shared_ptr<FEditorLevel>& Level);
}
