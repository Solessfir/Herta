#include "EditorFrameworkPrivate.h"

#include "Herta/ToolUI/EditorText.h"

namespace Herta
{
namespace
{
namespace Editor = Text::Editor;

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
}

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
	Implementation->Application = Descriptor.Application;
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
	Implementation->PollTextCatalogs(true);
	Implementation->Localization.SetCulture(Descriptor.ToolUI->GetAppearance().Culture);
	Descriptor.ToolUI->SetText(&Implementation->Localization);
	Implementation->RegisterUITexture = Descriptor.RegisterUITexture;
	Implementation->UnregisterUITexture = Descriptor.UnregisterUITexture;
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

			Implementation->StartAtFirstCameraBookmark();

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
		// The parkour course runs from the arena's front edge to the chimney 50 m up the middle.
		Implementation->ViewportCamera.Focus({0.f, 1.f, 22.f}, {12.f, 3.f, 26.f}, 16.f / 9.f);
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

void FEditorFramework::OpenMaterialGraph(const std::filesystem::path& File)
{
	if (const auto Key = Implementation->MountedContentPath(File))
	{
		Implementation->OpenMaterialGraphKey(*Key);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Material Graph {} is outside the project's and the engine's content", File.string());
}

void FEditorFramework::OpenAnimationGraph(const std::filesystem::path& File)
{
	if (const auto Key = Implementation->MountedContentPath(File))
	{
		Implementation->OpenAnimationGraphKey(*Key);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Animation graph {} is outside the project's and the engine's content", File.string());
}

void FEditorFramework::OpenAudioGraph(const std::filesystem::path& File)
{
	if (const auto Key = Implementation->MountedContentPath(File))
	{
		Implementation->OpenAudioGraphKey(*Key);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Audio Graph {} is outside the project's and the engine's content", File.string());
}

void FEditorFramework::OpenVfxGraph(const std::filesystem::path& File)
{
	if (const auto Key = Implementation->MountedContentPath(File))
	{
		Implementation->OpenVfxGraphKey(*Key);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "VFX Graph {} is outside the project's and the engine's content", File.string());
}

void FEditorFramework::OpenStateTree(const std::filesystem::path& File)
{
	if (const auto Key = Implementation->MountedContentPath(File))
	{
		Implementation->OpenStateTreeKey(*Key);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "StateTree {} is outside the project's and the engine's content", File.string());
}

void FEditorFramework::OpenMaterial(const std::filesystem::path& File)
{
	if (auto Label = Implementation->MountedContentPath(File))
	{
		Implementation->PendingMaterialPath = std::move(*Label);
		return;
	}

	HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Material {} is outside the project's and the engine's content", File.string());
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

std::expected<void, FEditorFrameworkError> FEditorFramework::SetScriptedPlay(const bool bPlaying)
{
	FImplementation& State = *Implementation;
	State.bScriptedPlay = bPlaying;
	State.ScriptedPlayFrame = 0;
	if (bPlaying != (State.PlaySession != nullptr))
	{
		State.TogglePlay();
	}

	if (bPlaying != (State.PlaySession != nullptr))
	{
		return std::unexpected(FEditorFrameworkError{bPlaying ? "Play could not start; see the log" : "Play could not stop"});
	}

	return {};
}

bool FEditorFramework::IsPlaying() const noexcept
{
	return Implementation->PlaySession != nullptr;
}

void FEditorFramework::SetPlayPaused(const bool bPaused) noexcept
{
	Implementation->bPlayPaused = bPaused && Implementation->PlaySession;
}

void FEditorFramework::SetRenderQuality(const ERenderQuality Quality) noexcept
{
	ApplyRenderQuality(Implementation->VisualSettings, Quality);
}

void FEditorFramework::SetPathTracing(const bool bEnabled) noexcept
{
	Implementation->VisualSettings.bPathTracing = bEnabled;
}

void FEditorFramework::SetManualExposure(const float ExposureValue) noexcept
{
	// EV100 = log2(N^2 / t) at ISO 100.
	Implementation->bAutoExposure = false;
	Implementation->PhysicalCamera.Iso = 100.f;
	Implementation->PhysicalCamera.ShutterSeconds = Implementation->PhysicalCamera.Aperture * Implementation->PhysicalCamera.Aperture / std::exp2(ExposureValue);
}

void FEditorFramework::MoveViewportCamera(const float Distance) noexcept
{
	FViewportCameraController& Camera = Implementation->ViewportCamera;
	const FVector3 Forward = Camera.MakePickingRay({0.5f, 0.5f}, 1.f).Direction;
	Camera.SetView(Camera.GetPosition() + Forward * Distance, Camera.GetYaw(), Camera.GetPitch());
}

void FEditorFramework::SetViewportResolution(const std::optional<FExtent2D> Resolution) noexcept
{
	Implementation->ViewportResolution = Resolution;
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
	Implementation->PollTextCatalogs(false);
	if (const std::string& Culture = Implementation->ToolUI->GetAppearance().Culture; Culture != Implementation->Localization.GetCultureName())
	{
		// A culture with no data leaves the text as it is; the menu only offers loaded ones.
		Implementation->Localization.SetCulture(Culture);
	}

	if (!IO.AppFocusLost && IO.KeyMods == ImGuiMod_Ctrl && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && ImGui::IsKeyPressed(ImGuiKey_Space, false))
	{
		if (Implementation->ToolUI->IsDocumentInFront())
		{
			Implementation->ToggleContentDrawer();
		}
		else
		{
			Implementation->SetBottomPanelOpen(!(Implementation->bBottomPanelOpen && (Implementation->bContentBrowserOpen || Implementation->bOutputLogOpen)));
		}
	}

	if (!IO.AppFocusLost && !IO.WantTextInput && IO.KeyMods == 0 && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		if (!Implementation->ToolUI->IsDocumentInFront() && ImGui::IsKeyPressed(ImGuiKey_F11, false))
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
		const auto Destination = Implementation->ToolUI->IsPanelHovered("Content Browser") || Implementation->ToolUI->IsPanelHovered("Content Drawer") ? Implementation->ContentBrowserState.GetImportDestination() : std::string{};
		auto DroppedFiles = Implementation->ToolUI->TakeDroppedFiles();
		if (!Implementation->bProjectBusy)
		{
			Implementation->Assets->ImportFiles(std::move(DroppedFiles), Destination);
		}
		else if (!DroppedFiles.empty())
		{
			HERTA_LOG_WARNING(*Implementation->Log, EditorLog, "Wait for the project to finish loading before importing files");
		}
		Implementation->Assets->RequestPlayerCharacter(Implementation->Level ? Implementation->Level->GetPlayerCharacter() : FAssetId{});
		Implementation->Assets->Tick();
	}

	Implementation->RefreshPreviewMeshes();
	Implementation->UpdateNavigation();

	// Esc stops play like in other editors; Shift+Esc is the game's own Esc and opens its pause menu.
	if (!IO.AppFocusLost && Implementation->PlaySession && !IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		Implementation->TogglePlay();
	}
	else if (!IO.AppFocusLost && Implementation->Simulation.IsRunning() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		Implementation->ToggleSimulation();
	}

	// Shift+Space only starts play, because sprinting players press it to jump.
	if (!Implementation->bProjectBusy && !IO.AppFocusLost && !IO.WantTextInput && ((IO.KeyMods == ImGuiMod_Alt && ImGui::IsKeyPressed(ImGuiKey_P, false)) || (!Implementation->PlaySession && IO.KeyMods == ImGuiMod_Shift && ImGui::IsKeyPressed(ImGuiKey_Space, false))) && !Implementation->Simulation.IsRunning() && !Implementation->Level->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		Implementation->TogglePlay();
	}

	Implementation->UpdatePlay(IO.DeltaTime);
	Implementation->UpdateAudio(IO.DeltaTime);
	Implementation->UpdateEffects(IO.DeltaTime);

	if (!Implementation->bProjectBusy && !IO.AppFocusLost && !IO.WantTextInput && IO.KeyAlt && !IO.KeyCtrl && !IO.KeyShift && !IO.KeySuper && ImGui::IsKeyPressed(ImGuiKey_S, false) && !Implementation->IsSimulating() && !Implementation->Level->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
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
	const bool bAuthoringAvailable = !Implementation->bProjectBusy && !Implementation->IsSimulating() && !Implementation->Level->HasActiveEdit() && Implementation->ViewportInteraction.DragButton < 0;
	const bool bShortcutsAvailable = bAuthoringAvailable && !Implementation->ToolUI->IsDocumentInFront() && !Implementation->MaterialPanel.IsFocused() && !Implementation->MaterialGraphPanel.IsFocused() && !Implementation->AnimationGraphPanel.IsFocused() && !Implementation->AudioGraphPanel.IsFocused() && !Implementation->VfxGraphPanel.IsFocused() && !Implementation->StateTreePanel.IsFocused() && !IO.AppFocusLost && !IO.WantTextInput && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !IO.KeyAlt && !IO.KeySuper;
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

	const FLocalization& Localization = Implementation->Localization;
	const std::string LevelTitle = std::format("{}{} - Herta Editor", Implementation->Level->GetName(), Implementation->Level->IsDirty() ? "*" : "");
	Implementation->ToolUI->SetLevelTabLabel(std::format("{}{}", Implementation->Level->GetName(), Implementation->Level->IsDirty() ? "*" : ""));
	Implementation->ToolUI->DrawWorkspace(LevelTitle, [&]
	{
		ToolUIMenuItem("Outliner", EToolUIMenuIcon::Outliner, &Implementation->bOutlinerOpen);
		ToolUIMenuItem("Details", EToolUIMenuIcon::Details, &Implementation->bDetailsOpen);
		bool bBottomVisible = Implementation->bBottomPanelOpen && (Implementation->bContentBrowserOpen || Implementation->bOutputLogOpen);
		if (ToolUIMenuItem(Localization.Format(Editor::Menu_BottomPanel), EToolUIMenuIcon::Panel, &bBottomVisible, "Ctrl+Space"))
		{
			Implementation->SetBottomPanelOpen(bBottomVisible);
		}
		const bool bDocumentInFront = Implementation->ToolUI->IsDocumentInFront();
		bool bBrowserVisible = bDocumentInFront ? Implementation->bContentDrawerOpen : Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen;
		if (ToolUIMenuItem("Content Browser", EToolUIMenuIcon::ContentBrowser, &bBrowserVisible))
		{
			if (bDocumentInFront)
			{
				Implementation->ToggleContentDrawer();
			}
			else
			{
				Implementation->bContentBrowserOpen = bBrowserVisible;
				if (bBrowserVisible)
				{
					Implementation->bBottomPanelOpen = true;
					Implementation->ContentBrowserState.bFocusRequested = true;
				}
			}
		}

		ImGui::Separator();
		bool bLogVisible = bDocumentInFront ? Implementation->bOutputLogDrawerOpen : Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen;
		if (ToolUIMenuItem("Output Log", EToolUIMenuIcon::Log, &bLogVisible))
		{
			if (bDocumentInFront)
			{
				Implementation->ToggleOutputLogDrawer();
			}
			else
			{
				Implementation->bOutputLogOpen = bLogVisible;
				Implementation->bBottomPanelOpen |= bLogVisible;
			}
		}

		ToolUIMenuItem("Performance", EToolUIMenuIcon::Performance, &Implementation->bPerformanceOpen);
		ToolUIMenuItem("Localization", EToolUIMenuIcon::Details, &Implementation->bLocalizationOpen);
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

		const bool bDocumentInFront = Implementation->ToolUI->IsDocumentInFront();
		const bool bBrowserShown = bDocumentInFront ? Implementation->bContentDrawerOpen : Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen;
		ImGui::PushStyleColor(ImGuiCol_Button, bBrowserShown ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
		if (ToolUIButton("Content Browser", EToolUIMenuIcon::ContentBrowser, 26.f * Scale))
		{
			if (bDocumentInFront)
			{
				Implementation->ToggleContentDrawer();
			}
			else
			{
				ToggleContentBrowser(Implementation->bContentBrowserOpen, Implementation->ContentBrowserState);
				Implementation->bBottomPanelOpen = true;
			}
		}

		ImGui::PopStyleColor();
		ImGui::SetItemTooltip(bDocumentInFront ? "Show or hide the content drawer (Ctrl+Space)." : "Show or hide Content Browser. Ctrl+Space toggles the entire bottom panel.");
		ImGui::SameLine();
		const bool bLogShown = bDocumentInFront ? Implementation->bOutputLogDrawerOpen : Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen;
		ImGui::PushStyleColor(ImGuiCol_Button, bLogShown ? ImVec4{1, 1, 1, 0.08f} : ImVec4{1, 1, 1, 0.04f});
		if (ToolUIButton("Output Log", EToolUIMenuIcon::Log, 26.f * Scale))
		{
			if (bDocumentInFront)
			{
				Implementation->ToggleOutputLogDrawer();
			}
			else
			{
				Implementation->bOutputLogOpen = !(Implementation->bBottomPanelOpen && Implementation->bOutputLogOpen);
				Implementation->bBottomPanelOpen = true;
			}
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
		ImGui::TextUnformatted(Localization.Format(Editor::Menu_Project).c_str());
		ImGui::Separator();
		ImGui::BeginDisabled(!bProjectChangeAvailable);
		bNewProjectRequested |= ToolUIMenuItem(Localization.Format(Editor::Menu_NewProject), EToolUIMenuIcon::Add, nullptr, "Ctrl+N");
		bOpenProjectRequested |= ToolUIMenuItem(Localization.Format(Editor::Menu_OpenProject), EToolUIMenuIcon::Open, nullptr, "Ctrl+O");
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextUnformatted(Localization.Format(Editor::Menu_Level).c_str());
		ImGui::Separator();
		ImGui::BeginDisabled(!bAuthoringAvailable);
		bOpenLevelRequested = ToolUIMenuItem(Localization.Format(Editor::Menu_OpenLevel), EToolUIMenuIcon::Open);
		bSaveLevelRequested |= ToolUIMenuItem(Localization.Format(Editor::Menu_SaveLevel), EToolUIMenuIcon::Save, nullptr, "Ctrl+S");
		ImGui::EndDisabled();
		ImGui::Spacing();
		ImGui::TextUnformatted(Localization.Format(Editor::Menu_Content).c_str());
		ImGui::Separator();
		ImGui::BeginDisabled(!Implementation->Assets || Implementation->bProjectBusy);
		bImportRequested |= ToolUIMenuItem(Localization.Format(Editor::Menu_Import), EToolUIMenuIcon::Import, nullptr, "Ctrl+I");
		ImGui::EndDisabled();
	}, [&]
	{
		ImGui::BeginDisabled(!bAuthoringAvailable);
		ImGui::BeginDisabled(!Implementation->Level->CanUndo());
		const std::string UndoLabel = Implementation->Level->CanUndo() ? Localization.Format(Editor::Menu_UndoAction, std::array{FMessageArgument{"action", std::string(Implementation->Level->GetUndoLabel())}}) : Localization.Format(Editor::Menu_Undo);
		if (ToolUIMenuItem(UndoLabel, EToolUIMenuIcon::Undo, nullptr, "Ctrl+Z"))
		{
			AuthoringAction = EAuthoringAction::Undo;
		}

		ImGui::EndDisabled();
		ImGui::BeginDisabled(!Implementation->Level->CanRedo());
		const std::string RedoLabel = Implementation->Level->CanRedo() ? Localization.Format(Editor::Menu_RedoAction, std::array{FMessageArgument{"action", std::string(Implementation->Level->GetRedoLabel())}}) : Localization.Format(Editor::Menu_Redo);
		if (ToolUIMenuItem(RedoLabel, EToolUIMenuIcon::Redo, nullptr, "Ctrl+Y"))
		{
			AuthoringAction = EAuthoringAction::Redo;
		}

		ImGui::EndDisabled();
		ImGui::Separator();
		ImGui::BeginDisabled(Implementation->PreviewSelection.Indices.empty());
		for (const auto& [Label, Icon, Shortcut, Action] : std::array{
		         std::tuple{Editor::Menu_Duplicate, EToolUIMenuIcon::Duplicate, "Ctrl+D", EAuthoringAction::Duplicate},
		         std::tuple{Editor::Menu_Copy, EToolUIMenuIcon::Copy, "Ctrl+C", EAuthoringAction::Copy},
		         std::tuple{Editor::Menu_Delete, EToolUIMenuIcon::Delete, "Delete", EAuthoringAction::Delete},
		     })
		{
			if (ToolUIMenuItem(Localization.Format(Label), Icon, nullptr, Shortcut))
			{
				AuthoringAction = Action;
			}
		}

		ImGui::EndDisabled();
		if (ToolUIMenuItem(Localization.Format(Editor::Menu_Paste), EToolUIMenuIcon::Paste, nullptr, "Ctrl+V"))
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
	const bool bDocumentInFront = Implementation->ToolUI->IsDocumentInFront();
	Implementation->bContentDrawerOpen &= bDocumentInFront;
	const bool bBrowserShown = bDocumentInFront ? Implementation->bContentDrawerOpen : Implementation->bBottomPanelOpen && Implementation->bContentBrowserOpen;
	if (!bBrowserShown && Implementation->Assets)
	{
		Implementation->Assets->SetThumbnailAssets({});
	}

	if (bBrowserShown)
	{
		auto* const Assets = Implementation->Assets.get();
		ImGui::BeginDisabled(Implementation->bProjectBusy);
		if (DrawContentBrowser(*Implementation->ToolUI, bDocumentInFront ? Implementation->bContentDrawerOpen : Implementation->bContentBrowserOpen, Implementation->ContentBrowserState, Assets, bDocumentInFront) && Assets)
		{
			Implementation->ImportWithDialog();
		}

		ImGui::EndDisabled();
	}

	Implementation->DrawViewport(RenderViewport);
	Implementation->DrawMaterialPanel();
	Implementation->DrawAnimationGraphPanel();
	Implementation->DrawAudioGraphPanel();
	Implementation->DrawVfxGraphPanel();
	Implementation->DrawStateTreePanel();
	// Document panels are named "<file>###<kind>", so the kind alone, such as StateTree, finds them.
	if (const std::string Focus = std::exchange(Implementation->PendingFocusPanel, std::string()); !Focus.empty())
	{
		// A name that matches no window is ignored, so both spellings can be tried.
		ImGui::SetWindowFocus(Focus.c_str());
		ImGui::SetWindowFocus(("###" + Focus).c_str());
	}
	Implementation->DrawPerformancePanel();
	Implementation->DrawLocalizationPanel();
	if (!Implementation->IsSimulating())
	{
		if (Implementation->bViewportEditCanceled && Implementation->Level->HasActiveEdit())
		{
			Implementation->ReportLevelResult(Implementation->Level->CancelEdit());
		}
		else if (Implementation->Level->HasActiveEdit() && (Implementation->bViewportEditFinished || (!Implementation->PreviewDragStart && !ImGui::IsAnyItemActive() && !IO.WantTextInput && !Implementation->OutlinerPanelState.bRenaming)))
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

	// Unsaved graph edits stop the close with a save prompt; closing again after answering it proceeds.
	if (!Implementation->MaterialGraphPanel.RequestClose() || !Implementation->AnimationGraphPanel.RequestClose() || !Implementation->AudioGraphPanel.RequestClose() || !Implementation->VfxGraphPanel.RequestClose() || !Implementation->StateTreePanel.RequestClose())
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

FViewportVisibleRegion FEditorFramework::GetViewportVisibleRegion() const noexcept
{
	const FImplementation& Editor = *Implementation;
	const FExtent2D Full = Editor.ViewportExtent;
	const auto Width = std::clamp(static_cast<std::uint32_t>(static_cast<float>(Full.Width) * Editor.ViewportVisibleSize.X), 1u, std::max(Full.Width, 1u));
	const auto Height = std::clamp(static_cast<std::uint32_t>(static_cast<float>(Full.Height) * Editor.ViewportVisibleSize.Y), 1u, std::max(Full.Height, 1u));
	const float Left = Editor.ViewportProjectionCenter.X * static_cast<float>(Full.Width) - static_cast<float>(Width) * 0.5f;
	const float Top = Editor.ViewportProjectionCenter.Y * static_cast<float>(Full.Height) - static_cast<float>(Height) * 0.5f;
	return {
	    .X = std::min(static_cast<std::uint32_t>(std::max(Left, 0.f)), Full.Width - std::min(Width, Full.Width)),
	    .Y = std::min(static_cast<std::uint32_t>(std::max(Top, 0.f)), Full.Height - std::min(Height, Full.Height)),
	    .Extent = {.Width = Width, .Height = Height},
	};
}

FMeshRenderView FEditorFramework::GetViewportRenderView() const noexcept
{
	// The renderer follows the swapchain that will present this frame, not the preference, which may be waiting for an HDR display.
	FMeshRenderView View = Implementation->PlaySession ? Implementation->PlayRenderView : Implementation->ViewportRenderView;
	if (!Implementation->PlaySession)
	{
		View.Particles = Implementation->PreviewParticles.GetRenderParticles();
		View.Ribbons = Implementation->PreviewParticles.GetRenderRibbons();
		View.RibbonPoints = Implementation->PreviewParticles.GetRenderRibbonPoints();
	}

	const FEditorDisplayState& Display = Implementation->DisplayState;
	if (Display.bHdrActive)
	{
		View.Visuals.HdrDisplay = FHdrDisplaySettings{.PaperWhite = Display.PaperWhite, .PeakLuminance = Display.PeakLuminance, .bCalibrationPattern = Implementation->bHdrCalibration};
	}

	return View;
}

std::span<const FDebugDrawList> FEditorFramework::GetViewportDebugDrawLists() const noexcept
{
	// Play shows the game as a player sees it, without editor overlays other than an explicitly enabled collision view.
	if (!Implementation->CombinedDebugDrawLists.empty())
	{
		return Implementation->CombinedDebugDrawLists;
	}

	return Implementation->PlaySession ? std::span<const FDebugDrawList>{} : std::span<const FDebugDrawList>(Implementation->ViewportDebugDrawLists);
}
}
