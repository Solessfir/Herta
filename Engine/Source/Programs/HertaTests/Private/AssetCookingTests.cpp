#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/TextureCooker.h"
#include "Herta/Core/BinaryStream.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>

namespace Herta
{
namespace
{
constexpr std::string_view QuadGltf = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [
    {"mesh": 0, "translation": [1, 2, 3]},
    {"mesh": 0, "scale": [-1, 1, 1], "translation": [-5, 0, 0]}
  ],
  "meshes": [{"name": "Quad", "primitives": [
    {"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0},
    {"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 1},
    {"attributes": {"POSITION": 0}, "mode": 0}
  ]}],
  "materials": [
    {"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
    {"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}}
  ],
  "textures": [{"source": 0}],
  "images": [{"uri": "Checker.png"}],
  "buffers": [{"uri": "Quad.bin", "byteLength": 92}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 48},
    {"buffer": 0, "byteOffset": 48, "byteLength": 32},
    {"buffer": 0, "byteOffset": 80, "byteLength": 12}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}
  ]
})";

constexpr std::array<std::uint8_t, 16> CheckerPixels{255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255};

std::vector<std::byte> MakeQuadBuffer()
{
	FBinaryWriter Writer;

	for (const float Value : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f})
	{
		Writer.WriteFloat(Value);
	}

	for (const float Value : {0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f})
	{
		Writer.WriteFloat(Value);
	}

	for (const std::uint16_t Index : std::initializer_list<std::uint16_t>{0, 1, 2, 0, 2, 3})
	{
		Writer.Write(Index);
	}

	return Writer.TakeBytes();
}

// Writes Models/Quad.gltf with its buffer and image into a content root.
void WriteQuadModel(const std::filesystem::path& Root, const std::string_view Gltf = QuadGltf)
{
	Tests::WriteText(Root / "Models/Quad.gltf", Gltf);
	Tests::WriteBytes(Root / "Models/Quad.bin", MakeQuadBuffer());
	Tests::WritePng(Root / "Models/Checker.png", 2, 2, CheckerPixels);
	Tests::WriteText(Root / "Models/Quad.gltf.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000001\nImporter = Gltf\n");
}

FAssetCookRequest MakeRequest(const Tests::FScratchDirectory& Scratch, const std::string_view SourcePath)
{
	return {.ContentRoot = Scratch.GetPath() / "Content", .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = std::string(SourcePath), .TargetPlatform = "TestPlatform", .bForce = false};
}

std::array<std::uint8_t, 4> Texel(const FCookedTextureMip& Mip, const std::size_t Index = 0)
{
	std::array<std::uint8_t, 4> Result{};

	for (std::size_t Channel = 0; Channel < 4; ++Channel)
	{
		Result[Channel] = static_cast<std::uint8_t>(Mip.Pixels[Index * 4 + Channel]);
	}

	return Result;
}

FCookedModel CookQuad(const Tests::FScratchDirectory& Scratch)
{
	const auto Result = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(Result);

	auto Asset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Result->Key);
	REQUIRE(Asset);
	REQUIRE(std::holds_alternative<FCookedModel>(*Asset));

	return std::get<FCookedModel>(std::move(*Asset));
}
}

TEST_CASE("Binary streams use little-endian fields and fail closed")
{
	FBinaryWriter Writer;
	Writer.Write(std::uint8_t{0x12});
	Writer.Write(std::uint32_t{0x34567890});
	Writer.WriteFloat(-2.5f);
	Writer.WriteString("Herta");
	const std::vector<std::byte> Bytes = Writer.TakeBytes();
	CHECK(Bytes[1] == std::byte{0x90});
	CHECK(Bytes[4] == std::byte{0x34});

	FBinaryReader Reader(Bytes);
	CHECK(Reader.Read<std::uint8_t>() == 0x12);
	CHECK(Reader.Read<std::uint32_t>() == 0x34567890u);
	CHECK(Reader.ReadFloat() == -2.5f);
	CHECK(Reader.ReadString(16) == "Herta");
	CHECK(Reader.IsAtEnd());
	CHECK(Reader.Read<std::uint64_t>() == 0);
	CHECK_FALSE(Reader.IsValid());

	FBinaryReader Limited(Bytes);
	(void)Limited.ReadBytes(9);
	CHECK(Limited.ReadString(4).empty());
	CHECK_FALSE(Limited.IsValid());
	CHECK_FALSE(FBinaryReader(Bytes).CanRead(1u << 30, 4));

	const FHash128 Hash = HashBytes(Bytes);
	CHECK(ParseHash128(ToString(Hash)) == Hash);
	CHECK_FALSE(ParseHash128("not a hash"));
	CHECK_FALSE(ParseHash128(std::string(32, 'G')));
}

TEST_CASE("Material assets round-trip canonical JSON and cooked data")
{
	FMaterialAsset Material;
	Material.Name = "Copper \"polished\"";
	Material.Parameters.BaseColor = {0.9f, 0.5f, 0.2f, 1.f};
	Material.Parameters.Metallic = 1.f;
	Material.Parameters.Roughness = 0.15f;
	Material.Parameters.BlendMode = EMaterialBlendMode::Masked;
	Material.Textures[1].Texture = FAssetId{1, 2};
	Material.Textures[1].Channel = EMaterialChannel::Blue;
	Material.ShaderPath = "Shaders/Copper.slang";
	const auto Text = SerializeMaterial(Material);
	REQUIRE(Text);
	const auto Loaded = DeserializeMaterial(*Text);
	REQUIRE(Loaded);
	CHECK(*Loaded == Material);
	const auto Again = SerializeMaterial(*Loaded);
	REQUIRE(Again);
	CHECK(*Again == *Text);
	const auto Cooked = SerializeCookedAsset(Material);
	REQUIRE(Cooked);
	const auto Reopened = DeserializeCookedAsset(*Cooked);
	REQUIRE(Reopened);
	REQUIRE(std::holds_alternative<FMaterialAsset>(*Reopened));
	CHECK(std::get<FMaterialAsset>(*Reopened) == Material);
	CHECK_FALSE(DeserializeCookedAsset(std::span(*Cooked).first(Cooked->size() - 1)));
}

TEST_CASE("Material assets reject invalid fields channels and unsafe shader paths")
{
	const auto Canonical = SerializeMaterial(FMaterialAsset{});
	REQUIRE(Canonical);
	CHECK_FALSE(DeserializeMaterial("{}"));
	CHECK_FALSE(DeserializeMaterial("[]"));
	CHECK_FALSE(DeserializeMaterial(Canonical->substr(0, Canonical->size() - 3)));
	std::string Duplicate = *Canonical;
	Duplicate.insert(1, "\"version\":1,");
	CHECK_FALSE(DeserializeMaterial(Duplicate));
	std::string Unknown = *Canonical;
	Unknown.insert(1, "\"unknown\":1,");
	CHECK_FALSE(DeserializeMaterial(Unknown));
	FMaterialAsset Material;
	Material.Parameters.Roughness = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(SerializeMaterial(Material));
	Material.Parameters.Roughness = -0.1f;
	CHECK_FALSE(SerializeMaterial(Material));
	Material.Parameters.Roughness = 0.5f;
	Material.ShaderPath = "../outside.slang";
	CHECK_FALSE(SerializeMaterial(Material));
	Material.ShaderPath = "";
	Material.Textures[1].ColorSpace = ETextureColorSpace::Srgb;
	CHECK_FALSE(SerializeMaterial(Material));
	Material.Textures[1].ColorSpace = ETextureColorSpace::Linear;
	Material.Textures[1].Channel = EMaterialChannel::Rgb;
	CHECK_FALSE(SerializeMaterial(Material));
	CHECK(FMaterialAsset{}.Textures[0].ColorSpace == ETextureColorSpace::Srgb);
	CHECK(FMaterialAsset{}.Textures[3].ColorSpace == ETextureColorSpace::Linear);
}

TEST_CASE("Material cooking tracks cross-mount texture and shader dependencies")
{
	const Tests::FScratchDirectory Scratch("HertaMaterialCook");
	const auto Root = Scratch.GetPath() / "Content";
	const auto EngineRoot = Scratch.GetPath() / "EngineContent";
	Tests::WritePng(EngineRoot / "Textures/Checker.png", 2, 2, CheckerPixels);
	Tests::WriteText(EngineRoot / "Textures/Checker.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Texture\n");
	FMaterialAsset Material;
	Material.Textures[0].Texture = *FAssetId::Parse("00000000-0000-4000-8000-000000000002");
	Material.ShaderPath = "Shaders/Material.slang";
	Tests::WriteText(Root / "Shaders/Material.slang", "#include \"Common.slang\"\n");
	Tests::WriteText(Root / "Shaders/Common.slang", "float value = 1;\n");
	const auto Text = SerializeMaterial(Material);
	REQUIRE(Text);
	Tests::WriteText(Root / "Copper.hmat", *Text);
	Tests::WriteText(Root / "Copper.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000001\nImporter = Material\n");
	auto Request = MakeRequest(Scratch, "Copper.hmat");
	CHECK_FALSE(CookAsset(Request));
	Request.DependencyContentRoots = {EngineRoot};
	const auto First = CookAsset(Request);
	REQUIRE(First);
	const auto Second = CookAsset(Request);
	REQUIRE(Second);
	CHECK(Second->bCacheHit);
	CHECK(Second->Key == First->Key);
	Request.DependencyContentRoots.push_back(Request.ContentRoot);
	const auto DuplicateRoot = CookAsset(Request);
	REQUIRE(DuplicateRoot);
	CHECK(DuplicateRoot->Key == First->Key);

	const auto Loaded = LoadCookedAsset(Request.DerivedDataRoot, First->Key);
	REQUIRE(Loaded);
	CHECK(std::get<FMaterialAsset>(*Loaded) == Material);
	Tests::WriteText(Root / "Shaders/Common.slang", "float value = 2;\n");
	const auto IncludeChanged = CookAsset(Request);
	REQUIRE(IncludeChanged);
	CHECK(IncludeChanged->Key != First->Key);
	Tests::WriteText(Root / "Shaders/Common.slang", "#include \"../Common.slang\"\n");
	CHECK_FALSE(CookAsset(Request));
	Tests::WriteText(Root / "Shaders/Common.slang", "float value = 2;\n");
	constexpr std::array<std::uint8_t, 4> Red{255, 0, 0, 255};
	Tests::WritePng(EngineRoot / "Textures/Checker.png", 1, 1, Red);
	const auto TextureChanged = CookAsset(Request);
	REQUIRE(TextureChanged);
	CHECK(TextureChanged->Key != IncludeChanged->Key);
}

TEST_CASE("Headless material reimport resolves explicit engine content and shader roots")
{
	const Tests::FScratchDirectory Scratch("HertaMaterialCommands");
	const auto Game = Scratch.GetPath() / "Game";
	const auto Engine = Scratch.GetPath() / "Engine/Content";
	const auto Shaders = Scratch.GetPath() / "Engine/Shaders";
	Tests::WritePng(Engine / "Textures/Checker.png", 2, 2, CheckerPixels);
	Tests::WriteText(Engine / "Textures/Checker.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Texture\n");
	Tests::WriteText(Game / "Shaders/Material.slang", "#include \"Shared.slangh\"\n");
	Tests::WriteText(Shaders / "Shared.slangh", "float value = 1;\n");
	FMaterialAsset Material;
	Material.Textures[0].Texture = *FAssetId::Parse("00000000-0000-4000-8000-000000000002");
	Material.ShaderPath = "Shaders/Material.slang";
	REQUIRE(WriteMaterialAsset(Game / "Copper.hmat", Material));
	Tests::WriteText(Game / "Copper.hmat.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000001\nImporter = Material\n");
	FAssetCommandOptions Options{.DefaultContentRoot = Game, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker"), .TargetPlatform = "TestPlatform"};
	FEditorCommandRegistry Unconfigured;
	REQUIRE(RegisterAssetCommands(Unconfigured, Options));
	CHECK_FALSE(Unconfigured.Execute("asset.reimport Copper.hmat"));
	Options.DependencyContentRoots = {Engine, Shaders};
	FEditorCommandRegistry Configured;
	REQUIRE(RegisterAssetCommands(Configured, Options));
	const auto Cooked = Configured.Execute("asset.reimport Copper.hmat");
	REQUIRE_MESSAGE(Cooked.has_value(), (Cooked ? "" : Cooked.error().Message));
	CHECK(Cooked->Message.ends_with("(cooked)"));
	const auto Cached = Configured.Execute("asset.reimport Copper.hmat");
	REQUIRE(Cached);
	CHECK(Cached->Message.ends_with("(cache hit)"));
	Tests::WriteText(Shaders / "Shared.slangh", "float value = 2;\n");
	const auto Changed = Configured.Execute("asset.reimport Copper.hmat");
	REQUIRE(Changed);
	CHECK(Changed->Message.ends_with("(cooked)"));
	CHECK(Changed->Message != Cooked->Message);
}

TEST_CASE("HDR cooking preserves radiance and deterministic linear mip filtering")
{
	constexpr std::array<float, 12> Pixels{4.f, 2.f, 1.f, 1.f, 8.f, 4.f, 2.f, 1.f, 12.f, 6.f, 3.f, 1.f};
	const auto Texture = CookHdrTexture(3, 1, Pixels);
	REQUIRE(Texture);
	CHECK(Texture->ColorSpace == ETextureColorSpace::Linear);
	CHECK(Texture->PixelFormat == ETexturePixelFormat::Rgba32Float);
	REQUIRE(Texture->Mips.size() == 2);
	std::array<float, 4> Average{};
	std::memcpy(Average.data(), Texture->Mips[1].Pixels.data(), sizeof(Average));
	CHECK(Average == std::array<float, 4>{8.f, 4.f, 2.f, 1.f});
	const auto Bytes = SerializeCookedAsset(*Texture);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	const auto Again = SerializeCookedAsset(*Loaded);
	REQUIRE(Again);
	CHECK(*Again == *Bytes);
	CHECK_FALSE(CookHdrTexture(1, 1, std::array<float, 4>{-1.f, 1.f, 1.f, 1.f}));
	FCookedTexture Bad = *Texture;
	Bad.ColorSpace = ETextureColorSpace::Srgb;
	CHECK_FALSE(ValidateCookedTexture(Bad));
}

TEST_CASE("Material source saves atomically without changing registered identity")
{
	const Tests::FScratchDirectory Scratch("HertaMaterialSave");
	const auto Root = Scratch.GetPath() / "Content";
	std::filesystem::create_directories(Root);
	const auto Path = Root / "Test.hmat";
	FMaterialAsset Material;
	REQUIRE(WriteMaterialAsset(Path, Material, false));
	CHECK_FALSE(WriteMaterialAsset(Path, Material, false));
	const auto Imported = ImportSource(Root, Path);
	REQUIRE(Imported);
	Material.Parameters.Roughness = 0.1f;
	REQUIRE(WriteMaterialAsset(Path, Material));
	const auto Scan = ScanContentRoot(Root);
	REQUIRE(Scan);
	REQUIRE(Scan->Registry.GetRecords().size() == 1);
	CHECK(Scan->Registry.GetRecords()[0].Id == Imported->Metadata.Id);
	CHECK_FALSE(WriteMaterialAsset(Root / "Test.txt", Material));
	const auto Cooked = CookAsset(MakeRequest(Scratch, "Test.hmat"));
	REQUIRE(Cooked);
	CHECK(std::get<FMaterialAsset>(*LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Cooked->Key)) == Material);
}

TEST_CASE("Texture color-space requests recook shared sources with correct filtering")
{
	const Tests::FScratchDirectory Scratch("HertaTextureOverrides");
	const auto Root = Scratch.GetPath() / "Content";
	Tests::WritePng(Root / "Checker.png", 2, 2, CheckerPixels);
	Tests::WriteText(Root / "Checker.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000001\nImporter = Texture\n");
	auto Request = MakeRequest(Scratch, "Checker.png");
	const auto Color = CookAsset(Request);
	REQUIRE(Color);
	Request.TextureColorSpace = ETextureColorSpace::Linear;
	const auto Linear = CookAsset(Request);
	REQUIRE(Linear);
	CHECK(Linear->Key != Color->Key);
	const auto LinearAsset = LoadCookedAsset(Request.DerivedDataRoot, Linear->Key);
	const auto ColorAsset = LoadCookedAsset(Request.DerivedDataRoot, Color->Key);
	REQUIRE(LinearAsset);
	REQUIRE(ColorAsset);
	CHECK(Texel(std::get<FCookedTexture>(*LinearAsset).Mips.back())[0] == 128);
	CHECK(Texel(std::get<FCookedTexture>(*ColorAsset).Mips.back())[0] == 188);
	const FAssetWorkerOptions Options{.WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker")};
	Request.bForce = true;
	const auto Worker = CookAssetInWorker(Request, Options);
	REQUIRE(Worker);
	CHECK(Worker->Key == Linear->Key);
}

TEST_CASE("Texture cooking filters mips in linear light")
{
	const auto Checker = CookTexture(2, 2, std::as_bytes(std::span(CheckerPixels)), ETextureColorSpace::Srgb);
	REQUIRE(Checker);
	REQUIRE(Checker->Mips.size() == 2);
	CHECK(Texel(Checker->Mips[0], 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
	CHECK(Texel(Checker->Mips[1]) == std::array<std::uint8_t, 4>{188, 188, 188, 255});

	const auto Linear = CookTexture(2, 2, std::as_bytes(std::span(CheckerPixels)), ETextureColorSpace::Linear);
	REQUIRE(Linear);
	CHECK(Texel(Linear->Mips[1]) == std::array<std::uint8_t, 4>{128, 128, 128, 255});

	const std::array<std::uint8_t, 4> White{255, 255, 255, 255};
	const auto Tinted = CookTexture(1, 1, std::as_bytes(std::span(White)), ETextureColorSpace::Srgb, {0.5f, 1.f, 0.f, 0.5f});
	REQUIRE(Tinted);
	CHECK(Texel(Tinted->Mips[0]) == std::array<std::uint8_t, 4>{188, 255, 0, 128});

	const std::vector<std::uint8_t> Odd(std::size_t{5} * 3 * 4, 77);
	const auto OddMips = CookTexture(5, 3, std::as_bytes(std::span(Odd)), ETextureColorSpace::Srgb);
	REQUIRE(OddMips);
	REQUIRE(OddMips->Mips.size() == 3);
	CHECK(OddMips->Mips[1].Width == 2);
	CHECK(OddMips->Mips[1].Height == 1);
	CHECK(Texel(OddMips->Mips[2]) == std::array<std::uint8_t, 4>{77, 77, 77, 77});

	const std::vector<std::uint8_t> Wide(std::size_t{8192} * 4, 255);
	const auto Clamped = CookTexture(8192, 1, std::as_bytes(std::span(Wide)), ETextureColorSpace::Srgb);
	REQUIRE(Clamped);
	CHECK(Clamped->Mips.front().Width == MaximumCookedTextureDimension);
	CHECK(ValidateCookedTexture(*Clamped));

	const auto Encoded = Tests::EncodePng(2, 2, CheckerPixels);
	const auto Decoded = CookEncodedTexture(Encoded, ETextureColorSpace::Srgb);
	REQUIRE(Decoded);
	CHECK(Decoded->Mips[0].Pixels == Checker->Mips[0].Pixels);

	CHECK_FALSE(CookTexture(2, 2, std::as_bytes(std::span(White)), ETextureColorSpace::Srgb));
	CHECK_FALSE(CookTexture(1, 1, std::as_bytes(std::span(White)), ETextureColorSpace::Srgb, {-1.f, 1.f, 1.f, 1.f}));
	CHECK_FALSE(CookEncodedTexture(std::as_bytes(std::span(White)), ETextureColorSpace::Srgb));
}

TEST_CASE("Texture mip filtering includes odd edges with proportional area weights")
{
	const std::array<std::uint8_t, 12> Edge{0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255};

	for (const bool bVertical : {false, true})
	{
		const auto Texture = CookTexture(bVertical ? 1u : 3u, bVertical ? 3u : 1u, std::as_bytes(std::span(Edge)), ETextureColorSpace::Linear);
		REQUIRE(Texture);
		REQUIRE(Texture->Mips.size() == 2);
		CHECK(Texel(Texture->Mips[1]) == std::array<std::uint8_t, 4>{85, 85, 85, 255});
	}

	const std::array<std::uint8_t, 20> Center{0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255};
	const auto Fractional = CookTexture(5, 1, std::as_bytes(std::span(Center)), ETextureColorSpace::Linear);
	REQUIRE(Fractional);
	REQUIRE(Fractional->Mips.size() == 3);
	CHECK(Texel(Fractional->Mips[1], 0) == std::array<std::uint8_t, 4>{51, 51, 51, 255});
	CHECK(Texel(Fractional->Mips[1], 1) == std::array<std::uint8_t, 4>{51, 51, 51, 255});
	CHECK(Texel(Fractional->Mips[2]) == std::array<std::uint8_t, 4>{51, 51, 51, 255});

	std::array<std::uint8_t, 36> Corner{};

	for (std::size_t Index = 3; Index < Corner.size(); Index += 4)
	{
		Corner[Index] = 255;
	}

	Corner[32] = Corner[33] = Corner[34] = 255;
	const auto TwoDimensions = CookTexture(3, 3, std::as_bytes(std::span(Corner)), ETextureColorSpace::Linear);
	REQUIRE(TwoDimensions);
	CHECK(Texel(TwoDimensions->Mips[1]) == std::array<std::uint8_t, 4>{28, 28, 28, 255});
}

TEST_CASE("Cooked assets round-trip and reject corrupt data")
{
	const auto Texture = CookTexture(2, 2, std::as_bytes(std::span(CheckerPixels)), ETextureColorSpace::Srgb);
	REQUIRE(Texture);
	FCookedModel Model;
	Model.Vertices = {{.Position = {0, 0, 0}, .UV = {0, 0}}, {.Position = {1, 0, 0}, .UV = {1, 0}}, {.Position = {0, 1, 0}, .UV = {0, 1}}};
	Model.Indices = {0, 1, 2};
	Model.Sections = {{.FirstIndex = 0, .IndexCount = 3, .Material = 0}};
	Model.Materials = {{.Name = "Default"}};
	Model.Materials[0].Textures[0] = 0;
	Model.Textures = {*Texture};

	const auto Bytes = SerializeCookedAsset(Model);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	REQUIRE(std::holds_alternative<FCookedModel>(*Loaded));
	const FCookedModel& RoundTrip = std::get<FCookedModel>(*Loaded);
	CHECK(RoundTrip.Indices == Model.Indices);
	CHECK(RoundTrip.Vertices[1].Position == Model.Vertices[1].Position);
	CHECK(RoundTrip.Materials[0].Name == "Default");
	CHECK(RoundTrip.Textures[0].Mips[1].Pixels == Texture->Mips[1].Pixels);

	const auto TextureBytes = SerializeCookedAsset(*Texture);
	REQUIRE(TextureBytes);
	REQUIRE(DeserializeCookedAsset(*TextureBytes));
	CHECK(std::holds_alternative<FCookedTexture>(*DeserializeCookedAsset(*TextureBytes)));

	CHECK_FALSE(DeserializeCookedAsset(std::span(*Bytes).first(Bytes->size() - 1)));
	std::vector<std::byte> Huge = *Bytes;
	Huge[9] = std::byte{0xff};
	Huge[10] = std::byte{0xff};
	Huge[11] = std::byte{0xff};
	CHECK_FALSE(DeserializeCookedAsset(Huge));

	FCookedModel BadIndex = Model;
	BadIndex.Indices[2] = 3;
	CHECK_FALSE(SerializeCookedAsset(BadIndex));
	FCookedModel BadSection = Model;
	BadSection.Sections[0].IndexCount = 6;
	CHECK_FALSE(SerializeCookedAsset(BadSection));
	FCookedModel BadMaterial = Model;
	BadMaterial.Materials[0].Textures[0] = 1;
	CHECK_FALSE(SerializeCookedAsset(BadMaterial));
	FCookedTexture BadChain = *Texture;
	BadChain.Mips.pop_back();
	CHECK_FALSE(SerializeCookedAsset(BadChain));
}

TEST_CASE("glTF cooking flattens nodes, merges materials, and keeps canonical winding")
{
	const Tests::FScratchDirectory Scratch("HertaCookTests");
	WriteQuadModel(Scratch.GetPath() / "Content");
	const auto Result = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(Result);
	CHECK_FALSE(Result->bCacheHit);
	REQUIRE(Result->Warnings.size() == 1);
	CHECK(Result->Warnings[0].find("not a triangle list") != std::string::npos);

	const FCookedModel Model = CookQuad(Scratch);
	REQUIRE(Model.Sections.size() == 2);
	CHECK(Model.Materials[Model.Sections[0].Material].Name == "Checker");
	CHECK(Model.Materials[Model.Sections[1].Material].Name == "Red");
	CHECK(Model.Sections[0].IndexCount == 12);
	CHECK(Model.Sections[1].IndexCount == 12);
	CHECK(Model.Vertices.size() == 8);

	const auto HasPosition = [&Model](const std::array<float, 3>& Position)
	{
		return std::ranges::any_of(Model.Vertices, [&Position](const FCookedVertex& Vertex)
		{
			return Vertex.Position == Position;
		});
	};

	CHECK(HasPosition({1, 2, 3}));
	CHECK(HasPosition({2, 3, 3}));
	CHECK(HasPosition({-6, 1, 0}));

	// Both instances face +Z, including the mirrored one whose triangles were reversed.
	for (std::size_t Index = 0; Index < Model.Indices.size(); Index += 3)
	{
		const auto& A = Model.Vertices[Model.Indices[Index]].Position;
		const auto& B = Model.Vertices[Model.Indices[Index + 1]].Position;
		const auto& C = Model.Vertices[Model.Indices[Index + 2]].Position;
		const float NormalZ = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
		CHECK(NormalZ > 0.f);
	}

	const FCookedTexture& CheckerTexture = Model.Textures[Model.Materials[Model.Sections[0].Material].Textures[0]];
	REQUIRE(CheckerTexture.Mips.size() == 2);
	CHECK(Texel(CheckerTexture.Mips[1]) == std::array<std::uint8_t, 4>{188, 188, 188, 255});
	const FCookedMaterial& RedMaterial = Model.Materials[Model.Sections[1].Material];
	CHECK(RedMaterial.Textures[0] == NoCookedTexture);
	CHECK(RedMaterial.Parameters.BaseColor == std::array<float, 4>{1.f, 0.f, 0.f, 1.f});
	CHECK(RedMaterial.Parameters.Metallic == 1.f);
	CHECK(RedMaterial.Parameters.Roughness == 1.f);

	for (const auto& Vertex : Model.Vertices)
	{
		CHECK(Vertex.Normal == std::array<float, 3>{0.f, 0.f, 1.f});
		CHECK(Vertex.Tangent[3] == (Vertex.Position[0] < 0.f ? 1.f : -1.f));
	}

	FAssetCookRequest Forced = MakeRequest(Scratch, "Models/Quad.gltf");
	Forced.bForce = true;
	const auto Recooked = CookAsset(Forced);
	REQUIRE(Recooked);
	CHECK(Recooked->Key == Result->Key);
	const auto AgainBytes = SerializeCookedAsset(CookQuad(Scratch));
	const auto ModelBytes = SerializeCookedAsset(Model);
	REQUIRE(AgainBytes);
	REQUIRE(ModelBytes);
	CHECK(*AgainBytes == *ModelBytes);
}

TEST_CASE("glTF cooking bounds cumulative geometry before decoding")
{
	const Tests::FScratchDirectory Scratch("HertaGltfLimits");
	const auto CheckRejected = [&](const std::string_view Accessors, const std::string_view Primitives, const std::string_view Error)
	{
		const std::string Gltf = std::format(R"({{"asset":{{"version":"2.0"}},"accessors":[{}],"materials":[{{}},{{}}],"meshes":[{{"primitives":[{}]}}],"nodes":[{{"mesh":0}}],"scenes":[{{"nodes":[0]}}],"scene":0}})", Accessors, Primitives);
		WriteQuadModel(Scratch.GetPath() / "Content", Gltf);
		const auto Cooked = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
		REQUIRE_FALSE(Cooked);
		CHECK(Cooked.error().Message.find(Error) != std::string::npos);
	};

	SUBCASE("A zero-filled accessor cannot request an oversized allocation")
	{
		CheckRejected(R"({"componentType":5126,"count":4294967295,"type":"VEC3","min":[0,0,0],"max":[0,0,0]})", R"({"attributes":{"POSITION":0}})", "unsupported vertex count");
	}

	SUBCASE("Different material buckets share the vertex budget")
	{
		constexpr std::size_t MaximumVertices = MaximumCookedBufferBytes / sizeof(FCookedVertex);
		const std::string Accessors = std::format(R"({{"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]}},{{"componentType":5126,"count":{},"type":"VEC3","min":[0,0,0],"max":[0,0,0]}})", MaximumVertices);
		CheckRejected(Accessors, R"({"attributes":{"POSITION":0},"material":0},{"attributes":{"POSITION":1},"material":1})", "unsupported vertex count");
	}

	SUBCASE("Index counts are bounded before decoding either accessor")
	{
		CheckRejected(R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]},{"componentType":5125,"count":4294967295,"type":"SCALAR"})", R"({"attributes":{"POSITION":0},"indices":1})", "unsupported triangle index count");
	}
}

TEST_CASE("glTF PBR maps retain independent factors channels and color spaces")
{
	const Tests::FScratchDirectory Scratch("HertaPbrCook");
	std::string Source(QuadGltf);
	const std::string Original = R"({"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}})";
	const std::string Replacement = R"({"name":"Checker","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.8],"baseColorTexture":{"index":0},"metallicFactor":0.7,"roughnessFactor":0.3,"metallicRoughnessTexture":{"index":0}},"normalTexture":{"index":0,"scale":0.5},"occlusionTexture":{"index":0,"strength":0.6},"emissiveTexture":{"index":0},"emissiveFactor":[0.1,0.2,0.3],"alphaMode":"MASK","alphaCutoff":0.4})";
	const auto Position = Source.find(Original);
	REQUIRE(Position != std::string::npos);
	Source.replace(Position, Original.size(), Replacement);
	WriteQuadModel(Scratch.GetPath() / "Content", Source);
	const auto Model = CookQuad(Scratch);
	const auto& Material = Model.Materials[0];
	CHECK(Material.Parameters.BaseColor == std::array<float, 4>{0.2f, 0.4f, 0.6f, 0.8f});
	CHECK(Material.Parameters.Metallic == 0.7f);
	CHECK(Material.Parameters.Roughness == 0.3f);
	CHECK(Material.Parameters.NormalStrength == 0.5f);
	CHECK(Material.Parameters.OcclusionStrength == 0.6f);
	CHECK(Material.Parameters.Emissive == std::array<float, 3>{0.1f, 0.2f, 0.3f});
	CHECK(Material.Parameters.BlendMode == EMaterialBlendMode::Masked);
	CHECK(Material.Parameters.AlphaCutoff == 0.4f);
	CHECK(Material.Channels[1] == EMaterialChannel::Blue);
	CHECK(Material.Channels[2] == EMaterialChannel::Green);
	CHECK(Material.Textures[1] == Material.Textures[2]);
	CHECK(Model.Textures[Material.Textures[0]].ColorSpace == ETextureColorSpace::Srgb);
	CHECK(Model.Textures[Material.Textures[1]].ColorSpace == ETextureColorSpace::Linear);
	CHECK(Texel(Model.Textures[Material.Textures[0]].Mips[0]) == std::array<std::uint8_t, 4>{255, 255, 255, 255});
	CHECK(Model.Textures.size() == 2);
}

TEST_CASE("glTF degenerate UVs generate finite orthonormal tangent frames")
{
	const Tests::FScratchDirectory Scratch("HertaTangentCook");
	WriteQuadModel(Scratch.GetPath() / "Content");
	auto Buffer = MakeQuadBuffer();
	std::ranges::fill(std::span(Buffer).subspan(48, 32), std::byte{});
	Tests::WriteBytes(Scratch.GetPath() / "Content/Models/Quad.bin", Buffer);
	const auto Model = CookQuad(Scratch);
	REQUIRE(ValidateCookedModel(Model));

	for (const auto& Vertex : Model.Vertices)
	{
		CHECK(Vertex.Normal == std::array<float, 3>{0.f, 0.f, 1.f});
		CHECK(Vertex.Tangent == std::array<float, 4>{1.f, 0.f, 0.f, 1.f});
	}
}

TEST_CASE("glTF supplied tangent frames preserve mirrored handedness")
{
	const Tests::FScratchDirectory Scratch("HertaSuppliedTangentCook");
	std::string Source(QuadGltf);
	const auto Replace = [&Source](const std::string_view From, const std::string_view To)
	{
		const auto Position = Source.find(From);
		REQUIRE(Position != std::string::npos);
		Source.replace(Position, From.size(), To);
	};

	Replace(R"("TEXCOORD_0": 1)", R"("TEXCOORD_0": 1, "NORMAL": 3, "TANGENT": 4)");
	Replace(R"("byteLength": 92)", R"("byteLength": 204)");
	Replace(R"("byteLength": 12})", R"("byteLength": 12}, {"buffer":0,"byteOffset":92,"byteLength":48}, {"buffer":0,"byteOffset":140,"byteLength":64})");
	Replace(R"("type": "SCALAR"})", R"("type": "SCALAR"}, {"bufferView":3,"componentType":5126,"count":4,"type":"VEC3"}, {"bufferView":4,"componentType":5126,"count":4,"type":"VEC4"})");
	WriteQuadModel(Scratch.GetPath() / "Content", Source);
	FBinaryWriter Writer;
	Writer.WriteBytes(MakeQuadBuffer());

	for (std::size_t Index = 0; Index < 4; ++Index)
	{
		Writer.WriteFloat(0.f);
		Writer.WriteFloat(0.f);
		Writer.WriteFloat(1.f);
	}

	for (std::size_t Index = 0; Index < 4; ++Index)
	{
		Writer.WriteFloat(1.f);
		Writer.WriteFloat(0.f);
		Writer.WriteFloat(0.f);
		Writer.WriteFloat(-1.f);
	}

	Tests::WriteBytes(Scratch.GetPath() / "Content/Models/Quad.bin", Writer.TakeBytes());
	const auto Model = CookQuad(Scratch);
	REQUIRE(ValidateCookedModel(Model));

	for (const auto& Vertex : Model.Vertices)
	{
		CHECK(Vertex.Normal[2] == 1.f);
		CHECK(Vertex.Tangent[3] == (Vertex.Position[0] < 0.f ? 1.f : -1.f));
	}
}

TEST_CASE("Asset cooking reuses derived data until any input changes")
{
	const Tests::FScratchDirectory Scratch("HertaCookTests");
	const std::filesystem::path Root = Scratch.GetPath() / "Content";
	WriteQuadModel(Root);
	const auto First = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(First);
	const auto Second = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(Second);
	CHECK(Second->bCacheHit);
	CHECK(Second->Key == First->Key);
	CHECK(Second->Warnings.empty());

	const std::array<std::uint8_t, 16> Inverted{0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
	Tests::WritePng(Root / "Models/Checker.png", 2, 2, Inverted);
	const auto Changed = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(Changed);
	CHECK_FALSE(Changed->bCacheHit);
	CHECK(Changed->Key != First->Key);

	FAssetCookRequest OtherPlatform = MakeRequest(Scratch, "Models/Quad.gltf");
	OtherPlatform.TargetPlatform = "OtherPlatform";
	const auto PlatformResult = CookAsset(OtherPlatform);
	REQUIRE(PlatformResult);
	CHECK(PlatformResult->Key != Changed->Key);

	Tests::WritePng(Root / "Textures/Mask.png", 2, 2, CheckerPixels);
	Tests::WriteText(Root / "Textures/Mask.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Texture\nSetting.ColorSpace = Linear\n");
	const auto Mask = CookAsset(MakeRequest(Scratch, "Textures/Mask.png"));
	REQUIRE(Mask);
	const auto MaskAsset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Mask->Key);
	REQUIRE(MaskAsset);
	REQUIRE(std::holds_alternative<FCookedTexture>(*MaskAsset));
	CHECK(std::get<FCookedTexture>(*MaskAsset).ColorSpace == ETextureColorSpace::Linear);

	Tests::WriteText(Root / "Textures/Mask.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Texture\nSetting.Colour = Linear\n");
	CHECK_FALSE(CookAsset(MakeRequest(Scratch, "Textures/Mask.png")));
	Tests::WriteText(Root / "Textures/Mask.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Blender\n");
	CHECK_FALSE(CookAsset(MakeRequest(Scratch, "Textures/Mask.png")));
	CHECK_FALSE(CookAsset(MakeRequest(Scratch, "Textures/Missing.png")));
	CHECK_FALSE(LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", FHash128{1, 2}));
}

TEST_CASE("glTF cooking rejects escaping URIs and unsupported required extensions")
{
	const Tests::FScratchDirectory Scratch("HertaCookTests");
	const std::filesystem::path Root = Scratch.GetPath() / "Content";
	std::string Escaping(QuadGltf);
	Escaping.replace(Escaping.find("Quad.bin"), 8, "../../Secret.bin");
	WriteQuadModel(Root, Escaping);
	const auto EscapingResult = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE_FALSE(EscapingResult);
	CHECK(EscapingResult.error().Message.find("escapes the content root") != std::string::npos);

	std::string Draco(QuadGltf);
	Draco.insert(Draco.find("\"scene\""), R"("extensionsUsed": ["KHR_draco_mesh_compression"], "extensionsRequired": ["KHR_draco_mesh_compression"], )");
	WriteQuadModel(Root, Draco);
	CHECK_FALSE(CookAsset(MakeRequest(Scratch, "Models/Quad.gltf")));

	WriteQuadModel(Root, "{ not json");
	CHECK_FALSE(CookAsset(MakeRequest(Scratch, "Models/Quad.gltf")));
}

TEST_CASE("The asset worker cooks out of process with the same result")
{
	const Tests::FScratchDirectory Scratch("HertaCookTests");
	WriteQuadModel(Scratch.GetPath() / "Content");
	const FAssetWorkerOptions Options{.WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker")};
	const auto Worker = CookAssetInWorker(MakeRequest(Scratch, "Models/Quad.gltf"), Options);
	REQUIRE(Worker);
	CHECK_FALSE(Worker->bCacheHit);
	REQUIRE(Worker->Warnings.size() == 1);

	const auto InProcess = CookAsset(MakeRequest(Scratch, "Models/Quad.gltf"));
	REQUIRE(InProcess);
	CHECK(InProcess->bCacheHit);
	CHECK(InProcess->Key == Worker->Key);

	const auto Missing = CookAssetInWorker(MakeRequest(Scratch, "Models/Missing.gltf"), Options);
	REQUIRE_FALSE(Missing);
	CHECK(Missing.error().Message.find("Missing.gltf") != std::string::npos);
}
}
