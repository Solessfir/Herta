#include "Herta/EditorFramework/EditorFramework.h"

#include "ConsoleInput.h"
#include "ContentBrowser.h"
#include "DetailsPanel.h"
#include "EditorLevel.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/LevelCommands.h"
#include "Herta/EditorCore/PreviewSelection.h"
#include "Herta/EditorCore/ProjectCommands.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/ViewportInteraction.h"
#include "Herta/Platform/FileDialog.h"
#include "Herta/Platform/Process.h"
#include "Herta/Project/Project.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"
#include "MaterialPanel.h"
#include "MaterialShaders.h"
#include "NumericField.h"
#include "OutlinerPanel.h"
#include "OutputLogTextLayout.h"
#include "PerformancePanel.h"
#include "PlaceObjectsMenu.h"
#include "PreviewAssets.h"
#include "PreviewLevel.h"
#include "PreviewSimulation.h"
#include "PreviewVisuals.h"
#include "SoftBodyGeometry.h"
#include "TimeOfDay.h"
#include "ViewportBoxSelection.h"
#include "ViewportGizmos.h"
#include "ViewportIsland.h"
#include "ViewportRotationFeedback.h"
#include "ViewportScaleGizmo.h"
#include "ViewportStats.h"
#include "VisualEnvironment.h"

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
#include <format>
#include <iterator>
#include <limits>
#include <map>
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
namespace
{
inline constexpr FLogCategory EditorLog{.Name = "Editor"};
inline constexpr FLogCategory ShellLog{.Name = "Shell"};

std::string MaterialShaderKey(const std::string_view Path, const std::string_view MaterialPath)
{
	if (Path.empty() || Path.starts_with("Engine/") || Path.starts_with("Game/"))
	{
		return std::string(Path);
	}

	return std::format("{}/{}", MaterialPath.starts_with("Engine/") ? "Engine" : "Game", Path);
}

enum class EAuthoringAction
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

inline constexpr std::array CategoryColors = {
    IM_COL32(126, 200, 255, 255),
    IM_COL32(142, 220, 182, 255),
    IM_COL32(156, 194, 210, 255),
    IM_COL32(203, 166, 255, 255),
    IM_COL32(166, 184, 224, 255),
    IM_COL32(115, 218, 224, 255),
    IM_COL32(152, 202, 194, 255),
    IM_COL32(188, 178, 220, 255),
    IM_COL32(166, 184, 255, 255),
    IM_COL32(160, 198, 230, 255),
    IM_COL32(135, 210, 154, 255),
    IM_COL32(182, 190, 214, 255),
};

[[nodiscard]] ImU32 PackColor(const FToolUIColor Color) noexcept
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

constexpr std::array ViewportAxisColors{Im3d::Color(0xd57b7fff), Im3d::Color(0x83b993ff), Im3d::Color(0x7c9ed5ff)};

[[nodiscard]] std::array<float, 4> ResolveViewportDebugColor(const Im3d::Color Color)
{
	static const auto LinearAxisColors = []
	{
		std::array<std::array<float, 3>, 3> Result{};

		for (std::size_t Axis = 0; Axis < Result.size(); ++Axis)
		{
			const auto Source = ViewportAxisColors[Axis];
			Result[Axis] = {Source.getR(), Source.getG(), Source.getB()};

			for (float& Channel : Result[Axis])
			{
				Channel = Channel <= 0.04045f ? Channel / 12.92f : std::pow((Channel + 0.055f) / 1.055f, 2.4f);
			}
		}

		return Result;
	}();

	constexpr std::array SourceColors{Im3d::Color_Red, Im3d::Color_Green, Im3d::Color_Blue};

	for (std::size_t Axis = 0; Axis < SourceColors.size(); ++Axis)
	{
		if ((Color.v & 0xffffff00u) == (SourceColors[Axis].v & 0xffffff00u))
		{
			const auto& Channels = LinearAxisColors[Axis];
			return {Channels[0], Channels[1], Channels[2], Color.getA()};
		}
	}

	return {Color.getR(), Color.getG(), Color.getB(), Color.getA()};
}

void DrawViewportAxes(const FMatrix4& View, const ImVec2 Minimum, const ImVec2 Size, const float Scale)
{
	const float AxisLength = 16.f * Scale;
	const float FontSize = ImGui::GetFontSize() * 0.8f;
	constexpr std::array Labels{"X", "Y", "Z"};
	std::array<ImVec2, 3> TipOffsets;
	std::array<ImVec2, 3> LabelOffsets;
	float Leftmost = -2.f * Scale;
	float Bottommost = 2.f * Scale;

	for (std::size_t Axis = 0; Axis < Labels.size(); ++Axis)
	{
		TipOffsets[Axis] = {-View(0, Axis) * AxisLength, -View(1, Axis) * AxisLength};
		const float Length = std::hypot(View(0, Axis), View(1, Axis));
		const ImVec2 Direction = Length > 0.001f ? ImVec2{-View(0, Axis) / Length, -View(1, Axis) / Length} : ImVec2{0.f, 1.f};
		const ImVec2 LabelSize = ImGui::GetFont()->CalcTextSizeA(FontSize, 100.f * Scale, 0.f, Labels[Axis]);
		const float LabelRadius = AxisLength * Length + 3.f * Scale + std::hypot(LabelSize.x, LabelSize.y) * 0.5f;
		LabelOffsets[Axis] = {Direction.x * LabelRadius - LabelSize.x * 0.5f, Direction.y * LabelRadius - LabelSize.y * 0.5f};
		Leftmost = std::min({Leftmost, TipOffsets[Axis].x - 2.f * Scale, LabelOffsets[Axis].x});
		Bottommost = std::max({Bottommost, TipOffsets[Axis].y + 2.f * Scale, LabelOffsets[Axis].y + LabelSize.y});
	}

	const float Inset = 14.f * Scale;
	const ImVec2 Origin{Minimum.x + Inset - Leftmost, Minimum.y + Size.y - Inset - Bottommost};
	std::array<std::size_t, 3> Order{0, 1, 2};
	std::ranges::sort(Order, [&](const std::size_t A, const std::size_t B)
	{
		return View(2, A) > View(2, B);
	});

	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	DrawList->PushClipRect(Minimum, {Minimum.x + Size.x, Minimum.y + Size.y}, true);

	for (const std::size_t Axis : Order)
	{
		const ImVec2 Tip{Origin.x + TipOffsets[Axis].x, Origin.y + TipOffsets[Axis].y};
		DrawList->AddLine(Origin, Tip, ViewportAxisColors[Axis].getABGR(), 1.5f * Scale);
		DrawList->AddCircleFilled(Tip, 2.f * Scale, ViewportAxisColors[Axis].getABGR());
	}

	for (const std::size_t Axis : Order)
	{
		const ImVec2 LabelPosition{Origin.x + LabelOffsets[Axis].x, Origin.y + LabelOffsets[Axis].y};
		DrawList->AddText(ImGui::GetFont(), FontSize, LabelPosition, ViewportAxisColors[Axis].getABGR(), Labels[Axis]);
	}

	DrawList->PopClipRect();
}

[[nodiscard]] ImU32 ResolveLineColor(const FOutputLogLine& Line, const bool bColorizeCategories) noexcept
{
	switch (Line.Record.Level)
	{
		case ELogLevel::Warning:
			return PackColor(ToolUITheme::Warning);
		case ELogLevel::Error:
		case ELogLevel::Critical:
			return PackColor(ToolUITheme::Error);
		case ELogLevel::Trace:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextMuted);
			}

			break;
		case ELogLevel::Debug:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextSecondary);
			}

			break;
		case ELogLevel::Info:
			if (!bColorizeCategories)
			{
				return PackColor(ToolUITheme::TextPrimary);
			}

			break;
		case ELogLevel::Off:
			return PackColor(ToolUITheme::TextMuted);
	}

	return CategoryColors[HashOutputLogCategory(Line.Record.Category) % CategoryColors.size()];
}

[[nodiscard]] FLogTextPosition HitTestText(const std::span<const FOutputLogLine> Lines, const FOutputLogColumns Columns, const ImVec2 TextOrigin, const float LineHeight, const ImVec2 MousePosition)
{
	if (Lines.empty())
	{
		return {};
	}

	std::size_t LineIndex = 0;

	if (MousePosition.y > TextOrigin.y)
	{
		LineIndex = std::min(static_cast<std::size_t>((MousePosition.y - TextOrigin.y) / LineHeight), Lines.size() - 1);
	}

	return {.Line = LineIndex, .Byte = FindOutputLogByteAtX(Lines[LineIndex], MousePosition.x - TextOrigin.x, Columns)};
}

// Draws a key as a small rounded chip centered on the frame row and returns its width; measures only when bDraw is false.
float DrawKeycap(const std::string_view Key, const bool bDraw = true)
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
void DrawFieldLabel(const char* const Label, const float ValueWidth, const char* const Shortcut = nullptr)
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

void CopyBuffer(std::span<char> Destination, const std::string_view Source)
{
	std::ranges::fill(Destination, '\0');
	const std::size_t Count = std::min(Source.size(), Destination.size() - 1);
	std::ranges::copy(Source.substr(0, Count), Destination.begin());
}

[[nodiscard]] constexpr const char* GetPanelTransparencyLabel(const EPanelTransparency Mode) noexcept
{
	switch (Mode)
	{
		case EPanelTransparency::AllPanels:
			return "All panels";
		case EPanelTransparency::FloatingOnly:
			return "Floating panels";
		case EPanelTransparency::DockedOnly:
			return "Docked panels";
		case EPanelTransparency::Disabled:
			return "Opaque panels";
	}

	return "Unknown";
}

[[nodiscard]] Im3d::Vec3 ToIm3dVector(const FVector3 Vector) noexcept
{
	return {Vector.X, Vector.Y, Vector.Z};
}

[[nodiscard]] FMatrix4 ToHertaMatrix(const Im3d::Mat4& Matrix) noexcept
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

[[nodiscard]] FTransform ToHertaTransform(const FPreviewObject& Object)
{
	const Im3d::Vec3 Euler = Im3d::ToEulerXYZ(Object.Rotation);
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0, 0, 1}, Euler.z) * FQuaternion::FromAxisAngle({0, 1, 0}, Euler.y) * FQuaternion::FromAxisAngle({1, 0, 0}, Euler.x);
	return {{Object.Translation.x, Object.Translation.y, Object.Translation.z}, Rotation, {Object.Scale.x, Object.Scale.y, Object.Scale.z}};
}

struct FIm3dContextScope final
{
	explicit FIm3dContextScope(Im3d::Context& Context) noexcept;
	~FIm3dContextScope();

	FIm3dContextScope(const FIm3dContextScope&) = delete;
	FIm3dContextScope& operator=(const FIm3dContextScope&) = delete;
	FIm3dContextScope(FIm3dContextScope&&) = delete;
	FIm3dContextScope& operator=(FIm3dContextScope&&) = delete;

	Im3d::Context& Previous;
};

FIm3dContextScope::FIm3dContextScope(Im3d::Context& Context) noexcept
    : Previous(Im3d::GetContext())
{
	Im3d::SetContext(Context);
}

FIm3dContextScope::~FIm3dContextScope()
{
	Im3d::SetContext(Previous);
}
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
	void FocusPreview();
	void RefreshPreviewMeshes();
	[[nodiscard]] const FRenderMesh* RefreshSoftBodyPreview(std::size_t Index, const FLevelEntity& Entity);
	void RefreshVisuals();
	void DrawHdrDisplaySettings(FEditorDisplay& Display, float ValueWidth);
	void RefreshSelectedVisualEntities();
	void DrawMaterialPanel();
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
	[[nodiscard]] FPreviewBodyShape GetPreviewBodyShape(std::size_t Index) const;
	[[nodiscard]] FPreviewObject& GetActivePreviewObject() noexcept;
	void SetPreviewSelection(int ObjectIndex, bool bToggle = false);
	void SetPreviewSelection(FPreviewSelection Selection);
	void CancelViewportBoxSelection();
	void RequestPreviewRename();
	void ToggleSimulation();
	void JumpToCameraBookmark(std::uint32_t Slot);
	void SaveCameraBookmark(std::uint32_t Slot);
	void DrawCameraBookmarks(float Scale);
	void UpdateSimulation(float DeltaSeconds);
	void RebuildSuggestions(std::string_view Prefix);
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();

	FToolUIContext* ToolUI = nullptr;
	FLogService* Log = nullptr;

	bool bOutlinerOpen = true;
	bool bDetailsOpen = true;
	bool bBottomPanelOpen = true;
	bool bBottomBrowserSelected = true;
	// Startup restores like a reopened panel; otherwise the last tab to appear, the Output Log, is selected.
	bool bRestoreBottomPanelFocus = true;
	bool bContentBrowserOpen = true;
	FContentBrowserState ContentBrowserState;
	FDetailsPanelState DetailsPanelState;
	FMaterialPanel MaterialPanel;
	FAssetId PendingMaterialOpen;
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
	FVisualSettings VisualSettings;
	FEditorDisplayState DisplayState;
	// Shown only while the Appearance popup that controls it is open.
	bool bHdrCalibration = false;
	std::vector<FObjectId> SunIds;
	std::vector<std::string> SunLabels;
	std::vector<FObjectId> AttachmentIds;
	std::vector<std::string> AttachmentLabels;
	std::vector<FAssetId> EnvironmentIds;
	std::vector<std::string> EnvironmentLabels;
	FPreviewSelection PreviewSelection;
	std::optional<Im3d::Mat4> PreviewDragStart;
	bool bDuplicateOnDrag = false;
	bool bViewportEditFinished = false;
	bool bViewportEditCanceled = false;

	FPreviewSimulation Simulation;
	std::vector<FPreviewObject> SimulationStart;
	bool bSimulationStoppedThisFrame = false;

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
	bool bAxesVisible = false;
	bool bOrientationIndicatorVisible = true;
	bool bBoundsVisible = false;
	bool bCameraReadoutVisible = false;
	bool bCameraSpeedVisible = false;
	std::array<char, 128> CameraBookmarkName{};
	bool bTimeOfDayVisible = true;
	FPhysicalCamera PhysicalCamera;
	// Like a camera's program mode: the renderer meters the scene, and the manual aperture, shutter, and ISO wait until it is off.
	bool bAutoExposure = true;
	float ExposureCompensation = 0.f;
	std::vector<FDebugDrawVertex> ViewportDebugVertices;
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

FEditorFramework::FImplementation::~FImplementation()
{
	ProjectCancellation.request_stop();
	if (ProjectScope)
	{
		ProjectScope->RequestCancellation();
		ProjectScope->Wait();
	}
}

FPreviewObject& FEditorFramework::FImplementation::GetActivePreviewObject() noexcept
{
	return PreviewObjects.empty() ? EmptyPreviewObject : PreviewObjects[static_cast<std::size_t>(std::max(PreviewSelection.Active, 0))];
}

FEditorFramework::FImplementation::FImplementation()
{
	PreviewModels.resize(PreviewObjects.size());
	PreviewMeshes.resize(PreviewObjects.size());
	const auto Camera = ViewportCamera.GetSnapshot(960.f / 540.f);

	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
	}

	ViewportRenderView = {.View = Camera.View, .Projection = Camera.Projection, .Models = PreviewModels};
	ViewportRenderView.Meshes = PreviewMeshes;
}

std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> FEditorFramework::Create(const FEditorFrameworkDescriptor& Descriptor)
{
	if (Descriptor.Log == nullptr || Descriptor.Commands == nullptr || Descriptor.ToolUI == nullptr)
	{
		return std::unexpected(FEditorFrameworkError{"EditorFramework requires logging, commands, and ToolUI"});
	}

	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> OutputLog = FOutputLogModel::Create(*Descriptor.Log, *Descriptor.Commands);
	if (!OutputLog)
	{
		return std::unexpected(FEditorFrameworkError{std::move(OutputLog.error().Message)});
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->ToolUI = Descriptor.ToolUI;
	Implementation->OutputLog = std::move(*OutputLog);
	Implementation->Log = Descriptor.Log;
	Implementation->Tasks = Descriptor.Tasks;
	Implementation->GraphicsDevice = Descriptor.GraphicsDevice;
	Implementation->AssetPaths = Descriptor.Assets;
	Implementation->RenderAssetThumbnail = Descriptor.RenderAssetThumbnail;
	Implementation->RenderMaterialThumbnail = Descriptor.RenderMaterialThumbnail;
	Implementation->MeshRenderer = Descriptor.MeshRenderer;
	if (Descriptor.Tasks && Descriptor.MeshRenderer)
	{
		Implementation->VisualEnvironment = FVisualEnvironment::Create(*Descriptor.Tasks, *Descriptor.MeshRenderer, *Descriptor.Log);
	}
	Implementation->EngineRoot = Descriptor.EngineRoot;
	Implementation->ProjectPath = Descriptor.ProjectPath;
	std::filesystem::path InitialLevelPath = Descriptor.LevelPath;
	if (!Descriptor.ProjectPath.empty())
	{
		const auto Project = LoadProject(Descriptor.ProjectPath);
		if (!Project)
		{
			return std::unexpected(FEditorFrameworkError{Project.error().Message});
		}

		Implementation->ProjectPath = Project->DescriptorPath;
		Implementation->AssetPaths.ContentRoot = Project->ContentRoot;
		Implementation->AssetPaths.DerivedDataRoot = Project->Root / "DerivedDataCache" / Descriptor.Assets.TargetPlatform;
		if (InitialLevelPath.empty())
		{
			InitialLevelPath = Project->StartingLevel;
		}
	}

	Implementation->Level->SetPath(InitialLevelPath);

	if (!InitialLevelPath.empty())
	{
		std::error_code Error;
		const bool bExists = std::filesystem::exists(InitialLevelPath, Error);
		if (Error)
		{
			return std::unexpected(FEditorFrameworkError{std::format("Cannot query level path: {}", Error.message())});
		}

		if (bExists)
		{
			if (auto Loaded = Implementation->Level->Load(InitialLevelPath); !Loaded)
			{
				return std::unexpected(FEditorFrameworkError{Loaded.error().Message});
			}

			HERTA_LOG_INFO(*Descriptor.Log, EditorLog, "Opened level {} in project {}", InitialLevelPath.string(), Implementation->ProjectPath.string());
		}
	}

	if (Descriptor.Tasks != nullptr)
	{
		if (std::expected<std::unique_ptr<FTaskScope>, FTaskError> Scope = Descriptor.Tasks->CreateScope("Shell commands"))
		{
			Implementation->Shell = std::make_unique<FShellRunner>(*Descriptor.Tasks, *Descriptor.Log, std::move(*Scope));
			Implementation->OutputLog->SetShellRunner([Shell = Implementation->Shell.get()](std::string Command)
			{
				Shell->Run(std::move(Command));
			});
		}
	}

	Implementation->RefreshLevel();
	if (InitialLevelPath.lexically_normal() == (Descriptor.EngineRoot / "Games/Sandbox/Levels/Sandbox.hlevel").lexically_normal())
	{
		Implementation->ViewportCamera.Focus({0.f, 1.f, 6.f}, {10.f, 3.f, 12.f}, 16.f / 9.f);
	}

	if (auto Result = RegisterLevelFileCommands(*Descriptor.Commands); !Result)
	{
		return std::unexpected(FEditorFrameworkError{Result.error().Message});
	}

	if (auto Result = RegisterEditorLevelCommands(*Descriptor.Commands, Implementation->Level); !Result)
	{
		return std::unexpected(FEditorFrameworkError{Result.error().Message});
	}

	if (auto Result = RegisterViewportStatsCommand(*Descriptor.Commands, Implementation->Stats); !Result)
	{
		return std::unexpected(FEditorFrameworkError{std::move(Result.error().Message)});
	}

	if (!Descriptor.EngineRoot.empty())
	{
		if (auto Result = RegisterProjectCommands(*Descriptor.Commands, Descriptor.EngineRoot); !Result)
		{
			return std::unexpected(FEditorFrameworkError{Result.error().Message});
		}
	}

	return std::unique_ptr<FEditorFramework>(new FEditorFramework(std::move(Implementation)));
}

FEditorFramework::FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept
    : Implementation(std::move(Implementation))
{
}

FEditorFramework::~FEditorFramework() = default;

void FEditorFramework::SetFrameTimings(const double CpuMilliseconds, const std::optional<double> GpuUIMilliseconds) noexcept
{
	Implementation->CpuFrameMilliseconds = std::lerp(Implementation->CpuFrameMilliseconds, CpuMilliseconds, 0.1);
	Implementation->GpuUIMilliseconds = GpuUIMilliseconds;
}

FEditorFrameMetrics FEditorFramework::GetFrameMetrics() const noexcept
{
	FEditorFrameMetrics Metrics = Implementation->FrameMetrics;
	Metrics.ObjectCount = Implementation->PreviewObjects.size();
	Metrics.SelectedCount = Implementation->PreviewSelection.Indices.size();
	Metrics.bSimulationRunning = Implementation->Simulation.IsRunning();
	Metrics.bAssetsReady = true;
	if (Implementation->VisualEnvironment && !Implementation->VisualEnvironment->GetStatus().bReady)
	{
		Metrics.bAssetsReady = false;
	}

	if (Implementation->Assets)
	{
		if (!Implementation->Assets->AreMaterialThumbnailsReady())
		{
			Metrics.bAssetsReady = false;
		}

		for (const auto& Entity : Implementation->VisualEntities)
		{
			if (Entity.Mesh)
			{
				for (const FAssetId Material : Entity.Mesh->Materials)
				{
					if (Material.IsValid() && !Implementation->Assets->GetMaterial(Material))
					{
						Metrics.bAssetsReady = false;
					}
				}
			}

			if (Entity.Light && Entity.Light->Type == ELightType::Sky && Entity.Light->Environment.IsValid() && !Implementation->Assets->GetCookedTexture(Entity.Light->Environment))
			{
				Metrics.bAssetsReady = false;
			}
		}
	}

	for (std::size_t Index = 0; Index < Implementation->PreviewObjects.size(); ++Index)
	{
		if (Implementation->PreviewObjects[Index].Mesh.IsValid() && Implementation->PreviewMeshes[Index] == nullptr)
		{
			Metrics.bAssetsReady = false;
			break;
		}
	}

	return Metrics;
}

std::optional<std::string> FEditorFramework::ShowCameraBookmark(const std::uint32_t Slot)
{
	const auto Bookmarks = Implementation->Level->GetCameraBookmarks();
	const auto Found = std::ranges::find(Bookmarks, Slot, &FLevelCameraBookmark::Slot);
	if (Found == Bookmarks.end())
	{
		return std::nullopt;
	}

	Implementation->JumpToCameraBookmark(Slot);
	return Found->Name;
}

std::expected<void, FEditorFrameworkError> FEditorFramework::SetScalingTestPhase(const bool bSelectAll, const bool bSimulate)
{
	auto& State = *Implementation;
	State.bScalingTest = true;
	if (State.Simulation.IsRunning())
	{
		State.ToggleSimulation();
	}

	State.ApplyAuthoringAction(EAuthoringAction::SelectAll);
	State.FocusPreview();
	if (!bSelectAll)
	{
		State.SetPreviewSelection(-1);
	}

	if (bSimulate)
	{
		State.ToggleSimulation();
		if (!State.Simulation.IsRunning())
		{
			return std::unexpected(FEditorFrameworkError{"Scaling simulation could not start; see the physics diagnostic"});
		}
	}

	return {};
}

bool FEditorFramework::IsUnitStatsVisible() const noexcept
{
	return Implementation->Stats->bUnitVisible;
}

std::expected<void, FEditorFrameworkError> FEditorFramework::Draw(const std::function<void()>& RenderViewport)
{
	Implementation->FrameMetrics = {};
	// Each frame uploads only the soft body vertices that changed since the last render.
	Implementation->SoftBodyVertexUpdates.clear();
	Implementation->ViewportRenderView.VertexUpdates = {};
	ImGuiIO& IO = ImGui::GetIO();
	Implementation->RefreshLevel();
	if (!IO.AppFocusLost && IO.KeyMods == ImGuiMod_Ctrl && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && ImGui::IsKeyPressed(ImGuiKey_Space, false))
	{
		Implementation->SetBottomPanelOpen(!(Implementation->bBottomPanelOpen && (Implementation->bContentBrowserOpen || Implementation->bOutputLogOpen)));
	}

	if (!IO.AppFocusLost && !IO.WantTextInput && IO.KeyMods == 0 && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
		{
			Implementation->ToolUI->SetViewportImmersive(!Implementation->ToolUI->IsViewportImmersive());
		}

		if (Implementation->ToolUI->IsPanelFocused("Viewport") && ImGui::IsKeyPressed(ImGuiKey_G, false))
		{
			Implementation->bGameView = !Implementation->bGameView;
			Implementation->ViewportGizmos.resetId();
		}
	}

	// Number keys recall camera bookmarks and Ctrl+number saves them, like Unreal's viewport bookmarks.
	if (!IO.AppFocusLost && !IO.WantTextInput && (IO.KeyMods == 0 || IO.KeyMods == ImGuiMod_Ctrl) && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && Implementation->ToolUI->IsPanelFocused("Viewport"))
	{
		for (std::uint32_t Slot = 1; Slot <= MaximumLevelCameraBookmarks; ++Slot)
		{
			if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + Slot), false))
			{
				continue;
			}

			if (IO.KeyMods == ImGuiMod_Ctrl)
			{
				Implementation->SaveCameraBookmark(Slot);
			}
			else
			{
				Implementation->JumpToCameraBookmark(Slot);
			}
		}
	}

	Implementation->bSimulationStoppedThisFrame = false;

	if (Implementation->Assets)
	{
		const auto Destination = Implementation->ToolUI->IsPanelHovered("Content Browser") ? Implementation->ContentBrowserState.GetImportDestination() : std::string{};
		auto DroppedFiles = Implementation->ToolUI->TakeDroppedFiles();
		if (!Implementation->bProjectBusy)
		{
			Implementation->Assets->ImportFiles(std::move(DroppedFiles), Destination);
		}
		else if (!DroppedFiles.empty())
		{
			HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Wait for the project to finish loading before importing files");
		}
		Implementation->Assets->Tick();
	}

	Implementation->RefreshPreviewMeshes();

	if (!IO.AppFocusLost && Implementation->Simulation.IsRunning() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		Implementation->ToggleSimulation();
	}

	if (!Implementation->bProjectBusy && !IO.AppFocusLost && !IO.WantTextInput && IO.KeyAlt && !IO.KeyCtrl && !IO.KeyShift && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false) && !Implementation->Simulation.IsRunning() && !Implementation->Level->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		Implementation->ToggleSimulation();
	}

	const auto SimulationStart = std::chrono::steady_clock::now();
	Implementation->UpdateSimulation(Implementation->bScalingTest ? 1.f / 60.f : IO.DeltaTime);
	Implementation->FrameMetrics.SimulationMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - SimulationStart).count();

	if (IO.KeyMods == 0 && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
	{
		Implementation->bBottomPanelOpen = true;
		Implementation->bOutputLogOpen = true;
		Implementation->bFocusCommandRequested = true;

		for (int Index = IO.InputQueueCharacters.Size - 1; Index >= 0; --Index)
		{
			if (IO.InputQueueCharacters[Index] == '`')
			{
				IO.InputQueueCharacters.erase(IO.InputQueueCharacters.begin() + Index);
			}
		}
	}

	bool bOpenLevelRequested = false;
	bool bPlaceObjectsRequested = std::exchange(Implementation->bPlaceObjectsRequested, false);
	const bool bAuthoringAvailable = !Implementation->bProjectBusy && !Implementation->Simulation.IsRunning() && !Implementation->Level->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0;
	const bool bShortcutsAvailable = bAuthoringAvailable && !Implementation->MaterialPanel.IsFocused() && !IO.AppFocusLost && !IO.WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !IO.KeyAlt && !IO.KeySuper;
	const bool bControlShortcutsAvailable = bShortcutsAvailable && IO.KeyMods == ImGuiMod_Ctrl;
	const bool bProjectChangeAvailable = bAuthoringAvailable && !Implementation->EngineRoot.empty() && !(Implementation->Assets && Implementation->Assets->IsImporting());
	bool bNewProjectRequested = bControlShortcutsAvailable && bProjectChangeAvailable && ImGui::IsKeyPressed(ImGuiKey_N, false);
	bool bSaveLevelRequested = bControlShortcutsAvailable && ImGui::IsKeyPressed(ImGuiKey_S, false);
	bool bImportRequested = bControlShortcutsAvailable && Implementation->Assets && ImGui::IsKeyPressed(ImGuiKey_I, false);
	bool bOpenProjectRequested = bControlShortcutsAvailable && bProjectChangeAvailable && ImGui::IsKeyPressed(ImGuiKey_O, false);
	EAuthoringAction AuthoringAction = EAuthoringAction::None;
	if (bShortcutsAvailable && !IO.KeyCtrl && IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false))
	{
		if (Implementation->ToolUI->IsPanelHovered("Details"))
		{
			Implementation->DetailsPanelState.bAddComponentRequested = !Implementation->PreviewSelection.Indices.empty();
		}
		else
		{
			bPlaceObjectsRequested = true;
		}
	}

	if (bShortcutsAvailable && (Implementation->ToolUI->IsPanelFocused("Viewport") || Implementation->ToolUI->IsPanelFocused("Outliner") || Implementation->ToolUI->IsPanelFocused("Details")))
	{
		if (IO.KeyCtrl)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
			{
				AuthoringAction = IO.KeyShift ? EAuthoringAction::Redo : EAuthoringAction::Undo;
			}
			else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false) && Implementation->ToolUI->IsPanelFocused("Viewport"))
			{
				AuthoringAction = EAuthoringAction::SelectAll;
			}
			else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false))
			{
				AuthoringAction = EAuthoringAction::Redo;
			}
			else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false))
			{
				AuthoringAction = EAuthoringAction::Duplicate;
			}
			else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false))
			{
				AuthoringAction = EAuthoringAction::Copy;
			}
			else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V, false))
			{
				AuthoringAction = EAuthoringAction::Paste;
			}
		}
		else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			AuthoringAction = EAuthoringAction::Delete;
		}
	}

	const std::string LevelTitle = std::format("{}{} - Herta Editor", Implementation->Level->GetName(), Implementation->Level->IsDirty() ? "*" : "");
	Implementation->ToolUI->DrawWorkspace(LevelTitle, [&]
	{
		ToolUIMenuItem("Outliner", EToolUIMenuIcon::Outliner, &Implementation->bOutlinerOpen);
		ToolUIMenuItem("Details", EToolUIMenuIcon::Details, &Implementation->bDetailsOpen);
		bool bBottomVisible = Implementation->bBottomPanelOpen && (Implementation->bContentBrowserOpen || Implementation->bOutputLogOpen);
		if (ToolUIMenuItem("Bottom panel", EToolUIMenuIcon::Panel, &bBottomVisible, "Ctrl+Space"))
		{
			Implementation->SetBottomPanelOpen(bBottomVisible);
		}
		bool bBrowserVisible = Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen;
		if (ToolUIMenuItem("Content Browser", EToolUIMenuIcon::ContentBrowser, &bBrowserVisible))
		{
			Implementation->bContentBrowserOpen = bBrowserVisible;
			if (bBrowserVisible)
			{
				Implementation->bBottomPanelOpen = true;
				Implementation->ContentBrowserState.bFocusRequested = true;
			}
		}

		ImGui::Separator();
		bool bLogVisible = Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen;
		if (ToolUIMenuItem("Output Log", EToolUIMenuIcon::Log, &bLogVisible))
		{
			Implementation->bOutputLogOpen = bLogVisible;
			Implementation->bBottomPanelOpen |= bLogVisible;
		}

		ToolUIMenuItem("Performance", EToolUIMenuIcon::Performance, &Implementation->bPerformanceOpen);
	}, [&]
	{
		const float Scale = ImGui::GetFontSize() / Implementation->ToolUI->GetMetrics().BaseFontSize;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * Scale, (26.f * Scale - ImGui::GetFontSize()) * 0.5f});
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * Scale);
		ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});

		const auto AppearanceButton = [Scale]()
		{
			constexpr const char* Label = "Appearance";
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1, 1, 1, 0.04f});
			ImGui::PushID(Label);
			const bool bPressed = ImGui::Button("##Status", {ImGui::CalcTextSize(Label).x + 42.f * Scale, 26.f * Scale});
			const ImVec2 Minimum = ImGui::GetItemRectMin();
			const ImVec2 Center{Minimum.x + 16.f * Scale, Minimum.y + 13.f * Scale};
			ImDrawList* const DrawList = ImGui::GetWindowDrawList();
			const ImU32 Color = ImGui::GetColorU32(ImGuiCol_Text);

			for (int Row = -1; Row <= 1; ++Row)
			{
				const float Y = Center.y + static_cast<float>(Row) * 4.f * Scale;
				DrawList->AddLine({Center.x - 6.f * Scale, Y}, {Center.x + 6.f * Scale, Y}, Color, Scale);
				const float X = Center.x + (Row == 0 ? 2.f : -2.f) * Scale;
				DrawList->AddLine({X, Y - 2.f * Scale}, {X, Y + 2.f * Scale}, Color, 2.f * Scale);
			}

			DrawList->AddText({Minimum.x + 30.f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, Color, Label);
			ImGui::PopID();
			ImGui::PopStyleColor();
			return bPressed;
		};

		ImGui::PushStyleColor(ImGuiCol_Button, Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
		if (ToolUIButton("Content Browser", EToolUIMenuIcon::ContentBrowser, 26.f * Scale))
		{
			ToggleContentBrowser(Implementation->bContentBrowserOpen, Implementation->ContentBrowserState);
			Implementation->bBottomPanelOpen = true;
		}

		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("Show or hide Content Browser. Ctrl+Space toggles the entire bottom panel.");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
		if (ToolUIButton("Output Log", EToolUIMenuIcon::Log, 26.f * Scale))
		{
			Implementation->bOutputLogOpen = !(Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen);
			Implementation->bBottomPanelOpen = true;
		}

		ImGui::PopStyleColor();
		ImGui::SameLine();
		const ImVec2 DotPosition = ImGui::GetCursorScreenPos();
		ImGui::Dummy({8.f * Scale, 26.f * Scale});
		ImGui::SameLine(0.f, 3.f * Scale);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(Implementation->bProjectBusy ? "Loading project..." : Implementation->Assets && Implementation->Assets->IsImporting() ? "Importing..."
		                                                                                                                                          : "Ready");
		if (Implementation->bProjectBusy)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Cancel"))
			{
				Implementation->ProjectCancellation.request_stop();
			}
		}
		const float ReadyCenterY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
		ImGui::GetWindowDrawList()->AddCircleFilled({DotPosition.x + 4.f * Scale, ReadyCenterY}, 2.5f * Scale, IM_COL32(164, 189, 148, 255));
		ImGui::SameLine();
		ImGui::TextDisabled("%zu objects", Implementation->PreviewObjects.size());
		const float Width = ImGui::CalcTextSize("Appearance").x + 42.f * Scale;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - Width - 12.f));

		if (AppearanceButton())
		{
			ImGui::OpenPopup("WorkspaceAppearance");
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(3);
		// Fixed width so right-aligned fields do not depend on auto-fit content.
		ImGui::SetNextWindowSize({320.f * Scale, 0.f});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.f * Scale, 12.f * Scale});
		const bool bAppearanceOpen = ImGui::BeginPopup("WorkspaceAppearance");
		ImGui::PopStyleVar();

		if (bAppearanceOpen)
		{
			FEditorAppearance Appearance = Implementation->ToolUI->GetAppearance();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ImGui::GetStyle().FramePadding.x, 3.f * Scale});
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Appearance");
			const float ResetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ResetWidth);
			ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

			if (ImGui::Button("Reset"))
			{
				// Display settings describe the monitor, not the look, so they survive a reset.
				const FEditorDisplay Display = Appearance.Display;
				Appearance = FEditorAppearance{};
				Appearance.Display = Display;
			}

			ImGui::PopStyleColor(2);
			ImGui::SetItemTooltip("Restore default appearance");
			ImGui::Separator();
			const float ValueWidth = 140.f * Scale;
			DrawFieldLabel("Panel transparency", ValueWidth);
			const char* const PanelMode = GetPanelTransparencyLabel(Appearance.PanelTransparency);

			if (ImGui::BeginCombo("##PanelTransparency", PanelMode))
			{
				constexpr std::array Modes = {
				    std::pair{EPanelTransparency::AllPanels, "All panels"},
				    std::pair{EPanelTransparency::FloatingOnly, "Floating panels"},
				    std::pair{EPanelTransparency::DockedOnly, "Docked panels"},
				    std::pair{EPanelTransparency::Disabled, "Opaque panels"},
				};

				for (const auto& [Mode, Label] : Modes)
				{
					if (ImGui::Selectable(Label, Appearance.PanelTransparency == Mode))
					{
						Appearance.PanelTransparency = Mode;
					}
				}

				ImGui::EndCombo();
			}

			float OpacityPercent = Appearance.PanelOpacity * 100.f;
			DrawFieldLabel("Panel opacity", ValueWidth);

			if (DrawNumericSliderFloat("##Opacity", &OpacityPercent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
			{
				Appearance.PanelOpacity = OpacityPercent / 100.f;
			}

			DrawFieldLabel("Panel blur", ValueWidth);
			DrawNumericSliderFloat("##Blur", &Appearance.BlurRadius, 0.f, 40.f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
			ImGui::Separator();

			float GradientHeightPercent = Appearance.GradientHeight * 100.f;
			float SaturationPercent = Appearance.Saturation * 100.f;
			float IntensityPercent = Appearance.Intensity * 100.f;
			DrawFieldLabel("Gradient height", ValueWidth);

			if (DrawNumericSliderFloat("##GradientHeight", &GradientHeightPercent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
			{
				Appearance.GradientHeight = GradientHeightPercent / 100.f;
			}

			DrawFieldLabel("Color saturation", ValueWidth);

			if (DrawNumericSliderFloat("##Saturation", &SaturationPercent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
			{
				Appearance.Saturation = SaturationPercent / 100.f;
			}

			DrawFieldLabel("Color intensity", ValueWidth);

			if (DrawNumericSliderFloat("##Intensity", &IntensityPercent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
			{
				Appearance.Intensity = IntensityPercent / 100.f;
			}

			std::array Color = {
			    static_cast<float>(Appearance.Accent.Red) / 255.f,
			    static_cast<float>(Appearance.Accent.Green) / 255.f,
			    static_cast<float>(Appearance.Accent.Blue) / 255.f,
			};

			DrawFieldLabel("Accent color", ValueWidth);

			if (ImGui::ColorEdit3("##AccentColor", Color.data()))
			{
				Appearance.Accent.Red = static_cast<std::uint8_t>(std::lround(Color[0] * 255.f));
				Appearance.Accent.Green = static_cast<std::uint8_t>(std::lround(Color[1] * 255.f));
				Appearance.Accent.Blue = static_cast<std::uint8_t>(std::lround(Color[2] * 255.f));
			}

			for (std::size_t Index = 0; Index < ToolUITheme::Presets.size(); ++Index)
			{
				if (Index > 0)
				{
					ImGui::SameLine();
				}

				const FToolUIColorPreset& Preset = ToolUITheme::Presets[Index];
				ImGui::PushID(static_cast<int>(Index));

				if (ImGui::ColorButton("##Preset", ImGui::ColorConvertU32ToFloat4(PackColor(Preset.Color)), ImGuiColorEditFlags_NoTooltip, {22.f * Scale, 22.f * Scale}))
				{
					Appearance.Accent = Preset.Color;
				}

				if (ImGui::IsItemHovered())
				{
					const char* const PresetName = Preset.Name.data();
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(PresetName, PresetName + Preset.Name.size());
					ImGui::EndTooltip();
				}

				ImGui::PopID();
			}

			Implementation->DrawHdrDisplaySettings(Appearance.Display, ValueWidth);
			ImGui::PopStyleVar();
			Implementation->ToolUI->SetAppearance(Appearance);
			ImGui::EndPopup();
		}
		else
		{
			Implementation->bHdrCalibration = false;
		}
	}, [&]
	{
		ImGui::TextUnformatted("Project");
		ImGui::Separator();
		ImGui::BeginDisabled(!bProjectChangeAvailable);
		bNewProjectRequested |= ToolUIMenuItem("New project...", EToolUIMenuIcon::Add, nullptr, "Ctrl+N");
		bOpenProjectRequested |= ToolUIMenuItem("Open project...", EToolUIMenuIcon::Open, nullptr, "Ctrl+O");
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextUnformatted("Level");
		ImGui::Separator();
		ImGui::BeginDisabled(!bAuthoringAvailable);
		bOpenLevelRequested = ToolUIMenuItem("Open level...", EToolUIMenuIcon::Open);
		bSaveLevelRequested |= ToolUIMenuItem("Save level", EToolUIMenuIcon::Save, nullptr, "Ctrl+S");
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextUnformatted("Content");
		ImGui::Separator();
		ImGui::BeginDisabled(!Implementation->Assets || Implementation->bProjectBusy);
		bImportRequested |= ToolUIMenuItem("Import...", EToolUIMenuIcon::Import, nullptr, "Ctrl+I");
		ImGui::EndDisabled();
	}, [&]
	{
		ImGui::BeginDisabled(!bAuthoringAvailable);
		ImGui::BeginDisabled(!Implementation->Level->CanUndo());
		const std::string UndoLabel = Implementation->Level->CanUndo() ? std::format("Undo {}", Implementation->Level->GetUndoLabel()) : "Undo";
		if (ToolUIMenuItem(UndoLabel, EToolUIMenuIcon::Undo, nullptr, "Ctrl+Z"))
		{
			AuthoringAction = EAuthoringAction::Undo;
		}

		ImGui::EndDisabled();
		ImGui::BeginDisabled(!Implementation->Level->CanRedo());
		const std::string RedoLabel = Implementation->Level->CanRedo() ? std::format("Redo {}", Implementation->Level->GetRedoLabel()) : "Redo";
		if (ToolUIMenuItem(RedoLabel, EToolUIMenuIcon::Redo, nullptr, "Ctrl+Y"))
		{
			AuthoringAction = EAuthoringAction::Redo;
		}

		ImGui::EndDisabled();
		ImGui::Separator();
		ImGui::BeginDisabled(Implementation->PreviewSelection.Indices.empty());
		for (const auto& [Label, Icon, Shortcut, Action] : std::array{
		         std::tuple{"Duplicate", EToolUIMenuIcon::Duplicate, "Ctrl+D", EAuthoringAction::Duplicate},
		         std::tuple{"Copy", EToolUIMenuIcon::Copy, "Ctrl+C", EAuthoringAction::Copy},
		         std::tuple{"Delete", EToolUIMenuIcon::Delete, "Delete", EAuthoringAction::Delete},
		     })
		{
			if (ToolUIMenuItem(Label, Icon, nullptr, Shortcut))
			{
				AuthoringAction = Action;
			}
		}

		ImGui::EndDisabled();
		if (ToolUIMenuItem("Paste", EToolUIMenuIcon::Paste, nullptr, "Ctrl+V"))
		{
			AuthoringAction = EAuthoringAction::Paste;
		}

		ImGui::EndDisabled();
	});

	ImGui::BeginDisabled(!bAuthoringAvailable);
	if (const auto Placement = DrawPlaceObjectsMenu(*Implementation->ToolUI, Implementation->PlaceObjectsMenuState, bPlaceObjectsRequested))
	{
		const FVector3 Pivot = Implementation->ViewportCamera.GetPivot();
		const FWorldPosition Position{Pivot.X, Pivot.Y, Pivot.Z};
		const auto Created = [&]() -> std::expected<FObjectId, FLevelError>
		{
			switch (*Placement)
			{
				case EPlaceObjectType::EmptyEntity:
					return Implementation->Level->CreateEmptyEntity(Position);
				case EPlaceObjectType::Cube:
					return Implementation->Level->CreateEntity(Position);
				case EPlaceObjectType::DirectionalLight:
					return Implementation->Level->CreateLightEntity(ELightType::Directional, Position);
				case EPlaceObjectType::SkyLight:
					return Implementation->Level->CreateLightEntity(ELightType::Sky, Position);
				case EPlaceObjectType::PointLight:
					return Implementation->Level->CreateLightEntity(ELightType::Point, Position);
				case EPlaceObjectType::SpotLight:
					return Implementation->Level->CreateLightEntity(ELightType::Spot, Position);
				case EPlaceObjectType::RectLight:
					return Implementation->Level->CreateLightEntity(ELightType::Rect, Position);
				case EPlaceObjectType::SkyAtmosphere:
					return Implementation->Level->CreateSkyAtmosphereEntity(Position);
				case EPlaceObjectType::HeightFog:
					return Implementation->Level->CreateHeightFogEntity(Position);
				case EPlaceObjectType::Rope:
					return Implementation->Level->CreateSoftBodyEntity(ESoftBodyShape::Rope, Position);
				case EPlaceObjectType::Cloth:
					return Implementation->Level->CreateSoftBodyEntity(ESoftBodyShape::Cloth, Position);
				case EPlaceObjectType::SoftBall:
					return Implementation->Level->CreateSoftBodyEntity(ESoftBodyShape::Ball, Position);
				case EPlaceObjectType::TriggerVolume:
					return Implementation->Level->CreateTriggerEntity(Position);
			}

			return std::unexpected(FLevelError{"Unsupported placement type"});
		}();

		if (!Created)
		{
			Implementation->ReportLevelResult(std::unexpected(Created.error()));
		}

		Implementation->RefreshLevel();
	}

	ImGui::EndDisabled();
	Implementation->ApplyAuthoringAction(AuthoringAction);

	if (const auto Closed = Implementation->MaterialPanel.TakeCloseResult(); Closed)
	{
		if (Implementation->bCloseRequested)
		{
			Implementation->bCloseConfirmed = *Closed;
			Implementation->bCloseRequested = false;
		}
		else if (Implementation->bWaitingForMaterialClose)
		{
			bOpenProjectRequested = *Closed && Implementation->PendingDocumentAction == 2;
			bNewProjectRequested = *Closed && Implementation->PendingDocumentAction == 3;
			Implementation->bWaitingForMaterialClose = false;
		}
	}

	if ((bOpenProjectRequested || bNewProjectRequested) && Implementation->MaterialPanel.IsOpen())
	{
		Implementation->PendingDocumentAction = bNewProjectRequested ? 3 : 2;
		Implementation->bWaitingForMaterialClose = !Implementation->MaterialPanel.RequestClose();
		if (Implementation->bWaitingForMaterialClose)
		{
			bOpenProjectRequested = false;
			bNewProjectRequested = false;
		}
	}

	if (bOpenLevelRequested || bOpenProjectRequested || bNewProjectRequested)
	{
		Implementation->PendingDocumentAction = bNewProjectRequested ? 3 : bOpenProjectRequested ? 2
		                                                                                         : 1;
		if (Implementation->Level->IsDirty())
		{
			ImGui::OpenPopup("Unsaved level changes");
		}
		else
		{
			if (bNewProjectRequested)
			{
				ImGui::OpenPopup("New project");
			}
			else if (bOpenProjectRequested)
			{
				Implementation->OpenProjectWithDialog();
			}
			else
			{
				Implementation->OpenLevelWithDialog();
			}
			Implementation->RefreshLevel();
		}
	}

	bool bOpenNewProjectPopup = false;

	if (ImGui::BeginPopupModal("Unsaved level changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("Save level changes before continuing?");
		ImGui::Spacing();
		bool bContinueOpening = false;
		if (ImGui::Button("Save"))
		{
			Implementation->SaveCurrentLevel();
			bContinueOpening = !Implementation->Level->IsDirty();
		}

		ImGui::SameLine();
		if (ImGui::Button("Discard"))
		{
			bContinueOpening = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
		}

		if (bContinueOpening)
		{
			ImGui::CloseCurrentPopup();
			if (Implementation->PendingDocumentAction == 3)
			{
				bOpenNewProjectPopup = true;
			}
			else if (Implementation->PendingDocumentAction == 2)
			{
				Implementation->OpenProjectWithDialog();
			}
			else
			{
				Implementation->OpenLevelWithDialog();
			}
			Implementation->RefreshLevel();
		}

		ImGui::EndPopup();
	}

	if (bOpenNewProjectPopup)
	{
		ImGui::OpenPopup("New project");
	}
	Implementation->DrawNewProject();

	if (bSaveLevelRequested)
	{
		Implementation->SaveCurrentLevel();
	}

	// The native dialog is modal and blocks this frame, like Unreal's import dialog, so nothing outlives editor shutdown.
	if (bImportRequested)
	{
		Implementation->ImportWithDialog();
	}

	const auto InspectorStart = std::chrono::steady_clock::now();
	if (Implementation->bOutlinerOpen)
	{
		Implementation->DrawOutlinerPanel();
	}

	if (Implementation->bDetailsOpen)
	{
		Implementation->DrawDetailsPanel();
	}

	Implementation->FrameMetrics.InspectorMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - InspectorStart).count();
	if ((!Implementation->bBottomPanelOpen || !Implementation->bContentBrowserOpen) && Implementation->Assets)
	{
		Implementation->Assets->SetThumbnailAssets({});
	}

	if (Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen)
	{
		auto* const Assets = Implementation->Assets.get();
		ImGui::BeginDisabled(Implementation->bProjectBusy);
		if (DrawContentBrowser(*Implementation->ToolUI, Implementation->bContentBrowserOpen, Implementation->ContentBrowserState, Assets) && Assets)
		{
			Implementation->ImportWithDialog();
		}

		ImGui::EndDisabled();
	}

	Implementation->DrawViewport(RenderViewport);
	Implementation->DrawMaterialPanel();
	Implementation->DrawPerformancePanel();
	if (!Implementation->Simulation.IsRunning())
	{
		if (Implementation->bViewportEditCanceled && Implementation->Level->HasActiveEdit())
		{
			Implementation->ReportLevelResult(Implementation->Level->CancelEdit());
		}
		else if (Implementation->bViewportEditFinished && Implementation->Level->HasActiveEdit())
		{
			Implementation->ReportLevelResult(Implementation->Level->EndEdit());
		}
		else if (Implementation->Level->HasActiveEdit() && !Implementation->PreviewDragStart && !ImGui::IsAnyItemActive() && !IO.WantTextInput && !Implementation->OutlinerPanelState.bRenaming)
		{
			Implementation->ReportLevelResult(Implementation->Level->EndEdit());
		}
		else
		{
			Implementation->ReportLevelResult(Implementation->Level->CommitEdits());
		}

		Implementation->bViewportEditFinished = false;
		Implementation->bViewportEditCanceled = false;
		Implementation->RefreshLevel();
	}

	auto LogResult = Implementation->DrawOutputLog();

	if (Implementation->bRestoreBottomPanelFocus)
	{
		// Newly re-added dock tabs auto-select; restore focus after both windows are registered.
		Implementation->ContentBrowserState.bFocusRequested = Implementation->bContentBrowserOpen && (Implementation->bBottomBrowserSelected || !Implementation->bOutputLogOpen);
		Implementation->bFocusOutputLogRequested = !Implementation->ContentBrowserState.bFocusRequested && Implementation->bOutputLogOpen;
		Implementation->bRestoreBottomPanelFocus = false;
	}

	return LogResult;
}

FOutputLogModel& FEditorFramework::GetOutputLog() noexcept
{
	return *Implementation->OutputLog;
}

const FOutputLogModel& FEditorFramework::GetOutputLog() const noexcept
{
	return *Implementation->OutputLog;
}

bool FEditorFramework::RequestClose()
{
	if (Implementation->bCloseConfirmed)
	{
		return true;
	}

	if (Implementation->bCloseRequested)
	{
		return false;
	}

	Implementation->bWaitingForMaterialClose = false;
	Implementation->PendingMaterialOpen = {};
	Implementation->bCloseRequested = !Implementation->MaterialPanel.RequestClose();
	Implementation->bCloseConfirmed = !Implementation->bCloseRequested;
	return Implementation->bCloseConfirmed;
}

bool FEditorFramework::HasConfirmedClose() const noexcept
{
	return Implementation->bCloseConfirmed;
}

void FEditorFramework::SetViewportImage(const std::uint64_t TextureId) noexcept
{
	Implementation->ViewportTexture = TextureId;
}

void FEditorFramework::SetDisplayState(const FEditorDisplayState& State) noexcept
{
	Implementation->DisplayState = State;
}

FExtent2D FEditorFramework::GetViewportExtent() const noexcept
{
	return Implementation->ViewportExtent;
}

FMeshRenderView FEditorFramework::GetViewportRenderView() const noexcept
{
	// The renderer follows the swapchain that will present this frame, not the preference, which may be waiting for an HDR display.
	FMeshRenderView View = Implementation->ViewportRenderView;
	const FEditorDisplayState& Display = Implementation->DisplayState;
	if (Display.bHdrActive)
	{
		View.Visuals.HdrDisplay = FHdrDisplaySettings{.PaperWhite = Display.PaperWhite, .PeakLuminance = Display.PeakLuminance, .bCalibrationPattern = Implementation->bHdrCalibration};
	}

	return View;
}

std::span<const FDebugDrawList> FEditorFramework::GetViewportDebugDrawLists() const noexcept
{
	return Implementation->ViewportDebugDrawLists;
}

void FEditorFramework::FImplementation::FocusPreview()
{
	if (PreviewSelection.Active < 0)
	{
		return;
	}

	constexpr float Infinity = std::numeric_limits<float>::infinity();
	FVector3 Minimum{Infinity, Infinity, Infinity};
	FVector3 Maximum{-Infinity, -Infinity, -Infinity};

	for (const int Index : PreviewSelection.Indices)
	{
		const FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
		const FMatrix4 Model = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale)) * GetPreviewBoundsMatrix(static_cast<std::size_t>(Index));

		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			const float Extent = std::abs(Model(Axis, 0)) + std::abs(Model(Axis, 1)) + std::abs(Model(Axis, 2));
			Minimum[Axis] = std::min(Minimum[Axis], Model(Axis, 3) - Extent);
			Maximum[Axis] = std::max(Maximum[Axis], Model(Axis, 3) + Extent);
		}
	}

	const float AspectRatio = ViewportExtent.Height > 0 ? static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height) : 16.f / 9.f;
	ViewportCamera.Focus((Minimum + Maximum) * 0.5f, (Maximum - Minimum) * 0.5f, AspectRatio, ViewportVisibleSize);
}

void FEditorFramework::FImplementation::ImportWithDialog()
{
	if (!Assets || bProjectBusy)
	{
		return;
	}

	FFileDialogFilter Importable{.Name = "Importable assets", .Extensions = {}};

	for (const std::string_view Extension : GetImportableExtensions())
	{
		Importable.Extensions.emplace_back(Extension.substr(1));
	}

	std::expected<std::vector<std::filesystem::path>, FFileDialogError> Chosen = OpenFilesDialog("Import assets", std::span(&Importable, 1));
	if (!Chosen)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not open the file dialog: {}", Chosen.error().Message);
		return;
	}

	Assets->ImportFiles(std::move(*Chosen), ContentBrowserState.GetImportDestination());
}

void FEditorFramework::FImplementation::OpenProjectWithDialog()
{
	const FFileDialogFilter Filter{.Name = "Herta project", .Extensions = {"hertaproject"}};
	const auto Chosen = OpenFilesDialog("Open project", std::span(&Filter, 1), ProjectPath.empty() ? EngineRoot / "Games" : ProjectPath.parent_path());
	if (!Chosen)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not open project dialog: {}", Chosen.error().Message);
	}
	else if (!Chosen->empty())
	{
		StartProjectOperation(Chosen->front());
	}
}

void FEditorFramework::FImplementation::StartProjectOperation(const std::filesystem::path& Path, std::optional<FCreateProjectRequest> Create)
{
	if (MaterialPanel.IsOpen() && MaterialPanel.IsDirty())
	{
		ProjectError = "Save or discard material edits before changing projects.";
		return;
	}

	MaterialPanel.RequestClose();
	PendingMaterialOpen = {};
	if (bProjectBusy || !Tasks || Simulation.IsRunning() || Level->HasActiveEdit())
	{
		return;
	}

	if (Assets && Assets->IsImporting())
	{
		ProjectError = "Wait for the current import to finish before switching projects";
		HERTA_LOG_WARNING(*Log, EditorLog, "{}", ProjectError);
		return;
	}

	if (!ProjectScope)
	{
		auto Scope = Tasks->CreateScope("Project operations");
		if (!Scope)
		{
			ProjectError = Scope.error().Message;
			return;
		}

		ProjectScope = std::move(*Scope);
	}

	struct FLoadResult
	{
		std::expected<FLoadedProject, FProjectError> Project = std::unexpected(FProjectError{"Project operation did not run"});
		std::expected<FLevelDocument, FLevelError> Document = std::unexpected(FLevelError{"Starting level did not load"});
	};

	ProjectCancellation = {};
	ProjectError.clear();
	if (Create)
	{
		Create->StopToken = ProjectCancellation.get_token();
	}
	const auto Before = Level->GetWorld().SnapshotEntities();
	const auto BeforePath = Level->GetPath();
	const auto BeforeGeneration = Level->GetGeneration();
	auto Result = std::make_shared<FLoadResult>();
	const auto StopToken = ProjectCancellation.get_token();
	auto Work = Tasks->Submit(*ProjectScope, {.Name = Create ? "Create project" : "Load project", .Lane = ETaskLane::BlockingIo}, [Result, Path, Create = std::move(Create), StopToken](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested() || StopToken.stop_requested())
		{
			return;
		}
		Result->Project = Create ? CreateProject(*Create) : LoadProject(Path);
		if (Result->Project && !StopToken.stop_requested())
		{
			Result->Document = LoadLevel(Result->Project->StartingLevel);
		}
	});

	if (!Work)
	{
		ProjectError = Work.error().Message;
		return;
	}

	bProjectBusy = true;
	const auto Report = Tasks->ContinueOnMainThread(*ProjectScope, *Work, "Publish project", [this, Result, Before, BeforePath, BeforeGeneration, StopToken](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}
		bProjectBusy = false;
		if (StopToken.stop_requested())
		{
			ProjectError = "Project operation cancelled";
		}
		else if (!Result->Project)
		{
			ProjectError = Result->Project.error().Message;
		}
		else if (!Result->Document)
		{
			ProjectError = Result->Document.error().Message;
		}
		else if (Level->GetGeneration() != BeforeGeneration || Level->GetPath() != BeforePath || Level->GetWorld().SnapshotEntities() != Before)
		{
			ProjectError = "The level changed while loading. Open the project again.";
		}
		else if (auto Loaded = Level->LoadDocument(std::move(*Result->Document), Result->Project->StartingLevel); !Loaded)
		{
			ProjectError = Loaded.error().Message;
		}
		else
		{
			Assets.reset();
			AssetPaths.ContentRoot = Result->Project->ContentRoot;
			AssetPaths.DerivedDataRoot = Result->Project->Root / "DerivedDataCache" / AssetPaths.TargetPlatform;
			ProjectPath = Result->Project->DescriptorPath;
			ContentBrowserState = {};
			DetailsPanelState = {};
			OutlinerPanelState = {};
			bCloseProjectPopup = true;
			RefreshLevel();
			HERTA_LOG_INFO(*Log, EditorLog, "Opened project '{}'", Result->Project->Descriptor.Name);
		}

		if (!ProjectError.empty())
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "{}", ProjectError);
		}
	});

	if (!Report)
	{
		ProjectCancellation.request_stop();
		bProjectBusy = false;
		ProjectError = Report.error().Message;
	}
}

void FEditorFramework::FImplementation::DrawNewProject()
{
	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	ImGui::SetNextWindowSize({440.f * Scale, 0.f}, ImGuiCond_Appearing);
	ImGui::SetNextWindowSizeConstraints({440.f * Scale, 0.f}, {440.f * Scale, std::numeric_limits<float>::max()});
	if (!ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	if (ImGui::IsWindowAppearing())
	{
		ProjectError.clear();
		ProjectName.fill('\0');
		ProjectModule.fill('\0');
		ProjectDestination.fill('\0');
		const auto Directory = (EngineRoot / "Games/NewGame").generic_u8string();
		std::ranges::copy(Directory | std::views::take(ProjectDestination.size() - 1), ProjectDestination.begin());
	}

	ImGui::TextUnformatted("Game project");
	ImGui::TextDisabled("C++ module, content folder, and an empty starting level.");
	ImGui::Separator();
	ImGui::BeginDisabled(bProjectBusy);
	ImGui::TextUnformatted("Name");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##ProjectName", "My Game", ProjectName.data(), ProjectName.size());
	ImGui::TextUnformatted("C++ module");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##ProjectModule", "MyGame", ProjectModule.data(), ProjectModule.size());
	ImGui::TextUnformatted("Location (new directory)");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##ProjectDestination", ProjectDestination.data(), ProjectDestination.size());
	ImGui::EndDisabled();
	if (!ProjectError.empty())
	{
		ImGui::TextWrapped("%s", ProjectError.c_str());
	}
	ImGui::Separator();
	ImGui::BeginDisabled(bProjectBusy || ProjectName[0] == '\0' || ProjectModule[0] == '\0' || ProjectDestination[0] == '\0');
	if (ImGui::Button("Create and open", {150.f * Scale, 0.f}))
	{
		const std::string_view Destination(ProjectDestination.data());
		StartProjectOperation({}, FCreateProjectRequest{.TemplateRoot = EngineRoot / "Templates/Projects/Game", .Destination = std::filesystem::path(std::u8string(Destination.begin(), Destination.end())), .Name = ProjectName.data(), .ModuleName = ProjectModule.data()});
	}

	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(bProjectBusy ? "Cancel operation" : "Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		if (bProjectBusy)
		{
			ProjectCancellation.request_stop();
		}
		else
		{
			ImGui::CloseCurrentPopup();
		}
	}

	if (bProjectBusy)
	{
		ImGui::TextDisabled("Creating project...");
	}
	if (std::exchange(bCloseProjectPopup, false))
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

const FRenderMesh* FEditorFramework::FImplementation::RefreshSoftBodyPreview(const std::size_t Index, const FLevelEntity& Entity)
{
	FSoftBodyPreview& Preview = SoftBodyPreviews[Entity.Id];
	const bool bRebuildTopology = !Preview.Mesh || Preview.Settings != *Entity.SoftBody;
	if (bRebuildTopology)
	{
		Preview.Settings = *Entity.SoftBody;
		Preview.Topology = BuildSoftBodyTopology(Preview.Settings);
	}

	const std::span<const FVector3> Simulated = Simulation.IsRunning() ? Simulation.GetSoftBodyPositions(Index) : std::span<const FVector3>{};
	const std::uint64_t Revision = Simulated.empty() ? 0 : Simulation.GetSoftBodyRevision();
	if ((!bRebuildTopology && Preview.Revision == Revision) || GraphicsDevice == nullptr)
	{
		return Preview.Mesh.get();
	}

	// Simulated vertices arrive in world space; the entity keeps its authored transform while it simulates.
	std::vector<FVector3> Local = Preview.Topology.Vertices;
	if (Simulated.size() == Local.size())
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		for (std::size_t Vertex = 0; Vertex < Local.size(); ++Vertex)
		{
			const Im3d::Vec3 Delta = ToIm3dVector(Simulated[Vertex]) - Object.Translation;
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				const float Rotated = Object.Rotation(0, Axis) * Delta.x + Object.Rotation(1, Axis) * Delta.y + Object.Rotation(2, Axis) * Delta.z;
				const float Scale = Axis == 0 ? Object.Scale.x : Axis == 1 ? Object.Scale.y
				                                                           : Object.Scale.z;
				Local[Vertex][static_cast<std::size_t>(Axis)] = std::abs(Scale) > 1e-6f ? Rotated / Scale : 0.f;
			}
		}
	}

	FCookedModel Model = BuildSoftBodyModel(Preview.Settings, Preview.Topology, Local);
	if (!bRebuildTopology)
	{
		// Creating a mesh per step opened extra GPU recordings that stalled on the previous frame; the frame's own recording uploads the vertices instead.
		// The mesh keeps its rest-pose bounds, which also size the physics shape when simulation starts.
		const FRenderMesh* const Target = Preview.Mesh.get();
		std::erase_if(SoftBodyVertexUpdates, [Target](const FRenderMeshVertexUpdate& Update)
		{
			return Update.Mesh == Target;
		});

		Preview.Vertices = std::move(Model.Vertices);
		Preview.Revision = Revision;
		SoftBodyVertexUpdates.push_back({.Mesh = Target, .Vertices = Preview.Vertices});
		return Target;
	}

	auto Mesh = FRenderMesh::Create(*GraphicsDevice, Model, Entity.Name);
	if (!Mesh)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not build soft body mesh for {}: {}", Entity.Name, Mesh.error().Message);
		return Preview.Mesh.get();
	}

	Preview.Mesh = std::move(*Mesh);
	Preview.Revision = Revision;
	return Preview.Mesh.get();
}

void FEditorFramework::FImplementation::RefreshPreviewMeshes()
{
	for (std::size_t Index = 0; Index < PreviewMeshes.size(); ++Index)
	{
		PreviewMeshes[Index] = Assets && PreviewObjects[Index].Mesh.IsValid() ? Assets->GetSlot(Index).Mesh.get() : nullptr;
	}

	RefreshVisuals();
}

// Property edits inside an active gesture change the world without a level generation bump, so the render snapshot follows the selection directly.
void FEditorFramework::FImplementation::RefreshSelectedVisualEntities()
{
	const FWorld& World = Level->GetWorld();
	for (const FObjectId Selected : Level->GetSelection())
	{
		const auto Visual = std::ranges::find(VisualEntities, Selected, &FLevelEntity::Id);
		const auto Handle = World.FindEntity(Selected);
		if (Visual == VisualEntities.end() || !Handle)
		{
			continue;
		}

		if (auto Entity = World.GetEntity(*Handle))
		{
			*Visual = std::move(*Entity);
		}
	}
}

void FEditorFramework::FImplementation::DrawHdrDisplaySettings(FEditorDisplay& Display, const float ValueWidth)
{
	ImGui::Separator();
	DrawFieldLabel("HDR output", ValueWidth);
	ImGui::BeginDisabled(!DisplayState.bHdrSupported && !Display.bHdrOutput);
	ToolUIToggle("##HdrOutput", &Display.bHdrOutput);
	ImGui::EndDisabled();

	if (!DisplayState.bHdrSupported)
	{
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("%s", Display.bHdrOutput ? "Waiting for a display that offers HDR10. On Windows, turn on Use HDR in display settings." : "This display does not offer HDR10. On Windows, turn on Use HDR in display settings.");
		ImGui::PopTextWrapPos();
	}

	if (!Display.bHdrOutput || !DisplayState.bHdrActive)
	{
		bHdrCalibration = false;
		return;
	}

	float Peak = DisplayState.PeakLuminance;
	DrawFieldLabel("Peak brightness", ValueWidth);
	if (DrawNumericSliderFloat("##HdrPeak", &Peak, MinimumHdrPeakLuminance, MaximumHdrPeakLuminance, Display.PeakLuminance > 0.f ? "%.0f nits" : "%.0f nits (system)", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
	{
		Display.PeakLuminance = Peak;
	}

	float PaperWhite = DisplayState.PaperWhite;
	DrawFieldLabel("Paper white", ValueWidth);
	if (DrawNumericSliderFloat("##HdrPaperWhite", &PaperWhite, MinimumHdrPaperWhite, MaximumHdrPaperWhite, Display.PaperWhite > 0.f ? "%.0f nits" : "%.0f nits (system)", ImGuiSliderFlags_AlwaysClamp))
	{
		Display.PaperWhite = PaperWhite;
	}

	ImGui::SetItemTooltip("Brightness of white in the editor and of a white surface in the scene");
	DrawFieldLabel("Calibrate peak", ValueWidth);
	ToolUIToggle("##HdrCalibration", &bHdrCalibration);
	ImGui::SetItemTooltip("Shows a test pattern in the viewport. Raise Peak brightness until the inner square disappears into the outer one.");

	ImGui::BeginDisabled(Display.PaperWhite == 0.f && Display.PeakLuminance == 0.f);
	if (ImGui::Button("Use system values", {-1.f, 0.f}))
	{
		Display.PaperWhite = 0.f;
		Display.PeakLuminance = 0.f;
	}

	ImGui::EndDisabled();
	ImGui::SetItemTooltip(DisplayState.SystemPeakLuminance > 0.f ? "Follow the brightness the operating system reports for this display" : "The operating system reports no brightness here, so defaults of 200 and 1000 nits apply");
}

void FEditorFramework::FImplementation::RefreshVisuals()
{
	VisualSettings.ExposureEV100 = GetPhysicalCameraExposureEV100(PhysicalCamera);
	VisualSettings.bAutoExposure = bAutoExposure;
	VisualSettings.ExposureCompensation = ExposureCompensation;
	VisualSettings.DeltaSeconds = ImGui::GetIO().DeltaTime;
	ViewportCamera.SetVerticalFieldOfView(GetPhysicalCameraVerticalFieldOfView(PhysicalCamera.FocalLengthMillimeters));
	VisualSettings.bStudioPreview = std::ranges::none_of(VisualEntities, [](const FLevelEntity& Entity)
	{
		return Entity.Light.has_value();
	});
	RenderLights.clear();
	SunIds.clear();
	SunLabels.clear();
	AttachmentIds.clear();
	AttachmentLabels.clear();
	EnvironmentIds.clear();
	EnvironmentLabels.clear();
	VisualSettings.Atmosphere.reset();
	VisualSettings.Fog.reset();
	std::vector<FAssetId> Materials;
	std::vector<FAssetId> Environments;
	std::vector<std::string> ShaderPaths;
	if (MaterialPanel.IsOpen())
	{
		Materials.push_back(MaterialPanel.GetAsset());
	}

	PreviewMaterials.resize(PreviewObjects.size());
	PreviewMaterialSpans.resize(PreviewObjects.size());
	std::erase_if(SoftBodyPreviews, [this](const auto& Entry)
	{
		const bool bRemoved = std::ranges::none_of(VisualEntities, [&Entry](const FLevelEntity& Entity)
		{
			return Entity.Id == Entry.first && Entity.SoftBody && !Entity.Mesh;
		});

		if (bRemoved)
		{
			std::erase_if(SoftBodyVertexUpdates, [&Entry](const FRenderMeshVertexUpdate& Update)
			{
				return Update.Mesh == Entry.second.Mesh.get();
			});
		}

		return bRemoved;
	});

	for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewModels.size(); ++Index)
	{
		const FLevelEntity& Entity = VisualEntities[Index];
		PreviewMaterials[Index].clear();
		if (Entity.Mesh)
		{
			for (const FAssetId Material : Entity.Mesh->Materials)
			{
				const auto Preview = Assets && MaterialPanel.IsOpen() && Assets->GetMaterialPreviewAsset() == Material ? Assets->GetMaterialPreview() : std::shared_ptr<const FRenderMaterial>{};
				PreviewMaterials[Index].push_back(Preview ? Preview.get() : Assets ? Assets->GetMaterial(Material).get()
				                                                                   : nullptr);
				if (Material.IsValid())
				{
					Materials.push_back(Material);
				}

				if (const auto* Source = Assets ? Assets->GetMaterialSource(Material) : nullptr; Source && !Source->ShaderPath.empty())
				{
					const auto Options = Assets->GetOptions();
					const auto Option = std::ranges::find(Options, Material, &FPreviewAssetOption::Id);
					ShaderPaths.push_back(MaterialShaderKey(Source->ShaderPath, Option != Options.end() ? Option->Label : "Game/"));
				}
			}
		}

		if (Entity.SoftBody && !Entity.Mesh)
		{
			PreviewMeshes[Index] = RefreshSoftBodyPreview(Index, Entity);
			const FAssetId Material = Entity.SoftBody->Material;
			PreviewMaterials[Index].push_back(Assets && Material.IsValid() ? Assets->GetMaterial(Material).get() : nullptr);
			if (Material.IsValid())
			{
				Materials.push_back(Material);
			}
		}

		PreviewMaterialSpans[Index] = PreviewMaterials[Index];
		if (Entity.Mesh && Entity.BodyType == ELevelBodyType::Dynamic && !Entity.SoftBody)
		{
			AttachmentIds.push_back(Entity.Id);
			AttachmentLabels.push_back(Entity.Name);
		}

		if (Entity.Light)
		{
			if (Entity.Light->bEnabled)
			{
				RenderLights.push_back({.Settings = *Entity.Light, .Transform = PreviewModels[Index], .Id = Entity.Id});
			}

			if (Entity.Light->Type == ELightType::Directional)
			{
				SunIds.push_back(Entity.Id);
				SunLabels.push_back(Entity.Name);
			}

			if (Entity.Light->Type == ELightType::Sky && Entity.Light->bEnabled && Entity.Light->Environment.IsValid())
			{
				Environments.push_back(Entity.Light->Environment);
			}
		}

		if (!VisualSettings.Atmosphere && Entity.SkyAtmosphere && Entity.SkyAtmosphere->bEnabled)
		{
			VisualSettings.Atmosphere = Entity.SkyAtmosphere;
		}

		if (!VisualSettings.Fog && Entity.HeightFog && Entity.HeightFog->bEnabled)
		{
			VisualSettings.Fog = Entity.HeightFog;
			VisualSettings.FogHeight = PreviewModels[Index](1, 3);
		}
	}

	if (VisualSettings.Atmosphere && VisualSettings.Atmosphere->Sun.IsValid())
	{
		const FObjectId Sun = VisualSettings.Atmosphere->Sun;
		const auto Linked = std::ranges::find(RenderLights, Sun, &FRenderLight::Id);
		if (Linked != RenderLights.end() && Linked->Settings.Type == ELightType::Directional && Linked->Settings.bEnabled)
		{
			std::rotate(RenderLights.begin(), Linked, Linked + 1);
		}
	}

	EnabledLightCount = RenderLights.size();
	if (EnabledLightCount > MaximumRenderLights)
	{
		if (ReportedLightBudgetCount != EnabledLightCount)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "{} enabled lights exceed the viewport budget of {}; rendering the first stable object IDs and linked sun", EnabledLightCount, MaximumRenderLights);
		}

		RenderLights.resize(MaximumRenderLights);
	}

	ReportedLightBudgetCount = EnabledLightCount;

	if (Assets)
	{
		if (PendingMaterialOpen.IsValid())
		{
			Materials.push_back(PendingMaterialOpen);
		}

		Assets->SetMaterialAssets(Materials);
		Assets->SetTextureAssets(Environments);
		for (const FPreviewAssetOption& Option : Assets->GetOptions())
		{
			if (Option.Importer == "Texture")
			{
				EnvironmentIds.push_back(Option.Id);
				EnvironmentLabels.push_back(Option.Label);
			}
		}
	}

	ViewportRenderView.Lights = RenderLights;
	ViewportRenderView.Materials = PreviewMaterialSpans;
	ViewportRenderView.Visuals = VisualSettings;
	ViewportRenderView.VertexUpdates = SoftBodyVertexUpdates;
	if (GetBuildConfiguration() != EBuildConfiguration::Shipping && Tasks && MeshRenderer && !EngineRoot.empty())
	{
		if (!MaterialShaders || WatchedShaderContentRoot != AssetPaths.ContentRoot)
		{
			std::filesystem::path Worker = AssetPaths.WorkerPath;
			Worker.replace_filename(Worker.has_extension() ? "HertaShaderWorker.exe" : "HertaShaderWorker");
			MaterialShaders = FMaterialShaders::Create(*Tasks, *MeshRenderer, *Log, EngineRoot / "Engine/Shaders", Worker, AssetPaths.EngineContentRoot, AssetPaths.ContentRoot);
			WatchedShaderContentRoot = AssetPaths.ContentRoot;
		}

		if (Assets && MaterialPanel.IsOpen() && !MaterialPanel.GetDraft().ShaderPath.empty())
		{
			const auto Options = Assets->GetOptions();
			const auto Option = std::ranges::find(Options, MaterialPanel.GetAsset(), &FPreviewAssetOption::Id);
			ShaderPaths.push_back(MaterialShaderKey(MaterialPanel.GetDraft().ShaderPath, Option != Options.end() ? Option->Label : "Game/"));
		}

		if (MaterialShaders)
		{
			MaterialShaders->Tick(ShaderPaths);
		}
	}

	if (Assets && MeshRenderer)
	{
		Assets->SetMaterialShaderGeneration(MeshRenderer->GetMaterialShaderGeneration());
	}

	if (VisualEnvironment && ViewportExtent.Width > 0 && ViewportExtent.Height > 0)
	{
		const auto Uniforms = BuildVisualUniforms(ViewportRenderView.View, ViewportRenderView.Projection, ViewportExtent, RenderLights, VisualSettings);
		if (Uniforms)
		{
			const auto Source = Assets && !Environments.empty() ? Assets->GetCookedTexture(Environments.front()) : std::shared_ptr<const FCookedTexture>{};
			VisualEnvironment->Tick(Source, *Uniforms);
		}
	}
}

// The atmosphere's linked sun wins; otherwise the first enabled directional light drives time of day.
int FEditorFramework::FImplementation::FindTimeOfDaySun() const
{
	int Fallback = -1;
	for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewObjects.size(); ++Index)
	{
		const FLevelEntity& Entity = VisualEntities[Index];
		if (!Entity.Light || Entity.Light->Type != ELightType::Directional || !Entity.Light->bEnabled)
		{
			continue;
		}

		if (VisualSettings.Atmosphere && VisualSettings.Atmosphere->Sun == Entity.Id)
		{
			return static_cast<int>(Index);
		}

		Fallback = Fallback < 0 ? static_cast<int>(Index) : Fallback;
	}

	return Fallback;
}

void FEditorFramework::FImplementation::DrawTimeOfDay(const float X, const float Top, const float Width, const float ButtonSize, const float Scale)
{
	const int SunIndex = FindTimeOfDaySun();
	FPreviewObject& Sun = PreviewObjects[static_cast<std::size_t>(SunIndex)];
	const ImVec2 Center{X + ButtonSize * 0.5f, Top + ButtonSize * 0.5f};
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const ImU32 Color = ImGui::GetColorU32(ImGuiCol_Text);
	Draw->AddCircle(Center, 3.5f * Scale, Color, 16, 1.2f * Scale);
	for (int Ray = 0; Ray < 8; ++Ray)
	{
		const float Angle = static_cast<float>(Ray) * std::numbers::pi_v<float> * 0.25f;
		const ImVec2 Direction{std::cos(Angle), std::sin(Angle)};
		Draw->AddLine({Center.x + Direction.x * 5.5f * Scale, Center.y + Direction.y * 5.5f * Scale}, {Center.x + Direction.x * 7.5f * Scale, Center.y + Direction.y * 7.5f * Scale}, Color, 1.2f * Scale);
	}

	// The light travels along its local +Z, so the sun sits opposite it.
	const Im3d::Vec3 Travel = Sun.Rotation.getCol(2);
	float Hours = GetTimeOfDayHours(FVector3{-Travel.x, -Travel.y, -Travel.z}.Normalized());
	ImGui::SetCursorScreenPos({X + ButtonSize, Top});
	ImGui::SetNextItemWidth(Width);
	ImGui::BeginDisabled(Simulation.IsRunning() || bProjectBusy);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f * Scale, (ButtonSize - ImGui::GetFontSize()) * 0.5f});
	const bool bChanged = DrawNumericSliderFloat("##TimeOfDay", &Hours, 0.f, 24.f, FormatTimeOfDay(Hours).c_str(), ImGuiSliderFlags_AlwaysClamp, nullptr, ParseTimeOfDay);
	ImGui::PopStyleVar();

	// The edit opens on the first real change, not on activation: a click only opens typed entry, and an edit held open by it would block whatever the next click does.
	if (bChanged && !Level->HasActiveEdit())
	{
		ReportLevelResult(Level->BeginEdit("Time of day"));
	}

	if (bChanged)
	{
		const FVector3 Light = -GetTimeOfDaySunDirection(Hours);
		const FVector3 Reference = std::abs(Light.Y) > 0.999f ? FVector3::Forward() : FVector3::Up();
		const FVector3 Right = Reference.Cross(Light).Normalized();
		Sun.Rotation = Im3d::Mat3(ToIm3dVector(Right), ToIm3dVector(Light.Cross(Right)), ToIm3dVector(Light));
	}

	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Time of day. Moves %s along an east-to-west arc.", Sun.Label.c_str());
}

void FEditorFramework::FImplementation::DrawPerformancePanel()
{
	// Recorded while closed so the graphs already have history when the panel opens.
	const std::vector<FGpuPassTiming> Passes = MeshRenderer ? MeshRenderer->GetGpuTimings() : std::vector<FGpuPassTiming>{};
	const FPerformanceSample Sample{
	    .FrameMilliseconds = ImGui::GetIO().DeltaTime * 1000.,
	    .CpuMilliseconds = CpuFrameMilliseconds,
	    .GpuUIMilliseconds = GpuUIMilliseconds,
	    .Passes = Passes,
	    .EnabledLights = EnabledLightCount,
	    .MaximumLights = MaximumRenderLights,
	    .RenderTargetBytes = MeshRenderer ? MeshRenderer->GetRenderTargetBytes() : 0,
	    .DrawCount = MeshRenderer ? MeshRenderer->GetLastDrawCount() : 0,
	};

	RecordPerformanceSample(PerformanceState, Sample);
	if (bPerformanceOpen)
	{
		Herta::DrawPerformancePanel(*ToolUI, bPerformanceOpen, PerformanceState, Sample);
	}

	// Performance appears after Details and would take its tab at startup; Details stays the main tab without taking keyboard focus.
	if (bSelectDetailsTab)
	{
		const ImGuiWindow* const Details = ImGui::FindWindowByName("Details");
		if (Details && Details->DockNode && Details->DockNode->TabBar)
		{
			Details->DockNode->TabBar->NextSelectedTabId = Details->TabId;
			bSelectDetailsTab = false;
		}
		else if (!bDetailsOpen)
		{
			bSelectDetailsTab = false;
		}
	}
}

void FEditorFramework::FImplementation::DrawMaterialPanel()
{
	if (!Assets)
	{
		return;
	}

	if (const auto Requested = std::exchange(ContentBrowserState.OpenMaterialRequested, FAssetId{}); Requested.IsValid() && !bProjectBusy && !bWaitingForMaterialClose && !bCloseRequested)
	{
		PendingMaterialOpen = Requested;
		Assets->RequestMaterial(Requested);
	}

	if (PendingMaterialOpen.IsValid() && !bProjectBusy && !bWaitingForMaterialClose && !bCloseRequested)
	{
		if (const auto* Source = Assets->GetMaterialSource(PendingMaterialOpen))
		{
			const auto Options = Assets->GetOptions();
			const auto Option = std::ranges::find(Options, PendingMaterialOpen, &FPreviewAssetOption::Id);
			const std::string Label = Option == Options.end() ? "Material" : Option->Label;
			MaterialPanel.Open(PendingMaterialOpen, *Source, Label, Label.starts_with("Engine/"));
			DraftMaterialThumbnail = Assets->GetThumbnail(PendingMaterialOpen);
			PendingMaterialOpen = {};
		}
	}

	MaterialTextureOptions.clear();
	for (const FPreviewAssetOption& Option : Assets->GetOptions())
	{
		if (Option.Importer == "Texture")
		{
			MaterialTextureOptions.push_back({.Id = Option.Id, .Label = Option.Label});
		}
	}

	std::string ShaderStatus;
	if (GetBuildConfiguration() == EBuildConfiguration::Shipping)
	{
		ShaderStatus = MaterialPanel.GetDraft().ShaderPath.empty() ? "Shader iteration is unavailable in Shipping." : "Custom shaders are unavailable in Shipping. Using engine PBR.";
	}
	else if (MaterialShaders && MaterialPanel.IsOpen())
	{
		const auto Options = Assets->GetOptions();
		const auto Option = std::ranges::find(Options, MaterialPanel.GetAsset(), &FPreviewAssetOption::Id);
		const std::string Key = MaterialShaderKey(MaterialPanel.GetDraft().ShaderPath, Option != Options.end() ? Option->Label : "Game/");
		if (const auto* Status = MaterialShaders->GetStatus(Key))
		{
			ShaderStatus = Status->bCompiling ? "Compiling shader..." : !Status->Diagnostics.empty() ? Status->Diagnostics
			                                                        : Status->bReady                 ? "Shader ready"
			                                                                                         : "Waiting for shader";
		}
	}

	const FMaterialPanelContext Context{
	    .Textures = MaterialTextureOptions,
	    .PreviewTexture = Assets->GetMaterialPreviewAsset() == MaterialPanel.GetAsset() && Assets->GetMaterialPreviewThumbnail() ? *Assets->GetMaterialPreviewThumbnail() : DraftMaterialThumbnail ? *DraftMaterialThumbnail
	                                                                                                                                                                                               : 0,
	    .PreviewStatus = Assets->IsMaterialPreviewLoading() ? "Updating preview..." : Assets->GetMaterialPreviewError(),
	    .ShaderStatus = ShaderStatus,
	    .Save = [&](const FAssetId Id, const FMaterialAsset& Material)
	{
		return Assets->SaveMaterial(Id, Material);
	},
	    .Reload = [&](const FAssetId Id) -> std::expected<FMaterialAsset, FAssetError>
	{
		const auto Path = Assets->GetMaterialPath(Id);
		if (!Path)
		{
			return std::unexpected(Path.error());
		}

		return LoadMaterialAsset(*Path);
	},
	    .Preview = [&](const FAssetId Id, const FMaterialAsset& Material)
	{
		Assets->RequestMaterialPreview(Id, Material);
	},
	};
	MaterialPanel.Draw(*ToolUI, Context);
}

void FEditorFramework::FImplementation::RefreshSelectionFromLevel()
{
	PreviewSelection.Indices.clear();
	PreviewSelection.Active = -1;
	PreviewSelection.Anchor = -1;
	// Level rebuilds preserve stable-ID order; avoid a full object scan per selected entity.
	for (const FObjectId Id : Level->GetSelection())
	{
		const auto Object = std::ranges::lower_bound(PreviewObjects, Id, {}, &FPreviewObject::Id);
		if (Object == PreviewObjects.end() || Object->Id != Id)
		{
			continue;
		}

		const int Index = static_cast<int>(Object - PreviewObjects.begin());
		PreviewSelection.Indices.push_back(Index);
		if (Level->GetActiveObject() == Id)
		{
			PreviewSelection.Active = Index;
		}
	}

	if (PreviewSelection.Active < 0 && !PreviewSelection.Indices.empty())
	{
		PreviewSelection.Active = PreviewSelection.Indices.back();
	}

	PreviewSelection.Anchor = PreviewSelection.Active;
	if (!PreviewSelection.Indices.empty())
	{
		OutlinerPanelState.SelectedFolder = {};
	}
}

void FEditorFramework::FImplementation::RefreshLevel(const bool bPreserveGizmoDrag)
{
	if (LevelGeneration == Level->GetGeneration())
	{
		return;
	}

	LevelGeneration = Level->GetGeneration();
	VisualEntities = Level->GetWorld().SnapshotEntities();
	RefreshSelectionFromLevel();
	OutlinerPanelState.bRenaming = false;
	OutlinerPanelState.bRenameRequested = false;
	if (!bPreserveGizmoDrag)
	{
		ViewportBoxSelection.Cancel();
		PreviewDragStart.reset();
		bDuplicateOnDrag = false;
		ViewportGizmos.resetId();
		RotationFeedback = {};
		ScaleGizmoState = {};
		ScaleFeedback = {};
	}
	PreviewModels.resize(PreviewObjects.size());
	PreviewMeshes.assign(PreviewObjects.size(), nullptr);
	PreviewMaterials.resize(PreviewObjects.size());
	PreviewMaterialSpans.resize(PreviewObjects.size());

	if (!Assets && Tasks != nullptr && GraphicsDevice != nullptr && !AssetPaths.ContentRoot.empty() && !AssetPaths.DerivedDataRoot.empty() && !AssetPaths.WorkerPath.empty() && !AssetPaths.TargetPlatform.empty())
	{
		Assets = FPreviewAssets::Create(*Tasks, *GraphicsDevice, *Log, AssetPaths, PreviewObjects.size(), RenderAssetThumbnail);
		Assets->SetMaterialThumbnailRenderer(RenderMaterialThumbnail);
	}

	std::vector<FAssetId> LevelAssets;
	LevelAssets.reserve(PreviewObjects.size());
	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
		LevelAssets.push_back(Object.Mesh);
	}

	if (Assets)
	{
		Assets->RebindObjects(LevelAssets);
		RefreshPreviewMeshes();
	}

	ViewportRenderView.Models = PreviewModels;
	ViewportRenderView.Meshes = PreviewMeshes;
	ViewportRenderView.Materials = PreviewMaterialSpans;
	RefreshVisuals();
}

void FEditorFramework::FImplementation::ReportLevelResult(const std::expected<void, FLevelError> Result)
{
	if (!Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Level operation failed: {}", Result.error().Message);
	}
}

void FEditorFramework::FImplementation::ApplyAuthoringAction(const EAuthoringAction Action)
{
	if (Action == EAuthoringAction::None || bProjectBusy || Simulation.IsRunning() || Level->HasActiveEdit())
	{
		return;
	}

	if (Action == EAuthoringAction::SelectAll)
	{
		std::vector<int> Indices(PreviewObjects.size());
		std::iota(Indices.begin(), Indices.end(), 0);
		FPreviewSelection Selection;
		Selection.SelectAll(Indices);
		SetPreviewSelection(std::move(Selection));
		return;
	}

	if (Action == EAuthoringAction::Undo)
	{
		if (Level->CanUndo())
		{
			ReportLevelResult(Level->Undo());
		}
	}
	else if (Action == EAuthoringAction::Redo)
	{
		if (Level->CanRedo())
		{
			ReportLevelResult(Level->Redo());
		}
	}
	else if (Action == EAuthoringAction::Create || Action == EAuthoringAction::CreateEmpty)
	{
		const FVector3 Position = ViewportCamera.GetPivot();
		const FWorldPosition WorldPosition{Position.X, Position.Y, Position.Z};
		const auto Result = Action == EAuthoringAction::CreateEmpty ? Level->CreateEmptyEntity(WorldPosition) : Level->CreateEntity(WorldPosition);
		if (!Result)
		{
			ReportLevelResult(std::unexpected(Result.error()));
		}
	}
	else if (Action == EAuthoringAction::Copy)
	{
		const auto Text = Level->CopySelected();
		if (Text)
		{
			ImGui::SetClipboardText(Text->c_str());
		}
		else
		{
			ReportLevelResult(std::unexpected(Text.error()));
		}
	}
	else if (Action == EAuthoringAction::Paste)
	{
		if (const char* const Text = ImGui::GetClipboardText(); Text != nullptr)
		{
			ReportLevelResult(Level->PasteEntities(Text));
		}
	}
	else if (Action == EAuthoringAction::Duplicate)
	{
		ReportLevelResult(Level->DuplicateSelected(false, {TranslationSnap, 0.f, TranslationSnap}));
	}
	else if (Action == EAuthoringAction::Delete)
	{
		ReportLevelResult(Level->DeleteSelected());
	}

	RefreshLevel();
	if (Action == EAuthoringAction::Undo || Action == EAuthoringAction::Redo)
	{
		RefreshSelectionFromLevel();
	}
}

void FEditorFramework::FImplementation::OpenLevelWithDialog()
{
	const FFileDialogFilter Filter{.Name = "Herta level", .Extensions = {"hlevel", "hscene"}};
	const std::filesystem::path Directory = Level->GetPath().has_parent_path() ? Level->GetPath().parent_path() : AssetPaths.ContentRoot.parent_path();
	const auto Chosen = OpenFilesDialog("Open level", std::span(&Filter, 1), Directory);
	if (!Chosen)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not open level dialog: {}", Chosen.error().Message);
		return;
	}

	if (!Chosen->empty())
	{
		if (auto Loaded = Level->Load(Chosen->front()); !Loaded)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not load level: {}", Loaded.error().Message);
		}
	}
}

void FEditorFramework::FImplementation::SaveCurrentLevel()
{
	if (auto Result = Level->Save(); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not save level: {}", Result.error().Message);
		return;
	}

	HERTA_LOG_INFO(*Log, EditorLog, "Level saved");
}

FMatrix4 FEditorFramework::FImplementation::GetPreviewBoundsMatrix(const std::size_t Index) const
{
	const FPreviewBodyShape Shape = GetPreviewBodyShape(Index);
	return FMatrix4::Translation(Shape.Center) * FMatrix4::Scale(Shape.HalfExtents);
}

FPreviewBodyShape FEditorFramework::FImplementation::GetPreviewBodyShape(const std::size_t Index) const
{
	if (!PreviewObjects[Index].Mesh.IsValid() && Index < VisualEntities.size() && VisualEntities[Index].Trigger)
	{
		return {.Center = {}, .HalfExtents = VisualEntities[Index].Trigger->Size * 0.5f};
	}

	if (!PreviewObjects[Index].Mesh.IsValid() && (Index >= PreviewMeshes.size() || PreviewMeshes[Index] == nullptr))
	{
		return {.Center = {}, .HalfExtents = {0.15f, 0.15f, 0.15f}};
	}

	const FRenderMesh* const Mesh = PreviewMeshes[Index];
	if (Mesh == nullptr)
	{
		// Use cube bounds until the selected mesh finishes loading.
		return {.Center = {}, .HalfExtents = {0.5f, 0.5f, 0.5f}};
	}

	const FVector3& Minimum = Mesh->GetBoundsMinimum();
	const FVector3& Maximum = Mesh->GetBoundsMaximum();
	// Flat meshes still need a pickable, collidable volume.
	constexpr float MinimumHalfExtent = 0.001f;
	return {.Center = (Minimum + Maximum) * 0.5f, .HalfExtents = {std::max((Maximum.X - Minimum.X) * 0.5f, MinimumHalfExtent), std::max((Maximum.Y - Minimum.Y) * 0.5f, MinimumHalfExtent), std::max((Maximum.Z - Minimum.Z) * 0.5f, MinimumHalfExtent)}};
}

void FEditorFramework::FImplementation::DrawViewportStats(const ImVec2 Minimum, const ImVec2 Size, const float ToolbarBottom)
{
	if (!Stats->bUnitVisible && !Stats->bFpsVisible)
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float Width = 190.f * Scale;
	const float RowHeight = ImGui::GetTextLineHeight() + 4.f * Scale;
	const float Height = RowHeight * ((Stats->bUnitVisible ? 3 : 0) + (Stats->bFpsVisible ? 1 : 0)) + 16.f * Scale;
	const float Top = ToolbarBottom + 8.f * Scale;

	if (Size.x < Width + 16.f * Scale || Top + Height > Minimum.y + Size.y - 60.f * Scale)
	{
		return;
	}

	const float Left = Minimum.x + Size.x - Width - 8.f * Scale;
	ToolUI->DrawGlassSurface(Left, Top, Width, Height, 8.f * Scale);
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	float Y = Top + 8.f * Scale;
	const auto Row = [&](const char* Label, const std::string& Value)
	{
		Draw->AddText({Left + 12.f * Scale, Y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Label);
		Draw->AddText({Left + Width - 12.f * Scale - ImGui::CalcTextSize(Value.c_str()).x, Y}, IM_COL32(156, 211, 174, 255), Value.c_str());
		Y += RowHeight;
	};

	const float Fps = ImGui::GetIO().Framerate;

	if (Stats->bFpsVisible)
	{
		Row("FPS", std::format("{:.1f}", Fps));
	}

	if (Stats->bUnitVisible)
	{
		Row("Frame", Fps > 0.f ? std::format("{:.2f} ms", 1000.f / Fps) : "--");
		Row("CPU frame", std::format("{:.2f} ms", CpuFrameMilliseconds));
		Row("GPU UI", GpuUIMilliseconds ? std::format("{:.2f} ms", *GpuUIMilliseconds) : "--");
	}
}

float FEditorFramework::FImplementation::DrawViewportToolbar(const ImVec2 Minimum, const ImVec2 Size)
{
	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float Gap = 4.f * Scale;
	const float Padding = 3.f * Scale;
	const float ButtonSize = ViewportIconButtonSize * Scale;
	const float Height = ButtonSize + 2.f * Padding;
	const float EdgeMargin = 8.f * Scale;
	const float Top = Minimum.y + EdgeMargin;
	float ToolbarBottom = Top + Height;
	bViewportControlsHovered = false;

	const auto IconButton = [&](const char* Id, const EViewportIcon Icon, const char* Tooltip, const bool bSelected = false)
	{
		return ViewportIconButton(Id, Icon, Tooltip, Scale, bSelected);
	};

	const auto Island = [&](const float X, const float Width, const float OffsetY = 0.f)
	{
		ToolbarBottom = std::max(ToolbarBottom, Top + OffsetY + Height);
		ToolUI->DrawGlassSurface(X, Top + OffsetY, Width, Height, Height * 0.5f);
		bViewportControlsHovered |= ImGui::IsMouseHoveringRect({X, Top + OffsetY}, {X + Width, Top + OffsetY + Height});
		ImGui::SetCursorScreenPos({X + Padding, Top + OffsetY + Padding});
	};

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.f * Scale, 3.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1.f * Scale, 3.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ButtonSize * 0.5f);
	ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
	ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
	const float SettingsX = std::max(Minimum.x + 4.f * Scale, Minimum.x + Size.x - EdgeMargin - Height);
	float RightControlsLeft = SettingsX;
	Island(SettingsX, Height);

	if (IconButton("Viewport settings", EViewportIcon::Settings, "Viewport settings"))
	{
		ImGui::OpenPopup("ViewportSettings");
	}

	if (Size.x > 340.f * Scale)
	{
		float Right = SettingsX - Gap;
		const bool bExpandedLayout = Size.x > 1040.f * Scale;
		if (bExpandedLayout)
		{
			Island(Right - Height, Height);

			if (IconButton("Focus", EViewportIcon::Focus, "Focus selected object (F)"))
			{
				FocusPreview();
			}

			Right -= Height + Gap;
		}

		const float SnapWidth = (bSnapEnabled ? ViewportIconButtonSize + 75.f : Height / Scale) * Scale;
		Island(Right - SnapWidth, SnapWidth);

		if (IconButton("Grid snap", EViewportIcon::Grid, "Toggle grid snapping (S)", bSnapEnabled))
		{
			bSnapEnabled = !bSnapEnabled;
		}

		if (bSnapEnabled)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(68.f * Scale);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.f * Scale, (ButtonSize - ImGui::GetFontSize()) * 0.5f});
			DrawNumericDragFloat("##GridStep", &TranslationSnap, 0.05f, 0.001f, 100.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
			ImGui::PopStyleVar();

			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Translation snap step in meters");
			}
		}

		TranslationSnap = std::isfinite(TranslationSnap) ? std::clamp(TranslationSnap, 0.001f, 100.f) : 0.5f;
		Right -= SnapWidth + Gap;

		if (bExpandedLayout)
		{
			const bool bHover = ImGui::IsMouseHoveringRect({Right - WorldIslandWidth * Scale, Top}, {Right, Top + Height});
			WorldIslandWidth = bHover ? 100.f : Height / Scale;
			Island(Right - WorldIslandWidth * Scale, WorldIslandWidth * Scale);

			if (IconButton("Coordinate space", EViewportIcon::World, bLocalGizmo ? "Local axes (L)" : "World axes (L)", bLocalGizmo))
			{
				bLocalGizmo = !bLocalGizmo;
				ViewportGizmos.resetId();
			}

			if (WorldIslandWidth > 90.f)
			{
				ImGui::SameLine();

				if (ImGui::Button(bLocalGizmo ? "Local##CoordinateSpaceLabel" : "World##CoordinateSpaceLabel", {WorldIslandWidth * Scale - ButtonSize - ImGui::GetStyle().ItemSpacing.x - 2.f * Padding, ButtonSize}))
				{
					bLocalGizmo = !bLocalGizmo;
					ViewportGizmos.resetId();
				}
			}

			Right -= WorldIslandWidth * Scale + Gap;
		}

		const float ModesWidth = 4.f * ButtonSize + 3.f * ImGui::GetStyle().ItemSpacing.x + 2.f * Padding;
		RightControlsLeft = std::max(Minimum.x + 4.f * Scale, Right - ModesWidth);
		Island(RightControlsLeft, ModesWidth);

		if (IconButton("Hide gizmo", EViewportIcon::Select, "Select / hide gizmo (Q)", !bTransformGizmoVisible))
		{
			bTransformGizmoVisible = false;
			ViewportGizmos.resetId();
		}

		constexpr std::array Modes{std::pair{Im3d::GizmoMode_Translation, EViewportIcon::Move}, std::pair{Im3d::GizmoMode_Rotation, EViewportIcon::Rotate}, std::pair{Im3d::GizmoMode_Scale, EViewportIcon::Scale}};
		constexpr std::array Labels{"Translate (W)", "Rotate (E)", "Scale (R)"};

		for (std::size_t Index = 0; Index < Modes.size(); ++Index)
		{
			ImGui::SameLine();

			if (IconButton(Labels[Index], Modes[Index].second, Labels[Index], bTransformGizmoVisible && GizmoMode == Modes[Index].first))
			{
				bTransformGizmoVisible = true;
				GizmoMode = Modes[Index].first;
				ViewportGizmos.resetId();
			}
		}
	}

	const float PlayWidth = 2.f * ButtonSize + ImGui::GetStyle().ItemSpacing.x + 2.f * Padding;
	const float PlayX = Minimum.x + (Size.x - PlayWidth) * 0.5f;
	const bool bShowPlayControls = Size.x > 180.f * Scale;
	if (bShowPlayControls)
	{
		const bool bFitsTopRow = CanFitViewportToolbarIsland(PlayX, PlayWidth, Minimum.x, RightControlsLeft, Gap);
		Island(PlayX, PlayWidth, bFitsTopRow ? 0.f : Height + Gap);
		ImGui::BeginDisabled();
		IconButton("Play", EViewportIcon::Play, "Play - game runtime is not implemented yet.");
		ImGui::EndDisabled();
		ImGui::SameLine();

		ImGui::BeginDisabled(bProjectBusy);
		if (IconButton("Simulate", Simulation.IsRunning() ? EViewportIcon::Stop : EViewportIcon::Simulate, Simulation.IsRunning() ? "Stop simulation (Esc)" : "Simulate (Alt+S)", Simulation.IsRunning()))
		{
			ToggleSimulation();
		}

		ImGui::EndDisabled();
	}

	// Top-left island; it yields to the playback island and right-hand controls instead of overlapping them.
	if (bTimeOfDayVisible && FindTimeOfDaySun() >= 0)
	{
		const float TimeWidth = ButtonSize + 112.f * Scale + 2.f * Padding;
		const float TimeX = Minimum.x + EdgeMargin;
		const bool bPlayOnTopRow = bShowPlayControls && CanFitViewportToolbarIsland(PlayX, PlayWidth, Minimum.x, RightControlsLeft, Gap);
		if (TimeX + TimeWidth + Gap <= (bPlayOnTopRow ? PlayX : RightControlsLeft))
		{
			Island(TimeX, TimeWidth);
			DrawTimeOfDay(TimeX + Padding, Top + Padding, 112.f * Scale, ButtonSize, Scale);
		}
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.f * Scale, 8.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 4.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7.f * Scale, 3.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * Scale);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.f * Scale);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.16f, 0.16f, 0.16f, 0.82f});
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.22f, 0.22f, 0.22f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.25f, 0.25f, 0.25f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_Button, {0.19f, 0.19f, 0.19f, 0.78f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.26f, 0.26f, 0.26f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.31f, 0.31f, 0.31f, 0.95f});
	ImGui::SetNextWindowPos({SettingsX + Height, Top + Height + 6.f * Scale}, ImGuiCond_Always, {1.f, 0.f});
	ImGui::SetNextWindowSize({288.f * Scale, 0.f}, ImGuiCond_Always);

	if (ImGui::BeginPopup("ViewportSettings"))
	{
		constexpr ImGuiTreeNodeFlags SectionFlags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen;
		ImGui::PushStyleColor(ImGuiCol_Separator, {1.f, 1.f, 1.f, 0.08f});
		ImGui::PushStyleColor(ImGuiCol_Header, {0.f, 0.f, 0.f, 0.f});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 6.f * Scale});
		const float ValueWidth = 116.f * Scale;

		const auto Section = [&](const char* const Label)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			const bool bSectionOpen = ImGui::TreeNodeEx(Label, SectionFlags);
			ImGui::PopStyleColor();
			return bSectionOpen;
		};

		const auto Toggle = [&](const char* const Label, bool& bValue, const char* const Shortcut = nullptr)
		{
			DrawFieldLabel(Label, ValueWidth, Shortcut);
			ImGui::PushID(Label);
			const bool bChanged = ToolUIToggle("##Toggle", &bValue);
			ImGui::PopID();
			return bChanged;
		};

		// Mirrors the toolbar's island breakpoints: gizmo modes and Focus appear here only while the toolbar hides them.
		const bool bToolbarShowsModes = Size.x > 340.f * Scale;
		const bool bToolbarShowsFocus = Size.x > 1040.f * Scale;

		if (Section("Overlays"))
		{
			if (Toggle("Game view", bGameView, "G"))
			{
				ViewportGizmos.resetId();
			}

			Toggle("Grid", bGridVisible);
			Toggle("World axes", bAxesVisible);
			Toggle("Corner axis indicator", bOrientationIndicatorVisible);
			Toggle("Preview bounds", bBoundsVisible);
			Toggle("Camera coordinates", bCameraReadoutVisible);
			Toggle("Camera speed", bCameraSpeedVisible);
			Toggle("Time of day", bTimeOfDayVisible);
		}

		if (Section("Camera"))
		{
			float MovementSpeed = ViewportCamera.GetMovementSpeed();
			DrawFieldLabel("Speed", ValueWidth);

			if (DrawNumericSliderFloat("##CameraSpeed", &MovementSpeed, 0.1f, 100.f, "%.1f m/s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
			{
				ViewportCamera.SetMovementSpeed(MovementSpeed);
			}

			float Sensitivity = ViewportCamera.GetMouseSensitivity() * 180.f / std::numbers::pi_v<float>;
			DrawFieldLabel("Look", ValueWidth);

			if (DrawNumericSliderFloat("##CameraLook", &Sensitivity, 0.02f, 1.f, "%.2f°/px", ImGuiSliderFlags_AlwaysClamp))
			{
				ViewportCamera.SetMouseSensitivity(Sensitivity * std::numbers::pi_v<float> / 180.f);
			}

			DrawFieldLabel("Focal length", ValueWidth);
			DrawNumericSliderFloat("##FocalLength", &PhysicalCamera.FocalLengthMillimeters, 8.f, 300.f, "%.0f mm", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
			Toggle("Auto exposure", bAutoExposure);
			if (bAutoExposure)
			{
				DrawFieldLabel("Compensation", ValueWidth);
				DrawNumericSliderFloat("##ExposureCompensation", &ExposureCompensation, -5.f, 5.f, "%+.1f EV", ImGuiSliderFlags_AlwaysClamp);
			}

			ImGui::BeginDisabled(bAutoExposure);
			DrawFieldLabel("Aperture", ValueWidth);
			DrawNumericSliderFloat("##Aperture", &PhysicalCamera.Aperture, 1.4f, 22.f, "f/%.1f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
			float ShutterDenominator = 1.f / PhysicalCamera.ShutterSeconds;
			DrawFieldLabel("Shutter", ValueWidth);
			if (DrawNumericSliderFloat("##Shutter", &ShutterDenominator, 1.f, 8000.f, "1/%.0f s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
			{
				PhysicalCamera.ShutterSeconds = 1.f / ShutterDenominator;
			}

			DrawFieldLabel("ISO", ValueWidth);
			DrawNumericSliderFloat("##Iso", &PhysicalCamera.Iso, 50.f, 25600.f, "%.0f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
			DrawFieldLabel("Exposure", ValueWidth);
			ImGui::TextDisabled("EV100 %.1f", GetPhysicalCameraExposureEV100(PhysicalCamera));
			ImGui::EndDisabled();
		}

		if (Section("Bookmarks"))
		{
			DrawCameraBookmarks(Scale);
		}

		if (Section("Gizmo"))
		{
			if (!bToolbarShowsModes)
			{
				if (IconButton("Select", EViewportIcon::Select, "Select / hide gizmo (Q)", !bTransformGizmoVisible))
				{
					bTransformGizmoVisible = false;
					ViewportGizmos.resetId();
				}

				constexpr std::array SettingsModes{std::pair{Im3d::GizmoMode_Translation, EViewportIcon::Move}, std::pair{Im3d::GizmoMode_Rotation, EViewportIcon::Rotate}, std::pair{Im3d::GizmoMode_Scale, EViewportIcon::Scale}};
				constexpr std::array SettingsLabels{"Translate (W)", "Rotate (E)", "Scale (R)"};

				for (std::size_t Index = 0; Index < SettingsModes.size(); ++Index)
				{
					ImGui::SameLine(0.f, 2.f * Scale);

					if (IconButton(SettingsLabels[Index], SettingsModes[Index].second, SettingsLabels[Index], bTransformGizmoVisible && GizmoMode == SettingsModes[Index].first))
					{
						bTransformGizmoVisible = true;
						GizmoMode = SettingsModes[Index].first;
						ViewportGizmos.resetId();
					}
				}
			}

			if (!bToolbarShowsFocus)
			{
				// Beside the mode buttons when they are shown, otherwise on its own labeled row.
				if (!bToolbarShowsModes)
				{
					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ButtonSize);
				}
				else
				{
					DrawFieldLabel("Focus selection", ButtonSize, "F");
				}

				if (IconButton("Focus", EViewportIcon::Focus, "Focus selected object (F)"))
				{
					FocusPreview();
				}
			}

			if (Toggle("Local axes", bLocalGizmo, "L"))
			{
				ViewportGizmos.resetId();
			}

			if (Toggle("Flip axes toward camera", bFlipGizmoAxesTowardCamera))
			{
				ViewportGizmos.resetId();
			}
		}

		if (Section("Snapping"))
		{
			Toggle("Snap to grid", bSnapEnabled, "S");
			ImGui::BeginDisabled(!bSnapEnabled);
			DrawFieldLabel("Translation", ValueWidth);
			DrawNumericDragFloat("##SnapTranslation", &TranslationSnap, 0.05f, 0.001f, 100.f, "%.3f m", ImGuiSliderFlags_AlwaysClamp);
			TranslationSnap = std::isfinite(TranslationSnap) ? std::clamp(TranslationSnap, 0.001f, 100.f) : 0.5f;
			DrawFieldLabel("Rotation", ValueWidth);
			DrawNumericDragFloat("##SnapRotation", &RotationSnapDegrees, 1.f, 0.1f, 180.f, "%.1f°", ImGuiSliderFlags_AlwaysClamp);
			DrawFieldLabel("Scale", ValueWidth);
			DrawNumericDragFloat("##SnapScale", &ScaleSnap, 0.01f, 0.001f, 10.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			RotationSnapDegrees = std::isfinite(RotationSnapDegrees) ? std::clamp(RotationSnapDegrees, 0.1f, 180.f) : 15.f;
			ScaleSnap = std::isfinite(ScaleSnap) ? std::clamp(ScaleSnap, 0.001f, 10.f) : 0.1f;
			ImGui::EndDisabled();
		}

		if (Section("Rendering"))
		{
			DrawFieldLabel("Anti-aliasing", ValueWidth);
			constexpr std::array Modes{"Off", "SMAA Low", "SMAA Medium", "SMAA High", "SMAA Ultra"};
			if (ImGui::BeginCombo("##AntiAliasing", Modes[static_cast<std::size_t>(VisualSettings.AntiAliasing)]))
			{
				for (std::size_t Index = 0; Index < Modes.size(); ++Index)
				{
					if (ImGui::Selectable(Modes[Index], Index == static_cast<std::size_t>(VisualSettings.AntiAliasing)))
					{
						VisualSettings.AntiAliasing = static_cast<EAntiAliasing>(Index);
					}
				}

				ImGui::EndCombo();
			}

			if (MeshRenderer)
			{
				DrawFieldLabel("Shadows", ValueWidth);
				constexpr std::array ShadowModes{"Off", "Hard", "Soft PCF"};
				if (ImGui::BeginCombo("##ShadowQuality", ShadowModes[static_cast<std::size_t>(VisualSettings.ShadowQuality)]))
				{
					for (std::size_t Index = 0; Index < ShadowModes.size(); ++Index)
					{
						if (ImGui::Selectable(ShadowModes[Index], Index == static_cast<std::size_t>(VisualSettings.ShadowQuality)))
						{
							VisualSettings.ShadowQuality = static_cast<EShadowQuality>(Index);
						}
					}

					ImGui::EndCombo();
				}
			}
		}

		ImGui::Spacing();
		ImGui::BeginDisabled(Simulation.IsRunning() || Level->HasActiveEdit() || PreviewSelection.Active < 0);

		if (ImGui::Button("Reset preview transform", {-1.f, 0.f}))
		{
			ReportLevelResult(Level->BeginEdit("Reset transform"));
			const FEditorLevel DefaultLevel;
			const auto& Defaults = DefaultLevel.GetObjects();

			for (const int Index : PreviewSelection.Indices)
			{
				FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
				const auto Default = std::ranges::find(Defaults, Object.Id, &FPreviewObject::Id);
				Object.Translation = Default == Defaults.end() ? Im3d::Vec3(0.f) : Default->Translation;
				Object.Rotation = Default == Defaults.end() ? Im3d::Mat3(1.f) : Default->Rotation;
				Object.Scale = Default == Defaults.end() ? Im3d::Vec3(1.f) : Default->Scale;
			}

			ReportLevelResult(Level->EndEdit());
		}

		ImGui::EndDisabled();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(2);
		ImGui::EndPopup();
	}

	ImGui::PopStyleColor(6);
	ImGui::PopStyleVar(5);
	ImGui::EndDisabled();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(4);
	return ToolbarBottom;
}

void FEditorFramework::FImplementation::UpdateViewport(const ImVec2 RenderMinimum, const ImVec2 RenderSize)
{
	const ImGuiIO& IO = ImGui::GetIO();
	const bool bImageHovered = ImGui::IsItemHovered();
	const bool bImageActive = ImGui::IsItemActive();
	const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	const bool bDeselectPressed = ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused);

	if (bEscapePressed && ViewportBoxSelection.bActive)
	{
		CancelViewportBoxSelection();
		ViewportInteraction.Cancel();
	}
	else if (bEscapePressed && PreviewDragStart)
	{
		ReportLevelResult(Level->CancelEdit());
		RefreshLevel();
		PreviewDragStart.reset();
		bDuplicateOnDrag = false;
		bViewportEditCanceled = true;
	}
	else if (bDeselectPressed && !bSimulationStoppedThisFrame && !bPopupOpen && !IO.WantTextInput && ViewportInteraction.DragButton < 0)
	{
		SetPreviewSelection(-1);
	}

	FViewportInteractionInput InteractionInput;
	InteractionInput.bImageHovered = bImageHovered;
	InteractionInput.bImageActive = bImageActive;
	InteractionInput.bWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IO.AppFocusLost;
	InteractionInput.bApplicationFocused = !IO.AppFocusLost;
	InteractionInput.bInputBlocked = bPopupOpen || (bViewportControlsHovered && ViewportInteraction.DragButton < 0) || (ImGui::IsAnyItemActive() && !bImageActive) || bEscapePressed;
	InteractionInput.bAlt = IO.KeyAlt;
	InteractionInput.bRightDragMoved = IO.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] >= IO.MouseDragThreshold * IO.MouseDragThreshold;
	InteractionInput.bCameraNavigationUsed = IO.MouseWheel != 0.f;

	if (ViewportInteraction.DragButton == ImGuiMouseButton_Right || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		for (const ImGuiKey Key : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E})
		{
			InteractionInput.bCameraNavigationUsed |= ImGui::IsKeyDown(Key);
		}
	}

	for (int Button = 0; Button < 3; ++Button)
	{
		InteractionInput.MouseClicked[static_cast<std::size_t>(Button)] = ImGui::IsMouseClicked(Button);
		InteractionInput.MouseDown[static_cast<std::size_t>(Button)] = ImGui::IsMouseDown(Button);
	}

	const ImVec2 Mouse = ImGui::GetMousePos();
	const FVector2 NormalizedMouse{(Mouse.x - RenderMinimum.x) / RenderSize.x, (Mouse.y - RenderMinimum.y) / RenderSize.y};
	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float GizmoPixelScale = GetViewportGizmoPixelScale(InterfaceScale, RenderSize.y, static_cast<float>(ViewportExtent.Height));
	ViewportGizmos.m_gizmoHeightPixels = 100.f * GizmoPixelScale;
	ViewportGizmos.m_gizmoSizePixels = 4.f * GizmoPixelScale;

	if (!bGameView && ViewportInteraction.DragButton < 0 && IO.KeyAlt && InteractionInput.MouseClicked[ImGuiMouseButton_Left] && bImageHovered && bImageActive && InteractionInput.bWindowFocused && !InteractionInput.bInputBlocked && !Simulation.IsRunning() && PreviewSelection.Active >= 0 && bTransformGizmoVisible)
	{
		const FIm3dContextScope ContextScope(ViewportGizmos);
		PrepareViewportGizmos(NormalizedMouse, true, false);
		ViewportGizmos.resetId();
		// Im3d cannot acquire a fresh hover while Select is down. Probe the current ray before routing Alt+LMB.
		Im3d::NewFrame();
		FPreviewObject HoverObject = GetActivePreviewObject();
		DrawPreviewGizmo(HoverObject);
		InteractionInput.bGizmoHovered = Im3d::GetHotId() != Im3d::Id_Invalid;
		Im3d::EndFrame();
	}

	ViewportInteraction.Update(InteractionInput);

	if (ViewportInteraction.DragButton == ImGuiMouseButton_Right)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
	}

	if (!ViewportBoxSelection.bActive && !Simulation.IsRunning() && PreviewSelection.Active >= 0 && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ViewportInteraction.CameraMode == EViewportCameraMode::None)
	{
		if (!PreviewDragStart)
		{
			const auto Result = Level->BeginEdit(IO.KeyAlt ? "Duplicate objects" : "Transform objects");
			ReportLevelResult(Result);
			if (Result)
			{
				const FPreviewObject& Object = GetActivePreviewObject();
				PreviewDragStart.emplace(Object.Translation, Object.Rotation, Object.Scale);
				bDuplicateOnDrag = IO.KeyAlt;
			}
			else
			{
				ViewportInteraction.Cancel();
				ViewportGizmos.resetId();
				bDuplicateOnDrag = false;
			}
		}
	}
	else
	{
		if (PreviewDragStart && !bViewportEditCanceled)
		{
			bViewportEditFinished = true;
		}

		PreviewDragStart.reset();
		bDuplicateOnDrag = false;
	}

	const bool bInputAllowed = InteractionInput.bWindowFocused && !InteractionInput.bInputBlocked;
	const bool bKeyboardAllowed = bInputAllowed && ViewportInteraction.bKeyboardFocus;
	FViewportCameraInput CameraInput;
	CameraInput.Mode = ViewportInteraction.CameraMode;
	CameraInput.MouseDeltaPixels = ImGui::IsItemActivated() ? FVector2{} : FVector2{IO.MouseDelta.x, IO.MouseDelta.y};
	CameraInput.bFast = IO.KeyShift;

	if (bKeyboardAllowed && CameraInput.Mode == EViewportCameraMode::Fly)
	{
		const auto IsMovementDown = [](const ImGuiKey Key)
		{
			return ImGui::SetItemKeyOwner(Key) && ImGui::IsKeyDown(Key);
		};

		CameraInput.Movement = {static_cast<float>(IsMovementDown(ImGuiKey_A)) - static_cast<float>(IsMovementDown(ImGuiKey_D)), static_cast<float>(IsMovementDown(ImGuiKey_E)) - static_cast<float>(IsMovementDown(ImGuiKey_Q)), static_cast<float>(IsMovementDown(ImGuiKey_W)) - static_cast<float>(IsMovementDown(ImGuiKey_S))};
	}

	if (bInputAllowed && !ViewportBoxSelection.bActive && (bImageHovered || ViewportInteraction.DragButton >= 0) && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY))
	{
		if (CameraInput.Mode == EViewportCameraMode::Fly)
		{
			ViewportCamera.SetMovementSpeed(ViewportCamera.GetMovementSpeed() * std::pow(1.2f, IO.MouseWheel));
		}
		else if (ViewportInteraction.DragButton < 0)
		{
			CameraInput.ScrollDelta = IO.MouseWheel;
		}
	}

	if (bKeyboardAllowed && ViewportInteraction.DragButton < 0 && !IO.KeyCtrl && !IO.KeyAlt && !IO.KeySuper)
	{
		if (ImGui::Shortcut(ImGuiKey_S, ImGuiInputFlags_RouteFocused))
		{
			bSnapEnabled = !bSnapEnabled;
		}

		if (ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused))
		{
			RequestPreviewRename();
		}

		if (ImGui::Shortcut(ImGuiKey_F, ImGuiInputFlags_RouteFocused))
		{
			FocusPreview();
		}

		if (ImGui::Shortcut(ImGuiKey_Q, ImGuiInputFlags_RouteFocused))
		{
			bTransformGizmoVisible = false;
			ViewportGizmos.resetId();
		}

		for (const auto& [Key, Mode] : std::array{std::pair{ImGuiKey_W, Im3d::GizmoMode_Translation}, std::pair{ImGuiKey_E, Im3d::GizmoMode_Rotation}, std::pair{ImGuiKey_R, Im3d::GizmoMode_Scale}})
		{
			if (ImGui::Shortcut(Key, ImGuiInputFlags_RouteFocused))
			{
				GizmoMode = Mode;
				bTransformGizmoVisible = true;
				ViewportGizmos.resetId();
			}
		}

		if (ImGui::Shortcut(ImGuiKey_L, ImGuiInputFlags_RouteFocused))
		{
			bLocalGizmo = !bLocalGizmo;
			ViewportGizmos.resetId();
		}
	}

	ViewportCamera.Update(CameraInput, IO.DeltaTime, {RenderSize.x, RenderSize.y});
	const float AspectRatio = static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height);
	const auto Camera = ViewportCamera.GetSnapshot(AspectRatio, ViewportProjectionCenter);
	ViewportRenderView.View = Camera.View;
	ViewportRenderView.Projection = Camera.Projection;
	ViewportRenderView.bDrawGrid = bGridVisible && !bGameView;
	ViewportRenderView.GridCenter = Camera.Position;
	const bool bBoxGesture = ViewportBoxSelection.bActive;
	if (bBoxGesture)
	{
		if (!bInputAllowed || (!bImageActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
		{
			CancelViewportBoxSelection();
			ViewportInteraction.Cancel();
		}
		else
		{
			const ImVec2 ImageMinimum = ImGui::GetItemRectMin();
			const ImVec2 ImageMaximum = ImGui::GetItemRectMax();
			const FVector2 Position{(std::clamp(Mouse.x, ImageMinimum.x, ImageMaximum.x) - RenderMinimum.x) / RenderSize.x, (std::clamp(Mouse.y, ImageMinimum.y, ImageMaximum.y) - RenderMinimum.y) / RenderSize.y};
			ViewportBoxSelection.Update(Position, IO.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= IO.MouseDragThreshold * IO.MouseDragThreshold);
			if (ViewportBoxSelection.bDragging)
			{
				std::vector<int> Hits;
				const FMatrix4 ViewProjection = Camera.Projection * Camera.View;
				for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
				{
					const FPreviewObject& Object = PreviewObjects[Index];
					const FMatrix4 Bounds = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale)) * GetPreviewBoundsMatrix(Index);
					if (IntersectsPreviewCubeSelectionRect(ViewProjection, Bounds, ViewportBoxSelection.Start, ViewportBoxSelection.Current))
					{
						Hits.push_back(static_cast<int>(Index));
					}
				}

				FPreviewSelection Selection = ViewportBoxSelection.MakeSelection(Hits);
				if (Selection != PreviewSelection)
				{
					SetPreviewSelection(std::move(Selection));
				}
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				ViewportBoxSelection.Cancel();
			}
		}
	}

	const bool bGizmoInput = !bProjectBusy && ImGui::GetDragDropPayload() == nullptr && ViewportInteraction.CanUseGizmo(InteractionInput) && !bBoxGesture;
	BuildViewportDebugDraw(bGizmoInput, NormalizedMouse);
}

void FEditorFramework::FImplementation::PrepareViewportGizmos(const FVector2 NormalizedMouse, const bool bGizmoInput, const bool bSelect)
{
	const float AspectRatio = static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height);
	const auto Camera = ViewportCamera.GetSnapshot(AspectRatio, ViewportProjectionCenter);
	const auto CursorRay = ViewportCamera.MakePickingRay(NormalizedMouse, AspectRatio, ViewportProjectionCenter);
	const auto ForwardRay = ViewportCamera.MakePickingRay(ViewportProjectionCenter, AspectRatio, ViewportProjectionCenter);
	Im3d::AppData& AppData = Im3d::GetAppData();
	AppData = Im3d::AppData{};
	AppData.m_deltaTime = ImGui::GetIO().DeltaTime;
	AppData.m_viewportSize = {static_cast<float>(ViewportExtent.Width), static_cast<float>(ViewportExtent.Height)};
	AppData.m_viewOrigin = ToIm3dVector(Camera.Position);
	AppData.m_viewDirection = ToIm3dVector(ForwardRay.Direction);
	AppData.m_cursorRayOrigin = ToIm3dVector(CursorRay.Origin);
	AppData.m_cursorRayDirection = ToIm3dVector(bGizmoInput ? CursorRay.Direction : -ForwardRay.Direction);
	AppData.m_projScaleY = 2.f / Camera.Projection(1, 1);
	AppData.m_flipGizmoWhenBehind = bFlipGizmoAxesTowardCamera;
	AppData.m_keyDown[Im3d::Mouse_Left] = bSelect;
	AppData.m_snapTranslation = bSnapEnabled ? TranslationSnap : 0.f;
	AppData.m_snapRotation = bSnapEnabled ? RotationSnapDegrees * std::numbers::pi_v<float> / 180.f : 0.f;
	AppData.m_snapScale = bSnapEnabled ? ScaleSnap : 0.f;
	ViewportGizmos.m_gizmoMode = static_cast<Im3d::GizmoMode>(GizmoMode);
	ViewportGizmos.m_gizmoLocal = bLocalGizmo;
}

void FEditorFramework::FImplementation::DrawPreviewGizmo(FPreviewObject& Object)
{
	if (GizmoMode == Im3d::GizmoMode_Translation)
	{
		DrawPreviewTranslationGizmo(Object.Translation, Object.Rotation, bLocalGizmo);
	}
	else if (GizmoMode == Im3d::GizmoMode_Rotation)
	{
		DrawPreviewRotationGizmo(Object.Translation, Object.Rotation, bLocalGizmo, RotationFeedback);
	}
	else
	{
		DrawPreviewScaleGizmo(Object.Translation, Object.Rotation, Object.Scale, ScaleGizmoState, ScaleFeedback);
	}
}

void FEditorFramework::FImplementation::BuildViewportDebugDraw(const bool bGizmoInput, const FVector2 NormalizedMouse)
{
	const FIm3dContextScope ContextScope(ViewportGizmos);
	PrepareViewportGizmos(NormalizedMouse, bGizmoInput, bGizmoInput && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ImGui::IsMouseDown(ImGuiMouseButton_Left));
	const float AspectRatio = static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height);
	const auto CursorRay = ViewportCamera.MakePickingRay(NormalizedMouse, AspectRatio, ViewportProjectionCenter);

	if (!bGizmoInput || bGameView)
	{
		ViewportGizmos.resetId();
	}

	Im3d::NewFrame();
	ScaleFeedback = {};

	if (!bGizmoInput || bGameView || PreviewSelection.Active < 0 || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Scale)
	{
		ScaleGizmoState.Reset();
	}

	if (!bGizmoInput || bGameView || PreviewSelection.Active < 0 || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Rotation)
	{
		RotationFeedback.Reset();
	}

	Im3d::PushLayerId("ViewportGizmos");
	FPreviewObject Candidate = GetActivePreviewObject();
	[[maybe_unused]] auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale, PreviewMesh, ObjectId, Parent, Kind] = Candidate;
	const Im3d::Vec3 PreviousTranslation = PreviewTranslation;
	const Im3d::Mat3 PreviousRotation = PreviewRotation;
	const Im3d::Vec3 PreviousScale = PreviewScale;
	const FPreviewObject PreviousObject = GetActivePreviewObject();

	if (!bGameView && !Simulation.IsRunning() && PreviewSelection.Active >= 0 && bTransformGizmoVisible)
	{
		DrawPreviewGizmo(Candidate);
	}

	const Im3d::Mat4 CandidateModel(PreviewTranslation, PreviewRotation, PreviewScale);

	if (!std::ranges::all_of(CandidateModel.m, [](const float Element)
	{
		return std::isfinite(Element);
	}))
	{
		PreviewTranslation = PreviousTranslation;
		PreviewRotation = PreviousRotation;
		PreviewScale = PreviousScale;
		ViewportGizmos.resetId();
	}

	PreviewScale = {std::clamp(PreviewScale.x, 0.001f, 1000.f), std::clamp(PreviewScale.y, 0.001f, 1000.f), std::clamp(PreviewScale.z, 0.001f, 1000.f)};
	const bool bTransformChanged = !std::ranges::equal(Im3d::Mat4(PreviewTranslation, PreviewRotation, PreviewScale).m, Im3d::Mat4(PreviousTranslation, PreviousRotation, PreviousScale).m);

	if (bDuplicateOnDrag && PreviewDragStart && Im3d::GetActiveId() != Im3d::Id_Invalid && bTransformChanged)
	{
		bDuplicateOnDrag = false;
		const auto Result = Level->DuplicateSelected(true);
		ReportLevelResult(Result);
		if (Result)
		{
			RefreshLevel(true);
		}
		else
		{
			Candidate = PreviousObject;
			ViewportGizmos.resetId();
			ViewportInteraction.Cancel();
			bViewportEditCanceled = true;
			PreviewDragStart.reset();
		}
	}

	FPreviewObject& ActiveObject = GetActivePreviewObject();
	ActiveObject.Translation = PreviewTranslation;
	ActiveObject.Rotation = PreviewRotation;
	ActiveObject.Scale = PreviewScale;
	ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
	if (!Simulation.IsRunning())
	{
		const auto Result = Level->CommitEdits();
		ReportLevelResult(Result);
		if (!Result)
		{
			if (Level->HasActiveEdit())
			{
				ReportLevelResult(Level->CancelEdit());
				RefreshLevel();
			}

			Candidate = GetActivePreviewObject();
			ViewportGizmos.resetId();
			ViewportInteraction.Cancel();
			bViewportEditCanceled = true;
			PreviewDragStart.reset();
		}
	}

	Im3d::PopLayerId();
	const Im3d::Mat4 Model(PreviewTranslation, PreviewRotation, PreviewScale);
	const auto ExtractionStart = std::chrono::steady_clock::now();

	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
	}

	FrameMetrics.ExtractionMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ExtractionStart).count();
	RefreshVisuals();

	if (bGizmoInput && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && Im3d::GetActiveId() == Im3d::Id_Invalid)
	{
		int ClosestObject = -1;
		double ClosestDistance = std::numeric_limits<double>::infinity();

		for (std::size_t Index = 0; Index < PreviewModels.size(); ++Index)
		{
			const auto Distance = HitTestPreviewCube(CursorRay, PreviewModels[Index] * GetPreviewBoundsMatrix(Index));
			if (Distance && *Distance < ClosestDistance)
			{
				ClosestObject = static_cast<int>(Index);
				ClosestDistance = *Distance;
			}
		}

		const ImGuiIO& IO = ImGui::GetIO();
		if (ClosestObject < 0 && !IO.KeyAlt && !IO.KeySuper)
		{
			ViewportBoxSelection.Begin(NormalizedMouse, PreviewSelection, IO.KeyShift, IO.KeyCtrl);
		}

		if (ClosestObject >= 0 || (!IO.KeyCtrl && !IO.KeyShift))
		{
			SetPreviewSelection(ClosestObject, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
			OutlinerPanelState.bRevealSelection = ClosestObject >= 0;
		}
	}

	Im3d::PushLayerId("ViewportWorld");

	if (bAxesVisible && !bGameView)
	{
		constexpr float AxisLength = 0.6f;
		Im3d::DrawLine({0.f}, {AxisLength, 0.f, 0.f}, 2.f, Im3d::Color_Red);
		Im3d::DrawLine({0.f}, {0.f, AxisLength, 0.f}, 2.f, Im3d::Color_Green);
		Im3d::DrawLine({0.f}, {0.f, 0.f, AxisLength}, 2.f, Im3d::Color_Blue);
	}

	if (bBoundsVisible && !bGameView && !PreviewObjects.empty())
	{
		const FPreviewBodyShape Bounds = GetPreviewBodyShape(static_cast<std::size_t>(std::max(PreviewSelection.Active, 0)));
		Im3d::PushMatrix(Model);
		Im3d::PushColor(Im3d::Color(0xefd07ccc));
		Im3d::DrawAlignedBox(ToIm3dVector(Bounds.Center - Bounds.HalfExtents), ToIm3dVector(Bounds.Center + Bounds.HalfExtents));
		Im3d::PopColor();
		Im3d::PopMatrix();
	}

	DrawPreviewVisuals(Level->GetWorld(), PreviewObjects, PreviewSelection, bGameView);

	if (!bGameView)
	{
		for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewObjects.size(); ++Index)
		{
			if (!VisualEntities[Index].Trigger)
			{
				continue;
			}

			// Occupied triggers turn green while simulating so enter and exit are visible without the log.
			const FVector3 Half = VisualEntities[Index].Trigger->Size * 0.5f;
			const bool bOccupied = Simulation.IsRunning() && Simulation.IsTriggerOccupied(Index);
			const Im3d::Color Color(bOccupied ? 0x6fd38aff : PreviewSelection.Contains(static_cast<int>(Index)) ? 0xc2b584ff
			                                                                                                    : 0x5fb3d9cc);
			Im3d::PushMatrix(Im3d::Mat4(PreviewObjects[Index].Translation, PreviewObjects[Index].Rotation, PreviewObjects[Index].Scale));
			Im3d::PushColor(Color);
			Im3d::PushSize(2.f);
			Im3d::DrawAlignedBox(ToIm3dVector(-Half), ToIm3dVector(Half));
			Im3d::PopSize();
			Im3d::PopColor();
			Im3d::PopMatrix();
		}
	}

	Im3d::PopLayerId();

	if (!bGameView && !PreviewObjects.empty())
	{
		Im3d::PushLayerId("ViewportSelection");

		for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
		{
			if (PreviewObjects[Index].Mesh.IsValid() || PreviewObjects[Index].Kind != EPreviewObjectKind::Entity)
			{
				continue;
			}

			const bool bSelected = PreviewSelection.Contains(static_cast<int>(Index));
			Im3d::PushMatrix(Im3d::Mat4(PreviewObjects[Index].Translation, PreviewObjects[Index].Rotation, PreviewObjects[Index].Scale));
			const Im3d::Color Color(bSelected ? 0xc2b584ff : 0xa0a0a0ff);
			Im3d::DrawLine({-0.15f, 0.f, 0.f}, {0.15f, 0.f, 0.f}, 2.f, Color);
			Im3d::DrawLine({0.f, -0.15f, 0.f}, {0.f, 0.15f, 0.f}, 2.f, Color);
			Im3d::DrawLine({0.f, 0.f, -0.15f}, {0.f, 0.f, 0.15f}, 2.f, Color);
			Im3d::DrawPoint({0.f}, 5.f, Color);
			Im3d::PopMatrix();
		}

		Im3d::PopLayerId();
	}

	Im3d::EndFrame();
	const std::span<const Im3d::DrawList> DrawLists{Im3d::GetDrawLists(), Im3d::GetDrawListCount()};
	std::size_t VertexCount = 0;

	for (const Im3d::DrawList& List : DrawLists)
	{
		VertexCount += List.m_vertexCount;
	}

	ViewportDebugVertices.resize(VertexCount);
	ViewportDebugDrawLists.clear();
	ViewportDebugDrawLists.reserve(DrawLists.size());
	std::size_t VertexOffset = 0;

	for (const Im3d::DrawList& List : DrawLists)
	{
		if (PreviewSelection.Active < 0 && List.m_layerId == Im3d::MakeId("ViewportGizmos"))
		{
			continue;
		}

		for (std::uint32_t Index = 0; Index < List.m_vertexCount; ++Index)
		{
			const Im3d::VertexData& Vertex = List.m_vertexData[Index];
			ViewportDebugVertices[VertexOffset + Index] = {.Position = {Vertex.m_positionSize.x, Vertex.m_positionSize.y, Vertex.m_positionSize.z}, .Size = Vertex.m_positionSize.w, .Color = ResolveViewportDebugColor(Vertex.m_color)};
		}

		const EDebugPrimitive Primitive = List.m_primType == Im3d::DrawPrimitive_Triangles ? EDebugPrimitive::Triangles : List.m_primType == Im3d::DrawPrimitive_Points ? EDebugPrimitive::Points
		                                                                                                                                                                : EDebugPrimitive::Lines;
		ViewportDebugDrawLists.push_back({.Primitive = Primitive, .Vertices = std::span<const FDebugDrawVertex>{ViewportDebugVertices}.subspan(VertexOffset, List.m_vertexCount), .bDepthTest = List.m_layerId != Im3d::MakeId("ViewportGizmos") && List.m_layerId != Im3d::MakeId("ViewportSelection")});
		VertexOffset += List.m_vertexCount;
	}
}

void FEditorFramework::FImplementation::DrawViewportContextMenu()
{
	const bool bAuthoringAvailable = !bProjectBusy && !Simulation.IsRunning() && !Level->HasActiveEdit();
	if (ViewportInteraction.bContextMenuRequested && bAuthoringAvailable)
	{
		ImGui::OpenPopup("Viewport context menu");
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	ImGui::SetNextWindowSizeConstraints({220.f * Scale, 0.f}, {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()});
	ImGui::SetNextWindowBgAlpha(0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.f * Scale, 8.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 8.f * Scale});
	ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
	if (ImGui::BeginPopup("Viewport context menu", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		const ImVec2 Position = ImGui::GetWindowPos();
		const ImVec2 Size = ImGui::GetWindowSize();
		ToolUI->DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI->GetMetrics().PopupRounding * Scale);
		ImGui::BeginDisabled(!bAuthoringAvailable);
		ImGui::BeginDisabled(PreviewObjects.empty());
		const bool bSelectAll = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A);
		if (ToolUIMenuItem("Select All", EToolUIMenuIcon::SelectAll, nullptr, "Ctrl+A") || bSelectAll)
		{
			ApplyAuthoringAction(EAuthoringAction::SelectAll);
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndDisabled();
		const bool bAdd = ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_A);
		if (ToolUIMenuItem("Add", EToolUIMenuIcon::Add, nullptr, "Shift+A") || bAdd)
		{
			bPlaceObjectsRequested = true;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void FEditorFramework::FImplementation::DrawViewport(const std::function<void()>& RenderViewport)
{
	ImGui::SetNextWindowSize({960, 540}, ImGuiCond_FirstUseEver);

	if (ToolUI->BeginPanel("Viewport", nullptr, true))
	{
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		const ImVec2 ImageMinimum = ImGui::GetCursorScreenPos();
		const std::optional<FToolUICanvasBounds> WorkspaceCanvas = ToolUI->GetWorkspaceCanvasForCurrentPanel();
		const ImVec2 RenderMinimum = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->X, WorkspaceCanvas->Y} : ImageMinimum;
		const ImVec2 RenderSize = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->Width, WorkspaceCanvas->Height} : Size;
		const float Scale = ImGui::GetWindowViewport()->DpiScale;
		const float RenderScale = std::min(Scale, 4096.f / std::max({RenderSize.x, RenderSize.y, 1.f}));
		ViewportExtent = {.Width = static_cast<std::uint32_t>(std::max(RenderSize.x * RenderScale, 1.f)), .Height = static_cast<std::uint32_t>(std::max(RenderSize.y * RenderScale, 1.f))};

		if (Size.x > 0 && Size.y > 0 && RenderSize.x > 0 && RenderSize.y > 0)
		{
			ViewportProjectionCenter = GetViewportProjectionCenter({RenderMinimum.x, RenderMinimum.y}, {RenderSize.x, RenderSize.y}, {ImageMinimum.x, ImageMinimum.y}, {Size.x, Size.y});
			ViewportVisibleSize = {std::clamp(Size.x / RenderSize.x, 0.001f, 1.f), std::clamp(Size.y / RenderSize.y, 0.001f, 1.f)};
			ImDrawList* const PanelDrawList = ImGui::GetWindowDrawList();
			ImDrawListSplitter Layers;
			Layers.Split(PanelDrawList, 2);
			Layers.SetCurrentChannel(PanelDrawList, 1);
			const float ToolbarBottom = DrawViewportToolbar(ImageMinimum, Size);
			if (!bGameView)
			{
				DrawViewportStats(ImageMinimum, Size, ToolbarBottom);
			}
			const float HudScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;

			const auto FormatCameraHud = [this](const FVector3& Position)
			{
				constexpr float Degrees = 180.f / std::numbers::pi_v<float>;
				return std::format("X {:.2f}   Y {:.2f}   Z {:.2f} m   Pitch {:.1f}°   Yaw {:.1f}°", Position.X, Position.Y, Position.Z, ViewportCamera.GetPitch() * Degrees, ViewportCamera.GetYaw() * Degrees);
			};

			const FVector3 LayoutCameraPosition = ViewportCamera.GetSnapshot(1.f).Position;
			const std::string LayoutCoordinates = FormatCameraHud(LayoutCameraPosition);
			const float CameraLabelWidth = std::max(ImGui::CalcTextSize("Camera").x, ImGui::CalcTextSize("Copied").x);
			const float CoordinatesWidth = CameraLabelWidth + ImGui::CalcTextSize(LayoutCoordinates.c_str()).x + 52.f * HudScale;
			const float CoordinatesHeight = 32.f * HudScale;
			const ImVec2 CoordinatesPosition{ImageMinimum.x + std::max(0.f, Size.x - CoordinatesWidth - 14.f * HudScale), ImageMinimum.y + std::max(0.f, Size.y - CoordinatesHeight - 14.f * HudScale)};
			bool bCopyCoordinates = false;
			bool bCoordinatesHovered = false;
			bool bCoordinatesFocused = false;
			if (bCameraReadoutVisible)
			{
				ImGui::SetCursorScreenPos(CoordinatesPosition);
				ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
				bCopyCoordinates = ImGui::InvisibleButton("Copy camera coordinates##CameraCoordinates", {CoordinatesWidth, CoordinatesHeight}, ImGuiButtonFlags_EnableNav);
				bCoordinatesHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
				bCoordinatesFocused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
				bViewportControlsHovered |= bCoordinatesHovered;

				if (bCoordinatesHovered)
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				}

				ImGui::EndDisabled();
			}

			ImGui::SetCursorScreenPos(ImageMinimum);
			ImGui::BeginDisabled(bViewportControlsHovered && ViewportInteraction.DragButton < 0);
			ImGui::InvisibleButton("##ViewportInteraction", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			ImGui::EndDisabled();
			std::optional<FAssetId> DroppedAsset;
			if (Assets && !bProjectBusy && !Simulation.IsRunning() && !Level->HasActiveEdit() && ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload(ContentAssetPayload); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FAssetId))
				{
					FAssetId Asset;
					std::memcpy(&Asset, Payload->Data, sizeof(Asset));
					DroppedAsset = Asset;
				}

				ImGui::EndDragDropTarget();
			}

			UpdateViewport(RenderMinimum, RenderSize);
			if (DroppedAsset)
			{
				const auto Options = Assets->GetOptions();
				const auto Asset = std::ranges::find_if(Options, [&DroppedAsset](const auto& Option)
				{
					return Option.Id == *DroppedAsset && (IsPlaceableContentAsset(Option) || Option.Importer == "Material");
				});
				if (Asset != Options.end())
				{
					const ImVec2 Mouse = ImGui::GetIO().MousePos;
					const FVector2 Normalized{(Mouse.x - RenderMinimum.x) / RenderSize.x, (Mouse.y - RenderMinimum.y) / RenderSize.y};
					const auto Ray = ViewportCamera.MakePickingRay(Normalized, RenderSize.x / RenderSize.y, ViewportProjectionCenter);
					if (Asset->Importer == "Material")
					{
						int Hit = -1;
						double Closest = std::numeric_limits<double>::infinity();

						for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
						{
							if (!PreviewObjects[Index].Mesh.IsValid())
							{
								continue;
							}

							const auto Distance = HitTestPreviewCube(Ray, PreviewModels[Index] * GetPreviewBoundsMatrix(Index));
							if (Distance && *Distance < Closest)
							{
								Hit = static_cast<int>(Index);
								Closest = *Distance;
							}
						}

						if (Hit >= 0)
						{
							SetPreviewSelection(Hit, false);
							ReportLevelResult(Level->SetSelectedMaterial(0, Asset->Id));
							RefreshLevel();
						}
					}
					else
					{
						const FVector3 Pivot = ViewportCamera.GetPivot();
						const float PlaneDistance = std::abs(Ray.Direction.Y) > 0.001f ? (Pivot.Y - Ray.Origin.Y) / Ray.Direction.Y : -1.f;
						const float Distance = PlaneDistance > 0.f && PlaneDistance < 10000.f ? PlaneDistance : (Pivot - Ray.Origin).Length();
						const FVector3 Position = Ray.Origin + Ray.Direction * Distance;
						const auto Separator = Asset->Label.rfind('/');
						const auto Label = Asset->Label.substr(Separator == std::string::npos ? 0 : Separator + 1);
						const auto Dot = Label.rfind('.');
						const auto Created = Level->CreateMeshEntity(Asset->Id, Label.substr(0, Dot), {Position.X, Position.Y, Position.Z});
						if (!Created)
						{
							ReportLevelResult(std::unexpected(Created.error()));
						}
						RefreshLevel();
					}
				}
			}

			DrawViewportContextMenu();
			const FVector3 CameraPosition = ViewportCamera.GetSnapshot(1.f).Position;
			const std::string Coordinates = FormatCameraHud(CameraPosition);

			if (bCopyCoordinates)
			{
				ImGui::SetClipboardText(FormatTransformVectorClipboard(CameraPosition).c_str());
				CameraCoordinatesCopiedUntil = ImGui::GetTime() + 1.5;
			}

			ViewportSelectedModels.clear();
			if (!bGameView)
			{
				for (const int Index : PreviewSelection.Indices)
				{
					ViewportSelectedModels.push_back(static_cast<std::size_t>(Index));
				}
			}

			ViewportRenderView.Selected = ViewportSelectedModels;

			// Render after layout and input, before recording the texture ID that resize may replace.
			RenderViewport();
			Layers.SetCurrentChannel(PanelDrawList, 0);

			if (ViewportTexture != 0 && WorkspaceCanvas && !ToolUI->IsViewportImmersive())
			{
				ImDrawList* const Background = ImGui::GetBackgroundDrawList(ImGui::GetWindowViewport());
				Background->PushClipRect(RenderMinimum, {RenderMinimum.x + RenderSize.x, RenderMinimum.y + RenderSize.y}, true);
				Background->AddImage(ImTextureRef(static_cast<ImTextureID>(ViewportTexture)), RenderMinimum, {RenderMinimum.x + RenderSize.x, RenderMinimum.y + RenderSize.y});
				Background->PopClipRect();
			}
			else if (ViewportTexture != 0)
			{
				PanelDrawList->AddImage(ImTextureRef(static_cast<ImTextureID>(ViewportTexture)), RenderMinimum, {RenderMinimum.x + RenderSize.x, RenderMinimum.y + RenderSize.y});
			}

			if (!bGameView && ViewportBoxSelection.bActive && ViewportBoxSelection.bDragging)
			{
				const FVector2 First = ViewportBoxSelection.Start;
				const FVector2 Second = ViewportBoxSelection.Current;
				const ImVec2 Minimum{RenderMinimum.x + std::min(First.X, Second.X) * RenderSize.x, RenderMinimum.y + std::min(First.Y, Second.Y) * RenderSize.y};
				const ImVec2 Maximum{RenderMinimum.x + std::max(First.X, Second.X) * RenderSize.x, RenderMinimum.y + std::max(First.Y, Second.Y) * RenderSize.y};
				const FToolUIColor Outline = ToolUI->GetAppearance().Accent;
				FToolUIColor Fill = Outline;
				Fill.Alpha = 28;
				PanelDrawList->PushClipRect(ImageMinimum, {ImageMinimum.x + Size.x, ImageMinimum.y + Size.y}, true);
				PanelDrawList->AddRectFilled(Minimum, Maximum, PackColor(Fill));
				PanelDrawList->AddRect(Minimum, Maximum, PackColor(Outline), 0.f, 0, HudScale);
				PanelDrawList->PopClipRect();
			}

			Layers.Merge(PanelDrawList);

			if (PreviewDragStart && ViewportGizmos.m_activeId != Im3d::Id_Invalid)
			{
				const Im3d::Vec3 PreviewTranslation = GetActivePreviewObject().Translation;
				const FVector3 Start = ToHertaMatrix(*PreviewDragStart).TransformPosition(FVector3::Zero());
				FVector3 End{PreviewTranslation.x, PreviewTranslation.y, PreviewTranslation.z};

				if (GizmoMode == Im3d::GizmoMode_Translation)
				{
					ImGui::SetTooltip("%.2f m", static_cast<double>((End - Start).Length()));
				}
				else if (ScaleFeedback.bActive)
				{
					const bool bUseY = ScaleFeedback.Handle == EViewportScaleHandle::Y || ScaleFeedback.Handle == EViewportScaleHandle::YZ;
					const float Factor = ScaleFeedback.Handle == EViewportScaleHandle::Z ? ScaleFeedback.Factors.z : bUseY ? ScaleFeedback.Factors.y
					                                                                                                       : ScaleFeedback.Factors.x;
					ImGui::SetTooltip("%.2fx", static_cast<double>(Factor));
					End = {ScaleFeedback.HandlePosition.x, ScaleFeedback.HandlePosition.y, ScaleFeedback.HandlePosition.z};
				}
				else if (RotationFeedback.AngleDegrees)
				{
					ImGui::SetTooltip("%+.1f\xC2\xB0", static_cast<double>(*RotationFeedback.AngleDegrees));
				}

				const FMatrix4 WorldToClip = ViewportRenderView.Projection * ViewportRenderView.View;
				const FVector4 ClipStart = WorldToClip * FVector4{Start, 1.f};
				const FVector4 ClipEnd = WorldToClip * FVector4{End, 1.f};

				if (ClipStart.W > 0.001f && ClipEnd.W > 0.001f)
				{
					const FVector2 ScreenStart{(ClipStart.X / ClipStart.W + 1.f) * RenderSize.x * 0.5f, (1.f - ClipStart.Y / ClipStart.W) * RenderSize.y * 0.5f};
					const FVector2 ScreenEnd = RotationFeedback.AngleDegrees ? FVector2{ImGui::GetMousePos().x - RenderMinimum.x, ImGui::GetMousePos().y - RenderMinimum.y} : FVector2{(ClipEnd.X / ClipEnd.W + 1.f) * RenderSize.x * 0.5f, (1.f - ClipEnd.Y / ClipEnd.W) * RenderSize.y * 0.5f};
					const FVector2 Delta = ScreenEnd - ScreenStart;
					const FVector2 PanelMinimum{ImageMinimum.x - RenderMinimum.x, ImageMinimum.y - RenderMinimum.y};
					const FVector2 PanelMaximum{PanelMinimum.X + Size.x, PanelMinimum.Y + Size.y};
					float First = 0.f;
					float Last = 1.f;

					for (std::size_t Axis = 0; Axis < 2; ++Axis)
					{
						if (std::abs(Delta[Axis]) > 0.001f)
						{
							const float A = (PanelMinimum[Axis] - ScreenStart[Axis]) / Delta[Axis];
							const float B = (PanelMaximum[Axis] - ScreenStart[Axis]) / Delta[Axis];
							First = std::max(First, std::min(A, B));
							Last = std::min(Last, std::max(A, B));
						}
						else if (ScreenStart[Axis] < PanelMinimum[Axis] || ScreenStart[Axis] > PanelMaximum[Axis])
						{
							Last = 0.f;
						}
					}

					const float Distance = Delta.Length();
					const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
					ImDrawList* const Trail = ImGui::GetWindowDrawList();
					Trail->PushClipRect(ImageMinimum, {ImageMinimum.x + Size.x, ImageMinimum.y + Size.y}, true);
					constexpr ImU32 TrailColor = IM_COL32(194, 181, 132, 255);

					if (Distance > 0.001f && std::isfinite(Distance))
					{
						const int DashCount = static_cast<int>(std::clamp(std::ceil((Last - First) * Distance / (7.f * UiScale)), 0.f, 4096.f));

						for (int Dash = 0; Dash < DashCount; ++Dash)
						{
							const float Offset = First * Distance + static_cast<float>(Dash) * 7.f * UiScale;
							const FVector2 A = ScreenStart + Delta * (Offset / Distance);
							const FVector2 B = ScreenStart + Delta * (std::min(Offset + 3.f * UiScale, Last * Distance) / Distance);
							Trail->AddLine({RenderMinimum.x + A.X, RenderMinimum.y + A.Y}, {RenderMinimum.x + B.X, RenderMinimum.y + B.Y}, TrailColor, 1.5f * UiScale);
						}

						Trail->AddCircleFilled({RenderMinimum.x + ScreenStart.X, RenderMinimum.y + ScreenStart.Y}, 3.f * UiScale, TrailColor);
					}

					Trail->PopClipRect();
				}
			}

			if (bCameraReadoutVisible)
			{
				ToolUI->DrawGlassSurface(CoordinatesPosition.x, CoordinatesPosition.y, CoordinatesWidth, CoordinatesHeight, CoordinatesHeight * 0.5f);
				const float CoordinatesTextY = CoordinatesPosition.y + (CoordinatesHeight - ImGui::GetFontSize()) * 0.5f;
				ImDrawList* const HudDraw = ImGui::GetWindowDrawList();

				if (bCoordinatesHovered)
				{
					HudDraw->AddRectFilled(CoordinatesPosition, {CoordinatesPosition.x + CoordinatesWidth, CoordinatesPosition.y + CoordinatesHeight}, IM_COL32(255, 255, 255, 12), CoordinatesHeight * 0.5f);
				}

				if (bCoordinatesFocused)
				{
					HudDraw->AddRect(CoordinatesPosition, {CoordinatesPosition.x + CoordinatesWidth, CoordinatesPosition.y + CoordinatesHeight}, ImGui::GetColorU32(ImGuiCol_NavCursor), CoordinatesHeight * 0.5f);
				}

				const ImVec2 CameraIconCenter{CoordinatesPosition.x + 18.f * HudScale, CoordinatesPosition.y + CoordinatesHeight * 0.5f};
				const ImU32 CameraIconColor = PackColor(ToolUITheme::TextSecondary);
				HudDraw->AddRect({CameraIconCenter.x - 6.f * HudScale, CameraIconCenter.y - 4.f * HudScale}, {CameraIconCenter.x + 6.f * HudScale, CameraIconCenter.y + 4.f * HudScale}, CameraIconColor, 2.f * HudScale, 0, HudScale);
				HudDraw->AddRectFilled({CameraIconCenter.x - 3.f * HudScale, CameraIconCenter.y - 6.f * HudScale}, {CameraIconCenter.x + HudScale, CameraIconCenter.y - 4.f * HudScale}, CameraIconColor, HudScale);
				HudDraw->AddCircle(CameraIconCenter, 2.f * HudScale, CameraIconColor, 12, HudScale);
				HudDraw->AddText({CoordinatesPosition.x + 32.f * HudScale, CoordinatesTextY}, CameraIconColor, ImGui::GetTime() < CameraCoordinatesCopiedUntil ? "Copied" : "Camera");
				HudDraw->AddText({CoordinatesPosition.x + 40.f * HudScale + CameraLabelWidth, CoordinatesTextY}, PackColor(ToolUITheme::TextPrimary), Coordinates.c_str());
			}

			if (!bGameView && bCameraSpeedVisible)
			{
				const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
				const std::string Speed = std::format("{:.2f} m/s", ViewportCamera.GetMovementSpeed() * (ImGui::GetIO().KeyShift ? 4.f : 1.f));
				const float LabelWidth = ImGui::CalcTextSize("Speed").x;
				const float Width = LabelWidth + ImGui::CalcTextSize(Speed.c_str()).x + 52.f * UiScale;
				const float Height = 32.f * UiScale;
				const float Bottom = bCameraReadoutVisible ? CoordinatesPosition.y - 6.f * UiScale : CoordinatesPosition.y + CoordinatesHeight;
				const ImVec2 Position{ImageMinimum.x + std::max(0.f, Size.x - Width - 14.f * UiScale), Bottom - Height};
				ToolUI->DrawGlassSurface(Position.x, Position.y, Width, Height, Height * 0.5f);
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				const ImVec2 Center{Position.x + 18.f * UiScale, Position.y + Height * 0.5f};
				const ImU32 Muted = PackColor(ToolUITheme::TextSecondary);
				Draw->PathArcTo(Center, 6.f * UiScale, std::numbers::pi_v<float> * 0.75f, std::numbers::pi_v<float> * 2.25f, 16);
				Draw->PathStroke(Muted, 0, UiScale);
				Draw->AddLine(Center, {Center.x + 3.f * UiScale, Center.y - 3.f * UiScale}, Muted, UiScale);
				const float TextY = Center.y - ImGui::GetFontSize() * 0.5f;
				Draw->AddText({Position.x + 32.f * UiScale, TextY}, Muted, "Speed");
				Draw->AddText({Position.x + 40.f * UiScale + LabelWidth, TextY}, PackColor(ToolUITheme::TextPrimary), Speed.c_str());
			}

			if (!bGameView && bOrientationIndicatorVisible)
			{
				DrawViewportAxes(ViewportRenderView.View, ImageMinimum, Size, ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize);
			}
		}
		else
		{
			CancelViewportBoxSelection();
			ViewportInteraction.Cancel();
			bViewportEditCanceled |= PreviewDragStart.has_value();
			PreviewDragStart.reset();
			ViewportGizmos.resetId();
			ViewportDebugDrawLists.clear();
		}
	}
	else
	{
		ViewportExtent = {};
		CancelViewportBoxSelection();
		ViewportInteraction.Cancel();
		bViewportEditCanceled |= PreviewDragStart.has_value();
		PreviewDragStart.reset();
		ViewportGizmos.resetId();
		ViewportDebugDrawLists.clear();
	}

	ToolUI->EndPanel();
}

void FEditorFramework::FImplementation::CancelViewportBoxSelection()
{
	if (ViewportBoxSelection.bActive)
	{
		FPreviewSelection Selection = ViewportBoxSelection.InitialSelection;
		ViewportBoxSelection.Cancel();
		SetPreviewSelection(std::move(Selection));
	}
}

void FEditorFramework::FImplementation::SetPreviewSelection(const int ObjectIndex, const bool bToggle)
{
	FPreviewSelection Selection = PreviewSelection;
	Selection.Select(ObjectIndex, bToggle);
	SetPreviewSelection(std::move(Selection));
}

void FEditorFramework::FImplementation::SetPreviewSelection(FPreviewSelection Selection)
{
	if (!Selection.Indices.empty())
	{
		OutlinerPanelState.SelectedFolder = {};
	}

	if (PreviewSelection == Selection)
	{
		return;
	}

	if (OutlinerPanelState.bRenaming)
	{
		if (OutlinerPanelState.RenameFolder.IsValid())
		{
			ReportLevelResult(Level->RenameFolder(OutlinerPanelState.RenameFolder, OutlinerPanelState.RenameBuffer.data()));
		}
		else
		{
			const auto Object = std::ranges::find(PreviewObjects, OutlinerPanelState.RenameObject, &FPreviewObject::Id);
			if (Object != PreviewObjects.end())
			{
				RenamePreviewObject(Object->Label, OutlinerPanelState.RenameBuffer.data());
				ReportLevelResult(Level->CommitEdits("Rename object"));
			}
		}
	}

	if (Level->HasActiveEdit())
	{
		ReportLevelResult(Level->EndEdit());
	}

	PreviewSelection = std::move(Selection);
	std::vector<FObjectId> SelectedIds;
	for (const int Index : PreviewSelection.Indices)
	{
		SelectedIds.push_back(PreviewObjects[static_cast<std::size_t>(Index)].Id);
	}

	Level->SetSelection(SelectedIds, PreviewSelection.Active >= 0 ? std::optional(PreviewObjects[static_cast<std::size_t>(PreviewSelection.Active)].Id) : std::nullopt);
	OutlinerPanelState.bRenaming = false;
	OutlinerPanelState.bRenameRequested = false;
	PreviewDragStart.reset();
	RotationFeedback = {};
	ScaleGizmoState = {};
	ScaleFeedback = {};
	ViewportGizmos.resetId();
}

void FEditorFramework::FImplementation::RequestPreviewRename()
{
	if (PreviewSelection.Active < 0 || Simulation.IsRunning() || ViewportInteraction.DragButton >= 0)
	{
		return;
	}

	bOutlinerOpen = true;
	OutlinerPanelState.bRenameRequested = true;
	ImGui::SetWindowFocus("Outliner");
}

void FEditorFramework::FImplementation::JumpToCameraBookmark(const std::uint32_t Slot)
{
	const auto Bookmarks = Level->GetCameraBookmarks();
	const auto Found = std::ranges::find(Bookmarks, Slot, &FLevelCameraBookmark::Slot);
	if (Found == Bookmarks.end())
	{
		HERTA_LOG_INFO(*Log, EditorLog, "Camera bookmark {} is empty; press Ctrl+{} in the viewport to save the current view", Slot, Slot);
		return;
	}

	const FVector3d& Position = Found->Position.Meters;
	ViewportCamera.SetView({static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z)}, Found->Yaw, Found->Pitch);
}

void FEditorFramework::FImplementation::SaveCameraBookmark(const std::uint32_t Slot)
{
	const auto Bookmarks = Level->GetCameraBookmarks();
	const auto Found = std::ranges::find(Bookmarks, Slot, &FLevelCameraBookmark::Slot);
	const FVector3& Position = ViewportCamera.GetPosition();
	FLevelCameraBookmark Bookmark{
	    .Slot = Slot,
	    .Name = Found != Bookmarks.end() ? Found->Name : std::format("Bookmark {}", Slot),
	    .Position = FWorldPosition{Position.X, Position.Y, Position.Z},
	    .Yaw = ViewportCamera.GetYaw(),
	    .Pitch = ViewportCamera.GetPitch(),
	};

	const std::string Name = Bookmark.Name;
	if (const auto Result = Level->SetCameraBookmark(std::move(Bookmark)); !Result)
	{
		ReportLevelResult(Result);
		return;
	}

	HERTA_LOG_INFO(*Log, EditorLog, "Saved camera bookmark {} ({})", Slot, Name);
}

void FEditorFramework::FImplementation::DrawCameraBookmarks(const float Scale)
{
	const auto Bookmarks = Level->GetCameraBookmarks();
	std::optional<std::uint32_t> Jump;
	std::optional<std::uint32_t> Update;
	std::optional<std::uint32_t> Remove;
	std::optional<FLevelCameraBookmark> Renamed;
	for (const FLevelCameraBookmark& Bookmark : Bookmarks)
	{
		ImGui::PushID(static_cast<int>(Bookmark.Slot));
		const std::string Key = std::to_string(Bookmark.Slot);
		if (ImGui::Selectable(Bookmark.Name.c_str()))
		{
			Jump = Bookmark.Slot;
		}

		ImGui::SetItemTooltip("Press %s in the viewport to recall. Right-click to rename, update, or delete.", Key.c_str());
		const ImVec2 RowMaximum = ImGui::GetItemRectMax();
		const ImVec2 KeySize = ImGui::CalcTextSize(Key.c_str());
		ImGui::GetWindowDrawList()->AddText({RowMaximum.x - KeySize.x - 4.f * Scale, RowMaximum.y - KeySize.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Key.c_str());
		if (ImGui::BeginPopupContextItem("BookmarkActions"))
		{
			if (ImGui::IsWindowAppearing())
			{
				CameraBookmarkName.fill('\0');
				Bookmark.Name.copy(CameraBookmarkName.data(), std::min(Bookmark.Name.size(), CameraBookmarkName.size() - 1));
				ImGui::SetKeyboardFocusHere();
			}

			ImGui::SetNextItemWidth(180.f * Scale);
			if (ImGui::InputText("##Name", CameraBookmarkName.data(), CameraBookmarkName.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) && CameraBookmarkName[0] != '\0')
			{
				Renamed = Bookmark;
				Renamed->Name = CameraBookmarkName.data();
				ImGui::CloseCurrentPopup();
			}

			if (ImGui::MenuItem("Update to current view", std::format("Ctrl+{}", Key).c_str()))
			{
				Update = Bookmark.Slot;
			}

			if (ImGui::MenuItem("Delete"))
			{
				Remove = Bookmark.Slot;
			}

			ImGui::EndPopup();
		}

		ImGui::PopID();
	}

	ImGui::TextDisabled(Bookmarks.empty() ? "Ctrl+1-9 saves the current view" : "1-9 recalls, Ctrl+1-9 saves");

	// Bookmarks are applied after the loop, which reads from the level's own storage.
	if (Jump)
	{
		JumpToCameraBookmark(*Jump);
	}

	if (Update)
	{
		SaveCameraBookmark(*Update);
	}

	if (Remove)
	{
		ReportLevelResult(Level->RemoveCameraBookmark(*Remove));
	}

	if (Renamed)
	{
		ReportLevelResult(Level->SetCameraBookmark(std::move(*Renamed)));
	}
}

void FEditorFramework::FImplementation::ToggleSimulation()
{
	if (bProjectBusy)
	{
		return;
	}

	if (Simulation.IsRunning())
	{
		Simulation.Stop();
		PreviewObjects = std::move(SimulationStart);
		Level->SetSimulationRunning(false);
		bSimulationStoppedThisFrame = true;
	}
	else
	{
		if (Level->HasActiveEdit())
		{
			if (auto Result = Level->EndEdit(); !Result)
			{
				ReportLevelResult(std::move(Result));
				return;
			}
		}

		if (auto Result = Level->CommitEdits(); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not apply level edits: {}", Result.error().Message);
			return;
		}

		std::vector<FPreviewSimulationBody> Bodies;
		for (const ELevelBodyType Type : {ELevelBodyType::Static, ELevelBodyType::Dynamic})
		{
			for (const std::size_t Index : Level->FindBodies(Type))
			{
				const auto Entity = Level->GetWorld().GetEntity(*Level->GetWorld().FindEntity(PreviewObjects[Index].Id));
				// A soft body owns its own deformable collision; a Rigid Body on the same entity would fight it.
				if (Entity->SoftBody)
				{
					continue;
				}

				const FLevelRigidBodySettings& Settings = Entity->BodySettings;
				Bodies.push_back({
				    .ObjectIndex = Index,
				    .Transform = ToHertaTransform(PreviewObjects[Index]),
				    .Shape = GetPreviewBodyShape(Index),
				    .Collision = Settings.Collision == ELevelCollisionShape::Sphere ? EPhysicsShape::Sphere : Settings.Collision == ELevelCollisionShape::Capsule ? EPhysicsShape::Capsule
				                                                                                                                                                  : EPhysicsShape::Box,
				    .MotionType = Type == ELevelBodyType::Dynamic ? EPhysicsMotionType::Dynamic : EPhysicsMotionType::Static,
				    .Properties = {
				        .MassKg = Settings.MassKg,
				        .Friction = Settings.Friction,
				        .Restitution = Settings.Restitution,
				        .LinearDamping = Settings.LinearDamping,
				        .AngularDamping = Settings.AngularDamping,
				        .GravityScale = Settings.GravityScale,
				    },
				    .Mover = Entity->Mover ? std::optional{FPreviewMover{.Offset = Entity->Mover->Offset, .PeriodSeconds = Entity->Mover->PeriodSeconds}} : std::nullopt,
				});
			}
		}

		// Movers without a Rigid Body still collide, as kinematic boxes; triggers become sensors unless the entity already simulates.
		for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewObjects.size(); ++Index)
		{
			const FLevelEntity& Entity = VisualEntities[Index];
			const bool bSimulated = std::ranges::contains(Bodies, Index, &FPreviewSimulationBody::ObjectIndex) || Entity.SoftBody;
			if (Entity.Mover && Entity.Mesh && !bSimulated)
			{
				Bodies.push_back({
				    .ObjectIndex = Index,
				    .Transform = ToHertaTransform(PreviewObjects[Index]),
				    .Shape = GetPreviewBodyShape(Index),
				    .Mover = FPreviewMover{.Offset = Entity.Mover->Offset, .PeriodSeconds = Entity.Mover->PeriodSeconds},
				});
			}
			else if (Entity.Trigger && bSimulated)
			{
				HERTA_LOG_WARNING(*Log, EditorLog, "Trigger on {} is skipped: the entity already simulates as a body", Entity.Name);
			}
			else if (Entity.Trigger)
			{
				const FVector3& Size = Entity.Trigger->Size;
				Bodies.push_back({.ObjectIndex = Index, .Transform = ToHertaTransform(PreviewObjects[Index]), .Shape = {.HalfExtents = Size * 0.5f}, .bTrigger = true});
			}
		}

		// Topology and world vertices must stay in place until Start copies them into the physics world.
		std::vector<std::size_t> SoftBodyIndices;
		for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewModels.size(); ++Index)
		{
			if (VisualEntities[Index].SoftBody && !VisualEntities[Index].Mesh)
			{
				SoftBodyIndices.push_back(Index);
			}
		}

		std::vector<FSoftBodyTopology> Topologies;
		std::vector<std::vector<FVector3>> WorldVertices;
		std::vector<FPreviewSimulationSoftBody> SoftBodies;
		Topologies.reserve(SoftBodyIndices.size());
		WorldVertices.reserve(SoftBodyIndices.size());
		for (const std::size_t Index : SoftBodyIndices)
		{
			const FSoftBodyComponent& SoftBody = *VisualEntities[Index].SoftBody;
			const FSoftBodyTopology& Topology = Topologies.emplace_back(BuildSoftBodyTopology(SoftBody));
			std::vector<FVector3>& Vertices = WorldVertices.emplace_back();
			Vertices.reserve(Topology.Vertices.size());
			for (const FVector3& Vertex : Topology.Vertices)
			{
				const FVector4 World = PreviewModels[Index] * FVector4{Vertex.X, Vertex.Y, Vertex.Z, 1.f};
				Vertices.push_back({World.X, World.Y, World.Z});
			}

			FPreviewSimulationSoftBody& Simulated = SoftBodies.emplace_back(FPreviewSimulationSoftBody{.ObjectIndex = Index, .Settings = MakeSoftBodyPhysicsSettings(SoftBody, Topology, Vertices)});
			if (Simulated.Settings.Attachment)
			{
				const auto Attached = std::ranges::find_if(Bodies, [&](const FPreviewSimulationBody& Body)
				{
					return PreviewObjects[Body.ObjectIndex].Id == SoftBody.Attachment && Body.MotionType == EPhysicsMotionType::Dynamic;
				});
				if (Attached != Bodies.end())
				{
					Simulated.AttachedObjectIndex = Attached->ObjectIndex;
				}
				else
				{
					HERTA_LOG_WARNING(*Log, EditorLog, "{} hangs free: its attached body is not a meshed Dynamic rigid body", VisualEntities[Index].Name);
					Simulated.Settings.Attachment.reset();
				}
			}
		}

		if (Bodies.empty() && SoftBodies.empty())
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Simulation preview requires a Static Mesh with a Rigid Body component or a Soft Body");
			return;
		}

		std::vector<FPreviewObject> OriginalObjects = PreviewObjects;
		if (const auto Result = Simulation.Start(Bodies, SoftBodies); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not start simulation: {}", Result.error().Message);
			return;
		}

		SimulationStart = std::move(OriginalObjects);
		Level->SetSimulationRunning(true);
		UpdateSimulation(0.f);
	}

	PreviewDragStart.reset();
	ViewportGizmos.resetId();
}

void FEditorFramework::FImplementation::UpdateSimulation(const float DeltaSeconds)
{
	if (!Simulation.IsRunning())
	{
		return;
	}

	if (const auto Result = Simulation.Update(DeltaSeconds); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Simulation stopped: {}", Result.error().Message);
		ToggleSimulation();
		return;
	}

	for (const FPreviewTriggerEvent& Event : Simulation.TakeTriggerEvents())
	{
		HERTA_LOG_INFO(*Log, EditorLog, "{} {} {}", PreviewObjects[Event.ObjectIndex].Label, Event.bEntered ? "entered" : "left", PreviewObjects[Event.TriggerObjectIndex].Label);
	}

	std::vector<FObjectId> Overrides;
	Overrides.reserve(Simulation.GetTransforms().size());
	for (const FPreviewSimulationTransform& Snapshot : Simulation.GetTransforms())
	{
		const FTransform& Transform = Snapshot.Transform;
		FPreviewObject& Object = PreviewObjects[Snapshot.ObjectIndex];
		Overrides.push_back(Object.Id);
		Object.Translation = ToIm3dVector(Transform.Translation);
		Object.Scale = ToIm3dVector(Transform.Scale3D);
		const FMatrix3 Rotation = FMatrix3::Rotation(Transform.Rotation);

		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			for (std::size_t Row = 0; Row < 3; ++Row)
			{
				Object.Rotation(static_cast<int>(Row), static_cast<int>(Column)) = Rotation(Row, Column);
			}
		}
	}

	if (auto Result = Level->UpdatePreviewHierarchy(Overrides); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Simulation stopped: {}", Result.error().Message);
		ToggleSimulation();
	}
}

void FEditorFramework::FImplementation::DrawDetailsPanel()
{
	FPreviewObject PreviousObject = GetActivePreviewObject();
	[[maybe_unused]] auto& [Label, Translation, Rotation, Scale, Mesh, ObjectId, Parent, Kind] = GetActivePreviewObject();
	FDetailsMeshField MeshField;
	std::string MeshStatus;
	const bool bHasSelection = !PreviewSelection.Indices.empty();
	FDetailsComponentField Components{.bAllMesh = bHasSelection, .bAllBody = bHasSelection, .bAllLight = bHasSelection, .bAllSkyAtmosphere = bHasSelection, .bAllHeightFog = bHasSelection, .bAllSoftBody = bHasSelection, .bAllMover = bHasSelection, .bAllTrigger = bHasSelection, .SunIds = SunIds, .SunLabels = SunLabels, .AttachmentIds = AttachmentIds, .AttachmentLabels = AttachmentLabels, .EnvironmentIds = EnvironmentIds, .EnvironmentLabels = EnvironmentLabels};
	std::vector<FDetailsMaterialSlot> MaterialSlots;
	std::vector<FAssetId> MaterialIds;
	std::vector<std::string> MaterialLabels;
	std::vector<std::uint64_t> MaterialThumbnails;
	std::vector<FLevelEntity> SelectedMeshes;
	std::array<std::optional<FLevelEntity>, 6> VisualBaselines;
	bool bFirstBody = true;
	bool bFirstBodySettings = true;
	constexpr std::array BodyProperties{&FLevelRigidBodySettings::MassKg, &FLevelRigidBodySettings::Friction, &FLevelRigidBodySettings::Restitution, &FLevelRigidBodySettings::LinearDamping, &FLevelRigidBodySettings::AngularDamping, &FLevelRigidBodySettings::GravityScale};

	for (const int Index : PreviewSelection.Indices)
	{
		const FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
		const auto Handle = Level->GetWorld().FindEntity(Object.Id);
		const auto Entity = Handle ? Level->GetWorld().GetEntity(*Handle) : std::nullopt;
		const ELevelBodyType Type = Entity ? Entity->BodyType : ELevelBodyType::None;
		Components.bAnyMesh |= Object.Mesh.IsValid();
		Components.bAllMesh &= Object.Mesh.IsValid();
		Components.bMixedMeshAsset |= Object.Mesh != Mesh;
		Components.bAnyBody |= Type != ELevelBodyType::None;
		Components.bAllBody &= Type != ELevelBodyType::None;
		Components.bAnyDynamicBody |= Type == ELevelBodyType::Dynamic;
		if (Entity)
		{
			if (Entity->Mesh)
			{
				SelectedMeshes.push_back(*Entity);
				const FRenderMesh* const RenderMesh = Assets ? Assets->GetSlot(static_cast<std::size_t>(Index)).Mesh.get() : nullptr;
				const std::size_t Count = std::min<std::size_t>(256, std::max({std::size_t{1}, Entity->Mesh->Materials.size(), RenderMesh ? RenderMesh->GetMaterials().size() : 0}));
				MaterialSlots.resize(std::max(MaterialSlots.size(), Count));

				if (RenderMesh)
				{
					const auto Imported = RenderMesh->GetMaterials();
					for (std::size_t Slot = 0; Slot < std::min(Imported.size(), MaterialSlots.size()); ++Slot)
					{
						if (MaterialSlots[Slot].Name.empty())
						{
							MaterialSlots[Slot].Name = Imported[Slot].Name;
						}
					}
				}
			}

			const auto Aggregate = [&](const std::size_t Baseline, const ELevelComponentType Component, const bool bPresent, bool& bAny, bool& bAll, const std::span<bool> Mixed)
			{
				bAny |= bPresent;
				bAll &= bPresent;
				if (!bPresent)
				{
					return;
				}

				if (!VisualBaselines[Baseline])
				{
					VisualBaselines[Baseline] = Entity;
					return;
				}

				const auto Properties = GetLevelComponentDescriptor(Component).Properties;
				for (std::size_t Property = 0; Property < std::min(Properties.size(), Mixed.size()); ++Property)
				{
					Mixed[Property] |= GetLevelVisualProperty(*Entity, Component, Properties[Property].Key) != GetLevelVisualProperty(*VisualBaselines[Baseline], Component, Properties[Property].Key);
				}
			};

			Aggregate(0, ELevelComponentType::Light, Entity->Light.has_value(), Components.bAnyLight, Components.bAllLight, Components.MixedLight);
			Aggregate(1, ELevelComponentType::SkyAtmosphere, Entity->SkyAtmosphere.has_value(), Components.bAnySkyAtmosphere, Components.bAllSkyAtmosphere, Components.MixedSkyAtmosphere);
			Aggregate(2, ELevelComponentType::HeightFog, Entity->HeightFog.has_value(), Components.bAnyHeightFog, Components.bAllHeightFog, Components.MixedHeightFog);
			Aggregate(3, ELevelComponentType::SoftBody, Entity->SoftBody.has_value(), Components.bAnySoftBody, Components.bAllSoftBody, Components.MixedSoftBody);
			Aggregate(4, ELevelComponentType::Mover, Entity->Mover.has_value(), Components.bAnyMover, Components.bAllMover, Components.MixedMover);
			Aggregate(5, ELevelComponentType::Trigger, Entity->Trigger.has_value(), Components.bAnyTrigger, Components.bAllTrigger, Components.MixedTrigger);
		}

		if (Type != ELevelBodyType::None)
		{
			if (bFirstBodySettings)
			{
				Components.BodySettings = Entity->BodySettings;
				bFirstBodySettings = false;
			}
			else
			{
				for (std::size_t PropertyIndex = 0; PropertyIndex < BodyProperties.size(); ++PropertyIndex)
				{
					const auto Property = BodyProperties[PropertyIndex];
					Components.MixedBodySettings[PropertyIndex] |= Entity->BodySettings.*Property != Components.BodySettings.*Property;
				}

				Components.bMixedCollision |= Entity->BodySettings.Collision != Components.BodySettings.Collision;
			}
		}

		if (bFirstBody)
		{
			Components.BodyType = Type;
			bFirstBody = false;
		}
		else if (Components.BodyType != Type)
		{
			Components.BodyType.reset();
		}
	}

	for (std::size_t Slot = 0; Slot < MaterialSlots.size(); ++Slot)
	{
		bool bFirst = true;

		for (const FLevelEntity& Entity : SelectedMeshes)
		{
			const auto& Overrides = Entity.Mesh->Materials;
			const FAssetId Material = Slot < Overrides.size() ? Overrides[Slot] : FAssetId{};
			if (bFirst)
			{
				MaterialSlots[Slot].Selected = Material;
				bFirst = false;
			}
			else
			{
				MaterialSlots[Slot].bMixed |= Material != MaterialSlots[Slot].Selected;
			}
		}
	}

	if (Assets)
	{
		for (const FPreviewAssetOption& Option : Assets->GetOptions())
		{
			if (Option.Importer == "Material")
			{
				MaterialIds.push_back(Option.Id);
				MaterialLabels.push_back(Option.Label);
				const auto Thumbnail = Assets->GetThumbnail(Option.Id);
				MaterialThumbnails.push_back(Thumbnail ? *Thumbnail : 0);
			}
		}
	}

	Components.MaterialSlots = MaterialSlots;
	Components.MaterialOptionIds = MaterialIds;
	Components.MaterialOptionLabels = MaterialLabels;
	Components.MaterialThumbnails = MaterialThumbnails;

	if (VisualBaselines[0])
	{
		Components.Light = *VisualBaselines[0]->Light;
	}

	if (VisualBaselines[1])
	{
		Components.SkyAtmosphere = *VisualBaselines[1]->SkyAtmosphere;
	}

	if (VisualBaselines[2])
	{
		Components.HeightFog = *VisualBaselines[2]->HeightFog;
	}

	if (VisualBaselines[3])
	{
		Components.SoftBody = *VisualBaselines[3]->SoftBody;
	}

	if (VisualBaselines[4])
	{
		Components.Mover = *VisualBaselines[4]->Mover;
	}

	if (VisualBaselines[5])
	{
		Components.Trigger = *VisualBaselines[5]->Trigger;
	}

	if (Assets && !PreviewObjects.empty() && Mesh.IsValid())
	{
		// ponytail: rebuilt every frame from the scan results; cache them per scan when content grows large.
		MeshOptions.clear();
		MeshOptionIds.clear();
		const FPreviewMeshSlot& Slot = Assets->GetSlot(static_cast<std::size_t>(std::max(PreviewSelection.Active, 0)));

		for (const FPreviewAssetOption& Option : Assets->GetOptions())
		{
			if (Option.Importer != "Gltf" && Option.Importer != "Blender" && Option.Importer != "Texture")
			{
				continue;
			}

			if (Option.Id == Slot.Asset)
			{
				MeshField.Selected = static_cast<int>(MeshOptions.size());
			}

			MeshOptions.push_back(Option.Importer == "Texture" ? std::format("{} (texture on cube)", Option.Label) : Option.Label);
			MeshOptionIds.push_back(Option.Id);
		}

		if (MeshField.Selected < 0)
		{
			// The current asset is not in the latest scan yet, such as during startup.
			MeshField.Selected = static_cast<int>(MeshOptions.size());
			MeshOptions.push_back(Slot.Label);
			MeshOptionIds.push_back(Slot.Asset);
		}

		if (Slot.bLoading)
		{
			MeshStatus = std::format("Cooking {}...", Slot.Label);
		}
		else if (!Slot.Error.empty())
		{
			MeshStatus = Slot.Error;
			MeshField.bError = true;
		}
		else if (Assets->IsScanning())
		{
			MeshStatus = "Scanning content...";
		}

		MeshField.Options = MeshOptions;
		MeshField.Status = MeshStatus;
	}

	const FDetailsEditCallbacks Edits{
	    .Begin = [&]
	{
		if (!Level->HasActiveEdit())
		{
			ReportLevelResult(Level->BeginEdit("Edit properties"));
		}
	},
	    .Flush = [&](const bool bCanceled)
	{
		if (bCanceled && Level->HasActiveEdit())
		{
			ReportLevelResult(Level->CancelEdit());
		}
		else if (Level->HasActiveEdit())
		{
			ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
			ReportLevelResult(Level->EndEdit());
		}

		PreviousObject = GetActivePreviewObject();
	},
	    .ApplyBodyProperty = [&](float FLevelRigidBodySettings::*const Property, const float Value)
	{
		ReportLevelResult(Level->SetSelectedBodyProperty(Property, Value));
	},
	    .ReadBodyProperty = [&](float FLevelRigidBodySettings::*const Property)
	{
		for (const FObjectId Selected : Level->GetSelection())
		{
			const auto Entity = Level->GetWorld().GetEntity(*Level->GetWorld().FindEntity(Selected));
			if (Entity->BodyType != ELevelBodyType::None)
			{
				return Entity->BodySettings.*Property;
			}
		}

		return FLevelRigidBodySettings{}.*Property;
	},
	    .ApplyVisualProperty = [&](const ELevelComponentType Type, const std::string_view Key, const FLevelPropertyValue& Value)
	{
		ReportLevelResult(Level->SetSelectedVisualProperty(Type, Key, Value));
		RefreshSelectedVisualEntities();
	},
	    .ReadVisualProperty = [&](const ELevelComponentType Type, const std::string_view Key) -> std::optional<FLevelPropertyValue>
	{
		for (const FObjectId Selected : Level->GetSelection())
		{
			const auto Handle = Level->GetWorld().FindEntity(Selected);
			const auto Entity = Handle ? Level->GetWorld().GetEntity(*Handle) : std::nullopt;
			if (Entity)
			{
				if (const auto Value = GetLevelVisualProperty(*Entity, Type, Key))
				{
					return Value;
				}
			}
		}

		return std::nullopt;
	},
	};
	const FDetailsMeshResult MeshResult = DrawPreviewDetailsPanel(*ToolUI, bDetailsOpen, PreviewSelection.Active >= 0, bProjectBusy || Simulation.IsRunning() || ViewportInteraction.DragButton >= 0, Translation, Rotation, Scale, DetailsPanelState, Label, PreviewSelection.Indices.size(), Assets && Mesh.IsValid() ? &MeshField : nullptr, &Edits, &Components);
	if (!MeshResult.bEditCanceled)
	{
		ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
	}

	if (Assets && MeshResult.bOptionsOpened)
	{
		Assets->RequestScan();
	}

	if (MeshResult.MaterialOpenRequested.IsValid() && Assets)
	{
		PendingMaterialOpen = MeshResult.MaterialOpenRequested;
		Assets->RequestMaterial(PendingMaterialOpen);
	}

	if (MeshResult.MaterialChosen)
	{
		if (Level->HasActiveEdit())
		{
			ReportLevelResult(Level->EndEdit());
		}

		ReportLevelResult(Level->SetSelectedMaterial(MeshResult.MaterialChosen->first, MeshResult.MaterialChosen->second));
	}

	if (Assets && MeshResult.Chosen >= 0)
	{
		ReportLevelResult(Level->BeginEdit("Change mesh"));
		const FAssetId Chosen = MeshOptionIds[static_cast<std::size_t>(MeshResult.Chosen)];

		for (const int Index : PreviewSelection.Indices)
		{
			FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
			if (!Object.Mesh.IsValid())
			{
				continue;
			}

			Object.Mesh = Chosen;
			Assets->RequestMesh(static_cast<std::size_t>(Index), Chosen);
		}

		RefreshPreviewMeshes();
		ReportLevelResult(Level->EndEdit());
	}

	if (MeshResult.bEditCanceled && Level->HasActiveEdit())
	{
		ReportLevelResult(Level->CancelEdit());
	}
	else if (MeshResult.bEditFinished && Level->HasActiveEdit())
	{
		ReportLevelResult(Level->EndEdit());
	}

	if (MeshResult.ComponentAction != EDetailsComponentAction::None || MeshResult.BodyTypeChosen || MeshResult.CollisionChosen)
	{
		if (Level->HasActiveEdit())
		{
			ReportLevelResult(Level->EndEdit());
		}

		switch (MeshResult.ComponentAction)
		{
			case EDetailsComponentAction::AddStaticMesh:
				ReportLevelResult(Level->AddStaticMeshToSelected(EngineCubeAsset));
				break;
			case EDetailsComponentAction::RemoveStaticMesh:
				ReportLevelResult(Level->RemoveStaticMeshFromSelected());
				break;
			case EDetailsComponentAction::AddRigidBody:
				ReportLevelResult(Level->AddRigidBodyToSelected());
				break;
			case EDetailsComponentAction::RemoveRigidBody:
				ReportLevelResult(Level->SetSelectedBodyType(ELevelBodyType::None));
				break;
			case EDetailsComponentAction::AddDirectionalLight:
				ReportLevelResult(Level->AddLightToSelected(ELightType::Directional));
				break;
			case EDetailsComponentAction::AddSkyLight:
				ReportLevelResult(Level->AddLightToSelected(ELightType::Sky));
				break;
			case EDetailsComponentAction::AddPointLight:
				ReportLevelResult(Level->AddLightToSelected(ELightType::Point));
				break;
			case EDetailsComponentAction::AddSpotLight:
				ReportLevelResult(Level->AddLightToSelected(ELightType::Spot));
				break;
			case EDetailsComponentAction::AddRectLight:
				ReportLevelResult(Level->AddLightToSelected(ELightType::Rect));
				break;
			case EDetailsComponentAction::RemoveLight:
				ReportLevelResult(Level->SetSelectedLight(std::nullopt));
				break;
			case EDetailsComponentAction::AddSkyAtmosphere:
				ReportLevelResult(Level->AddSkyAtmosphereToSelected());
				break;
			case EDetailsComponentAction::RemoveSkyAtmosphere:
				ReportLevelResult(Level->SetSelectedSkyAtmosphere(std::nullopt));
				break;
			case EDetailsComponentAction::AddHeightFog:
				ReportLevelResult(Level->AddHeightFogToSelected());
				break;
			case EDetailsComponentAction::RemoveHeightFog:
				ReportLevelResult(Level->SetSelectedHeightFog(std::nullopt));
				break;
			case EDetailsComponentAction::AddSoftBody:
				ReportLevelResult(Level->AddSoftBodyToSelected());
				break;
			case EDetailsComponentAction::RemoveSoftBody:
				ReportLevelResult(Level->SetSelectedSoftBody(std::nullopt));
				break;
			case EDetailsComponentAction::AddMover:
				ReportLevelResult(Level->AddMoverToSelected());
				break;
			case EDetailsComponentAction::RemoveMover:
				ReportLevelResult(Level->SetSelectedMover(std::nullopt));
				break;
			case EDetailsComponentAction::AddTrigger:
				ReportLevelResult(Level->AddTriggerToSelected());
				break;
			case EDetailsComponentAction::RemoveTrigger:
				ReportLevelResult(Level->SetSelectedTrigger(std::nullopt));
				break;
			case EDetailsComponentAction::None:
				break;
		}

		if (MeshResult.BodyTypeChosen)
		{
			ReportLevelResult(Level->SetSelectedBodyType(*MeshResult.BodyTypeChosen));
		}

		if (MeshResult.CollisionChosen)
		{
			ReportLevelResult(Level->SetSelectedCollisionShape(*MeshResult.CollisionChosen));
		}
	}

	RefreshLevel();
}

void FEditorFramework::FImplementation::DrawOutlinerPanel()
{
	FPreviewSelection NewSelection = PreviewSelection;
	const bool bFocusRequested = DrawPreviewOutlinerPanel(*ToolUI, bOutlinerOpen, NewSelection, PreviewObjects, bProjectBusy || Simulation.IsRunning() || ViewportInteraction.DragButton >= 0, OutlinerPanelState, Level->GetFolders());
	if (OutlinerPanelState.bRenameCommitted)
	{
		if (OutlinerPanelState.RenameFolder.IsValid())
		{
			ReportLevelResult(Level->RenameFolder(OutlinerPanelState.RenameFolder, OutlinerPanelState.RenameBuffer.data()));
		}
		else
		{
			const auto Object = std::ranges::find(PreviewObjects, OutlinerPanelState.RenameObject, &FPreviewObject::Id);
			if (Object != PreviewObjects.end())
			{
				RenamePreviewObject(Object->Label, OutlinerPanelState.RenameBuffer.data());
				ReportLevelResult(Level->CommitEdits("Rename object"));
			}
		}
	}

	SetPreviewSelection(std::move(NewSelection));
	if (auto Request = std::exchange(OutlinerPanelState.FolderRequest, std::nullopt))
	{
		if (Level->HasActiveEdit())
		{
			ReportLevelResult(Level->EndEdit());
		}

		switch (Request->Action)
		{
			case EOutlinerFolderAction::Create:
			{
				const auto Created = Level->CreateFolder(Request->Parent, Request->Objects);
				if (!Created)
				{
					ReportLevelResult(std::unexpected(Created.error()));
					break;
				}

				SetPreviewSelection(FPreviewSelection{});
				OutlinerPanelState.Search.Clear();
				for (FObjectId Parent = Request->Parent.value_or(FObjectId{}); Parent.IsValid();)
				{
					OutlinerPanelState.CollapsedFolders.erase(Parent);
					const auto Folder = std::ranges::find(Level->GetFolders(), Parent, &FLevelFolder::Id);
					Parent = Folder == Level->GetFolders().end() ? FObjectId{} : Folder->Parent;
				}

				OutlinerPanelState.SelectedFolder = *Created;
				OutlinerPanelState.RenameFolder = {};
				OutlinerPanelState.bRenameRequested = true;
				break;
			}
			case EOutlinerFolderAction::Delete:
				ReportLevelResult(Level->DeleteFolder(Request->Folder));
				break;
			case EOutlinerFolderAction::MoveFolder:
				ReportLevelResult(Level->MoveFolder(Request->Folder, Request->Parent));
				break;
			case EOutlinerFolderAction::MoveEntities:
				ReportLevelResult(Level->MoveEntitiesToFolder(Request->Objects, Request->Parent));
				break;
		}
	}

	if (OutlinerPanelState.SelectedFolder.IsValid() && std::ranges::find(Level->GetFolders(), OutlinerPanelState.SelectedFolder, &FLevelFolder::Id) == Level->GetFolders().end())
	{
		OutlinerPanelState.SelectedFolder = {};
		OutlinerPanelState.bRenaming = false;
		OutlinerPanelState.bRenameRequested = false;
	}

	if (auto Request = std::exchange(OutlinerPanelState.ReparentRequest, std::nullopt))
	{
		if (Level->HasActiveEdit())
		{
			ReportLevelResult(Level->EndEdit());
		}

		ReportLevelResult(Level->ReparentEntities(Request->Objects, Request->Parent));
		RefreshLevel();
	}

	if (bFocusRequested)
	{
		FocusPreview();
	}
}

void FEditorFramework::FImplementation::RebuildSuggestions(const std::string_view Prefix)
{
	if (HasConsoleCommandText(Prefix))
	{
		Suggestions = OutputLog->CompleteCommand(Prefix);
	}
	else
	{
		Suggestions.clear();
	}

	if (Suggestions.empty())
	{
		SuggestionIndex = -1;
	}
	else
	{
		SuggestionIndex = std::clamp(SuggestionIndex, 0, static_cast<int>(Suggestions.size()) - 1);
	}
}

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::SubmitCommand()
{
	bReclaimCommandFocus = false;
	ImGui::SetWindowFocus("Viewport");

	if (CommandBuffer[0] == '\0')
	{
		return {};
	}

	std::expected<void, FOutputLogError> SubmitResult = OutputLog->SubmitCommand(CommandBuffer.data());
	if (!SubmitResult)
	{
		return std::unexpected(FEditorFrameworkError{std::move(SubmitResult.error().Message)});
	}

	CommandBuffer.fill('\0');
	Suggestions.clear();
	SuggestionIndex = -1;
	return {};
}

void FEditorFramework::FImplementation::SetBottomPanelOpen(const bool bOpen)
{
	const bool bWasOpen = bBottomPanelOpen;
	if (!bOpen && bWasOpen)
	{
		const ImGuiWindow* const Browser = ImGui::FindWindowByID(ImHashStr("Content Browser"));
		bBottomBrowserSelected = Browser && Browser->DockNode && Browser->DockNode->SelectedTabId == Browser->TabId;
	}

	bBottomPanelOpen = bOpen;

	if (bOpen && !bContentBrowserOpen && !bOutputLogOpen)
	{
		bContentBrowserOpen = true;
	}

	if (bOpen && !bWasOpen)
	{
		bRestoreBottomPanelFocus = true;
		ContentBrowserState.bFocusFolderName |= !ContentBrowserState.RenamingFolder.empty();
	}
}

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::DrawOutputLog()
{
	const bool bReceivedRecords = OutputLog->Synchronize();

	if (!bBottomPanelOpen || !bOutputLogOpen)
	{
		return {};
	}

	if (bFocusCommandRequested || bFocusOutputLogRequested)
	{
		ImGui::SetNextWindowFocus();
		ImGui::SetNextWindowCollapsed(false);
	}

	const bool bPanelVisible = ToolUI->BeginPanel(">_  Output Log###Output Log", &bOutputLogOpen);
	bFocusOutputLogRequested = false;

	if (!bPanelVisible)
	{
		ToolUI->EndPanel();
		return {};
	}

	const float ToolbarScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * ToolbarScale, 5.f * ToolbarScale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * ToolbarScale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * ToolbarScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;

	const auto ButtonWidth = [](const char* const Label)
	{
		return ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.f;
	};

	const float ClearWidth = ButtonWidth("Clear");
	const float CopyWidth = ButtonWidth("Copy");
	const auto VisibleLines = OutputLog->GetVisibleLines();
	std::size_t WarningCount = 0;
	std::optional<std::uint64_t> PreviousSequence;

	for (const FOutputLogLine& Line : VisibleLines)
	{
		if (Line.Record.Sequence != PreviousSequence && Line.Record.Level == ELogLevel::Warning)
		{
			++WarningCount;
		}

		PreviousSequence = Line.Record.Sequence;
	}

	const std::string CountLabel = std::to_string(WarningCount);
	const float CountFontSize = ImGui::GetFontSize() * 0.8f;
	const ImVec2 CountTextSize = ImGui::CalcTextSize(CountLabel.c_str());
	const ImVec2 CountSize{CountTextSize.x * 0.8f, CountTextSize.y * 0.8f};
	const ImVec2 BadgeSize{std::max(18.f * ToolbarScale, CountSize.x + 10.f * ToolbarScale), CountFontSize + 4.f * ToolbarScale};
	const float FilterWidth = ButtonWidth("Warnings") + Spacing + BadgeSize.x;
	const float ButtonsWidth = FilterWidth + ButtonWidth("Options") + ClearWidth + CopyWidth + Spacing * 3.f;
	const float AvailableWidth = ImGui::GetContentRegionAvail().x;
	const bool bSingleRow = AvailableWidth >= ButtonsWidth + Spacing + 160.f * ToolbarScale;
	ImGui::SetNextItemWidth(bSingleRow ? std::min(320.f * ToolbarScale, AvailableWidth - ButtonsWidth - Spacing) : -1.f);
	const bool bSearchChanged = ToolUI->DrawSearchField("##OutputLogSearch", "Search Log", SearchBuffer.data(), SearchBuffer.size());

	if (bSingleRow)
	{
		ImGui::SameLine();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.f, 0.5f});
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

	if (ImGui::Button("Warnings###OutputLogFilters", {FilterWidth, 0.f}))
	{
		ImGui::OpenPopup("OutputLogFilter");
	}

	const ImVec2 FilterMinimum = ImGui::GetItemRectMin();
	const ImVec2 FilterMaximum = ImGui::GetItemRectMax();
	const ImVec2 BadgeMinimum{FilterMaximum.x - ImGui::GetStyle().FramePadding.x - BadgeSize.x, (FilterMinimum.y + FilterMaximum.y - BadgeSize.y) * 0.5f};
	ImDrawList* const ToolbarDrawList = ImGui::GetWindowDrawList();
	ToolbarDrawList->AddRectFilled(BadgeMinimum, {BadgeMinimum.x + BadgeSize.x, BadgeMinimum.y + BadgeSize.y}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.08f}), 4.f * ToolbarScale);
	ToolbarDrawList->AddText(ImGui::GetFont(), CountFontSize, {BadgeMinimum.x + (BadgeSize.x - CountSize.x) * 0.5f, BadgeMinimum.y + (BadgeSize.y - CountSize.y) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), CountLabel.c_str());
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::SameLine();

	if (ImGui::Button("Options"))
	{
		ImGui::OpenPopup("OutputLogOptions");
	}

	const float RightX = ImGui::GetWindowContentRegionMax().x - ClearWidth - CopyWidth - Spacing;

	if (bSingleRow || RightX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + Spacing)
	{
		ImGui::SameLine(RightX);
	}

	const bool bClearRequested = ImGui::Button("Clear");
	ImGui::SameLine();
	const bool bCopyRequested = ImGui::Button("Copy");
	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(4);

	if (bClearRequested)
	{
		OutputLog->Clear();
	}

	if (bSearchChanged)
	{
		OutputLog->SetSearch(SearchBuffer.data());
	}

	if (ImGui::BeginPopup("OutputLogFilter"))
	{
		for (const ELogLevel Verbosity : {ELogLevel::Trace, ELogLevel::Debug, ELogLevel::Info, ELogLevel::Warning, ELogLevel::Error, ELogLevel::Critical})
		{
			bool bVisible = OutputLog->IsLevelVisible(Verbosity);
			const std::string LevelName{GetLogLevelName(Verbosity)};

			if (ImGui::MenuItem(LevelName.c_str(), nullptr, &bVisible))
			{
				OutputLog->SetLevelVisible(Verbosity, bVisible);
			}
		}

		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("OutputLogOptions"))
	{
		bool bAutoScroll = OutputLog->IsAutoScroll();
		bool bPaused = OutputLog->IsPaused();
		bool bColorize = OutputLog->IsCategoryColorizationEnabled();

		if (ImGui::MenuItem("Auto-scroll", nullptr, &bAutoScroll))
		{
			OutputLog->SetAutoScroll(bAutoScroll);
		}

		if (ImGui::MenuItem("Pause", nullptr, &bPaused))
		{
			OutputLog->SetPaused(bPaused);
		}

		if (ImGui::MenuItem("Colorize categories", nullptr, &bColorize))
		{
			OutputLog->SetCategoryColorization(bColorize);
		}

		ImGui::EndPopup();
	}

	if (bCopyRequested)
	{
		ImGui::SetClipboardText(OutputLog->CopySelectionOrVisible().c_str());
	}

	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float CommandRowPadding = ImGui::GetStyle().WindowPadding.y;
	const float FooterHeight = ImGui::GetFontSize() + 10.f * InterfaceScale + CommandRowPadding;
	ToolUI->PushLogFont();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.f * InterfaceScale, 8.f * InterfaceScale});

	if (ImGui::BeginChild("OutputLogEntries", {0.f, -FooterHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const std::span<const FOutputLogLine> Lines = OutputLog->GetVisibleLines();
		const std::span<const std::string> TextLines = OutputLog->GetVisibleText();
		const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
		const bool bShouldScroll = !Lines.empty() && ShouldScrollOutputLog(bReceivedRecords, OutputLog->IsAutoScroll(), bWasAtBottom, OutputLog->HasTailRequest());
		const float LineHeight = ImGui::GetFontSize() + 3.f * InterfaceScale;
		const float TextOffsetY = (LineHeight - ImGui::GetFontSize()) * 0.5f;
		const ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		float TimeWidth = 0.f;
		float CategoryWidth = 0.f;

		for (const FOutputLogLine& Line : Lines)
		{
			TimeWidth = std::max(TimeWidth, ImGui::CalcTextSize(Line.Text.data(), Line.Text.data() + Line.TimeEnd, false).x);
			CategoryWidth = std::max(CategoryWidth, ImGui::CalcTextSize(Line.Text.data() + Line.CategoryBegin, Line.Text.data() + Line.CategoryEnd, false).x);
		}

		const float CategoryX = TimeWidth + 12.f * InterfaceScale;
		const FOutputLogColumns Columns{.CategoryX = CategoryX, .MessageX = CategoryX + CategoryWidth + 8.f * InterfaceScale};
		float ContentWidth = AvailableSize.x;

		for (const FOutputLogLine& Line : Lines)
		{
			ContentWidth = std::max(ContentWidth, MeasureOutputLogTextPrefix(Line, Line.Text.size(), Columns) + 8.f * InterfaceScale);
		}

		// Available height includes the scroll offset, so it must not determine content height.
		const float ContentHeight = std::max(1.f, static_cast<float>(TextLines.size()) * LineHeight);
		const ImVec2 TextOrigin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##OutputLogText", {ContentWidth, ContentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
		FLogTextSelection& Selection = OutputLog->GetSelection();

		if (ImGui::IsItemActivated())
		{
			Selection.Begin(HitTestText(Lines, Columns, TextOrigin, LineHeight, ImGui::GetMousePos()), ImGui::GetIO().KeyShift);
		}

		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 MousePosition = ImGui::GetMousePos();
			Selection.Update(HitTestText(Lines, Columns, TextOrigin, LineHeight, MousePosition));
			const ImVec2 WindowPosition = ImGui::GetWindowPos();
			const ImVec2 ContentMinimum = ImGui::GetWindowContentRegionMin();
			const ImVec2 ContentMaximum = ImGui::GetWindowContentRegionMax();
			const float ScrollStep = 360.f * ImGui::GetIO().DeltaTime;

			if (MousePosition.y < WindowPosition.y + ContentMinimum.y)
			{
				ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - ScrollStep));
			}
			else if (MousePosition.y > WindowPosition.y + ContentMaximum.y)
			{
				ImGui::SetScrollY(ImGui::GetScrollY() + ScrollStep);
			}
		}

		if (bShouldScroll)
		{
			ImGui::SetScrollHereY(1.f);
			OutputLog->AcknowledgeTailRequest();
		}

		if (ImGui::IsWindowFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
		{
			Selection.SelectAll(TextLines);
		}

		if (ImGui::IsWindowFocused() && Selection.HasSelection() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
		{
			const std::string Clipboard = Selection.Copy(TextLines);
			ImGui::SetClipboardText(Clipboard.c_str());
		}

		const auto [SelectionFirst, SelectionLast] = Selection.GetOrderedRange();
		const bool bHasSelection = Selection.HasSelection();
		const ImU32 SelectionColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
		const float WindowTop = ImGui::GetWindowPos().y;
		const float WindowBottom = WindowTop + ImGui::GetWindowSize().y;
		const std::size_t FirstVisibleLine = TextOrigin.y < WindowTop ? std::min(TextLines.size(), static_cast<std::size_t>((WindowTop - TextOrigin.y) / LineHeight)) : 0;
		const std::size_t LastVisibleLine = std::min(TextLines.size(), static_cast<std::size_t>(std::max(0.f, (WindowBottom - TextOrigin.y) / LineHeight)) + 1);
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();

		for (std::size_t LineIndex = FirstVisibleLine; LineIndex < LastVisibleLine; ++LineIndex)
		{
			const FOutputLogLine& Line = Lines[LineIndex];
			const std::string& Text = TextLines[LineIndex];
			const float LineY = TextOrigin.y + static_cast<float>(LineIndex) * LineHeight;

			if (bHasSelection && LineIndex >= SelectionFirst.Line && LineIndex <= SelectionLast.Line)
			{
				const std::size_t FirstByte = LineIndex == SelectionFirst.Line ? SelectionFirst.Byte : 0;
				const std::size_t LastByte = LineIndex == SelectionLast.Line ? SelectionLast.Byte : Text.size();
				const float SelectionX = TextOrigin.x + MeasureOutputLogTextPrefix(Line, FirstByte, Columns);
				float SelectionEndX = TextOrigin.x + MeasureOutputLogTextPrefix(Line, LastByte, Columns);

				if (LineIndex < SelectionLast.Line)
				{
					SelectionEndX += ImGui::GetFontSize() * 0.35f;
				}

				DrawList->AddRectFilled({SelectionX, LineY}, {std::max(SelectionX + 1.f, SelectionEndX), LineY + LineHeight}, SelectionColor);
			}

			const ImU32 Color = ResolveLineColor(Line, OutputLog->IsCategoryColorizationEnabled());
			DrawList->AddText({TextOrigin.x, LineY + TextOffsetY}, PackColor(ToolUITheme::TextMuted), Text.data(), Text.data() + Line.TimeEnd);
			DrawList->AddText({TextOrigin.x + Columns.CategoryX, LineY + TextOffsetY}, Color, Text.data() + Line.CategoryBegin, Text.data() + Line.CategoryEnd);
			DrawList->AddText({TextOrigin.x + Columns.MessageX, LineY + TextOffsetY}, Color, Text.data() + Line.MessageBegin, Text.data() + Text.size());
		}
	}

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ToolUI->PopLogFont();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + CommandRowPadding - ImGui::GetStyle().ItemSpacing.y);

	struct FInputCallbackContext
	{
		FImplementation* Editor;
	};

	FInputCallbackContext CallbackContext{this};

	const auto InputCallback = [](ImGuiInputTextCallbackData* const Data)
	{
		FImplementation& Editor = *static_cast<FInputCallbackContext*>(Data->UserData)->Editor;

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
		{
			return HandleConsoleInputShortcuts(*Data);
		}

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit || Data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			HandleConsoleInputShortcuts(*Data);
			if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit || Data->BufDirty)
			{
				Editor.SuggestionIndex = 0;
				Editor.RebuildSuggestions({Data->Buf, static_cast<std::size_t>(Data->BufTextLen)});
			}

			return 0;
		}

		if (Data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			if (Editor.SuggestionIndex >= 0 && Editor.SuggestionIndex < static_cast<int>(Editor.Suggestions.size()))
			{
				const std::string& Suggestion = Editor.Suggestions[static_cast<std::size_t>(Editor.SuggestionIndex)];
				Data->DeleteChars(0, Data->BufTextLen);
				Data->InsertChars(0, Suggestion.data(), Suggestion.data() + Suggestion.size());
				Editor.Suggestions.clear();
				Editor.SuggestionIndex = -1;
			}

			return 0;
		}

		if (!Editor.Suggestions.empty())
		{
			if (Data->EventKey == ImGuiKey_UpArrow)
			{
				Editor.SuggestionIndex = std::max(0, Editor.SuggestionIndex - 1);
			}
			else if (Data->EventKey == ImGuiKey_DownArrow)
			{
				Editor.SuggestionIndex = std::min(static_cast<int>(Editor.Suggestions.size()) - 1, Editor.SuggestionIndex + 1);
			}

			return 0;
		}

		const int Direction = Data->EventKey == ImGuiKey_UpArrow ? -1 : Data->EventKey == ImGuiKey_DownArrow ? 1
		                                                                                                     : 0;

		if (Direction != 0)
		{
			const std::string HistoryCommand = Editor.OutputLog->NavigateHistory(Direction);
			Data->DeleteChars(0, Data->BufTextLen);

			if (!HistoryCommand.empty())
			{
				Data->InsertChars(0, HistoryCommand.data(), HistoryCommand.data() + HistoryCommand.size());
			}
		}

		return 0;
	};

	constexpr ImGuiInputTextFlags CommandFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCharFilter;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * InterfaceScale, 5.f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	constexpr const char* SubmitLabel = "Enter";
	const float SubmitWidth = ImGui::CalcTextSize(SubmitLabel).x + ImGui::GetStyle().FramePadding.x * 2.f;
	ImGui::SetNextItemWidth(-(SubmitWidth + ImGui::GetStyle().ItemSpacing.x));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {32.f * InterfaceScale, 5.f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.5f + 5.f * InterfaceScale);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(PackColor(ToolUITheme::Border)));

	if (bFocusCommandRequested)
	{
		ImGui::SetKeyboardFocusHere();
		bFocusCommandRequested = false;
	}

	ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
	const bool bCommandSubmitted = ImGui::InputTextWithHint("##OutputLogCommand", "Enter Console Command", CommandBuffer.data(), CommandBuffer.size(), CommandFlags, InputCallback, &CallbackContext);
	const bool bCommandActive = ImGui::IsItemActive();
	const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

	if (bEscapePressed)
	{
		Suggestions.clear();
		SuggestionIndex = -1;
	}

	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(3);

	if (bCommandSubmitted)
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleVar(2);
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}

	const ImVec2 InputMinimum = ImGui::GetItemRectMin();
	const ImVec2 InputMaximum = ImGui::GetItemRectMax();
	const float PromptX = InputMinimum.x + 12.f * InterfaceScale;
	const float PromptY = (InputMinimum.y + InputMaximum.y) * 0.5f;
	const ImU32 PromptColor = PackColor(ToolUITheme::TextMuted);
	ImDrawList* const CommandDraw = ImGui::GetWindowDrawList();
	CommandDraw->PushClipRect(InputMinimum, InputMaximum, true);
	CommandDraw->AddLine({PromptX, PromptY - 4.f * InterfaceScale}, {PromptX + 4.f * InterfaceScale, PromptY}, PromptColor, InterfaceScale);
	CommandDraw->AddLine({PromptX + 4.f * InterfaceScale, PromptY}, {PromptX, PromptY + 4.f * InterfaceScale}, PromptColor, InterfaceScale);
	CommandDraw->AddLine({PromptX + 7.f * InterfaceScale, PromptY + 4.f * InterfaceScale}, {PromptX + 12.f * InterfaceScale, PromptY + 4.f * InterfaceScale}, PromptColor, InterfaceScale);
	CommandDraw->PopClipRect();

	if (bReclaimCommandFocus)
	{
		ImGui::SetKeyboardFocusHere(-1);
		bReclaimCommandFocus = false;
	}

	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});

	if (ImGui::Button(SubmitLabel, {SubmitWidth, 0.f}))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar(4);
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}

	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(4);

	std::optional<std::string> ClickedSuggestion;
	bool bSuggestionsHovered = false;

	if (!Suggestions.empty())
	{
		const std::size_t VisibleCount = std::min<std::size_t>(6, Suggestions.size());
		const float PopupPadding = 4.f * InterfaceScale;
		const float PopupHeight = static_cast<float>(VisibleCount) * ImGui::GetFrameHeight() + PopupPadding * 2.f;
		const ImGuiViewport* const Viewport = ImGui::GetWindowViewport();
		ImGui::SetNextWindowPos({InputMinimum.x, std::max(Viewport->WorkPos.y, InputMinimum.y - PopupHeight)});
		ImGui::SetNextWindowSize({InputMaximum.x - InputMinimum.x, PopupHeight});
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::SetNextWindowBgAlpha(0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PopupPadding, PopupPadding});
		constexpr ImGuiWindowFlags SuggestionFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

		if (ImGui::Begin("Command suggestions###OutputLogCommandSuggestions", nullptr, SuggestionFlags))
		{
			bSuggestionsHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			const ImVec2 Position = ImGui::GetWindowPos();
			const ImVec2 Size = ImGui::GetWindowSize();
			ImGui::GetWindowDrawList()->PushClipRect(Position, {Position.x + Size.x, Position.y + Size.y}, false);
			ToolUI->DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI->GetMetrics().PopupRounding * InterfaceScale);
			ImGui::GetWindowDrawList()->PopClipRect();

			for (std::size_t Index = 0; Index < Suggestions.size(); ++Index)
			{
				ImGui::PushID(static_cast<int>(Index));

				if (ImGui::Selectable(Suggestions[Index].c_str(), SuggestionIndex == static_cast<int>(Index)))
				{
					ClickedSuggestion = Suggestions[Index];
				}

				if (ImGui::IsItemHovered())
				{
					SuggestionIndex = static_cast<int>(Index);
				}

				ImGui::PopID();
			}
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	if (ClickedSuggestion)
	{
		CopyBuffer(CommandBuffer, *ClickedSuggestion);
		Suggestions.clear();
		SuggestionIndex = -1;
		bReclaimCommandFocus = true;
	}
	else if (ShouldDismissConsoleSuggestions(bCommandActive, bSuggestionsHovered, bEscapePressed))
	{
		Suggestions.clear();
		SuggestionIndex = -1;
	}

	ToolUI->EndPanel();
	return {};
}
}
