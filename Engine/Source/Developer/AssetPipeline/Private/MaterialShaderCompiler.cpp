#include "Herta/AssetPipeline/MaterialShaderCompiler.h"

#include "FileUtilities.h"
#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/MaterialGraph/MaterialGraph.h"
#include "Herta/Platform/Process.h"

#include <format>
#include <fstream>
#include <thread>
#include <utility>

namespace Herta
{
namespace
{
struct FStageEntry
{
	std::string_view EntryPoint;
	bool bFragment = false;
};

// In MaterialShaderStageNames order.
constexpr std::array<FStageEntry, MaterialShaderStageCount> StageEntries{
    FStageEntry{.EntryPoint = "vertexMain"},
    FStageEntry{.EntryPoint = "instancedVertexMain"},
    FStageEntry{.EntryPoint = "fragmentMain", .bFragment = true},
    FStageEntry{.EntryPoint = "depthFragmentMain", .bFragment = true},
    FStageEntry{.EntryPoint = "velocityFragmentMain", .bFragment = true},
    FStageEntry{.EntryPoint = "instancedVelocityVertexMain"},
    FStageEntry{.EntryPoint = "surfaceFragmentMain", .bFragment = true},
    FStageEntry{.EntryPoint = "velocitySurfaceFragmentMain", .bFragment = true},
};

std::string Utf8Path(const std::filesystem::path& Path)
{
	const auto Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

std::filesystem::path SourcePath(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

class FShaderScratch final
{
public:
	~FShaderScratch();
	FShaderScratch() = default;
	FShaderScratch(const FShaderScratch&) = delete;
	FShaderScratch& operator=(const FShaderScratch&) = delete;
	FShaderScratch(FShaderScratch&&) = delete;
	FShaderScratch& operator=(FShaderScratch&&) = delete;

	[[nodiscard]] std::expected<void, FAssetError> Initialize(const FHash128& Key);
	const std::filesystem::path& GetPath() const noexcept;

private:
	std::filesystem::path Path;
};

FShaderScratch::~FShaderScratch()
{
	if (!Path.empty())
	{
		std::error_code Error;
		std::filesystem::remove_all(Path, Error);
	}
}

std::expected<void, FAssetError> FShaderScratch::Initialize(const FHash128& Key)
{
	std::error_code Error;
	const auto Parent = std::filesystem::temp_directory_path(Error);
	if (Error)
	{
		return std::unexpected(FAssetError{"Cannot resolve the shader snapshot directory"});
	}

	// Debug info embeds the snapshot's paths, so naming it by the source key keeps the SPIR-V identical across runs and lets the driver's pipeline cache hit.
	// Directories left by a crash or held by a concurrent compile take the next numbered name, which stays stable from run to run; a unique name is the last resort.
	constexpr int NumberedNames = 8;
	for (int Attempt = 0; Attempt <= NumberedNames; ++Attempt)
	{
		const std::string Suffix = Attempt == 0 ? ToString(Key) : Attempt < NumberedNames ? std::format("{}-{}", ToString(Key), Attempt) : FAssetId::Generate().ToString();
		const auto Candidate = Parent / ("HertaMaterialShader-" + Suffix);
		if (std::filesystem::create_directory(Candidate, Error) && !Error)
		{
			// Only an exclusively created child of the system temp directory is owned for recursive cleanup.
			Path = Candidate;
			return {};
		}
	}

	return std::unexpected(FAssetError{"Cannot create an exclusive shader snapshot directory"});
}

const std::filesystem::path& FShaderScratch::GetPath() const noexcept
{
	return Path;
}
}

std::expected<FMaterialShaderSources, FAssetError> CollectMaterialShaderSources(const std::span<const std::filesystem::path> Roots, const std::string_view MountedPath)
{
	if (MountedPath.empty())
	{
		auto Files = CollectShaderSources(Roots, 0, "TexturedMesh.slang");
		if (!Files)
		{
			return std::unexpected(Files.error());
		}

		return FMaterialShaderSources{.Root = 0, .Source = "TexturedMesh.slang", .Files = std::move(*Files)};
	}

	const std::size_t Root = MountedPath.starts_with("Engine/") ? 1 : 2;
	const std::string Relative(MountedPath.substr(MountedPath.find('/') + 1));
	if (Roots.size() <= Root || Roots[Root].empty())
	{
		return std::unexpected(FAssetError{"Shader content mount is unavailable"});
	}

	if (!Relative.ends_with(".hmatgraph"))
	{
		auto Files = CollectShaderSources(Roots, Root, Relative);
		if (!Files)
		{
			return std::unexpected(Files.error());
		}

		return FMaterialShaderSources{.Root = Root, .Source = Relative, .Files = std::move(*Files)};
	}

	const auto Bytes = ReadWholeFile(Roots[Root] / SourcePath(Relative), std::uint64_t{64} * 1024 * 1024);
	if (!Bytes || !*Bytes)
	{
		return std::unexpected(FAssetError{"Cannot read the Material Graph"});
	}

	const std::string_view Text(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size());
	const auto Graph = DeserializeGraph(Text, GetMaterialGraphSchema());
	if (!Graph)
	{
		return std::unexpected(FAssetError{"Material Graph: " + Graph.error().Message});
	}

	const auto Compiled = CompileMaterialGraph(*Graph);
	if (!Compiled)
	{
		std::string Message = "Material Graph:";
		for (const FGraphDiagnostic& Diagnostic : Compiled.error())
		{
			Message += " " + Diagnostic.Message + ".";
		}

		return std::unexpected(FAssetError{std::move(Message)});
	}

	auto Includes = CollectShaderSources(Roots, 0, "GraphMaterial.slangh");
	if (!Includes)
	{
		return std::unexpected(Includes.error());
	}

	FMaterialShaderSources Result{.Root = Root, .Source = Relative + ".slang", .Files = std::move(*Includes), .Parameters = Compiled->Parameters};
	const auto Generated = std::as_bytes(std::span(Compiled->Source));
	Result.Files.push_back({.Root = Root, .Path = Result.Source, .Bytes = {Generated.begin(), Generated.end()}});
	return Result;
}

FHash128 HashMaterialShaderSources(const FMaterialShaderSources& Sources)
{
	std::vector<std::byte> Identity;
	for (const FShaderSourceFile& File : Sources.Files)
	{
		const auto Path = std::as_bytes(std::span(File.Path));
		Identity.push_back(static_cast<std::byte>(File.Root));
		Identity.insert(Identity.end(), Path.begin(), Path.end());
		Identity.push_back(std::byte{0});
		Identity.insert(Identity.end(), File.Bytes.begin(), File.Bytes.end());
	}

	return HashBytes(Identity);
}

std::expected<FMaterialShaderBytes, FAssetError> CompileMaterialShader(const std::filesystem::path& Worker, const FMaterialShaderSources& Sources, const FHash128& Key, const bool bDebug, const std::function<bool()>& ShouldCancel)
{
	FShaderScratch Scratch;
	if (auto Created = Scratch.Initialize(Key); !Created)
	{
		return std::unexpected(Created.error());
	}

	constexpr std::size_t RootCount = 3;
	std::vector<std::filesystem::path> Copies;
	std::error_code Error;

	for (std::size_t Index = 0; Index < RootCount; ++Index)
	{
		Copies.push_back(Scratch.GetPath() / std::format("Root{}", Index));
		std::filesystem::create_directory(Copies.back(), Error);
		if (Error)
		{
			return std::unexpected(FAssetError{"Cannot create shader include snapshot roots"});
		}
	}

	for (const auto& File : Sources.Files)
	{
		if (ShouldCancel && ShouldCancel())
		{
			return std::unexpected(FAssetError{"Shader compilation cancelled"});
		}

		if (File.Root >= RootCount)
		{
			return std::unexpected(FAssetError{"A shader source lies outside the material shader roots"});
		}

		const auto Output = Copies[File.Root] / SourcePath(File.Path);
		std::filesystem::create_directories(Output.parent_path(), Error);
		if (Error)
		{
			return std::unexpected(FAssetError{"Cannot create shader snapshot subdirectories"});
		}

		std::ofstream Stream(Output, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(File.Bytes.data()), static_cast<std::streamsize>(File.Bytes.size()));
		Stream.close();
		if (!Stream)
		{
			return std::unexpected(FAssetError{"Cannot write an immutable shader source snapshot"});
		}
	}

	const auto Source = Copies[Sources.Root] / SourcePath(Sources.Source);
	const auto CompileStage = [&](const std::size_t Index) -> std::expected<std::vector<std::byte>, FAssetError>
	{
		const FStageEntry& Entry = StageEntries[Index];
		const auto Output = Scratch.GetPath() / std::format("Stage{}.hshader", Index);
		std::vector<std::string> Arguments{Utf8Path(Source), Entry.bFragment ? "fragment" : "vertex", std::string(Entry.EntryPoint), Utf8Path(Output)};
		if (bDebug)
		{
			Arguments.emplace_back("--debug");
		}

		for (const auto& Root : Copies)
		{
			Arguments.emplace_back("--include-root");
			Arguments.push_back(Utf8Path(Root));
		}

		const auto Process = RunProcess({
		    .Executable = Worker,
		    .Arguments = std::move(Arguments),
		    .Timeout = std::chrono::seconds(60),
		    .ShouldCancel = ShouldCancel,
		    .MaximumOutputBytes = 64 * 1024,
		});

		if (!Process)
		{
			return std::unexpected(FAssetError{Process.error().Message});
		}

		if (Process->ExitCode != 0)
		{
			return std::unexpected(FAssetError{std::format("{} failed: {}{}", Entry.EntryPoint, Process->StandardError, Process->StandardOutput)});
		}

		auto Bytes = ReadWholeFile(Output, std::uint64_t{256} * 1024 * 1024);
		if (!Bytes || !*Bytes)
		{
			return std::unexpected(FAssetError{std::format("{} produced no cooked shader", Entry.EntryPoint)});
		}

		return std::move(**Bytes);
	};

	// Each stage is a separate worker process that parses the whole shader, so they run side by side; the slowest stage sets the latency.
	std::array<std::expected<std::vector<std::byte>, FAssetError>, MaterialShaderStageCount> Compiled;
	{
		std::vector<std::jthread> Workers;
		Workers.reserve(MaterialShaderStageCount - 1);
		for (std::size_t Index = 1; Index < MaterialShaderStageCount; ++Index)
		{
			Workers.emplace_back([&Compiled, &CompileStage, Index]()
			{
				Compiled[Index] = CompileStage(Index);
			});
		}

		Compiled[0] = CompileStage(0);
	}

	FMaterialShaderBytes Result;
	for (std::size_t Index = 0; Index < MaterialShaderStageCount; ++Index)
	{
		if (!Compiled[Index])
		{
			return std::unexpected(Compiled[Index].error());
		}

		Result[Index] = std::move(*Compiled[Index]);
	}

	return Result;
}
}
