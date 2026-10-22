#include "Herta/AssetPipeline/GameStaging.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Project/Project.h"

#include <array>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr std::string_view StageMarker = ".hertastage";

struct FMount
{
	std::filesystem::path SourceRoot;
	std::filesystem::path StagedRoot;
};

struct FLocation
{
	const FMount* Mount = nullptr;
	FAssetRecord Record;
};

using FStagedKey = std::pair<FAssetId, std::optional<ETextureColorSpace>>;

[[nodiscard]] std::unexpected<FAssetError> StageError(std::string Message)
{
	return std::unexpected(FAssetError{std::move(Message)});
}

// Only a directory that is empty or was staged before may be replaced, so a mistyped output path never deletes unrelated files.
[[nodiscard]] std::expected<void, FAssetError> PrepareOutput(const std::filesystem::path& OutputRoot, const std::filesystem::path& StagedProjectRoot)
{
	std::error_code Error;
	if (std::filesystem::exists(OutputRoot, Error) && !std::filesystem::is_empty(OutputRoot, Error) && !std::filesystem::exists(OutputRoot / StageMarker, Error))
	{
		return StageError(std::format("Refusing to stage into '{}': it is not empty and was not staged before", PathToUtf8(OutputRoot)));
	}

	for (const std::filesystem::path& Owned : {GetStagedEngineContentRoot(OutputRoot), StagedProjectRoot})
	{
		std::filesystem::remove_all(Owned, Error);
		if (Error)
		{
			return StageError(std::format("Cannot replace staged '{}': {}", PathToUtf8(Owned), Error.message()));
		}
	}

	std::filesystem::create_directories(OutputRoot, Error);
	if (Error)
	{
		return StageError(std::format("Cannot create '{}': {}", PathToUtf8(OutputRoot), Error.message()));
	}

	return WriteFileAtomically(OutputRoot / StageMarker, {});
}

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadSource(const std::filesystem::path& Path)
{
	auto Bytes = ReadWholeFile(Path, std::uint64_t{64} * 1024 * 1024);
	if (!Bytes)
	{
		return std::unexpected(Bytes.error());
	}

	if (!*Bytes)
	{
		return StageError(std::format("'{}' does not exist", PathToUtf8(Path)));
	}

	return std::move(**Bytes);
}
}

std::filesystem::path GetStagedEngineContentRoot(const std::filesystem::path& OutputRoot)
{
	return OutputRoot / "Engine" / "Content";
}

std::expected<FGameStageResult, FAssetError> StageGameContent(const FGameStageRequest& Request)
{
	const auto Project = LoadProject(Request.ProjectPath);
	if (!Project)
	{
		return StageError(std::format("Cannot load project: {}", Project.error().Message));
	}

	const auto Level = LoadLevel(Project->StartingLevel);
	if (!Level)
	{
		return StageError(std::format("Cannot load the starting level: {}", Level.error().Message));
	}

	const std::filesystem::path StagedProjectRoot = Request.OutputRoot / Project->Descriptor.Name;
	const std::filesystem::path StagedProjectPath = StagedProjectRoot / Project->DescriptorPath.filename();
	const std::filesystem::path StagedLevelPath = StagedProjectRoot / Project->StartingLevel.lexically_relative(Project->Root);
	const std::filesystem::path StagedContentRoot = StagedProjectRoot / Project->ContentRoot.lexically_relative(Project->Root);
	const auto IsInsideProject = [&StagedProjectRoot](const std::filesystem::path& Path)
	{
		const std::filesystem::path Relative = Path.lexically_relative(StagedProjectRoot);
		return !Relative.empty() && *Relative.begin() != "..";
	};

	if (!IsInsideProject(StagedLevelPath) || !IsInsideProject(StagedContentRoot))
	{
		return StageError("Staging requires the starting level and content root inside the project directory");
	}

	const std::array Mounts{
	    FMount{.SourceRoot = Request.EngineContentRoot, .StagedRoot = GetStagedEngineContentRoot(Request.OutputRoot)},
	    FMount{.SourceRoot = Project->ContentRoot, .StagedRoot = StagedContentRoot},
	};

	std::map<FAssetId, FLocation> Locations;
	for (const FMount& Mount : Mounts)
	{
		const auto Scan = ScanContentRoot(Mount.SourceRoot);
		if (!Scan)
		{
			return std::unexpected(Scan.error());
		}

		for (const FAssetRecord& Record : Scan->Registry.GetRecords())
		{
			Locations.insert_or_assign(Record.Id, FLocation{.Mount = &Mount, .Record = Record});
		}
	}

	// Every reference must resolve: a staged build has no importer to fall back on.
	std::set<FStagedKey> Pending;
	for (const FLevelEntity& Entity : Level->Entities)
	{
		if (Entity.Mesh)
		{
			Pending.emplace(Entity.Mesh->Asset, std::nullopt);
			for (const FAssetId& Material : Entity.Mesh->Materials)
			{
				if (Material.IsValid())
				{
					Pending.emplace(Material, std::nullopt);
				}
			}
		}

		if (Entity.SoftBody && Entity.SoftBody->Material.IsValid())
		{
			Pending.emplace(Entity.SoftBody->Material, std::nullopt);
		}

		// Sky environments are HDR radiance, which only filters correctly in linear light.
		if (Entity.Light && Entity.Light->Type == ELightType::Sky && Entity.Light->Environment.IsValid())
		{
			Pending.emplace(Entity.Light->Environment, ETextureColorSpace::Linear);
		}
	}

	auto Output = PrepareOutput(Request.OutputRoot, StagedProjectRoot);
	if (!Output)
	{
		return std::unexpected(Output.error());
	}

	FGameStageResult Result{.ProjectPath = StagedProjectPath};
	std::set<FStagedKey> Staged;
	while (!Pending.empty())
	{
		const FStagedKey Key = Pending.extract(Pending.begin()).value();
		if (!Staged.insert(Key).second)
		{
			continue;
		}

		const auto Location = Locations.find(Key.first);
		if (Location == Locations.end())
		{
			return StageError(std::format("Asset {} referenced by the starting level is not registered in Engine or project content", Key.first.ToString()));
		}

		const FMount& Mount = *Location->second.Mount;
		FAssetCookRequest Cook{
		    .ContentRoot = Mount.SourceRoot,
		    .DerivedDataRoot = Request.DerivedDataRoot,
		    .SourcePath = Location->second.Record.SourcePath,
		    .TargetPlatform = Request.TargetPlatform,
		    .DependencyContentRoots = {Request.EngineContentRoot, Project->ContentRoot, Request.EngineShaderRoot},
		    .TextureColorSpace = Key.second,
		    .EngineContentRoot = Request.EngineContentRoot,
		    .GameContentRoot = Project->ContentRoot,
		};

		const auto Cooked = CookAssetInWorker(Cook, {.WorkerPath = Request.WorkerPath});
		if (!Cooked)
		{
			return StageError(std::format("Cannot cook {}: {}", Location->second.Record.SourcePath, Cooked.error().Message));
		}

		const auto Asset = LoadCookedAsset(Request.DerivedDataRoot, Cooked->Key);
		if (!Asset)
		{
			return std::unexpected(Asset.error());
		}

		if (const auto* const Material = std::get_if<FMaterialAsset>(&*Asset))
		{
			// ponytail: custom material shaders are compiled by the editor only; stage their cooked shaders when a shipped project uses one.
			if (!Material->ShaderPath.empty())
			{
				return StageError(std::format("Material {} uses a custom shader, which staged builds do not support yet", Location->second.Record.SourcePath));
			}

			for (const FMaterialTextureBinding& Binding : Material->Textures)
			{
				if (Binding.Texture.IsValid())
				{
					Pending.emplace(Binding.Texture, Binding.ColorSpace);
				}
			}
		}

		const bool bTexture = std::holds_alternative<FCookedTexture>(*Asset);
		if (bTexture != Key.second.has_value())
		{
			return StageError(std::format("{} is referenced as a {} but cooks to a different asset kind", Location->second.Record.SourcePath, Key.second ? "texture" : "model or material"));
		}

		const auto Bytes = SerializeCookedAsset(*Asset);
		if (!Bytes)
		{
			return std::unexpected(Bytes.error());
		}

		if (auto Written = WriteFileAtomically(GetStagedAssetPath(Mount.StagedRoot, Key.first, Key.second), *Bytes); !Written)
		{
			return std::unexpected(Written.error());
		}

		++Result.Assets;
		Result.Bytes += Bytes->size();
	}

	// The descriptor and level are copied verbatim so the staged build loads exactly what was authored. The HUD font ships with its license.
	const std::filesystem::path FontSource = Request.EngineContentRoot / "Editor" / "Fonts" / "Roboto";
	const std::filesystem::path FontDestination = GetStagedEngineContentRoot(Request.OutputRoot) / "Fonts";
	const std::array Copies{
	    std::pair{Project->DescriptorPath, StagedProjectPath},
	    std::pair{Project->StartingLevel, StagedLevelPath},
	    std::pair{FontSource / "Roboto-Medium.ttf", FontDestination / "Roboto-Medium.ttf"},
	    std::pair{FontSource / "OFL.txt", FontDestination / "OFL.txt"},
	};

	for (const auto& [Source, Destination] : Copies)
	{
		const auto Bytes = ReadSource(Source);
		if (!Bytes)
		{
			return std::unexpected(Bytes.error());
		}

		if (auto Written = WriteFileAtomically(Destination, *Bytes); !Written)
		{
			return std::unexpected(Written.error());
		}

		Result.Bytes += Bytes->size();
	}

	std::error_code Error;
	std::filesystem::create_directories(StagedContentRoot, Error);
	if (Error)
	{
		return StageError(std::format("Cannot create '{}': {}", PathToUtf8(StagedContentRoot), Error.message()));
	}

	return Result;
}
}
