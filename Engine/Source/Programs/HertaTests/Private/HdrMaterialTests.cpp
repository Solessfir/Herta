#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/TextureCooker.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <cstring>

namespace Herta
{
namespace
{
std::string EncodedHdr()
{
	std::string Source = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
	const std::array<char, 4> Pixel{static_cast<char>(128), 64, 32, static_cast<char>(130)};
	Source.append(Pixel.data(), Pixel.size());
	return Source;
}
}

TEST_CASE("HDR texture and material cooking rejects sRGB interpretation without losing radiance")
{
	const auto Encoded = EncodedHdr();
	const auto Bytes = std::as_bytes(std::span(Encoded.data(), Encoded.size()));
	CHECK(IsEncodedHdrTexture(Bytes));
	CHECK_FALSE(CookEncodedTexture(Bytes, ETextureColorSpace::Srgb));
	const auto Linear = CookEncodedTexture(Bytes, ETextureColorSpace::Linear);
	REQUIRE(Linear);
	std::array<float, 4> Pixel{};
	std::memcpy(Pixel.data(), Linear->Mips[0].Pixels.data(), sizeof(Pixel));
	CHECK(Pixel[0] == doctest::Approx(2.f));
	const Tests::FScratchDirectory Scratch("HertaHdrMaterialTests");
	const auto Root = Scratch.GetPath() / "Content";
	Tests::WriteText(Root / "Radiance.hdr", Encoded);
	Tests::WriteText(Root / "Radiance.hdr.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000061\nImporter = Texture\n");
	FAssetCookRequest Request{.ContentRoot = Root, .DerivedDataRoot = Scratch.GetPath() / "Cache", .SourcePath = "Radiance.hdr", .TargetPlatform = "TestPlatform"};
	REQUIRE(CookAsset(Request));
	Request.TextureColorSpace = ETextureColorSpace::Srgb;
	CHECK_FALSE(CookAsset(Request));
	Request.TextureColorSpace.reset();
	Request.SourcePath = "Surface.hmat";
	FMaterialAsset Material;
	Material.Textures[0].Texture = *FAssetId::Parse("00000000-0000-4000-8000-000000000061");
	REQUIRE(WriteMaterialAsset(Root / "Surface.hmat", Material));
	Tests::WriteText(Root / "Surface.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000062\nImporter = Material\n");
	CHECK_FALSE(CookAsset(Request));
	Material.Textures[0].ColorSpace = ETextureColorSpace::Linear;
	REQUIRE(WriteMaterialAsset(Root / "Surface.hmat", Material));
	REQUIRE(CookAsset(Request));
	Material.Textures[5].Texture = Material.Textures[0].Texture;
	REQUIRE(WriteMaterialAsset(Root / "Surface.hmat", Material));
	CHECK_FALSE(CookAsset(Request));
}

TEST_CASE("Renderer rejects sRGB HDR material bindings at its runtime boundary")
{
	Tests::FTestGraphicsDevice Device;
	FMaterialAsset Material;
	Material.Textures[0].Texture = FAssetId{61, 1};
	std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount> Textures;
	Textures[0] = *CookHdrTexture(1, 1, std::array<float, 4>{2.f, 1.f, 0.5f, 1.f});
	const auto Rejected = FRenderMaterial::Create(Device, Material, Textures, "HDR material");
	REQUIRE_FALSE(Rejected);
	CHECK(Rejected.error().Message == "HDR textures require Linear color space");
	CHECK(Device.TextureDescriptors.empty());
	Material.Textures[0].ColorSpace = ETextureColorSpace::Linear;
	REQUIRE(FRenderMaterial::Create(Device, Material, Textures, "HDR material"));
	CHECK(Device.TextureDescriptors[0].Format == ETextureFormat::Rgba32Float);
}
}
