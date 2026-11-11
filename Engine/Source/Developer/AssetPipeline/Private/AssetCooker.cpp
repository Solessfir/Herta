#include "Herta/AssetPipeline/AssetCooker.h"

#include "BlenderImporter.h"
#include "FileUtilities.h"
#include "GltfCooker.h"
#include "SoundCooker.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/AssetPipeline/BuildKey.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/DerivedDataCache.h"
#include "Herta/AssetPipeline/ShaderSources.h"
#include "Herta/AssetPipeline/TextureCooker.h"
#include "Herta/AnimationGraph/AnimationGraph.h"
#include "Herta/StateTreeCompiler/StateTreeCompiler.h"
#include "Herta/VfxGraph/VfxGraph.h"
#include "Herta/GameplayGraph/GameplayGraph.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>

namespace Herta
{
namespace
{
inline constexpr std::uint64_t MaximumSourceFileSize = std::uint64_t{1} << 30;

// A model's metadata may name an animation graph beside it, which cooks into the model's state machine. Returns its content path.
[[nodiscard]] std::expected<std::optional<std::string>, FAssetError> FindModelAnimationGraph(const std::string& SourcePath, const FAssetImportSettings& Settings, const std::string_view Importer)
{
	const bool bKnown = std::ranges::all_of(Settings, [](const auto& Setting)
	{
		return Setting.first == "AnimationGraph" || Setting.first == "SeverableJoints";
	});

	if (!bKnown)
	{
		return std::unexpected(FAssetError{std::format("The {} importer's settings are AnimationGraph and SeverableJoints", Importer)});
	}

	const auto Graph = Settings.find("AnimationGraph");
	if (Graph == Settings.end())
	{
		return std::nullopt;
	}

	std::string Resolved = (Utf8ToPath(SourcePath).parent_path() / Utf8ToPath(Graph->second)).lexically_normal().generic_string();
	if (!IsValidAssetPath(Resolved) || !Resolved.ends_with(AnimationGraphExtension))
	{
		return std::unexpected(FAssetError{std::format("AnimationGraph '{}' must be a {} file inside the content root, relative to the model", Graph->second, AnimationGraphExtension)});
	}

	return Resolved;
}

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadSource(const std::filesystem::path& Path);

// Compiles the graph into the model's state machine. The Animation module checks its clips against the model when a character uses it.
[[nodiscard]] std::expected<void, FAssetError> CookModelAnimationGraph(const std::filesystem::path& ContentRoot, const std::string& GraphPath, FCookedModel& Model)
{
	if (!Model.Skeleton)
	{
		return std::unexpected(FAssetError{"Only skinned models can have an animation graph"});
	}

	auto Bytes = ReadSource(ContentRoot / Utf8ToPath(GraphPath));
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}

	auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Bytes->data()), Bytes->size()), GetAnimationGraphSchema());
	if (!Graph)
	{
		return std::unexpected(FAssetError{std::format("{}: {}", GraphPath, Graph.error().Message)});
	}

	auto Compiled = CompileAnimationGraph(*Graph);
	if (!Compiled)
	{
		std::string Message = std::format("{} does not compile:", GraphPath);
		for (const FGraphDiagnostic& Diagnostic : Compiled.error())
		{
			Message += " " + Diagnostic.Message + ".";
		}

		return std::unexpected(FAssetError{std::move(Message)});
	}

	Model.StateMachine = std::move(Compiled->StateMachine);
	return {};
}

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadSource(const std::filesystem::path& Path)
{
	std::error_code Error;

	if (std::filesystem::symlink_status(Path, Error).type() != std::filesystem::file_type::regular)
	{
		return std::unexpected(FAssetError{std::format("'{}' is missing or not a regular file", PathToUtf8(Path))});
	}

	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(Path, MaximumSourceFileSize);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}

	std::optional<std::vector<std::byte>>& FileBytes = *Bytes;
	if (!FileBytes)
	{
		return std::unexpected(FAssetError{std::format("'{}' disappeared while cooking", PathToUtf8(Path))});
	}

	return std::move(*FileBytes);
}

[[nodiscard]] std::expected<ETextureColorSpace, FAssetError> GetTextureColorSpace(const FAssetImportSettings& Settings)
{
	ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;

	for (const auto& [Name, Value] : Settings)
	{
		if (Name != "ColorSpace" || (Value != "Srgb" && Value != "Linear"))
		{
			return std::unexpected(FAssetError{std::format("Unsupported texture setting '{} = {}'. Use ColorSpace = Srgb or Linear", Name, Value)});
		}

		ColorSpace = Value == "Linear" ? ETextureColorSpace::Linear : ETextureColorSpace::Srgb;
	}

	return ColorSpace;
}

// Content-relative paths, one per line. A record is keyed by the .blend and Blender version, so its dependencies are known without starting Blender.
[[nodiscard]] std::vector<std::byte> SerializeDependencyRecord(const std::vector<std::string>& Dependencies)
{
	std::string Text;

	for (const std::string& Dependency : Dependencies)
	{
		Text += Dependency;
		Text += '\n';
	}

	const std::span<const std::byte> Bytes = std::as_bytes(std::span(Text));
	return {Bytes.begin(), Bytes.end()};
}

[[nodiscard]] std::optional<std::vector<std::string>> ParseDependencyRecord(const std::span<const std::byte> Bytes)
{
	std::vector<std::string> Dependencies;
	const std::string_view Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());

	for (const auto Line : std::views::split(Text, '\n'))
	{
		const std::string_view Path(Line.begin(), Line.end());
		if (Path.empty())
		{
			continue;
		}

		if (!IsValidAssetPath(Path))
		{
			return std::nullopt;
		}

		Dependencies.emplace_back(Path);
	}

	return Dependencies;
}

// Missing dependencies hash to zero, so creating the file later changes the key.
[[nodiscard]] std::expected<FHash128, FAssetError> HashDependency(const std::filesystem::path& ContentRoot, const std::string& Path)
{
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(ContentRoot / Utf8ToPath(Path), MaximumSourceFileSize);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}

	const std::optional<std::vector<std::byte>>& FileBytes = *Bytes;
	return FileBytes ? HashBytes(*FileBytes) : FHash128{};
}

[[nodiscard]] std::expected<void, FAssetError> AppendDependencyHashes(const std::filesystem::path& ContentRoot, const std::vector<std::string>& Paths, std::vector<FAssetBuildDependency>& Dependencies)
{
	for (const std::string& Path : Paths)
	{
		std::expected<FHash128, FAssetError> Hash = HashDependency(ContentRoot, Path);
		if (!Hash)
		{
			return std::unexpected(std::move(Hash.error()));
		}

		Dependencies.push_back({.Path = Path, .ContentHash = *Hash});
	}

	return {};
}

std::string ShaderDirectiveLine(const std::string_view Line, bool& bBlockComment)
{
	std::string Result;
	bool bQuoted = false;
	bool bEscaped = false;

	for (std::size_t Index = 0; Index < Line.size(); ++Index)
	{
		const char Character = Line[Index];
		const char Next = Index + 1 < Line.size() ? Line[Index + 1] : '\0';
		if (bBlockComment)
		{
			if (Character == '*' && Next == '/')
			{
				bBlockComment = false;
				++Index;
			}

			continue;
		}

		if (!bQuoted && Character == '/' && Next == '/')
		{
			break;
		}

		if (!bQuoted && Character == '/' && Next == '*')
		{
			bBlockComment = true;
			++Index;
			Result += ' ';
			continue;
		}

		Result += Character;
		if (Character == '"' && !bEscaped)
		{
			bQuoted = !bQuoted;
		}

		bEscaped = bQuoted && Character == '\\' && !bEscaped;
	}

	return Result;
}

std::expected<void, FAssetError> AppendShaderDependencies(const std::vector<std::filesystem::path>& Roots, const std::size_t RootIndex, const std::string& Path, std::set<std::pair<std::size_t, std::string>>& Seen, std::vector<FAssetBuildDependency>& Dependencies, std::vector<FShaderSourceFile>* Files = nullptr)
{
	if (!IsValidAssetPath(Path))
	{
		return std::unexpected(FAssetError{"Shader dependency path is invalid or exceeds the 256-file limit"});
	}

	if (Seen.contains({RootIndex, Path}))
	{
		return {};
	}

	if (Seen.size() >= 256)
	{
		return std::unexpected(FAssetError{"Shader dependency graph exceeds the 256-file limit"});
	}

	Seen.emplace(RootIndex, Path);

	std::filesystem::path Current = Roots[RootIndex];

	for (const auto& Part : Utf8ToPath(Path))
	{
		Current /= Part;
		std::error_code Error;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(Current, Error)))
		{
			return std::unexpected(FAssetError{"Shader dependencies cannot follow symbolic links"});
		}
	}

	const auto Bytes = ReadWholeFile(Roots[RootIndex] / Utf8ToPath(Path), std::uint64_t{16} * 1024 * 1024);
	if (!Bytes || !*Bytes)
	{
		return std::unexpected(Bytes ? FAssetError{std::format("Shader dependency '{}' is missing", Path)} : Bytes.error());
	}

	Dependencies.push_back({.Path = std::format("shader/{}/{}", RootIndex, Path), .ContentHash = HashBytes(**Bytes)});
	if (Files)
	{
		std::size_t TotalBytes = (**Bytes).size();

		for (const auto& File : *Files)
		{
			TotalBytes += File.Bytes.size();
		}

		if (TotalBytes > std::size_t{64} * 1024 * 1024)
		{
			return std::unexpected(FAssetError{"Shader source graph exceeds the 64 MiB limit"});
		}

		Files->push_back({.Root = RootIndex, .Path = Path, .Bytes = **Bytes});
	}

	const std::string_view Text(reinterpret_cast<const char*>((**Bytes).data()), (**Bytes).size());
	bool bBlockComment = false;

	for (const auto LineRange : std::views::split(Text, '\n'))
	{
		const std::string Clean = ShaderDirectiveLine(std::string_view(LineRange.begin(), LineRange.end()), bBlockComment);
		std::string_view Line(Clean);
		const auto Begin = Line.find_first_not_of(" \t\r");
		if (Begin == std::string_view::npos)
		{
			continue;
		}

		Line.remove_prefix(Begin);
		const bool bDirective = Line.starts_with('#');
		if (bDirective)
		{
			Line.remove_prefix(1);
			const auto Command = Line.find_first_not_of(" \t");
			if (Command == std::string_view::npos)
			{
				continue;
			}

			Line.remove_prefix(Command);
		}

		std::string Include;
		if (bDirective && Line.starts_with("include") && (Line.size() == 7 || Line[7] == ' ' || Line[7] == '\t' || Line[7] == '"' || Line[7] == '<'))
		{
			const auto First = Line.find_first_of("\"<", 7);
			const auto Last = First == std::string_view::npos ? First : Line.find(Line[First] == '"' ? '"' : '>', First + 1);
			if (First == std::string_view::npos || Last == std::string_view::npos)
			{
				return std::unexpected(FAssetError{"Shader includes must use literal portable paths"});
			}

			Include = Line.substr(First + 1, Last - First - 1);
		}
		else if (!bDirective && (Line.starts_with("import ") || Line.starts_with("import\t")))
		{
			const auto Last = Line.find(';', 7);
			if (Last == std::string_view::npos)
			{
				return std::unexpected(FAssetError{"Invalid shader import"});
			}

			Include = Line.substr(7, Last - 7);
			const auto Module = Include.find_first_not_of(" \t");
			if (Module != std::string::npos)
			{
				Include.erase(0, Module);
			}

			while (!Include.empty() && (Include.back() == ' ' || Include.back() == '\t'))
			{
				Include.pop_back();
			}

			std::ranges::replace(Include, '.', '/');
			Include += ".slang";
		}
		else
		{
			continue;
		}

		const auto Relative = GenericPathToUtf8((Utf8ToPath(Path).parent_path() / Utf8ToPath(Include)).lexically_normal());
		if (Utf8ToPath(Include).has_root_name() || Utf8ToPath(Include).has_root_directory() || !IsValidAssetPath(Relative))
		{
			return std::unexpected(FAssetError{"Shader include escapes its content root"});
		}

		std::size_t FoundRoot = RootIndex;
		std::string FoundPath = Relative;
		std::error_code Error;
		bool bFound = std::filesystem::is_regular_file(Roots[RootIndex] / Utf8ToPath(Relative), Error);

		for (std::size_t Index = 0; !bFound && Index < Roots.size(); ++Index)
		{
			bFound = std::filesystem::is_regular_file(Roots[Index] / Utf8ToPath(Include), Error);
			if (bFound)
			{
				FoundRoot = Index;
				FoundPath = Include;
			}
		}

		if (!bFound)
		{
			return std::unexpected(FAssetError{std::format("Shader dependency '{}' is missing", Include)});
		}

		if (auto Added = AppendShaderDependencies(Roots, FoundRoot, FoundPath, Seen, Dependencies, Files); !Added)
		{
			return Added;
		}
	}

	return {};
}

std::expected<void, FAssetError> AppendMaterialDependencies(const FAssetCookRequest& Request, const FMaterialAsset& Material, std::vector<FAssetBuildDependency>& Dependencies)
{
	std::vector<std::filesystem::path> Roots{Request.ContentRoot};
	Roots.insert(Roots.end(), Request.DependencyContentRoots.begin(), Request.DependencyContentRoots.end());
	if (!Request.EngineContentRoot.empty())
	{
		Roots.push_back(Request.EngineContentRoot);
	}

	if (!Request.GameContentRoot.empty())
	{
		Roots.push_back(Request.GameContentRoot);
	}
	std::vector<std::filesystem::path> UniqueRoots;

	for (const auto& Root : Roots)
	{
		std::error_code Error;
		const auto Absolute = std::filesystem::absolute(Root, Error).lexically_normal();
		if (Error)
		{
			return std::unexpected(FAssetError{"Cannot resolve a material dependency content root"});
		}

		if (std::ranges::find(UniqueRoots, Absolute) == UniqueRoots.end())
		{
			UniqueRoots.push_back(Absolute);
		}
	}

	Roots = std::move(UniqueRoots);
	std::map<FAssetId, std::pair<std::filesystem::path, FAssetRecord>> Records;
	const bool bHasTextures = std::ranges::any_of(Material.Textures, [](const FMaterialTextureBinding& Binding)
	{
		return Binding.Texture.IsValid();
	});

	if (bHasTextures)
	{
		for (const auto& Root : Roots)
		{
			std::error_code Error;
			// Additional roots also serve optional shader includes, which are not shipped with stock materials.
			if (!std::filesystem::exists(Root, Error) && !Error)
			{
				continue;
			}

			const auto Scan = ScanContentRoot(Root);
			if (!Scan)
			{
				return std::unexpected(Scan.error());
			}

			for (const auto& Record : Scan->Registry.GetRecords())
			{
				const auto [Iterator, bInserted] = Records.emplace(Record.Id, std::make_pair(Root, Record));
				if (!bInserted && Iterator->second.first / Utf8ToPath(Iterator->second.second.SourcePath) != Root / Utf8ToPath(Record.SourcePath))
				{
					return std::unexpected(FAssetError{"Duplicate asset ID across material dependency roots"});
				}
			}
		}
	}

	std::set<FAssetId> Seen;
	std::set<FAssetId> HdrTextures;

	for (const auto& Binding : Material.Textures)
	{
		if (!Binding.Texture.IsValid())
		{
			continue;
		}

		if (Seen.contains(Binding.Texture))
		{
			if (HdrTextures.contains(Binding.Texture) && Binding.ColorSpace != ETextureColorSpace::Linear)
			{
				return std::unexpected(FAssetError{"HDR textures require Linear color space"});
			}

			continue;
		}

		const auto Found = Records.find(Binding.Texture);
		if (Found == Records.end() || Found->second.second.Importer != "Texture")
		{
			return std::unexpected(FAssetError{std::format("Material texture {} is missing or is not a texture", Binding.Texture.ToString())});
		}

		const auto& [Root, Record] = Found->second;
		const auto Pixels = ReadSource(Root / Utf8ToPath(Record.SourcePath));
		const auto Metadata = ReadSource(Root / Utf8ToPath(Record.SourcePath + ".hmeta"));
		if (!Pixels || !Metadata)
		{
			return std::unexpected(!Pixels ? Pixels.error() : Metadata.error());
		}

		if (IsEncodedHdrTexture(*Pixels))
		{
			if (Binding.ColorSpace != ETextureColorSpace::Linear)
			{
				return std::unexpected(FAssetError{"HDR textures require Linear color space"});
			}

			HdrTextures.insert(Binding.Texture);
		}

		Seen.insert(Binding.Texture);

		Dependencies.push_back({.Path = std::format("texture/{}", Binding.Texture.ToString()), .ContentHash = HashBytes(*Pixels)});
		Dependencies.push_back({.Path = std::format("metadata/{}", Binding.Texture.ToString()), .ContentHash = HashBytes(*Metadata)});
	}

	if (!Material.ShaderPath.empty())
	{
		std::size_t ShaderRoot = 0;
		std::string ShaderPath = Material.ShaderPath;
		const std::filesystem::path* MountedRoot = nullptr;
		if (ShaderPath.starts_with("Engine/"))
		{
			MountedRoot = &Request.EngineContentRoot;
			ShaderPath.erase(0, 7);
		}
		else if (ShaderPath.starts_with("Game/"))
		{
			MountedRoot = &Request.GameContentRoot;
			ShaderPath.erase(0, 5);
		}

		if (MountedRoot)
		{
			if (MountedRoot->empty())
			{
				return std::unexpected(FAssetError{"Mounted material shader path requires its explicit Engine or Game content root"});
			}

			std::error_code Error;
			const auto Absolute = std::filesystem::absolute(*MountedRoot, Error).lexically_normal();
			const auto Found = std::ranges::find(Roots, Absolute);
			if (Error || Found == Roots.end())
			{
				return std::unexpected(FAssetError{"Cannot resolve mounted material shader root"});
			}

			ShaderRoot = static_cast<std::size_t>(std::distance(Roots.begin(), Found));
		}

		std::set<std::pair<std::size_t, std::string>> ShaderFiles;
		return AppendShaderDependencies(Roots, ShaderRoot, ShaderPath, ShaderFiles, Dependencies);
	}

	return {};
}
}

std::expected<std::vector<FShaderSourceFile>, FAssetError> CollectShaderSources(const std::span<const std::filesystem::path> Roots, const std::size_t SourceRoot, const std::string_view SourcePath)
{
	if (Roots.empty() || Roots.size() > 16 || SourceRoot >= Roots.size())
	{
		return std::unexpected(FAssetError{"Invalid shader source roots"});
	}

	std::vector<FShaderSourceFile> Files;
	std::vector<FAssetBuildDependency> Dependencies;
	std::set<std::pair<std::size_t, std::string>> Seen;
	const std::vector<std::filesystem::path> Directories(Roots.begin(), Roots.end());
	if (auto Added = AppendShaderDependencies(Directories, SourceRoot, std::string(SourcePath), Seen, Dependencies, &Files); !Added)
	{
		return std::unexpected(Added.error());
	}

	return Files;
}

std::expected<FAssetCookResult, FAssetError> CookAsset(const FAssetCookRequest& Request)
{
	if (!IsValidAssetPath(Request.SourcePath) || Request.TargetPlatform.empty())
	{
		return std::unexpected(FAssetError{std::format("Invalid cook request for '{}'", Request.SourcePath)});
	}

	const std::filesystem::path SourceFile = Request.ContentRoot / Utf8ToPath(Request.SourcePath);
	std::expected<FAssetMetadata, FAssetError> Metadata = LoadAssetMetadata(SourceFile);
	if (!Metadata)
	{
		return std::unexpected(std::move(Metadata.error()));
	}

	std::expected<std::vector<std::byte>, FAssetError> Source = ReadSource(SourceFile);
	if (!Source)
	{
		return std::unexpected(std::move(Source.error()));
	}

	FAssetBuildKeyInput KeyInput{.SourceHash = HashBytes(*Source), .SourcePath = Request.SourcePath, .Importer = Metadata->Importer, .ImporterVersion = 0, .Settings = Metadata->Settings, .Dependencies = {}, .TargetPlatform = Request.TargetPlatform, .CookedFormatVersion = CookedAssetFormatVersion};
	const FDerivedDataCache Cache(Request.DerivedDataRoot);
	FAssetCookResult Result;
	std::optional<FBlenderInstallation> Blender;
	std::optional<FBlenderExport> BlenderExport;
	std::optional<FMaterialAsset> Material;
	std::optional<FSoundSettings> SoundSettings;
	std::optional<FCookedSoundGraph> SoundGraph;
	std::optional<FCookedParticleSystem> ParticleSystem;
	std::optional<FCookedStateTree> StateTree;
	std::optional<FCookedGameplayGraph> GameplayGraph;
	FHash128 BlenderRecordKey;
	std::expected<ETextureColorSpace, FAssetError> ColorSpace = ETextureColorSpace::Srgb;

	if (Metadata->Importer == "Texture")
	{
		KeyInput.ImporterVersion = TextureImporterVersion;
		ColorSpace = GetTextureColorSpace(Metadata->Settings);
		if (!ColorSpace)
		{
			return std::unexpected(std::move(ColorSpace.error()));
		}

		if (IsEncodedHdrTexture(*Source) && !Metadata->Settings.contains("ColorSpace"))
		{
			ColorSpace = ETextureColorSpace::Linear;
		}

		if (Request.TextureColorSpace)
		{
			if (*Request.TextureColorSpace != ETextureColorSpace::Linear && *Request.TextureColorSpace != ETextureColorSpace::Srgb)
			{
				return std::unexpected(FAssetError{"Unknown requested texture color space"});
			}

			ColorSpace = *Request.TextureColorSpace;
		}

		if (IsEncodedHdrTexture(*Source) && *ColorSpace != ETextureColorSpace::Linear)
		{
			return std::unexpected(FAssetError{"HDR textures require Linear color space"});
		}

		KeyInput.Settings["ColorSpace"] = *ColorSpace == ETextureColorSpace::Linear ? "Linear" : "Srgb";
	}
	else if (Metadata->Importer == "Material")
	{
		KeyInput.ImporterVersion = MaterialImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The Material importer has no settings"});
		}

		const auto Parsed = DeserializeMaterial(std::string_view(reinterpret_cast<const char*>(Source->data()), Source->size()));
		if (!Parsed)
		{
			return std::unexpected(Parsed.error());
		}

		Material = *Parsed;
		if (auto Added = AppendMaterialDependencies(Request, *Material, KeyInput.Dependencies); !Added)
		{
			return std::unexpected(Added.error());
		}
	}
	else if (Metadata->Importer == "Sound" || Metadata->Importer == "AudioGraph")
	{
		const bool bWave = Metadata->Importer == "Sound";
		KeyInput.ImporterVersion = bWave ? SoundImporterVersion : AudioGraphImporterVersion;
		auto Parsed = ParseSoundSettings(Metadata->Settings, bWave);
		if (!Parsed)
		{
			return std::unexpected(std::move(Parsed.error()));
		}

		SoundSettings = std::move(*Parsed);
		if (!bWave)
		{
			auto Graph = CookSoundGraph(Request.ContentRoot, Request.SourcePath, *Source, *SoundSettings, KeyInput.Dependencies);
			if (!Graph)
			{
				return std::unexpected(std::move(Graph.error()));
			}

			SoundGraph = std::move(*Graph);
		}
	}
	else if (Metadata->Importer == "VfxGraph")
	{
		KeyInput.ImporterVersion = VfxGraphImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The VfxGraph importer has no settings"});
		}

		auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Source->data()), Source->size()), GetVfxGraphSchema());
		if (!Graph)
		{
			return std::unexpected(FAssetError{std::format("{}: {}", Request.SourcePath, Graph.error().Message)});
		}

		auto Compiled = CompileVfxGraph(*Graph);
		if (!Compiled)
		{
			std::string Message = std::format("{} does not compile:", Request.SourcePath);
			for (const FGraphDiagnostic& Diagnostic : Compiled.error())
			{
				Message += " " + Diagnostic.Message + ".";
			}

			return std::unexpected(FAssetError{std::move(Message)});
		}

		ParticleSystem = std::move(Compiled->System);
	}
	else if (Metadata->Importer == "GameplayGraph")
	{
		KeyInput.ImporterVersion = GameplayGraphImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The GameplayGraph importer has no settings"});
		}

		auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Source->data()), Source->size()), GetGameplayGraphSchema());
		if (!Graph)
		{
			return std::unexpected(FAssetError{std::format("{}: {}", Request.SourcePath, Graph.error().Message)});
		}

		auto Compiled = CompileGameplayGraph(*Graph);
		if (!Compiled)
		{
			std::string Message = std::format("{} does not compile:", Request.SourcePath);
			for (const FGraphDiagnostic& Diagnostic : Compiled.error())
			{
				Message += " " + Diagnostic.Message + ".";
			}

			return std::unexpected(FAssetError{std::move(Message)});
		}

		GameplayGraph = std::move(Compiled->Program);
	}
	else if (Metadata->Importer == "StateTree")
	{
		KeyInput.ImporterVersion = StateTreeImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The StateTree importer has no settings"});
		}

		auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Source->data()), Source->size()), GetStateTreeSchema());
		if (!Graph)
		{
			return std::unexpected(FAssetError{std::format("{}: {}", Request.SourcePath, Graph.error().Message)});
		}

		auto Compiled = CompileStateTree(*Graph);
		if (!Compiled)
		{
			std::string Message = std::format("{} does not compile:", Request.SourcePath);
			for (const FGraphDiagnostic& Diagnostic : Compiled.error())
			{
				Message += std::format(" {} (node {}).", Diagnostic.Message, Diagnostic.Node.Value);
			}

			return std::unexpected(FAssetError{std::move(Message)});
		}

		StateTree = std::move(Compiled->Tree);
	}
	else if (Metadata->Importer == "Gltf")
	{
		KeyInput.ImporterVersion = GltfImporterVersion;

		std::expected<std::vector<std::string>, FAssetError> Dependencies = FindGltfDependencies(Request.ContentRoot, Request.SourcePath, *Source);
		if (!Dependencies)
		{
			return std::unexpected(std::move(Dependencies.error()));
		}

		for (std::string& Dependency : *Dependencies)
		{
			std::expected<std::vector<std::byte>, FAssetError> Bytes = ReadSource(Request.ContentRoot / Utf8ToPath(Dependency));
			if (!Bytes)
			{
				return std::unexpected(std::move(Bytes.error()));
			}

			KeyInput.Dependencies.push_back({.Path = std::move(Dependency), .ContentHash = HashBytes(*Bytes)});
		}
	}
	else if (Metadata->Importer == "Blender")
	{
		KeyInput.ImporterVersion = BlenderImporterVersion;

		std::expected<FBlenderInstallation, FAssetError> Found = FindBlender();
		if (!Found)
		{
			return std::unexpected(std::move(Found.error()));
		}

		Blender = std::move(*Found);
		KeyInput.Dependencies.push_back({.Path = "<Blender>", .ContentHash = HashBytes(std::as_bytes(std::span(Blender->Version)))});

		FAssetBuildKeyInput RecordInput = KeyInput;
		RecordInput.Importer = "Blender/Dependencies";
		BlenderRecordKey = ComputeAssetBuildKey(RecordInput);
		std::optional<std::vector<std::string>> Dependencies;

		if (!Request.bForce)
		{
			std::expected<std::optional<std::vector<std::byte>>, FAssetError> Record = Cache.Get(BlenderRecordKey);
			if (Record)
			{
				if (const std::optional<std::vector<std::byte>>& RecordBytes = *Record)
				{
					Dependencies = ParseDependencyRecord(*RecordBytes);
				}
			}
		}

		if (!Dependencies)
		{
			std::expected<FBlenderExport, FAssetError> Exported = ExportBlend(*Blender, Request.ContentRoot, Request.SourcePath);
			if (!Exported)
			{
				return std::unexpected(std::move(Exported.error()));
			}

			BlenderExport = std::move(*Exported);
			Dependencies = BlenderExport->Dependencies;
			std::ranges::move(BlenderExport->Warnings, std::back_inserter(Result.Warnings));

			if (std::expected<void, FAssetError> Stored = Cache.Put(BlenderRecordKey, SerializeDependencyRecord(*Dependencies)); !Stored)
			{
				return std::unexpected(std::move(Stored.error()));
			}
		}

		if (std::expected<void, FAssetError> Hashed = AppendDependencyHashes(Request.ContentRoot, *Dependencies, KeyInput.Dependencies); !Hashed)
		{
			return std::unexpected(std::move(Hashed.error()));
		}
	}
	else
	{
		return std::unexpected(FAssetError{std::format("The {} importer is not available yet", Metadata->Importer)});
	}

	std::optional<std::string> AnimationGraph;
	if (Metadata->Importer == "Gltf" || Metadata->Importer == "Blender")
	{
		auto Found = FindModelAnimationGraph(Request.SourcePath, Metadata->Settings, Metadata->Importer);
		if (!Found)
		{
			return std::unexpected(std::move(Found.error()));
		}

		AnimationGraph = std::move(*Found);
		if (AnimationGraph)
		{
			auto Bytes = ReadSource(Request.ContentRoot / Utf8ToPath(*AnimationGraph));
			if (!Bytes)
			{
				return std::unexpected(std::move(Bytes.error()));
			}

			KeyInput.Dependencies.push_back({.Path = *AnimationGraph, .ContentHash = HashBytes(*Bytes)});
		}
	}

	Result.Key = ComputeAssetBuildKey(KeyInput);

	if (!Request.bForce)
	{
		std::expected<std::optional<std::vector<std::byte>>, FAssetError> Cached = Cache.Get(Result.Key);
		if (Cached && *Cached)
		{
			Result.bCacheHit = true;
			return Result;
		}

		if (!Cached)
		{
			Result.Warnings.push_back(std::format("{}; cooking again", Cached.error().Message));
		}
	}

	std::expected<FCookedAsset, FAssetError> Cooked = std::unexpected(FAssetError{});

	if (Metadata->Importer == "Texture")
	{
		std::expected<FCookedTexture, FAssetError> Texture = CookEncodedTexture(*Source, *ColorSpace);
		Cooked = Texture ? std::expected<FCookedAsset, FAssetError>(std::move(*Texture)) : std::unexpected(std::move(Texture.error()));
	}
	else if (Metadata->Importer == "Material")
	{
		if (!Material)
		{
			return std::unexpected(FAssetError{"The Material importer produced no material"});
		}

		Cooked = std::move(*Material);
	}
	else if (Metadata->Importer == "Sound")
	{
		std::expected<FCookedSound, FAssetError> Sound = CookSound(*Source, *SoundSettings);
		Cooked = Sound ? std::expected<FCookedAsset, FAssetError>(std::move(*Sound)) : std::unexpected(std::move(Sound.error()));
	}
	else if (Metadata->Importer == "AudioGraph")
	{
		Cooked = std::move(*SoundGraph);
	}
	else if (Metadata->Importer == "VfxGraph")
	{
		Cooked = std::move(*ParticleSystem);
	}
	else if (Metadata->Importer == "GameplayGraph")
	{
		Cooked = std::move(*GameplayGraph);
	}
	else if (Metadata->Importer == "StateTree")
	{
		Cooked = std::move(*StateTree);
	}
	else if (Metadata->Importer == "Gltf")
	{
		std::expected<FCookedModel, FAssetError> Model = CookGltf(Request.ContentRoot, Request.SourcePath, *Source, Result.Warnings);
		Cooked = Model ? std::expected<FCookedAsset, FAssetError>(std::move(*Model)) : std::unexpected(std::move(Model.error()));
	}
	else
	{
		if (!Blender)
		{
			return std::unexpected(FAssetError{"The Blender importer found no Blender installation"});
		}

		if (!BlenderExport)
		{
			std::expected<FBlenderExport, FAssetError> Exported = ExportBlend(*Blender, Request.ContentRoot, Request.SourcePath);
			if (!Exported)
			{
				return std::unexpected(std::move(Exported.error()));
			}

			BlenderExport = std::move(*Exported);
			std::ranges::move(BlenderExport->Warnings, std::back_inserter(Result.Warnings));

			// A changed linked library can introduce new images or libraries without changing the root .blend.
			KeyInput.Dependencies.resize(1);

			if (std::expected<void, FAssetError> Hashed = AppendDependencyHashes(Request.ContentRoot, BlenderExport->Dependencies, KeyInput.Dependencies); !Hashed)
			{
				return std::unexpected(std::move(Hashed.error()));
			}

			if (std::expected<void, FAssetError> Stored = Cache.Put(BlenderRecordKey, SerializeDependencyRecord(BlenderExport->Dependencies)); !Stored)
			{
				return std::unexpected(std::move(Stored.error()));
			}

			Result.Key = ComputeAssetBuildKey(KeyInput);
		}

		// The GLB is parsed as if it sat beside the .blend. The preset embeds every image, so an external reference means the export is not what Herta asked for.
		std::expected<std::vector<std::string>, FAssetError> External = FindGltfDependencies(Request.ContentRoot, Request.SourcePath, BlenderExport->Glb);
		if (!External || !External->empty())
		{
			return std::unexpected(External ? FAssetError{std::format("Blender's export of '{}' references external files", Request.SourcePath)} : std::move(External.error()));
		}

		std::expected<FCookedModel, FAssetError> Model = CookGltf(Request.ContentRoot, Request.SourcePath, BlenderExport->Glb, Result.Warnings);
		Cooked = Model ? std::expected<FCookedAsset, FAssetError>(std::move(*Model)) : std::unexpected(std::move(Model.error()));
	}

	if (!Cooked)
	{
		return std::unexpected(std::move(Cooked.error()));
	}

	if (AnimationGraph)
	{
		if (auto Compiled = CookModelAnimationGraph(Request.ContentRoot, *AnimationGraph, std::get<FCookedModel>(*Cooked)); !Compiled)
		{
			return std::unexpected(std::move(Compiled.error()));
		}
	}

	// Severable joints are named in the metadata, comma separated, and must be non-root joints of the model's skeleton.
	if (const auto Severable = Metadata->Settings.find("SeverableJoints"); Severable != Metadata->Settings.end())
	{
		FCookedModel* const Model = std::get_if<FCookedModel>(&*Cooked);
		if (!Model || !Model->Skeleton)
		{
			return std::unexpected(FAssetError{"Only skinned models can have severable joints"});
		}

		std::string_view List = Severable->second;
		while (!List.empty())
		{
			const std::size_t Comma = std::min(List.find(','), List.size());
			std::string_view Name = List.substr(0, Comma);
			List.remove_prefix(std::min(Comma + 1, List.size()));
			while (!Name.empty() && Name.front() == ' ')
			{
				Name.remove_prefix(1);
			}

			while (!Name.empty() && Name.back() == ' ')
			{
				Name.remove_suffix(1);
			}

			const auto& Names = Model->Skeleton->JointNames;
			const auto Found = std::ranges::find(Names, Name);
			if (Name.empty() || Found == Names.end() || Model->Skeleton->JointParents[static_cast<std::size_t>(Found - Names.begin())] < 0)
			{
				return std::unexpected(FAssetError{std::format("Severable joint '{}' is not a non-root joint of the skeleton", Name)});
			}

			Model->SeverableJoints.emplace_back(Name);
		}
	}

	std::expected<std::vector<std::byte>, FAssetError> Bytes = SerializeCookedAsset(*Cooked);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}

	if (std::expected<void, FAssetError> Stored = Cache.Put(Result.Key, *Bytes); !Stored)
	{
		return std::unexpected(std::move(Stored.error()));
	}

	return Result;
}

std::expected<FCookedAsset, FAssetError> LoadCookedAsset(const std::filesystem::path& DerivedDataRoot, const FHash128& Key)
{
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = FDerivedDataCache(DerivedDataRoot).Get(Key);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}

	const std::optional<std::vector<std::byte>>& CachedBytes = *Bytes;
	if (!CachedBytes)
	{
		return std::unexpected(FAssetError{std::format("Derived data {} is missing", ToString(Key))});
	}

	return DeserializeCookedAsset(*CachedBytes);
}
}
