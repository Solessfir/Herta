#include "EditorFrameworkPrivate.h"

namespace Herta
{
std::optional<std::string> FEditorFramework::FImplementation::MountedContentPath(const std::filesystem::path& File) const
{
	std::error_code Error;
	const auto Absolute = std::filesystem::weakly_canonical(File, Error);
	for (const auto& [Mount, Root] : {std::pair{"Game/", AssetPaths.ContentRoot}, std::pair{"Engine/", AssetPaths.EngineContentRoot}})
	{
		const auto Relative = Absolute.lexically_relative(std::filesystem::weakly_canonical(Root, Error));
		const std::u8string Text = Relative.generic_u8string();
		if (!Root.empty() && !Relative.empty() && !Text.starts_with(u8".."))
		{
			return Mount + std::string(Text.begin(), Text.end());
		}
	}

	return std::nullopt;
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

	if (!MaterialGraphPanel.RequestClose() || !AnimationGraphPanel.RequestClose())
	{
		ProjectError = "Save or discard graph edits before changing projects.";
		return;
	}

	MaterialPanel.RequestClose();
	PendingMaterialOpen = {};
	if (bProjectBusy || !Tasks || IsSimulating() || Level->HasActiveEdit())
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
			StartAtFirstCameraBookmark();
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
	if (!Entity.SoftBody)
	{
		return nullptr;
	}

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

void FEditorFramework::FImplementation::RefreshVisuals()
{
	VisualSettings.ExposureEV100 = GetPhysicalCameraExposureEV100(PhysicalCamera);
	VisualSettings.bAutoExposure = bAutoExposure;
	if (!bExposureCompensationActive)
	{
		ExposureCompensation = Level->GetExposureCompensation();
	}

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
	ConstraintTargetIds.clear();
	ConstraintTargetLabels.clear();
	EnvironmentIds.clear();
	EnvironmentLabels.clear();
	SoundIds.clear();
	SoundLabels.clear();
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
					ShaderPaths.push_back(MaterialShaderKey(Source->ShaderPath, Option != Options.end() ? Option->Label : "Game/") + GetMaterialSwitchSuffix(*Source));
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
		if ((Entity.Mesh && Entity.BodyType != ELevelBodyType::None && !Entity.SoftBody) || (Entity.SoftBody && Entity.SoftBody->Shape == ESoftBodyShape::Rope && !Entity.Mesh))
		{
			ConstraintTargetIds.push_back(Entity.Id);
			ConstraintTargetLabels.push_back(Entity.Name);
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
			else if (Option.Importer == "Sound" || Option.Importer == "AudioGraph")
			{
				SoundIds.push_back(Option.Id);
				SoundLabels.push_back(Option.Label);
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
			ShaderPaths.push_back(MaterialShaderKey(MaterialPanel.GetDraft().ShaderPath, Option != Options.end() ? Option->Label : "Game/") + GetMaterialSwitchSuffix(MaterialPanel.GetDraft()));
		}

		if (MaterialGraphPanel.IsOpen())
		{
			ShaderPaths.push_back(MaterialGraphPanel.GetPath());
		}

		// An unsaved graph compiles from the editor's draft, so every material using it previews the edit; saving or discarding returns to the file.
		const bool bLiveGraph = MaterialShaders && MaterialGraphPanel.IsOpen() && MaterialGraphPanel.IsDirty();
		if (MaterialShaders && !LiveGraphPath.empty() && (!bLiveGraph || LiveGraphPath != MaterialGraphPanel.GetPath()))
		{
			MaterialShaders->SetGraphOverride(LiveGraphPath, std::nullopt);
			LiveGraphPath.clear();
		}

		if (bLiveGraph && (LiveGraphPath.empty() || LiveGraphRevision != MaterialGraphPanel.GetDraftRevision()))
		{
			LiveGraphPath = MaterialGraphPanel.GetPath();
			LiveGraphRevision = MaterialGraphPanel.GetDraftRevision();
			if (auto Text = SerializeGraph(MaterialGraphPanel.GetDraft(), GetMaterialGraphSchema()))
			{
				MaterialShaders->SetGraphOverride(LiveGraphPath, std::move(*Text));
			}
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
	if (Action == EAuthoringAction::None || bProjectBusy || IsSimulating() || Level->HasActiveEdit())
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
		else
		{
			StartAtFirstCameraBookmark();
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
}
