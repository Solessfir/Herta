#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Level/World.h"

#include <array>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

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
	// A skinned model's cooked data, which characters animate and slice on the CPU. Null for models that are not skinned or not loaded.
	[[nodiscard]] virtual std::shared_ptr<const FCookedModel> FindSkinnedModel(const FAssetId&) const
	{
		return nullptr;
	}
};

// Cooked data for entities, read and decoded on any thread, ready to upload on the game thread.
struct FDecodedGameAssets
{
	struct FMaterial
	{
		FMaterialAsset Material{};
		std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount> Textures{};
	};

	struct FModel
	{
		FCookedModel Model{};
		std::string Label{};
		bool bSkinned = false;
	};

	std::map<FAssetId, FModel> Models{};
	std::map<FAssetId, FMaterial> Materials{};
	std::map<FAssetId, FCookedTexture> Environments{};
};

[[nodiscard]] std::expected<FDecodedGameAssets, FAssetError> DecodeGameAssets(const FCookedContent& Content, std::span<const FLevelEntity> Entities);

// Loads everything a level references from staged content up front, so play never waits on a load. Streamed regions add theirs later and
// release them when they unload; the starting level's assets stay for the session.
class FStagedGameAssets final : public IGameAssets
{
public:
	// Without a device, as on a dedicated server, meshes keep only their bounds for collision and materials are skipped.
	[[nodiscard]] static std::expected<std::unique_ptr<FStagedGameAssets>, FAssetError> Create(IGraphicsDevice* Device, const FCookedContent& Content, std::span<const FLevelEntity> Entities);

	// Uploads what is not loaded yet and counts a reference to every asset in Decoded, uploaded now or before.
	[[nodiscard]] std::expected<std::vector<FAssetId>, FAssetError> Add(IGraphicsDevice* Device, FDecodedGameAssets&& Decoded);
	// Drops the references Add returned; an asset no one references any longer, and the starting level does not use, is freed.
	void Release(std::span<const FAssetId> Assets);
	[[nodiscard]] std::size_t GetLoadedAssetCount() const noexcept;

	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId& Asset) const override;
	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId& Asset) const override;
	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId& Asset) const override;
	[[nodiscard]] std::shared_ptr<const FCookedModel> FindSkinnedModel(const FAssetId& Asset) const override;
	// Mounted paths of the custom shaders the loaded materials draw with.
	std::set<std::string> GetMaterialShaders() const;

private:
	FStagedGameAssets() = default;

	std::map<FAssetId, std::shared_ptr<const FRenderMesh>> Meshes;
	std::map<FAssetId, std::shared_ptr<const FRenderMaterial>> Materials;
	std::map<FAssetId, std::shared_ptr<const FCookedTexture>> Environments;
	std::map<FAssetId, std::shared_ptr<const FCookedModel>> SkinnedModels;
	// Region references per asset. Assets loaded by Create have none and are never freed.
	std::map<FAssetId, std::uint32_t> References;
	std::set<FAssetId> Permanent;
};

// A staged level ready to play. The session reads the assets, so they must outlive it, and both read Content.
struct FStagedSession
{
	std::unique_ptr<FStagedGameAssets> Assets{};
	std::unique_ptr<FGameSession> Session{};
	// Things that will not work, such as a character or a StateTree that failed to load, which still leave the level playable.
	std::vector<std::string> Warnings{};
};

// Loads a staged level's assets, player character, particle systems, sounds, regions, navigation, and StateTrees into a session, the same way
// for a player's game, a client, and a headless server. Device may be null for a server, which draws nothing.
[[nodiscard]] std::expected<FStagedSession, FGameError> CreateStagedSession(const std::filesystem::path& LevelPath, const FLevelDocument& Level, const FCookedContent& Content, IGraphicsDevice* Device, FTaskSystem* Tasks);

// Loads regions placed by the level at LevelPath from staged content: a worker reads the region's level and decodes its assets, and the game
// thread uploads them into Assets before the region activates. Content, Assets, and Device, which may be null, must outlive the session.
[[nodiscard]] FRegionLoader MakeStagedRegionLoader(std::filesystem::path LevelPath, const FCookedContent& Content, FStagedGameAssets& Assets, IGraphicsDevice* Device);
}
