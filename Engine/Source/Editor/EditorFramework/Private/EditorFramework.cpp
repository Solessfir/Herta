#include "Herta/EditorFramework/EditorFramework.h"

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/PreviewSelection.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/ViewportInteraction.h"
#include "Herta/ToolUI/Theme.h"
#include "Herta/ToolUI/ToolUI.h"
#include "ViewportGizmos.h"
#include "ViewportRotationFeedback.h"
#include "ViewportScaleGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <iterator>
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
inline constexpr std::array CategoryColors = {
    IM_COL32(126, 200, 255, 255),
    IM_COL32(142, 220, 182, 255),
    IM_COL32(244, 202, 128, 255),
    IM_COL32(203, 166, 255, 255),
    IM_COL32(255, 157, 170, 255),
    IM_COL32(115, 218, 224, 255),
    IM_COL32(192, 215, 128, 255),
    IM_COL32(240, 166, 219, 255),
    IM_COL32(166, 184, 255, 255),
    IM_COL32(235, 186, 151, 255),
    IM_COL32(135, 210, 154, 255),
    IM_COL32(225, 168, 255, 255)};

[[nodiscard]] ImU32 PackColor(const FToolUIColor Color) noexcept
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

constexpr std::array ViewportAxisColors{Im3d::Color(0xf05a5aff), Im3d::Color(0x64dc6eff), Im3d::Color(0x64a0ffff)};

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
	const ImVec2 Origin{Minimum.x + 36.0f * Scale, Minimum.y + Size.y - 36.0f * Scale};
	const float AxisLength = 16.0f * Scale;
	const float FontSize = ImGui::GetFontSize() * 0.8f;
	constexpr std::array Labels{"X", "Y", "Z"};
	std::array<std::size_t, 3> Order{0, 1, 2};
	std::ranges::sort(Order, [&](const std::size_t A, const std::size_t B)
	                  {
		                  return View(2, A) > View(2, B);
	                  });
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	DrawList->PushClipRect(Minimum, {Minimum.x + Size.x, Minimum.y + Size.y}, true);
	for (const std::size_t Axis : Order)
	{
		// Camera +X points left; screen coordinates point right and down.
		const ImVec2 Tip{Origin.x - View(0, Axis) * AxisLength, Origin.y - View(1, Axis) * AxisLength};
		DrawList->AddLine(Origin, Tip, ViewportAxisColors[Axis].getABGR(), 1.5f * Scale);
		DrawList->AddCircleFilled(Tip, 2.0f * Scale, ViewportAxisColors[Axis].getABGR());
	}
	for (const std::size_t Axis : Order)
	{
		const float Length = std::hypot(View(0, Axis), View(1, Axis));
		const ImVec2 Direction = Length > 0.001f ? ImVec2{-View(0, Axis) / Length, -View(1, Axis) / Length} : ImVec2{0.0f, 1.0f};
		const ImVec2 LabelSize = ImGui::GetFont()->CalcTextSizeA(FontSize, 100.0f * Scale, 0.0f, Labels[Axis]);
		const float LabelRadius = AxisLength * Length + 3.0f * Scale + std::hypot(LabelSize.x, LabelSize.y) * 0.5f;
		const ImVec2 LabelPosition{Origin.x + Direction.x * LabelRadius - LabelSize.x * 0.5f, Origin.y + Direction.y * LabelRadius - LabelSize.y * 0.5f};
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

[[nodiscard]] std::size_t GetNextUtf8Boundary(const std::string_view Text, const std::size_t ByteOffset) noexcept
{
	if (ByteOffset >= Text.size())
	{
		return Text.size();
	}

	const unsigned char FirstByte = static_cast<unsigned char>(Text[ByteOffset]);
	std::size_t CodePointSize = 1;
	if ((FirstByte & 0xE0u) == 0xC0u)
	{
		CodePointSize = 2;
	}
	else if ((FirstByte & 0xF0u) == 0xE0u)
	{
		CodePointSize = 3;
	}
	else if ((FirstByte & 0xF8u) == 0xF0u)
	{
		CodePointSize = 4;
	}
	return std::min(Text.size(), ByteOffset + CodePointSize);
}

[[nodiscard]] float MeasureTextPrefix(const std::string_view Text, const std::size_t ByteCount)
{
	const std::size_t ClampedByteCount = std::min(ByteCount, Text.size());
	const char* const TextBegin = Text.data();
	return ImGui::CalcTextSize(TextBegin, TextBegin + ClampedByteCount, false).x;
}

[[nodiscard]] std::size_t FindByteAtX(const std::string_view Text, const float LocalX)
{
	if (LocalX <= 0.0f)
	{
		return 0;
	}

	float TextX = 0.0f;
	for (std::size_t ByteOffset = 0; ByteOffset < Text.size();)
	{
		const std::size_t NextByteOffset = GetNextUtf8Boundary(Text, ByteOffset);
		const float CharacterWidth = ImGui::CalcTextSize(Text.data() + ByteOffset, Text.data() + NextByteOffset, false).x;
		if (LocalX < TextX + CharacterWidth * 0.5f)
		{
			return ByteOffset;
		}
		TextX += CharacterWidth;
		ByteOffset = NextByteOffset;
	}
	return Text.size();
}

[[nodiscard]] FLogTextPosition HitTestText(const std::span<const std::string> Lines, const ImVec2 TextOrigin, const float LineHeight, const ImVec2 MousePosition)
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
	return {LineIndex, FindByteAtX(Lines[LineIndex], MousePosition.x - TextOrigin.x)};
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

void ContinueViewportToolbar(const char* const Label, const float ExtraWidth = 0.0f)
{
	const float RemainingWidth = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
	const float RequiredWidth = ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x + ExtraWidth;
	if (RemainingWidth >= RequiredWidth)
	{
		ImGui::SameLine();
	}
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
	std::unique_ptr<FOutputLogModel> OutputLog;
	std::array<char, 512> SearchBuffer = {};
	std::array<char, 512> CommandBuffer = {};
	std::vector<std::string> Suggestions;
	int SuggestionIndex = -1;
	bool bOutputLogOpen = true;
	bool bReclaimCommandFocus = false;
	std::uint64_t ViewportTexture = 0;
	FExtent2D ViewportExtent{960, 540};
	FViewportCameraController ViewportCamera;
	Im3d::Context ViewportGizmos;
	Im3d::Vec3 PreviewTranslation{0.0f};
	Im3d::Mat3 PreviewRotation{1.0f};
	Im3d::Vec3 PreviewScale{1.0f};
	std::optional<Im3d::Mat4> PreviewDragStart;
	FViewportRotationFeedbackState RotationFeedback;
	FViewportScaleGizmoState ScaleGizmoState;
	FViewportScaleGizmoFeedback ScaleFeedback;
	FMeshRenderView ViewportRenderView;
	std::vector<FDebugDrawVertex> ViewportDebugVertices;
	std::vector<FDebugDrawList> ViewportDebugDrawLists;
	FViewportInteractionState ViewportInteraction;
	bool bPreviewSelected = true;
	bool bStartPanelOpen = false;
	bool bLocalGizmo = false;
	bool bSnapEnabled = false;
	bool bGridVisible = true;
	bool bAxesVisible = true;
	bool bOrientationIndicatorVisible = true;
	bool bBoundsVisible = false;
	float TranslationSnap = 0.5f;
	float RotationSnapDegrees = 15.0f;
	float ScaleSnap = 0.1f;
	int GizmoMode = Im3d::GizmoMode_Translation;
	bool bTransformGizmoVisible = true;

	FImplementation();

	[[nodiscard]] std::expected<void, FEditorFrameworkError> DrawOutputLog();
	void DrawStartPanel();
	void DrawDetailsPanel();
	void DrawViewport();
	void DrawViewportToolbar();
	void UpdateViewport(const ImVec2 ImageMinimum, const ImVec2 Size);
	void BuildViewportDebugDraw(bool bGizmoInput, const FVector2 NormalizedMouse);
	void FocusPreview();
	void RebuildSuggestions();
	[[nodiscard]] std::expected<void, FEditorFrameworkError> SubmitCommand();
};

FEditorFramework::FImplementation::FImplementation()
{
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
	ViewportRenderView = {Camera.View, Camera.Projection, ToHertaMatrix(Im3d::Mat4(PreviewTranslation, PreviewRotation, PreviewScale))};
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

std::expected<void, FEditorFrameworkError> FEditorFramework::Draw()
{
	try
	{
		Implementation->ToolUI->DrawWorkspace("Herta Editor", [&]
		                                      {
			                                      ImGui::MenuItem("Start", nullptr, &Implementation->bStartPanelOpen);
		                                      });
		if (Implementation->bStartPanelOpen)
		{
			Implementation->DrawStartPanel();
		}
		Implementation->DrawDetailsPanel();
		Implementation->DrawViewport();
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
	const FMatrix4 Model = ToHertaMatrix(Im3d::Mat4(PreviewTranslation, PreviewRotation, PreviewScale));
	const FVector3 HalfExtent{
	    std::abs(Model(0, 0)) + std::abs(Model(0, 1)) + std::abs(Model(0, 2)),
	    std::abs(Model(1, 0)) + std::abs(Model(1, 1)) + std::abs(Model(1, 2)),
	    std::abs(Model(2, 0)) + std::abs(Model(2, 1)) + std::abs(Model(2, 2))};
	const float AspectRatio = ViewportExtent.Height > 0 ? static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height) : 16.0f / 9.0f;
	ViewportCamera.Focus({PreviewTranslation.x, PreviewTranslation.y, PreviewTranslation.z}, HalfExtent, AspectRatio);
}

void FEditorFramework::FImplementation::DrawViewportToolbar()
{
	ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
	if (ImGui::RadioButton("Hide (Q)", !bTransformGizmoVisible))
	{
		bTransformGizmoVisible = false;
		ViewportGizmos.resetId();
	}
	constexpr std::array Modes{std::pair{Im3d::GizmoMode_Translation, "Move (W)"}, std::pair{Im3d::GizmoMode_Rotation, "Rotate (E)"}, std::pair{Im3d::GizmoMode_Scale, "Scale (R)"}};
	for (std::size_t Index = 0; Index < Modes.size(); ++Index)
	{
		const auto [Mode, Label] = Modes[Index];
		ContinueViewportToolbar(Label);
		if (ImGui::RadioButton(Label, bTransformGizmoVisible && GizmoMode == Mode))
		{
			bTransformGizmoVisible = true;
			GizmoMode = Mode;
			ViewportGizmos.resetId();
		}
	}
	ContinueViewportToolbar(bLocalGizmo ? "Local (L)" : "World (L)");
	if (ImGui::Button(bLocalGizmo ? "Local (L)" : "World (L)"))
	{
		bLocalGizmo = !bLocalGizmo;
		ViewportGizmos.resetId();
	}
	ContinueViewportToolbar("Snap", ImGui::GetFrameHeight());
	ImGui::Checkbox("Snap", &bSnapEnabled);
	ContinueViewportToolbar("Grid (m)", 100.0f);
	ImGui::SetNextItemWidth(100.0f);
	ImGui::DragFloat("Grid (m)", &TranslationSnap, 0.05f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	TranslationSnap = std::isfinite(TranslationSnap) ? std::clamp(TranslationSnap, 0.001f, 100.0f) : 0.5f;
	ContinueViewportToolbar("Settings");
	if (ImGui::Button("Settings"))
	{
		ImGui::OpenPopup("ViewportSettings");
	}
	if (ImGui::BeginPopup("ViewportSettings"))
	{
		ImGui::SeparatorText("Snapping");
		ImGui::SetNextItemWidth(160.0f);
		ImGui::DragFloat("Rotation (degrees)", &RotationSnapDegrees, 1.0f, 0.1f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SetNextItemWidth(160.0f);
		ImGui::DragFloat("Scale", &ScaleSnap, 0.01f, 0.001f, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		RotationSnapDegrees = std::isfinite(RotationSnapDegrees) ? std::clamp(RotationSnapDegrees, 0.1f, 180.0f) : 15.0f;
		ScaleSnap = std::isfinite(ScaleSnap) ? std::clamp(ScaleSnap, 0.001f, 10.0f) : 0.1f;
		ImGui::SeparatorText("Camera");
		float MovementSpeed = ViewportCamera.GetMovementSpeed();
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::SliderFloat("Speed (m/s)", &MovementSpeed, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
		{
			ViewportCamera.SetMovementSpeed(MovementSpeed);
		}
		float Sensitivity = ViewportCamera.GetMouseSensitivity() * 180.0f / std::numbers::pi_v<float>;
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::SliderFloat("Look (degrees/pixel)", &Sensitivity, 0.02f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			ViewportCamera.SetMouseSensitivity(Sensitivity * std::numbers::pi_v<float> / 180.0f);
		}
		ImGui::SeparatorText("Overlays");
		ImGui::Checkbox("Grid", &bGridVisible);
		ImGui::Checkbox("World axes", &bAxesVisible);
		ImGui::Checkbox("Corner axis indicator", &bOrientationIndicatorVisible);
		ImGui::Checkbox("Preview bounds", &bBoundsVisible);
		if (ImGui::Button("Reset preview transform"))
		{
			PreviewTranslation = Im3d::Vec3(0.0f);
			PreviewRotation = Im3d::Mat3(1.0f);
			PreviewScale = Im3d::Vec3(1.0f);
		}
		ImGui::EndPopup();
	}
	ContinueViewportToolbar("Focus (F)");
	if (ImGui::Button("Focus (F)"))
	{
		FocusPreview();
	}
	ContinueViewportToolbar("?");
	ImGui::TextDisabled("?");
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("RMB + WASD/QE: fly | Shift: faster | RMB + wheel: speed\nAlt + LMB: orbit | MMB: pan | Wheel / Alt + RMB: dolly\nF: focus preview | Q: hide gizmo | W/E/R: move/rotate/scale | L: local/world\nScale gizmos always use local axes. Escape cancels the drag.");
	}
	ImGui::EndDisabled();
}

void FEditorFramework::FImplementation::UpdateViewport(const ImVec2 ImageMinimum, const ImVec2 Size)
{
	const ImGuiIO& IO = ImGui::GetIO();
	const bool bImageHovered = ImGui::IsItemHovered();
	const bool bImageActive = ImGui::IsItemActive();
	const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	const bool bEscapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	if (bEscapePressed && PreviewDragStart)
	{
		PreviewTranslation = PreviewDragStart->getTranslation();
		PreviewRotation = PreviewDragStart->getRotation();
		PreviewScale = PreviewDragStart->getScale();
	}
	FViewportInteractionInput InteractionInput;
	InteractionInput.bImageHovered = bImageHovered;
	InteractionInput.bImageActive = bImageActive;
	InteractionInput.bWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IO.AppFocusLost;
	InteractionInput.bInputBlocked = bPopupOpen || (ImGui::IsAnyItemActive() && !bImageActive) || bEscapePressed;
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
	if (bPreviewSelected && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ViewportInteraction.CameraMode == EViewportCameraMode::None)
	{
		if (!PreviewDragStart)
		{
			PreviewDragStart.emplace(PreviewTranslation, PreviewRotation, PreviewScale);
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
	ViewportCamera.Update(CameraInput, IO.DeltaTime, {Size.x, Size.y});
	const float AspectRatio = static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height);
	const auto Camera = ViewportCamera.GetSnapshot(AspectRatio);
	ViewportRenderView.View = Camera.View;
	ViewportRenderView.Projection = Camera.Projection;
	const ImVec2 Mouse = ImGui::GetMousePos();
	const FVector2 NormalizedMouse{(Mouse.x - ImageMinimum.x) / Size.x, (Mouse.y - ImageMinimum.y) / Size.y};
	const bool bGizmoInput = bInputAllowed && CameraInput.Mode == EViewportCameraMode::None && (bImageHovered || ViewportInteraction.DragButton == ImGuiMouseButton_Left);
	const float InterfaceScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float GizmoPixelScale = GetViewportGizmoPixelScale(InterfaceScale, Size.y, static_cast<float>(ViewportExtent.Height));
	ViewportGizmos.m_gizmoHeightPixels = 80.0f * GizmoPixelScale;
	ViewportGizmos.m_gizmoSizePixels = 4.0f * GizmoPixelScale;
	BuildViewportDebugDraw(bGizmoInput, NormalizedMouse);
}

void FEditorFramework::FImplementation::BuildViewportDebugDraw(const bool bGizmoInput, const FVector2 NormalizedMouse)
{
	const FIm3dContextScope ContextScope(ViewportGizmos);
	const float AspectRatio = static_cast<float>(ViewportExtent.Width) / static_cast<float>(ViewportExtent.Height);
	const auto Camera = ViewportCamera.GetSnapshot(AspectRatio);
	const auto CursorRay = ViewportCamera.MakePickingRay(NormalizedMouse, AspectRatio);
	const auto ForwardRay = ViewportCamera.MakePickingRay({0.5f, 0.5f}, AspectRatio);
	Im3d::AppData& AppData = Im3d::GetAppData();
	AppData = Im3d::AppData{};
	AppData.m_deltaTime = ImGui::GetIO().DeltaTime;
	AppData.m_viewportSize = {static_cast<float>(ViewportExtent.Width), static_cast<float>(ViewportExtent.Height)};
	AppData.m_viewOrigin = ToIm3dVector(Camera.Position);
	AppData.m_viewDirection = ToIm3dVector(ForwardRay.Direction);
	AppData.m_cursorRayOrigin = ToIm3dVector(CursorRay.Origin);
	AppData.m_cursorRayDirection = ToIm3dVector(bGizmoInput ? CursorRay.Direction : -ForwardRay.Direction);
	AppData.m_projScaleY = 2.0f / Camera.Projection(1, 1);
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
	if (!bGizmoInput || !bPreviewSelected || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Scale)
	{
		ScaleGizmoState.Reset();
	}
	if (!bGizmoInput || !bPreviewSelected || !bTransformGizmoVisible || GizmoMode != Im3d::GizmoMode_Rotation)
	{
		RotationFeedback.Reset();
	}
	Im3d::PushLayerId("ViewportGizmos");
	const Im3d::Vec3 PreviousTranslation = PreviewTranslation;
	const Im3d::Mat3 PreviousRotation = PreviewRotation;
	const Im3d::Vec3 PreviousScale = PreviewScale;
	if (bPreviewSelected && bTransformGizmoVisible)
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
	Im3d::PopLayerId();
	const Im3d::Mat4 Model(PreviewTranslation, PreviewRotation, PreviewScale);
	ViewportRenderView.Model = ToHertaMatrix(Model);
	if (bGizmoInput && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && Im3d::GetActiveId() == Im3d::Id_Invalid)
	{
		bPreviewSelected = HitTestPreviewCube(CursorRay, ViewportRenderView.Model);
		if (!bPreviewSelected)
		{
			PreviewDragStart.reset();
			ViewportGizmos.resetId();
		}
	}
	Im3d::PushLayerId("ViewportWorld");
	if (bGridVisible)
	{
		for (int Coordinate = -10; Coordinate <= 10; ++Coordinate)
		{
			const float Offset = static_cast<float>(Coordinate);
			const Im3d::Color Color = Coordinate == 0 ? Im3d::Color(0x747a8480) : Im3d::Color(0x555b6560);
			Im3d::DrawLine({Offset, 0.0f, -10.0f}, {Offset, 0.0f, 10.0f}, 1.0f, Color);
			Im3d::DrawLine({-10.0f, 0.0f, Offset}, {10.0f, 0.0f, Offset}, 1.0f, Color);
		}
	}
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
	if (bPreviewSelected)
	{
		Im3d::PushLayerId("ViewportSelection");
		for (const auto& [Start, End] : GetPreviewCubeSilhouette(Camera.Position, ViewportRenderView.Model))
		{
			Im3d::DrawLine(ToIm3dVector(Start), ToIm3dVector(End), ViewportGizmos.m_gizmoSizePixels * 0.5f, Im3d::Color(0xeba30aff));
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
		if (!bPreviewSelected && List.m_layerId == Im3d::MakeId("ViewportGizmos"))
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

void FEditorFramework::FImplementation::DrawViewport()
{
	ImGui::SetNextWindowSize({960, 540}, ImGuiCond_FirstUseEver);
	if (ToolUI->BeginPanel("Viewport"))
	{
		DrawViewportToolbar();
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		const float Scale = ImGui::GetWindowViewport()->DpiScale;
		ViewportExtent = {static_cast<std::uint32_t>(std::clamp(Size.x * Scale, 1.0f, 4096.0f)), static_cast<std::uint32_t>(std::clamp(Size.y * Scale, 1.0f, 4096.0f))};
		if (ViewportTexture != 0 && Size.x > 0 && Size.y > 0)
		{
			const ImVec2 ImageMinimum = ImGui::GetCursorScreenPos();
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(ViewportTexture)), Size);
			ImGui::SetCursorScreenPos(ImageMinimum);
			(void)ImGui::InvisibleButton("##ViewportInteraction", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			UpdateViewport(ImageMinimum, Size);
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
					const FVector2 ScreenStart{(ClipStart.X / ClipStart.W + 1.0f) * Size.x * 0.5f, (1.0f - ClipStart.Y / ClipStart.W) * Size.y * 0.5f};
					const FVector2 ScreenEnd = RotationFeedback.AngleDegrees ? FVector2{ImGui::GetMousePos().x - ImageMinimum.x, ImGui::GetMousePos().y - ImageMinimum.y} : FVector2{(ClipEnd.X / ClipEnd.W + 1.0f) * Size.x * 0.5f, (1.0f - ClipEnd.Y / ClipEnd.W) * Size.y * 0.5f};
					const FVector2 Delta = ScreenEnd - ScreenStart;
					float First = 0.0f;
					float Last = 1.0f;
					for (std::size_t Axis = 0; Axis < 2; ++Axis)
					{
						const float Limit = Axis == 0 ? Size.x : Size.y;
						if (std::abs(Delta[Axis]) > 0.001f)
						{
							const float A = -ScreenStart[Axis] / Delta[Axis];
							const float B = (Limit - ScreenStart[Axis]) / Delta[Axis];
							First = std::max(First, std::min(A, B));
							Last = std::min(Last, std::max(A, B));
						}
					}
					const float Distance = Delta.Length();
					const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
					ImDrawList* const Trail = ImGui::GetWindowDrawList();
					Trail->PushClipRect(ImageMinimum, {ImageMinimum.x + Size.x, ImageMinimum.y + Size.y}, true);
					constexpr ImU32 TrailColor = IM_COL32(235, 163, 10, 255);
					if (Distance > 0.001f && std::isfinite(Distance))
					{
						const int DashCount = static_cast<int>(std::clamp(std::ceil((Last - First) * Distance / (7.0f * UiScale)), 0.0f, 4096.0f));
						for (int Dash = 0; Dash < DashCount; ++Dash)
						{
							const float Offset = First * Distance + static_cast<float>(Dash) * 7.0f * UiScale;
							const FVector2 A = ScreenStart + Delta * (Offset / Distance);
							const FVector2 B = ScreenStart + Delta * (std::min(Offset + 3.0f * UiScale, Last * Distance) / Distance);
							Trail->AddLine({ImageMinimum.x + A.X, ImageMinimum.y + A.Y}, {ImageMinimum.x + B.X, ImageMinimum.y + B.Y}, TrailColor, 1.5f * UiScale);
						}
						Trail->AddCircleFilled({ImageMinimum.x + ScreenStart.X, ImageMinimum.y + ScreenStart.Y}, 3.0f * UiScale, TrailColor);
					}
					Trail->PopClipRect();
				}
			}
			const std::string FpsText = ViewportInteraction.CameraMode == EViewportCameraMode::Fly ? std::format("{:.0f} FPS | Speed: {:.2f} m/s", ImGui::GetIO().Framerate, ViewportCamera.GetMovementSpeed() * (ImGui::GetIO().KeyShift ? 4.0f : 1.0f)) : std::format("{:.0f} FPS", ImGui::GetIO().Framerate);
			const ImVec2 TextSize = ImGui::CalcTextSize(FpsText.c_str());
			const ImVec2 TextPosition{ImageMinimum.x + 12.0f, ImageMinimum.y + 10.0f};
			FToolUIColor Background = ToolUITheme::Surface0;
			Background.Alpha = 210;
			ImDrawList* const DrawList = ImGui::GetWindowDrawList();
			DrawList->AddRectFilled({TextPosition.x - 6.0f, TextPosition.y - 4.0f}, {TextPosition.x + TextSize.x + 6.0f, TextPosition.y + TextSize.y + 4.0f}, PackColor(Background), 4.0f);
			DrawList->AddText(TextPosition, PackColor(ToolUITheme::TextPrimary), FpsText.c_str());
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

void FEditorFramework::FImplementation::DrawDetailsPanel()
{
	if (!ToolUI->BeginPanel("Details"))
	{
		ToolUI->EndPanel();
		return;
	}
	if (!bPreviewSelected)
	{
		ImGui::TextDisabled("Select an object in the viewport.");
		ToolUI->EndPanel();
		return;
	}
	ImGui::TextUnformatted("Preview Cube");
	ImGui::SeparatorText("Transform");
	const auto DrawProperty = [](const char* Label, Im3d::Vec3& Value, const float Speed, const float Minimum, const float Maximum, const float ResetValue)
	{
		ImGui::PushID(Label);
		ImGui::TextUnformatted(Label);
		const float Width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f);
		constexpr std::array Formats{"X: %.3f", "Y: %.3f", "Z: %.3f"};
		bool bChanged = false;
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis > 0)
			{
				ImGui::SameLine();
			}
			ImGui::PushID(Axis);
			ImGui::SetNextItemWidth(Width);
			const float Previous = Value[Axis];
			bChanged |= ImGui::DragFloat("##Value", &Value[Axis], Speed, Minimum, Maximum, Formats[static_cast<std::size_t>(Axis)], ImGuiSliderFlags_AlwaysClamp);
			if (!std::isfinite(Value[Axis]))
			{
				Value[Axis] = Previous;
			}
			const ImVec2 Min = ImGui::GetItemRectMin();
			const ImVec2 Max = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled({Min.x, Min.y + 3.0f}, {Min.x + 2.0f, Max.y - 3.0f}, ViewportAxisColors[static_cast<std::size_t>(Axis)].getABGR());
			ImGui::PopID();
		}
		if (ImGui::SmallButton("Reset"))
		{
			Value = Im3d::Vec3(ResetValue);
			bChanged = true;
		}
		ImGui::PopID();
		return bChanged;
	};
	ImGui::BeginDisabled(ViewportInteraction.DragButton >= 0);
	DrawProperty("Location (m)", PreviewTranslation, 0.01f, 0.0f, 0.0f, 0.0f);
	Im3d::Vec3 RotationDegrees = Im3d::ToEulerXYZ(PreviewRotation) * (180.0f / std::numbers::pi_v<float>);
	if (DrawProperty("Rotation (degrees)", RotationDegrees, 0.1f, -360.0f, 360.0f, 0.0f))
	{
		Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.0f);
		PreviewRotation = Im3d::FromEulerXYZ(Radians);
	}
	DrawProperty("Scale", PreviewScale, 0.01f, 0.001f, 1000.0f, 1.0f);
	ImGui::EndDisabled();
	ToolUI->EndPanel();
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
	bReclaimCommandFocus = true;
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

	if (!ToolUI->BeginPanel("Output Log", &bOutputLogOpen))
	{
		ToolUI->EndPanel();
		return {};
	}

	bool bCopyRequested = false;
	if (ImGui::Button("Clear"))
	{
		OutputLog->Clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy"))
	{
		bCopyRequested = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Filter"))
	{
		ImGui::OpenPopup("OutputLogFilter");
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
	ImGui::SameLine();
	if (ImGui::Button("Options"))
	{
		ImGui::OpenPopup("OutputLogOptions");
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
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputTextWithHint("##OutputLogSearch", "Search messages, categories, and verbosity", SearchBuffer.data(), SearchBuffer.size()))
	{
		std::expected<void, FOutputLogError> SearchResult = OutputLog->SetSearch(SearchBuffer.data());
		if (!SearchResult)
		{
			ToolUI->EndPanel();
			return std::unexpected(FEditorFrameworkError{std::move(SearchResult.error().Message)});
		}
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

	const float InterfaceScale = ImGui::GetFontSize() / 15.0f;
	const float FooterHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f * InterfaceScale, 4.0f * InterfaceScale});
	if (ImGui::BeginChild("OutputLogEntries", {0.0f, -FooterHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const std::span<const FOutputLogLine> Lines = OutputLog->GetVisibleLines();
		const std::span<const std::string> TextLines = OutputLog->GetVisibleText();
		const bool bWasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		const bool bShouldScroll = !Lines.empty() && ShouldScrollOutputLog(bReceivedRecords, OutputLog->IsAutoScroll(), bWasAtBottom, OutputLog->HasTailRequest());
		const float LineHeight = ImGui::GetFontSize() + 3.0f * InterfaceScale;
		const float TextOffsetY = (LineHeight - ImGui::GetFontSize()) * 0.5f;
		const ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		float ContentWidth = AvailableSize.x;
		for (const std::string& Text : TextLines)
		{
			ContentWidth = std::max(ContentWidth, ImGui::CalcTextSize(Text.data(), Text.data() + Text.size(), false).x + 8.0f * InterfaceScale);
		}

		const float ContentHeight = std::max(AvailableSize.y, static_cast<float>(TextLines.size()) * LineHeight);
		const ImVec2 TextOrigin = ImGui::GetCursorScreenPos();
		(void)ImGui::InvisibleButton("##OutputLogText", {ContentWidth, ContentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_EnableNav);
		FLogTextSelection& Selection = OutputLog->GetSelection();
		if (ImGui::IsItemActivated())
		{
			Selection.Begin(HitTestText(TextLines, TextOrigin, LineHeight, ImGui::GetMousePos()), ImGui::GetIO().KeyShift);
		}
		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const ImVec2 MousePosition = ImGui::GetMousePos();
			Selection.Update(HitTestText(TextLines, TextOrigin, LineHeight, MousePosition));
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
			const std::string& Text = TextLines[LineIndex];
			const float LineY = TextOrigin.y + static_cast<float>(LineIndex) * LineHeight;
			if (bHasSelection && LineIndex >= SelectionFirst.Line && LineIndex <= SelectionLast.Line)
			{
				const std::size_t FirstByte = LineIndex == SelectionFirst.Line ? SelectionFirst.Byte : 0;
				const std::size_t LastByte = LineIndex == SelectionLast.Line ? SelectionLast.Byte : Text.size();
				const float SelectionX = TextOrigin.x + MeasureTextPrefix(Text, FirstByte);
				float SelectionEndX = TextOrigin.x + MeasureTextPrefix(Text, LastByte);
				if (LineIndex < SelectionLast.Line)
				{
					SelectionEndX += ImGui::GetFontSize() * 0.35f;
				}
				DrawList->AddRectFilled({SelectionX, LineY}, {std::max(SelectionX + 1.0f, SelectionEndX), LineY + LineHeight}, SelectionColor);
			}
			DrawList->AddText({TextOrigin.x, LineY + TextOffsetY}, ResolveLineColor(Lines[LineIndex], OutputLog->IsCategoryColorizationEnabled()), Text.data(), Text.data() + Text.size());
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();

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
	constexpr const char* SubmitLabel = "Submit";
	const float SubmitWidth = ImGui::CalcTextSize(SubmitLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(-(SubmitWidth + ImGui::GetStyle().ItemSpacing.x));
	if (ImGui::InputTextWithHint("##OutputLogCommand", "Enter command, or type help", CommandBuffer.data(), CommandBuffer.size(), CommandFlags, InputCallback, &CallbackContext))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}
	const ImVec2 InputMinimum = ImGui::GetItemRectMin();
	const ImVec2 InputMaximum = ImGui::GetItemRectMax();
	if (bReclaimCommandFocus)
	{
		ImGui::SetKeyboardFocusHere(-1);
		bReclaimCommandFocus = false;
	}
	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.5f});
	if (ImGui::Button(SubmitLabel, {SubmitWidth, 0.0f}))
	{
		std::expected<void, FEditorFrameworkError> SubmitResult = SubmitCommand();
		if (!SubmitResult)
		{
			ImGui::PopStyleVar();
			ToolUI->EndPanel();
			return SubmitResult;
		}
	}
	ImGui::PopStyleVar();

	std::optional<std::string> ClickedSuggestion;
	if (!Suggestions.empty())
	{
		const std::size_t VisibleCount = std::min<std::size_t>(6, Suggestions.size());
		const float PopupPadding = 4.0f * InterfaceScale;
		const float PopupHeight = static_cast<float>(VisibleCount) * ImGui::GetFrameHeight() + PopupPadding * 2.0f;
		const ImGuiViewport* const Viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos({InputMinimum.x, std::max(Viewport->WorkPos.y, InputMinimum.y - PopupHeight)});
		ImGui::SetNextWindowSize({InputMaximum.x - InputMinimum.x, PopupHeight});
		ImGui::SetNextWindowViewport(Viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PopupPadding, PopupPadding});
		constexpr ImGuiWindowFlags SuggestionFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("Command suggestions###OutputLogCommandSuggestions", nullptr, SuggestionFlags))
		{
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
