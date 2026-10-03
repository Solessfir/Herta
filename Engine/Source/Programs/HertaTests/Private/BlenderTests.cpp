#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/Blender.h"
#include "Herta/AssetPipeline/DerivedDataCache.h"
#include "Herta/Platform/Process.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>

namespace Herta
{
namespace
{
constexpr std::string_view SceneScript = R"PY(import sys

import bpy

blend, external = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, image):
    result = bpy.data.materials.new(name)
    result.use_nodes = True
    texture = result.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    result.node_tree.links.new(texture.outputs["Color"], result.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
    return result


bpy.ops.mesh.primitive_cube_add(location=(1, 0, 0))
parent = bpy.context.object
parent.modifiers.new("Subdivision", "SUBSURF").levels = 1
parent.data.materials.append(material("External", bpy.data.images.load(external)))
bpy.ops.mesh.primitive_plane_add(location=(0, 2, 0))
child = bpy.context.object
child.parent = parent
child.scale = (-1, 1, 1)
packed = bpy.data.images.new("Packed", 4, 4)
packed.pixels = [1.0, 0.0, 0.0, 1.0] * 16
packed.pack()
child.data.materials.append(material("Packed", packed))
bpy.ops.object.camera_add()
bpy.ops.object.light_add(type="POINT")
bpy.ops.wm.save_as_mainfile(filepath=blend, relative_remap=True)
)PY";

[[nodiscard]] std::optional<FBlenderInstallation> FindBlenderForTests()
{
	std::expected<FBlenderInstallation, FAssetError> Blender = FindBlender();
	if (!Blender)
	{
		MESSAGE("Skipping: ", Blender.error().Message);
		return std::nullopt;
	}

	return *Blender;
}

// Saves a scene with a parented negative-scale child, an unapplied modifier, external and packed images, and non-mesh objects.
void WriteEdgeCaseBlend(const FBlenderInstallation& Blender, const Tests::FScratchDirectory& Scratch, const std::filesystem::path& Blend, const std::filesystem::path& ExternalImage)
{
	Tests::WriteText(Scratch.GetPath() / "Scene.py", SceneScript);
	std::filesystem::create_directories(Blend.parent_path());
	const auto Result = RunProcess({.Executable = Blender.Executable, .Arguments = {"--background", "--factory-startup", "--python-exit-code", "1", "--python", (Scratch.GetPath() / "Scene.py").generic_string(), "--", Blend.generic_string(), ExternalImage.generic_string()}, .Timeout = std::chrono::minutes(2)});
	REQUIRE(Result);
	INFO(Result->StandardError);
	REQUIRE(Result->ExitCode == 0);
}

void WriteBlenderMetadata(const std::filesystem::path& Blend)
{
	Tests::WriteText(Blend.string() + ".hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-0000000000b1\nImporter = Blender\n");
}

void SetBlenderOverride(const std::optional<std::string>& Value)
{
#ifdef HERTA_PLATFORM_WINDOWS
	_putenv_s("HERTA_BLENDER", Value ? Value->c_str() : "");
#else
	if (Value)
	{
		setenv("HERTA_BLENDER", Value->c_str(), 1);
	}
	else
	{
		unsetenv("HERTA_BLENDER");
	}
#endif
}
}

TEST_CASE("Blender import cooks a .blend through Herta's preset, tracks external images, and is deterministic")
{
	const std::optional<FBlenderInstallation> Blender = FindBlenderForTests();
	if (!Blender)
	{
		return;
	}

	const Tests::FScratchDirectory Scratch("HertaBlender");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	const std::array<std::uint8_t, 16> Pixels{0, 128, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 128, 255, 255};
	Tests::WritePng(Content / "Textures/Checker.png", 2, 2, Pixels);
	WriteEdgeCaseBlend(*Blender, Scratch, Content / "Models/Edge.blend", Content / "Textures/Checker.png");
	WriteBlenderMetadata(Content / "Models/Edge.blend");

	const FAssetCookRequest Request{.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Models/Edge.blend", .TargetPlatform = "TestPlatform", .bForce = false};
	const auto First = CookAsset(Request);
	REQUIRE_MESSAGE(First, (First ? std::string() : First.error().Message));
	CHECK_FALSE(First->bCacheHit);
	CHECK(First->Warnings.empty());
	auto Asset = LoadCookedAsset(Request.DerivedDataRoot, First->Key);
	REQUIRE(Asset);
	REQUIRE(std::holds_alternative<FCookedModel>(*Asset));
	const FCookedModel& Model = std::get<FCookedModel>(*Asset);
	// The subdivided cube and the plane keep separate materials; the camera and light export nothing.
	CHECK(Model.Sections.size() == 2);
	CHECK(Model.Textures.size() == 2);
	CHECK(Model.Vertices.size() > 28);

	// The dependency record lets a repeat cook hit the cache without starting Blender for an export.
	const auto Second = CookAsset(Request);
	REQUIRE(Second);
	CHECK(Second->bCacheHit);
	CHECK(Second->Key == First->Key);

	const FDerivedDataCache Cache(Request.DerivedDataRoot);
	const auto Before = Cache.Get(First->Key);
	REQUIRE((Before && *Before));
	FAssetCookRequest Forced = Request;
	Forced.bForce = true;
	const auto Recooked = CookAsset(Forced);
	REQUIRE(Recooked);
	CHECK(Recooked->Key == First->Key);
	const auto After = Cache.Get(First->Key);
	REQUIRE((After && *After));
	CHECK(**After == **Before);

	// Editing the external image changes the key even though the .blend is unchanged.
	const std::array<std::uint8_t, 16> Edited{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Content / "Textures/Checker.png", 2, 2, Edited);
	const auto AfterEdit = CookAsset(Request);
	REQUIRE(AfterEdit);
	CHECK_FALSE(AfterEdit->bCacheHit);
	CHECK(AfterEdit->Key != First->Key);
}

TEST_CASE("Blender dependency records follow changes inside linked libraries")
{
	const std::optional<FBlenderInstallation> Blender = FindBlenderForTests();
	if (!Blender)
	{
		return;
	}

	const Tests::FScratchDirectory Scratch("HertaBlenderLinked");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	const std::filesystem::path Library = Content / "Models/Library.blend";
	const std::filesystem::path Root = Content / "Models/Root.blend";
	const std::filesystem::path OldImage = Content / "Textures/Old.png";
	const std::filesystem::path NewImage = Content / "Textures/New.png";
	const std::array<std::uint8_t, 4> White{255, 255, 255, 255};
	const std::array<std::uint8_t, 4> Green{0, 255, 0, 255};
	Tests::WritePng(OldImage, 1, 1, White);
	Tests::WritePng(NewImage, 1, 1, White);
	WriteEdgeCaseBlend(*Blender, Scratch, Library, OldImage);

	constexpr std::string_view LinkScript = R"PY(import sys

import bpy

root, library = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
with bpy.data.libraries.load(library, link=True) as (source, target):
    target.objects = source.objects
for obj in target.objects:
    bpy.context.scene.collection.objects.link(obj)
bpy.ops.wm.save_as_mainfile(filepath=root, relative_remap=True)
)PY";
	Tests::WriteText(Scratch.GetPath() / "Link.py", LinkScript);
	const auto Linked = RunProcess({.Executable = Blender->Executable, .Arguments = {"--background", "--factory-startup", "--python-exit-code", "1", "--python", (Scratch.GetPath() / "Link.py").generic_string(), "--", Root.generic_string(), Library.generic_string()}, .Timeout = std::chrono::minutes(2)});
	REQUIRE(Linked);
	INFO(Linked->StandardError);
	REQUIRE(Linked->ExitCode == 0);
	WriteBlenderMetadata(Root);

	const FAssetCookRequest Request{.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Models/Root.blend", .TargetPlatform = "TestPlatform", .bForce = false};
	const auto First = CookAsset(Request);
	REQUIRE_MESSAGE(First, (First ? std::string() : First.error().Message));
	CHECK_FALSE(First->bCacheHit);

	// Only the linked library changes; the root file retains its original dependency-record key.
	WriteEdgeCaseBlend(*Blender, Scratch, Library, NewImage);
	const auto LibraryChanged = CookAsset(Request);
	REQUIRE_MESSAGE(LibraryChanged, (LibraryChanged ? std::string() : LibraryChanged.error().Message));
	CHECK_FALSE(LibraryChanged->bCacheHit);
	CHECK(LibraryChanged->Key != First->Key);
	const auto Repeated = CookAsset(Request);
	REQUIRE(Repeated);
	CHECK(Repeated->bCacheHit);
	CHECK(Repeated->Key == LibraryChanged->Key);

	Tests::WritePng(NewImage, 1, 1, Green);
	const auto NewImageChanged = CookAsset(Request);
	REQUIRE(NewImageChanged);
	CHECK_FALSE(NewImageChanged->bCacheHit);
	CHECK(NewImageChanged->Key != LibraryChanged->Key);

	Tests::WritePng(OldImage, 1, 1, Green);
	const auto RemovedImageChanged = CookAsset(Request);
	REQUIRE(RemovedImageChanged);
	CHECK(RemovedImageChanged->bCacheHit);
	CHECK(RemovedImageChanged->Key == NewImageChanged->Key);
}

TEST_CASE("Blender import rejects images outside the content root")
{
	const std::optional<FBlenderInstallation> Blender = FindBlenderForTests();
	if (!Blender)
	{
		return;
	}

	const Tests::FScratchDirectory Scratch("HertaBlenderOutside");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	const std::array<std::uint8_t, 4> Pixel{255, 255, 255, 255};
	Tests::WritePng(Scratch.GetPath() / "Outside.png", 1, 1, Pixel);
	WriteEdgeCaseBlend(*Blender, Scratch, Content / "Escape.blend", Scratch.GetPath() / "Outside.png");
	WriteBlenderMetadata(Content / "Escape.blend");

	const auto Cooked = CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Escape.blend", .TargetPlatform = "TestPlatform", .bForce = false});
	REQUIRE_FALSE(Cooked);
	CHECK(Cooked.error().Message.find("outside the content root") != std::string::npos);
}

TEST_CASE("Blender import fails clearly when Blender is unavailable")
{
	const Tests::FScratchDirectory Scratch("HertaBlenderMissing");
	Tests::WriteText(Scratch.GetPath() / "Missing.blend", "BLENDER");
	WriteBlenderMetadata(Scratch.GetPath() / "Missing.blend");

	SetBlenderOverride((Scratch.GetPath() / "NoBlenderHere").string());
	const auto Cooked = CookAsset({.ContentRoot = Scratch.GetPath(), .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Missing.blend", .TargetPlatform = "TestPlatform", .bForce = false});
	SetBlenderOverride(std::nullopt);
	REQUIRE_FALSE(Cooked);
	CHECK(Cooked.error().Message.find("Blender was not found") != std::string::npos);
}
}
