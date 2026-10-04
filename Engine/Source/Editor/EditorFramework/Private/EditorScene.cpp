#include "EditorScene.h"

#include "Herta/EditorCore/CommandRegistry.h"

#include <cmath>
#include <exception>
#include <format>
#include <unordered_set>

namespace Herta
{
namespace
{
constexpr FObjectId DefaultSceneId{0x10935c1f4f594b12, 0x9a139dca302101e0};
constexpr FObjectId DefaultCubeId{0x21935c1f4f594b12, 0x9a139dca302101e1};
constexpr FObjectId DefaultFloorId{0x31935c1f4f594b12, 0x9a139dca302101e2};

bool ContainsObjectId(const std::span<const FObjectId> Objects, const FObjectId Id)
{
	// Predicate lookup avoids the MSVC STL's unsupported 16-byte vectorized equality path under ClangCL.
	return std::ranges::any_of(Objects, [Id](const FObjectId Object)
	{
		return Object == Id;
	});
}

Im3d::Mat3 ToEditorRotation(const FQuaternion& Rotation)
{
	const FMatrix3 Matrix = FMatrix3::Rotation(Rotation);
	Im3d::Mat3 Result{1.f};

	for (std::size_t Column = 0; Column < 3; ++Column)
	{
		for (std::size_t Row = 0; Row < 3; ++Row)
		{
			Result(static_cast<int>(Row), static_cast<int>(Column)) = Matrix(Row, Column);
		}
	}

	return Result;
}

FQuaternion ToSceneRotation(const Im3d::Mat3& Rotation)
{
	const Im3d::Vec3 Euler = Im3d::ToEulerXYZ(Rotation);
	return FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, Euler.z) * FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, Euler.y) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, Euler.x);
}

FPreviewObject ToEditorObject(const FSceneEntity& Entity)
{
	return {
	    .Label = Entity.Name,
	    .Translation = {static_cast<float>(Entity.Transform.Translation.Meters.X), static_cast<float>(Entity.Transform.Translation.Meters.Y), static_cast<float>(Entity.Transform.Translation.Meters.Z)},
	    .Rotation = ToEditorRotation(Entity.Transform.Rotation),
	    .Scale = {Entity.Transform.Scale.X, Entity.Transform.Scale.Y, Entity.Transform.Scale.Z},
	    .Mesh = Entity.Mesh ? Entity.Mesh->Asset : FAssetId{},
	    .Id = Entity.Id,
	};
}

std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::expected<void, FSceneError> ValidateEditorEntityRange(const FSceneEntity& Entity)
{
	const auto& Position = Entity.Transform.Translation.Meters;
	const auto& Scale = Entity.Transform.Scale;
	if (std::abs(Position.X) > 1.e7 || std::abs(Position.Y) > 1.e7 || std::abs(Position.Z) > 1.e7 || Scale.X < 0.001f || Scale.Y < 0.001f || Scale.Z < 0.001f || Scale.X > 1000.f || Scale.Y > 1000.f || Scale.Z > 1000.f)
	{
		return std::unexpected(FSceneError{"Scene transform exceeds the current float viewport editing range"});
	}

	return {};
}

std::vector<FSceneEntityChange> ReverseChanges(const std::span<const FSceneEntityChange> Changes)
{
	std::vector<FSceneEntityChange> Reversed(Changes.begin(), Changes.end());

	for (FSceneEntityChange& Change : Reversed)
	{
		std::swap(Change.Before, Change.After);
	}

	return Reversed;
}

std::vector<FSceneEntityChange> DiffEntities(const std::span<const FSceneEntity> Before, const std::span<const FSceneEntity> After)
{
	std::vector<FSceneEntityChange> Changes;
	std::size_t BeforeIndex = 0;
	std::size_t AfterIndex = 0;

	while (BeforeIndex < Before.size() || AfterIndex < After.size())
	{
		if (AfterIndex == After.size() || (BeforeIndex < Before.size() && Before[BeforeIndex].Id < After[AfterIndex].Id))
		{
			Changes.push_back({.Before = Before[BeforeIndex++]});
		}
		else if (BeforeIndex == Before.size() || After[AfterIndex].Id < Before[BeforeIndex].Id)
		{
			Changes.push_back({.After = After[AfterIndex++]});
		}
		else
		{
			if (Before[BeforeIndex] != After[AfterIndex])
			{
				Changes.push_back({.Before = Before[BeforeIndex], .After = After[AfterIndex]});
			}

			++BeforeIndex;
			++AfterIndex;
		}
	}

	return Changes;
}

std::string UniqueEntityName(const std::string_view Base, std::unordered_set<std::string>& Used, const std::string_view Tail = {})
{
	for (std::size_t Index = 1;; ++Index)
	{
		const std::string Suffix = std::string(Tail) + (Index == 1 ? std::string{} : std::format(" {}", Index));
		std::size_t Length = std::min(Base.size(), 1024 - Suffix.size());

		while (Length < Base.size() && (static_cast<unsigned char>(Base[Length]) & 0xc0u) == 0x80u)
		{
			--Length;
		}

		std::string Name = std::string(Base.substr(0, Length)) + Suffix;
		if (Used.insert(Name).second)
		{
			return Name;
		}
	}
}
}

FEditorScene::FEditorScene(const std::size_t MaximumTransactions, const std::size_t MaximumMemoryCost)
    : Id(DefaultSceneId)
    , Name("Sandbox")
    , History(MaximumTransactions, MaximumMemoryCost)
{
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, 0.4f) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, -0.25f);
	const std::array Defaults{
	    FSceneEntity{.Id = DefaultCubeId, .Name = "Preview Cube", .Transform = {.Translation = FWorldPosition{0., 4., 0.}, .Rotation = Rotation}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ESceneBodyType::Dynamic},
	    FSceneEntity{.Id = DefaultFloorId, .Name = "Floor", .Transform = {.Translation = FWorldPosition{0., -0.25, 0.}, .Scale = {10.f, 0.5f, 10.f}}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ESceneBodyType::Static},
	};

	if (!World.ReplaceEntities(Defaults))
	{
		std::terminate();
	}

	for (const FSceneEntity& Entity : World.SnapshotEntities())
	{
		Objects.push_back(ToEditorObject(Entity));
	}

	Selection.push_back(Objects.front().Id);
	ActiveObject = Objects.front().Id;
}

std::expected<void, FSceneError> FEditorScene::Load(const std::filesystem::path& Path)
{
	if (bSimulationRunning)
	{
		return std::unexpected(FSceneError{"Stop simulation before loading a scene"});
	}

	if (ActiveEdit)
	{
		return std::unexpected(FSceneError{"Finish or cancel the active edit before loading a scene"});
	}

	auto Document = LoadScene(Path);
	if (!Document)
	{
		return std::unexpected(Document.error());
	}

	// Hierarchy authoring is a later slice; flat entities may have no components.
	for (const FSceneEntity& Entity : Document->Entities)
	{
		if (Entity.Parent.IsValid())
		{
			return std::unexpected(FSceneError{"This editor slice opens flat scenes only"});
		}

		if (auto Result = ValidateEditorEntityRange(Entity); !Result)
		{
			return Result;
		}
	}

	if (auto Result = World.ReplaceEntities(Document->Entities); !Result)
	{
		return Result;
	}

	Id = Document->Id;
	Name = std::move(Document->Name);
	CurrentPath = Path;
	RebuildObjects();
	Selection.clear();
	ActiveObject.reset();
	if (!Objects.empty())
	{
		Selection.push_back(Objects.front().Id);
		ActiveObject = Objects.front().Id;
	}

	History.Clear();
	History.MarkSaved();
	return {};
}

std::expected<void, FSceneError> FEditorScene::CommitEdits(const std::string_view Label)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (Objects.size() != World.GetEntityCount())
	{
		RestoreObjects();
		return std::unexpected(FSceneError{"Editor view no longer matches the authored entity set"});
	}

	std::vector<FSceneEntity> Candidates;
	Candidates.reserve(Objects.size());

	for (const FPreviewObject& Object : Objects)
	{
		const auto Handle = World.FindEntity(Object.Id);
		if (!Handle)
		{
			RestoreObjects();
			return std::unexpected(FSceneError{"Editor view contains a stale entity"});
		}

		FSceneEntity Entity = *World.GetEntity(*Handle);
		const FPreviewObject Previous = ToEditorObject(Entity);
		Entity.Name = Object.Label;
		Entity.Mesh = Object.Mesh.IsValid() ? std::optional{FStaticMeshComponent{Object.Mesh}} : std::nullopt;

		// Preserve double coordinates on axes that the float editing view did not change.
		if (Object.Translation.x != Previous.Translation.x)
		{
			Entity.Transform.Translation.Meters.X = Object.Translation.x;
		}

		if (Object.Translation.y != Previous.Translation.y)
		{
			Entity.Transform.Translation.Meters.Y = Object.Translation.y;
		}

		if (Object.Translation.z != Previous.Translation.z)
		{
			Entity.Transform.Translation.Meters.Z = Object.Translation.z;
		}

		if (!std::ranges::equal(Object.Rotation.m, Previous.Rotation.m))
		{
			Entity.Transform.Rotation = ToSceneRotation(Object.Rotation);
		}

		Entity.Transform.Scale = {Object.Scale.x, Object.Scale.y, Object.Scale.z};
		if (auto Result = ValidateEditorEntityRange(Entity); !Result)
		{
			RestoreObjects();
			return Result;
		}

		Candidates.push_back(std::move(Entity));
	}

	if (auto Result = ValidateSceneEntities(Candidates); !Result)
	{
		RestoreObjects();
		return Result;
	}

	std::ranges::sort(Candidates, {}, &FSceneEntity::Id);
	const auto Changes = DiffEntities(World.SnapshotEntities(), Candidates);
	if (Changes.empty())
	{
		return {};
	}

	if (auto Result = World.ApplyEntityChanges(Changes); !Result)
	{
		RestoreObjects();
		return Result;
	}

	if (!ActiveEdit)
	{
		if (auto Result = RecordChanges(Label, Changes, Selection, ActiveObject); !Result)
		{
			if (!World.ApplyEntityChanges(ReverseChanges(Changes)))
			{
				std::terminate();
			}

			RestoreObjects();
			return Result;
		}
	}

	return {};
}

std::expected<void, FSceneError> FEditorScene::Save(const std::filesystem::path& Path)
{
	if (ActiveEdit)
	{
		return std::unexpected(FSceneError{"Finish or cancel the active edit before saving a scene"});
	}

	if (!bSimulationRunning)
	{
		if (auto Result = CommitEdits(); !Result)
		{
			return Result;
		}
	}

	const std::filesystem::path Target = Path.empty() ? CurrentPath : Path;
	if (Target.empty())
	{
		return std::unexpected(FSceneError{"No scene path is set; use scene.save <path>"});
	}

	if (auto Result = SaveScene(Target, {.Id = Id, .Name = Name, .Entities = World.SnapshotEntities()}); !Result)
	{
		return Result;
	}

	CurrentPath = Target;
	History.MarkSaved();
	return {};
}

std::expected<void, FSceneError> FEditorScene::CheckAuthoringAllowed(const bool bAllowActiveEdit) const
{
	if (bSimulationRunning)
	{
		return std::unexpected(FSceneError{"Stop simulation before editing the authored scene"});
	}

	if (ActiveEdit && !bAllowActiveEdit)
	{
		return std::unexpected(FSceneError{"Finish or cancel the active edit first"});
	}

	return {};
}

void FEditorScene::RebuildObjects()
{
	Objects.clear();

	for (const FSceneEntity& Entity : World.SnapshotEntities())
	{
		Objects.push_back(ToEditorObject(Entity));
	}

	if (++Generation == 0)
	{
		std::terminate();
	}
}

void FEditorScene::RestoreObjects()
{
	const auto Entities = World.SnapshotEntities();
	if (Objects.size() != Entities.size())
	{
		RebuildObjects();
		return;
	}

	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		Objects[Index] = ToEditorObject(Entities[Index]);
	}

	if (++Generation == 0)
	{
		std::terminate();
	}
}

FEditorTransaction FEditorScene::MakeTransaction(const std::string_view Label, const std::vector<FSceneEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, const std::optional<FObjectId> BeforeActive, const std::vector<FObjectId>& AfterSelection, const std::optional<FObjectId> AfterActive)
{
	std::size_t Cost = sizeof(FSceneEntityChange) * Changes.size() + sizeof(FObjectId) * (BeforeSelection.size() + AfterSelection.size());

	for (const FSceneEntityChange& Change : Changes)
	{
		Cost += Change.Before ? Change.Before->Name.size() : 0;
		Cost += Change.After ? Change.After->Name.size() : 0;
	}

	return {
	    .Label = std::string(Label),
	    .Apply = [this, Changes, BeforeSelection = std::move(BeforeSelection), BeforeActive, AfterSelection, AfterActive](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		const auto Replay = bUndo ? ReverseChanges(Changes) : Changes;
		if (auto Result = World.ApplyEntityChanges(Replay); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		SetSelection(bUndo ? BeforeSelection : AfterSelection, bUndo ? BeforeActive : AfterActive);
		RebuildObjects();
		return {};
	},
	    .MemoryCost = Cost,
	};
}

std::expected<void, FSceneError> FEditorScene::RecordChanges(const std::string_view Label, const std::vector<FSceneEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, const std::optional<FObjectId> BeforeActive)
{
	if (Changes.empty())
	{
		return {};
	}

	const auto Recorded = History.RecordApplied(MakeTransaction(Label, Changes, std::move(BeforeSelection), BeforeActive, Selection, ActiveObject));

	if (!Recorded)
	{
		return std::unexpected(FSceneError{Recorded.error().Message});
	}

	return {};
}

std::expected<void, FSceneError> FEditorScene::BeginEdit(const std::string_view Label)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Label.empty())
	{
		return std::unexpected(FSceneError{"An edit requires a transaction label"});
	}

	// Flush any preceding discrete edit before taking the gesture's original snapshots.
	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	ActiveEdit = FActiveEdit{.Label = std::string(Label), .Before = World.SnapshotEntities(), .Selection = Selection, .Active = ActiveObject};
	return {};
}

std::expected<void, FSceneError> FEditorScene::EndEdit()
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!ActiveEdit)
	{
		return std::unexpected(FSceneError{"There is no active edit to finish"});
	}

	if (auto Result = CommitEdits(); !Result)
	{
		if (!CancelEdit())
		{
			std::terminate();
		}

		return Result;
	}

	const auto Changes = DiffEntities(ActiveEdit->Before, World.SnapshotEntities());
	const auto Recorded = RecordChanges(ActiveEdit->Label, Changes, ActiveEdit->Selection, ActiveEdit->Active);
	if (!Recorded)
	{
		if (auto Result = CancelEdit(); !Result)
		{
			std::terminate();
		}

		return Recorded;
	}

	ActiveEdit.reset();
	return {};
}

std::expected<void, FSceneError> FEditorScene::CancelEdit()
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!ActiveEdit)
	{
		return std::unexpected(FSceneError{"There is no active edit to cancel"});
	}

	const auto Changes = DiffEntities(World.SnapshotEntities(), ActiveEdit->Before);
	if (auto Result = World.ApplyEntityChanges(Changes); !Result)
	{
		return Result;
	}

	SetSelection(ActiveEdit->Selection, ActiveEdit->Active);
	ActiveEdit.reset();
	RestoreObjects();
	return {};
}

bool FEditorScene::HasActiveEdit() const
{
	return ActiveEdit.has_value();
}

std::expected<void, FSceneError> FEditorScene::Undo()
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	if (auto Result = History.Undo(); !Result)
	{
		return std::unexpected(FSceneError{Result.error().Message});
	}

	return {};
}

std::expected<void, FSceneError> FEditorScene::Redo()
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	if (auto Result = History.Redo(); !Result)
	{
		return std::unexpected(FSceneError{Result.error().Message});
	}

	return {};
}

bool FEditorScene::CanUndo() const
{
	return !bSimulationRunning && !ActiveEdit && History.CanUndo();
}

bool FEditorScene::CanRedo() const
{
	return !bSimulationRunning && !ActiveEdit && History.CanRedo();
}

bool FEditorScene::IsDirty() const
{
	return History.IsDirty() || (ActiveEdit && ActiveEdit->Before != World.SnapshotEntities());
}

std::string_view FEditorScene::GetUndoLabel() const
{
	return History.GetUndoLabel();
}

std::string_view FEditorScene::GetRedoLabel() const
{
	return History.GetRedoLabel();
}

void FEditorScene::SetSelection(const std::span<const FObjectId> Selected, const std::optional<FObjectId> Active)
{
	std::vector<FObjectId> ValidSelection;
	ValidSelection.reserve(Selected.size());

	for (const FObjectId Object : Selected)
	{
		if (World.FindEntity(Object) && !ContainsObjectId(ValidSelection, Object))
		{
			ValidSelection.push_back(Object);
		}
	}

	Selection = std::move(ValidSelection);
	ActiveObject = Active && ContainsObjectId(Selection, *Active) ? Active : (Selection.empty() ? std::nullopt : std::optional{Selection.back()});
}

std::span<const FObjectId> FEditorScene::GetSelection() const
{
	return Selection;
}

std::optional<FObjectId> FEditorScene::GetActiveObject() const
{
	return ActiveObject;
}

std::expected<void, FSceneError> FEditorScene::ApplyStructuralChanges(const std::string_view Label, const std::vector<FSceneEntityChange>& Changes, const std::vector<FObjectId>& AfterSelection, const std::optional<FObjectId> AfterActive)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Changes.empty())
	{
		return {};
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	const auto EffectiveActive = AfterActive ? AfterActive : (AfterSelection.empty() ? std::nullopt : std::optional{AfterSelection.back()});
	const auto Executed = History.Execute(MakeTransaction(Label, Changes, Selection, ActiveObject, AfterSelection, EffectiveActive));
	if (!Executed)
	{
		return std::unexpected(FSceneError{Executed.error().Message});
	}

	return {};
}

std::expected<FObjectId, FSceneError> FEditorScene::CreateEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Cube", .Transform = {.Translation = Position}, .Mesh = FStaticMeshComponent{.Asset = EngineCubeAsset}});
}

std::expected<FObjectId, FSceneError> FEditorScene::CreateEmptyEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Entity", .Transform = {.Translation = Position}});
}

std::expected<FObjectId, FSceneError> FEditorScene::InsertEntity(FSceneEntity Entity)
{
	if (auto Result = ValidateEditorEntityRange(Entity); !Result)
	{
		return std::unexpected(Result.error());
	}

	std::unordered_set<std::string> Names;

	for (const FPreviewObject& Object : Objects)
	{
		Names.insert(Object.Label);
	}

	Entity.Name = UniqueEntityName(Entity.Name, Names);
	const auto Result = ApplyStructuralChanges(Entity.Mesh ? "Create object" : "Create empty entity", {{.After = Entity}}, {Entity.Id});
	if (!Result)
	{
		return std::unexpected(Result.error());
	}

	return Entity.Id;
}

std::expected<void, FSceneError> FEditorScene::AddStaticMeshToSelected(const FAssetId Asset)
{
	if (!Asset.IsValid())
	{
		return std::unexpected(FSceneError{"A static mesh requires a valid asset ID"});
	}

	return ApplySelectedMesh(FStaticMeshComponent{.Asset = Asset});
}

std::expected<void, FSceneError> FEditorScene::RemoveStaticMeshFromSelected()
{
	return ApplySelectedMesh(std::nullopt);
}

std::expected<void, FSceneError> FEditorScene::ApplySelectedMesh(const std::optional<FStaticMeshComponent> Mesh)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FSceneEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FSceneEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.Mesh.has_value() == Mesh.has_value())
		{
			continue;
		}

		FSceneEntity After = Before;
		After.Mesh = Mesh;
		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	return ApplyStructuralChanges(Mesh ? "Add static mesh" : "Remove static mesh", Changes, Selection, ActiveObject);
}

std::expected<void, FSceneError> FEditorScene::AddRigidBodyToSelected(const ESceneBodyType Type)
{
	if (Type != ESceneBodyType::Static && Type != ESceneBodyType::Dynamic)
	{
		return std::unexpected(FSceneError{"Adding a rigid body requires Static or Dynamic motion"});
	}

	return ApplySelectedBodyType(Type, true);
}

std::expected<void, FSceneError> FEditorScene::SetSelectedBodyType(const ESceneBodyType Type)
{
	if (Type != ESceneBodyType::None && Type != ESceneBodyType::Static && Type != ESceneBodyType::Dynamic)
	{
		return std::unexpected(FSceneError{"The rigid body motion type is invalid"});
	}

	return ApplySelectedBodyType(Type, false);
}

std::expected<void, FSceneError> FEditorScene::ApplySelectedBodyType(const ESceneBodyType Type, const bool bOnlyAbsent)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FSceneEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FSceneEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.BodyType == Type || (bOnlyAbsent && Before.BodyType != ESceneBodyType::None) || (!bOnlyAbsent && Before.BodyType == ESceneBodyType::None))
		{
			continue;
		}

		FSceneEntity After = Before;
		After.BodyType = Type;

		if (Type == ESceneBodyType::None)
		{
			After.BodySettings = {};
		}

		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	return ApplyStructuralChanges(bOnlyAbsent ? "Add rigid body" : (Type == ESceneBodyType::None ? "Remove rigid body" : "Set rigid body motion"), Changes, Selection, ActiveObject);
}

std::expected<void, FSceneError> FEditorScene::SetSelectedBodyProperty(float FSceneRigidBodySettings::* const Property, const float Value)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!Property)
	{
		return std::unexpected(FSceneError{"A rigid body property is required"});
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FSceneEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FSceneEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.BodyType == ESceneBodyType::None || Before.BodySettings.*Property == Value)
		{
			continue;
		}

		FSceneEntity After = Before;
		After.BodySettings.*Property = Value;

		if (auto Result = ValidateSceneRigidBodySettings(After.BodySettings); !Result)
		{
			return Result;
		}

		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	if (ActiveEdit)
	{
		return World.ApplyEntityChanges(Changes);
	}

	return ApplyStructuralChanges("Edit rigid body", Changes, Selection, ActiveObject);
}

std::expected<void, FSceneError> FEditorScene::DuplicateSelected(const bool bWithinActiveEdit, const FVector3d& WorldOffset)
{
	if (auto Result = CheckAuthoringAllowed(bWithinActiveEdit); !Result)
	{
		return Result;
	}

	if (bWithinActiveEdit)
	{
		if (!ActiveEdit)
		{
			return std::unexpected(FSceneError{"Begin an edit before duplicating within a gesture"});
		}

		if (ActiveEdit->bDuplicated)
		{
			return {};
		}
	}

	if (!std::isfinite(WorldOffset.X) || !std::isfinite(WorldOffset.Y) || !std::isfinite(WorldOffset.Z))
	{
		return std::unexpected(FSceneError{"Duplication requires a finite world translation offset"});
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::unordered_set<std::string> Names;
	const auto Entities = World.SnapshotEntities();

	for (const FSceneEntity& Entity : Entities)
	{
		Names.insert(Entity.Name);
	}

	std::vector<FSceneEntityChange> Changes;
	std::vector<FObjectId> Duplicated;
	std::optional<FObjectId> DuplicatedActive;

	for (const FSceneEntity& Original : Entities)
	{
		if (!ContainsObjectId(Selection, Original.Id))
		{
			continue;
		}

		FSceneEntity Entity = Original;
		Entity.Id = FObjectId::Generate();
		Entity.Name = UniqueEntityName(Original.Name, Names, " Copy");
		Entity.Transform.Translation = Entity.Transform.Translation.TranslatedBy(WorldOffset);
		if (auto Result = ValidateEditorEntityRange(Entity); !Result)
		{
			return Result;
		}

		if (ActiveObject == Original.Id)
		{
			DuplicatedActive = Entity.Id;
		}

		Duplicated.push_back(Entity.Id);
		Changes.push_back({.After = std::move(Entity)});
	}

	if (bWithinActiveEdit)
	{
		if (Changes.empty())
		{
			return {};
		}

		if (auto Result = World.ApplyEntityChanges(Changes); !Result)
		{
			return Result;
		}

		ActiveEdit->bDuplicated = true;
		SetSelection(Duplicated, DuplicatedActive);
		RebuildObjects();
		return {};
	}

	return ApplyStructuralChanges("Duplicate objects", Changes, Duplicated);
}

std::expected<void, FSceneError> FEditorScene::DeleteSelected()
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FSceneEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const auto Handle = World.FindEntity(Object);
		if (Handle)
		{
			Changes.push_back({.Before = *World.GetEntity(*Handle)});
		}
	}

	return ApplyStructuralChanges("Delete objects", std::move(Changes), {});
}

std::expected<std::string, FSceneError> FEditorScene::CopySelected() const
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return std::unexpected(Result.error());
	}

	FSceneDocument Document{.Id = Id, .Name = "Clipboard"};

	for (const FObjectId Object : Selection)
	{
		const auto Handle = World.FindEntity(Object);
		if (Handle)
		{
			Document.Entities.push_back(*World.GetEntity(*Handle));
		}
	}

	return SerializeScene(Document);
}

std::expected<void, FSceneError> FEditorScene::PasteEntities(const std::string_view Text)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	auto Document = ParseScene(Text);
	if (!Document)
	{
		return std::unexpected(Document.error());
	}

	std::vector<FSceneEntityChange> Changes;
	std::vector<FObjectId> Pasted;
	std::unordered_set<std::string> Names;

	for (const FPreviewObject& Object : Objects)
	{
		Names.insert(Object.Label);
	}

	for (FSceneEntity& Entity : Document->Entities)
	{
		if (Entity.Parent.IsValid())
		{
			return std::unexpected(FSceneError{"This editor slice pastes flat entities only"});
		}

		if (auto Result = ValidateEditorEntityRange(Entity); !Result)
		{
			return Result;
		}

		Entity.Id = FObjectId::Generate();
		Entity.Name = UniqueEntityName(Entity.Name, Names);
		Pasted.push_back(Entity.Id);
		Changes.push_back({.After = std::move(Entity)});
	}

	return ApplyStructuralChanges("Paste objects", std::move(Changes), std::move(Pasted));
}

void FEditorScene::SetPath(std::filesystem::path Path)
{
	CurrentPath = std::move(Path);
}

void FEditorScene::SetSimulationRunning(const bool bRunning)
{
	bSimulationRunning = bRunning;
}

std::vector<std::size_t> FEditorScene::FindBodies(const ESceneBodyType Type) const
{
	std::vector<std::size_t> Indices;
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		const auto Handle = World.FindEntity(Objects[Index].Id);
		const auto Entity = Handle ? World.GetEntity(*Handle) : std::nullopt;
		if (Entity && Entity->Mesh && Entity->BodyType == Type)
		{
			Indices.push_back(Index);
		}
	}

	return Indices;
}

std::vector<FPreviewObject>& FEditorScene::GetObjects()
{
	return Objects;
}

const std::vector<FPreviewObject>& FEditorScene::GetObjects() const
{
	return Objects;
}

std::uint64_t FEditorScene::GetGeneration() const
{
	return Generation;
}

const std::filesystem::path& FEditorScene::GetPath() const
{
	return CurrentPath;
}

std::string_view FEditorScene::GetName() const
{
	return Name;
}

const FWorld& FEditorScene::GetWorld() const
{
	return World;
}

std::expected<void, FEditorCommandError> RegisterEditorSceneCommands(FEditorCommandRegistry& Commands, const std::shared_ptr<FEditorScene>& Scene)
{
	if (auto Result = Commands.Register({.Name = "scene.save", .Description = "Save the authored scene. Usage: scene.save [path]", .Handler = [Scene](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() > 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: scene.save [path]"});
		}

		if (auto Saved = Scene->Save(Arguments.empty() ? std::filesystem::path{} : Utf8Path(Arguments.front())); !Saved)
		{
			return std::unexpected(FEditorCommandError{.Message = Saved.error().Message});
		}

		return FEditorCommandResult{.Message = "Scene saved"};
	}});
	    !Result)
	{
		return Result;
	}

	return Commands.Register({.Name = "scene.load", .Description = "Load a scene without replacing the current scene on failure. Usage: scene.load <path>", .Handler = [Scene](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: scene.load <path>"});
		}

		if (auto Loaded = Scene->Load(Utf8Path(Arguments.front())); !Loaded)
		{
			return std::unexpected(FEditorCommandError{.Message = Loaded.error().Message});
		}

		return FEditorCommandResult{.Message = "Scene loaded"};
	}});
}
}
