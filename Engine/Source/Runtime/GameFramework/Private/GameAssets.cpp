#include "Herta/GameFramework/GameAssets.h"

#include "Herta/Level/LevelSerialization.h"
#include "Herta/Navigation/NavigationData.h"
#include "Herta/Navigation/NavigationWorld.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace Herta
{
std::expected<FDecodedGameAssets, FAssetError> DecodeGameAssets(const FCookedContent& Content, const std::span<const FLevelEntity> Entities)
{
	FDecodedGameAssets Decoded;
	const auto DecodeMaterial = [&](const FAssetId& Asset) -> std::expected<void, FAssetError>
	{
		if (!Asset.IsValid() || Decoded.Materials.contains(Asset))
		{
			return {};
		}

		auto Cooked = Content.Load(Asset);
		if (!Cooked || !std::holds_alternative<FMaterialAsset>(*Cooked))
		{
			return std::unexpected(Cooked ? FAssetError{std::format("Asset {} is not a material", Asset.ToString())} : Cooked.error());
		}

		FDecodedGameAssets::FMaterial Material{.Material = std::move(std::get<FMaterialAsset>(*Cooked))};
		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			const FMaterialTextureBinding& Binding = Material.Material.Textures[Slot];
			if (!Binding.Texture.IsValid())
			{
				continue;
			}

			auto Texture = Content.Load(Binding.Texture, Binding.ColorSpace);
			if (!Texture)
			{
				return std::unexpected(Texture.error());
			}

			Material.Textures[Slot] = std::move(std::get<FCookedTexture>(*Texture));
		}

		Decoded.Materials.emplace(Asset, std::move(Material));
		return {};
	};

	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.Mesh && !Decoded.Models.contains(Entity.Mesh->Asset))
		{
			auto Cooked = Content.Load(Entity.Mesh->Asset);
			if (!Cooked || !std::holds_alternative<FCookedModel>(*Cooked))
			{
				return std::unexpected(Cooked ? FAssetError{std::format("Asset {} on {} is not a model", Entity.Mesh->Asset.ToString(), Entity.Name)} : Cooked.error());
			}

			FCookedModel& Model = std::get<FCookedModel>(*Cooked);
			const bool bSkinned = Entity.Character && Model.Skeleton;
			Decoded.Models.emplace(Entity.Mesh->Asset, FDecodedGameAssets::FModel{.Model = std::move(Model), .Label = Entity.Name, .bSkinned = bSkinned});
		}

		for (const FAssetId& Material : Entity.Mesh ? std::span<const FAssetId>(Entity.Mesh->Materials) : std::span<const FAssetId>{})
		{
			if (auto Loaded = DecodeMaterial(Material); !Loaded)
			{
				return std::unexpected(Loaded.error());
			}
		}

		if (Entity.SoftBody)
		{
			if (auto Loaded = DecodeMaterial(Entity.SoftBody->Material); !Loaded)
			{
				return std::unexpected(Loaded.error());
			}
		}

		if (Entity.Light && Entity.Light->Type == ELightType::Sky && Entity.Light->Environment.IsValid() && !Decoded.Environments.contains(Entity.Light->Environment))
		{
			auto Texture = Content.Load(Entity.Light->Environment, ETextureColorSpace::Linear);
			if (!Texture)
			{
				return std::unexpected(Texture.error());
			}

			Decoded.Environments.emplace(Entity.Light->Environment, std::move(std::get<FCookedTexture>(*Texture)));
		}
	}

	return Decoded;
}

std::expected<std::unique_ptr<FStagedGameAssets>, FAssetError> FStagedGameAssets::Create(IGraphicsDevice* const Device, const FCookedContent& Content, const std::span<const FLevelEntity> Entities)
{
	auto Decoded = DecodeGameAssets(Content, Entities);
	if (!Decoded)
	{
		return std::unexpected(Decoded.error());
	}

	std::unique_ptr<FStagedGameAssets> Assets(new FStagedGameAssets);
	auto Added = Assets->Add(Device, std::move(*Decoded));
	if (!Added)
	{
		return std::unexpected(Added.error());
	}

	Assets->References.clear();
	Assets->Permanent.insert(Added->begin(), Added->end());
	return Assets;
}

std::expected<std::vector<FAssetId>, FAssetError> FStagedGameAssets::Add(IGraphicsDevice* const Device, FDecodedGameAssets&& Decoded)
{
	std::vector<FAssetId> Referenced;
	for (auto& [Asset, Model] : Decoded.Models)
	{
		if (!Meshes.contains(Asset))
		{
			auto Mesh = Device ? FRenderMesh::Create(*Device, Model.Model, Model.Label) : FRenderMesh::CreateBoundsOnly(Model.Model);
			if (!Mesh)
			{
				return std::unexpected(FAssetError{std::format("Cannot upload the mesh of {}: {}", Model.Label, Mesh.error().Message)});
			}

			Meshes.emplace(Asset, std::move(*Mesh));
			if (Model.bSkinned)
			{
				SkinnedModels.emplace(Asset, std::make_shared<const FCookedModel>(std::move(Model.Model)));
			}
		}

		Referenced.push_back(Asset);
	}

	// A headless session draws nothing, so it keeps no materials.
	if (Device)
	{
		for (auto& [Asset, Material] : Decoded.Materials)
		{
			if (!Materials.contains(Asset))
			{
				auto Created = FRenderMaterial::Create(*Device, Material.Material, Material.Textures, Material.Material.Name);
				if (!Created)
				{
					return std::unexpected(FAssetError{std::format("Cannot upload material {}: {}", Material.Material.Name, Created.error().Message)});
				}

				Materials.emplace(Asset, std::move(*Created));
			}

			Referenced.push_back(Asset);
		}
	}

	for (auto& [Asset, Texture] : Decoded.Environments)
	{
		Environments.try_emplace(Asset, std::make_shared<const FCookedTexture>(std::move(Texture)));
		Referenced.push_back(Asset);
	}

	for (const FAssetId& Asset : Referenced)
	{
		++References[Asset];
	}

	return Referenced;
}

void FStagedGameAssets::Release(const std::span<const FAssetId> Assets)
{
	for (const FAssetId& Asset : Assets)
	{
		const auto Found = References.find(Asset);
		if (Found == References.end() || --Found->second > 0)
		{
			continue;
		}

		References.erase(Found);
		if (!Permanent.contains(Asset))
		{
			Meshes.erase(Asset);
			SkinnedModels.erase(Asset);
			Materials.erase(Asset);
			Environments.erase(Asset);
		}
	}
}

std::size_t FStagedGameAssets::GetLoadedAssetCount() const noexcept
{
	return Meshes.size() + Materials.size() + Environments.size();
}

const FRenderMesh* FStagedGameAssets::FindMesh(const FAssetId& Asset) const
{
	const auto Found = Meshes.find(Asset);
	return Found != Meshes.end() ? Found->second.get() : nullptr;
}

std::shared_ptr<const FCookedModel> FStagedGameAssets::FindSkinnedModel(const FAssetId& Asset) const
{
	const auto Found = SkinnedModels.find(Asset);
	return Found != SkinnedModels.end() ? Found->second : nullptr;
}

const FRenderMaterial* FStagedGameAssets::FindMaterial(const FAssetId& Asset) const
{
	const auto Found = Materials.find(Asset);
	return Found != Materials.end() ? Found->second.get() : nullptr;
}

std::set<std::string> FStagedGameAssets::GetMaterialShaders() const
{
	std::set<std::string> Result;
	for (const auto& [Asset, Material] : Materials)
	{
		if (!Material->GetShaderKey().empty())
		{
			Result.emplace(Material->GetShaderKey());
		}
	}

	return Result;
}

std::shared_ptr<const FCookedTexture> FStagedGameAssets::FindEnvironment(const FAssetId& Asset) const
{
	const auto Found = Environments.find(Asset);
	return Found != Environments.end() ? Found->second : nullptr;
}

// ponytail: a region uploads in one frame; spread uploads over frames if a large region hitches.
FRegionLoader MakeStagedRegionLoader(std::filesystem::path LevelPath, const FCookedContent& Content, FStagedGameAssets& Assets, IGraphicsDevice* const Device)
{
	return [LevelPath = std::move(LevelPath), &Content, &Assets, Device](const FLevelRegion& Region, const std::atomic<bool>& Cancel) -> std::expected<FLoadedRegion, FGameError>
	{
		const auto Bytes = Content.ReadFile(ResolveLevelRegionPath(LevelPath, Region));
		auto Document = Bytes ? ParseLevel(std::string_view(reinterpret_cast<const char*>(Bytes->data()), Bytes->size())) : std::unexpected(FLevelError{Bytes.error().Message});
		if (!Document)
		{
			return std::unexpected(FGameError{std::format("Cannot load the level of region '{}': {}", Region.Name, Document.error().Message)});
		}

		if (Cancel)
		{
			return std::unexpected(FGameError{"Cancelled"});
		}

		// Only the scenery a region streams needs its assets.
		std::vector<FLevelEntity> Scenery;
		std::ranges::copy_if(Document->Entities, std::back_inserter(Scenery), &IsLevelRegionScenery);
		auto Decoded = DecodeGameAssets(Content, Scenery);
		if (!Decoded)
		{
			return std::unexpected(FGameError{std::format("Cannot load the assets of region '{}': {}", Region.Name, Decoded.error().Message)});
		}

		auto Pending = std::make_shared<FDecodedGameAssets>(std::move(*Decoded));
		auto Referenced = std::make_shared<std::vector<FAssetId>>();
		return FLoadedRegion{
		    .Document = std::move(*Document),
		    .Prepare = [Pending, Referenced, &Assets, Device]() -> std::expected<bool, FGameError>
		{
			auto Added = Assets.Add(Device, std::move(*Pending));
			if (!Added)
			{
				return std::unexpected(FGameError{std::move(Added.error().Message)});
			}

			*Referenced = std::move(*Added);
			return true;
		},
		    .Release = [Referenced, &Assets]
		{
			Assets.Release(*Referenced);
		},
		};
	};
}

std::expected<FStagedSession, FGameError> CreateStagedSession(const std::filesystem::path& LevelPath, const FLevelDocument& Level, const FCookedContent& Content, IGraphicsDevice* const Device, FTaskSystem* const Tasks)
{
	FStagedSession Staged;
	auto Assets = FStagedGameAssets::Create(Device, Content, Level.Entities);
	if (!Assets)
	{
		return std::unexpected(FGameError{std::format("Cannot load game content: {}", Assets.error().Message)});
	}

	Staged.Assets = std::move(*Assets);

	// The level's character is optional: without one, or if it fails to load, the player is the bare capsule.
	std::shared_ptr<const FCookedModel> Character;
	if (Level.PlayerCharacter.IsValid())
	{
		auto Cooked = Content.Load(Level.PlayerCharacter);
		if (Cooked && std::holds_alternative<FCookedModel>(*Cooked))
		{
			Character = std::make_shared<const FCookedModel>(std::move(std::get<FCookedModel>(*Cooked)));
		}
		else
		{
			Staged.Warnings.push_back(std::format("Cannot load the player character: {}", Cooked ? "the asset is not a model" : Cooked.error().Message));
		}
	}

	auto Session = FGameSession::Create(Level.Entities, *Staged.Assets, Device, {}, Tasks, std::move(Character));
	if (!Session)
	{
		return std::unexpected(Session.error());
	}

	Staged.Session = std::move(*Session);
	FGameSession& Game = *Staged.Session;
	Staged.Warnings.insert(Staged.Warnings.end(), Game.GetWarnings().begin(), Game.GetWarnings().end());
	Game.SetLevelEffects(Level.Effects);
	for (const FAssetId& System : Game.GetParticleSystems())
	{
		auto Cooked = Content.Load(System);
		auto* const Particles = Cooked ? std::get_if<FCookedParticleSystem>(&*Cooked) : nullptr;
		const auto Added = Particles ? Game.AddParticleSystem(System, std::make_shared<const FCookedParticleSystem>(std::move(*Particles))) : std::unexpected(FGameError{Cooked ? "the asset is not a particle system" : Cooked.error().Message});
		if (!Added)
		{
			Staged.Warnings.push_back(std::format("Particle system {} will not play: {}", System.ToString(), Added.error().Message));
		}
	}

	Game.SetLevelSounds(Level.Sounds);
	Game.SetRegions(Level.Regions, MakeStagedRegionLoader(LevelPath, Content, *Staged.Assets, Device));

	// Staging cooks the level's navigation beside it; a level without Navigation Bounds has none.
	std::filesystem::path NavigationPath = LevelPath;
	NavigationPath.replace_extension(NavigationDataExtension);
	if (Content.Exists(NavigationPath))
	{
		const auto Bytes = Content.ReadFile(NavigationPath);
		auto Navigation = Bytes ? ParseNavigationFile(*Bytes) : std::unexpected(FNavigationError{Bytes.error().Message});
		const auto Loaded = Navigation ? Game.SetNavigation(std::make_shared<const FNavigationFile>(std::move(*Navigation))) : std::unexpected(FGameError{Navigation.error().Message});
		if (!Loaded)
		{
			Staged.Warnings.push_back(std::format("Agents have no navigation: {}", Loaded.error().Message));
		}
	}

	// Agents start once their StateTree arrives; one that fails to load leaves its character standing.
	for (const FAssetId& TreeId : Game.GetStateTrees())
	{
		auto Cooked = Content.Load(TreeId);
		auto* const Tree = Cooked ? std::get_if<FCookedStateTree>(&*Cooked) : nullptr;
		const auto Added = Tree ? Game.AddStateTree(TreeId, std::make_shared<const FCookedStateTree>(std::move(*Tree))) : std::unexpected(FGameError{Cooked ? "the asset is not a StateTree" : Cooked.error().Message});
		if (!Added)
		{
			Staged.Warnings.push_back(std::format("StateTree {} will not run: {}", TreeId.ToString(), Added.error().Message));
		}
	}

	for (const FAssetId& GraphId : Game.GetGameplayGraphs())
	{
		auto Cooked = Content.Load(GraphId);
		if (auto* const Graph = Cooked ? std::get_if<FCookedGameplayGraph>(&*Cooked) : nullptr)
		{
			Game.AddGameplayGraph(GraphId, std::make_shared<const FCookedGameplayGraph>(std::move(*Graph)));
		}
		else
		{
			Staged.Warnings.push_back(std::format("Gameplay graph {} will not run: {}", GraphId.ToString(), Cooked ? "the asset is not a gameplay graph" : Cooked.error().Message));
		}
	}

	return Staged;
}
}
