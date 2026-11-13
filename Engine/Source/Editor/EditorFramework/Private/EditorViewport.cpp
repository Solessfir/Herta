#include "EditorFrameworkPrivate.h"
#include "Herta/Navigation/NavigationWorld.h"

namespace Herta
{
namespace
{
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

void FEditorFramework::FImplementation::BuildCollisionDebugDraw()
{
	CombinedDebugDrawLists.clear();
	const FPhysicsWorld* const Physics = !bCollisionVisible ? nullptr : PlaySession ? PlaySession->GetPhysicsWorld()
	                                                                                : const_cast<FLevelSimulation&>(Simulation).GetPhysicsWorld();
	// Play shows its own navigation, cut by obstacles as they move; otherwise the latest build of the level.
	std::span<const FVector3> Navigated;
	if (bNavigationVisible)
	{
		if (const FNavigationWorld* const PlayNavigation = PlaySession ? PlaySession->GetNavigation() : nullptr)
		{
			NavigationTriangles.clear();
			PlayNavigation->GetDebugTriangles(NavigationTriangles);
			Navigated = NavigationTriangles;
		}
		else if (Navigation && !PlaySession)
		{
			Navigated = Navigation->GetDebugTriangles();
		}
	}

	if (!Physics && Navigated.empty())
	{
		return;
	}

	if (!PlaySession)
	{
		CombinedDebugDrawLists = ViewportDebugDrawLists;
	}

	if (Physics)
	{
		Physics->GetDebugLines(CollisionLines);
		CollisionVertices.clear();
		CollisionVertices.reserve(CollisionLines.size() * 2);
		for (const FPhysicsDebugLine& Line : CollisionLines)
		{
			// Static grey, kinematic blue, dynamic green, sensors amber; sleeping bodies dim.
			std::array<float, 4> Color = Line.bSensor ? std::array{1.f, 0.7f, 0.2f, 1.f} : Line.MotionType == EPhysicsMotionType::Static  ? std::array{0.55f, 0.58f, 0.62f, 0.6f}
			                                                                           : Line.MotionType == EPhysicsMotionType::Kinematic ? std::array{0.35f, 0.6f, 1.f, 1.f}
			                                                                                                                              : std::array{0.35f, 1.f, 0.45f, 1.f};
			if (Line.bSleeping && Line.MotionType != EPhysicsMotionType::Static)
			{
				Color = {Color[0] * 0.5f, Color[1] * 0.5f, Color[2] * 0.5f, Color[3]};
			}

			// The editor's float view space is world space while the physics origin stays at zero.
			const FVector3 Start{Physics->ToWorld(Line.Start).Meters};
			const FVector3 End{Physics->ToWorld(Line.End).Meters};
			CollisionVertices.push_back({.Position = Start, .Size = 1.5f, .Color = Color});
			CollisionVertices.push_back({.Position = End, .Size = 1.5f, .Color = Color});
		}

		CombinedDebugDrawLists.push_back({.Primitive = EDebugPrimitive::Lines, .Vertices = CollisionVertices, .bDepthTest = true});
	}

	if (!Navigated.empty())
	{
		// Lifted a little off the ground it lies on, so the depth test keeps it visible without showing it through walls.
		NavigationVertices.clear();
		NavigationVertices.reserve(Navigated.size());
		for (const FVector3& Point : Navigated)
		{
			NavigationVertices.push_back({.Position = Point + FVector3{0.f, 0.04f, 0.f}, .Size = 1.f, .Color = {0.2f, 0.85f, 0.45f, 0.35f}});
		}

		CombinedDebugDrawLists.push_back({.Primitive = EDebugPrimitive::Triangles, .Vertices = NavigationVertices, .bDepthTest = true});
	}
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

FMatrix4 FEditorFramework::FImplementation::GetPreviewBoundsMatrix(const std::size_t Index) const
{
	const FLevelBodyShape Shape = GetPreviewBodyShape(Index);
	return FMatrix4::Translation(Shape.Center) * FMatrix4::Scale(Shape.HalfExtents);
}

FLevelBodyShape FEditorFramework::FImplementation::GetPreviewBodyShape(const std::size_t Index) const
{
	return Index < VisualEntities.size() ? GetLevelBodyShape(VisualEntities[Index], Index < PreviewMeshes.size() ? PreviewMeshes[Index] : nullptr) : FLevelBodyShape{.Center = {}, .HalfExtents = {0.15f, 0.15f, 0.15f}};
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

	if (!bGameView && ViewportInteraction.DragButton < 0 && IO.KeyAlt && InteractionInput.MouseClicked[ImGuiMouseButton_Left] && bImageHovered && bImageActive && InteractionInput.bWindowFocused && !InteractionInput.bInputBlocked && !IsSimulating() && PreviewSelection.Active >= 0 && bTransformGizmoVisible)
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

	if (!ViewportBoxSelection.bActive && !IsSimulating() && PreviewSelection.Active >= 0 && ViewportInteraction.DragButton == ImGuiMouseButton_Left && ViewportInteraction.CameraMode == EViewportCameraMode::None)
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

	// While playing, the game owns the mouse and keyboard.
	const bool bInputAllowed = InteractionInput.bWindowFocused && !InteractionInput.bInputBlocked && !PlaySession;
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
			CameraSpeedShownUntil = IO.MouseWheel != 0.f ? ImGui::GetTime() + 1.5 : CameraSpeedShownUntil;
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
	BuildCollisionDebugDraw();
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
	[[maybe_unused]] auto& [Label, PreviewTranslation, PreviewRotation, PreviewScale, PreviewMesh, ObjectId, Parent, Kind, Category] = Candidate;
	const Im3d::Vec3 PreviousTranslation = PreviewTranslation;
	const Im3d::Mat3 PreviousRotation = PreviewRotation;
	const Im3d::Vec3 PreviousScale = PreviewScale;
	const FPreviewObject PreviousObject = GetActivePreviewObject();

	if (!bGameView && !IsSimulating() && PreviewSelection.Active >= 0 && bTransformGizmoVisible)
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
	if (!IsSimulating())
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
			// Hidden and locked entities let the click through to what is behind them.
			if (Index < PreviewObjects.size() && (OutlinerPanelState.HiddenObjects.contains(PreviewObjects[Index].Id) || OutlinerPanelState.LockedObjects.contains(PreviewObjects[Index].Id)))
			{
				continue;
			}

			const auto Distance = HitTestPreviewCube(CursorRay, PreviewModels[Index] * GetPreviewBoundsMatrix(Index));
			if (Distance && *Distance < ClosestDistance)
			{
				ClosestObject = static_cast<int>(Index);
				ClosestDistance = *Distance;
			}

			if (bGameView || Index >= VisualEntities.size())
			{
				continue;
			}

			// The always-drawn wire boxes pick on their lines where they show, so a level-sized navigation volume does not swallow clicks on what is inside it.
			const float PickSlope = 6.f * ImGui::GetStyle().FontScaleDpi * Im3d::GetAppData().m_projScaleY / Im3d::GetAppData().m_viewportSize.y;
			const FLevelEntity& Visual = VisualEntities[Index];
			for (const std::optional<FVector3>& Size : {Visual.NavigationBounds ? std::optional{Visual.NavigationBounds->Size} : std::nullopt, Visual.NavigationObstacle ? std::optional{Visual.NavigationObstacle->Size} : std::nullopt, Visual.Trigger ? std::optional{Visual.Trigger->Size} : std::nullopt})
			{
				const auto Edge = Size ? HitTestPreviewCubeEdges(CursorRay, PreviewModels[Index] * FMatrix4::Scale(*Size * 0.5f), PickSlope) : std::nullopt;
				// A line lying on a surface draws over it, so it counts as a little nearer.
				if (Edge && *Edge - 0.05 < ClosestDistance)
				{
					ClosestObject = static_cast<int>(Index);
					ClosestDistance = std::max(*Edge - 0.05, 0.);
				}
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
		const FLevelBodyShape Bounds = GetPreviewBodyShape(static_cast<std::size_t>(std::max(PreviewSelection.Active, 0)));
		Im3d::PushMatrix(Model);
		Im3d::PushColor(Im3d::Color(0xefd07ccc));
		Im3d::DrawAlignedBox(ToIm3dVector(Bounds.Center - Bounds.HalfExtents), ToIm3dVector(Bounds.Center + Bounds.HalfExtents));
		Im3d::PopColor();
		Im3d::PopMatrix();
	}

	DrawPreviewVisuals(Level->GetWorld(), PreviewObjects, PreviewSelection, bGameView);

	// Each region's load ring, and its fainter unload ring, on the ground around where it is placed.
	if (!bGameView)
	{
		for (const FLevelRegion& Region : GetShownRegions())
		{
			const FVector3d& Offset = Region.Offset.Meters;
			const Im3d::Vec3 Center{static_cast<float>(Offset.X), static_cast<float>(Offset.Y), static_cast<float>(Offset.Z)};
			Im3d::PushColor(Im3d::Color(0x7cc4efdd));
			Im3d::PushSize(2.f);
			Im3d::DrawCircle(Center, {0.f, 1.f, 0.f}, Region.LoadRadius, 64);
			Im3d::PopSize();
			Im3d::PushAlpha(0.4f);
			Im3d::DrawCircle(Center, {0.f, 1.f, 0.f}, Region.UnloadRadius, 64);
			Im3d::PopAlpha();
			Im3d::PopColor();
			Im3d::DrawLine(Center, Center + Im3d::Vec3{0.f, 2.f, 0.f}, 2.f, Im3d::Color(0x7cc4efdd));
		}
	}

	if (!bGameView)
	{
		for (std::size_t Index = 0; Index < VisualEntities.size() && Index < PreviewObjects.size(); ++Index)
		{
			const FLevelEntity& Visual = VisualEntities[Index];
			const bool bSelected = PreviewSelection.Contains(static_cast<int>(Index));
			const Im3d::Mat4 EntityMatrix(PreviewObjects[Index].Translation, PreviewObjects[Index].Rotation, PreviewObjects[Index].Scale);
			// Navigation bounds stay faintly visible so the built area reads at a glance; obstacles show what they cut.
			for (const auto& [Size, Color] : {std::pair{Visual.NavigationBounds ? std::optional{Visual.NavigationBounds->Size} : std::nullopt, bSelected ? 0x8fe39cffu : 0x6fd38a66u}, std::pair{Visual.NavigationObstacle ? std::optional{Visual.NavigationObstacle->Size} : std::nullopt, bSelected ? 0xf0a35effu : 0xe08a3c99u}})
			{
				if (Size)
				{
					Im3d::PushMatrix(EntityMatrix);
					Im3d::PushColor(Im3d::Color(Color));
					Im3d::PushSize(2.f);
					Im3d::DrawAlignedBox(ToIm3dVector(*Size * -0.5f), ToIm3dVector(*Size * 0.5f));
					Im3d::PopSize();
					Im3d::PopColor();
					Im3d::PopMatrix();
				}
			}

			// A selected agent's field of view, on the ground at its sight range.
			if (Visual.AiAgent && bSelected)
			{
				const FAiAgentComponent& Agent = *Visual.AiAgent;
				const Im3d::Vec3 Center = PreviewObjects[Index].Translation + Im3d::Vec3{0.f, 0.05f, 0.f};
				const Im3d::Vec3 Forward = PreviewObjects[Index].Rotation * Im3d::Vec3{0.f, 0.f, 1.f};
				const float Facing = std::atan2(Forward.x, Forward.z);
				constexpr int Segments = 24;
				Im3d::PushColor(Im3d::Color(0xe9d36cff));
				Im3d::PushSize(2.f);
				Im3d::BeginLineStrip();
				Im3d::Vertex(Center);
				for (int Segment = 0; Segment <= Segments; ++Segment)
				{
					const float Angle = Facing - Agent.SightHalfAngle + 2.f * Agent.SightHalfAngle * static_cast<float>(Segment) / static_cast<float>(Segments);
					Im3d::Vertex(Center + Im3d::Vec3{std::sin(Angle), 0.f, std::cos(Angle)} * Agent.SightRange);
				}

				Im3d::Vertex(Center);
				Im3d::End();
				Im3d::PushAlpha(0.5f);
				Im3d::DrawCircle(Center, {0.f, 1.f, 0.f}, Agent.HearingRange, 48);
				Im3d::PopAlpha();
				Im3d::PopSize();
				Im3d::PopColor();
			}

			const auto& Trigger = Visual.Trigger;
			if (!Trigger)
			{
				continue;
			}

			// Occupied triggers turn green while simulating so enter and exit are visible without the log.
			const FVector3 Half = Trigger->Size * 0.5f;
			const bool bOccupied = Simulation.IsRunning() && Simulation.IsTriggerOccupied(Index);
			const Im3d::Color Color(bOccupied ? 0x6fd38aff : bSelected ? 0xc2b584ff
			                                                           : 0x5fb3d9cc);
			Im3d::PushMatrix(EntityMatrix);
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
	const bool bAuthoringAvailable = !bProjectBusy && !IsSimulating() && !Level->HasActiveEdit();
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

	if (ToolUI->BeginPanel("Viewport", nullptr, EToolUIPanelKind::Viewport))
	{
		const ImVec2 Size = ImGui::GetContentRegionAvail();
		const ImVec2 ImageMinimum = ImGui::GetCursorScreenPos();
		const std::optional<FToolUICanvasBounds> WorkspaceCanvas = ToolUI->GetWorkspaceCanvasForCurrentPanel();
		const ImVec2 RenderMinimum = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->X, WorkspaceCanvas->Y} : ImageMinimum;
		const ImVec2 RenderSize = WorkspaceCanvas ? ImVec2{WorkspaceCanvas->Width, WorkspaceCanvas->Height} : Size;
		const float Scale = ImGui::GetWindowViewport()->DpiScale;
		const float RenderScale = std::min(Scale, 4096.f / std::max({RenderSize.x, RenderSize.y, 1.f}));
		ViewportExtent = ViewportResolution.value_or(FExtent2D{.Width = static_cast<std::uint32_t>(std::max(RenderSize.x * RenderScale, 1.f)), .Height = static_cast<std::uint32_t>(std::max(RenderSize.y * RenderScale, 1.f))});

		if (Size.x > 0 && Size.y > 0 && RenderSize.x > 0 && RenderSize.y > 0)
		{
			ViewportProjectionCenter = GetViewportProjectionCenter({RenderMinimum.x, RenderMinimum.y}, {RenderSize.x, RenderSize.y}, {ImageMinimum.x, ImageMinimum.y}, {Size.x, Size.y});
			ViewportVisibleSize = {std::clamp(Size.x / RenderSize.x, 0.001f, 1.f), std::clamp(Size.y / RenderSize.y, 0.001f, 1.f)};
			ImDrawList* const PanelDrawList = ImGui::GetWindowDrawList();
			ImDrawListSplitter Layers;
			Layers.Split(PanelDrawList, 2);
			Layers.SetCurrentChannel(PanelDrawList, 1);
			const float ToolbarBottom = DrawViewportToolbar(ImageMinimum, Size);
			if (PlaySession)
			{
				DrawPlayHud(ImageMinimum, Size);
			}
			else if (!bGameView)
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
			if (bCameraReadoutVisible && !PlaySession)
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
			if (Assets && !bProjectBusy && !IsSimulating() && !Level->HasActiveEdit() && ImGui::BeginDragDropTarget())
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
					return Option.Id == *DroppedAsset && (IsPlaceableContentAsset(Option) || IsSoundContentAsset(Option) || IsEffectContentAsset(Option) || Option.Importer == "Material");
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
						// Sounds become emitters a little above where they land, so they are not buried in the floor.
						const auto Created = IsSoundContentAsset(*Asset)  ? Level->CreateSoundEntity(Asset->Id, Label.substr(0, Dot), {Position.X, Position.Y + 1., Position.Z})
						                     : IsEffectContentAsset(*Asset) ? Level->CreateEffectEntity(Asset->Id, Label.substr(0, Dot), {Position.X, Position.Y, Position.Z})
						                                                    : Level->CreateMeshEntity(Asset->Id, Label.substr(0, Dot), {Position.X, Position.Y, Position.Z});
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
				PanelDrawList->AddRect(Minimum, Maximum, PackColor(Outline), 0.f, HudScale);
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

			if (bCameraReadoutVisible && !PlaySession)
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
				ToolUIIcon(EToolUIMenuIcon::Camera, CameraIconCenter.x, CameraIconCenter.y, 0.9f * HudScale, CameraIconColor);
				HudDraw->AddText({CoordinatesPosition.x + 32.f * HudScale, CoordinatesTextY}, CameraIconColor, ImGui::GetTime() < CameraCoordinatesCopiedUntil ? "Copied" : "Camera");
				HudDraw->AddText({CoordinatesPosition.x + 40.f * HudScale + CameraLabelWidth, CoordinatesTextY}, PackColor(ToolUITheme::TextPrimary), Coordinates.c_str());
			}

			if (!bGameView && bCameraSpeedVisible && !PlaySession && ImGui::GetTime() < CameraSpeedShownUntil)
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
				ToolUIIcon(EToolUIMenuIcon::CameraSpeed, Center.x, Center.y - UiScale, 0.9f * UiScale, Muted);
				const float TextY = Center.y - ImGui::GetFontSize() * 0.5f;
				Draw->AddText({Position.x + 32.f * UiScale, TextY}, Muted, "Speed");
				Draw->AddText({Position.x + 40.f * UiScale + LabelWidth, TextY}, PackColor(ToolUITheme::TextPrimary), Speed.c_str());
			}

			// Convergence of the reference path tracer, with a thin bar along the pill's lower edge.
			if (!bGameView && !PlaySession && VisualSettings.bPathTracing && MeshRenderer && MeshRenderer->SupportsPathTracing())
			{
				const float UiScale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
				const std::uint32_t Samples = MeshRenderer->GetPathTracingSamples();
				const std::string Progress = Samples >= PathTracingSampleLimit ? std::format("{} spp, converged", Samples) : std::format("{} / {} spp", Samples, PathTracingSampleLimit);
				const float LabelWidth = ImGui::CalcTextSize("Path tracing").x;
				const float Width = LabelWidth + ImGui::CalcTextSize(Progress.c_str()).x + 36.f * UiScale;
				const float Height = 32.f * UiScale;
				const ImVec2 Position{ImageMinimum.x + std::round((Size.x - Width) * 0.5f), ImageMinimum.y + Size.y - Height - 14.f * UiScale};
				ToolUI->DrawGlassSurface(Position.x, Position.y, Width, Height, Height * 0.5f);
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				const float TextY = Position.y + (Height - ImGui::GetFontSize()) * 0.5f;
				Draw->AddText({Position.x + 14.f * UiScale, TextY}, PackColor(ToolUITheme::TextSecondary), "Path tracing");
				Draw->AddText({Position.x + 22.f * UiScale + LabelWidth, TextY}, PackColor(ToolUITheme::TextPrimary), Progress.c_str());
				const float Inset = Height * 0.5f;
				const float Fraction = std::min(1.f, static_cast<float>(Samples) / static_cast<float>(PathTracingSampleLimit));
				Draw->AddLine({Position.x + Inset, Position.y + Height - 3.f * UiScale}, {Position.x + Inset + (Width - 2.f * Inset) * Fraction, Position.y + Height - 3.f * UiScale}, IM_COL32(156, 211, 174, 255), 2.f * UiScale);
			}

			// Play shows only the game HUD.
			if (!bGameView && bOrientationIndicatorVisible && !PlaySession)
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
	SelectedIds.reserve(PreviewSelection.Indices.size());
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
	if (PreviewSelection.Active < 0 || IsSimulating() || ViewportInteraction.DragButton >= 0)
	{
		return;
	}

	bOutlinerOpen = true;
	OutlinerPanelState.bRenameRequested = true;
	ImGui::SetWindowFocus("Outliner");
}
}
