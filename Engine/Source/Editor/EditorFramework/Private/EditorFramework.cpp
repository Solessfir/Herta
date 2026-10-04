#include "Herta/EditorFramework/EditorFramework.h"

#include "ConsoleInput.h"
#include "DetailsPanel.h"
#include "EditorScene.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/PreviewSelection.h"
#include "Herta/EditorCore/SceneCommands.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/ViewportInteraction.h"
#include "Herta/Platform/FileDialog.h"
#include "Herta/Platform/Process.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"
#include "NumericField.h"
#include "OutlinerPanel.h"
#include "OutputLogTextLayout.h"
#include "PlaceObjectsMenu.h"
#include "PreviewAssets.h"
#include "PreviewScene.h"
#include "PreviewSimulation.h"
#include "ViewportBoxSelection.h"
#include "ViewportGizmos.h"
#include "ViewportIsland.h"
#include "ViewportRotationFeedback.h"
#include "ViewportScaleGizmo.h"
#include "ViewportStats.h"

#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
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

// Space-separated keys become keycaps; "+" and "/" stay plain separators.
float DrawKeyChord(const std::string_view Keys, const bool bDraw = true)
{
	const float Spacing = std::round(ImGui::GetFontSize() * 0.25f);
	float Width = 0.f;
	bool bFirst = true;

	for (const auto Part : std::views::split(Keys, ' '))
	{
		const std::string_view Token(Part.begin(), Part.end());
		if (Token.empty())
		{
			continue;
		}

		if (!bFirst)
		{
			Width += Spacing;

			if (bDraw)
			{
				ImGui::SameLine(0.f, Spacing);
			}
		}

		bFirst = false;

		if (Token == "+" || Token == "/")
		{
			Width += ImGui::CalcTextSize(Token.data(), Token.data() + Token.size()).x;

			if (bDraw)
			{
				ImGui::TextDisabled("%.*s", static_cast<int>(Token.size()), Token.data());
			}
		}
		else
		{
			Width += DrawKeycap(Token, bDraw);
		}
	}

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

	[[nodiscard]] std::expected<void, FEditorFrameworkError> DrawOutputLog();
	void DrawStartPanel();
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
	void ImportWithDialog();
	void OpenSceneWithDialog();
	void SaveCurrentScene();
	void RefreshScene(bool bPreserveGizmoDrag = false);
	void ApplyAuthoringAction(EAuthoringAction Action);
	void ReportSceneResult(std::expected<void, FSceneError> Result);
	// Maps the built-in cube's [-1, 1] box onto the object's mesh bounds, so cube-based picking, outlines, and physics fit any mesh.
	[[nodiscard]] FMatrix4 GetPreviewBoundsMatrix(std::size_t Index) const;
	[[nodiscard]] FPreviewBodyShape GetPreviewBodyShape(std::size_t Index) const;
	[[nodiscard]] FPreviewObject& GetActivePreviewObject() noexcept;
	void SetPreviewSelection(int ObjectIndex, bool bToggle = false);
	void SetPreviewSelection(FPreviewSelection Selection);
	void CancelViewportBoxSelection();
	void RequestPreviewRename();
	void ToggleSimulation();
	void UpdateSimulation(float DeltaSeconds);
	void RebuildSuggestions(std::string_view Prefix);
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();

	FToolUIContext* ToolUI = nullptr;
	FLogService* Log = nullptr;

	bool bOutlinerOpen = true;
	bool bDetailsOpen = true;
	bool bStartPanelOpen = false;
	FDetailsPanelState DetailsPanelState;
	FOutlinerPanelState OutlinerPanelState;
	FPlaceObjectsMenuState PlaceObjectsMenuState;
	bool bPlaceObjectsRequested = false;

	bool bOutputLogOpen = true;
	std::unique_ptr<FOutputLogModel> OutputLog;
	// Declared after OutputLog so it drains before the model that forwards to it is destroyed.
	std::unique_ptr<FShellRunner> Shell;
	std::array<char, 512> SearchBuffer = {};
	std::array<char, 512> CommandBuffer = {};
	std::vector<std::string> Suggestions;
	int SuggestionIndex = -1;
	bool bReclaimCommandFocus = false;
	bool bFocusCommandRequested = false;

	std::shared_ptr<FEditorScene> Scene = std::make_shared<FEditorScene>();
	std::vector<FPreviewObject>& PreviewObjects = Scene->GetObjects();
	FPreviewObject EmptyPreviewObject;
	std::uint64_t SceneGeneration = 0;
	std::vector<FMatrix4> PreviewModels;
	// Null entries draw the built-in cube. Refreshed from Assets at the start of every frame.
	std::vector<const FRenderMesh*> PreviewMeshes;
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
	FVector2 ViewportProjectionCenter{0.5f, 0.5f};
	FVector2 ViewportVisibleSize{1.f, 1.f};
	FViewportInteractionState ViewportInteraction;
	FViewportBoxSelectionState ViewportBoxSelection;
	bool bViewportControlsHovered = false;
	double CameraCoordinatesCopiedUntil = 0.0;
	float SnapIslandWidth = ViewportIconButtonSize + 6.f;
	float WorldIslandWidth = ViewportIconButtonSize + 6.f;

	bool bGameView = false;
	bool bGridVisible = true;
	bool bAxesVisible = false;
	bool bOrientationIndicatorVisible = true;
	bool bBoundsVisible = false;
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

	std::vector<std::string> MeshOptions;
	std::vector<FAssetId> MeshOptionIds;
	std::unique_ptr<FPreviewAssets> Assets;
	FEditorAssetPaths AssetPaths;
	FTaskSystem* Tasks = nullptr;
	IGraphicsDevice* GraphicsDevice = nullptr;
};

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
	Implementation->Scene->SetPath(Descriptor.ScenePath);

	if (!Descriptor.ScenePath.empty())
	{
		std::error_code Error;
		const bool bExists = std::filesystem::exists(Descriptor.ScenePath, Error);
		if (Error)
		{
			return std::unexpected(FEditorFrameworkError{std::format("Cannot query scene path: {}", Error.message())});
		}

		if (bExists)
		{
			if (auto Loaded = Implementation->Scene->Load(Descriptor.ScenePath); !Loaded)
			{
				return std::unexpected(FEditorFrameworkError{Loaded.error().Message});
			}
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

	Implementation->RefreshScene();

	if (auto Result = RegisterSceneFileCommands(*Descriptor.Commands); !Result)
	{
		return std::unexpected(FEditorFrameworkError{Result.error().Message});
	}

	if (auto Result = RegisterEditorSceneCommands(*Descriptor.Commands, Implementation->Scene); !Result)
	{
		return std::unexpected(FEditorFrameworkError{Result.error().Message});
	}

	if (auto Result = RegisterViewportStatsCommand(*Descriptor.Commands, Implementation->Stats); !Result)
	{
		return std::unexpected(FEditorFrameworkError{std::move(Result.error().Message)});
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

bool FEditorFramework::IsUnitStatsVisible() const noexcept
{
	return Implementation->Stats->bUnitVisible;
}

std::expected<void, FEditorFrameworkError> FEditorFramework::Draw(const std::function<void()>& RenderViewport)
{
	ImGuiIO& IO = ImGui::GetIO();
	Implementation->RefreshScene();
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

	Implementation->bSimulationStoppedThisFrame = false;

	if (Implementation->Assets)
	{
		Implementation->Assets->ImportFiles(Implementation->ToolUI->TakeDroppedFiles());
		Implementation->Assets->Tick();
	}

	Implementation->RefreshPreviewMeshes();

	if (!IO.AppFocusLost && Implementation->Simulation.IsRunning() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		Implementation->ToggleSimulation();
	}

	if (!IO.AppFocusLost && !IO.WantTextInput && IO.KeyAlt && !IO.KeyCtrl && !IO.KeyShift && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false) && !Implementation->Simulation.IsRunning() && !Implementation->Scene->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		Implementation->ToggleSimulation();
	}

	Implementation->UpdateSimulation(IO.DeltaTime);

	if (IO.KeyMods == 0 && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
	{
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

	bool bImportRequested = false;
	bool bOpenSceneRequested = false;
	bool bPlaceObjectsRequested = std::exchange(Implementation->bPlaceObjectsRequested, false);
	const bool bAuthoringAvailable = !Implementation->Simulation.IsRunning() && !Implementation->Scene->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0;
	const bool bShortcutsAvailable = bAuthoringAvailable && !IO.AppFocusLost && !IO.WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !IO.KeyAlt && !IO.KeySuper;
	bool bSaveSceneRequested = bShortcutsAvailable && IO.KeyCtrl && !IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false);
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

	const std::string SceneTitle = std::format("{}{} - Herta Editor", Implementation->Scene->GetName(), Implementation->Scene->IsDirty() ? "*" : "");
	Implementation->ToolUI->DrawWorkspace(SceneTitle, [&]
	{
		ToolUIMenuItem("Start panel", EToolUIMenuIcon::Panel, &Implementation->bStartPanelOpen);
		ImGui::Separator();
		ToolUIMenuItem("Outliner", EToolUIMenuIcon::Outliner, &Implementation->bOutlinerOpen);
		ToolUIMenuItem("Details", EToolUIMenuIcon::Details, &Implementation->bDetailsOpen);
		ImGui::Separator();
		ToolUIMenuItem("Output Log", EToolUIMenuIcon::Log, &Implementation->bOutputLogOpen);
	}, [&]
	{
		const float Scale = ImGui::GetFontSize() / Implementation->ToolUI->GetMetrics().BaseFontSize;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.f * Scale, (26.f * Scale - ImGui::GetFontSize()) * 0.5f});
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * Scale);
		ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});

		const auto StatusButton = [Scale](const char* Label, const bool bAppearance, const bool bSelected)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, bSelected ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
			ImGui::PushID(Label);
			const bool bPressed = ImGui::Button("##Status", {ImGui::CalcTextSize(Label).x + 42.f * Scale, 26.f * Scale});
			const ImVec2 Minimum = ImGui::GetItemRectMin();
			const ImVec2 Center{Minimum.x + 16.f * Scale, Minimum.y + 13.f * Scale};
			ImDrawList* const DrawList = ImGui::GetWindowDrawList();
			const ImU32 Color = ImGui::GetColorU32(ImGuiCol_Text);

			if (bAppearance)
			{
				for (int Row = -1; Row <= 1; ++Row)
				{
					const float Y = Center.y + static_cast<float>(Row) * 4.f * Scale;
					DrawList->AddLine({Center.x - 6.f * Scale, Y}, {Center.x + 6.f * Scale, Y}, Color, Scale);
					const float X = Center.x + (Row == 0 ? 2.f : -2.f) * Scale;
					DrawList->AddLine({X, Y - 2.f * Scale}, {X, Y + 2.f * Scale}, Color, 2.f * Scale);
				}
			}
			else
			{
				DrawList->AddLine({Center.x - 6.f * Scale, Center.y - 4.f * Scale}, {Center.x - 2.f * Scale, Center.y}, Color, Scale);
				DrawList->AddLine({Center.x - 2.f * Scale, Center.y}, {Center.x - 6.f * Scale, Center.y + 4.f * Scale}, Color, Scale);
				DrawList->AddLine({Center.x + Scale, Center.y + 4.f * Scale}, {Center.x + 6.f * Scale, Center.y + 4.f * Scale}, Color, Scale);
			}

			DrawList->AddText({Minimum.x + 30.f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, Color, Label);
			ImGui::PopID();
			ImGui::PopStyleColor();
			return bPressed;
		};

		if (StatusButton("Output Log", false, Implementation->bOutputLogOpen))
		{
			Implementation->bOutputLogOpen = !Implementation->bOutputLogOpen;
		}

		ImGui::SameLine();
		const ImVec2 DotPosition = ImGui::GetCursorScreenPos();
		ImGui::Dummy({8.f * Scale, 26.f * Scale});
		ImGui::SameLine(0.f, 3.f * Scale);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Ready");
		const float ReadyCenterY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
		ImGui::GetWindowDrawList()->AddCircleFilled({DotPosition.x + 4.f * Scale, ReadyCenterY}, 2.5f * Scale, IM_COL32(164, 189, 148, 255));
		ImGui::SameLine();
		ImGui::TextDisabled("%zu objects", Implementation->PreviewObjects.size());
		const float Width = ImGui::CalcTextSize("Appearance").x + 42.f * Scale;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - Width - 12.f));

		if (StatusButton("Appearance", true, false))
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
				Appearance = FEditorAppearance{};
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
			DrawFieldLabel("Reduced motion", ValueWidth);
			ToolUIToggle("##ReducedMotion", &Appearance.bReducedMotion);
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

			ImGui::PopStyleVar();
			Implementation->ToolUI->SetAppearance(Appearance);
			ImGui::EndPopup();
		}
	}, [&]
	{
		ImGui::TextUnformatted("Scene");
		ImGui::Separator();
		ImGui::BeginDisabled(!bAuthoringAvailable);
		bOpenSceneRequested = ToolUIMenuItem("Open scene...", EToolUIMenuIcon::Open);
		bSaveSceneRequested |= ToolUIMenuItem("Save scene", EToolUIMenuIcon::Save, nullptr, "Ctrl+S");
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextUnformatted("Content");
		ImGui::Separator();
		ImGui::BeginDisabled(!Implementation->Assets);
		bImportRequested = ToolUIMenuItem("Import...", EToolUIMenuIcon::Import);
		ImGui::EndDisabled();
	}, [&]
	{
		ImGui::BeginDisabled(!bAuthoringAvailable);
		ImGui::BeginDisabled(!Implementation->Scene->CanUndo());
		const std::string UndoLabel = Implementation->Scene->CanUndo() ? std::format("Undo {}", Implementation->Scene->GetUndoLabel()) : "Undo";
		if (ToolUIMenuItem(UndoLabel, EToolUIMenuIcon::Undo, nullptr, "Ctrl+Z"))
		{
			AuthoringAction = EAuthoringAction::Undo;
		}

		ImGui::EndDisabled();
		ImGui::BeginDisabled(!Implementation->Scene->CanRedo());
		const std::string RedoLabel = Implementation->Scene->CanRedo() ? std::format("Redo {}", Implementation->Scene->GetRedoLabel()) : "Redo";
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
		AuthoringAction = *Placement == EPlaceObjectType::EmptyEntity ? EAuthoringAction::CreateEmpty : EAuthoringAction::Create;
	}

	ImGui::EndDisabled();
	Implementation->ApplyAuthoringAction(AuthoringAction);

	if (bOpenSceneRequested)
	{
		if (Implementation->Scene->IsDirty())
		{
			ImGui::OpenPopup("Unsaved scene changes");
		}
		else
		{
			Implementation->OpenSceneWithDialog();
			Implementation->RefreshScene();
		}
	}

	if (ImGui::BeginPopupModal("Unsaved scene changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("Save changes before opening another scene?");
		ImGui::Spacing();
		bool bContinueOpening = false;
		if (ImGui::Button("Save"))
		{
			Implementation->SaveCurrentScene();
			bContinueOpening = !Implementation->Scene->IsDirty();
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
			Implementation->OpenSceneWithDialog();
			Implementation->RefreshScene();
		}

		ImGui::EndPopup();
	}

	if (bSaveSceneRequested)
	{
		Implementation->SaveCurrentScene();
	}

	// The native dialog is modal and blocks this frame, like Unreal's import dialog, so nothing outlives editor shutdown.
	if (bImportRequested)
	{
		Implementation->ImportWithDialog();
	}

	if (Implementation->bStartPanelOpen)
	{
		Implementation->DrawStartPanel();
	}

	if (Implementation->bOutlinerOpen)
	{
		Implementation->DrawOutlinerPanel();
	}

	if (Implementation->bDetailsOpen)
	{
		Implementation->DrawDetailsPanel();
	}

	Implementation->DrawViewport(RenderViewport);
	if (!Implementation->Simulation.IsRunning())
	{
		if (Implementation->bViewportEditCanceled && Implementation->Scene->HasActiveEdit())
		{
			Implementation->ReportSceneResult(Implementation->Scene->CancelEdit());
		}
		else if (Implementation->bViewportEditFinished && Implementation->Scene->HasActiveEdit())
		{
			Implementation->ReportSceneResult(Implementation->Scene->EndEdit());
		}
		else if (Implementation->Scene->HasActiveEdit() && !Implementation->PreviewDragStart && !ImGui::IsAnyItemActive() && !IO.WantTextInput && !Implementation->OutlinerPanelState.bRenaming)
		{
			Implementation->ReportSceneResult(Implementation->Scene->EndEdit());
		}
		else
		{
			Implementation->ReportSceneResult(Implementation->Scene->CommitEdits());
		}

		Implementation->bViewportEditFinished = false;
		Implementation->bViewportEditCanceled = false;
		Implementation->RefreshScene();
	}

	return Implementation->DrawOutputLog();
}

FOutputLogModel& FEditorFramework::GetOutputLog() noexcept
{
	return *Implementation->OutputLog;
}

const FOutputLogModel& FEditorFramework::GetOutputLog() const noexcept
{
	return *Implementation->OutputLog;
}

void FEditorFramework::SetViewportImage(const std::uint64_t TextureId) noexcept
{
	Implementation->ViewportTexture = TextureId;
}

FExtent2D FEditorFramework::GetViewportExtent() const noexcept
{
	return Implementation->ViewportExtent;
}

FMeshRenderView FEditorFramework::GetViewportRenderView() const noexcept
{
	return Implementation->ViewportRenderView;
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

	Assets->ImportFiles(std::move(*Chosen));
}

void FEditorFramework::FImplementation::RefreshPreviewMeshes()
{
	for (std::size_t Index = 0; Index < PreviewMeshes.size(); ++Index)
	{
		PreviewMeshes[Index] = Assets && PreviewObjects[Index].Mesh.IsValid() ? Assets->GetSlot(Index).Mesh.get() : nullptr;
	}
}

void FEditorFramework::FImplementation::RefreshScene(const bool bPreserveGizmoDrag)
{
	if (SceneGeneration == Scene->GetGeneration())
	{
		return;
	}

	SceneGeneration = Scene->GetGeneration();
	PreviewSelection.Indices.clear();
	PreviewSelection.Active = -1;
	PreviewSelection.Anchor = -1;
	for (const FObjectId Id : Scene->GetSelection())
	{
		const auto Object = std::ranges::find(PreviewObjects, Id, &FPreviewObject::Id);
		if (Object == PreviewObjects.end())
		{
			continue;
		}

		const int Index = static_cast<int>(Object - PreviewObjects.begin());
		PreviewSelection.Indices.push_back(Index);
		if (Scene->GetActiveObject() == Id)
		{
			PreviewSelection.Active = Index;
		}
	}

	if (PreviewSelection.Active < 0 && !PreviewSelection.Indices.empty())
	{
		PreviewSelection.Active = PreviewSelection.Indices.back();
	}

	PreviewSelection.Anchor = PreviewSelection.Active;
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

	if (!Assets && Tasks != nullptr && GraphicsDevice != nullptr && !AssetPaths.ContentRoot.empty() && !AssetPaths.DerivedDataRoot.empty() && !AssetPaths.WorkerPath.empty() && !AssetPaths.TargetPlatform.empty())
	{
		Assets = FPreviewAssets::Create(*Tasks, *GraphicsDevice, *Log, AssetPaths, PreviewObjects.size());
	}

	std::vector<FAssetId> SceneAssets;
	SceneAssets.reserve(PreviewObjects.size());
	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
		SceneAssets.push_back(Object.Mesh);
	}

	if (Assets)
	{
		Assets->RebindObjects(SceneAssets);
		RefreshPreviewMeshes();
	}

	ViewportRenderView.Models = PreviewModels;
	ViewportRenderView.Meshes = PreviewMeshes;
}

void FEditorFramework::FImplementation::ReportSceneResult(const std::expected<void, FSceneError> Result)
{
	if (!Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Scene operation failed: {}", Result.error().Message);
	}
}

void FEditorFramework::FImplementation::ApplyAuthoringAction(const EAuthoringAction Action)
{
	if (Action == EAuthoringAction::None || Simulation.IsRunning() || Scene->HasActiveEdit())
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
		if (Scene->CanUndo())
		{
			ReportSceneResult(Scene->Undo());
		}
	}
	else if (Action == EAuthoringAction::Redo)
	{
		if (Scene->CanRedo())
		{
			ReportSceneResult(Scene->Redo());
		}
	}
	else if (Action == EAuthoringAction::Create || Action == EAuthoringAction::CreateEmpty)
	{
		const FVector3 Position = ViewportCamera.GetPivot();
		const FWorldPosition WorldPosition{Position.X, Position.Y, Position.Z};
		const auto Result = Action == EAuthoringAction::CreateEmpty ? Scene->CreateEmptyEntity(WorldPosition) : Scene->CreateEntity(WorldPosition);
		if (!Result)
		{
			ReportSceneResult(std::unexpected(Result.error()));
		}
	}
	else if (Action == EAuthoringAction::Copy)
	{
		const auto Text = Scene->CopySelected();
		if (Text)
		{
			ImGui::SetClipboardText(Text->c_str());
		}
		else
		{
			ReportSceneResult(std::unexpected(Text.error()));
		}
	}
	else if (Action == EAuthoringAction::Paste)
	{
		if (const char* const Text = ImGui::GetClipboardText(); Text != nullptr)
		{
			ReportSceneResult(Scene->PasteEntities(Text));
		}
	}
	else if (Action == EAuthoringAction::Duplicate)
	{
		ReportSceneResult(Scene->DuplicateSelected(false, {TranslationSnap, 0.f, TranslationSnap}));
	}
	else if (Action == EAuthoringAction::Delete)
	{
		ReportSceneResult(Scene->DeleteSelected());
	}

	RefreshScene();
}

void FEditorFramework::FImplementation::OpenSceneWithDialog()
{
	const FFileDialogFilter Filter{.Name = "Herta scene", .Extensions = {"hscene"}};
	const std::filesystem::path Directory = Scene->GetPath().has_parent_path() ? Scene->GetPath().parent_path() : AssetPaths.ContentRoot.parent_path();
	const auto Chosen = OpenFilesDialog("Open scene", std::span(&Filter, 1), Directory);
	if (!Chosen)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not open scene dialog: {}", Chosen.error().Message);
		return;
	}

	if (!Chosen->empty())
	{
		if (auto Loaded = Scene->Load(Chosen->front()); !Loaded)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not load scene: {}", Loaded.error().Message);
		}
	}
}

void FEditorFramework::FImplementation::SaveCurrentScene()
{
	if (auto Result = Scene->Save(); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not save scene: {}", Result.error().Message);
		return;
	}

	HERTA_LOG_INFO(*Log, EditorLog, "Scene saved");
}

FMatrix4 FEditorFramework::FImplementation::GetPreviewBoundsMatrix(const std::size_t Index) const
{
	const FPreviewBodyShape Shape = GetPreviewBodyShape(Index);
	return FMatrix4::Translation(Shape.Center) * FMatrix4::Scale(Shape.HalfExtents);
}

FPreviewBodyShape FEditorFramework::FImplementation::GetPreviewBodyShape(const std::size_t Index) const
{
	if (!PreviewObjects[Index].Mesh.IsValid())
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
	const bool bReducedMotion = ToolUI->GetAppearance().bReducedMotion;
	const float AnimationStep = bReducedMotion ? 1.f : std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);

	const auto IconButton = [&](const char* Id, const EViewportIcon Icon, const char* Tooltip, const bool bSelected = false)
	{
		return ViewportIconButton(Id, Icon, Tooltip, Scale, bSelected, bReducedMotion);
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

		const float SnapTarget = bSnapEnabled ? ViewportIconButtonSize + 75.f : Height / Scale;
		SnapIslandWidth += (SnapTarget - SnapIslandWidth) * AnimationStep;
		const float SnapWidth = SnapIslandWidth * Scale;
		Island(Right - SnapWidth, SnapWidth);

		if (IconButton("Grid snap", EViewportIcon::Grid, "Toggle grid snapping (S)", bSnapEnabled))
		{
			bSnapEnabled = !bSnapEnabled;
		}

		if (bSnapEnabled && SnapIslandWidth > SnapTarget - 1.f)
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
			const float Target = bHover ? 100.f : Height / Scale;
			WorldIslandWidth += (Target - WorldIslandWidth) * AnimationStep;
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

		if (IconButton("Simulate", Simulation.IsRunning() ? EViewportIcon::Stop : EViewportIcon::Simulate, Simulation.IsRunning() ? "Stop simulation (Esc)" : "Simulate (Alt+S)", Simulation.IsRunning()))
		{
			ToggleSimulation();
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

		const auto ShortcutRow = [](const char* const Action, const char* const Keys)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Action);
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - DrawKeyChord(Keys, false));
			DrawKeyChord(Keys);
		};

		// Mirrors the toolbar's island breakpoints: gizmo modes and Focus appear here only while the toolbar hides them.
		const bool bToolbarShowsModes = Size.x > 340.f * Scale;
		const bool bToolbarShowsFocus = Size.x > 1040.f * Scale;

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
		}

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
		}

		if (Section("Navigation"))
		{
			ShortcutRow("Add object", "Shift + A");
			ShortcutRow("Fly", "RMB + WASD / QE");
			ShortcutRow("Orbit", "Alt + LMB");
			ShortcutRow("Pan", "MMB");
			ShortcutRow("Dolly", "Wheel");
			ShortcutRow("Fly speed", "RMB + Wheel");
		}

		ImGui::Spacing();
		ImGui::BeginDisabled(Simulation.IsRunning() || Scene->HasActiveEdit() || PreviewSelection.Active < 0);

		if (ImGui::Button("Reset preview transform", {-1.f, 0.f}))
		{
			ReportSceneResult(Scene->BeginEdit("Reset transform"));
			const FEditorScene DefaultScene;
			const auto& Defaults = DefaultScene.GetObjects();

			for (const int Index : PreviewSelection.Indices)
			{
				FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
				const auto Default = std::ranges::find(Defaults, Object.Id, &FPreviewObject::Id);
				Object.Translation = Default == Defaults.end() ? Im3d::Vec3(0.f) : Default->Translation;
				Object.Rotation = Default == Defaults.end() ? Im3d::Mat3(1.f) : Default->Rotation;
				Object.Scale = Default == Defaults.end() ? Im3d::Vec3(1.f) : Default->Scale;
			}

			ReportSceneResult(Scene->EndEdit());
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
		ReportSceneResult(Scene->CancelEdit());
		RefreshScene();
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
			const auto Result = Scene->BeginEdit(IO.KeyAlt ? "Duplicate objects" : "Transform objects");
			ReportSceneResult(Result);
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

	const bool bGizmoInput = ViewportInteraction.CanUseGizmo(InteractionInput) && !bBoxGesture;
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
	const auto Camera = ViewportCamera.GetSnapshot(AspectRatio, ViewportProjectionCenter);
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
	[[maybe_unused]] auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale, PreviewMesh, ObjectId] = Candidate;
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
		const auto Result = Scene->DuplicateSelected(true);
		ReportSceneResult(Result);
		if (Result)
		{
			RefreshScene(true);
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
	Im3d::PopLayerId();
	const Im3d::Mat4 Model(PreviewTranslation, PreviewRotation, PreviewScale);

	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
	}

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

	Im3d::PopLayerId();

	if (!bGameView && !PreviewObjects.empty())
	{
		Im3d::PushLayerId("ViewportSelection");

		for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
		{
			if (PreviewObjects[Index].Mesh.IsValid())
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

		for (const int Index : PreviewSelection.Indices)
		{
			const auto ObjectIndex = static_cast<std::size_t>(Index);
			if (!PreviewObjects[ObjectIndex].Mesh.IsValid())
			{
				continue;
			}

			for (const auto& [Start, End] : GetPreviewCubeSilhouette(Camera.Position, PreviewModels[ObjectIndex] * GetPreviewBoundsMatrix(ObjectIndex)))
			{
				Im3d::DrawLine(ToIm3dVector(Start), ToIm3dVector(End), ViewportGizmos.m_gizmoSizePixels * 0.5f, Im3d::Color(0xc2b584ff));
			}
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
	const bool bAuthoringAvailable = !Simulation.IsRunning() && !Scene->HasActiveEdit();
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
			ImGui::SetCursorScreenPos(CoordinatesPosition);
			ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
			const bool bCopyCoordinates = ImGui::InvisibleButton("Copy camera coordinates##CameraCoordinates", {CoordinatesWidth, CoordinatesHeight}, ImGuiButtonFlags_EnableNav);
			const bool bCoordinatesHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
			const bool bCoordinatesFocused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
			bViewportControlsHovered |= bCoordinatesHovered;

			if (bCoordinatesHovered)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			}

			ImGui::EndDisabled();
			ImGui::SetCursorScreenPos(ImageMinimum);
			ImGui::BeginDisabled(bViewportControlsHovered && ViewportInteraction.DragButton < 0);
			ImGui::InvisibleButton("##ViewportInteraction", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			ImGui::EndDisabled();
			UpdateViewport(RenderMinimum, RenderSize);
			DrawViewportContextMenu();
			const FVector3 CameraPosition = ViewportCamera.GetSnapshot(1.f).Position;
			const std::string Coordinates = FormatCameraHud(CameraPosition);

			if (bCopyCoordinates)
			{
				ImGui::SetClipboardText(FormatTransformVectorClipboard(CameraPosition).c_str());
				CameraCoordinatesCopiedUntil = ImGui::GetTime() + 1.5;
			}

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

			if (!bGameView && ViewportInteraction.CameraMode == EViewportCameraMode::Fly)
			{
				const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
				const std::string Speed = std::format("{:.2f} m/s", ViewportCamera.GetMovementSpeed() * (ImGui::GetIO().KeyShift ? 4.f : 1.f));
				const float LabelWidth = ImGui::CalcTextSize("Speed").x;
				const float Width = LabelWidth + ImGui::CalcTextSize(Speed.c_str()).x + 52.f * UiScale;
				const float Height = 32.f * UiScale;
				const ImVec2 Position{ImageMinimum.x + std::max(0.f, Size.x - Width - 14.f * UiScale), CoordinatesPosition.y - Height - 6.f * UiScale};
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
	if (PreviewSelection == Selection)
	{
		return;
	}

	if (OutlinerPanelState.bRenaming)
	{
		const auto Object = std::ranges::find(PreviewObjects, OutlinerPanelState.RenameObject, &FPreviewObject::Id);
		if (Object != PreviewObjects.end())
		{
			RenamePreviewObject(Object->Label, OutlinerPanelState.RenameBuffer.data());
			ReportSceneResult(Scene->CommitEdits("Rename object"));
		}
	}

	if (Scene->HasActiveEdit())
	{
		ReportSceneResult(Scene->EndEdit());
	}

	PreviewSelection = std::move(Selection);
	std::vector<FObjectId> SelectedIds;
	for (const int Index : PreviewSelection.Indices)
	{
		SelectedIds.push_back(PreviewObjects[static_cast<std::size_t>(Index)].Id);
	}

	Scene->SetSelection(SelectedIds, PreviewSelection.Active >= 0 ? std::optional(PreviewObjects[static_cast<std::size_t>(PreviewSelection.Active)].Id) : std::nullopt);
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

void FEditorFramework::FImplementation::ToggleSimulation()
{
	if (Simulation.IsRunning())
	{
		Simulation.Stop();
		PreviewObjects = std::move(SimulationStart);
		Scene->SetSimulationRunning(false);
		bSimulationStoppedThisFrame = true;
	}
	else
	{
		if (Scene->HasActiveEdit())
		{
			if (auto Result = Scene->EndEdit(); !Result)
			{
				ReportSceneResult(std::move(Result));
				return;
			}
		}

		if (auto Result = Scene->CommitEdits(); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not apply scene edits: {}", Result.error().Message);
			return;
		}

		std::vector<FPreviewSimulationBody> Bodies;
		for (const ESceneBodyType Type : {ESceneBodyType::Static, ESceneBodyType::Dynamic})
		{
			for (const std::size_t Index : Scene->FindBodies(Type))
			{
				Bodies.push_back({
				    .ObjectIndex = Index,
				    .Transform = ToHertaTransform(PreviewObjects[Index]),
				    .Shape = GetPreviewBodyShape(Index),
				    .MotionType = Type == ESceneBodyType::Dynamic ? EPhysicsMotionType::Dynamic : EPhysicsMotionType::Static,
				});
			}
		}

		if (Bodies.empty())
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Simulation preview requires a Static Mesh with a Rigid Body component");
			return;
		}

		std::vector<FPreviewObject> OriginalObjects = PreviewObjects;
		if (const auto Result = Simulation.Start(Bodies); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not start simulation: {}", Result.error().Message);
			return;
		}

		SimulationStart = std::move(OriginalObjects);
		Scene->SetSimulationRunning(true);
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

	for (const FPreviewSimulationTransform& Snapshot : Simulation.GetTransforms())
	{
		const FTransform& Transform = Snapshot.Transform;
		FPreviewObject& Object = PreviewObjects[Snapshot.ObjectIndex];
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
}

void FEditorFramework::FImplementation::DrawDetailsPanel()
{
	FPreviewObject PreviousObject = GetActivePreviewObject();
	[[maybe_unused]] auto& [Label, Translation, Rotation, Scale, Mesh, ObjectId] = GetActivePreviewObject();
	FDetailsMeshField MeshField;
	std::string MeshStatus;
	FDetailsComponentField Components{.bAllMesh = !PreviewSelection.Indices.empty(), .bAllBody = !PreviewSelection.Indices.empty()};
	bool bFirstBody = true;

	for (const int Index : PreviewSelection.Indices)
	{
		const FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
		const auto Handle = Scene->GetWorld().FindEntity(Object.Id);
		const auto Entity = Handle ? Scene->GetWorld().GetEntity(*Handle) : std::nullopt;
		const ESceneBodyType Type = Entity ? Entity->BodyType : ESceneBodyType::None;
		Components.bAnyMesh |= Object.Mesh.IsValid();
		Components.bAllMesh &= Object.Mesh.IsValid();
		Components.bMixedMeshAsset |= Object.Mesh != Mesh;
		Components.bAnyBody |= Type != ESceneBodyType::None;
		Components.bAllBody &= Type != ESceneBodyType::None;
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
		if (!Scene->HasActiveEdit())
		{
			ReportSceneResult(Scene->BeginEdit("Edit properties"));
		}
	},
	    .Flush = [&](const bool bCanceled)
	{
		if (bCanceled && Scene->HasActiveEdit())
		{
			ReportSceneResult(Scene->CancelEdit());
		}
		else if (Scene->HasActiveEdit())
		{
			ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
			ReportSceneResult(Scene->EndEdit());
		}

		PreviousObject = GetActivePreviewObject();
	},
	};
	const FDetailsMeshResult MeshResult = DrawPreviewDetailsPanel(*ToolUI, bDetailsOpen, PreviewSelection.Active >= 0, Simulation.IsRunning() || ViewportInteraction.DragButton >= 0, Translation, Rotation, Scale, DetailsPanelState, Label, PreviewSelection.Indices.size(), Assets && Mesh.IsValid() ? &MeshField : nullptr, &Edits, &Components);
	if (!MeshResult.bEditCanceled)
	{
		ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
	}

	if (Assets && MeshResult.bOptionsOpened)
	{
		Assets->RequestScan();
	}

	if (Assets && MeshResult.Chosen >= 0)
	{
		ReportSceneResult(Scene->BeginEdit("Change mesh"));
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
		ReportSceneResult(Scene->EndEdit());
	}

	if (MeshResult.bEditCanceled && Scene->HasActiveEdit())
	{
		ReportSceneResult(Scene->CancelEdit());
	}
	else if (MeshResult.bEditFinished && Scene->HasActiveEdit())
	{
		ReportSceneResult(Scene->EndEdit());
	}

	if (MeshResult.ComponentAction != EDetailsComponentAction::None || MeshResult.BodyTypeChosen)
	{
		if (Scene->HasActiveEdit())
		{
			ReportSceneResult(Scene->EndEdit());
		}

		switch (MeshResult.ComponentAction)
		{
			case EDetailsComponentAction::AddStaticMesh:
				ReportSceneResult(Scene->AddStaticMeshToSelected(EngineCubeAsset));
				break;
			case EDetailsComponentAction::RemoveStaticMesh:
				ReportSceneResult(Scene->RemoveStaticMeshFromSelected());
				break;
			case EDetailsComponentAction::AddRigidBody:
				ReportSceneResult(Scene->AddRigidBodyToSelected());
				break;
			case EDetailsComponentAction::RemoveRigidBody:
				ReportSceneResult(Scene->SetSelectedBodyType(ESceneBodyType::None));
				break;
			case EDetailsComponentAction::None:
				break;
		}

		if (MeshResult.BodyTypeChosen)
		{
			ReportSceneResult(Scene->SetSelectedBodyType(*MeshResult.BodyTypeChosen));
		}
	}

	RefreshScene();
}

void FEditorFramework::FImplementation::DrawOutlinerPanel()
{
	FPreviewSelection NewSelection = PreviewSelection;
	const bool bFocusRequested = DrawPreviewOutlinerPanel(*ToolUI, bOutlinerOpen, NewSelection, PreviewObjects, Simulation.IsRunning() || ViewportInteraction.DragButton >= 0, OutlinerPanelState);
	if (OutlinerPanelState.bRenameCommitted)
	{
		const auto Object = std::ranges::find(PreviewObjects, OutlinerPanelState.RenameObject, &FPreviewObject::Id);
		if (Object != PreviewObjects.end())
		{
			RenamePreviewObject(Object->Label, OutlinerPanelState.RenameBuffer.data());
			ReportSceneResult(Scene->CommitEdits("Rename object"));
		}
	}

	SetPreviewSelection(std::move(NewSelection));

	if (bFocusRequested)
	{
		FocusPreview();
	}
}

void FEditorFramework::FImplementation::DrawStartPanel()
{
	if (!ToolUI->BeginPanel("Start", &bStartPanelOpen))
	{
		ToolUI->EndPanel();
		return;
	}

	ImGui::TextDisabled("HERTA / NATIVE C++23");
	ImGui::Spacing();
	ImGui::TextUnformatted("Build something remarkable.");
	ImGui::TextDisabled("The Viewport renders a textured mesh through Herta RHI and RenderGraph.");

	ToolUI->EndPanel();
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

std::expected<void, FEditorFrameworkError> FEditorFramework::FImplementation::DrawOutputLog()
{
	const bool bReceivedRecords = OutputLog->Synchronize();

	if (!bOutputLogOpen)
	{
		return {};
	}

	if (bFocusCommandRequested)
	{
		ImGui::SetNextWindowFocus();
		ImGui::SetNextWindowCollapsed(false);
	}

	if (!ToolUI->BeginPanel(">_  Output Log###Output Log", &bOutputLogOpen))
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
		for (const ELogLevel Level : {ELogLevel::Trace, ELogLevel::Debug, ELogLevel::Info, ELogLevel::Warning, ELogLevel::Error, ELogLevel::Critical})
		{
			bool bVisible = OutputLog->IsLevelVisible(Level);
			const std::string LevelName{GetLogLevelName(Level)};

			if (ImGui::MenuItem(LevelName.c_str(), nullptr, &bVisible))
			{
				OutputLog->SetLevelVisible(Level, bVisible);
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
