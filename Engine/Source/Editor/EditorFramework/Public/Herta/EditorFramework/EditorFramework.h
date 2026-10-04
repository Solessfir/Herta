#pragma once

#include "Herta/EditorFramework/OutputLog.h"
#include "Herta/RHI/Presentation.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Herta
{
class FEditorCommandRegistry;
class FLogService;
class FTaskSystem;
class FToolUIContext;

struct FEditorFrameworkError
{
	std::string Message;
};

struct FEditorAssetPaths
{
	std::filesystem::path EngineContentRoot;
	std::filesystem::path ContentRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
};

struct FEditorFrameworkDescriptor
{
	FLogService* Log = nullptr;
	FEditorCommandRegistry* Commands = nullptr;
	FToolUIContext* ToolUI = nullptr;
	// Asset previews are disabled unless all of these are provided. The task system and device must outlive the framework.
	FTaskSystem* Tasks = nullptr;
	IGraphicsDevice* GraphicsDevice = nullptr;
	FEditorAssetPaths Assets;
	// Empty disables automatic scene loading and the default save path.
	std::filesystem::path ScenePath;
};

struct FEditorFrameMetrics
{
	double InspectorMilliseconds = 0.;
	double ExtractionMilliseconds = 0.;
	double SimulationMilliseconds = 0.;
	std::size_t ObjectCount = 0;
	std::size_t SelectedCount = 0;
	bool bAssetsReady = false;
	bool bSimulationRunning = false;
};

class FEditorFramework final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> Create(const FEditorFrameworkDescriptor& Descriptor);

	~FEditorFramework();

	FEditorFramework(const FEditorFramework&) = delete;
	FEditorFramework& operator=(const FEditorFramework&) = delete;
	FEditorFramework(FEditorFramework&&) = delete;
	FEditorFramework& operator=(FEditorFramework&&) = delete;

	// RenderViewport runs after viewport layout/input and must set the image before it is queued for display.
	[[nodiscard]] std::expected<void, FEditorFrameworkError> Draw(const std::function<void()>& RenderViewport);
	void SetViewportImage(std::uint64_t TextureId) noexcept;
	void SetFrameTimings(double CpuMilliseconds, std::optional<double> GpuUIMilliseconds) noexcept;
	FEditorFrameMetrics GetFrameMetrics() const noexcept;
	// Bounded diagnostic phases reuse the same selection, camera, and simulation paths as interactive editing.
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SetScalingTestPhase(bool bSelectAll, bool bSimulate);
	[[nodiscard]] bool IsUnitStatsVisible() const noexcept;
	[[nodiscard]] FExtent2D GetViewportExtent() const noexcept;
	[[nodiscard]] FMeshRenderView GetViewportRenderView() const noexcept;
	[[nodiscard]] std::span<const FDebugDrawList> GetViewportDebugDrawLists() const noexcept;
	[[nodiscard]] FOutputLogModel& GetOutputLog() noexcept;
	[[nodiscard]] const FOutputLogModel& GetOutputLog() const noexcept;

private:
	explicit FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
