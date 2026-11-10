#pragma once

// Shared by the EditorFramework source files, which split FEditorFramework::FImplementation by area so one edit recompiles one area.

#include "ConsoleInput.h"
#include "ContentBrowser.h"
#include "DetailsPanel.h"
#include "EditorLevel.h"
#include "Herta/Application/Application.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Audio/AudioEngine.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Json.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/LevelCommands.h"
#include "Herta/EditorCore/PreviewSelection.h"
#include "Herta/EditorCore/ProjectCommands.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/EditorFramework/ViewportInteraction.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameHud.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/LevelSimulation.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/GameFramework/VisualEnvironment.h"
#include "Herta/AnimationGraph/AnimationGraph.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/MaterialGraph/MaterialGraph.h"
#include "Herta/Platform/FileDialog.h"
#include "Herta/Platform/Process.h"
#include "Herta/Project/Project.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"
#include "GraphDocumentPanel.h"
#include "MaterialPanel.h"
#include "MaterialShaders.h"
#include "NumericField.h"
#include "OutlinerPanel.h"
#include "OutputLogTextLayout.h"
#include "PerformancePanel.h"
#include "PlaceObjectsMenu.h"
#include "PreviewAssets.h"
#include "PreviewLevel.h"
#include "PreviewVisuals.h"
#include "TimeOfDay.h"
#include "ViewportBoxSelection.h"
#include "ViewportGizmos.h"
#include "ViewportIsland.h"
#include "ViewportRotationFeedback.h"
#include "ViewportScaleGizmo.h"
#include "ViewportStats.h"

#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Herta
{
inline constexpr FLogCategory EditorLog{.Name = "Editor"};

inline constexpr FLogCategory ShellLog{.Name = "Shell"};

inline std::string MaterialShaderKey(const std::string_view Path, const std::string_view MaterialPath)
{
	if (Path.empty() || Path.starts_with("Engine/") || Path.starts_with("Game/"))
	{
		return std::string(Path);
	}

	return std::format("{}/{}", MaterialPath.starts_with("Engine/") ? "Engine" : "Game", Path);
}

enum class EAuthoringAction : std::uint8_t
{
	None,
	Undo,
	Redo,
	Create,
	CreateEmpty,
	SelectAll,
	Copy,
	Paste,
	Duplicate,
	Delete
};

// Runs Output Log "!" lines through the user's shell on a blocking-IO task and logs the output when the command exits.
// ponytail: output arrives at exit and stdin is empty, so interactive programs fail; the Milestone 15 terminal plugin covers those.
class FShellRunner final
{
public:
	FShellRunner(FTaskSystem& InTasks, FLogService& InLog, std::unique_ptr<FTaskScope> InScope)
	    : Tasks(InTasks)
	    , Log(InLog)
	    , Scope(std::move(InScope))
	{
	}

	FShellRunner(const FShellRunner&) = delete;
	FShellRunner& operator=(const FShellRunner&) = delete;
	FShellRunner(FShellRunner&&) = delete;
	FShellRunner& operator=(FShellRunner&&) = delete;

	~FShellRunner()
	{
		// Cancellation kills each running shell and its children.
		Scope->RequestCancellation();
		Scope->Wait();
	}

	void Run(std::string Command)
	{
		std::expected<FTaskHandle, FTaskError> Task = Tasks.Submit(*Scope, {.Name = "Shell command", .Lane = ETaskLane::BlockingIo}, [&Log = Log, Command = std::move(Command)](FTaskContext& Context)
		{
			FProcessRequest Request = MakeShellRequest(Command);
			Request.Timeout = std::chrono::minutes(10);

			Request.ShouldCancel = [&Context]
			{
				return Context.IsCancellationRequested();
			};

			const std::expected<FProcessResult, FProcessError> Result = RunProcess(Request);
			if (!Result)
			{
				Log.LogText(ShellLog, ELogLevel::Warning, std::format("'{}' did not finish: {}", Command, Result.error().Message));
				return;
			}

			const auto LogLines = [&](const std::string& Text, const ELogLevel Level)
			{
				for (const auto Part : std::views::split(std::string_view(Text), '\n'))
				{
					std::string_view Line(Part.begin(), Part.end());
					if (Line.ends_with('\r'))
					{
						Line.remove_suffix(1);
					}

					if (!Line.empty())
					{
						Log.LogText(ShellLog, Level, Line);
					}
				}
			};

			// Many tools, git included, write progress to stderr, so it only counts as a warning when the command fails.
			LogLines(Result->StandardOutput, ELogLevel::Info);
			LogLines(Result->StandardError, Result->ExitCode == 0 ? ELogLevel::Info : ELogLevel::Warning);

			if (Result->ExitCode != 0)
			{
				Log.LogText(ShellLog, ELogLevel::Warning, std::format("'{}' exited with code {}", Command, Result->ExitCode));
			}
		});

		if (!Task)
		{
			Log.LogText(ShellLog, ELogLevel::Warning, std::format("Could not run '{}': {}", Command, Task.error().Message));
		}
	}

private:
	FTaskSystem& Tasks;
	FLogService& Log;
	std::unique_ptr<FTaskScope> Scope;
};

[[nodiscard]] inline ImU32 PackColor(const FToolUIColor Color) noexcept
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

// Draws a key as a small rounded chip centered on the frame row and returns its width; measures only when bDraw is false.
inline float DrawKeycap(const std::string_view Key, const bool bDraw = true)
{
	const float PaddingX = std::round(ImGui::GetFontSize() * 0.35f);
	const ImVec2 TextSize = ImGui::CalcTextSize(Key.data(), Key.data() + Key.size());
	const float Width = TextSize.x + PaddingX * 2.f;

	if (!bDraw)
	{
		return Width;
	}

	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const float Height = std::round(ImGui::GetFontSize() * 1.3f);
	const float Top = Position.y + std::round((ImGui::GetFrameHeight() - Height) * 0.5f);
	ImGui::Dummy({Width, ImGui::GetFrameHeight()});
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled({Position.x, Top}, {Position.x + Width, Top + Height}, ImGui::GetColorU32(ImGuiCol_FrameBg), Height * 0.25f);
	Draw->AddRect({Position.x, Top}, {Position.x + Width, Top + Height}, ImGui::GetColorU32(ImGuiCol_Border), Height * 0.25f);
	Draw->AddText({Position.x + PaddingX, Top + std::round((Height - TextSize.y) * 0.5f)}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Key.data(), Key.data() + Key.size());
	return Width;
}

// Labels a compact settings row on the left, with an optional shortcut keycap, and right-aligns the next item.
// The audio engine's statistics as JSON, for play_state and diagnostics.
[[nodiscard]] std::string DescribeAudio(const FAudioStats& Stats);

inline void DrawFieldLabel(const char* const Label, const float ValueWidth, const char* const Shortcut = nullptr)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(Label);

	if (Shortcut != nullptr)
	{
		ImGui::SameLine(0.f, std::round(ImGui::GetFontSize() * 0.4f));
		DrawKeycap(Shortcut);
	}

	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ValueWidth);
	ImGui::SetNextItemWidth(ValueWidth);
}

[[nodiscard]] inline Im3d::Vec3 ToIm3dVector(const FVector3 Vector) noexcept
{
	return {Vector.X, Vector.Y, Vector.Z};
}

[[nodiscard]] inline FMatrix4 ToHertaMatrix(const Im3d::Mat4& Matrix) noexcept
{
	FMatrix4 Result;

	for (std::size_t Column = 0; Column < 4; ++Column)
	{
		for (std::size_t Row = 0; Row < 4; ++Row)
		{
			Result(Row, Column) = Matrix(static_cast<int>(Row), static_cast<int>(Column));
		}
	}

	return Result;
}

struct FEditorFramework::FImplementation
{
	FImplementation();
	~FImplementation();
	FImplementation(const FImplementation&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	void SetBottomPanelOpen(bool bOpen);
	void ToggleContentDrawer();
	void ToggleOutputLogDrawer();
	[[nodiscard]] std::expected<void, FEditorFrameworkError> DrawOutputLog();
	void DrawDetailsPanel();
	void DrawOutlinerPanel();
	void DrawViewport(const std::function<void()>& RenderViewport);
	void DrawViewportContextMenu();
	[[nodiscard]] float DrawViewportToolbar(ImVec2 Minimum, ImVec2 Size);
	void DrawViewportStats(ImVec2 Minimum, ImVec2 Size, float ToolbarBottom);
	void UpdateViewport(const ImVec2 RenderMinimum, const ImVec2 RenderSize);
	void PrepareViewportGizmos(FVector2 NormalizedMouse, bool bGizmoInput, bool bSelect);
	void DrawPreviewGizmo(FPreviewObject& Object);
	void BuildViewportDebugDraw(bool bGizmoInput, const FVector2 NormalizedMouse);
	void BuildCollisionDebugDraw();
	[[nodiscard]] std::string DescribePlay() const;
	// Steers, traces, and checks the stop condition of the automation input that just played one frame.
	void FinishAutomationFrame();
	// The level's player character, chosen from the models in content.
	void DrawPlayerCharacterPicker(float ValueWidth);
	void DrawLevelSoundPickers(float ValueWidth);
	void FocusPreview();
	void RefreshPreviewMeshes();
	[[nodiscard]] const FRenderMesh* RefreshSoftBodyPreview(std::size_t Index, const FLevelEntity& Entity);
	void RefreshVisuals();
	void DrawHdrDisplaySettings(FEditorDisplay& Display, float ValueWidth);
	void RefreshSelectedVisualEntities();
	void DrawMaterialPanel();
	std::expected<std::string, FAssetError> CreateMaterialGraph(FAssetId Id);
	void OpenMaterialGraph(FAssetId Id, std::string_view ShaderPath);
	void OpenMaterialGraphKey(const std::string& Key);
	// Opens the player character model's animation graph, first creating one beside the model from the built-in locomotion when its metadata names none.
	void OpenPlayerAnimationGraph();
	void OpenAnimationGraphKey(const std::string& Key);
	void DrawAnimationGraphPanel();
	// A content file as its mounted path, "Game/..." or "Engine/...", or nothing outside both content roots.
	std::optional<std::string> MountedContentPath(const std::filesystem::path& File) const;
	void ResolveGraphPreview();
	void DrawPerformancePanel();
	[[nodiscard]] int FindTimeOfDaySun() const;
	void DrawTimeOfDay(float X, float Top, float Width, float ButtonSize, float Scale);
	void ImportWithDialog();
	void OpenLevelWithDialog();
	void OpenProjectWithDialog();
	void DrawNewProject();
	void StartProjectOperation(const std::filesystem::path& Path, std::optional<FCreateProjectRequest> Create = std::nullopt);
	void SaveCurrentLevel();
	void RefreshSelectionFromLevel();
	void RefreshLevel(bool bPreserveGizmoDrag = false);
	void ApplyAuthoringAction(EAuthoringAction Action);
	void ReportLevelResult(std::expected<void, FLevelError> Result);
	// Maps the built-in cube's [-1, 1] box onto the object's mesh bounds, so cube-based picking, outlines, and physics fit any mesh.
	[[nodiscard]] FMatrix4 GetPreviewBoundsMatrix(std::size_t Index) const;
	[[nodiscard]] FLevelBodyShape GetPreviewBodyShape(std::size_t Index) const;
	[[nodiscard]] FPreviewObject& GetActivePreviewObject() noexcept;
	void SetPreviewSelection(int ObjectIndex, bool bToggle = false);
	void SetPreviewSelection(FPreviewSelection Selection);
	void CancelViewportBoxSelection();
	void RequestPreviewRename();
	void ToggleSimulation();
	// Play runs the level through the same game session as HertaGame, on a copy that leaves authored state untouched.
	void TogglePlay();
	void UpdatePlay(float DeltaSeconds);
	// Keeps the level's sounds loaded into the editor's audio engine, and mutes it while Play is paused.
	void UpdateAudio(float DeltaSeconds);
	[[nodiscard]] bool AreLevelSoundsLoading() const;
	void SubmitPlayAudio();
	void StopPlayAudio();
	bool IsPlayGamepadPressed(EGamepadButton Button) const;
	void DrawPlayHud(ImVec2 Minimum, ImVec2 Size);

	[[nodiscard]] bool IsSimulating() const noexcept
	{
		return Simulation.IsRunning() || PlaySession != nullptr;
	}

	void JumpToCameraBookmark(std::uint32_t Slot);
	void StartAtFirstCameraBookmark();
	void SaveCameraBookmark(std::uint32_t Slot);
	void DrawCameraBookmarks(float Scale);
	void UpdateSimulation(float DeltaSeconds);
	void RebuildSuggestions(std::string_view Prefix);
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();

	FToolUIContext* ToolUI = nullptr;
	FApplication* Application = nullptr;
	FLogService* Log = nullptr;

	bool bOutlinerOpen = true;
	bool bDetailsOpen = true;
	bool bBottomPanelOpen = true;
	bool bBottomBrowserSelected = true;
	// Startup restores like a reopened panel; otherwise the last tab to appear, the Output Log, is selected.
	bool bRestoreBottomPanelFocus = true;
	bool bContentBrowserOpen = true;
	// The material previewed beside the open Material Graph, and the shader generation its preview shows.
	// The graph whose unsaved draft the shader watcher compiles, and the draft revision it last received.
	std::string LiveGraphPath;
	std::uint64_t LiveGraphRevision = 0;
	FAssetId GraphPreviewAsset{};
	std::optional<FMaterialAsset> GraphPreviewMaterial;
	std::uint64_t GraphPreviewGeneration = UINT64_MAX;
	bool bGraphPreviewPending = false;
	// The preview camera's zoom, shared by the Material and Material Graph editors since they share one preview.
	float MaterialPreviewZoom = 1.f;

	// The bottom panel belongs to the level tab; an asset editor tab in front summons the browser or log as a drawer instead.
	bool bContentDrawerOpen = false;
	bool bOutputLogDrawerOpen = false;
	FContentBrowserState ContentBrowserState;
	FDetailsPanelState DetailsPanelState;
	FMaterialPanel MaterialPanel;
	FGraphDocumentPanel MaterialGraphPanel{GetMaterialGraphDocumentDomain()};
	// The player character's animation graph, and its compiled state and transition nodes for highlighting Play, rebuilt when the draft changes.
	FGraphDocumentPanel AnimationGraphPanel{GetAnimationGraphDocumentDomain()};
	std::vector<FGraphNodeId> AnimationStateNodes;
	std::vector<FGraphNodeId> AnimationTransitionNodes;
	std::uint64_t AnimationGraphRevision = UINT64_MAX;
	FAssetId PendingMaterialOpen;
	// A material asset opened from the command line, by mounted path, until the content scan registers it.
	std::string PendingMaterialPath;
	FEditorAssetThumbnail DraftMaterialThumbnail;
	std::vector<FMaterialTextureOption> MaterialTextureOptions;
	FOutlinerPanelState OutlinerPanelState;
	bool bPerformanceOpen = true;
	bool bSelectDetailsTab = true;
	FPerformancePanelState PerformanceState;
	FPlaceObjectsMenuState PlaceObjectsMenuState;
	bool bPlaceObjectsRequested = false;

	bool bOutputLogOpen = true;
	bool bFocusOutputLogRequested = false;
	std::unique_ptr<FOutputLogModel> OutputLog;
	// Declared after OutputLog so it drains before the model that forwards to it is destroyed.
	std::unique_ptr<FShellRunner> Shell;
	std::array<char, 512> SearchBuffer = {};
	std::array<char, 512> CommandBuffer = {};
	std::vector<std::string> Suggestions;
	int SuggestionIndex = -1;
	bool bReclaimCommandFocus = false;
	bool bFocusCommandRequested = false;

	std::shared_ptr<FEditorLevel> Level = std::make_shared<FEditorLevel>();
	std::vector<FPreviewObject>& PreviewObjects = Level->GetObjects();
	FPreviewObject EmptyPreviewObject;
	std::uint64_t LevelGeneration = 0;
	std::vector<FMatrix4> PreviewModels;
	// Null entries draw the built-in cube. Refreshed from Assets at the start of every frame.
	std::vector<const FRenderMesh*> PreviewMeshes;

	// Generated soft body geometry. Settings changes rebuild the mesh; simulation steps rewrite its vertices.
	struct FSoftBodyPreview
	{
		FSoftBodyComponent Settings;
		FSoftBodyTopology Topology;
		std::shared_ptr<const FRenderMesh> Mesh;
		std::vector<FCookedVertex> Vertices;
		std::uint64_t Revision = 0;
	};

	std::map<FObjectId, FSoftBodyPreview> SoftBodyPreviews;
	// Vertex rewrites for this frame's render, pointing into SoftBodyPreviews.
	std::vector<FRenderMeshVertexUpdate> SoftBodyVertexUpdates;
	std::vector<FLevelEntity> VisualEntities;
	std::vector<std::vector<const FRenderMaterial*>> PreviewMaterials;
	std::vector<std::span<const FRenderMaterial* const>> PreviewMaterialSpans;
	std::vector<FRenderLight> RenderLights;
	std::size_t EnabledLightCount = 0;
	std::size_t ReportedLightBudgetCount = 0;
	FVisualSettings VisualSettings = []
	{
		FVisualSettings Settings;
		ApplyRenderQuality(Settings, ERenderQuality::High);
		return Settings;
	}();
	std::optional<FExtent2D> ViewportResolution;
	FEditorDisplayState DisplayState;
	// Shown only while the Appearance popup that controls it is open.
	bool bHdrCalibration = false;
	std::vector<FObjectId> SunIds;
	std::vector<std::string> SunLabels;
	std::vector<FObjectId> ConstraintTargetIds;
	std::vector<std::string> ConstraintTargetLabels;
	std::vector<FAssetId> EnvironmentIds;
	std::vector<std::string> EnvironmentLabels;
	std::vector<FAssetId> SoundIds;
	std::vector<std::string> SoundLabels;
	FPreviewSelection PreviewSelection;
	std::optional<Im3d::Mat4> PreviewDragStart;
	bool bDuplicateOnDrag = false;
	bool bViewportEditFinished = false;
	bool bViewportEditCanceled = false;

	FLevelSimulation Simulation;
	std::vector<FPreviewObject> SimulationStart;
	bool bSimulationStoppedThisFrame = false;

	// The session reads meshes and materials through PlayAssets, so it is released first.
	std::unique_ptr<IGameAssets> PlayAssets;
	std::unique_ptr<FGameSession> PlaySession;

	// The editor's audio engine plays Play's sounds and Audio Graph auditions, keyed by the build key of each loaded sound.
	std::unique_ptr<FAudioEngine> Audio;
	bool bAudioUnavailable = false;
	bool bAudioMuted = false;
	std::map<FAssetId, FHash128> AudioKeys;
	std::map<FAssetId, std::shared_ptr<const FCookedSoundGraph>> AuditionGraphs;
	std::set<FAssetId> AuditionKeys;
	FMeshRenderView PlayRenderView;
	// Play draws the game's own HUD and pause menu with GameUI, so it matches the standalone game.
	std::unique_ptr<FGameUI> PlayUI;
	bool bPlayPaused = false;
	std::size_t PlayMenuFocus = 0;
	std::optional<std::size_t> PlayMenuChoice;
	bool bScriptedPlay = false;
	// Automation drives Play with queued per-frame input and answers some calls on a later frame. Play it started belongs to it until it
	// stops: people and gamepads cannot move the player, and the world holds still between calls, so only queued frames advance it.
	bool bAutomationOwnsPlay = false;

	struct FAutomationFrame
	{
		FGameInput Input{};
		// Turns toward this world [x, z] point before the frame plays.
		std::optional<FVector2> SteerTo{};
	};

	// What one play_input call asked for beyond its frames, and what happened, for its reply.
	struct FAutomationRun
	{
		std::string Until{};
		std::size_t TraceEvery = 0;
		std::size_t Frames = 0;
		bool bWasAirborne = false;
		std::string StartMode{};
		std::string LastMode{};
		std::string Trace{};
		std::string StoppedBy{};
		// Animation events in the order they first fired, with how often each did.
		std::vector<std::pair<std::string, std::size_t>> AnimationEvents{};
	};

	std::deque<FAutomationFrame> AutomationInput;
	FAutomationRun AutomationRun{};
	std::optional<FVector3> AutomationPlayAt{};
	float AutomationPlayYaw = 0.f;
	std::optional<std::uint64_t> AutomationInputRequest;
	std::optional<std::uint64_t> AutomationPlayRequest;
	std::chrono::steady_clock::time_point AutomationPlayDeadline;
	std::chrono::steady_clock::time_point AutomationPlayRetry;
	// Shift+Space starts play, so Space must come up once before it can jump.
	bool bPlayJumpArmed = false;
	// This frame's gamepad and the previous one's, so buttons act once per press.
	std::optional<FGamepadState> PlayGamepad;
	std::optional<FGamepadState> PreviousPlayGamepad;
	FGameGamepadControls PlayGamepadControls;
	std::uint32_t ScriptedPlayFrame = 0;

	std::uint64_t ViewportTexture = 0;
	FExtent2D ViewportExtent{.Width = 960, .Height = 540};
	FViewportCameraController ViewportCamera;
	FMeshRenderView ViewportRenderView;
	std::vector<std::size_t> ViewportSelectedModels;
	FVector2 ViewportProjectionCenter{0.5f, 0.5f};
	FVector2 ViewportVisibleSize{1.f, 1.f};
	FViewportInteractionState ViewportInteraction;
	FViewportBoxSelectionState ViewportBoxSelection;
	bool bViewportControlsHovered = false;
	double CameraCoordinatesCopiedUntil = 0.0;
	float WorldIslandWidth = ViewportIconButtonSize + 6.f;

	bool bGameView = false;
	bool bGridVisible = true;
	// Physics wireframes while Simulate or Play runs.
	bool bCollisionVisible = false;
	bool bAxesVisible = false;
	bool bOrientationIndicatorVisible = true;
	bool bBoundsVisible = false;
	bool bCameraReadoutVisible = false;
	// The speed readout appears while the speed changes and lingers briefly after.
	bool bCameraSpeedVisible = true;
	double CameraSpeedShownUntil = 0.;
	std::array<char, 128> CameraBookmarkName{};
	bool bTimeOfDayVisible = true;
	FPhysicalCamera PhysicalCamera;
	// Like a camera's program mode: the renderer meters the scene, and the manual aperture, shutter, and ISO wait until it is off.
	bool bAutoExposure = true;
	// The level's compensation, previewed here while its slider is dragged.
	float ExposureCompensation = 0.f;
	bool bExposureCompensationActive = false;
	std::vector<FDebugDrawVertex> ViewportDebugVertices;
	std::vector<FPhysicsDebugLine> CollisionLines;
	std::vector<FDebugDrawVertex> CollisionVertices;
	// The viewport's lists plus collision, or collision alone during Play, which otherwise shows no editor overlays.
	std::vector<FDebugDrawList> CombinedDebugDrawLists;
	std::vector<FDebugDrawList> ViewportDebugDrawLists;

	Im3d::Context ViewportGizmos;
	int GizmoMode = Im3d::GizmoMode_Translation;
	bool bTransformGizmoVisible = true;
	bool bLocalGizmo = false;
	bool bFlipGizmoAxesTowardCamera = false;
	FViewportRotationFeedbackState RotationFeedback;
	FViewportScaleGizmoState ScaleGizmoState;
	FViewportScaleGizmoFeedback ScaleFeedback;

	bool bSnapEnabled = false;
	float TranslationSnap = 0.5f;
	float RotationSnapDegrees = 15.f;
	float ScaleSnap = 0.1f;

	std::shared_ptr<FViewportStats> Stats = std::make_shared<FViewportStats>();
	double CpuFrameMilliseconds = 0.0;
	std::optional<double> GpuUIMilliseconds;
	FEditorFrameMetrics FrameMetrics;
	bool bScalingTest = false;

	std::vector<std::string> MeshOptions;
	std::vector<FAssetId> MeshOptionIds;
	std::unique_ptr<FPreviewAssets> Assets;
	FEditorAssetPaths AssetPaths;
	FEditorAssetThumbnailRenderer RenderAssetThumbnail;
	FEditorMaterialThumbnailRenderer RenderMaterialThumbnail;
	FMeshRenderer* MeshRenderer = nullptr;
	std::function<std::expected<std::uint64_t, FPresentationError>(const FTextureHandle&)> RegisterUITexture;
	std::function<void(std::uint64_t)> UnregisterUITexture;
	std::unique_ptr<FVisualEnvironment> VisualEnvironment;
	std::unique_ptr<FMaterialShaders> MaterialShaders;
	std::filesystem::path WatchedShaderContentRoot;
	FTaskSystem* Tasks = nullptr;
	IGraphicsDevice* GraphicsDevice = nullptr;

	std::filesystem::path EngineRoot;
	std::filesystem::path ProjectPath;
	std::array<char, 128> ProjectName{};
	std::array<char, 128> ProjectModule{};
	std::array<char, 1024> ProjectDestination{};
	std::string ProjectError;
	bool bProjectBusy = false;
	bool bCloseProjectPopup = false;
	int PendingDocumentAction = 0;
	bool bWaitingForMaterialClose = false;
	bool bCloseRequested = false;
	bool bCloseConfirmed = false;
	std::stop_source ProjectCancellation;
	// Drained before callback targets and editor services are destroyed.
	std::unique_ptr<FTaskScope> ProjectScope;
};
}
