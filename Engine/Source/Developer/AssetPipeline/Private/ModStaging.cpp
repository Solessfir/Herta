#include "Herta/AssetPipeline/ModStaging.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Assets/Material.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/Project/Project.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FAssetError> ModError(std::string Message)
{
	return std::unexpected(FAssetError{std::move(Message)});
}
}

std::expected<FModStageResult, FAssetError> StageModContent(const FModStageRequest& Request)
{
	const auto Project = LoadProject(Request.ProjectPath);
	if (!Project)
	{
		return ModError(std::format("Cannot load project: {}", Project.error().Message));
	}

	const auto ManifestBytes = ReadDiskFile(Request.ModRoot / DataModManifestName);
	auto Manifest = ManifestBytes ? ParseModManifest(std::string_view(reinterpret_cast<const char*>(ManifestBytes->data()), ManifestBytes->size())) : std::unexpected(ManifestBytes.error());
	if (!Manifest)
	{
		return ModError(std::format("Cannot read {}: {}", (Request.ModRoot / DataModManifestName).generic_string(), Manifest.error().Message));
	}

	const std::filesystem::path ModContent = Request.ModRoot / "Content";
	const auto Scan = ScanContentRoot(ModContent);
	if (!Scan)
	{
		return std::unexpected(Scan.error());
	}

	// The mod's files take the game's staged paths: <Project>/<content root>/<id>.hasset.
	const std::filesystem::path FilesRoot = Request.OutputRoot / "Files";
	const std::filesystem::path StagedContent = FilesRoot / Project->Descriptor.Name / Project->ContentRoot.lexically_relative(Project->Root);
	std::error_code Error;
	std::filesystem::remove_all(FilesRoot, Error);
	FModStageResult Result{.Manifest = *Manifest};
	for (const FAssetRecord& Record : Scan->Registry.GetRecords())
	{
		const bool bTexture = Record.Importer == "Texture";
		const std::array<std::optional<ETextureColorSpace>, 2> Spaces = bTexture ? std::array<std::optional<ETextureColorSpace>, 2>{ETextureColorSpace::Srgb, ETextureColorSpace::Linear} : std::array<std::optional<ETextureColorSpace>, 2>{std::nullopt, std::nullopt};
		for (std::size_t Space = 0; Space < (bTexture ? 2u : 1u); ++Space)
		{
			const FAssetCookRequest Cook{
			    .ContentRoot = ModContent,
			    .DerivedDataRoot = Request.DerivedDataRoot,
			    .SourcePath = Record.SourcePath,
			    .TargetPlatform = Request.TargetPlatform,
			    .DependencyContentRoots = {Request.EngineContentRoot, Project->ContentRoot, Request.EngineShaderRoot},
			    .TextureColorSpace = Spaces[Space],
			    .EngineContentRoot = Request.EngineContentRoot,
			    .GameContentRoot = Project->ContentRoot,
			};

			const auto Cooked = CookAssetInWorker(Cook, {.WorkerPath = Request.WorkerPath});
			const auto Asset = Cooked ? LoadCookedAsset(Request.DerivedDataRoot, Cooked->Key) : std::unexpected(Cooked.error());
			if (!Asset)
			{
				return ModError(std::format("Cannot cook {}: {}", Record.SourcePath, Asset.error().Message));
			}

			if (const auto* const Material = std::get_if<FMaterialAsset>(&*Asset); Material && !Material->ShaderPath.empty())
			{
				return ModError(std::format("{} uses its own shader, which mods cannot carry yet", Record.SourcePath));
			}

			const auto Bytes = SerializeCookedAsset(*Asset);
			if (!Bytes)
			{
				return std::unexpected(Bytes.error());
			}

			const std::filesystem::path Staged = GetStagedAssetPath(StagedContent, Record.Id, Spaces[Space]);
			if (auto Written = WriteFileAtomically(Staged, *Bytes); !Written)
			{
				return std::unexpected(Written.error());
			}

			const std::u8string Relative = Staged.lexically_relative(FilesRoot).generic_u8string();
			Result.Files.emplace_back(Relative.begin(), Relative.end());
			Result.Bytes += Bytes->size();
			++Result.Assets;
		}
	}

	if (auto Written = WriteFileAtomically(Request.OutputRoot / DataModManifestName, *ManifestBytes); !Written)
	{
		return std::unexpected(Written.error());
	}

	std::ranges::sort(Result.Files);
	return Result;
}
}
