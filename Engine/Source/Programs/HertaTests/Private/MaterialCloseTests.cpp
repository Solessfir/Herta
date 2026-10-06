#include "MaterialPanel.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Material close requests preserve drafts until save discard or cancel is resolved")
{
	FMaterialPanel Panel;
	const FAssetId Asset{42, 8};
	const FMaterialAsset Original;
	Panel.Open(Asset, Original, "Game/Test.hmat");
	FMaterialAsset Changed = Original;
	Changed.Parameters.Roughness = 0.2f;
	REQUIRE(Panel.SetDraft(Changed));
	CHECK_FALSE(Panel.RequestClose());
	CHECK_FALSE(Panel.TakeCloseResult().has_value());
	CHECK(Panel.IsOpen());
	REQUIRE(Panel.ResolvePendingChanges(EMaterialCloseChoice::Cancel, {}));
	CHECK(Panel.TakeCloseResult() == false);
	CHECK_FALSE(Panel.TakeCloseResult().has_value());
	CHECK(Panel.IsOpen());
	CHECK(Panel.GetDraft() == Changed);
	CHECK_FALSE(Panel.RequestClose());
	const FMaterialPanelContext FailedSave{
	    .Save = [](FAssetId, const FMaterialAsset&) -> std::expected<void, FAssetError>
	{
		return std::unexpected(FAssetError{"Save failed"});
	},
	};

	CHECK_FALSE(Panel.ResolvePendingChanges(EMaterialCloseChoice::Save, FailedSave));
	CHECK_FALSE(Panel.TakeCloseResult().has_value());
	CHECK(Panel.IsOpen());
	CHECK(Panel.IsDirty());
	FMaterialAsset Restored;
	const FMaterialPanelContext Discard{
	    .Preview = [&](const FAssetId Id, const FMaterialAsset& Material)
	{
		CHECK(Id == Asset);
		Restored = Material;
	},
	};

	REQUIRE(Panel.ResolvePendingChanges(EMaterialCloseChoice::Discard, Discard));
	CHECK(Panel.TakeCloseResult() == true);
	CHECK_FALSE(Panel.IsOpen());
	CHECK_FALSE(Panel.IsDirty());
	CHECK(Restored == Original);
	Panel.Open(Asset, Original, "Game/Test.hmat");
	REQUIRE(Panel.SetDraft(Changed));
	CHECK_FALSE(Panel.RequestClose());
	const FMaterialPanelContext Save{
	    .Save = [&](const FAssetId Id, const FMaterialAsset& Material) -> std::expected<void, FAssetError>
	{
		CHECK(Id == Asset);
		CHECK(Material == Changed);
		return {};
	},
	};

	REQUIRE(Panel.ResolvePendingChanges(EMaterialCloseChoice::Save, Save));
	CHECK(Panel.TakeCloseResult() == true);
	CHECK_FALSE(Panel.IsOpen());
	CHECK_FALSE(Panel.IsDirty());
	CHECK(Panel.GetDraft() == Changed);
}
}
