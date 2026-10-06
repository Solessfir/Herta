#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>

namespace Herta
{
TEST_CASE("Material worker cooking resolves explicit Game and Engine shader mounts")
{
	const Tests::FScratchDirectory Scratch("HertaMountedMaterialTests");
	const auto Engine = Scratch.GetPath() / "EngineContent";
	const auto Game = Scratch.GetPath() / "ProjectContent";
	const auto Includes = Scratch.GetPath() / "ShaderIncludes";
	Tests::WriteText(Engine / "Shaders/Surface.slang", "#include \"Shared.slangh\"\n");
	Tests::WriteText(Game / "Shaders/Surface.slang", "#include \"Shared.slangh\"\n");
	Tests::WriteText(Includes / "Shared.slangh", "float sharedValue = 1;\n");
	FMaterialAsset Material;
	Material.ShaderPath = "Engine/Shaders/Surface.slang";
	REQUIRE(WriteMaterialAsset(Game / "Surface.hmat", Material));
	Tests::WriteText(Game / "Surface.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000051\nImporter = Material\n");
	FAssetCookRequest Request{
	    .ContentRoot = Game,
	    .DerivedDataRoot = Scratch.GetPath() / "Cache",
	    .SourcePath = "Surface.hmat",
	    .TargetPlatform = "TestPlatform",
	    .DependencyContentRoots = {Engine, Includes},
	};

	CHECK_FALSE(CookAsset(Request));
	Request.EngineContentRoot = Engine;
	Request.GameContentRoot = Game;
	const auto First = CookAssetInWorker(Request, {.WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker")});
	REQUIRE_MESSAGE(First.has_value(), (First ? "" : First.error().Message));
	const auto Cached = CookAsset(Request);
	REQUIRE(Cached);
	CHECK(Cached->bCacheHit);
	CHECK(Cached->Key == First->Key);
	Tests::WriteText(Includes / "Shared.slangh", "float sharedValue = 2;\n");
	const auto Changed = CookAsset(Request);
	REQUIRE(Changed);
	CHECK(Changed->Key != First->Key);
	Material.ShaderPath = "Game/Shaders/Surface.slang";
	REQUIRE(WriteMaterialAsset(Engine / "Surface.hmat", Material));
	Tests::WriteText(Engine / "Surface.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000052\nImporter = Material\n");
	Request.ContentRoot = Engine;
	Request.DependencyContentRoots = {Game, Includes};
	const auto FromEngine = CookAssetInWorker(Request, {.WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker")});
	REQUIRE_MESSAGE(FromEngine.has_value(), (FromEngine ? "" : FromEngine.error().Message));
	const auto Loaded = LoadCookedAsset(Request.DerivedDataRoot, FromEngine->Key);
	REQUIRE(Loaded);
	CHECK(std::get<FMaterialAsset>(*Loaded) == Material);
	Request.GameContentRoot.clear();
	CHECK_FALSE(CookAsset(Request));
}

TEST_CASE("Stock material worker cooking does not require optional shader source directories")
{
	const Tests::FScratchDirectory Scratch("HertaPackagedMaterialTests");
	const auto Engine = Scratch.GetPath() / "Engine/Content";
	const auto Game = Scratch.GetPath() / "Game/Content";
	const auto ShaderIncludes = Scratch.GetPath() / "Engine/Shaders";
	constexpr std::array<std::uint8_t, 4> Pixels{255, 128, 64, 255};
	Tests::WritePng(Engine / "Color.png", 1, 1, Pixels);
	Tests::WriteText(Engine / "Color.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000053\nImporter = Texture\n");
	FMaterialAsset Material;
	REQUIRE(WriteMaterialAsset(Game / "Surface.hmat", Material));
	Tests::WriteText(Game / "Surface.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000054\nImporter = Material\n");
	FAssetCookRequest Request{
	    .ContentRoot = Game,
	    .DerivedDataRoot = Scratch.GetPath() / "Cache",
	    .SourcePath = "Surface.hmat",
	    .TargetPlatform = "TestPlatform",
	    .DependencyContentRoots = {Engine, ShaderIncludes},
	    .EngineContentRoot = Engine,
	    .GameContentRoot = Game,
	};
	const FAssetWorkerOptions Worker{.WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker")};
	CHECK_FALSE(std::filesystem::exists(ShaderIncludes));
	const auto Stock = CookAssetInWorker(Request, Worker);
	REQUIRE_MESSAGE(Stock.has_value(), (Stock ? "" : Stock.error().Message));
	Request.DependencyContentRoots = {Engine};
	const auto WithoutOptionalRoot = CookAsset(Request);
	REQUIRE(WithoutOptionalRoot);
	CHECK(WithoutOptionalRoot->bCacheHit);
	CHECK(WithoutOptionalRoot->Key == Stock->Key);

	Material.Textures[static_cast<std::size_t>(EMaterialTextureSlot::BaseColor)].Texture = *FAssetId::Parse("00000000-0000-4000-8000-000000000053");
	REQUIRE(WriteMaterialAsset(Game / "Surface.hmat", Material));
	Request.DependencyContentRoots.push_back(ShaderIncludes);
	const auto Textured = CookAssetInWorker(Request, Worker);
	REQUIRE_MESSAGE(Textured.has_value(), (Textured ? "" : Textured.error().Message));
	const auto Loaded = LoadCookedAsset(Request.DerivedDataRoot, Textured->Key);
	REQUIRE(Loaded);
	CHECK(std::get<FMaterialAsset>(*Loaded) == Material);

	Material.Textures[static_cast<std::size_t>(EMaterialTextureSlot::BaseColor)].Texture = *FAssetId::Parse("00000000-0000-4000-8000-000000000055");
	REQUIRE(WriteMaterialAsset(Game / "Surface.hmat", Material));
	const auto MissingTexture = CookAssetInWorker(Request, Worker);
	REQUIRE_FALSE(MissingTexture);
	CHECK(MissingTexture.error().Message.find("is missing or is not a texture") != std::string::npos);

	Material.Textures[static_cast<std::size_t>(EMaterialTextureSlot::BaseColor)].Texture = {};
	Material.ShaderPath = "Surface.slang";
	Tests::WriteText(Game / "Surface.slang", "#include \"VisualShared.slangh\"\n");
	REQUIRE(WriteMaterialAsset(Game / "Surface.hmat", Material));
	const auto MissingInclude = CookAssetInWorker(Request, Worker);
	REQUIRE_FALSE(MissingInclude);
	CHECK(MissingInclude.error().Message.find("Shader dependency 'VisualShared.slangh' is missing") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(ShaderIncludes));
}
}
