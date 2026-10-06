#pragma once

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/TransactionHistory.h"
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
	explicit FEditorScene(std::size_t MaximumTransactions = 256, std::size_t MaximumMemoryCost = 64 * 1024 * 1024);
	[[nodiscard]] std::expected<void, FSceneError> Load(const std::filesystem::path& Path);
	[[nodiscard]] std::expected<void, FSceneError> LoadDocument(FSceneDocument Document, const std::filesystem::path& Path);
	[[nodiscard]] std::expected<void, FSceneError> Save(const std::filesystem::path& Path = {});
	[[nodiscard]] std::expected<void, FSceneError> BeginEdit(std::string_view Label);
	[[nodiscard]] std::expected<void, FSceneError> EndEdit();
	[[nodiscard]] std::expected<void, FSceneError> CancelEdit();
	bool HasActiveEdit() const;
	[[nodiscard]] std::expected<void, FSceneError> CommitEdits(std::string_view Label = "Edit objects");
	[[nodiscard]] std::expected<void, FSceneError> Undo();
	[[nodiscard]] std::expected<void, FSceneError> Redo();
	bool CanUndo() const;
	bool CanRedo() const;
	bool IsDirty() const;
	std::string_view GetUndoLabel() const;
	std::string_view GetRedoLabel() const;
	void SetSelection(std::span<const FObjectId> Selected, std::optional<FObjectId> Active = std::nullopt);
	std::span<const FObjectId> GetSelection() const;
	std::optional<FObjectId> GetActiveObject() const;
	[[nodiscard]] std::expected<FObjectId, FSceneError> CreateEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FSceneError> CreateMeshEntity(FAssetId Asset, std::string_view Label, const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<FObjectId, FSceneError> CreateEmptyEntity(const FWorldPosition& Position = {});
	[[nodiscard]] std::expected<void, FSceneError> AddStaticMeshToSelected(FAssetId Asset);
	[[nodiscard]] std::expected<void, FSceneError> RemoveStaticMeshFromSelected();
	[[nodiscard]] std::expected<void, FSceneError> AddRigidBodyToSelected(ESceneBodyType Type = ESceneBodyType::Dynamic);
	[[nodiscard]] std::expected<void, FSceneError> SetSelectedBodyType(ESceneBodyType Type);
	[[nodiscard]] std::expected<void, FSceneError> SetSelectedBodyProperty(float FSceneRigidBodySettings::* Property, float Value);
	[[nodiscard]] std::expected<void, FSceneError> DuplicateSelected(bool bWithinActiveEdit = false, const FVector3d& WorldOffset = {});
	[[nodiscard]] std::expected<void, FSceneError> DeleteSelected();
	[[nodiscard]] std::expected<void, FSceneError> ReparentEntities(std::span<const FObjectId> Entities, std::optional<FObjectId> Parent);
	[[nodiscard]] std::expected<void, FSceneError> ReparentSelected(std::optional<FObjectId> Parent);
	[[nodiscard]] std::expected<std::string, FSceneError> CopySelected() const;
	[[nodiscard]] std::expected<void, FSceneError> PasteEntities(std::string_view Text);
	void SetPath(std::filesystem::path Path);
	void SetSimulationRunning(bool bRunning);
	[[nodiscard]] std::expected<void, FSceneError> UpdatePreviewHierarchy(std::span<const FObjectId> OverrideWorldPoses);
	std::vector<std::size_t> FindBodies(ESceneBodyType Type) const;
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
		std::vector<FSceneEntity> Before;
		std::vector<FObjectId> Selection;
		std::optional<FObjectId> Active;
		bool bDuplicated = false;
	};

	void RebuildObjects();
	void RestoreObjects(bool bNotify = true);
	FEditorTransaction MakeTransaction(std::string_view Label, const std::vector<FSceneEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, std::optional<FObjectId> BeforeActive, const std::vector<FObjectId>& AfterSelection, std::optional<FObjectId> AfterActive);
	[[nodiscard]] std::expected<void, FSceneError> RecordChanges(std::string_view Label, const std::vector<FSceneEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, std::optional<FObjectId> BeforeActive);
	[[nodiscard]] std::expected<void, FSceneError> ApplyStructuralChanges(std::string_view Label, const std::vector<FSceneEntityChange>& Changes, const std::vector<FObjectId>& AfterSelection, std::optional<FObjectId> AfterActive = std::nullopt);
	[[nodiscard]] std::expected<FObjectId, FSceneError> InsertEntity(FSceneEntity Entity);
	[[nodiscard]] std::expected<void, FSceneError> ApplySelectedMesh(std::optional<FStaticMeshComponent> Mesh);
	[[nodiscard]] std::expected<void, FSceneError> ApplySelectedBodyType(ESceneBodyType Type, bool bOnlyAbsent);
	[[nodiscard]] std::expected<void, FSceneError> CheckAuthoringAllowed(bool bAllowActiveEdit = false) const;

	FWorld World;
	FObjectId Id;
	std::string Name;
	std::filesystem::path CurrentPath;
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

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterEditorSceneCommands(FEditorCommandRegistry& Commands, const std::shared_ptr<FEditorScene>& Scene);
}
