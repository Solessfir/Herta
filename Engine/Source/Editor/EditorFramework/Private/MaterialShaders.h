#pragma once

#include "Herta/Core/Hash.h"
#include "Herta/RHI/CookedShader.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FLogService;
class FMeshRenderer;
class FTaskScope;
class FTaskSystem;

struct FMaterialShaderStatus
{
	bool bCompiling = false;
	bool bReady = false;
	std::uint64_t Generation = 0;
	std::string Diagnostics;
};

FHash128 ComputeMaterialShaderSourceKey(std::span<const FShaderSourceDependency> Dependencies);

class FMaterialShaders final
{
public:
	[[nodiscard]] static std::unique_ptr<FMaterialShaders> Create(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log, std::filesystem::path EngineShaderRoot, std::filesystem::path ShaderWorkerPath, std::filesystem::path EngineContentRoot, std::filesystem::path GameContentRoot);
	~FMaterialShaders();
	FMaterialShaders(const FMaterialShaders&) = delete;
	FMaterialShaders& operator=(const FMaterialShaders&) = delete;
	FMaterialShaders(FMaterialShaders&&) = delete;
	FMaterialShaders& operator=(FMaterialShaders&&) = delete;

	void Tick(std::span<const std::string> MountedShaderPaths);
	const FMaterialShaderStatus* GetStatus(std::string_view Path) const;

private:
	friend struct FMaterialShadersTestAccess;

	struct FSourceSnapshot;

	struct FEntry
	{
		FMaterialShaderStatus Status;
		std::shared_ptr<const FSourceSnapshot> Desired;
		std::optional<FHash128> PublishedKey;
		std::optional<FHash128> FailedKey;
		std::string FailedDiagnostics;
		std::stop_source ActiveStop;
		bool bInFlight = false;
	};

	FMaterialShaders(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log, std::vector<std::filesystem::path> Roots, std::filesystem::path WorkerPath, std::unique_ptr<FTaskScope> Scope);
	void Poll();
	void Start(const std::string& Path, FEntry& Entry);

	FTaskSystem& Tasks;
	FMeshRenderer& Renderer;
	FLogService& Log;
	std::vector<std::filesystem::path> Roots;
	std::filesystem::path WorkerPath;
	// The destructor cancels and drains continuations before destroying their captured state.
	std::unique_ptr<FTaskScope> Scope;

	std::map<std::string, FEntry, std::less<>> Entries;
	std::set<std::string> Wanted;
	std::chrono::steady_clock::time_point NextPoll{};
	bool bPolling = false;
};
}
