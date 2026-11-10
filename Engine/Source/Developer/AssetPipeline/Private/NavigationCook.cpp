#include "Herta/AssetPipeline/NavigationCook.h"

#include "Herta/AssetPipeline/ContentRoot.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace Herta
{
std::filesystem::path GetLevelNavigationPath(const std::filesystem::path& LevelPath)
{
	std::filesystem::path Path = LevelPath;
	Path.replace_extension(NavigationDataExtension);
	return Path;
}

FNavigationBox GetCookedModelBounds(const FCookedModel& Model)
{
	if (Model.Vertices.empty())
	{
		return {};
	}

	FNavigationBox Box{.Minimum = {1.e30f, 1.e30f, 1.e30f}, .Maximum = {-1.e30f, -1.e30f, -1.e30f}};
	for (const FCookedVertex& Vertex : Model.Vertices)
	{
		const FVector3 Position{Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]};
		Box.Minimum = {std::min(Box.Minimum.X, Position.X), std::min(Box.Minimum.Y, Position.Y), std::min(Box.Minimum.Z, Position.Z)};
		Box.Maximum = {std::max(Box.Maximum.X, Position.X), std::max(Box.Maximum.Y, Position.Y), std::max(Box.Maximum.Z, Position.Z)};
	}

	return Box;
}

std::vector<std::optional<FLevelDocument>> LoadLevelRegions(const FLevelDocument& Level, const std::filesystem::path& LevelPath)
{
	std::vector<std::optional<FLevelDocument>> Regions;
	std::map<std::filesystem::path, std::optional<FLevelDocument>> Loaded;
	for (const FLevelRegion& Region : Level.Regions)
	{
		const std::filesystem::path Path = ResolveLevelRegionPath(LevelPath, Region);
		auto [Found, bInserted] = Loaded.try_emplace(Path);
		if (bInserted)
		{
			auto Document = LoadLevel(Path);
			Found->second = Document ? std::optional{std::move(*Document)} : std::nullopt;
		}

		Regions.push_back(Found->second);
	}

	return Regions;
}

std::expected<std::optional<FNavigationFile>, FAssetError> CookLevelNavigation(const FLevelDocument& Level, const FNavigationCookRequest& Request)
{
	const std::vector<std::optional<FLevelDocument>> Regions = LoadLevelRegions(Level, Request.LevelPath);
	std::set<FAssetId> Meshes;
	const auto Gather = [&Meshes](const FLevelEntity& Entity)
	{
		if (Entity.Mesh && Entity.BodyType == ELevelBodyType::Static)
		{
			Meshes.insert(Entity.Mesh->Asset);
		}
	};

	std::ranges::for_each(Level.Entities, Gather);
	for (const std::optional<FLevelDocument>& Region : Regions)
	{
		if (Region)
		{
			std::ranges::for_each(Region->Entities, Gather);
		}
	}

	// Only the meshes static bodies use are cooked, and only for their bounds.
	std::map<FAssetId, std::pair<std::filesystem::path, std::string>> Sources;
	for (const std::filesystem::path& Root : {Request.EngineContentRoot, Request.GameContentRoot})
	{
		const auto Scan = ScanContentRoot(Root);
		if (!Scan)
		{
			return std::unexpected(Scan.error());
		}

		for (const FAssetRecord& Record : Scan->Registry.GetRecords())
		{
			if (Meshes.contains(Record.Id))
			{
				Sources.insert_or_assign(Record.Id, std::pair{Root, Record.SourcePath});
			}
		}
	}

	std::map<FAssetId, FNavigationBox> Bounds;
	for (const FAssetId& Mesh : Meshes)
	{
		const auto Source = Sources.find(Mesh);
		if (Source == Sources.end())
		{
			return std::unexpected(FAssetError{std::format("Mesh {} of a static body is not registered in Engine or project content", Mesh.ToString())});
		}

		const auto Cooked = CookAssetInWorker({
		                                          .ContentRoot = Source->second.first,
		                                          .DerivedDataRoot = Request.DerivedDataRoot,
		                                          .SourcePath = Source->second.second,
		                                          .TargetPlatform = Request.TargetPlatform,
		                                          .DependencyContentRoots = {Request.EngineContentRoot, Request.GameContentRoot},
		                                          .EngineContentRoot = Request.EngineContentRoot,
		                                          .GameContentRoot = Request.GameContentRoot,
		                                      },
		    {.WorkerPath = Request.WorkerPath});
		if (!Cooked)
		{
			return std::unexpected(Cooked.error());
		}

		const auto Asset = LoadCookedAsset(Request.DerivedDataRoot, Cooked->Key);
		if (!Asset)
		{
			return std::unexpected(Asset.error());
		}

		if (const auto* const Model = std::get_if<FCookedModel>(&*Asset))
		{
			Bounds.emplace(Mesh, GetCookedModelBounds(*Model));
		}
	}

	auto Built = BuildLevelNavigation(Level, Regions, [&Bounds](const FAssetId& Mesh) -> std::optional<FNavigationBox>
	{
		const auto Found = Bounds.find(Mesh);
		return Found != Bounds.end() ? std::optional{Found->second} : std::nullopt;
	}, {}, Request.Tasks);

	if (!Built)
	{
		return std::unexpected(FAssetError{Built.error().Message});
	}

	return std::move(*Built);
}
}
