#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/AssetPipeline/BuildKey.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/DerivedDataCache.h"
#include "Herta/Platform/Process.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <string>

namespace Herta
{
namespace
{
struct FScratchDirectory
{
	FScratchDirectory()
	    : Path(std::filesystem::temp_directory_path() / ("HertaAssetTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		std::filesystem::create_directories(Path);
	}

	~FScratchDirectory()
	{
		std::error_code Error;
		std::filesystem::remove_all(Path, Error);
	}

	FScratchDirectory(const FScratchDirectory&) = delete;
	FScratchDirectory& operator=(const FScratchDirectory&) = delete;
	FScratchDirectory(FScratchDirectory&&) = delete;
	FScratchDirectory& operator=(FScratchDirectory&&) = delete;

	std::filesystem::path Path;
};

void WriteText(const std::filesystem::path& Path, const std::string_view Text)
{
	std::filesystem::create_directories(Path.parent_path());
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
}

std::string ToCommandPath(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return '"' + std::string(Text.begin(), Text.end()) + '"';
}

std::string MakeMetadataText(const std::string_view Id, const std::string_view Importer)
{
	return std::string("Format = HertaAssetMetadata\nVersion = 1\nId = ") + std::string(Id) + "\nImporter = " + std::string(Importer) + "\n";
}

FAssetId ParseId(const std::string_view Text)
{
	return FAssetId::Parse(Text).value_or(FAssetId());
}

std::span<const std::byte> AsBytes(const std::string_view Text)
{
	return std::as_bytes(std::span(Text.data(), Text.size()));
}

FAssetBuildKeyInput MakeBuildKeyInput()
{
	FAssetBuildKeyInput Input;
	Input.SourceHash = HashBytes(AsBytes("source"));
	Input.SourcePath = "Meshes/Cube.glb";
	Input.Importer = "Gltf";
	Input.ImporterVersion = 1;
	Input.Settings = {{"Scale", "1"}};
	Input.Dependencies = {{.Path = "Meshes/Cube.bin", .ContentHash = HashBytes(AsBytes("buffer"))}, {.Path = "Textures/Wood.png", .ContentHash = HashBytes(AsBytes("image"))}};
	Input.TargetPlatform = "windows";
	Input.CookedFormatVersion = 1;
	return Input;
}
}

TEST_CASE("Content hashes are stable XXH3-128 values")
{
	CHECK(ToString(HashBytes({})) == "99aa06d3014798d86001c324468d497f");
	CHECK(HashBytes(AsBytes("abc")) == HashBytes(AsBytes("abc")));
	CHECK(HashBytes(AsBytes("abc")) != HashBytes(AsBytes("abd")));
}

TEST_CASE("Asset IDs round-trip through one canonical text form")
{
	const FAssetId Generated = FAssetId::Generate();
	CHECK(Generated.IsValid());
	CHECK(((Generated.GetHigh() >> 12) & 0xf) == 4);
	CHECK((Generated.GetLow() >> 62) == 2);
	CHECK(FAssetId::Parse(Generated.ToString()) == Generated);
	CHECK(FAssetId::Generate() != Generated);

	const FAssetId Parsed = ParseId("123e4567-e89b-12d3-a456-426614174000");
	REQUIRE(Parsed.IsValid());
	CHECK(Parsed.ToString() == "123e4567-e89b-12d3-a456-426614174000");
	CHECK(Parsed.GetHigh() == 0x123e4567e89b12d3ull);
	CHECK(Parsed.GetLow() == 0xa456426614174000ull);

	CHECK_FALSE(FAssetId::Parse("123E4567-E89B-12D3-A456-426614174000"));
	CHECK_FALSE(FAssetId::Parse("123e4567e89b-12d3-a456-4266141740000"));
	CHECK_FALSE(FAssetId::Parse("{123e4567-e89b-12d3-a456-426614174000}"));
	CHECK_FALSE(FAssetId::Parse("123e4567-e89b-12d3-a456-42661417400"));
	CHECK_FALSE(FAssetId::Parse("00000000-0000-0000-0000-000000000000"));
	CHECK_FALSE(FAssetId().IsValid());
}

TEST_CASE("Asset paths must be relative and portable")
{
	CHECK(IsValidAssetPath("Meshes/Cube.glb"));
	CHECK(IsValidAssetPath("Textures/\xc3\x9cmlaut.png"));
	for (const std::string_view Path : {"", "/Meshes/Cube.glb", "Meshes//Cube.glb", "Meshes/./Cube.glb", "../Cube.glb", "Meshes\\Cube.glb", "C:/Cube.glb", "Cube.", "Cube ", "Meshes/", "Cube?.glb", "Cu\tbe.glb"})
	{
		const std::string PathText(Path);
		CAPTURE(PathText);
		CHECK_FALSE(IsValidAssetPath(Path));
	}
}

TEST_CASE("Asset registries index records and reject ambiguity")
{
	const FAssetId Cube = ParseId("00000000-0000-4000-8000-000000000002");
	const FAssetId Wood = ParseId("00000000-0000-4000-8000-000000000001");
	const auto Registry = FAssetRegistry::Create({{.Id = Wood, .SourcePath = "Textures/Wood.png", .Importer = "Texture"}, {.Id = Cube, .SourcePath = "Meshes/Cube.glb", .Importer = "Gltf"}});
	REQUIRE(Registry);
	REQUIRE(Registry->GetRecords().size() == 2);
	CHECK(Registry->GetRecords()[0].SourcePath == "Meshes/Cube.glb");
	REQUIRE(Registry->Find(Wood));
	CHECK(Registry->Find(Wood)->SourcePath == "Textures/Wood.png");
	REQUIRE(Registry->FindBySourcePath("Meshes/Cube.glb"));
	CHECK(Registry->FindBySourcePath("Meshes/Cube.glb")->Id == Cube);
	CHECK_FALSE(Registry->Find(FAssetId::Generate()));
	CHECK_FALSE(Registry->FindBySourcePath("meshes/cube.glb"));

	CHECK_FALSE(FAssetRegistry::Create({{Cube, "A.png", "Texture"}, {Cube, "B.png", "Texture"}}));
	CHECK_FALSE(FAssetRegistry::Create({{Cube, "Textures/Wood.png", "Texture"}, {Wood, "textures/wood.PNG", "Texture"}}));
	CHECK_FALSE(FAssetRegistry::Create({{FAssetId(), "A.png", "Texture"}}));
	CHECK_FALSE(FAssetRegistry::Create({{Cube, "../A.png", "Texture"}}));
}

TEST_CASE("Asset metadata uses a canonical text format")
{
	const FAssetMetadata Metadata{.Id = ParseId("123e4567-e89b-42d3-a456-426614174000"), .Importer = "Texture", .Settings = {{"Srgb", "true"}, {"Mips", "Full"}}};
	const auto Text = SerializeAssetMetadata(Metadata);
	REQUIRE(Text);
	CHECK(*Text == "Format = HertaAssetMetadata\n"
	               "Version = 1\n"
	               "Id = 123e4567-e89b-42d3-a456-426614174000\n"
	               "Importer = Texture\n"
	               "Setting.Mips = Full\n"
	               "Setting.Srgb = true\n");

	const auto Parsed = ParseAssetMetadata(*Text);
	REQUIRE(Parsed);
	CHECK(Parsed->Id == Metadata.Id);
	CHECK(Parsed->Importer == "Texture");
	CHECK(Parsed->Settings == Metadata.Settings);

	const auto Unordered = ParseAssetMetadata("Format = HertaAssetMetadata\r\nVersion = 1\r\nSetting.Srgb = true\r\nImporter = Texture\r\nSetting.Mips = Full\r\nId = 123e4567-e89b-42d3-a456-426614174000\r\n");
	REQUIRE(Unordered);
	const auto Canonical = SerializeAssetMetadata(*Unordered);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);

	const std::string Valid = MakeMetadataText("123e4567-e89b-42d3-a456-426614174000", "Texture");
	CHECK(ParseAssetMetadata(Valid));
	CHECK_FALSE(ParseAssetMetadata(Valid + "Colour = Red\n"));
	CHECK_FALSE(ParseAssetMetadata(Valid + "Setting.A = 1\nSetting.A = 2\n"));
	CHECK_FALSE(ParseAssetMetadata(Valid + "Setting.A = 1 \n"));
	CHECK_FALSE(ParseAssetMetadata(Valid + "Id = 123e4567-e89b-42d3-a456-426614174001\n"));
	CHECK_FALSE(ParseAssetMetadata("Format = HertaAssetMetadata\nVersion = 2\nId = 123e4567-e89b-42d3-a456-426614174000\nImporter = Texture\n"));
	CHECK_FALSE(ParseAssetMetadata("Format = HertaAssetMetadata\nVersion = 1\nImporter = Texture\n"));
	CHECK_FALSE(ParseAssetMetadata(MakeMetadataText("123e4567-e89b-42d3-a456-426614174000", "Tex ture")));
	CHECK_FALSE(SerializeAssetMetadata(FAssetMetadata{FAssetId(), "Texture", {}}));
}

TEST_CASE("Asset build keys cover every input")
{
	const FAssetBuildKeyInput Base = MakeBuildKeyInput();
	const FHash128 BaseKey = ComputeAssetBuildKey(Base);
	CHECK(ComputeAssetBuildKey(MakeBuildKeyInput()) == BaseKey);

	FAssetBuildKeyInput Reordered = Base;
	std::ranges::reverse(Reordered.Dependencies);
	CHECK(ComputeAssetBuildKey(Reordered) == BaseKey);

	const auto CheckChanged = [&BaseKey](const auto& Mutate)
	{
		FAssetBuildKeyInput Input = MakeBuildKeyInput();
		Mutate(Input);
		CHECK(ComputeAssetBuildKey(Input) != BaseKey);
	};

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.SourceHash.Low ^= 1;
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.SourcePath = "Meshes/Crate.glb";
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.Importer = "Blender";
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.ImporterVersion = 2;
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.Settings["Scale"] = "2";
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.Settings.clear();
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.Dependencies[0].ContentHash.High ^= 1;
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.Dependencies.pop_back();
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.TargetPlatform = "linux";
	});

	CheckChanged([](FAssetBuildKeyInput& Input)
	{
		Input.CookedFormatVersion = 2;
	});

	FAssetBuildKeyInput Left = Base;
	Left.SourcePath = "ab";
	Left.Importer = "c";
	FAssetBuildKeyInput Right = Base;
	Right.SourcePath = "a";
	Right.Importer = "bc";
	CHECK(ComputeAssetBuildKey(Left) != ComputeAssetBuildKey(Right));
}

TEST_CASE("Derived data cache entries are atomic and validated")
{
	const FScratchDirectory Scratch;
	const FDerivedDataCache Cache(Scratch.Path / "DerivedDataCache");
	const FHash128 Key = ComputeAssetBuildKey(MakeBuildKeyInput());
	const std::string KeyText = ToString(Key);
	CHECK(Cache.GetEntryPath(Key) == Scratch.Path / "DerivedDataCache" / KeyText.substr(0, 2) / (KeyText + ".hddc"));

	const auto Miss = Cache.Get(Key);
	REQUIRE(Miss);
	CHECK_FALSE(*Miss);

	const std::span<const std::byte> Payload = AsBytes("cooked payload");
	REQUIRE(Cache.Put(Key, Payload));
	REQUIRE(Cache.Put(Key, Payload));
	const auto Hit = Cache.Get(Key);
	REQUIRE(Hit);
	REQUIRE(Hit->has_value());
	CHECK(std::ranges::equal(Hit->value_or(std::vector<std::byte>{}), Payload));
	CHECK(std::distance(std::filesystem::directory_iterator(Cache.GetEntryPath(Key).parent_path()), std::filesystem::directory_iterator()) == 1);

	const FHash128 EmptyKey{.High = 1, .Low = 2};
	REQUIRE(Cache.Put(EmptyKey, {}));
	const auto Empty = Cache.Get(EmptyKey);
	REQUIRE(Empty);
	REQUIRE(Empty->has_value());
	CHECK(Empty->value_or(std::vector<std::byte>(1)).empty());

	std::filesystem::copy_file(Cache.GetEntryPath(Key), Cache.GetEntryPath(EmptyKey), std::filesystem::copy_options::overwrite_existing);
	CHECK_FALSE(Cache.Get(EmptyKey));

	{
		std::fstream Stream(Cache.GetEntryPath(Key), std::ios::binary | std::ios::in | std::ios::out);
		Stream.seekp(40);
		Stream.put('X');
	}
	CHECK_FALSE(Cache.Get(Key));

	std::filesystem::resize_file(Cache.GetEntryPath(Key), 10);
	CHECK_FALSE(Cache.Get(Key));
}

TEST_CASE("Importable extensions cover every importer and drive the import dialog filter")
{
	const std::vector<std::string_view> Extensions = GetImportableExtensions();
	CHECK(std::ranges::find(Extensions, ".blend") != Extensions.end());
	CHECK(std::ranges::find(Extensions, ".gltf") != Extensions.end());
	for (const std::string_view Extension : Extensions)
	{
		CHECK(Extension.starts_with('.'));
		CHECK(FindImporterForSource(std::string("Asset") + std::string(Extension)).has_value());
	}
}

TEST_CASE("Content scans reuse unchanged sidecars from a cache and reparse edited ones")
{
	const FScratchDirectory Scratch;
	const std::filesystem::path& Root = Scratch.Path;
	WriteText(Root / "Meshes/Cube.glb", "glb");
	const std::filesystem::path Sidecar = Root / "Meshes/Cube.glb.hmeta";
	WriteText(Sidecar, MakeMetadataText("00000000-0000-4000-8000-000000000001", "Gltf"));

	FContentScanCache Cache;
	const auto First = ScanContentRoot(Root, &Cache);
	REQUIRE(First);
	REQUIRE(Cache.Metadata.size() == 1);

	// Same size and restored write time: the cached parse wins, which proves the sidecar was not read again.
	const auto WriteTime = std::filesystem::last_write_time(Sidecar);
	WriteText(Sidecar, MakeMetadataText("00000000-0000-4000-8000-000000000002", "Gltf"));
	std::filesystem::last_write_time(Sidecar, WriteTime);
	const auto Cached = ScanContentRoot(Root, &Cache);
	REQUIRE(Cached);
	REQUIRE(Cached->Registry.GetRecords().size() == 1);
	CHECK(Cached->Registry.GetRecords()[0].Id.ToString() == "00000000-0000-4000-8000-000000000001");

	// A real edit changes the write time and is parsed.
	std::filesystem::last_write_time(Sidecar, WriteTime + std::chrono::seconds(2));
	const auto Edited = ScanContentRoot(Root, &Cache);
	REQUIRE(Edited);
	CHECK(Edited->Registry.GetRecords()[0].Id.ToString() == "00000000-0000-4000-8000-000000000002");

	// Removed sidecars leave the cache.
	std::filesystem::remove(Sidecar);
	REQUIRE(ScanContentRoot(Root, &Cache));
	CHECK(Cache.Metadata.empty());
}

TEST_CASE("Content scans register sources and report problems")
{
	const FScratchDirectory Scratch;
	const std::filesystem::path& Root = Scratch.Path;
	WriteText(Root / "Meshes/Cube.glb", "glb");
	WriteText(Root / "Meshes/Cube.glb.hmeta", MakeMetadataText("00000000-0000-4000-8000-000000000001", "Gltf"));
	WriteText(Root / "Textures/Wood.png", "png");
	WriteText(Root / "Orphan.png.hmeta", MakeMetadataText("00000000-0000-4000-8000-000000000002", "Texture"));
	WriteText(Root / "Broken.png", "png");
	WriteText(Root / "Broken.png.hmeta", "Format = Something\n");
	WriteText(Root / "A.png", "png");
	WriteText(Root / "A.png.hmeta", MakeMetadataText("00000000-0000-4000-8000-000000000003", "Texture"));
	WriteText(Root / "B.png", "png");
	WriteText(Root / "B.png.hmeta", MakeMetadataText("00000000-0000-4000-8000-000000000003", "Texture"));
	WriteText(Root / ".gitkeep", "");
	WriteText(Root / "Fonts/Readme.txt", "not an asset");
	WriteText(Root / ".git/Ignored.png", "png");

	const auto Scan = ScanContentRoot(Root);
	REQUIRE(Scan);
	REQUIRE(Scan->Registry.GetRecords().size() == 1);
	CHECK(Scan->Registry.GetRecords()[0].SourcePath == "Meshes/Cube.glb");
	CHECK(Scan->Registry.GetRecords()[0].Importer == "Gltf");
	CHECK(Scan->UnregisteredSources == std::vector<std::string>{"Textures/Wood.png"});

	std::vector<std::string> ErrorPaths;
	for (const FContentDiagnostic& Diagnostic : Scan->Errors)
	{
		ErrorPaths.push_back(Diagnostic.Path);
	}

	CHECK(ErrorPaths == std::vector<std::string>{"A.png", "B.png", "Broken.png.hmeta", "Orphan.png.hmeta"});

	CHECK_FALSE(ScanContentRoot(Root / "Missing"));
}

TEST_CASE("Importing registers content sources and copies external ones")
{
	const FScratchDirectory Scratch;
	const std::filesystem::path Root = Scratch.Path / "Content";
	std::filesystem::create_directories(Root);
	WriteText(Scratch.Path / "External/Crate.PNG", "crate");
	WriteText(Root / "Meshes/Ship.gltf", "ship");
	WriteText(Scratch.Path / "External/Notes.txt", "notes");

	const auto Copied = ImportSource(Root, Scratch.Path / "External/Crate.PNG", "Textures");
	REQUIRE(Copied);
	CHECK(Copied->SourcePath == "Textures/Crate.PNG");
	CHECK(Copied->Metadata.Importer == "Texture");
	CHECK(std::filesystem::exists(Root / "Textures/Crate.PNG"));
	CHECK(std::filesystem::exists(Root / "Textures/Crate.PNG.hmeta"));
	CHECK_FALSE(ImportSource(Root, Scratch.Path / "External/Crate.PNG", "Textures"));
	CHECK_FALSE(ImportSource(Root, Root / "Textures/Crate.PNG"));

	const auto InPlace = ImportSource(Root, Root / "Meshes/Ship.gltf", "Ignored");
	REQUIRE(InPlace);
	CHECK(InPlace->SourcePath == "Meshes/Ship.gltf");
	CHECK(InPlace->Metadata.Importer == "Gltf");

	CHECK_FALSE(ImportSource(Root, Scratch.Path / "External/Notes.txt"));
	WriteText(Scratch.Path / "External/Ship.gltf", "{}");
	const auto ExternalGltf = ImportSource(Root, Scratch.Path / "External/Ship.gltf");
	REQUIRE_FALSE(ExternalGltf);
	CHECK(ExternalGltf.error().Message.find("imported in place") != std::string::npos);
	CHECK_FALSE(ImportSource(Root, Scratch.Path / "External/Missing.png"));
	CHECK_FALSE(ImportSource(Root, Scratch.Path / "External/Crate.PNG", "../Escape"));

	const auto Scan = ScanContentRoot(Root);
	REQUIRE(Scan);
	CHECK(Scan->Errors.empty());
	CHECK(Scan->UnregisteredSources.empty());
	REQUIRE(Scan->Registry.Find(Copied->Metadata.Id));
	CHECK(Scan->Registry.Find(Copied->Metadata.Id)->SourcePath == "Textures/Crate.PNG");
}

TEST_CASE("Asset commands run headlessly against a content root")
{
	const FScratchDirectory Scratch;
	const std::filesystem::path Root = Scratch.Path / "Content";
	std::filesystem::create_directories(Root);
	const std::array<std::uint8_t, 4> Wood{150, 111, 51, 255};
	Tests::WritePng(Scratch.Path / "Wood Planks.png", 1, 1, Wood);

	FEditorCommandRegistry Registry;
	REQUIRE(RegisterAssetCommands(Registry, {Root, Scratch.Path / "DerivedDataCache", Tests::GetSiblingExecutable("HertaAssetWorker"), "TestPlatform"}));

	const auto Imported = Registry.Execute("asset.import " + ToCommandPath(Scratch.Path / "Wood Planks.png") + " --destination Textures");
	REQUIRE(Imported);
	CHECK(Imported->ExitCode == 0);
	CHECK(Imported->Message.starts_with("Imported Textures/Wood Planks.png as "));
	CHECK(Imported->Message.find("\nCooked Textures/Wood Planks.png -> ") != std::string::npos);
	CHECK(Imported->Message.ends_with("(cooked)"));

	const auto Cached = Registry.Execute("asset.reimport \"Textures/Wood Planks.png\"");
	REQUIRE(Cached);
	CHECK(Cached->Message.ends_with("(cache hit)"));
	const auto Forced = Registry.Execute("asset.reimport \"Textures/Wood Planks.png\" --force");
	REQUIRE(Forced);
	CHECK(Forced->Message.ends_with("(cooked)"));
	CHECK_FALSE(Registry.Execute("asset.reimport 00000000-0000-4000-8000-00000000dead"));
	CHECK_FALSE(Registry.Execute("asset.reimport Textures/Missing.png"));

	const auto Listed = Registry.Execute("asset.list");
	REQUIRE(Listed);
	CHECK(Listed->Message.ends_with(" Texture  Textures/Wood Planks.png"));

	const auto Valid = Registry.Execute("asset.validate --content-root " + ToCommandPath(Root));
	REQUIRE(Valid);
	CHECK(Valid->ExitCode == 0);
	CHECK(Valid->Message == "1 assets, 0 errors, 0 unregistered");

	WriteText(Root / "Orphan.png.hmeta", MakeMetadataText("00000000-0000-4000-8000-000000000002", "Texture"));
	const auto Invalid = Registry.Execute("asset.validate");
	REQUIRE(Invalid);
	CHECK(Invalid->ExitCode == 1);
	CHECK(Invalid->Message.starts_with("error: Orphan.png.hmeta: Metadata has no source file\n"));

	CHECK_FALSE(Registry.Execute("asset.import"));
	CHECK_FALSE(Registry.Execute("asset.list --unknown"));
	CHECK_FALSE(Registry.Execute("asset.list --content-root " + ToCommandPath(Root / "Missing")));
}
}
