#include "Herta/EditorFramework/EditorFramework.h"

#include "DetailsPanel.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/PreviewSelection.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/ViewportInteraction.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"
#include "NumericField.h"
#include "OutlinerPanel.h"
#include "OutputLogTextLayout.h"
#include "PreviewSimulation.h"
#include "PreviewScene.h"
#include "ViewportGizmos.h"
#include "ViewportIsland.h"
#include "ViewportRotationFeedback.h"
#include "ViewportScaleGizmo.h"
#include "ViewportStats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
inline constexpr FLogCategory EditorLog{"Editor"};
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
    IM_COL32(182, 190, 214, 255)};

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
	const float AxisLength = 16.0f * Scale;
	const float FontSize = ImGui::GetFontSize() * 0.8f;
	constexpr std::array Labels{"X", "Y", "Z"};
	std::array<ImVec2, 3> TipOffsets;
	std::array<ImVec2, 3> LabelOffsets;
	float Leftmost = -2.0f * Scale;
	float Bottommost = 2.0f * Scale;
	for (std::size_t Axis = 0; Axis < Labels.size(); ++Axis)
	{
		TipOffsets[Axis] = {-View(0, Axis) * AxisLength, -View(1, Axis) * AxisLength};
		const float Length = std::hypot(View(0, Axis), View(1, Axis));
		const ImVec2 Direction = Length > 0.001f ? ImVec2{-View(0, Axis) / Length, -View(1, Axis) / Length} : ImVec2{0.0f, 1.0f};
		const ImVec2 LabelSize = ImGui::GetFont()->CalcTextSizeA(FontSize, 100.0f * Scale, 0.0f, Labels[Axis]);
		const float LabelRadius = AxisLength * Length + 3.0f * Scale + std::hypot(LabelSize.x, LabelSize.y) * 0.5f;
		LabelOffsets[Axis] = {Direction.x * LabelRadius - LabelSize.x * 0.5f, Direction.y * LabelRadius - LabelSize.y * 0.5f};
		Leftmost = std::min({Leftmost, TipOffsets[Axis].x - 2.0f * Scale, LabelOffsets[Axis].x});
		Bottommost = std::max({Bottommost, TipOffsets[Axis].y + 2.0f * Scale, LabelOffsets[Axis].y + LabelSize.y});
	}
	const float Inset = 14.0f * Scale;
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
		DrawList->AddCircleFilled(Tip, 2.0f * Scale, ViewportAxisColors[Axis].getABGR());
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
	return {LineIndex, FindOutputLogByteAtX(Lines[LineIndex], MousePosition.x - TextOrigin.x, Columns)};
}

void CopyBuffer(std::span<char> Destination, const std::string_view Source)
{
	std::fill(Destination.begin(), Destination.end(), '\0');
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
	Im3d::Context& Previous;

	explicit FIm3dContextScope(Im3d::Context& Context) noexcept
	    : Previous(Im3d::GetContext())
	{
		Im3d::SetContext(Context);
	}

	~FIm3dContextScope()
	{
		Im3d::SetContext(Previous);
	}
};
}

struct FEditorFramework::FImplementation
{
	FToolUIContext* ToolUI = nullptr;
	FLogService* Log = nullptr;
	FPreviewSimulation Simulation;
	std::optional<FPreviewObject> SimulationStart;
	bool bSimulationStoppedThisFrame = false;
	FDetailsPanelState DetailsPanelState;
	FOutlinerPanelState OutlinerPanelState;
	std::shared_ptr<FViewportStats> Stats = std::make_shared<FViewportStats>();
	double CpuFrameMilliseconds = 0.0;
	std::optional<double> GpuUIMilliseconds;
	std::unique_ptr<FOutputLogModel> OutputLog;
	std::array<char, 512> SearchBuffer = {};
	std::array<char, 512> CommandBuffer = {};
	std::vector<std::string> Suggestions;
	int SuggestionIndex = -1;
	bool bOutputLogOpen = true;
	bool bReclaimCommandFocus = false;
	bool bFocusCommandRequested = false;
	std::uint64_t ViewportTexture = 0;
	FExtent2D ViewportExtent{960, 540};
	FViewportCameraController ViewportCamera;
	Im3d::Context ViewportGizmos;
	std::array<FPreviewObject, 2> PreviewObjects = CreatePreviewObjects();
	std::array<FMatrix4, 2> PreviewModels;
	std::optional<Im3d::Mat4> PreviewDragStart;
	std::array<FPreviewObject, 2> PreviewDragObjects;
	FViewportRotationFeedbackState RotationFeedback;
	FViewportScaleGizmoState ScaleGizmoState;
	FViewportScaleGizmoFeedback ScaleFeedback;
	FMeshRenderView ViewportRenderView;
	FVector2 ViewportProjectionCenter{0.5f, 0.5f};
	FVector2 ViewportVisibleSize{1.0f, 1.0f};
	std::vector<FDebugDrawVertex> ViewportDebugVertices;
	std::vector<FDebugDrawList> ViewportDebugDrawLists;
	FViewportInteractionState ViewportInteraction;
	FPreviewSelection PreviewSelection;
	bool bOutlinerOpen = true;
	bool bDetailsOpen = true;
	bool bStartPanelOpen = false;
	bool bLocalGizmo = false;
	bool bFlipGizmoAxesTowardCamera = false;
	bool bSnapEnabled = false;
	bool bGridVisible = true;
	bool bAxesVisible = false;
	bool bOrientationIndicatorVisible = true;
	bool bBoundsVisible = false;
	float TranslationSnap = 0.5f;
	float RotationSnapDegrees = 15.0f;
	float ScaleSnap = 0.1f;
	int GizmoMode = Im3d::GizmoMode_Translation;
	bool bTransformGizmoVisible = true;
	bool bViewportControlsHovered = false;
	double CameraCoordinatesCopiedUntil = 0.0;
	float SnapIslandWidth = ViewportIconButtonSize + 6.0f;
	float WorldIslandWidth = ViewportIconButtonSize + 6.0f;

	FImplementation();

	[[nodiscard]] std::expected<void, FEditorFrameworkError> DrawOutputLog();
	void DrawStartPanel();
	void DrawDetailsPanel();
	void DrawOutlinerPanel();
	void DrawViewport(const std::function<void()>& RenderViewport);
	void DrawViewportToolbar(ImVec2 Minimum, ImVec2 Size);
	void DrawViewportStats(ImVec2 Minimum, ImVec2 Size);
	void UpdateViewport(const ImVec2 RenderMinimum, const ImVec2 RenderSize);
	void BuildViewportDebugDraw(bool bGizmoInput, const FVector2 NormalizedMouse);
	void FocusPreview();
	[[nodiscard]] FPreviewObject& GetActivePreviewObject() noexcept { return PreviewObjects[static_cast<std::size_t>(std::max(PreviewSelection.Active, 0))]; }
	void SetPreviewSelection(int ObjectIndex, bool bToggle = false);
	void SetPreviewSelection(FPreviewSelection Selection);
	void RequestPreviewRename();
	void ToggleSimulation();
	void UpdateSimulation(float DeltaSeconds);
	void RebuildSuggestions();
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();
};

FEditorFramework::FImplementation::FImplementation()
{
	Im3d::Mat3& PreviewRotation = PreviewObjects[PreviewCubeIndex].Rotation;
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.0f, 1.0f, 0.0f}, 0.4f) * FQuaternion::FromAxisAngle({1.0f, 0.0f, 0.0f}, -0.25f);
	const FMatrix3 Matrix = FMatrix3::Rotation(Rotation);
	for (std::size_t Column = 0; Column < 3; ++Column)
	{
		for (std::size_t Row = 0; Row < 3; ++Row)
		{
			PreviewRotation(static_cast<int>(Row), static_cast<int>(Column)) = Matrix(Row, Column);
		}
	}
	const auto Camera = ViewportCamera.GetSnapshot(960.0f / 540.0f);
	for (std::size_t Index = 0; Index < PreviewObjects.size(); ++Index)
	{
		const FPreviewObject& Object = PreviewObjects[Index];
		PreviewModels[Index] = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
	}
	ViewportRenderView = {Camera.View, Camera.Projection, PreviewModels};
}

std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> FEditorFramework::Create(const FEditorFrameworkDescriptor Descriptor)
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

	try
	{
		auto Implementation = std::make_unique<FImplementation>();
		Implementation->ToolUI = Descriptor.ToolUI;
		Implementation->OutputLog = std::move(*OutputLog);
		Implementation->Log = Descriptor.Log;
		if (auto Result = RegisterViewportStatsCommand(*Descriptor.Commands, Implementation->Stats); !Result)
		{
			return std::unexpected(FEditorFrameworkError{std::move(Result.error().Message)});
		}
		return std::unique_ptr<FEditorFramework>(new FEditorFramework(std::move(Implementation)));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorFrameworkError{Exception.what()});
	}
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
	try
	{
		ImGuiIO& IO = ImGui::GetIO();
		Implementation->bSimulationStoppedThisFrame = false;
		if (!IO.AppFocusLost && Implementation->Simulation.IsRunning() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			Implementation->ToggleSimulation();
		if (!IO.AppFocusLost && !IO.WantTextInput && IO.KeyAlt && !IO.KeyCtrl && !IO.KeyShift && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false) && !Implementation->Simulation.IsRunning() && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			Implementation->ToggleSimulation();
		Implementation->UpdateSimulation(IO.DeltaTime);
		if (IO.KeyMods == 0 && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
		{
			Implementation->bOutputLogOpen = true;
			Implementation->bFocusCommandRequested = true;
			for (int Index = IO.InputQueueCharacters.Size - 1; Index >= 0; --Index)
			{
				if (IO.InputQueueCharacters[Index] == '`')
					IO.InputQueueCharacters.erase(IO.InputQueueCharacters.begin() + Index);
			}
		}
		Implementation->ToolUI->DrawWorkspace("Herta Editor", [&]
		                                      {
			                                      (void)ToolUIMenuItem("Start panel", EToolUIMenuIcon::Panel, &Implementation->bStartPanelOpen);
			                                      ImGui::Separator();
			                                      (void)ToolUIMenuItem("Outliner", EToolUIMenuIcon::Outliner, &Implementation->bOutlinerOpen);
			                                      (void)ToolUIMenuItem("Details", EToolUIMenuIcon::Details, &Implementation->bDetailsOpen);
			                                      ImGui::Separator();
			                                      (void)ToolUIMenuItem("Output Log", EToolUIMenuIcon::Log, &Implementation->bOutputLogOpen);
		                                      },
		                                      [&]
		                                      {
			                                      const float Scale = ImGui::GetFontSize() / Implementation->ToolUI->GetMetrics().BaseFontSize;
			                                      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f * Scale, (26.0f * Scale - ImGui::GetFontSize()) * 0.5f});
			                                      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			                                      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * Scale);
			                                      ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
			                                      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
			                                      ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
			                                      const auto StatusButton = [Scale](const char* Label, const bool bAppearance, const bool bSelected)
			                                      {
				                                      ImGui::PushStyleColor(ImGuiCol_Button, bSelected ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
				                                      ImGui::PushID(Label);
				                                      const bool bPressed = ImGui::Button("##Status", {ImGui::CalcTextSize(Label).x + 42.0f * Scale, 26.0f * Scale});
				                                      const ImVec2 Minimum = ImGui::GetItemRectMin();
				                                      const ImVec2 Center{Minimum.x + 16.0f * Scale, Minimum.y + 13.0f * Scale};
				                                      ImDrawList* const DrawList = ImGui::GetWindowDrawList();
				                                      const ImU32 Color = ImGui::GetColorU32(ImGuiCol_Text);
				                                      if (bAppearance)
				                                      {
					                                      for (int Row = -1; Row <= 1; ++Row)
					                                      {
						                                      const float Y = Center.y + static_cast<float>(Row) * 4.0f * Scale;
						                                      DrawList->AddLine({Center.x - 6.0f * Scale, Y}, {Center.x + 6.0f * Scale, Y}, Color, Scale);
						                                      const float X = Center.x + (Row == 0 ? 2.0f : -2.0f) * Scale;
						                                      DrawList->AddLine({X, Y - 2.0f * Scale}, {X, Y + 2.0f * Scale}, Color, 2.0f * Scale);
					                                      }
				                                      }
				                                      else
				                                      {
					                                      DrawList->AddLine({Center.x - 6.0f * Scale, Center.y - 4.0f * Scale}, {Center.x - 2.0f * Scale, Center.y}, Color, Scale);
					                                      DrawList->AddLine({Center.x - 2.0f * Scale, Center.y}, {Center.x - 6.0f * Scale, Center.y + 4.0f * Scale}, Color, Scale);
					                                      DrawList->AddLine({Center.x + Scale, Center.y + 4.0f * Scale}, {Center.x + 6.0f * Scale, Center.y + 4.0f * Scale}, Color, Scale);
				                                      }
				                                      DrawList->AddText({Minimum.x + 30.0f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, Color, Label);
				                                      ImGui::PopID();
				                                      ImGui::PopStyleColor();
				                                      return bPressed;
			                                      };
			                                      if (StatusButton("Output Log", false, Implementation->bOutputLogOpen))
				                                      Implementation->bOutputLogOpen = !Implementation->bOutputLogOpen;
			                                      ImGui::SameLine();
			                                      const ImVec2 DotPosition = ImGui::GetCursorScreenPos();
			                                      ImGui::Dummy({8.0f * Scale, 26.0f * Scale});
			                                      ImGui::SameLine(0.0f, 3.0f * Scale);
			                                      ImGui::AlignTextToFramePadding();
			                                      ImGui::TextDisabled("Ready");
			                                      const float ReadyCenterY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
			                                      ImGui::GetWindowDrawList()->AddCircleFilled({DotPosition.x + 4.0f * Scale, ReadyCenterY}, 2.5f * Scale, IM_COL32(164, 189, 148, 255));
			                                      ImGui::SameLine();
			                                      ImGui::TextDisabled("%zu objects", Implementation->PreviewObjects.size());
			                                      const float Width = ImGui::CalcTextSize("Appearance").x + 42.0f * Scale;
			                                      ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - Width - 12.0f));
			                                      if (StatusButton("Appearance", true, false))
				                                      ImGui::OpenPopup("WorkspaceAppearance");
			                                      ImGui::PopStyleColor(3);
			                                      ImGui::PopStyleVar(3);
			                                      if (ImGui::BeginPopup("WorkspaceAppearance"))
			                                      {
				                                      FEditorAppearance Appearance = Implementation->ToolUI->GetAppearance();
				                                      ImGui::TextUnformatted("Workspace appearance");
				                                      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ImGui::GetStyle().FramePadding.x, 1.0f * Scale});
				                                      ImGui::SetNextItemWidth(180.0f);
				                                      DrawNumericSliderFloat("Opacity", &Appearance.PanelOpacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				                                      ImGui::SetNextItemWidth(180.0f);
				                                      DrawNumericSliderFloat("Blur", &Appearance.BlurRadius, 0.0f, 40.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
				                                      ImGui::PopStyleVar();
				                                      ImGui::Checkbox("Reduced motion", &Appearance.bReducedMotion);
				                                      if (ImGui::Button("Reset appearance"))
					                                      Appearance = FEditorAppearance{};
				                                      Implementation->ToolUI->SetAppearance(Appearance);
				                                      ImGui::EndPopup();
			                                      }
		                                      });
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
		return Implementation->DrawOutputLog();
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorFrameworkError{Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorFrameworkError{"Editor drawing failed due to an unknown error"});
	}
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
		return;
	constexpr float Infinity = std::numeric_limits<float>::infinity();
	FVector3 Minimum{Infinity, Infinity, Infinity};
	FVector3 Maximum{-Infinity, -Infinity, -Infinity};
	for (const int Index : PreviewSelection.Indices)
	{
		const FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
		const FMatrix4 Model = ToHertaMatrix(Im3d::Mat4(Object.Translation, Object.Rotation, Object.Scale));
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			const float Extent = std::abs(Model(Axis, 0)) + std::abs(Model(Axis, 1)) + std::abs(Model(Axis, 2));
			Minimum[Axis] = std::min(Minimum[Axis], Model(Axis, 3) - Extent);
			Maximum[Axis] = std::max(Maximum[Axis], Model(Axis, 3) + Extent);
		}
	}
	const float AspectRatio = ViewportExtent.Height > 0 ? static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height) : 16.0f / 9.0f;
	ViewportCamera.Focus((Minimum + Maximum) * 0.5f, (Maximum - Minimum) * 0.5f, AspectRatio, ViewportVisibleSize);
}

void FEditorFramework::FImplementation::DrawViewportStats(const ImVec2 Minimum, const ImVec2 Size)
{
	if (!Stats->bUnitVisible && !Stats->bFpsVisible)
		return;
	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float Width = 190.0f * Scale;
	const float RowHeight = ImGui::GetTextLineHeight() + 4.0f * Scale;
	const float Height = RowHeight * ((Stats->bUnitVisible ? 3 : 0) + (Stats->bFpsVisible ? 1 : 0)) + 16.0f * Scale;
	const float Top = Minimum.y + (Size.x > 680.0f * Scale ? 50.0f : 88.0f) * Scale;
	if (Size.x < Width + 16.0f * Scale || Top + Height > Minimum.y + Size.y - 60.0f * Scale)
		return;
	const float Left = Minimum.x + Size.x - Width - 8.0f * Scale;
	ToolUI->DrawGlassSurface(Left, Top, Width, Height, 8.0f * Scale);
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	float Y = Top + 8.0f * Scale;
	const auto Row = [&](const char* Label, const std::string& Value)
	{
		Draw->AddText({Left + 12.0f * Scale, Y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Label);
		Draw->AddText({Left + Width - 12.0f * Scale - ImGui::CalcTextSize(Value.c_str()).x, Y}, IM_COL32(156, 211, 174, 255), Value.c_str());
		Y += RowHeight;
	};
	const float Fps = ImGui::GetIO().Framerate;
	if (Stats->bFpsVisible)
		Row("FPS", std::format("{:.1f}", Fps));
	if (Stats->bUnitVisible)
	{
		Row("Frame", Fps > 0.0f ? std::format("{:.2f} ms", 1000.0f / Fps) : "--");
		Row("CPU frame", std::format("{:.2f} ms", CpuFrameMilliseconds));
		Row("GPU UI", GpuUIMilliseconds ? std::format("{:.2f} ms", *GpuUIMilliseconds) : "--");
	}
}

void FEditorFramework::FImplementation::DrawViewportToolbar(const ImVec2 Minimum, const ImVec2 Size)
{
	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float Gap = 4.0f * Scale;
	const float Padding = 3.0f * Scale;
	const float ButtonSize = ViewportIconButtonSize * Scale;
	const float Height = ButtonSize + 2.0f * Padding;
	const float EdgeMargin = 8.0f * Scale;
	const float Top = Minimum.y + EdgeMargin;
	bViewportControlsHovered = false;
	const bool bReducedMotion = ToolUI->GetAppearance().bReducedMotion;
	const float AnimationStep = bReducedMotion ? 1.0f : std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
	const auto IconButton = [&](const char* Id, const EViewportIcon Icon, const char* Tooltip, const bool bSelected = false)
	{
		return ViewportIconButton(Id, Icon, Tooltip, Scale, bSelected, bReducedMotion);
	};
	const auto Island = [&](const float X, const float Width, const float OffsetY = 0.0f)
	{
		ToolUI->DrawGlassSurface(X, Top + OffsetY, Width, Height, Height * 0.5f);
		bViewportControlsHovered |= ImGui::IsMouseHoveringRect({X, Top + OffsetY}, {X + Width, Top + OffsetY + Height});
		ImGui::SetCursorScreenPos({X + Padding, Top + OffsetY + Padding});
	};
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * Scale, 3.0f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1.0f * Scale, 3.0f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ButtonSize * 0.5f);
	ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
	ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
	if (Size.x > 680.0f * Scale)
	{
		const float ProjectionWidth = ImGui::CalcTextSize("Perspective").x + 10.0f * Scale;
		Island(Minimum.x + EdgeMargin, ProjectionWidth + 2.0f * Padding);
		if (ImGui::Button("Perspective", {ProjectionWidth, ButtonSize}))
			ImGui::OpenPopup("Projection");
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f * Scale, 10.0f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * Scale, 6.0f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f * Scale, 4.0f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * Scale);
		ImGui::SetNextWindowSize({320.0f * Scale, 0.0f}, ImGuiCond_Appearing);
		if (ImGui::BeginPopup("Projection"))
		{
			ImGui::MenuItem("Perspective", nullptr, true);
			ImGui::Spacing();
			ImGui::TextDisabled("Orthographic views are\nnot available yet.");
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(4);
	}
	if (Size.x > 180.0f * Scale)
	{
		const float PlayWidth = 2.0f * ButtonSize + ImGui::GetStyle().ItemSpacing.x + 2.0f * Padding;
		Island(Minimum.x + (Size.x - PlayWidth) * 0.5f, PlayWidth, Size.x > 680.0f * Scale ? 0.0f : Height + Gap);
		ImGui::BeginDisabled();
		IconButton("Play", EViewportIcon::Play, "Play - game runtime is not implemented yet.");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (IconButton("Simulate", Simulation.IsRunning() ? EViewportIcon::Stop : EViewportIcon::Simulate, Simulation.IsRunning() ? "Stop simulation (Esc)" : "Simulate (Alt+S)", Simulation.IsRunning()))
			ToggleSimulation();
	}
	const float SettingsX = std::max(Minimum.x + 4.0f * Scale, Minimum.x + Size.x - EdgeMargin - Height);
	Island(SettingsX, Height);
	if (IconButton("Viewport settings", EViewportIcon::Settings, "Viewport settings"))
		ImGui::OpenPopup("ViewportSettings");
	if (Size.x > 340.0f * Scale)
	{
		float Right = SettingsX - Gap;
		const bool bExpandedLayout = Size.x > 1040.0f * Scale;
		if (bExpandedLayout)
		{
			Island(Right - Height, Height);
			if (IconButton("Focus", EViewportIcon::Focus, "Focus selected object (F)"))
				FocusPreview();
			Right -= Height + Gap;
		}
		const float SnapTarget = bSnapEnabled ? ViewportIconButtonSize + 75.0f : Height / Scale;
		SnapIslandWidth += (SnapTarget - SnapIslandWidth) * AnimationStep;
		const float SnapWidth = SnapIslandWidth * Scale;
		Island(Right - SnapWidth, SnapWidth);
		if (IconButton("Grid snap", EViewportIcon::Grid, "Toggle grid snapping (S)", bSnapEnabled))
			bSnapEnabled = !bSnapEnabled;
		if (bSnapEnabled && SnapIslandWidth > SnapTarget - 1.0f)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(68.0f * Scale);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * Scale, (ButtonSize - ImGui::GetFontSize()) * 0.5f});
			DrawNumericDragFloat("##GridStep", &TranslationSnap, 0.05f, 0.001f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
			ImGui::PopStyleVar();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Translation snap step in meters");
		}
		TranslationSnap = std::isfinite(TranslationSnap) ? std::clamp(TranslationSnap, 0.001f, 100.0f) : 0.5f;
		Right -= SnapWidth + Gap;
		if (bExpandedLayout)
		{
			const bool bHover = ImGui::IsMouseHoveringRect({Right - WorldIslandWidth * Scale, Top}, {Right, Top + Height});
			const float Target = bHover ? 100.0f : Height / Scale;
			WorldIslandWidth += (Target - WorldIslandWidth) * AnimationStep;
			Island(Right - WorldIslandWidth * Scale, WorldIslandWidth * Scale);
			if (IconButton("Coordinate space", EViewportIcon::World, bLocalGizmo ? "Local axes (L)" : "World axes (L)", bLocalGizmo))
			{
				bLocalGizmo = !bLocalGizmo;
				ViewportGizmos.resetId();
			}
			if (WorldIslandWidth > 90.0f)
			{
				ImGui::SameLine();
				if (ImGui::Button(bLocalGizmo ? "Local##CoordinateSpaceLabel" : "World##CoordinateSpaceLabel", {WorldIslandWidth * Scale - ButtonSize - ImGui::GetStyle().ItemSpacing.x - 2.0f * Padding, ButtonSize}))
				{
					bLocalGizmo = !bLocalGizmo;
					ViewportGizmos.resetId();
				}
			}
			Right -= WorldIslandWidth * Scale + Gap;
		}
		const float ModesWidth = 4.0f * ButtonSize + 3.0f * ImGui::GetStyle().ItemSpacing.x + 2.0f * Padding;
		Island(std::max(Minimum.x + 4.0f * Scale, Right - ModesWidth), ModesWidth);
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
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f * Scale, 8.0f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * Scale, 4.0f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7.0f * Scale, 3.0f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * Scale);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * Scale);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.16f, 0.16f, 0.16f, 0.82f});
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.22f, 0.22f, 0.22f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.25f, 0.25f, 0.25f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, {0.64f, 0.64f, 0.64f, 1.0f});
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0.82f, 0.82f, 0.82f, 1.0f});
	ImGui::PushStyleColor(ImGuiCol_Button, {0.19f, 0.19f, 0.19f, 0.78f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.26f, 0.26f, 0.26f, 0.95f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.31f, 0.31f, 0.31f, 0.95f});
	ImGui::SetNextWindowPos({SettingsX + Height, Top + Height + 6.0f * Scale}, ImGuiCond_Always, {1.0f, 0.0f});
	ImGui::SetNextWindowSize({288.0f * Scale, 0.0f}, ImGuiCond_Always);
	if (ImGui::BeginPopup("ViewportSettings"))
	{
		constexpr ImGuiTreeNodeFlags SectionFlags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_Framed;
		ImGui::PushStyleColor(ImGuiCol_Separator, {1.0f, 1.0f, 1.0f, 0.12f});
		ImGui::PushStyleColor(ImGuiCol_Header, {0.0f, 0.0f, 0.0f, 0.0f});
		const float ValueWidth = 108.0f * Scale;
		const auto FieldLabel = [&](const char* const Label)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Label);
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ValueWidth);
			ImGui::SetNextItemWidth(ValueWidth);
		};
		ImGui::TextUnformatted("Transform");
		const float ModeWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		const auto ModeButton = [&](const char* const Label, const bool bSelected)
		{
			if (bSelected)
				ImGui::PushStyleColor(ImGuiCol_Button, {0.30f, 0.30f, 0.30f, 0.93f});
			const bool bPressed = ImGui::Button(Label, {ModeWidth, 0.0f});
			if (bSelected)
				ImGui::PopStyleColor();
			return bPressed;
		};
		if (ModeButton("Select (Q)", !bTransformGizmoVisible))
		{
			bTransformGizmoVisible = false;
			ViewportGizmos.resetId();
		}
		ImGui::SameLine();
		constexpr std::array ModeNames{"Translate (W)", "Rotate (E)", "Scale (R)"};
		constexpr std::array ModeValues{Im3d::GizmoMode_Translation, Im3d::GizmoMode_Rotation, Im3d::GizmoMode_Scale};
		for (std::size_t Index = 0; Index < ModeValues.size(); ++Index)
		{
			if (ModeButton(ModeNames[Index], bTransformGizmoVisible && GizmoMode == ModeValues[Index]))
			{
				bTransformGizmoVisible = true;
				GizmoMode = ModeValues[Index];
				ViewportGizmos.resetId();
			}
			if (Index == 1)
				ImGui::SameLine();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * Scale, 2.0f * Scale});
		if (ImGui::Checkbox("Local axes (L)", &bLocalGizmo))
			ViewportGizmos.resetId();
		ImGui::PopStyleVar();
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 88.0f * Scale);
		if (ImGui::Button("Focus (F)", {88.0f * Scale, 0.0f}))
			FocusPreview();
		if (ImGui::Checkbox("Flip gizmo axes toward camera", &bFlipGizmoAxesTowardCamera))
			ViewportGizmos.resetId();
		ImGui::Spacing();
		ImGui::Separator();
		if (ImGui::TreeNodeEx("Snapping", SectionFlags))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * Scale, 2.0f * Scale});
			ImGui::Checkbox("Enable snapping", &bSnapEnabled);
			ImGui::PopStyleVar();
			FieldLabel("Translation (m)");
			DrawNumericDragFloat("##SnapTranslation", &TranslationSnap, 0.05f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			TranslationSnap = std::isfinite(TranslationSnap) ? std::clamp(TranslationSnap, 0.001f, 100.0f) : 0.5f;
			FieldLabel("Rotation (deg)");
			DrawNumericDragFloat("##SnapRotation", &RotationSnapDegrees, 1.0f, 0.1f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			FieldLabel("Scale");
			DrawNumericDragFloat("##SnapScale", &ScaleSnap, 0.01f, 0.001f, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			RotationSnapDegrees = std::isfinite(RotationSnapDegrees) ? std::clamp(RotationSnapDegrees, 0.1f, 180.0f) : 15.0f;
			ScaleSnap = std::isfinite(ScaleSnap) ? std::clamp(ScaleSnap, 0.001f, 10.0f) : 0.1f;
		}
		ImGui::Separator();
		if (ImGui::TreeNodeEx("Camera", SectionFlags))
		{
			float MovementSpeed = ViewportCamera.GetMovementSpeed();
			FieldLabel("Speed (m/s)");
			if (DrawNumericSliderFloat("##CameraSpeed", &MovementSpeed, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
				ViewportCamera.SetMovementSpeed(MovementSpeed);
			float Sensitivity = ViewportCamera.GetMouseSensitivity() * 180.0f / std::numbers::pi_v<float>;
			FieldLabel("Look (deg/pixel)");
			if (DrawNumericSliderFloat("##CameraLook", &Sensitivity, 0.02f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
				ViewportCamera.SetMouseSensitivity(Sensitivity * std::numbers::pi_v<float> / 180.0f);
		}
		ImGui::Separator();
		if (ImGui::TreeNodeEx("Overlays", SectionFlags))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f * Scale, 2.0f * Scale});
			ImGui::Checkbox("Grid", &bGridVisible);
			ImGui::Checkbox("World axes", &bAxesVisible);
			ImGui::Checkbox("Corner axis indicator", &bOrientationIndicatorVisible);
			ImGui::Checkbox("Preview bounds", &bBoundsVisible);
			ImGui::PopStyleVar();
		}
		ImGui::Separator();
		if (ImGui::TreeNodeEx("Navigation", SectionFlags))
			ImGui::TextDisabled("RMB + WASD/QE  Fly\nAlt + LMB  Orbit   |   MMB  Pan\nWheel  Dolly   |   RMB + wheel  Speed");
		ImGui::Separator();
		ImGui::BeginDisabled(Simulation.IsRunning() || PreviewSelection.Active < 0);
		if (ImGui::Button("Reset preview transform", {-1.0f, 0.0f}))
		{
			const auto Defaults = CreatePreviewObjects();
			for (const int Index : PreviewSelection.Indices)
			{
				FPreviewObject& Object = PreviewObjects[static_cast<std::size_t>(Index)];
				Object.Translation = Defaults[static_cast<std::size_t>(Index)].Translation;
				Object.Rotation = Defaults[static_cast<std::size_t>(Index)].Rotation;
				Object.Scale = Defaults[static_cast<std::size_t>(Index)].Scale;
			}
		}
		ImGui::EndDisabled();
		ImGui::PopStyleColor(2);
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor(8);
	ImGui::PopStyleVar(5);
	ImGui::EndDisabled();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(4);
}

void FEditorFramework::FImplementation::UpdateViewport(const ImVec2 RenderMinimum, const ImVec2 RenderSize)
{
	auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale] = GetActivePreviewObject();
	const ImGuiIO& IO = ImGui::GetIO();
	const bool bImageHovered = ImGui::IsItemHovered();
	const bool bImageActive = ImGui::IsItemActive();
	const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	const bool bDeselectPressed = ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteFocused);
	if (bEscapePressed && PreviewDragStart)
	{
		PreviewObjects = PreviewDragObjects;
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
	for (int Button = 0; Button < 3; ++Button)
	{
		InteractionInput.MouseClicked[static_cast<std::size_t>(Button)] = ImGui::IsMouseClicked(Button);
		InteractionInput.MouseDown[static_cast<std::size_t>(Button)] = ImGui::IsMouseDown(Button);
	}
	ViewportInteraction.Update(InteractionInput);
	if (ViewportInteraction.DragButton == ImGuiMouseButton_Right)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
	}
	if (!Simulation.IsRunning() && PreviewSelection.Active >= 0 && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ViewportInteraction.CameraMode == EViewportCameraMode::None)
	{
		if (!PreviewDragStart)
		{
			PreviewDragStart.emplace(PreviewTranslation, PreviewRotation, PreviewScale);
			PreviewDragObjects = PreviewObjects;
		}
	}
	else
	{
		PreviewDragStart.reset();
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
	if (bInputAllowed && (bImageHovered || ViewportInteraction.DragButton >= 0) && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY))
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
			bSnapEnabled = !bSnapEnabled;
		if (ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused))
			RequestPreviewRename();
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
	ViewportRenderView.bDrawGrid = bGridVisible;
	ViewportRenderView.GridCenter = Camera.Position;
	const ImVec2 Mouse = ImGui::GetMousePos();
	const FVector2 NormalizedMouse{(Mouse.x - RenderMinimum.x) / RenderSize.x, (Mouse.y - RenderMinimum.y) / RenderSize.y};
	const bool bGizmoInput = ViewportInteraction.CanUseGizmo(InteractionInput);
	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float GizmoPixelScale = GetViewportGizmoPixelScale(InterfaceScale, RenderSize.y, static_cast<float>(ViewportExtent.Height));
	ViewportGizmos.m_gizmoHeightPixels = 100.0f * GizmoPixelScale;
	ViewportGizmos.m_gizmoSizePixels = 4.0f * GizmoPixelScale;
	BuildViewportDebugDraw(bGizmoInput, NormalizedMouse);
}

void FEditorFramework::FImplementation::BuildViewportDebugDraw(const bool bGizmoInput, const FVector2 NormalizedMouse)
{
	auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale] = GetActivePreviewObject();
	const FIm3dContextScope ContextScope(ViewportGizmos);
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
	AppData.m_projScaleY = 2.0f / Camera.Projection(1, 1);
	AppData.m_flipGizmoWhenBehind = bFlipGizmoAxesTowardCamera;
	AppData.m_keyDown[Im3d::Mouse_Left] = bGizmoInput && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ImGui::IsMouseDown(ImGuiMouseButton_Left);
	AppData.m_snapTranslation = bSnapEnabled ? TranslationSnap : 0.0f;
	AppData.m_snapRotation = bSnapEnabled ? RotationSnapDegrees * std::numbers::pi_v<float> / 180.0f : 0.0f;
	AppData.m_snapScale = bSnapEnabled ? ScaleSnap : 0.0f;
	ViewportGizmos.m_gizmoMode = static_cast<Im3d::GizmoMode>(GizmoMode);
	ViewportGizmos.m_gizmoLocal = bLocalGizmo;
	if (!bGizmoInput)
	{
		ViewportGizmos.resetId();
	}
	Im3d::NewFrame();
	ScaleFeedback = {};
	if (!bGizmoInput || PreviewSelection.Active < 0 || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Scale)
	{
		ScaleGizmoState.Reset();
	}
	if (!bGizmoInput || PreviewSelection.Active < 0 || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Rotation)
	{
		RotationFeedback.Reset();
	}
	Im3d::PushLayerId("ViewportGizmos");
	const Im3d::Vec3 PreviousTranslation = PreviewTranslation;
	const Im3d::Mat3 PreviousRotation = PreviewRotation;
	const Im3d::Vec3 PreviousScale = PreviewScale;
	const FPreviewObject PreviousObject = GetActivePreviewObject();
	if (!Simulation.IsRunning() && PreviewSelection.Active >= 0 && bTransformGizmoVisible)
	{
		if (GizmoMode == Im3d::GizmoMode_Translation)
		{
			DrawPreviewTranslationGizmo(PreviewTranslation, PreviewRotation, bLocalGizmo);
		}
		else if (GizmoMode == Im3d::GizmoMode_Rotation)
		{
			DrawPreviewRotationGizmo(PreviewTranslation, PreviewRotation, bLocalGizmo, RotationFeedback);
		}
		else
		{
			DrawPreviewScaleGizmo(PreviewTranslation, PreviewRotation, PreviewScale, ScaleGizmoState, ScaleFeedback);
		}
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
	PreviewScale = {std::clamp(PreviewScale.x, 0.001f, 1000.0f), std::clamp(PreviewScale.y, 0.001f, 1000.0f), std::clamp(PreviewScale.z, 0.001f, 1000.0f)};
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
			const auto Distance = HitTestPreviewCube(CursorRay, PreviewModels[Index]);
			if (Distance && *Distance < ClosestDistance)
			{
				ClosestObject = static_cast<int>(Index);
				ClosestDistance = *Distance;
			}
		}
		if (ClosestObject >= 0 || (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift))
			SetPreviewSelection(ClosestObject, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
	}
	Im3d::PushLayerId("ViewportWorld");
	if (bAxesVisible)
	{
		constexpr float AxisLength = 0.6f;
		Im3d::DrawLine({0.0f}, {AxisLength, 0.0f, 0.0f}, 2.0f, Im3d::Color_Red);
		Im3d::DrawLine({0.0f}, {0.0f, AxisLength, 0.0f}, 2.0f, Im3d::Color_Green);
		Im3d::DrawLine({0.0f}, {0.0f, 0.0f, AxisLength}, 2.0f, Im3d::Color_Blue);
	}
	if (bBoundsVisible)
	{
		Im3d::PushMatrix(Model);
		Im3d::PushColor(Im3d::Color(0xefd07ccc));
		Im3d::DrawAlignedBox(Im3d::Vec3(-1.0f), Im3d::Vec3(1.0f));
		Im3d::PopColor();
		Im3d::PopMatrix();
	}
	Im3d::PopLayerId();
	if (PreviewSelection.Active >= 0)
	{
		Im3d::PushLayerId("ViewportSelection");
		for (const int Index : PreviewSelection.Indices)
		{
			for (const auto& [Start, End] : GetPreviewCubeSilhouette(Camera.Position, PreviewModels[static_cast<std::size_t>(Index)]))
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
			ViewportDebugVertices[VertexOffset + Index] = {{Vertex.m_positionSize.x, Vertex.m_positionSize.y, Vertex.m_positionSize.z}, Vertex.m_positionSize.w, ResolveViewportDebugColor(Vertex.m_color)};
		}
		const EDebugPrimitive Primitive = List.m_primType == Im3d::DrawPrimitive_Triangles ? EDebugPrimitive::Triangles : List.m_primType == Im3d::DrawPrimitive_Points ? EDebugPrimitive::Points
		                                                                                                                                                                : EDebugPrimitive::Lines;
		ViewportDebugDrawLists.push_back({Primitive, std::span<const FDebugDrawVertex>{ViewportDebugVertices}.subspan(VertexOffset, List.m_vertexCount), List.m_layerId != Im3d::MakeId("ViewportGizmos") && List.m_layerId != Im3d::MakeId("ViewportSelection")});
		VertexOffset += List.m_vertexCount;
	}
}

void FEditorFramework::FImplementation::DrawViewport(const std::function<void()>& RenderViewport)
{
	auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale] = GetActivePreviewObject();
	ImGui::SetNextWindowSize({960, 540}, ImGuiCond_FirstUseEver);
	if (ToolUI->BeginPanel("Viewport", nullptr, true))
	{
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		const ImVec2 ImageMinimum = ImGui::GetCursorScreenPos();
		const std::optional<FToolUICanvasBounds> WorkspaceCanvas = ToolUI->GetWorkspaceCanvasForCurrentPanel();
		const ImVec2 RenderMinimum = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->X, WorkspaceCanvas->Y} : ImageMinimum;
		const ImVec2 RenderSize = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->Width, WorkspaceCanvas->Height} : Size;
		const float Scale = ImGui::GetWindowViewport()->DpiScale;
		const float RenderScale = std::min(Scale, 4096.0f / std::max({RenderSize.x, RenderSize.y, 1.0f}));
		ViewportExtent = {static_cast<std::uint32_t>(std::max(RenderSize.x * RenderScale, 1.0f)), static_cast<std::uint32_t>(std::max(RenderSize.y * RenderScale, 1.0f))};
		if (Size.x > 0 && Size.y > 0 && RenderSize.x > 0 && RenderSize.y > 0)
		{
			ViewportProjectionCenter = GetViewportProjectionCenter({RenderMinimum.x, RenderMinimum.y}, {RenderSize.x, RenderSize.y}, {ImageMinimum.x, ImageMinimum.y}, {Size.x, Size.y});
			ViewportVisibleSize = {std::clamp(Size.x / RenderSize.x, 0.001f, 1.0f), std::clamp(Size.y / RenderSize.y, 0.001f, 1.0f)};
			ImDrawList* const PanelDrawList = ImGui::GetWindowDrawList();
			ImDrawListSplitter Layers;
			Layers.Split(PanelDrawList, 2);
			Layers.SetCurrentChannel(PanelDrawList, 1);
			DrawViewportToolbar(ImageMinimum, Size);
			DrawViewportStats(ImageMinimum, Size);
			const float HudScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
			const FVector3 LayoutCameraPosition = ViewportCamera.GetSnapshot(1.0f).Position;
			const std::string LayoutCoordinates = std::format("X {:.2f}   Y {:.2f}   Z {:.2f} m", LayoutCameraPosition.X, LayoutCameraPosition.Y, LayoutCameraPosition.Z);
			const float CameraLabelWidth = std::max(ImGui::CalcTextSize("Camera").x, ImGui::CalcTextSize("Copied").x);
			const float CoordinatesWidth = CameraLabelWidth + ImGui::CalcTextSize(LayoutCoordinates.c_str()).x + 52.0f * HudScale;
			const float CoordinatesHeight = 32.0f * HudScale;
			const ImVec2 CoordinatesPosition{ImageMinimum.x + std::max(0.0f, Size.x - CoordinatesWidth - 14.0f * HudScale), ImageMinimum.y + std::max(0.0f, Size.y - CoordinatesHeight - 14.0f * HudScale)};
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
			(void)ImGui::InvisibleButton("##ViewportInteraction", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			ImGui::EndDisabled();
			UpdateViewport(RenderMinimum, RenderSize);
			const FVector3 CameraPosition = ViewportCamera.GetSnapshot(1.0f).Position;
			const std::string Coordinates = std::format("X {:.2f}   Y {:.2f}   Z {:.2f} m", CameraPosition.X, CameraPosition.Y, CameraPosition.Z);
			if (bCopyCoordinates)
			{
				ImGui::SetClipboardText(FormatTransformVectorClipboard(CameraPosition).c_str());
				CameraCoordinatesCopiedUntil = ImGui::GetTime() + 1.5;
			}
			// Render after layout and input, before recording the texture ID that resize may replace.
			RenderViewport();
			Layers.SetCurrentChannel(PanelDrawList, 0);
			if (ViewportTexture != 0 && WorkspaceCanvas)
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
			Layers.Merge(PanelDrawList);
			if (PreviewDragStart && ViewportGizmos.m_activeId != Im3d::Id_Invalid)
			{
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
				const FVector4 ClipStart = WorldToClip * FVector4{Start, 1.0f};
				const FVector4 ClipEnd = WorldToClip * FVector4{End, 1.0f};
				if (ClipStart.W > 0.001f && ClipEnd.W > 0.001f)
				{
					const FVector2 ScreenStart{(ClipStart.X / ClipStart.W + 1.0f) * RenderSize.x * 0.5f, (1.0f - ClipStart.Y / ClipStart.W) * RenderSize.y * 0.5f};
					const FVector2 ScreenEnd = RotationFeedback.AngleDegrees ? FVector2{ImGui::GetMousePos().x - RenderMinimum.x, ImGui::GetMousePos().y - RenderMinimum.y} : FVector2{(ClipEnd.X / ClipEnd.W + 1.0f) * RenderSize.x * 0.5f, (1.0f - ClipEnd.Y / ClipEnd.W) * RenderSize.y * 0.5f};
					const FVector2 Delta = ScreenEnd - ScreenStart;
					const FVector2 PanelMinimum{ImageMinimum.x - RenderMinimum.x, ImageMinimum.y - RenderMinimum.y};
					const FVector2 PanelMaximum{PanelMinimum.X + Size.x, PanelMinimum.Y + Size.y};
					float First = 0.0f;
					float Last = 1.0f;
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
							Last = 0.0f;
						}
					}
					const float Distance = Delta.Length();
					const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
					ImDrawList* const Trail = ImGui::GetWindowDrawList();
					Trail->PushClipRect(ImageMinimum, {ImageMinimum.x + Size.x, ImageMinimum.y + Size.y}, true);
					constexpr ImU32 TrailColor = IM_COL32(194, 181, 132, 255);
					if (Distance > 0.001f && std::isfinite(Distance))
					{
						const int DashCount = static_cast<int>(std::clamp(std::ceil((Last - First) * Distance / (7.0f * UiScale)), 0.0f, 4096.0f));
						for (int Dash = 0; Dash < DashCount; ++Dash)
						{
							const float Offset = First * Distance + static_cast<float>(Dash) * 7.0f * UiScale;
							const FVector2 A = ScreenStart + Delta * (Offset / Distance);
							const FVector2 B = ScreenStart + Delta * (std::min(Offset + 3.0f * UiScale, Last * Distance) / Distance);
							Trail->AddLine({RenderMinimum.x + A.X, RenderMinimum.y + A.Y}, {RenderMinimum.x + B.X, RenderMinimum.y + B.Y}, TrailColor, 1.5f * UiScale);
						}
						Trail->AddCircleFilled({RenderMinimum.x + ScreenStart.X, RenderMinimum.y + ScreenStart.Y}, 3.0f * UiScale, TrailColor);
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
			const ImVec2 CameraIconCenter{CoordinatesPosition.x + 18.0f * HudScale, CoordinatesPosition.y + CoordinatesHeight * 0.5f};
			const ImU32 CameraIconColor = PackColor(ToolUITheme::TextSecondary);
			HudDraw->AddRect({CameraIconCenter.x - 6.0f * HudScale, CameraIconCenter.y - 4.0f * HudScale}, {CameraIconCenter.x + 6.0f * HudScale, CameraIconCenter.y + 4.0f * HudScale}, CameraIconColor, 2.0f * HudScale, 0, HudScale);
			HudDraw->AddRectFilled({CameraIconCenter.x - 3.0f * HudScale, CameraIconCenter.y - 6.0f * HudScale}, {CameraIconCenter.x + HudScale, CameraIconCenter.y - 4.0f * HudScale}, CameraIconColor, HudScale);
			HudDraw->AddCircle(CameraIconCenter, 2.0f * HudScale, CameraIconColor, 12, HudScale);
			HudDraw->AddText({CoordinatesPosition.x + 32.0f * HudScale, CoordinatesTextY}, CameraIconColor, ImGui::GetTime() < CameraCoordinatesCopiedUntil ? "Copied" : "Camera");
			HudDraw->AddText({CoordinatesPosition.x + 40.0f * HudScale + CameraLabelWidth, CoordinatesTextY}, PackColor(ToolUITheme::TextPrimary), Coordinates.c_str());
			if (ViewportInteraction.CameraMode == EViewportCameraMode::Fly)
			{
				const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
				const std::string Speed = std::format("{:.2f} m/s", ViewportCamera.GetMovementSpeed() * (ImGui::GetIO().KeyShift ? 4.0f : 1.0f));
				const float LabelWidth = ImGui::CalcTextSize("Speed").x;
				const float Width = LabelWidth + ImGui::CalcTextSize(Speed.c_str()).x + 52.0f * UiScale;
				const float Height = 32.0f * UiScale;
				const ImVec2 Position{ImageMinimum.x + std::max(0.0f, Size.x - Width - 14.0f * UiScale), CoordinatesPosition.y - Height - 6.0f * UiScale};
				ToolUI->DrawGlassSurface(Position.x, Position.y, Width, Height, Height * 0.5f);
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				const ImVec2 Center{Position.x + 18.0f * UiScale, Position.y + Height * 0.5f};
				const ImU32 Muted = PackColor(ToolUITheme::TextSecondary);
				Draw->PathArcTo(Center, 6.0f * UiScale, std::numbers::pi_v<float> * 0.75f, std::numbers::pi_v<float> * 2.25f, 16);
				Draw->PathStroke(Muted, 0, UiScale);
				Draw->AddLine(Center, {Center.x + 3.0f * UiScale, Center.y - 3.0f * UiScale}, Muted, UiScale);
				const float TextY = Center.y - ImGui::GetFontSize() * 0.5f;
				Draw->AddText({Position.x + 32.0f * UiScale, TextY}, Muted, "Speed");
				Draw->AddText({Position.x + 40.0f * UiScale + LabelWidth, TextY}, PackColor(ToolUITheme::TextPrimary), Speed.c_str());
			}
			if (bOrientationIndicatorVisible)
			{
				DrawViewportAxes(ViewportRenderView.View, ImageMinimum, Size, ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize);
			}
		}
		else
		{
			ViewportInteraction.Cancel();
			PreviewDragStart.reset();
			ViewportGizmos.resetId();
			ViewportDebugDrawLists.clear();
		}
	}
	else
	{
		ViewportExtent = {};
		ViewportInteraction.Cancel();
		PreviewDragStart.reset();
		ViewportGizmos.resetId();
		ViewportDebugDrawLists.clear();
	}
	ToolUI->EndPanel();
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
		return;
	if (DetailsPanelState.bRenaming && PreviewSelection.Active >= 0)
		(void)RenamePreviewObject(GetActivePreviewObject().Label, DetailsPanelState.RenameBuffer.data());
	PreviewSelection = std::move(Selection);
	DetailsPanelState.bRenaming = false;
	DetailsPanelState.bRenameRequested = false;
	PreviewDragStart.reset();
	RotationFeedback = {};
	ScaleGizmoState = {};
	ScaleFeedback = {};
	ViewportGizmos.resetId();
}

void FEditorFramework::FImplementation::RequestPreviewRename()
{
	if (PreviewSelection.Active < 0 || Simulation.IsRunning() || ViewportInteraction.DragButton >= 0)
		return;
	bDetailsOpen = true;
	DetailsPanelState.bRenameRequested = true;
	ImGui::SetWindowFocus("Details");
}

void FEditorFramework::FImplementation::ToggleSimulation()
{
	if (Simulation.IsRunning())
	{
		Simulation.Stop();
		PreviewObjects[PreviewCubeIndex] = *SimulationStart;
		SimulationStart.reset();
		bSimulationStoppedThisFrame = true;
	}
	else
	{
		if (const auto Result = Simulation.Start(ToHertaTransform(PreviewObjects[PreviewCubeIndex]), ToHertaTransform(PreviewObjects[PreviewFloorIndex])); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not start simulation: {}", Result.error().Message);
			return;
		}
		SimulationStart = PreviewObjects[PreviewCubeIndex];
		UpdateSimulation(0.0f);
	}
	PreviewDragStart.reset();
	ViewportGizmos.resetId();
}

void FEditorFramework::FImplementation::UpdateSimulation(const float DeltaSeconds)
{
	if (!Simulation.IsRunning())
		return;
	if (const auto Result = Simulation.Update(DeltaSeconds); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Simulation stopped: {}", Result.error().Message);
		ToggleSimulation();
		return;
	}
	const FTransform& Transform = Simulation.GetTransform();
	FPreviewObject& CubeObject = PreviewObjects[PreviewCubeIndex];
	CubeObject.Translation = ToIm3dVector(Transform.Translation);
	const FMatrix3 Rotation = FMatrix3::Rotation(Transform.Rotation);
	for (std::size_t Column = 0; Column < 3; ++Column)
	{
		for (std::size_t Row = 0; Row < 3; ++Row)
			CubeObject.Rotation(static_cast<int>(Row), static_cast<int>(Column)) = Rotation(Row, Column);
	}
}

void FEditorFramework::FImplementation::DrawDetailsPanel()
{
	const FPreviewObject PreviousObject = GetActivePreviewObject();
	auto& [Label, Translation, Rotation, Scale] = GetActivePreviewObject();
	DrawPreviewDetailsPanel(*ToolUI, bDetailsOpen, PreviewSelection.Active >= 0, Simulation.IsRunning() || ViewportInteraction.DragButton >= 0, Translation, Rotation, Scale, DetailsPanelState, Label, PreviewSelection.Indices.size());
	ApplyPreviewTransformDelta(PreviewObjects, PreviewSelection, PreviousObject);
}

void FEditorFramework::FImplementation::DrawOutlinerPanel()
{
	FPreviewSelection NewSelection = PreviewSelection;
	const bool bFocusRequested = DrawPreviewOutlinerPanel(*ToolUI, bOutlinerOpen, NewSelection, PreviewObjects, ViewportInteraction.DragButton >= 0, OutlinerPanelState);
	SetPreviewSelection(std::move(NewSelection));
	if (OutlinerPanelState.bRenameRequested)
		RequestPreviewRename();
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
	ImGui::Spacing();
	ImGui::SeparatorText("Workspace appearance");

	FEditorAppearance Appearance = ToolUI->GetAppearance();
	const char* const PanelMode = GetPanelTransparencyLabel(Appearance.PanelTransparency);
	if (ImGui::BeginCombo("Panel transparency", PanelMode))
	{
		constexpr std::array Modes = {
		    std::pair{EPanelTransparency::AllPanels, "All panels"},
		    std::pair{EPanelTransparency::FloatingOnly, "Floating panels"},
		    std::pair{EPanelTransparency::DockedOnly, "Docked panels"},
		    std::pair{EPanelTransparency::Disabled, "Opaque panels"}};
		for (const auto& [Mode, Label] : Modes)
		{
			if (ImGui::Selectable(Label, Appearance.PanelTransparency == Mode))
			{
				Appearance.PanelTransparency = Mode;
			}
		}
		ImGui::EndCombo();
	}

	int GradientHeightPercent = static_cast<int>(std::lround(Appearance.GradientHeight * 100.0f));
	int SaturationPercent = static_cast<int>(std::lround(Appearance.Saturation * 100.0f));
	int IntensityPercent = static_cast<int>(std::lround(Appearance.Intensity * 100.0f));
	if (ImGui::SliderInt("Gradient height", &GradientHeightPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.GradientHeight = static_cast<float>(GradientHeightPercent) / 100.0f;
	}
	if (ImGui::SliderInt("Color saturation", &SaturationPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.Saturation = static_cast<float>(SaturationPercent) / 100.0f;
	}
	if (ImGui::SliderInt("Color intensity", &IntensityPercent, 0, 100, "%d%%", ImGuiSliderFlags_ClampOnInput))
	{
		Appearance.Intensity = static_cast<float>(IntensityPercent) / 100.0f;
	}
	std::array Color = {
	    static_cast<float>(Appearance.Accent.Red) / 255.0f,
	    static_cast<float>(Appearance.Accent.Green) / 255.0f,
	    static_cast<float>(Appearance.Accent.Blue) / 255.0f};
	if (ImGui::ColorEdit3("Color", Color.data()))
	{
		Appearance.Accent.Red = static_cast<std::uint8_t>(std::lround(Color[0] * 255.0f));
		Appearance.Accent.Green = static_cast<std::uint8_t>(std::lround(Color[1] * 255.0f));
		Appearance.Accent.Blue = static_cast<std::uint8_t>(std::lround(Color[2] * 255.0f));
	}
	for (std::size_t Index = 0; Index < ToolUITheme::Presets.size(); ++Index)
	{
		if (Index > 0)
		{
			ImGui::SameLine();
		}
		const FToolUIColorPreset& Preset = ToolUITheme::Presets[Index];
		ImGui::PushID(static_cast<int>(Index));
		if (ImGui::ColorButton("##Preset", ImGui::ColorConvertU32ToFloat4(PackColor(Preset.Color)), ImGuiColorEditFlags_NoTooltip, {22.0f, 22.0f}))
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
	ToolUI->SetAppearance(Appearance);
	ToolUI->EndPanel();
}

void FEditorFramework::FImplementation::RebuildSuggestions()
{
	std::expected<std::vector<std::string>, FOutputLogError> Completion = OutputLog->CompleteCommand(CommandBuffer.data());
	Suggestions = Completion ? std::move(*Completion) : std::vector<std::string>{};
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
	std::expected<bool, FOutputLogError> SynchronizeResult = OutputLog->Synchronize();
	if (!SynchronizeResult)
	{
		return std::unexpected(FEditorFrameworkError{std::move(SynchronizeResult.error().Message)});
	}
	const bool bReceivedRecords = *SynchronizeResult;
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
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f * ToolbarScale, 5.0f * ToolbarScale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * ToolbarScale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * ToolbarScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;
	const auto ButtonWidth = [](const char* const Label)
	{
		return ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	};
	const float ClearWidth = ButtonWidth("Clear");
	const float CopyWidth = ButtonWidth("Copy");
	const auto VisibleLines = OutputLog->GetVisibleLines();
	std::size_t WarningCount = 0;
	std::optional<std::uint64_t> PreviousSequence;
	for (const FOutputLogLine& Line : VisibleLines)
	{
		if (Line.Record.Sequence != PreviousSequence && Line.Record.Level == ELogLevel::Warning)
			++WarningCount;
		PreviousSequence = Line.Record.Sequence;
	}
	const std::string CountLabel = std::to_string(WarningCount);
	const float CountFontSize = ImGui::GetFontSize() * 0.8f;
	const ImVec2 CountTextSize = ImGui::CalcTextSize(CountLabel.c_str());
	const ImVec2 CountSize{CountTextSize.x * 0.8f, CountTextSize.y * 0.8f};
	const ImVec2 BadgeSize{std::max(18.0f * ToolbarScale, CountSize.x + 10.0f * ToolbarScale), CountFontSize + 4.0f * ToolbarScale};
	const float FilterWidth = ButtonWidth("Warnings") + Spacing + BadgeSize.x;
	const float ButtonsWidth = FilterWidth + ButtonWidth("Options") + ClearWidth + CopyWidth + Spacing * 3.0f;
	const float AvailableWidth = ImGui::GetContentRegionAvail().x;
	const bool bSingleRow = AvailableWidth >= ButtonsWidth + Spacing + 160.0f * ToolbarScale;
	ImGui::SetNextItemWidth(bSingleRow ? std::min(320.0f * ToolbarScale, AvailableWidth - ButtonsWidth - Spacing) : -1.0f);
	const bool bSearchChanged = ToolUI->DrawSearchField("##OutputLogSearch", "Search Log", SearchBuffer.data(), SearchBuffer.size());
	if (bSingleRow)
	{
		ImGui::SameLine();
	}
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.0f, 0.5f});
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	if (ImGui::Button("Warnings###OutputLogFilters", {FilterWidth, 0.0f}))
	{
		ImGui::OpenPopup("OutputLogFilter");
	}
	const ImVec2 FilterMinimum = ImGui::GetItemRectMin();
	const ImVec2 FilterMaximum = ImGui::GetItemRectMax();
	const ImVec2 BadgeMinimum{FilterMaximum.x - ImGui::GetStyle().FramePadding.x - BadgeSize.x, (FilterMinimum.y + FilterMaximum.y - BadgeSize.y) * 0.5f};
	ImDrawList* const ToolbarDrawList = ImGui::GetWindowDrawList();
	ToolbarDrawList->AddRectFilled(BadgeMinimum, {BadgeMinimum.x + BadgeSize.x, BadgeMinimum.y + BadgeSize.y}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.08f}), 4.0f * ToolbarScale);
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
		std::expected<void, FOutputLogError> SearchResult = OutputLog->SetSearch(SearchBuffer.data());
		if (!SearchResult)
		{
			ToolUI->EndPanel();
			return std::unexpected(FEditorFrameworkError{std::move(SearchResult.error().Message)});
		}
	}
	if (ImGui::BeginPopup("OutputLogFilter"))
	{
		for (const ELogLevel Level : {ELogLevel::Trace, ELogLevel::Debug, ELogLevel::Info, ELogLevel::Warning, ELogLevel::Error, ELogLevel::Critical})
		{
			bool bVisible = OutputLog->IsLevelVisible(Level);
			const std::string LevelName{GetLogLevelName(Level)};
			if (ImGui::MenuItem(LevelName.c_str(), nullptr, &bVisible))
			{
				std::expected<void, FOutputLogError> FilterResult = OutputLog->SetLevelVisible(Level, bVisible);
				if (!FilterResult)
				{
					ImGui::EndPopup();
					ToolUI->EndPanel();
					return std::unexpected(FEditorFrameworkError{std::move(FilterResult.error().Message)});
				}
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
		std::expected<std::string, FOutputLogError> Clipboard = OutputLog->CopySelectionOrVisible();
		if (!Clipboard)
		{
			ToolUI->EndPanel();
			return std::unexpected(FEditorFrameworkError{std::move(Clipboard.error().Message)});
		}
		ImGui::SetClipboardText(Clipboard->c_str());
	}

	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float CommandRowPadding = ImGui::GetStyle().WindowPadding.y;
	const float FooterHeight = ImGui::GetFontSize() + 10.0f * InterfaceScale + CommandRowPadding;
	ToolUI->PushLogFont();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f * InterfaceScale, 8.0f * InterfaceScale});
	if (ImGui::BeginChild("OutputLogEntries", {0.0f, -FooterHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const std::span<const FOutputLogLine> Lines = OutputLog->GetVisibleLines();
		const std::span<const std::string> TextLines = OutputLog->GetVisibleText();
		const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		const bool bShouldScroll = !Lines.empty() && ShouldScrollOutputLog(bReceivedRecords, OutputLog->IsAutoScroll(), bWasAtBottom, OutputLog->HasTailRequest());
		const float LineHeight = ImGui::GetFontSize() + 3.0f * InterfaceScale;
		const float TextOffsetY = (LineHeight - ImGui::GetFontSize()) * 0.5f;
		const ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		float TimeWidth = 0.0f;
		float CategoryWidth = 0.0f;
		for (const FOutputLogLine& Line : Lines)
		{
			TimeWidth = std::max(TimeWidth, ImGui::CalcTextSize(Line.Text.data(), Line.Text.data() + Line.TimeEnd, false).x);
			CategoryWidth = std::max(CategoryWidth, ImGui::CalcTextSize(Line.Text.data() + Line.CategoryBegin, Line.Text.data() + Line.CategoryEnd, false).x);
		}
		const float CategoryX = TimeWidth + 12.0f * InterfaceScale;
		const FOutputLogColumns Columns{CategoryX, CategoryX + CategoryWidth + 8.0f * InterfaceScale};
		float ContentWidth = AvailableSize.x;
		for (const FOutputLogLine& Line : Lines)
		{
			ContentWidth = std::max(ContentWidth, MeasureOutputLogTextPrefix(Line, Line.Text.size(), Columns) + 8.0f * InterfaceScale);
		}

		// Available height includes the scroll offset, so it must not determine content height.
		const float ContentHeight = std::max(1.0f, static_cast<float>(TextLines.size()) * LineHeight);
		const ImVec2 TextOrigin = ImGui::GetCursorScreenPos();
		(void)ImGui::InvisibleButton("##OutputLogText", {ContentWidth, ContentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
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
			const float ScrollStep = 360.0f * ImGui::GetIO().DeltaTime;
			if (MousePosition.y < WindowPosition.y + ContentMinimum.y)
			{
				ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - ScrollStep));
			}
			else if (MousePosition.y > WindowPosition.y + ContentMaximum.y)
			{
				ImGui::SetScrollY(ImGui::GetScrollY() + ScrollStep);
			}
		}
		if (bShouldScroll)
		{
			ImGui::SetScrollHereY(1.0f);
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
		const std::size_t LastVisibleLine = std::min(TextLines.size(), static_cast<std::size_t>(std::max(0.0f, (WindowBottom - TextOrigin.y) / LineHeight)) + 1);
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
				DrawList->AddRectFilled({SelectionX, LineY}, {std::max(SelectionX + 1.0f, SelectionEndX), LineY + LineHeight}, SelectionColor);
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
		if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
		{
			Editor.SuggestionIndex = 0;
			Editor.RebuildSuggestions();
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
			std::expected<std::string, FOutputLogError> HistoryCommand = Editor.OutputLog->NavigateHistory(Direction);
			Data->DeleteChars(0, Data->BufTextLen);
			if (HistoryCommand && !HistoryCommand->empty())
			{
				Data->InsertChars(0, HistoryCommand->data(), HistoryCommand->data() + HistoryCommand->size());
			}
		}
		return 0;
	};

	constexpr ImGuiInputTextFlags CommandFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackHistory;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f * InterfaceScale, 5.0f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	constexpr const char* SubmitLabel = "Enter";
	const float SubmitWidth = ImGui::CalcTextSize(SubmitLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(-(SubmitWidth + ImGui::GetStyle().ItemSpacing.x));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {32.0f * InterfaceScale, 5.0f * InterfaceScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.5f + 5.0f * InterfaceScale);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(PackColor(ToolUITheme::Border)));
	if (bFocusCommandRequested)
	{
		ImGui::SetKeyboardFocusHere();
		bFocusCommandRequested = false;
	}
	const bool bCommandSubmitted = ImGui::InputTextWithHint("##OutputLogCommand", "Enter Console Command", CommandBuffer.data(), CommandBuffer.size(), CommandFlags, InputCallback, &CallbackContext);
	ImGui::PopStyleColor();
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
	const float PromptX = InputMinimum.x + 12.0f * InterfaceScale;
	const float PromptY = (InputMinimum.y + InputMaximum.y) * 0.5f;
	const ImU32 PromptColor = PackColor(ToolUITheme::TextMuted);
	ImDrawList* const CommandDraw = ImGui::GetWindowDrawList();
	CommandDraw->PushClipRect(InputMinimum, InputMaximum, true);
	CommandDraw->AddLine({PromptX, PromptY - 4.0f * InterfaceScale}, {PromptX + 4.0f * InterfaceScale, PromptY}, PromptColor, InterfaceScale);
	CommandDraw->AddLine({PromptX + 4.0f * InterfaceScale, PromptY}, {PromptX, PromptY + 4.0f * InterfaceScale}, PromptColor, InterfaceScale);
	CommandDraw->AddLine({PromptX + 7.0f * InterfaceScale, PromptY + 4.0f * InterfaceScale}, {PromptX + 12.0f * InterfaceScale, PromptY + 4.0f * InterfaceScale}, PromptColor, InterfaceScale);
	CommandDraw->PopClipRect();
	if (bReclaimCommandFocus)
	{
		ImGui::SetKeyboardFocusHere(-1);
		bReclaimCommandFocus = false;
	}
	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * InterfaceScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
	if (ImGui::Button(SubmitLabel, {SubmitWidth, 0.0f}))
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
	if (!Suggestions.empty())
	{
		const std::size_t VisibleCount = std::min<std::size_t>(6, Suggestions.size());
		const float PopupPadding = 4.0f * InterfaceScale;
		const float PopupHeight = static_cast<float>(VisibleCount) * ImGui::GetFrameHeight() + PopupPadding * 2.0f;
		const ImGuiViewport* const Viewport = ImGui::GetWindowViewport();
		ImGui::SetNextWindowPos({InputMinimum.x, std::max(Viewport->WorkPos.y, InputMinimum.y - PopupHeight)});
		ImGui::SetNextWindowSize({InputMaximum.x - InputMinimum.x, PopupHeight});
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::SetNextWindowBgAlpha(0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PopupPadding, PopupPadding});
		constexpr ImGuiWindowFlags SuggestionFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("Command suggestions###OutputLogCommandSuggestions", nullptr, SuggestionFlags))
		{
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

	ToolUI->EndPanel();
	return {};
}
}
