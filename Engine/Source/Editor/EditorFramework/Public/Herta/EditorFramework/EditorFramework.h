#pragma once

#include "Herta/EditorCore/Automation.h"
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
class FApplication;
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

using FEditorAssetThumbnail = std::shared_ptr<const std::uint64_t>;
using FEditorAssetThumbnailRenderer = std::function<std::expected<FEditorAssetThumbnail, FPresentationError>(const FRenderMesh&)>;
// Zoom 1 frames the preview mesh; larger values move the camera closer.
using FEditorMaterialThumbnailRenderer = std::function<std::expected<FEditorAssetThumbnail, FPresentationError>(const FRenderMesh&, const FRenderMaterial&, float Zoom)>;

struct FEditorFrameworkDescriptor
{
	FLogService* Log = nullptr;
	FEditorCommandRegistry* Commands = nullptr;
	FToolUIContext* ToolUI = nullptr;
	// Play reads gamepads through it; without one, Play takes keyboard and mouse only.
	FApplication* Application = nullptr;
	// Asset previews are disabled unless all of these are provided. The task system and device must outlive the framework.
	FTaskSystem* Tasks = nullptr;
	IGraphicsDevice* GraphicsDevice = nullptr;
	FEditorAssetPaths Assets;
	// Empty disables automatic level loading and the default save path.
	std::filesystem::path LevelPath;
	std::filesystem::path EngineRoot{};
	std::filesystem::path ProjectPath{};
	// Called between frames; the shared handle owns the registered UI texture.
	FEditorAssetThumbnailRenderer RenderAssetThumbnail;
	FEditorMaterialThumbnailRenderer RenderMaterialThumbnail;
	FMeshRenderer* MeshRenderer = nullptr;
	// Make a texture drawable in the editor UI and release it again, such as the game HUD's glyphs during Play.
	std::function<std::expected<std::uint64_t, FPresentationError>(const FTextureHandle& Texture)> RegisterUITexture{};
	std::function<void(std::uint64_t TextureId)> UnregisterUITexture{};
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

// What the host's presentation reports for HDR output, refreshed before each Draw.
struct FEditorDisplayState
{
	bool bHdrSupported = false;
	bool bHdrActive = false;
	float PaperWhite = 200.f;
	float PeakLuminance = 1000.f;
	// What the operating system reports, or zero when unknown.
	float SystemPaperWhite = 0.f;
	float SystemPeakLuminance = 0.f;
};

// The part of the viewport render target the viewport panel shows. The render target also covers the editor's glass panels, which only
// decorate, so this is where the viewport's perceived center and size come from.
struct FViewportVisibleRegion
{
	std::uint32_t X = 0;
	std::uint32_t Y = 0;
	FExtent2D Extent{};
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
	bool RequestClose();
	bool HasConfirmedClose() const noexcept;
	void SetViewportImage(std::uint64_t TextureId) noexcept;
	void SetDisplayState(const FEditorDisplayState& State) noexcept;
	void SetFrameTimings(double CpuMilliseconds, std::optional<double> GpuUIMilliseconds) noexcept;
	FEditorFrameMetrics GetFrameMetrics() const noexcept;
	// Bounded diagnostic phases reuse the same selection, camera, and simulation paths as interactive editing.
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SetScalingTestPhase(bool bSelectAll, bool bSimulate);
	// Starts or stops play for unattended checks. Scripted play walks and jumps on its own, without reading the keyboard or capturing the pointer.
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SetScriptedPlay(bool bPlaying);
	[[nodiscard]] bool IsPlaying() const noexcept;
	// Opens or closes the game's pause menu during play, as Shift+Esc does.
	void SetPlayPaused(bool bPaused) noexcept;
	void SetRenderQuality(ERenderQuality Quality) noexcept;
	void SetPathTracing(bool bEnabled) noexcept;
	// Replaces auto exposure with a fixed EV100, set through the physical camera's shutter, so captures compare without metering differences.
	void SetManualExposure(float ExposureValue) noexcept;
	// Moves the viewport camera along its view direction, as flying forward does.
	void MoveViewportCamera(float Distance) noexcept;
	// Renders the viewport at a fixed size, shown scaled to the panel, so GPU timings compare across displays. Unset follows the panel.
	void SetViewportResolution(std::optional<FExtent2D> Resolution) noexcept;
	// Opens a Material Graph from the project's or the engine's content in the graph editor, as passing its file to the editor does.
	void OpenMaterialGraph(const std::filesystem::path& File);
	void OpenAnimationGraph(const std::filesystem::path& File);
	void OpenAudioGraph(const std::filesystem::path& File);
	// Opens a material asset from the project's or the engine's content in the material editor once the content scan has registered it.
	void OpenMaterial(const std::filesystem::path& File);
	// Moves the viewport camera to a level camera bookmark and returns its name, or nothing when the slot is empty.
	std::optional<std::string> ShowCameraBookmark(std::uint32_t Slot);
	[[nodiscard]] bool IsUnitStatsVisible() const noexcept;
	[[nodiscard]] FExtent2D GetViewportExtent() const noexcept;
	[[nodiscard]] FViewportVisibleRegion GetViewportVisibleRegion() const noexcept;
	[[nodiscard]] FMeshRenderView GetViewportRenderView() const noexcept;
	[[nodiscard]] std::span<const FDebugDrawList> GetViewportDebugDrawLists() const noexcept;
	[[nodiscard]] FOutputLogModel& GetOutputLog() noexcept;
	[[nodiscard]] const FOutputLogModel& GetOutputLog() const noexcept;
	// Answers automation calls about the editor, the level, Play, and the log. Returns false for tools it does not know, so the host can answer them.
	bool HandleAutomation(FAutomationServer& Server, const FAutomationRequest& Request);
	// Finishes calls that take several frames, such as starting Play or scripted Play input. Call once a frame.
	void UpdateAutomation(FAutomationServer& Server);

private:
	explicit FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
