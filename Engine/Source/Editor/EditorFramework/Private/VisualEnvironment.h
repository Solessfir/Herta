#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Core/Hash.h"
#include "Herta/Renderer/Visuals.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace Herta
{
class FLogService;
class FMeshRenderer;
class FTaskScope;
class FTaskSystem;

struct FVisualEnvironmentStatus
{
	bool bUpdating = false;
	bool bReady = false;
	std::uint64_t Generation = 0;
	std::string Message;
	double FilterMilliseconds = 0.;
	double UploadMilliseconds = 0.;
	std::size_t TextureBytes = 0;
};

// Only radiance inputs affect the key. Camera, fog, material, and environment visibility do not.
FHash128 GetVisualEnvironmentKey(const FCookedTexture* Environment, const FVisualUniforms& Snapshot);

class FVisualEnvironment final
{
public:
	[[nodiscard]] static std::unique_ptr<FVisualEnvironment> Create(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log);
	~FVisualEnvironment();
	FVisualEnvironment(const FVisualEnvironment&) = delete;
	FVisualEnvironment& operator=(const FVisualEnvironment&) = delete;
	FVisualEnvironment(FVisualEnvironment&&) = delete;
	FVisualEnvironment& operator=(FVisualEnvironment&&) = delete;

	void Tick(std::shared_ptr<const FCookedTexture> Environment, const FVisualUniforms& Snapshot);
	const FVisualEnvironmentStatus& GetStatus() const noexcept;

private:
	struct FInputs
	{
		std::shared_ptr<const FCookedTexture> Environment;
		FVisualUniforms Snapshot;
		FHash128 Key;
		std::uint64_t Generation = 0;
	};

	FVisualEnvironment(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log, std::unique_ptr<FTaskScope> Scope);
	void Start(const FInputs& Inputs);

	FTaskSystem& Tasks;
	FMeshRenderer& Renderer;
	FLogService& Log;
	// Continuations capture this. Cancellation and draining precede destruction of the remaining state.
	std::unique_ptr<FTaskScope> Scope;

	std::shared_ptr<const FCookedTexture> CachedSource;
	FHash128 CachedSourceKey;
	std::optional<FInputs> Desired;
	std::optional<FHash128> PublishedKey;
	std::optional<FHash128> FailedKey;
	std::string FailedMessage;
	std::stop_source ActiveStop;
	bool bInFlight = false;
	FVisualEnvironmentStatus Status;
};
}
