#include "Herta/AssetPipeline/GameStaging.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/MaterialShaderCompiler.h"
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

// Custom material shaders ship precompiled, so the game needs neither the shader compiler nor the Material Graph. Release builds drop debug info.
[[nodiscard]] std::expected<std::uint64_t, FAssetError> StageMaterialShader(const FGameStageRequest& Request, const std::array<FMount, 2>& Mounts, const std::string& MountedPath)
{
	const std::array Roots{Request.EngineShaderRoot, Mounts[0].SourceRoot, Mounts[1].SourceRoot};
	const auto Sources = CollectMaterialShaderSources(Roots, MountedPath);
	if (!Sources)
	{
		return std::unexpected(Sources.error());
	}

	std::filesystem::path Worker = Request.WorkerPath;
	Worker.replace_filename(Request.WorkerPath.has_extension() ? "HertaShaderWorker" + PathToUtf8(Request.WorkerPath.extension()) : "HertaShaderWorker");
	const auto Stages = CompileMaterialShader(Worker, *Sources, HashMaterialShaderSources(*Sources), false, {});
	if (!Stages)
	{
		return std::unexpected(Stages.error());
	}

	const FMount& Mount = MountedPath.starts_with("Engine/") ? Mounts[0] : Mounts[1];
	const std::string_view Relative = std::string_view(MountedPath).substr(MountedPath.find('/') + 1);
	std::uint64_t Bytes = 0;
	for (std::size_t Stage = 0; Stage < Stages->size(); ++Stage)
	{
		if (auto Written = WriteFileAtomically(GetStagedMaterialShaderPath(Mount.StagedRoot, Relative, MaterialShaderStageNames[Stage]), (*Stages)[Stage]); !Written)
		{
			return std::unexpected(Written.error());
		}

		Bytes += (*Stages)[Stage].size();
	}

	const std::string Table = SerializeMaterialShaderParameters(Sources->Parameters);
	if (auto Written = WriteFileAtomically(GetStagedMaterialShaderParametersPath(Mount.StagedRoot, Relative), std::as_bytes(std::span(Table))); !Written)
	{
		return std::unexpected(Written.error());
	}

	return Bytes + Table.size();
}

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadSource(const std::filesystem::path& Path)
{
	auto Bytes = ReadWholeFile(Path, std::uint64_t{64} * 1024 * 1024);
	if (!Bytes)
	{
		return std::unexpected(Bytes.error());
	}

	std::optional<std::vector<std::byte>>& FileBytes = *Bytes;
	if (!FileBytes)
	{
		return StageError(std::format("'{}' does not exist", PathToUtf8(Path)));
	}

	return std::move(*FileBytes);
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
	if (Level->PlayerCharacter.IsValid())
	{
		Pending.emplace(Level->PlayerCharacter, std::nullopt);
	}

	for (const FAssetId& Sound : Level->Sounds)
	{
		if (Sound.IsValid())
		{
			Pending.emplace(Sound, std::nullopt);
		}
	}

	for (const FAssetId& Effect : Level->Effects)
	{
		if (Effect.IsValid())
		{
			Pending.emplace(Effect, std::nullopt);
		}
	}

	// Region levels stage beside the starting level at the same relative paths. Regions do not nest, so only the starting level's count.
	std::vector<std::pair<std::filesystem::path, std::filesystem::path>> RegionCopies;
	std::vector<FLevelEntity> Entities = Level->Entities;
	for (const FLevelRegion& Region : Level->Regions)
	{
		const std::filesystem::path Source = ResolveLevelRegionPath(Project->StartingLevel, Region);
		const std::filesystem::path Destination = ResolveLevelRegionPath(StagedLevelPath, Region);
		if (!IsInsideProject(Destination))
		{
			return StageError(std::format("Region '{}' places a level outside the project", Region.Name));
		}

		if (std::ranges::contains(RegionCopies, Source, &std::pair<std::filesystem::path, std::filesystem::path>::first))
		{
			continue;
		}

		const auto Placed = LoadLevel(Source);
		if (!Placed)
		{
			return StageError(std::format("Cannot load the level of region '{}': {}", Region.Name, Placed.error().Message));
		}

		Entities.insert(Entities.end(), Placed->Entities.begin(), Placed->Entities.end());
		RegionCopies.emplace_back(Source, Destination);
	}

	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.AudioEmitter && Entity.AudioEmitter->Sound.IsValid())
		{
			Pending.emplace(Entity.AudioEmitter->Sound, std::nullopt);
		}

		if (Entity.ParticleEmitter && Entity.ParticleEmitter->System.IsValid())
		{
			Pending.emplace(Entity.ParticleEmitter->System, std::nullopt);
		}

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
	std::set<std::string> StagedShaders;
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
			if (std::string Shader = GetMountedMaterialShaderPath(*Material, Material->Name); !Shader.empty() && !StagedShaders.contains(Shader))
			{
				const auto Bytes = StageMaterialShader(Request, Mounts, Shader);
				if (!Bytes)
				{
					return StageError(std::format("Cannot compile the shader of {}: {}", Location->second.Record.SourcePath, Bytes.error().Message));
				}

				Result.Bytes += *Bytes;
				StagedShaders.insert(std::move(Shader));
			}

			for (const FMaterialTextureBinding& Binding : Material->Textures)
			{
				if (Binding.Texture.IsValid())
				{
					Pending.emplace(Binding.Texture, Binding.ColorSpace);
				}
			}
		}

		if (const auto* const Graph = std::get_if<FCookedSoundGraph>(&*Asset))
		{
			for (const FAssetId& Wave : GetSoundGraphWaves(Graph->Graph))
			{
				Pending.emplace(Wave, std::nullopt);
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
	std::vector Copies{
	    std::pair{Project->DescriptorPath, StagedProjectPath},
	    std::pair{Project->StartingLevel, StagedLevelPath},
	    std::pair{FontSource / "Roboto-Medium.ttf", FontDestination / "Roboto-Medium.ttf"},
	    std::pair{FontSource / "OFL.txt", FontDestination / "OFL.txt"},
	};

	Copies.insert(Copies.end(), RegionCopies.begin(), RegionCopies.end());

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
