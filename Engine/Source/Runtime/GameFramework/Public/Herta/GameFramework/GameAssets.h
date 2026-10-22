#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Level/World.h"

#include <expected>
#include <map>
#include <memory>
#include <span>

namespace Herta
{
class IGraphicsDevice;
class FRenderMaterial;
class FRenderMesh;

// GPU resources a game session draws with. The standalone game loads them from staged content; the editor shares the assets it already has loaded.
class IGameAssets
{
public:
	IGameAssets() = default;
	virtual ~IGameAssets() = default;
	IGameAssets(const IGameAssets&) = delete;
	IGameAssets& operator=(const IGameAssets&) = delete;
	IGameAssets(IGameAssets&&) = delete;
	IGameAssets& operator=(IGameAssets&&) = delete;

	// Null when the asset is missing or still loading; the entity is then skipped.
	[[nodiscard]] virtual const FRenderMesh* FindMesh(const FAssetId& Asset) const = 0;
	// Null keeps the material imported with the mesh.
	[[nodiscard]] virtual const FRenderMaterial* FindMaterial(const FAssetId& Asset) const = 0;
	[[nodiscard]] virtual std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId& Asset) const = 0;
};

// Loads everything a level references from staged content up front, so play never waits on a load.
class FStagedGameAssets final : public IGameAssets
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FStagedGameAssets>, FAssetError> Create(IGraphicsDevice& Device, const FCookedContent& Content, std::span<const FLevelEntity> Entities);

	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId& Asset) const override;
	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId& Asset) const override;
	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId& Asset) const override;

private:
	FStagedGameAssets() = default;

	std::map<FAssetId, std::shared_ptr<const FRenderMesh>> Meshes;
	std::map<FAssetId, std::shared_ptr<const FRenderMaterial>> Materials;
	std::map<FAssetId, std::shared_ptr<const FCookedTexture>> Environments;
};
}
