#include "EditorScene.h"

#include "Herta/EditorCore/CommandRegistry.h"

#include <exception>
#include <format>

namespace Herta
{
namespace
{
constexpr FObjectId DefaultSceneId{0x10935c1f4f594b12, 0x9a139dca302101e0};
constexpr FObjectId DefaultCubeId{0x21935c1f4f594b12, 0x9a139dca302101e1};
constexpr FObjectId DefaultFloorId{0x31935c1f4f594b12, 0x9a139dca302101e2};

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
	    .Mesh = Entity.Mesh->Asset,
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
}

FEditorScene::FEditorScene()
    : Id(DefaultSceneId)
    , Name("Sandbox")
{
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, 0.4f) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, -0.25f);
	const std::array Defaults{
	    FSceneEntity{.Id = DefaultCubeId, .Name = "Preview Cube", .Transform = {.Translation = FWorldPosition{0., 4., 0.}, .Rotation = Rotation}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyMotion = ESceneBodyMotion::Dynamic},
	    FSceneEntity{.Id = DefaultFloorId, .Name = "Floor", .Transform = {.Translation = FWorldPosition{0., -0.25, 0.}, .Scale = {10.f, 0.5f, 10.f}}, .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyMotion = ESceneBodyMotion::Static},
	};

	if (!World.ReplaceEntities(Defaults))
	{
		std::terminate();
	}

	for (const FSceneEntity& Entity : World.SnapshotEntities())
	{
		Objects.push_back(ToEditorObject(Entity));
	}
}

std::expected<void, FSceneError> FEditorScene::Load(const std::filesystem::path& Path)
{
	if (bSimulationRunning)
	{
		return std::unexpected(FSceneError{"Stop simulation before loading a scene"});
	}

	auto Document = LoadScene(Path);
	if (!Document)
	{
		return std::unexpected(Document.error());
	}

	// Hierarchical and non-mesh entities are valid runtime data, but their editor authoring UI is a later slice.
	for (const FSceneEntity& Entity : Document->Entities)
	{
		if (Entity.Parent.IsValid() || !Entity.Mesh)
		{
			return std::unexpected(FSceneError{"This editor slice opens flat static-mesh scenes only"});
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
	Objects.clear();

	for (const FSceneEntity& Entity : World.SnapshotEntities())
	{
		Objects.push_back(ToEditorObject(Entity));
	}

	++Generation;
	return {};
}

std::expected<void, FSceneError> FEditorScene::CommitEdits()
{
	if (bSimulationRunning)
	{
		return {};
	}

	std::vector<FSceneEntity> Candidates;
	Candidates.reserve(Objects.size());

	for (const FPreviewObject& Object : Objects)
	{
		const auto Handle = World.FindEntity(Object.Id);
		if (!Handle)
		{
			return std::unexpected(FSceneError{"Editor view contains a stale entity"});
		}

		FSceneEntity Entity = *World.GetEntity(*Handle);
		const FPreviewObject Previous = ToEditorObject(Entity);
		Entity.Name = Object.Label;
		Entity.Mesh = FStaticMeshComponent{Object.Mesh};

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
			return Result;
		}

		Candidates.push_back(std::move(Entity));
	}

	if (auto Result = ValidateSceneEntities(Candidates); !Result)
	{
		return Result;
	}

	for (const FSceneEntity& Entity : Candidates)
	{
		const FEntityId Handle = *World.FindEntity(Entity.Id);
		if (World.GetEntity(Handle) != Entity)
		{
			if (auto Result = World.SetEntity(Handle, Entity); !Result)
			{
				return Result;
			}
		}
	}

	return {};
}

std::expected<void, FSceneError> FEditorScene::Save(const std::filesystem::path& Path)
{
	if (auto Result = CommitEdits(); !Result)
	{
		return Result;
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
	return {};
}

void FEditorScene::SetPath(std::filesystem::path Path)
{
	CurrentPath = std::move(Path);
}

void FEditorScene::SetSimulationRunning(const bool bRunning)
{
	bSimulationRunning = bRunning;
}

std::optional<std::size_t> FEditorScene::FindBody(const ESceneBodyMotion Motion) const
{
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		const auto Handle = World.FindEntity(Objects[Index].Id);
		if (Handle && World.GetEntity(*Handle)->BodyMotion == Motion)
		{
			return Index;
		}
	}

	return std::nullopt;
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
