#include "MaterialShaders.h"

#include "Herta/AssetPipeline/MaterialShaderCompiler.h"
#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Core/BinaryStream.h"
#include "Herta/Core/Log.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Herta
{
namespace
{
constexpr FLogCategory ShaderLog{.Name = "Shaders"};
constexpr std::chrono::milliseconds PollInterval{500};
constexpr std::size_t MaximumWatchedShaders = 64;

std::string MountedPath(const std::string_view Path)
{
	return Path.empty() || Path.starts_with("Engine/") || Path.starts_with("Game/") ? std::string(Path) : "Game/" + std::string(Path);
}

bool IsMaterialShaderPath(const std::string_view Path)
{
	return Path.ends_with(".slang") || Path.ends_with(".hmatgraph");
}

}

struct FMaterialShaders::FSourceSnapshot
{
	FMaterialShaderSources Sources;
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

void FMaterialShaders::SetGraphOverride(const std::string_view Path, std::optional<std::string> GraphText)
{
	const std::string Key = MountedPath(Path);
	const auto Found = GraphOverrides.find(Key);
	if (GraphText ? Found != GraphOverrides.end() && Found->second == *GraphText : Found == GraphOverrides.end())
	{
		return;
	}

	if (GraphText)
	{
		GraphOverrides.insert_or_assign(Key, std::move(*GraphText));
	}
	else
	{
		GraphOverrides.erase(Found);
	}

	// Edits should reach the preview at once rather than at the next file poll.
	NextPoll = {};
}

void FMaterialShaders::Tick(const std::span<const std::string> MountedShaderPaths)
{
	std::set<std::string> Desired{""};

	for (const auto& Input : MountedShaderPaths)
	{
		const std::string Path = MountedPath(Input);
		// Static switch permutations share their graph's file, so validation looks at the path without the switch suffix.
		const auto Key = ParseMaterialShaderKey(Path);
		if ((Path.empty() || (Key && IsValidAssetPath(Key->first) && IsMaterialShaderPath(Key->first))) && Desired.size() < MaximumWatchedShaders)
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
	const auto Work = Tasks.Submit(*Scope, {.Name = "Watch material shader sources", .Lane = ETaskLane::BlockingIo}, [Results, Paths = Wanted, Directories = Roots, Overrides = GraphOverrides](FTaskContext& Context)
	{
		std::size_t CapturedBytes = 0;

		for (const auto& Path : Paths)
		{
			if (Context.IsCancellationRequested())
			{
				return;
			}

			const auto Override = Overrides.find(Path.substr(0, Path.find('#')));
			auto Collected = CollectMaterialShaderSources(Directories, Path, Override != Overrides.end() ? std::optional<std::string_view>(Override->second) : std::nullopt);
			if (!Collected)
			{
				Results->emplace(Path, std::unexpected(FShaderError{Collected.error().Message}));
				continue;
			}

			auto* const Files = &Collected->Files;

			std::vector<FShaderSourceDependency> Dependencies;
			Dependencies.reserve(Files->size());
			std::size_t SourceBytes = 0;

			for (const auto& File : *Files)
			{
				SourceBytes += File.Bytes.size();
				Dependencies.push_back({.Path = std::format("{}/{}", File.Root, File.Path), .ContentHash = HashShaderContent(File.Bytes)});
			}

			if (CapturedBytes + SourceBytes > std::size_t{64} * 1024 * 1024)
			{
				Results->emplace(Path, std::unexpected(FShaderError{"Watched material shader snapshots exceed the 64 MiB budget"}));
				continue;
			}

			CapturedBytes += SourceBytes;
			auto Snapshot = std::make_shared<FSourceSnapshot>();
			Snapshot->Sources = std::move(*Collected);
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
	using FCompileResult = std::expected<FMaterialShaderStages, FShaderError>;
	auto Result = std::make_shared<FCompileResult>(std::unexpected(FShaderError{"Shader compilation cancelled"}));
	Entry.ActiveStop = std::stop_source{};
	Entry.bInFlight = true;
	Entry.Status.bCompiling = true;
	Entry.Status.Diagnostics = "Compiling material shader";
	const auto Token = Entry.ActiveStop.get_token();
	const auto Snapshot = Entry.Desired;
	const auto Work = Tasks.Submit(*Scope, {.Name = "Compile material shader", .Lane = ETaskLane::BlockingIo}, [Result, Snapshot, Worker = WorkerPath, Token](FTaskContext& Context)
	{
		const FCancellationToken& ScopeToken = Context.GetCancellationToken();
		// The editor's live material pipelines replace the cooked ones, so they carry source for profilers too.
		auto Stages = CompileMaterialShader(Worker, Snapshot->Sources, Snapshot->Key, true, [Token, ScopeToken]()
		{
			return Token.stop_requested() || ScopeToken.IsCancellationRequested();
		});

		if (!Stages)
		{
			*Result = std::unexpected(FShaderError{Stages.error().Message});
			return;
		}

		FMaterialShaderStages Compiled;
		for (std::size_t Index = 0; Index < Compiled.size(); ++Index)
		{
			auto Shader = DeserializeCookedShader((*Stages)[Index]);
			if (!Shader)
			{
				*Result = std::unexpected(Shader.error());
				return;
			}

			Compiled[Index] = std::move(*Shader);
		}

		*Result = std::move(Compiled);
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

		const auto Installed = Renderer.PublishMaterialShader(Path, std::move(**Result), Current.Desired->Sources.Parameters);
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
		Current.Status.Parameters = Current.Desired->Sources.Parameters;
		Current.Status.Switches = Current.Desired->Sources.Switches;
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
