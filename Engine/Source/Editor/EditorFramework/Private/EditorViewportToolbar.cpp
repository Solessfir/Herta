#include "EditorFrameworkPrivate.h"

namespace Herta
{
void FEditorFramework::FImplementation::DrawViewportStats(const ImVec2 Minimum, const ImVec2 Size, const float ToolbarBottom)
{
	if (!Stats->bUnitVisible && !Stats->bFpsVisible)
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	const float Width = 190.f * Scale;
	const float RowHeight = ImGui::GetTextLineHeight() + 4.f * Scale;
	const float Height = RowHeight * static_cast<float>((Stats->bUnitVisible ? 3 : 0) + (Stats->bFpsVisible ? 1 : 0)) + 16.f * Scale;
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

	const auto IconButton = [&](const char* Id, const EToolUIMenuIcon Icon, const char* Tooltip, const bool bSelected = false)
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
	// Play keeps only the playback island; editing tools and time of day would cover the game.
	float RightControlsLeft = PlaySession ? Minimum.x + Size.x : SettingsX;
	if (!PlaySession)
	{
		Island(SettingsX, Height);

		if (IconButton("Viewport settings", EToolUIMenuIcon::Settings, "Viewport settings"))
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

				if (IconButton("Focus", EToolUIMenuIcon::Focus, "Focus selected object (F)"))
				{
					FocusPreview();
				}

				Right -= Height + Gap;
			}

			const float SnapWidth = (bSnapEnabled ? ViewportIconButtonSize + 75.f : Height / Scale) * Scale;
			Island(Right - SnapWidth, SnapWidth);

			if (IconButton("Grid snap", EToolUIMenuIcon::Grid, "Toggle grid snapping (S)", bSnapEnabled))
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

				if (IconButton("Coordinate space", EToolUIMenuIcon::World, bLocalGizmo ? "Local axes (L)" : "World axes (L)", bLocalGizmo))
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

			if (IconButton("Hide gizmo", EToolUIMenuIcon::Select, "Select / hide gizmo (Q)", !bTransformGizmoVisible))
			{
				bTransformGizmoVisible = false;
				ViewportGizmos.resetId();
			}

			constexpr std::array Modes{std::pair{Im3d::GizmoMode_Translation, EToolUIMenuIcon::Move}, std::pair{Im3d::GizmoMode_Rotation, EToolUIMenuIcon::Rotate}, std::pair{Im3d::GizmoMode_Scale, EToolUIMenuIcon::Scale}};
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
	}

	const float PlayWidth = 2.f * ButtonSize + ImGui::GetStyle().ItemSpacing.x + 2.f * Padding;
	const float PlayX = Minimum.x + (Size.x - PlayWidth) * 0.5f;
	const bool bShowPlayControls = Size.x > 180.f * Scale;
	if (bShowPlayControls)
	{
		const bool bFitsTopRow = CanFitViewportToolbarIsland(PlayX, PlayWidth, Minimum.x, RightControlsLeft, Gap);
		Island(PlayX, PlayWidth, bFitsTopRow ? 0.f : Height + Gap);
		ImGui::BeginDisabled(bProjectBusy || Simulation.IsRunning());
		if (IconButton("Play", PlaySession ? EToolUIMenuIcon::Stop : EToolUIMenuIcon::Play, PlaySession ? "Stop playing (Esc)" : "Play (Alt+P or Shift+Space)", PlaySession != nullptr))
		{
			TogglePlay();
		}

		ImGui::EndDisabled();
		ImGui::SameLine();

		ImGui::BeginDisabled(bProjectBusy || PlaySession != nullptr);
		if (IconButton("Simulate", Simulation.IsRunning() ? EToolUIMenuIcon::Stop : EToolUIMenuIcon::Simulate, Simulation.IsRunning() ? "Stop simulation (Esc)" : "Simulate (Alt+S)", Simulation.IsRunning()))
		{
			ToggleSimulation();
		}

		ImGui::EndDisabled();
	}

	// Top-left island; it yields to the playback island and right-hand controls instead of overlapping them.
	if (bTimeOfDayVisible && !PlaySession && FindTimeOfDaySun() >= 0)
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
		ImGui::PushStyleColor(ImGuiCol_Separator, {1.f, 1.f, 1.f, 0.08f});
		ImGui::PushStyleColor(ImGuiCol_Header, {0.f, 0.f, 0.f, 0.f});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 6.f * Scale});
		const float ValueWidth = 116.f * Scale;

		// Short pages instead of one long list: each shows what one kind of change needs.
		constexpr std::array<const char*, 4> Pages{"View", "Camera", "Gizmo", "Rendering"};
		const float PageWidth = (ImGui::GetContentRegionAvail().x - 3.f * 2.f * Scale) / static_cast<float>(Pages.size());
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f * Scale);
		for (std::size_t Page = 0; Page < Pages.size(); ++Page)
		{
			const bool bCurrent = ViewportSettingsPage == Page;
			ImGui::PushStyleColor(ImGuiCol_Button, bCurrent ? ImVec4{1.f, 1.f, 1.f, 0.14f} : ImVec4{0.f, 0.f, 0.f, 0.f});
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(bCurrent ? ImGuiCol_Text : ImGuiCol_TextDisabled));
			if (Page > 0)
			{
				ImGui::SameLine(0.f, 2.f * Scale);
			}

			if (ImGui::Button(Pages[Page], {PageWidth, 0.f}))
			{
				ViewportSettingsPage = Page;
			}

			ImGui::PopStyleColor(2);
		}

		ImGui::PopStyleVar();
		ImGui::Spacing();
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

		if (ViewportSettingsPage == 0)
		{
			DrawFieldLabel("Editor volume", ValueWidth);
			float VolumePercent = EditorVolume * 100.f;
			if (DrawNumericSliderFloat("##EditorVolume", &VolumePercent, 0.f, 100.f, VolumePercent > 0.f ? "%.0f%%" : "Muted", ImGuiSliderFlags_AlwaysClamp))
			{
				EditorVolume = VolumePercent / 100.f;
			}

			ImGui::SetItemTooltip("Everything the editor plays: Play, auditions, and level emitters");

			if (Toggle("Game view", bGameView, "G"))
			{
				ViewportGizmos.resetId();
			}

			Toggle("Grid", bGridVisible);
			Toggle("Collision", bCollisionVisible);
			Toggle("Navigation", bNavigationVisible);
			Toggle("World axes", bAxesVisible);
			Toggle("Corner axis indicator", bOrientationIndicatorVisible);
			Toggle("Preview bounds", bBoundsVisible);
			Toggle("Camera coordinates", bCameraReadoutVisible);
			Toggle("Camera speed", bCameraSpeedVisible);
			Toggle("Time of day", bTimeOfDayVisible);
		}

		if (ViewportSettingsPage == 1)
		{
			float MovementSpeed = ViewportCamera.GetMovementSpeed();
			DrawFieldLabel("Speed", ValueWidth);

			if (DrawNumericSliderFloat("##CameraSpeed", &MovementSpeed, 0.1f, 100.f, "%.1f m/s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
			{
				ViewportCamera.SetMovementSpeed(MovementSpeed);
				CameraSpeedShownUntil = ImGui::GetTime() + 1.5;
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

		if (ViewportSettingsPage == 2)
		{
			if (!bToolbarShowsModes)
			{
				if (IconButton("Select", EToolUIMenuIcon::Select, "Select / hide gizmo (Q)", !bTransformGizmoVisible))
				{
					bTransformGizmoVisible = false;
					ViewportGizmos.resetId();
				}

				constexpr std::array SettingsModes{std::pair{Im3d::GizmoMode_Translation, EToolUIMenuIcon::Move}, std::pair{Im3d::GizmoMode_Rotation, EToolUIMenuIcon::Rotate}, std::pair{Im3d::GizmoMode_Scale, EToolUIMenuIcon::Scale}};
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

				if (IconButton("Focus", EToolUIMenuIcon::Focus, "Focus selected object (F)"))
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

		if (ViewportSettingsPage == 2)
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

		if (ViewportSettingsPage == 3)
		{
			DrawFieldLabel("Quality", ValueWidth);
			constexpr std::array Tiers{"Low", "Medium", "High"};
			const std::optional<ERenderQuality> Tier = FindRenderQuality(VisualSettings);
			if (ImGui::BeginCombo("##RenderQuality", Tier ? Tiers[static_cast<std::size_t>(*Tier)] : "Custom"))
			{
				for (std::size_t Index = 0; Index < Tiers.size(); ++Index)
				{
					if (ImGui::Selectable(Tiers[Index], Tier && Index == static_cast<std::size_t>(*Tier)))
					{
						ApplyRenderQuality(VisualSettings, static_cast<ERenderQuality>(Index));
					}
				}

				ImGui::EndCombo();
			}

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

				DrawFieldLabel("Ambient occlusion", ValueWidth);
				constexpr std::array OcclusionModes{"Off", "GTAO Low", "GTAO Medium", "GTAO High"};
				if (ImGui::BeginCombo("##AmbientOcclusion", OcclusionModes[static_cast<std::size_t>(VisualSettings.AmbientOcclusion)]))
				{
					for (std::size_t Index = 0; Index < OcclusionModes.size(); ++Index)
					{
						if (ImGui::Selectable(OcclusionModes[Index], Index == static_cast<std::size_t>(VisualSettings.AmbientOcclusion)))
						{
							VisualSettings.AmbientOcclusion = static_cast<EAmbientOcclusion>(Index);
						}
					}

					ImGui::EndCombo();
				}

				Toggle("Contact shadows", VisualSettings.bContactShadows);
				DrawFieldLabel("Penumbra saturation", ValueWidth);
				DrawNumericSliderFloat("##PenumbraSaturation", &VisualSettings.PenumbraSaturation, 1.f, 4.f, VisualSettings.PenumbraSaturation > 1.f ? "%.2fx" : "Off", ImGuiSliderFlags_AlwaysClamp);
				DrawFieldLabel("Bloom", ValueWidth);
				float BloomPercent = VisualSettings.BloomIntensity * 100.f;
				if (DrawNumericSliderFloat("##Bloom", &BloomPercent, 0.f, 20.f, BloomPercent > 0.f ? "%.1f%%" : "Off", ImGuiSliderFlags_AlwaysClamp))
				{
					VisualSettings.BloomIntensity = BloomPercent / 100.f;
				}

				Toggle("Occlusion culling", VisualSettings.bOcclusionCulling);
				if (MeshRenderer->SupportsPathTracing())
				{
					Toggle("Global illumination", VisualSettings.bGlobalIllumination);
					Toggle("Path tracing", VisualSettings.bPathTracing);
				}
			}
		}

		ImGui::Spacing();
		ImGui::BeginDisabled(IsSimulating() || Level->HasActiveEdit() || PreviewSelection.Active < 0);

		if (ViewportSettingsPage == 2 && ImGui::Button("Reset preview transform", {-1.f, 0.f}))
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

// A level opens at its first camera bookmark, which frames it as its author intended; the default camera often lands inside large levels' walls.
void FEditorFramework::FImplementation::StartAtFirstCameraBookmark()
{
	const auto Bookmarks = Level->GetCameraBookmarks();
	if (const auto First = std::ranges::min_element(Bookmarks, {}, &FLevelCameraBookmark::Slot); First != Bookmarks.end())
	{
		JumpToCameraBookmark(First->Slot);
	}
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
}
