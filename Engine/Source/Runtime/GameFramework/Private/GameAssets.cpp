#include "Herta/GameFramework/GameAssets.h"

#include "Herta/Renderer/MeshRenderer.h"

#include <format>
#include <utility>

namespace Herta
{
std::expected<std::unique_ptr<FStagedGameAssets>, FAssetError> FStagedGameAssets::Create(IGraphicsDevice& Device, const FCookedContent& Content, const std::span<const FLevelEntity> Entities)
{
	std::unique_ptr<FStagedGameAssets> Assets(new FStagedGameAssets);
	const auto LoadMaterial = [&](const FAssetId& Asset) -> std::expected<void, FAssetError>
	{
		if (!Asset.IsValid() || Assets->Materials.contains(Asset))
		{
			return {};
		}

		auto Cooked = Content.Load(Asset);
		if (!Cooked || !std::holds_alternative<FMaterialAsset>(*Cooked))
		{
			return std::unexpected(Cooked ? FAssetError{std::format("Asset {} is not a material", Asset.ToString())} : Cooked.error());
		}

		const FMaterialAsset& Material = std::get<FMaterialAsset>(*Cooked);
		std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount> Textures;
		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			const FMaterialTextureBinding& Binding = Material.Textures[Slot];
			if (!Binding.Texture.IsValid())
			{
				continue;
			}

			auto Texture = Content.Load(Binding.Texture, Binding.ColorSpace);
			if (!Texture)
			{
				return std::unexpected(Texture.error());
			}

			Textures[Slot] = std::move(std::get<FCookedTexture>(*Texture));
		}

		auto Created = FRenderMaterial::Create(Device, Material, Textures, Material.Name);
		if (!Created)
		{
			return std::unexpected(FAssetError{std::format("Cannot upload material {}: {}", Material.Name, Created.error().Message)});
		}

		Assets->Materials.emplace(Asset, std::move(*Created));
		return {};
	};

	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.Mesh && !Assets->Meshes.contains(Entity.Mesh->Asset))
		{
			auto Cooked = Content.Load(Entity.Mesh->Asset);
			if (!Cooked || !std::holds_alternative<FCookedModel>(*Cooked))
			{
				return std::unexpected(Cooked ? FAssetError{std::format("Asset {} on {} is not a model", Entity.Mesh->Asset.ToString(), Entity.Name)} : Cooked.error());
			}

			auto Mesh = FRenderMesh::Create(Device, std::get<FCookedModel>(*Cooked), Entity.Name);
			if (!Mesh)
			{
				return std::unexpected(FAssetError{std::format("Cannot upload the mesh of {}: {}", Entity.Name, Mesh.error().Message)});
			}

			Assets->Meshes.emplace(Entity.Mesh->Asset, std::move(*Mesh));
		}

		for (const FAssetId& Material : Entity.Mesh ? std::span<const FAssetId>(Entity.Mesh->Materials) : std::span<const FAssetId>{})
		{
			if (auto Loaded = LoadMaterial(Material); !Loaded)
			{
				return std::unexpected(Loaded.error());
			}
		}

		if (Entity.SoftBody)
		{
			if (auto Loaded = LoadMaterial(Entity.SoftBody->Material); !Loaded)
			{
				return std::unexpected(Loaded.error());
			}
		}

		if (Entity.Light && Entity.Light->Type == ELightType::Sky && Entity.Light->Environment.IsValid() && !Assets->Environments.contains(Entity.Light->Environment))
		{
			auto Texture = Content.Load(Entity.Light->Environment, ETextureColorSpace::Linear);
			if (!Texture)
			{
				return std::unexpected(Texture.error());
			}

			Assets->Environments.emplace(Entity.Light->Environment, std::make_shared<const FCookedTexture>(std::move(std::get<FCookedTexture>(*Texture))));
		}
	}

	return Assets;
}

const FRenderMesh* FStagedGameAssets::FindMesh(const FAssetId& Asset) const
{
	const auto Found = Meshes.find(Asset);
	return Found != Meshes.end() ? Found->second.get() : nullptr;
}

const FRenderMaterial* FStagedGameAssets::FindMaterial(const FAssetId& Asset) const
{
	const auto Found = Materials.find(Asset);
	return Found != Materials.end() ? Found->second.get() : nullptr;
}

std::shared_ptr<const FCookedTexture> FStagedGameAssets::FindEnvironment(const FAssetId& Asset) const
{
	const auto Found = Environments.find(Asset);
	return Found != Environments.end() ? Found->second : nullptr;
}
}
