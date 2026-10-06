#include "MaterialShaders.h"

#include "Herta/AssetPipeline/ShaderSources.h"
#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Core/BinaryStream.h"
#include "Herta/Core/Log.h"
#include "Herta/Platform/Process.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <utility>

namespace Herta
{
namespace
{
constexpr FLogCategory ShaderLog{.Name = "Shaders"};
constexpr std::chrono::milliseconds PollInterval{500};
constexpr std::size_t MaximumWatchedShaders = 64;

std::string Utf8Path(const std::filesystem::path& Path)
{
	const auto Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

std::filesystem::path SourcePath(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::string MountedPath(const std::string_view Path)
{
	return Path.empty() || Path.starts_with("Engine/") || Path.starts_with("Game/") ? std::string(Path) : "Game/" + std::string(Path);
}

std::pair<std::size_t, std::string> ResolveSource(const std::string_view Path)
{
	if (Path.empty())
	{
		return {0, "TexturedMesh.slang"};
	}

	return {Path.starts_with("Engine/") ? 1 : 2, std::string(Path.substr(Path.find('/') + 1))};
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

	[[nodiscard]] std::expected<void, FShaderError> Initialize();
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

std::expected<void, FShaderError> FShaderScratch::Initialize()
{
	std::error_code Error;
	const auto Parent = std::filesystem::temp_directory_path(Error);
	if (Error)
	{
		return std::unexpected(FShaderError{"Cannot resolve the shader snapshot directory"});
	}

	const auto Candidate = Parent / ("HertaMaterialShader-" + FAssetId::Generate().ToString());
	if (!std::filesystem::create_directory(Candidate, Error) || Error)
	{
		return std::unexpected(FShaderError{"Cannot create an exclusive shader snapshot directory"});
	}

	// Only an exclusively created, UUID-named child of the system temp directory is owned for recursive cleanup.
	Path = Candidate;
	return {};
}

const std::filesystem::path& FShaderScratch::GetPath() const noexcept
{
	return Path;
}

struct FCompiledMaterialShader
{
	FShaderAsset Vertex;
	FShaderAsset Instanced;
	FShaderAsset Fragment;
};

std::expected<FCompiledMaterialShader, FShaderError> CompileSnapshot(const std::filesystem::path& Worker, const std::vector<FShaderSourceFile>& Files, const std::size_t RootIndex, const std::string& RelativeSource, const std::size_t RootCount, const std::stop_token StopToken, const FCancellationToken ScopeToken)
{
	FShaderScratch Scratch;
	if (auto Created = Scratch.Initialize(); !Created)
	{
		return std::unexpected(Created.error());
	}

	std::vector<std::filesystem::path> Copies;
	std::error_code Error;

	for (std::size_t Index = 0; Index < RootCount; ++Index)
	{
		Copies.push_back(Scratch.GetPath() / std::format("Root{}", Index));
		std::filesystem::create_directory(Copies.back(), Error);
		if (Error)
		{
			return std::unexpected(FShaderError{"Cannot create shader include snapshot roots"});
		}
	}

	for (const auto& File : Files)
	{
		if (StopToken.stop_requested() || ScopeToken.IsCancellationRequested())
		{
			return std::unexpected(FShaderError{"Shader compilation cancelled"});
		}

		const auto Output = Copies[File.Root] / SourcePath(File.Path);
		std::filesystem::create_directories(Output.parent_path(), Error);
		if (Error)
		{
			return std::unexpected(FShaderError{"Cannot create shader snapshot subdirectories"});
		}

		std::ofstream Stream(Output, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(File.Bytes.data()), static_cast<std::streamsize>(File.Bytes.size()));
		Stream.close();
		if (!Stream)
		{
			return std::unexpected(FShaderError{"Cannot write an immutable shader source snapshot"});
		}
	}

	const auto Source = Copies[RootIndex] / SourcePath(RelativeSource);
	FCompiledMaterialShader Result;
	constexpr std::array<std::string_view, 3> EntryPoints{"vertexMain", "instancedVertexMain", "fragmentMain"};
	const std::array<FShaderAsset*, 3> Stages{&Result.Vertex, &Result.Instanced, &Result.Fragment};

	for (std::size_t Index = 0; Index < EntryPoints.size(); ++Index)
	{
		const auto Output = Scratch.GetPath() / std::format("Stage{}.hshader", Index);
		std::vector<std::string> Arguments{Utf8Path(Source), Index == 2 ? "fragment" : "vertex", std::string(EntryPoints[Index]), Utf8Path(Output)};

		for (const auto& Root : Copies)
		{
			Arguments.emplace_back("--include-root");
			Arguments.push_back(Utf8Path(Root));
		}

		const auto Process = RunProcess({
		    .Executable = Worker,
		    .Arguments = std::move(Arguments),
		    .Timeout = std::chrono::seconds(60),
		    .ShouldCancel = [StopToken, ScopeToken]()
		{
			return StopToken.stop_requested() || ScopeToken.IsCancellationRequested();
		},
		    .MaximumOutputBytes = 64 * 1024,
		});

		if (!Process)
		{
			return std::unexpected(FShaderError{Process.error().Message});
		}

		if (Process->ExitCode != 0)
		{
			return std::unexpected(FShaderError{std::format("{} failed: {}{}", EntryPoints[Index], Process->StandardError, Process->StandardOutput)});
		}

		auto Shader = LoadCookedShader(Output);
		if (!Shader)
		{
			return std::unexpected(Shader.error());
		}

		*Stages[Index] = std::move(*Shader);
	}

	return Result;
}
}

struct FMaterialShaders::FSourceSnapshot
{
	std::size_t Root = 0;
	std::string Source;
	std::vector<FShaderSourceFile> Files;
	FHash128 Key;
};

FHash128 ComputeMaterialShaderSourceKey(const std::span<const FShaderSourceDependency> Dependencies)
{
	std::vector<FShaderSourceDependency> Sorted(Dependencies.begin(), Dependencies.end());
	std::ranges::sort(Sorted, [](const FShaderSourceDependency& A, const FShaderSourceDependency& B)
	{
		return A.Path != B.Path ? A.Path < B.Path : A.ContentHash < B.ContentHash;
	});
	FBinaryWriter Writer;
	Writer.Write(static_cast<std::uint32_t>(Sorted.size()));

	for (const auto& Dependency : Sorted)
	{
		Writer.WriteString(Dependency.Path);
		Writer.Write(Dependency.ContentHash);
	}

	return HashBytes(Writer.GetBytes());
}

FMaterialShaders::FMaterialShaders(FTaskSystem& InTasks, FMeshRenderer& InRenderer, FLogService& InLog, std::vector<std::filesystem::path> InRoots, std::filesystem::path InWorkerPath, std::unique_ptr<FTaskScope> InScope)
    : Tasks(InTasks)
    , Renderer(InRenderer)
    , Log(InLog)
    , Roots(std::move(InRoots))
    , WorkerPath(std::move(InWorkerPath))
    , Scope(std::move(InScope))
{
}

std::unique_ptr<FMaterialShaders> FMaterialShaders::Create(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log, std::filesystem::path EngineShaderRoot, std::filesystem::path ShaderWorkerPath, std::filesystem::path EngineContentRoot, std::filesystem::path GameContentRoot)
{
	auto Scope = Tasks.CreateScope("Material shader iteration");
	if (!Scope)
	{
		HERTA_LOG_ERROR(Log, ShaderLog, "Material shader iteration is unavailable: {}", Scope.error().Message);
		return nullptr;
	}

	std::vector<std::filesystem::path> Roots{std::move(EngineShaderRoot), std::move(EngineContentRoot), std::move(GameContentRoot)};
	return std::unique_ptr<FMaterialShaders>(new FMaterialShaders(Tasks, Renderer, Log, std::move(Roots), std::move(ShaderWorkerPath), std::move(*Scope)));
}

FMaterialShaders::~FMaterialShaders()
{
	for (auto& [Path, Entry] : Entries)
	{
		Entry.ActiveStop.request_stop();
	}

	Scope->RequestCancellation();
	Scope->Wait();
}

const FMaterialShaderStatus* FMaterialShaders::GetStatus(const std::string_view Path) const
{
	const auto Found = Entries.find(MountedPath(Path));
	return Found == Entries.end() ? nullptr : &Found->second.Status;
}

void FMaterialShaders::Tick(const std::span<const std::string> MountedShaderPaths)
{
	std::set<std::string> Desired{""};

	for (const auto& Input : MountedShaderPaths)
	{
		const std::string Path = MountedPath(Input);
		if ((Path.empty() || (IsValidAssetPath(Path) && Path.ends_with(".slang"))) && Desired.size() < MaximumWatchedShaders)
		{
			Desired.insert(Path);
		}
	}

	if (Desired != Wanted)
	{
		Wanted = std::move(Desired);
		NextPoll = {};

		for (const auto& Path : Wanted)
		{
			const auto [Iterator, bInserted] = Entries.try_emplace(Path);
			if (bInserted && Path.empty())
			{
				Iterator->second.Status.bReady = true;
			}
		}

		for (auto& [Path, Entry] : Entries)
		{
			if (!Wanted.contains(Path))
			{
				Entry.ActiveStop.request_stop();
			}
		}
	}

	std::erase_if(Entries, [&](const auto& Pair)
	{
		return !Wanted.contains(Pair.first) && !Pair.second.bInFlight;
	});

	for (auto& [Path, Entry] : Entries)
	{
		if (!Wanted.contains(Path) || !Entry.Desired || Entry.bInFlight)
		{
			continue;
		}

		if (Entry.PublishedKey == Entry.Desired->Key)
		{
			Entry.Status.Diagnostics = "Material shader ready";
		}
		else if (Entry.FailedKey == Entry.Desired->Key)
		{
			Entry.Status.Diagnostics = Entry.FailedDiagnostics;
		}
		else
		{
			Start(Path, Entry);
		}
	}

	const auto Now = std::chrono::steady_clock::now();
	if (!bPolling && Now >= NextPoll)
	{
		NextPoll = Now + PollInterval;
		Poll();
	}
}

void FMaterialShaders::Poll()
{
	using FSnapshotResult = std::expected<std::shared_ptr<const FSourceSnapshot>, FShaderError>;
	auto Results = std::make_shared<std::map<std::string, FSnapshotResult>>();
	bPolling = true;
	const auto Work = Tasks.Submit(*Scope, {.Name = "Watch material shader sources", .Lane = ETaskLane::BlockingIo}, [Results, Paths = Wanted, Directories = Roots](FTaskContext& Context)
	{
		std::size_t CapturedBytes = 0;

		for (const auto& Path : Paths)
		{
			if (Context.IsCancellationRequested())
			{
				return;
			}

			const auto [Root, Relative] = ResolveSource(Path);
			auto Files = Directories[Root].empty() ? std::expected<std::vector<FShaderSourceFile>, FAssetError>(std::unexpected(FAssetError{"Shader content mount is unavailable"})) : CollectShaderSources(Directories, Root, Relative);
			if (!Files)
			{
				Results->emplace(Path, std::unexpected(FShaderError{Files.error().Message}));
				continue;
			}

			std::vector<FShaderSourceDependency> Dependencies;
			Dependencies.reserve(Files->size());
			std::size_t SourceBytes = 0;

			for (const auto& File : *Files)
			{
				SourceBytes += File.Bytes.size();
				Dependencies.push_back({.Path = std::format("{}/{}", File.Root, File.Path), .ContentHash = HashShaderContent(File.Bytes)});
			}

			if (CapturedBytes + SourceBytes > 64 * 1024 * 1024)
			{
				Results->emplace(Path, std::unexpected(FShaderError{"Watched material shader snapshots exceed the 64 MiB budget"}));
				continue;
			}

			CapturedBytes += SourceBytes;
			auto Snapshot = std::make_shared<FSourceSnapshot>();
			Snapshot->Root = Root;
			Snapshot->Source = Relative;
			Snapshot->Files = std::move(*Files);
			Snapshot->Key = ComputeMaterialShaderSourceKey(Dependencies);
			Results->emplace(Path, std::move(Snapshot));
		}
	});

	const auto Published = Work ? Tasks.ContinueOnMainThread(*Scope, *Work, "Publish material shader source changes", [this, Results](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		bPolling = false;

		for (const auto& [Path, Result] : *Results)
		{
			if (!Wanted.contains(Path))
			{
				continue;
			}

			auto& Entry = Entries.at(Path);
			if (!Result)
			{
				Entry.ActiveStop.request_stop();
				Entry.Desired.reset();
				if (Entry.Status.Diagnostics != Result.error().Message)
				{
					Entry.Status.Diagnostics = Result.error().Message;
					HERTA_LOG_WARNING(Log, ShaderLog, "Material shader '{}' retained its last valid pipeline: {}", Path.empty() ? "Engine default" : Path, Entry.Status.Diagnostics);
				}

				continue;
			}

			if (!Entry.Desired || Entry.Desired->Key != (*Result)->Key)
			{
				++Entry.Status.Generation;
				Entry.Desired = *Result;
				Entry.ActiveStop.request_stop();
			}
		}
	})
	                            : std::expected<FTaskHandle, FTaskError>(std::unexpected(Work.error()));

	if (!Published)
	{
		bPolling = false;
		HERTA_LOG_WARNING(Log, ShaderLog, "Material shader source poll could not be scheduled: {}", Published.error().Message);
	}
}

void FMaterialShaders::Start(const std::string& Path, FEntry& Entry)
{
	using FCompileResult = std::expected<FCompiledMaterialShader, FShaderError>;
	auto Result = std::make_shared<FCompileResult>(std::unexpected(FShaderError{"Shader compilation cancelled"}));
	Entry.ActiveStop = std::stop_source{};
	Entry.bInFlight = true;
	Entry.Status.bCompiling = true;
	Entry.Status.Diagnostics = "Compiling material shader";
	const auto Token = Entry.ActiveStop.get_token();
	const auto Snapshot = Entry.Desired;
	const auto Work = Tasks.Submit(*Scope, {.Name = "Compile material shader", .Lane = ETaskLane::BlockingIo}, [Result, Snapshot, Worker = WorkerPath, RootCount = Roots.size(), Token](FTaskContext& Context)
	{
		*Result = CompileSnapshot(Worker, Snapshot->Files, Snapshot->Root, Snapshot->Source, RootCount, Token, Context.GetCancellationToken());
	});

	const auto Published = Work ? Tasks.ContinueOnMainThread(*Scope, *Work, "Publish material shader pipeline", [this, Path, Result, Key = Snapshot->Key, Generation = Entry.Status.Generation](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		auto& Current = Entries.at(Path);
		Current.bInFlight = false;
		Current.Status.bCompiling = false;
		if (!Wanted.contains(Path) || !Current.Desired || Current.Desired->Key != Key || Current.Status.Generation != Generation)
		{
			return;
		}

		if (!*Result)
		{
			Current.FailedKey = Key;
			Current.Status.Diagnostics = Result->error().Message;
			Current.FailedDiagnostics = Current.Status.Diagnostics;
			HERTA_LOG_WARNING(Log, ShaderLog, "Material shader '{}' retained its last valid pipeline: {}", Path.empty() ? "Engine default" : Path, Current.Status.Diagnostics);
			return;
		}

		const auto Installed = Renderer.PublishMaterialShader(Path, std::move((*Result)->Vertex), std::move((*Result)->Instanced), std::move((*Result)->Fragment));
		if (!Installed)
		{
			Current.FailedKey = Key;
			Current.Status.Diagnostics = Installed.error().Message;
			Current.FailedDiagnostics = Current.Status.Diagnostics;
			HERTA_LOG_WARNING(Log, ShaderLog, "Material shader '{}' is incompatible; retained its last valid pipeline: {}", Path.empty() ? "Engine default" : Path, Current.Status.Diagnostics);
			return;
		}

		Current.PublishedKey = Key;
		Current.FailedKey.reset();
		Current.Status.bReady = true;
		Current.Status.Diagnostics = "Material shader ready";
	})
	                            : std::expected<FTaskHandle, FTaskError>(std::unexpected(Work.error()));

	if (!Published)
	{
		Entry.ActiveStop.request_stop();
		Entry.bInFlight = false;
		Entry.Status.bCompiling = false;
		Entry.FailedKey = Snapshot->Key;
		Entry.Status.Diagnostics = Published.error().Message;
		Entry.FailedDiagnostics = Entry.Status.Diagnostics;
		HERTA_LOG_WARNING(Log, ShaderLog, "Material shader compilation could not be scheduled: {}", Entry.Status.Diagnostics);
	}
}
}
