#include "EditorLevel.h"

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/Math/AffineTransform.h"

#include <array>
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
constexpr FObjectId DefaultLevelId{0x10935c1f4f594b12, 0x9a139dca302101e0};
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

// Shape-appropriate starting points: a 3 m rope, a 2 x 1.5 m curtain, and a 0.8 m ball that keeps its shape under pressure.
FSoftBodyComponent MakeDefaultSoftBody(const ESoftBodyShape Shape)
{
	switch (Shape)
	{
		case ESoftBodyShape::Cloth:
			return {.Shape = Shape, .Length = 2.f, .Height = 1.5f, .Thickness = 0.02f, .MassKg = 0.6f, .Stiffness = 0.8f};
		case ESoftBodyShape::Ball:
			return {.Shape = Shape, .Length = 0.8f, .Thickness = 0.02f, .MassKg = 1.f, .Stiffness = 0.7f, .Pressure = 400.f, .bPinned = false};
		case ESoftBodyShape::Rope:
			break;
	}

	return {};
}

FPreviewObject ToEditorObject(const FLevelEntity& Entity, const TTransform<double>& WorldPose)
{
	const auto& Rotation = WorldPose.Rotation;
	EPreviewObjectKind Kind = Entity.Mesh ? EPreviewObjectKind::Mesh : EPreviewObjectKind::Entity;
	if (Entity.Light)
	{
		Kind = static_cast<EPreviewObjectKind>(static_cast<std::uint8_t>(EPreviewObjectKind::DirectionalLight) + static_cast<std::uint8_t>(Entity.Light->Type));
	}
	else if (Entity.SkyAtmosphere)
	{
		Kind = EPreviewObjectKind::SkyAtmosphere;
	}
	else if (Entity.HeightFog)
	{
		Kind = EPreviewObjectKind::HeightFog;
	}
	else if (Entity.SoftBody && !Entity.Mesh)
	{
		Kind = EPreviewObjectKind::SoftBody;
	}

	return {
	    .Label = Entity.Name,
	    .Translation = {static_cast<float>(WorldPose.Translation.X), static_cast<float>(WorldPose.Translation.Y), static_cast<float>(WorldPose.Translation.Z)},
	    .Rotation = ToEditorRotation({static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y), static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W)}),
	    .Scale = {static_cast<float>(WorldPose.Scale3D.X), static_cast<float>(WorldPose.Scale3D.Y), static_cast<float>(WorldPose.Scale3D.Z)},
	    .Mesh = Entity.Mesh ? Entity.Mesh->Asset : FAssetId{},
	    .Id = Entity.Id,
	    .Parent = Entity.Parent.IsValid() ? std::optional{Entity.Parent} : std::nullopt,
	    .Kind = Kind,
	};
}

std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::expected<void, FLevelError> ValidateEditorPoseRange(const TTransform<double>& Pose)
{
	const auto& Position = Pose.Translation;
	const FVector3 Scale{Pose.Scale3D};
	if (std::abs(Position.X) > 1.e7 || std::abs(Position.Y) > 1.e7 || std::abs(Position.Z) > 1.e7 || Scale.X < 0.001f || Scale.Y < 0.001f || Scale.Z < 0.001f || Scale.X > 1000.f || Scale.Y > 1000.f || Scale.Z > 1000.f)
	{
		return std::unexpected(FLevelError{"Level transform exceeds the current float viewport editing range"});
	}

	return {};
}

TMatrix4<double> MakeLocalMatrix(const FLevelTransform& Transform)
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

std::expected<TTransform<double>, FLevelError> DecomposeEditorWorld(const TMatrix4<double>& Matrix)
{
	const auto Pose = TryDecomposeTransform(Matrix);
	if (!Pose)
	{
		return std::unexpected(FLevelError{"Level hierarchy requires shear or an unrepresentable world transform"});
	}

	if (auto Result = ValidateEditorPoseRange(*Pose); !Result)
	{
		return std::unexpected(Result.error());
	}

	return *Pose;
}

std::expected<FEditorHierarchy, FLevelError> BuildEditorHierarchy(const std::span<const FLevelEntity> Entities)
{
	if (auto Result = ValidateLevelEntities(Entities); !Result)
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

std::expected<void, FLevelError> SetLocalFromWorld(FLevelEntity& Entity, const TMatrix4<double>& DesiredWorld, const TMatrix4<double>& ParentWorld)
{
	const auto Inverse = TryInverseAffine(ParentWorld);
	if (!Inverse)
	{
		return std::unexpected(FLevelError{"The parent world transform cannot be inverted"});
	}

	const auto Local = TryDecomposeTransform(*Inverse * DesiredWorld);
	if (!Local)
	{
		return std::unexpected(FLevelError{"This hierarchy edit would require shear or an unrepresentable local transform"});
	}

	const auto& Rotation = Local->Rotation;
	Entity.Transform = {
	    .Translation = FWorldPosition{Local->Translation},
	    .Rotation = {static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y), static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W)},
	    .Scale = FVector3{Local->Scale3D},
	};

	return {};
}

std::expected<TTransform<double>, FLevelError> EditedWorldPose(const FPreviewObject& Object, const FPreviewObject& Previous, TTransform<double> Pose)
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
			return std::unexpected(FLevelError{"The edited world rotation is not representable"});
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

std::vector<FLevelEntity> ChangedEntities(const std::span<const FLevelEntity> Before, const std::span<const FLevelEntityChange> Changes)
{
	std::map<FObjectId, FLevelEntity> Candidates;

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

	std::vector<FLevelEntity> Entities;
	Entities.reserve(Candidates.size());

	for (auto& [Id, Entity] : Candidates)
	{
		Entities.push_back(std::move(Entity));
	}

	return Entities;
}

std::vector<FLevelEntityChange> ReverseChanges(const std::span<const FLevelEntityChange> Changes)
{
	std::vector<FLevelEntityChange> Reversed(Changes.begin(), Changes.end());

	for (FLevelEntityChange& Change : Reversed)
	{
		std::swap(Change.Before, Change.After);
	}

	return Reversed;
}

std::vector<FLevelEntityChange> DiffEntities(const std::span<const FLevelEntity> Before, const std::span<const FLevelEntity> After)
{
	std::vector<FLevelEntityChange> Changes;
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

std::expected<void, FLevelError> OrganizeEntities(const std::span<const FLevelEntity> Entities, const std::span<const FObjectId> Requested, const std::optional<FObjectId> Destination, std::vector<FLevelFolder>& Folders)
{
	const auto Target = Destination ? std::ranges::find(Folders, *Destination, &FLevelFolder::Id) : Folders.end();
	if (Destination && Target == Folders.end())
	{
		return std::unexpected(FLevelError{"The destination folder no longer exists"});
	}

	const auto Hierarchy = BuildEditorHierarchy(Entities);
	if (!Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	std::map<FObjectId, FObjectId> AssemblyRoots;

	for (const auto Index : Hierarchy->Order)
	{
		const auto& Entity = Entities[Index];
		AssemblyRoots.emplace(Entity.Id, Entity.Parent.IsValid() ? AssemblyRoots.at(Entity.Parent) : Entity.Id);
	}

	std::set<FObjectId> MovedRoots;

	for (const auto Entity : Requested)
	{
		const auto Found = AssemblyRoots.find(Entity);
		if (Found == AssemblyRoots.end())
		{
			return std::unexpected(FLevelError{"An organized entity no longer exists"});
		}

		MovedRoots.insert(Found->second);
	}

	// Only hierarchy roots determine visible placement; discard obsolete descendant membership on an assembly move.
	for (auto& Folder : Folders)
	{
		std::erase_if(Folder.Entities, [&AssemblyRoots, &MovedRoots](const FObjectId Entity)
		{
			return MovedRoots.contains(AssemblyRoots.at(Entity));
		});
	}

	if (Target != Folders.end())
	{
		Target->Entities.insert(Target->Entities.end(), MovedRoots.begin(), MovedRoots.end());
		std::ranges::sort(Target->Entities);
	}

	return {};
}
}

FEditorLevel::FEditorLevel(const std::size_t MaximumTransactions, const std::size_t MaximumMemoryCost)
    : Id(DefaultLevelId)
    , Name("Sandbox")
    , History(MaximumTransactions, MaximumMemoryCost)
{
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, 0.4f) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, -0.25f);
	const std::array Defaults{
	    FLevelEntity{.Id = DefaultCubeId, .Name = "Preview Cube", .Transform = {.Translation = FWorldPosition{0., 4., 0.}, .Rotation = Rotation}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ELevelBodyType::Dynamic},
	    FLevelEntity{.Id = DefaultFloorId, .Name = "Floor", .Transform = {.Translation = FWorldPosition{0., -0.25, 0.}, .Scale = {10.f, 0.5f, 10.f}}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ELevelBodyType::Static},
	};

	if (!World.ReplaceEntities(Defaults))
	{
		std::terminate();
	}

	RebuildObjects();

	Selection.push_back(Objects.front().Id);
	ActiveObject = Objects.front().Id;
}

std::expected<void, FLevelError> FEditorLevel::Load(const std::filesystem::path& Path)
{
	auto Document = LoadLevel(Path);
	if (!Document)
	{
		return std::unexpected(Document.error());
	}

	return LoadDocument(std::move(*Document), Path);
}

std::expected<void, FLevelError> FEditorLevel::LoadDocument(FLevelDocument Document, const std::filesystem::path& Path)
{
	if (auto Result = ValidateLevelDocument(Document); !Result)
	{
		return Result;
	}

	if (bSimulationRunning)
	{
		return std::unexpected(FLevelError{"Stop simulation before loading a level"});
	}

	if (ActiveEdit)
	{
		return std::unexpected(FLevelError{"Finish or cancel the active edit before loading a level"});
	}

	if (const auto Hierarchy = BuildEditorHierarchy(Document.Entities); !Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	if (auto Result = World.ReplaceEntities(Document.Entities); !Result)
	{
		return Result;
	}

	Id = Document.Id;
	Name = std::move(Document.Name);
	Folders = std::move(Document.Folders);
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

std::expected<void, FLevelError> FEditorLevel::CommitEdits(const std::string_view Label)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (Objects.size() != World.GetEntityCount())
	{
		RestoreObjects(false);
		return std::unexpected(FLevelError{"Editor view no longer matches the authored entity set"});
	}

	bool bChanged = false;

	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		const auto& Object = Objects[Index];
		const auto& Previous = AuthoredObjects[Index];

		if (Object.Id != Previous.Id || Object.Parent != Previous.Parent)
		{
			RestoreObjects(false);
			return std::unexpected(FLevelError{"Editor view contains stale identity or hierarchy data"});
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

	if (auto Result = ValidateLevelEntities(Candidates); !Result)
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

std::expected<void, FLevelError> FEditorLevel::Save(const std::filesystem::path& Path)
{
	if (ActiveEdit)
	{
		return std::unexpected(FLevelError{"Finish or cancel the active edit before saving a level"});
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
		return std::unexpected(FLevelError{"No level path is set; use level.save <path>"});
	}

	if (auto Result = SaveLevel(Target, {.Id = Id, .Name = Name, .Entities = World.SnapshotEntities(), .Folders = Folders}); !Result)
	{
		return Result;
	}

	CurrentPath = Target;
	History.MarkSaved();
	return {};
}

std::expected<void, FLevelError> FEditorLevel::CheckAuthoringAllowed(const bool bAllowActiveEdit) const
{
	if (bSimulationRunning)
	{
		return std::unexpected(FLevelError{"Stop simulation before editing the authored level"});
	}

	if (ActiveEdit && !bAllowActiveEdit)
	{
		return std::unexpected(FLevelError{"Finish or cancel the active edit first"});
	}

	return {};
}

void FEditorLevel::RebuildObjects()
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

void FEditorLevel::RestoreObjects(const bool bNotify)
{
	const auto Entities = World.SnapshotEntities();
	if (Objects.size() != Entities.size() || !std::ranges::equal(Objects, Entities, {}, &FPreviewObject::Id, &FLevelEntity::Id))
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

std::expected<void, FLevelError> FEditorLevel::ApplyAuthoringChanges(const std::span<const FLevelEntityChange> Changes, const std::optional<std::vector<FLevelFolder>>& AfterFolders)
{
	const auto Entities = ChangedEntities(World.SnapshotEntities(), Changes);
	if (auto Result = ValidateLevelDocument({.Id = Id, .Name = Name, .Entities = Entities, .Folders = AfterFolders ? *AfterFolders : Folders}); !Result)
	{
		return Result;
	}

	if (const auto Hierarchy = BuildEditorHierarchy(Entities); !Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	if (!Changes.empty())
	{
		if (auto Result = World.ApplyEntityChanges(Changes); !Result)
		{
			return Result;
		}
	}

	if (AfterFolders)
	{
		Folders = *AfterFolders;
	}

	return {};
}

FEditorTransaction FEditorLevel::MakeTransaction(const std::string_view Label, const std::vector<FLevelEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, const std::optional<FObjectId> BeforeActive, const std::vector<FObjectId>& AfterSelection, const std::optional<FObjectId> AfterActive, std::optional<std::vector<FLevelFolder>> BeforeFolders, std::optional<std::vector<FLevelFolder>> AfterFolders)
{
	std::size_t Cost = sizeof(FLevelEntityChange) * Changes.size() + sizeof(FObjectId) * (BeforeSelection.size() + AfterSelection.size());

	for (const FLevelEntityChange& Change : Changes)
	{
		Cost += Change.Before ? Change.Before->Name.size() : 0;
		Cost += Change.After ? Change.After->Name.size() : 0;
		Cost += Change.Before && Change.Before->Mesh ? sizeof(FAssetId) * Change.Before->Mesh->Materials.size() : 0;
		Cost += Change.After && Change.After->Mesh ? sizeof(FAssetId) * Change.After->Mesh->Materials.size() : 0;
	}

	for (const auto* Snapshot : {&BeforeFolders, &AfterFolders})
	{
		if (*Snapshot)
		{
			Cost += sizeof(FLevelFolder) * (*Snapshot)->size();

			for (const auto& Folder : **Snapshot)
			{
				Cost += Folder.Name.size() + sizeof(FObjectId) * Folder.Entities.size();
			}
		}
	}

	return {
	    .Label = std::string(Label),
	    .Apply = [this, Changes, BeforeSelection = std::move(BeforeSelection), BeforeActive, AfterSelection, AfterActive, BeforeFolders = std::move(BeforeFolders), AfterFolders = std::move(AfterFolders)](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		const auto Replay = bUndo ? ReverseChanges(Changes) : Changes;
		if (auto Result = ApplyAuthoringChanges(Replay, bUndo ? BeforeFolders : AfterFolders); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		SetSelection(bUndo ? BeforeSelection : AfterSelection, bUndo ? BeforeActive : AfterActive);

		if (!Replay.empty())
		{
			RestoreObjects();
		}

		return {};
	},
	    .MemoryCost = Cost,
	};
}

std::expected<void, FLevelError> FEditorLevel::RecordChanges(const std::string_view Label, const std::vector<FLevelEntityChange>& Changes, std::vector<FObjectId> BeforeSelection, const std::optional<FObjectId> BeforeActive, std::optional<std::vector<FLevelFolder>> BeforeFolders)
{
	if (BeforeFolders && *BeforeFolders == Folders)
	{
		BeforeFolders.reset();
	}

	if (Changes.empty() && !BeforeFolders)
	{
		return {};
	}

	const auto AfterFolders = BeforeFolders ? std::optional{Folders} : std::nullopt;
	const auto Recorded = History.RecordApplied(MakeTransaction(Label, Changes, std::move(BeforeSelection), BeforeActive, Selection, ActiveObject, std::move(BeforeFolders), AfterFolders));

	if (!Recorded)
	{
		return std::unexpected(FLevelError{Recorded.error().Message});
	}

	return {};
}

std::expected<void, FLevelError> FEditorLevel::BeginEdit(const std::string_view Label)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Label.empty())
	{
		return std::unexpected(FLevelError{"An edit requires a transaction label"});
	}

	// Flush any preceding discrete edit before taking the gesture's original snapshots.
	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	ActiveEdit = FActiveEdit{.Label = std::string(Label), .Before = World.SnapshotEntities(), .BeforeFolders = Folders, .Selection = Selection, .Active = ActiveObject};
	return {};
}

std::expected<void, FLevelError> FEditorLevel::EndEdit()
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!ActiveEdit)
	{
		return std::unexpected(FLevelError{"There is no active edit to finish"});
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
	const auto Recorded = RecordChanges(ActiveEdit->Label, Changes, ActiveEdit->Selection, ActiveEdit->Active, ActiveEdit->BeforeFolders);
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

std::expected<void, FLevelError> FEditorLevel::CancelEdit()
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!ActiveEdit)
	{
		return std::unexpected(FLevelError{"There is no active edit to cancel"});
	}

	const auto Changes = DiffEntities(World.SnapshotEntities(), ActiveEdit->Before);
	if (auto Result = ApplyAuthoringChanges(Changes, ActiveEdit->BeforeFolders); !Result)
	{
		return Result;
	}

	SetSelection(ActiveEdit->Selection, ActiveEdit->Active);
	ActiveEdit.reset();
	RestoreObjects();
	return {};
}

bool FEditorLevel::HasActiveEdit() const
{
	return ActiveEdit.has_value();
}

std::expected<void, FLevelError> FEditorLevel::Undo()
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
		return std::unexpected(FLevelError{Result.error().Message});
	}

	return {};
}

std::expected<void, FLevelError> FEditorLevel::Redo()
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
		return std::unexpected(FLevelError{Result.error().Message});
	}

	return {};
}

bool FEditorLevel::CanUndo() const
{
	return !bSimulationRunning && !ActiveEdit && History.CanUndo();
}

bool FEditorLevel::CanRedo() const
{
	return !bSimulationRunning && !ActiveEdit && History.CanRedo();
}

bool FEditorLevel::IsDirty() const
{
	return History.IsDirty() || (ActiveEdit && (ActiveEdit->Before != World.SnapshotEntities() || ActiveEdit->BeforeFolders != Folders));
}

std::string_view FEditorLevel::GetUndoLabel() const
{
	return History.GetUndoLabel();
}

std::string_view FEditorLevel::GetRedoLabel() const
{
	return History.GetRedoLabel();
}

void FEditorLevel::SetSelection(const std::span<const FObjectId> Selected, const std::optional<FObjectId> Active)
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

std::span<const FObjectId> FEditorLevel::GetSelection() const
{
	return Selection;
}

std::optional<FObjectId> FEditorLevel::GetActiveObject() const
{
	return ActiveObject;
}

std::span<const FLevelFolder> FEditorLevel::GetFolders() const
{
	return Folders;
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateFolder(const std::optional<FObjectId> Parent, const std::span<const FObjectId> Entities)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return std::unexpected(Result.error());
	}

	if (Parent && std::ranges::find(Folders, *Parent, &FLevelFolder::Id) == Folders.end())
	{
		return std::unexpected(FLevelError{"The parent folder no longer exists"});
	}

	std::unordered_set<std::string> Names;

	for (const auto& Folder : Folders)
	{
		if (Folder.Parent == Parent.value_or(FObjectId{}))
		{
			Names.insert(Folder.Name);
		}
	}

	auto After = Folders;
	const auto FolderId = FObjectId::Generate();
	After.push_back({.Id = FolderId, .Name = UniqueEntityName("New Folder", Names), .Parent = Parent.value_or(FObjectId{})});

	if (auto Result = OrganizeEntities(World.SnapshotEntities(), Entities, FolderId, After); !Result)
	{
		return std::unexpected(Result.error());
	}

	if (auto Result = ApplyStructuralChanges("Create folder", {}, Selection, ActiveObject, std::move(After)); !Result)
	{
		return std::unexpected(Result.error());
	}

	return FolderId;
}

std::expected<void, FLevelError> FEditorLevel::RenameFolder(const FObjectId Folder, const std::string_view NewName)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	auto After = Folders;
	const auto Found = std::ranges::find(After, Folder, &FLevelFolder::Id);
	if (Found == After.end())
	{
		return std::unexpected(FLevelError{"The renamed folder no longer exists"});
	}

	Found->Name = NewName;
	return ApplyStructuralChanges("Rename folder", {}, Selection, ActiveObject, std::move(After));
}

std::expected<void, FLevelError> FEditorLevel::DeleteFolder(const FObjectId Folder)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	auto After = Folders;
	const auto Found = std::ranges::find(After, Folder, &FLevelFolder::Id);
	if (Found == After.end())
	{
		return std::unexpected(FLevelError{"The deleted folder no longer exists"});
	}

	const auto Parent = Found->Parent;
	if (Parent.IsValid())
	{
		const auto Target = std::ranges::find(After, Parent, &FLevelFolder::Id);
		Target->Entities.insert(Target->Entities.end(), Found->Entities.begin(), Found->Entities.end());
		std::ranges::sort(Target->Entities);
	}

	for (auto& Child : After)
	{
		if (Child.Parent == Folder)
		{
			Child.Parent = Parent;
		}
	}

	After.erase(Found);
	return ApplyStructuralChanges("Delete folder", {}, Selection, ActiveObject, std::move(After));
}

std::expected<void, FLevelError> FEditorLevel::MoveFolder(const FObjectId Folder, const std::optional<FObjectId> Parent)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Parent && std::ranges::find(Folders, *Parent, &FLevelFolder::Id) == Folders.end())
	{
		return std::unexpected(FLevelError{"The parent folder no longer exists"});
	}

	auto After = Folders;
	const auto Found = std::ranges::find(After, Folder, &FLevelFolder::Id);
	if (Found == After.end())
	{
		return std::unexpected(FLevelError{"The moved folder no longer exists"});
	}

	Found->Parent = Parent.value_or(FObjectId{});
	return ApplyStructuralChanges("Move folder", {}, Selection, ActiveObject, std::move(After));
}

std::expected<void, FLevelError> FEditorLevel::MoveEntitiesToFolder(const std::span<const FObjectId> Entities, const std::optional<FObjectId> Folder)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	auto After = Folders;
	if (auto Result = OrganizeEntities(World.SnapshotEntities(), Entities, Folder, After); !Result)
	{
		return Result;
	}

	return ApplyStructuralChanges("Move objects to folder", {}, Selection, ActiveObject, std::move(After));
}

std::expected<void, FLevelError> FEditorLevel::ApplyStructuralChanges(const std::string_view Label, const std::vector<FLevelEntityChange>& Changes, const std::vector<FObjectId>& AfterSelection, const std::optional<FObjectId> AfterActive, std::optional<std::vector<FLevelFolder>> AfterFolders)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	for (const auto& Change : Changes)
	{
		if (!Change.Before || !Change.After || !Change.Before->Parent.IsValid() || Change.After->Parent.IsValid())
		{
			continue;
		}

		const auto& Candidates = AfterFolders ? *AfterFolders : Folders;
		if (std::ranges::any_of(Candidates, [&Change](const FLevelFolder& Folder)
		{
			return ContainsObjectId(Folder.Entities, Change.After->Id);
		}))
		{
			continue;
		}

		auto Root = Change.Before->Parent;

		while (true)
		{
			const auto Entity = World.GetEntity(*World.FindEntity(Root));
			if (!Entity->Parent.IsValid())
			{
				break;
			}

			Root = Entity->Parent;
		}

		const auto Placement = std::ranges::find_if(Folders, [Root](const FLevelFolder& Folder)
		{
			return ContainsObjectId(Folder.Entities, Root);
		});

		if (Placement != Folders.end())
		{
			if (!AfterFolders)
			{
				AfterFolders = Folders;
			}

			const auto Target = std::ranges::find(*AfterFolders, Placement->Id, &FLevelFolder::Id);
			Target->Entities.push_back(Change.After->Id);
			std::ranges::sort(Target->Entities);
		}
	}

	std::set<FObjectId> Deleted;

	for (const auto& Change : Changes)
	{
		if (Change.Before && !Change.After)
		{
			Deleted.insert(Change.Before->Id);
		}
	}

	if (!Deleted.empty() && (!Folders.empty() || AfterFolders))
	{
		if (!AfterFolders)
		{
			AfterFolders = Folders;
		}

		for (auto& Folder : *AfterFolders)
		{
			std::erase_if(Folder.Entities, [&Deleted](const FObjectId Entity)
			{
				return Deleted.contains(Entity);
			});
		}
	}

	if (AfterFolders && *AfterFolders == Folders)
	{
		AfterFolders.reset();
	}

	if (Changes.empty() && !AfterFolders)
	{
		return {};
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	const auto EffectiveActive = AfterActive ? AfterActive : (AfterSelection.empty() ? std::nullopt : std::optional{AfterSelection.back()});
	const auto BeforeFolders = AfterFolders ? std::optional{Folders} : std::nullopt;
	const auto Executed = History.Execute(MakeTransaction(Label, Changes, Selection, ActiveObject, AfterSelection, EffectiveActive, BeforeFolders, std::move(AfterFolders)));
	if (!Executed)
	{
		return std::unexpected(FLevelError{Executed.error().Message});
	}

	return {};
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Cube", .Transform = {.Translation = Position}, .Mesh = FStaticMeshComponent{.Asset = EngineCubeAsset}});
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateEmptyEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Entity", .Transform = {.Translation = Position}});
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateMeshEntity(const FAssetId Asset, const std::string_view Label, const FWorldPosition& Position)
{
	if (!Asset.IsValid())
	{
		return std::unexpected(FLevelError{"A static mesh requires a valid asset ID"});
	}

	return InsertEntity({.Id = FObjectId::Generate(), .Name = std::string(Label), .Transform = {.Translation = Position}, .Mesh = FStaticMeshComponent{.Asset = Asset}});
}

std::expected<FObjectId, FLevelError> FEditorLevel::InsertEntity(FLevelEntity Entity)
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

std::expected<void, FLevelError> FEditorLevel::AddStaticMeshToSelected(const FAssetId Asset)
{
	if (!Asset.IsValid())
	{
		return std::unexpected(FLevelError{"A static mesh requires a valid asset ID"});
	}

	return ApplySelectedMesh(FStaticMeshComponent{.Asset = Asset});
}

std::expected<void, FLevelError> FEditorLevel::RemoveStaticMeshFromSelected()
{
	return ApplySelectedMesh(std::nullopt);
}

std::expected<void, FLevelError> FEditorLevel::ApplySelectedMesh(const std::optional<FStaticMeshComponent> Mesh)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.Mesh.has_value() == Mesh.has_value())
		{
			continue;
		}

		FLevelEntity After = Before;
		After.Mesh = Mesh;
		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	return ApplyStructuralChanges(Mesh ? "Add static mesh" : "Remove static mesh", Changes, Selection, ActiveObject);
}

std::expected<void, FLevelError> FEditorLevel::AddRigidBodyToSelected(const ELevelBodyType Type)
{
	if (Type != ELevelBodyType::Static && Type != ELevelBodyType::Dynamic)
	{
		return std::unexpected(FLevelError{"Adding a rigid body requires Static or Dynamic motion"});
	}

	return ApplySelectedBodyType(Type, true);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedBodyType(const ELevelBodyType Type)
{
	if (Type != ELevelBodyType::None && Type != ELevelBodyType::Static && Type != ELevelBodyType::Dynamic)
	{
		return std::unexpected(FLevelError{"The rigid body motion type is invalid"});
	}

	return ApplySelectedBodyType(Type, false);
}

std::expected<void, FLevelError> FEditorLevel::ApplySelectedBodyType(const ELevelBodyType Type, const bool bOnlyAbsent)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.BodyType == Type || (bOnlyAbsent && Before.BodyType != ELevelBodyType::None) || (!bOnlyAbsent && Before.BodyType == ELevelBodyType::None))
		{
			continue;
		}

		FLevelEntity After = Before;
		After.BodyType = Type;

		if (Type == ELevelBodyType::None)
		{
			After.BodySettings = {};
		}

		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	return ApplyStructuralChanges(bOnlyAbsent ? "Add rigid body" : (Type == ELevelBodyType::None ? "Remove rigid body" : "Set rigid body motion"), Changes, Selection, ActiveObject);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedBodyProperty(float FLevelRigidBodySettings::* const Property, const float Value)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (!Property)
	{
		return std::unexpected(FLevelError{"A rigid body property is required"});
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.BodyType == ELevelBodyType::None || Before.BodySettings.*Property == Value)
		{
			continue;
		}

		FLevelEntity After = Before;
		After.BodySettings.*Property = Value;

		if (auto Result = ValidateLevelRigidBodySettings(After.BodySettings); !Result)
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

template <typename T> std::expected<void, FLevelError> FEditorLevel::ApplySelectedComponent(std::optional<T> FLevelEntity::* const Member, const std::optional<T> Value, const std::string_view Label, const bool bOnlyAbsent)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (Before.*Member == Value || (bOnlyAbsent && (Before.*Member).has_value()) || (!bOnlyAbsent && Value && !(Before.*Member)))
		{
			continue;
		}

		FLevelEntity After = Before;
		After.*Member = Value;
		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	if (ActiveEdit)
	{
		return World.ApplyEntityChanges(Changes);
	}

	return ApplyStructuralChanges(Label, Changes, Selection, ActiveObject);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedMaterial(const std::size_t Slot, const FAssetId Asset)
{
	if (Slot >= 256)
	{
		return std::unexpected(FLevelError{"Material slot index exceeds the limit of 256 slots"});
	}

	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		if (!Before.Mesh || (Slot < Before.Mesh->Materials.size() ? Before.Mesh->Materials[Slot] : FAssetId{}) == Asset)
		{
			continue;
		}

		FLevelEntity After = Before;
		After.Mesh->Materials.resize(std::max(Slot + 1, After.Mesh->Materials.size()));
		After.Mesh->Materials[Slot] = Asset;

		while (!After.Mesh->Materials.empty() && !After.Mesh->Materials.back().IsValid())
		{
			After.Mesh->Materials.pop_back();
		}

		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	if (ActiveEdit)
	{
		return World.ApplyEntityChanges(Changes);
	}

	return ApplyStructuralChanges("Assign material", Changes, Selection, ActiveObject);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedVisualProperty(const ELevelComponentType Type, const std::string_view Key, const FLevelPropertyValue& Value)
{
	if (auto Result = CheckAuthoringAllowed(true); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;

	for (const FObjectId Object : Selection)
	{
		const FLevelEntity Before = *World.GetEntity(*World.FindEntity(Object));
		const auto Previous = GetLevelVisualProperty(Before, Type, Key);
		if (!Previous || *Previous == Value)
		{
			continue;
		}

		FLevelEntity After = Before;
		if (auto Result = SetLevelVisualProperty(After, Type, Key, Value); !Result)
		{
			return Result;
		}

		Changes.push_back({.Before = Before, .After = std::move(After)});
	}

	if (ActiveEdit)
	{
		return World.ApplyEntityChanges(Changes);
	}

	return ApplyStructuralChanges("Edit visual property", Changes, Selection, ActiveObject);
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateLightEntity(const ELightType Type, const FWorldPosition& Position)
{
	constexpr std::array<std::string_view, 5> Names{"Directional Light", "Sky Light", "Point Light", "Spot Light", "Rect Light"};
	if (Type > ELightType::Rect)
	{
		return std::unexpected(FLevelError{"Unknown light type"});
	}

	FLightComponent Light{.Type = Type};
	Light.Intensity = Type == ELightType::Directional ? 50'000.f : (Type == ELightType::Sky ? 1.f : 1000.f);
	Light.Range = Type == ELightType::Directional ? 60.f : Light.Range;
	return InsertEntity({.Id = FObjectId::Generate(), .Name = std::string(Names[static_cast<std::size_t>(Type)]), .Transform = {.Translation = Position}, .Light = Light});
}

std::expected<void, FLevelError> FEditorLevel::AddLightToSelected(const ELightType Type)
{
	if (Type > ELightType::Rect)
	{
		return std::unexpected(FLevelError{"Unknown light type"});
	}

	FLightComponent Light{.Type = Type};
	Light.Intensity = Type == ELightType::Directional ? 50'000.f : (Type == ELightType::Sky ? 1.f : 1000.f);
	Light.Range = Type == ELightType::Directional ? 60.f : Light.Range;
	return ApplySelectedComponent(&FLevelEntity::Light, std::optional{Light}, "Add light", true);
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateSkyAtmosphereEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Sky Atmosphere", .Transform = {.Translation = Position}, .SkyAtmosphere = FSkyAtmosphereComponent{}});
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateHeightFogEntity(const FWorldPosition& Position)
{
	return InsertEntity({.Id = FObjectId::Generate(), .Name = "Height Fog", .Transform = {.Translation = Position}, .HeightFog = FHeightFogComponent{}});
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedLight(const std::optional<FLightComponent> Light)
{
	return ApplySelectedComponent(&FLevelEntity::Light, Light, Light ? "Edit light" : "Remove light");
}

std::expected<void, FLevelError> FEditorLevel::AddSkyAtmosphereToSelected()
{
	return ApplySelectedComponent(&FLevelEntity::SkyAtmosphere, std::optional{FSkyAtmosphereComponent{}}, "Add sky atmosphere", true);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedSkyAtmosphere(const std::optional<FSkyAtmosphereComponent> Atmosphere)
{
	return ApplySelectedComponent(&FLevelEntity::SkyAtmosphere, Atmosphere, Atmosphere ? "Edit sky atmosphere" : "Remove sky atmosphere");
}

std::expected<FObjectId, FLevelError> FEditorLevel::CreateSoftBodyEntity(const ESoftBodyShape Shape, const FWorldPosition& Position)
{
	constexpr std::array<std::string_view, 3> Names{"Rope", "Cloth", "Soft Ball"};
	if (Shape > ESoftBodyShape::Ball)
	{
		return std::unexpected(FLevelError{"Unknown soft body shape"});
	}

	return InsertEntity({.Id = FObjectId::Generate(), .Name = std::string(Names[static_cast<std::size_t>(Shape)]), .Transform = {.Translation = Position}, .SoftBody = MakeDefaultSoftBody(Shape)});
}

std::expected<void, FLevelError> FEditorLevel::AddSoftBodyToSelected()
{
	return ApplySelectedComponent(&FLevelEntity::SoftBody, std::optional{FSoftBodyComponent{}}, "Add soft body", true);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedSoftBody(const std::optional<FSoftBodyComponent> SoftBody)
{
	return ApplySelectedComponent(&FLevelEntity::SoftBody, SoftBody, SoftBody ? "Edit soft body" : "Remove soft body");
}

std::expected<void, FLevelError> FEditorLevel::AddHeightFogToSelected()
{
	return ApplySelectedComponent(&FLevelEntity::HeightFog, std::optional{FHeightFogComponent{}}, "Add height fog", true);
}

std::expected<void, FLevelError> FEditorLevel::SetSelectedHeightFog(const std::optional<FHeightFogComponent> Fog)
{
	return ApplySelectedComponent(&FLevelEntity::HeightFog, Fog, Fog ? "Edit height fog" : "Remove height fog");
}

std::expected<void, FLevelError> FEditorLevel::DuplicateSelected(const bool bWithinActiveEdit, const FVector3d& WorldOffset)
{
	if (auto Result = CheckAuthoringAllowed(bWithinActiveEdit); !Result)
	{
		return Result;
	}

	if (bWithinActiveEdit)
	{
		if (!ActiveEdit)
		{
			return std::unexpected(FLevelError{"Begin an edit before duplicating within a gesture"});
		}

		if (ActiveEdit->bDuplicated)
		{
			return {};
		}
	}

	if (!std::isfinite(WorldOffset.X) || !std::isfinite(WorldOffset.Y) || !std::isfinite(WorldOffset.Z))
	{
		return std::unexpected(FLevelError{"Duplication requires a finite world translation offset"});
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::unordered_set<std::string> Names;
	const auto Entities = World.SnapshotEntities();

	for (const FLevelEntity& Entity : Entities)
	{
		Names.insert(Entity.Name);
	}

	std::vector<FLevelEntityChange> Changes;
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

	for (const FLevelEntity& Original : Entities)
	{
		if (!Remapped.contains(Original.Id))
		{
			continue;
		}

		FLevelEntity Entity = Original;
		Entity.Id = Remapped.at(Original.Id);
		Entity.Name = UniqueEntityName(Original.Name, Names, " Copy");

		if (Entity.SkyAtmosphere && Remapped.contains(Entity.SkyAtmosphere->Sun))
		{
			Entity.SkyAtmosphere->Sun = Remapped.at(Entity.SkyAtmosphere->Sun);
		}

		if (Entity.SoftBody && Remapped.contains(Entity.SoftBody->Attachment))
		{
			Entity.SoftBody->Attachment = Remapped.at(Entity.SoftBody->Attachment);
		}

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

	auto AfterFolders = Folders;

	for (auto& Folder : AfterFolders)
	{
		const auto OriginalMembers = Folder.Entities;

		for (const auto Original : OriginalMembers)
		{
			const auto Duplicate = Remapped.find(Original);
			if (Duplicate != Remapped.end())
			{
				Folder.Entities.push_back(Duplicate->second);
			}
		}

		std::ranges::sort(Folder.Entities);
	}

	if (bWithinActiveEdit)
	{
		if (Changes.empty())
		{
			return {};
		}

		if (auto Result = ApplyAuthoringChanges(Changes, AfterFolders); !Result)
		{
			return Result;
		}

		ActiveEdit->bDuplicated = true;
		SetSelection(Duplicated, DuplicatedActive);
		RebuildObjects();
		return {};
	}

	return ApplyStructuralChanges("Duplicate objects", Changes, Duplicated, DuplicatedActive, std::move(AfterFolders));
}

std::expected<void, FLevelError> FEditorLevel::DeleteSelected()
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
	}

	std::vector<FLevelEntityChange> Changes;
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

std::expected<void, FLevelError> FEditorLevel::ReparentEntities(const std::span<const FObjectId> Requested, const std::optional<FObjectId> Parent)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	if (Parent && (!Parent->IsValid() || !World.FindEntity(*Parent)))
	{
		return std::unexpected(FLevelError{"The requested parent no longer exists"});
	}

	for (const auto Object : Requested)
	{
		if (!World.FindEntity(Object))
		{
			return std::unexpected(FLevelError{"A reparented entity no longer exists"});
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
				return std::unexpected(FLevelError{"An entity cannot be parented to itself or one of its descendants"});
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
	std::vector<FLevelEntityChange> Changes;

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

std::expected<void, FLevelError> FEditorLevel::ReparentSelected(const std::optional<FObjectId> Parent)
{
	return ReparentEntities(Selection, Parent);
}

std::expected<std::string, FLevelError> FEditorLevel::CopySelected() const
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return std::unexpected(Result.error());
	}

	FLevelDocument Document{.Id = Id, .Name = "Clipboard"};
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

	return SerializeLevel(Document);
}

std::expected<void, FLevelError> FEditorLevel::PasteEntities(const std::string_view Text)
{
	if (auto Result = CheckAuthoringAllowed(); !Result)
	{
		return Result;
	}

	auto Document = ParseLevel(Text);
	if (!Document)
	{
		return std::unexpected(Document.error());
	}

	if (const auto Hierarchy = BuildEditorHierarchy(Document->Entities); !Hierarchy)
	{
		return std::unexpected(Hierarchy.error());
	}

	std::vector<FLevelEntityChange> Changes;
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

	for (FLevelEntity& Entity : Document->Entities)
	{
		Entity.Id = Remapped.at(Entity.Id);

		if (Entity.SkyAtmosphere && Remapped.contains(Entity.SkyAtmosphere->Sun))
		{
			Entity.SkyAtmosphere->Sun = Remapped.at(Entity.SkyAtmosphere->Sun);
		}

		if (Entity.SoftBody && Remapped.contains(Entity.SoftBody->Attachment))
		{
			Entity.SoftBody->Attachment = Remapped.at(Entity.SoftBody->Attachment);
		}

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

void FEditorLevel::SetPath(std::filesystem::path Path)
{
	CurrentPath = std::move(Path);
}

void FEditorLevel::SetSimulationRunning(const bool bRunning)
{
	bSimulationRunning = bRunning;
}

std::expected<void, FLevelError> FEditorLevel::UpdatePreviewHierarchy(const std::span<const FObjectId> OverrideWorldPoses)
{
	if (std::ranges::none_of(AuthoredObjects, [](const FPreviewObject& Object)
	{
		return Object.Parent.has_value();
	}))
	{
		if (Objects.size() != World.GetEntityCount() || !std::ranges::equal(Objects, AuthoredObjects, {}, &FPreviewObject::Id, &FPreviewObject::Id))
		{
			return std::unexpected(FLevelError{"Simulation preview contains a stale entity set"});
		}

		for (const auto Object : OverrideWorldPoses)
		{
			if (!World.FindEntity(Object))
			{
				return std::unexpected(FLevelError{"A simulation pose refers to a stale entity"});
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

	if (Objects.size() != Entities.size() || !std::ranges::equal(Objects, Entities, {}, &FPreviewObject::Id, &FLevelEntity::Id))
	{
		return std::unexpected(FLevelError{"Simulation preview contains a stale entity set"});
	}

	const std::set<FObjectId> Overrides(OverrideWorldPoses.begin(), OverrideWorldPoses.end());

	for (const auto Object : Overrides)
	{
		if (!Hierarchy->Indices.contains(Object))
		{
			return std::unexpected(FLevelError{"A simulation pose refers to a stale entity"});
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

std::vector<std::size_t> FEditorLevel::FindBodies(const ELevelBodyType Type) const
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

std::vector<FPreviewObject>& FEditorLevel::GetObjects()
{
	return Objects;
}

const std::vector<FPreviewObject>& FEditorLevel::GetObjects() const
{
	return Objects;
}

std::uint64_t FEditorLevel::GetGeneration() const
{
	return Generation;
}

const std::filesystem::path& FEditorLevel::GetPath() const
{
	return CurrentPath;
}

std::string_view FEditorLevel::GetName() const
{
	return Name;
}

const FWorld& FEditorLevel::GetWorld() const
{
	return World;
}

std::expected<void, FEditorCommandError> RegisterEditorLevelCommands(FEditorCommandRegistry& Commands, const std::shared_ptr<FEditorLevel>& Level)
{
	if (auto Result = Commands.Register({.Name = "level.save", .Description = "Save the authored level. Usage: level.save [path]", .Handler = [Level](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() > 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: level.save [path]"});
		}

		if (auto Saved = Level->Save(Arguments.empty() ? std::filesystem::path{} : Utf8Path(Arguments.front())); !Saved)
		{
			return std::unexpected(FEditorCommandError{.Message = Saved.error().Message});
		}

		return FEditorCommandResult{.Message = "Level saved"};
	}});
	    !Result)
	{
		return Result;
	}

	return Commands.Register({.Name = "level.load", .Description = "Load a level without replacing the current level on failure. Usage: level.load <path>", .Handler = [Level](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: level.load <path>"});
		}

		if (auto Loaded = Level->Load(Utf8Path(Arguments.front())); !Loaded)
		{
			return std::unexpected(FEditorCommandError{.Message = Loaded.error().Message});
		}

		return FEditorCommandResult{.Message = "Level loaded"};
	}});
}
}
