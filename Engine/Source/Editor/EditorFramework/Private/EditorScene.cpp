#include "EditorScene.h"

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/Math/AffineTransform.h"

#include <cmath>
#include <exception>
#include <format>
#include <map>
#include <set>
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

FPreviewObject ToEditorObject(const FSceneEntity& Entity, const TTransform<double>& WorldPose)
{
	const auto& Rotation = WorldPose.Rotation;
	return {
	    .Label = Entity.Name,
	    .Translation = {static_cast<float>(WorldPose.Translation.X), static_cast<float>(WorldPose.Translation.Y), static_cast<float>(WorldPose.Translation.Z)},
	    .Rotation = ToEditorRotation({static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y), static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W)}),
	    .Scale = {static_cast<float>(WorldPose.Scale3D.X), static_cast<float>(WorldPose.Scale3D.Y), static_cast<float>(WorldPose.Scale3D.Z)},
	    .Mesh = Entity.Mesh ? Entity.Mesh->Asset : FAssetId{},
	    .Id = Entity.Id,
	    .Parent = Entity.Parent.IsValid() ? std::optional{Entity.Parent} : std::nullopt,
	};
}

std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::expected<void, FSceneError> ValidateEditorPoseRange(const TTransform<double>& Pose)
{
	const auto& Position = Pose.Translation;
	const FVector3 Scale{Pose.Scale3D};
	if (std::abs(Position.X) > 1.e7 || std::abs(Position.Y) > 1.e7 || std::abs(Position.Z) > 1.e7 || Scale.X < 0.001f || Scale.Y < 0.001f || Scale.Z < 0.001f || Scale.X > 1000.f || Scale.Y > 1000.f || Scale.Z > 1000.f)
	{
		return std::unexpected(FSceneError{"Scene transform exceeds the current float viewport editing range"});
	}

	return {};
}

TMatrix4<double> MakeLocalMatrix(const FSceneTransform& Transform)
{
	const auto& Rotation = Transform.Rotation;
	return TMatrix4<double>::Transform(Transform.Translation.Meters, TQuaternion<double>{Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}, FVector3d{Transform.Scale});
}

struct FEditorHierarchy
{
	std::map<FObjectId, std::size_t> Indices;
	std::vector<std::size_t> Parents;
	std::vector<std::size_t> Order;
	std::vector<TMatrix4<double>> Matrices;
	std::vector<TTransform<double>> Poses;
};

std::expected<TTransform<double>, FSceneError> DecomposeEditorWorld(const TMatrix4<double>& Matrix)
{
	const auto Pose = TryDecomposeTransform(Matrix);
	if (!Pose)
	{
		return std::unexpected(FSceneError{"Scene hierarchy requires shear or an unrepresentable world transform"});
	}

	if (auto Result = ValidateEditorPoseRange(*Pose); !Result)
	{
		return std::unexpected(Result.error());
	}

	return *Pose;
}

std::expected<FEditorHierarchy, FSceneError> BuildEditorHierarchy(const std::span<const FSceneEntity> Entities)
{
	if (auto Result = ValidateSceneEntities(Entities); !Result)
	{
		return std::unexpected(Result.error());
	}

	FEditorHierarchy Hierarchy;
	Hierarchy.Parents.resize(Entities.size(), Entities.size());
	Hierarchy.Matrices.resize(Entities.size());
	Hierarchy.Poses.resize(Entities.size());
	Hierarchy.Order.reserve(Entities.size());

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		Hierarchy.Indices.emplace(Entities[Index].Id, Index);
	}

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		if (Entities[Index].Parent.IsValid())
		{
			Hierarchy.Parents[Index] = Hierarchy.Indices.at(Entities[Index].Parent);
		}
	}

	std::vector<bool> Evaluated(Entities.size());
	std::vector<std::size_t> Ancestors;

	for (std::size_t Start = 0; Start < Entities.size(); ++Start)
	{
		std::size_t Current = Start;

		while (Current != Entities.size() && !Evaluated[Current])
		{
			Ancestors.push_back(Current);
			Current = Hierarchy.Parents[Current];
		}

		while (!Ancestors.empty())
		{
			const std::size_t Index = Ancestors.back();
			Ancestors.pop_back();
			const std::size_t Parent = Hierarchy.Parents[Index];
			const auto Local = MakeLocalMatrix(Entities[Index].Transform);
			Hierarchy.Matrices[Index] = Parent == Entities.size() ? Local : Hierarchy.Matrices[Parent] * Local;
			const auto Pose = DecomposeEditorWorld(Hierarchy.Matrices[Index]);
			if (!Pose)
			{
				return std::unexpected(Pose.error());
			}

			Hierarchy.Poses[Index] = *Pose;
			Hierarchy.Order.push_back(Index);
			Evaluated[Index] = true;
		}
	}

	return Hierarchy;
}

std::expected<void, FSceneError> SetLocalFromWorld(FSceneEntity& Entity, const TMatrix4<double>& DesiredWorld, const TMatrix4<double>& ParentWorld)
{
	const auto Inverse = TryInverseAffine(ParentWorld);
	if (!Inverse)
	{
		return std::unexpected(FSceneError{"The parent world transform cannot be inverted"});
	}

	const auto Local = TryDecomposeTransform(*Inverse * DesiredWorld);
	if (!Local)
	{
		return std::unexpected(FSceneError{"This hierarchy edit would require shear or an unrepresentable local transform"});
	}

	const auto& Rotation = Local->Rotation;
	Entity.Transform = {
	    .Translation = FWorldPosition{Local->Translation},
	    .Rotation = {static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y), static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W)},
	    .Scale = FVector3{Local->Scale3D},
	};

	return {};
}

std::expected<TTransform<double>, FSceneError> EditedWorldPose(const FPreviewObject& Object, const FPreviewObject& Previous, TTransform<double> Pose)
{
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		if (Object.Translation[Axis] != Previous.Translation[Axis])
		{
			Pose.Translation[static_cast<std::size_t>(Axis)] = Object.Translation[Axis];
		}

		if (Object.Scale[Axis] != Previous.Scale[Axis])
		{
			Pose.Scale3D[static_cast<std::size_t>(Axis)] = Object.Scale[Axis];
		}
	}

	if (!std::ranges::equal(Object.Rotation.m, Previous.Rotation.m))
	{
		TMatrix4<double> Matrix;

		for (int Column = 0; Column < 3; ++Column)
		{
			for (int Row = 0; Row < 3; ++Row)
			{
				Matrix(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column)) = Object.Rotation(Row, Column);
			}
		}

		const auto Rotation = TryDecomposeTransform(Matrix);
		if (!Rotation || std::abs(Rotation->Scale3D.X - 1.) > 1e-6 || std::abs(Rotation->Scale3D.Y - 1.) > 1e-6 || std::abs(Rotation->Scale3D.Z - 1.) > 1e-6)
		{
			return std::unexpected(FSceneError{"The edited world rotation is not representable"});
		}

		Pose.Rotation = Rotation->Rotation;
	}

	if (auto Result = ValidateEditorPoseRange(Pose); !Result)
	{
		return std::unexpected(Result.error());
	}

	return DecomposeEditorWorld(Pose.ToMatrix());
}

bool SamePreviewPose(const FPreviewObject& Left, const FPreviewObject& Right)
{
	return Left.Translation.x == Right.Translation.x && Left.Translation.y == Right.Translation.y && Left.Translation.z == Right.Translation.z
	       && Left.Scale.x == Right.Scale.x && Left.Scale.y == Right.Scale.y && Left.Scale.z == Right.Scale.z
	       && std::ranges::equal(Left.Rotation.m, Right.Rotation.m);
}

std::expected<FEditorHierarchy, FSceneError> ValidateEditorChanges(const std::span<const FSceneEntity> Before, const std::span<const FSceneEntityChange> Changes)
{
	std::map<FObjectId, FSceneEntity> Candidates;

	for (const auto& Entity : Before)
	{
		Candidates.emplace(Entity.Id, Entity);
	}

	for (const auto& Change : Changes)
	{
		if (Change.Before)
		{
			Candidates.erase(Change.Before->Id);
		}

		if (Change.After)
		{
			Candidates.insert_or_assign(Change.After->Id, *Change.After);
		}
	}

	std::vector<FSceneEntity> Entities;
	Entities.reserve(Candidates.size());

	for (auto& [Id, Entity] : Candidates)
	{
		Entities.push_back(std::move(Entity));
	}

	return BuildEditorHierarchy(Entities);
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

	RebuildObjects();

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

	if (const auto Hierarchy = BuildEditorHierarchy(Document->Entities); !Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
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
		RestoreObjects(false);
		return std::unexpected(FSceneError{"Editor view no longer matches the authored entity set"});
	}

	bool bChanged = false;

	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		const auto& Object = Objects[Index];
		const auto& Previous = AuthoredObjects[Index];

		if (Object.Id != Previous.Id || Object.Parent != Previous.Parent)
		{
			RestoreObjects(false);
			return std::unexpected(FSceneError{"Editor view contains stale identity or hierarchy data"});
		}

		bChanged = bChanged || Object.Label != Previous.Label || Object.Mesh != Previous.Mesh || !SamePreviewPose(Object, Previous);
	}

	if (!bChanged)
	{
		return {};
	}

	const auto Before = World.SnapshotEntities();
	auto Candidates = Before;
	auto Hierarchy = BuildEditorHierarchy(Before);
	if (!Hierarchy)
	{
		RestoreObjects(false);
		return std::unexpected(Hierarchy.error());
	}

	for (const std::size_t Index : Hierarchy->Order)
	{
		const auto& Object = Objects[Index];
		const auto& Previous = AuthoredObjects[Index];
		auto& Entity = Candidates[Index];
		Entity.Name = Object.Label;
		Entity.Mesh = Object.Mesh.IsValid() ? std::optional{FStaticMeshComponent{Object.Mesh}} : std::nullopt;
		const auto Parent = Hierarchy->Parents[Index];
		const auto ParentWorld = Parent == Candidates.size() ? TMatrix4<double>::Identity() : Hierarchy->Matrices[Parent];

		if (!SamePreviewPose(Object, Previous))
		{
			const auto FollowingWorld = ParentWorld * MakeLocalMatrix(Entity.Transform);
			const auto FollowingPose = DecomposeEditorWorld(FollowingWorld);
			// Group gestures may already place a child exactly where its edited parent takes it.
			if (FollowingPose && SamePreviewPose(Object, ToEditorObject(Entity, *FollowingPose)))
			{
				Hierarchy->Matrices[Index] = FollowingWorld;
				continue;
			}

			const auto Pose = EditedWorldPose(Object, Previous, Hierarchy->Poses[Index]);
			if (!Pose)
			{
				RestoreObjects(false);
				return std::unexpected(Pose.error());
			}

			const auto OldTransform = Entity.Transform;
			const auto OldLocal = MakeLocalMatrix(OldTransform);
			if (auto Result = SetLocalFromWorld(Entity, Pose->ToMatrix(), ParentWorld); !Result)
			{
				RestoreObjects(false);
				return Result;
			}

			// Translation-only gestures must not renormalize untouched authored rotation or scale.
			const auto NewLocal = MakeLocalMatrix(Entity.Transform);
			bool bSameLinear = true;

			for (std::size_t Column = 0; Column < 3; ++Column)
			{
				const double Scale = std::hypot(OldLocal(0, Column), OldLocal(1, Column), OldLocal(2, Column));

				for (std::size_t Row = 0; Row < 3; ++Row)
				{
					bSameLinear = bSameLinear && std::abs(OldLocal(Row, Column) / Scale - NewLocal(Row, Column) / Scale) <= 1e-7;
				}
			}

			if (bSameLinear)
			{
				Entity.Transform.Rotation = OldTransform.Rotation;
				Entity.Transform.Scale = OldTransform.Scale;
			}
		}

		Hierarchy->Matrices[Index] = ParentWorld * MakeLocalMatrix(Entity.Transform);
		if (const auto Pose = DecomposeEditorWorld(Hierarchy->Matrices[Index]); !Pose)
		{
			RestoreObjects(false);
			return std::unexpected(Pose.error());
		}
	}

	if (auto Result = ValidateSceneEntities(Candidates); !Result)
	{
		RestoreObjects(false);
		return Result;
	}

	const auto Changes = DiffEntities(Before, Candidates);
	if (Changes.empty())
	{
		RestoreObjects(false);
		return {};
	}

	if (auto Result = World.ApplyEntityChanges(Changes); !Result)
	{
		RestoreObjects(false);
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

			RestoreObjects(false);
			return Result;
		}
	}

	RestoreObjects(false);
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
	const auto Entities = World.SnapshotEntities();
	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		std::terminate();
	}

	Objects.clear();

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		Objects.push_back(ToEditorObject(Entities[Index], Hierarchy->Poses[Index]));
	}

	AuthoredObjects = Objects;

	if (++Generation == 0)
	{
		std::terminate();
	}
}

void FEditorScene::RestoreObjects(const bool bNotify)
{
	const auto Entities = World.SnapshotEntities();
	if (Objects.size() != Entities.size() || !std::ranges::equal(Objects, Entities, {}, &FPreviewObject::Id, &FSceneEntity::Id))
	{
		RebuildObjects();
		return;
	}

	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		std::terminate();
	}

	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		Objects[Index] = ToEditorObject(Entities[Index], Hierarchy->Poses[Index]);
	}

	AuthoredObjects = Objects;

	if (bNotify && ++Generation == 0)
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
		if (const auto Hierarchy = ValidateEditorChanges(World.SnapshotEntities(), Replay); !Hierarchy)
		{
			return std::unexpected(FEditorCommandError{.Message = Hierarchy.error().Message});
		}

		if (auto Result = World.ApplyEntityChanges(Replay); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		SetSelection(bUndo ? BeforeSelection : AfterSelection, bUndo ? BeforeActive : AfterActive);
		RestoreObjects();
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
	std::set<FObjectId> Seen;

	for (const FObjectId Object : Selected)
	{
		if (World.FindEntity(Object) && Seen.insert(Object).second)
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
	if (const auto Result = DecomposeEditorWorld(MakeLocalMatrix(Entity.Transform)); !Result)
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
	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	std::map<FObjectId, FObjectId> Remapped;

	for (const FObjectId Object : Selection)
	{
		Remapped.emplace(Object, FObjectId::Generate());
	}

	for (const FSceneEntity& Original : Entities)
	{
		if (!Remapped.contains(Original.Id))
		{
			continue;
		}

		FSceneEntity Entity = Original;
		Entity.Id = Remapped.at(Original.Id);
		Entity.Name = UniqueEntityName(Original.Name, Names, " Copy");
		const auto Parent = Remapped.find(Original.Parent);

		if (Parent != Remapped.end())
		{
			Entity.Parent = Parent->second;
		}
		else if (WorldOffset != FVector3d::Zero())
		{
			const auto Index = Hierarchy->Indices.at(Original.Id);
			const auto ParentIndex = Hierarchy->Parents[Index];
			const auto ParentWorld = ParentIndex == Entities.size() ? TMatrix4<double>::Identity() : Hierarchy->Matrices[ParentIndex];
			const auto DesiredWorld = TMatrix4<double>::Translation(WorldOffset) * Hierarchy->Matrices[Index];
			if (auto Result = SetLocalFromWorld(Entity, DesiredWorld, ParentWorld); !Result)
			{
				return Result;
			}

			Entity.Transform.Rotation = Original.Transform.Rotation;
			Entity.Transform.Scale = Original.Transform.Scale;
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

		if (const auto Valid = ValidateEditorChanges(Entities, Changes); !Valid)
		{
			return std::unexpected(Valid.error());
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

	return ApplyStructuralChanges("Duplicate objects", Changes, Duplicated, DuplicatedActive);
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
	const auto Entities = World.SnapshotEntities();
	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	const std::set<FObjectId> Deleted(Selection.begin(), Selection.end());

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const auto& Entity = Entities[Index];

		if (Deleted.contains(Entity.Id))
		{
			Changes.push_back({.Before = Entity});
		}
		else if (Deleted.contains(Entity.Parent))
		{
			auto After = Entity;
			After.Parent = {};
			if (auto Result = SetLocalFromWorld(After, Hierarchy->Matrices[Index], TMatrix4<double>::Identity()); !Result)
			{
				return Result;
			}

			Changes.push_back({.Before = Entity, .After = std::move(After)});
		}
	}

	return ApplyStructuralChanges("Delete objects", std::move(Changes), {});
}

std::expected<void, FSceneError> FEditorScene::ReparentEntities(const std::span<const FObjectId> Requested, const std::optional<FObjectId> Parent)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Parent && (!Parent->IsValid() || !World.FindEntity(*Parent)))
	{
		return std::unexpected(FSceneError{"The requested parent no longer exists"});
	}

	for (const auto Object : Requested)
	{
		if (!World.FindEntity(Object))
		{
			return std::unexpected(FSceneError{"A reparented entity no longer exists"});
		}
	}

	const auto Before = World.SnapshotEntities();
	const auto Hierarchy = BuildEditorHierarchy(Before);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	const std::set<FObjectId> Selected(Requested.begin(), Requested.end());
	std::vector<std::size_t> Roots;
	std::vector<bool> HasSelectedAncestor(Before.size());

	for (const auto Index : Hierarchy->Order)
	{
		const auto Ancestor = Hierarchy->Parents[Index];
		HasSelectedAncestor[Index] = Ancestor != Before.size() && (Selected.contains(Before[Ancestor].Id) || HasSelectedAncestor[Ancestor]);
	}

	for (const auto Object : Selected)
	{
		const auto Index = Hierarchy->Indices.at(Object);

		if (!HasSelectedAncestor[Index])
		{
			Roots.push_back(Index);
		}
	}

	if (Parent)
	{
		auto Ancestor = Hierarchy->Indices.at(*Parent);

		while (Ancestor != Before.size())
		{
			if (Selected.contains(Before[Ancestor].Id))
			{
				return std::unexpected(FSceneError{"An entity cannot be parented to itself or one of its descendants"});
			}

			Ancestor = Hierarchy->Parents[Ancestor];
		}
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	// A pending property edit may have changed the world poses without changing identities.
	const auto Entities = World.SnapshotEntities();
	const auto Current = BuildEditorHierarchy(Entities);
	if (!Current)
	{
		return std::unexpected(Current.error());
	}

	const auto ParentWorld = Parent ? Current->Matrices[Current->Indices.at(*Parent)] : TMatrix4<double>::Identity();
	std::vector<FSceneEntityChange> Changes;

	for (const auto Index : Roots)
	{
		const auto& Original = Entities[Index];
		if (Original.Parent == Parent.value_or(FObjectId{}))
		{
			continue;
		}

		auto After = Original;
		After.Parent = Parent.value_or(FObjectId{});
		if (auto Result = SetLocalFromWorld(After, Current->Matrices[Index], ParentWorld); !Result)
		{
			return Result;
		}

		Changes.push_back({.Before = Original, .After = std::move(After)});
	}

	return ApplyStructuralChanges(Parent ? "Reparent objects" : "Detach objects", Changes, Selection, ActiveObject);
}

std::expected<void, FSceneError> FEditorScene::ReparentSelected(const std::optional<FObjectId> Parent)
{
	return ReparentEntities(Selection, Parent);
}

std::expected<std::string, FSceneError> FEditorScene::CopySelected() const
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return std::unexpected(Result.error());
	}

	FSceneDocument Document{.Id = Id, .Name = "Clipboard"};
	const auto Entities = World.SnapshotEntities();
	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	const std::set<FObjectId> Selected(Selection.begin(), Selection.end());

	for (const FObjectId Object : Selection)
	{
		const auto Found = Hierarchy->Indices.find(Object);
		if (Found != Hierarchy->Indices.end())
		{
			const auto Index = Found->second;
			auto Entity = Entities[Index];

			if (Entity.Parent.IsValid() && !Selected.contains(Entity.Parent))
			{
				Entity.Parent = {};
				if (auto Result = SetLocalFromWorld(Entity, Hierarchy->Matrices[Index], TMatrix4<double>::Identity()); !Result)
				{
					return std::unexpected(Result.error());
				}
			}

			Document.Entities.push_back(std::move(Entity));
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

	if (const auto Hierarchy = BuildEditorHierarchy(Document->Entities); !Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	std::vector<FSceneEntityChange> Changes;
	std::vector<FObjectId> Pasted;
	std::unordered_set<std::string> Names;

	for (const FPreviewObject& Object : Objects)
	{
		Names.insert(Object.Label);
	}

	std::map<FObjectId, FObjectId> Remapped;

	for (const auto& Entity : Document->Entities)
	{
		Remapped.emplace(Entity.Id, FObjectId::Generate());
	}

	for (FSceneEntity& Entity : Document->Entities)
	{
		Entity.Id = Remapped.at(Entity.Id);

		if (Entity.Parent.IsValid())
		{
			Entity.Parent = Remapped.at(Entity.Parent);
		}

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

std::expected<void, FSceneError> FEditorScene::UpdatePreviewHierarchy(const std::span<const FObjectId> OverrideWorldPoses)
{
	if (std::ranges::none_of(AuthoredObjects, [](const FPreviewObject& Object)
	{
		return Object.Parent.has_value();
	}))
	{
		if (Objects.size() != World.GetEntityCount() || !std::ranges::equal(Objects, AuthoredObjects, {}, &FPreviewObject::Id, &FPreviewObject::Id))
		{
			return std::unexpected(FSceneError{"Simulation preview contains a stale entity set"});
		}

		for (const auto Object : OverrideWorldPoses)
		{
			if (!World.FindEntity(Object))
			{
				return std::unexpected(FSceneError{"A simulation pose refers to a stale entity"});
			}
		}

		return {};
	}

	const auto Entities = World.SnapshotEntities();
	auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	if (Objects.size() != Entities.size() || !std::ranges::equal(Objects, Entities, {}, &FPreviewObject::Id, &FSceneEntity::Id))
	{
		return std::unexpected(FSceneError{"Simulation preview contains a stale entity set"});
	}

	const std::set<FObjectId> Overrides(OverrideWorldPoses.begin(), OverrideWorldPoses.end());

	for (const auto Object : Overrides)
	{
		if (!Hierarchy->Indices.contains(Object))
		{
			return std::unexpected(FSceneError{"A simulation pose refers to a stale entity"});
		}
	}

	for (const auto Index : Hierarchy->Order)
	{
		if (Overrides.contains(Entities[Index].Id))
		{
			const auto& Object = Objects[Index];
			const auto Pose = EditedWorldPose(Object, AuthoredObjects[Index], Hierarchy->Poses[Index]);
			if (!Pose)
			{
				return std::unexpected(Pose.error());
			}

			Hierarchy->Matrices[Index] = Pose->ToMatrix();
		}
		else
		{
			const auto Parent = Hierarchy->Parents[Index];
			const auto Local = MakeLocalMatrix(Entities[Index].Transform);
			Hierarchy->Matrices[Index] = Parent == Entities.size() ? Local : Hierarchy->Matrices[Parent] * Local;
		}

		const auto Pose = DecomposeEditorWorld(Hierarchy->Matrices[Index]);
		if (!Pose)
		{
			return std::unexpected(Pose.error());
		}

		Hierarchy->Poses[Index] = *Pose;
	}

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		if (!Overrides.contains(Entities[Index].Id))
		{
			Objects[Index] = ToEditorObject(Entities[Index], Hierarchy->Poses[Index]);
		}
	}

	return {};
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
