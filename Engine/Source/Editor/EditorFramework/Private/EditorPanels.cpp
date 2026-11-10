#include "EditorFrameworkPrivate.h"

namespace Herta
{
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
	ImGui::BeginDisabled(IsSimulating() || bProjectBusy);
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

	// A material named on the command line opens once the content scan lists it.
	if (!PendingMaterialPath.empty())
	{
		const auto Options = Assets->GetOptions();
		if (const auto Found = std::ranges::find(Options, PendingMaterialPath, &FPreviewAssetOption::Label); Found != Options.end())
		{
			ContentBrowserState.OpenMaterialRequested = Found->Id;
			PendingMaterialPath.clear();
		}
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
	std::span<const FMaterialShaderParameter> GraphParameters;
	std::span<const FMaterialShaderParameter> GraphSwitches;
	if (GetBuildConfiguration() == EBuildConfiguration::Shipping)
	{
		ShaderStatus = MaterialPanel.GetDraft().ShaderPath.empty() ? "Shader iteration is unavailable in Shipping." : "Custom shaders are unavailable in Shipping. Using engine PBR.";
	}
	else if (MaterialShaders && MaterialPanel.IsOpen())
	{
		const auto Options = Assets->GetOptions();
		const auto Option = std::ranges::find(Options, MaterialPanel.GetAsset(), &FPreviewAssetOption::Id);
		const std::string Key = MaterialShaderKey(MaterialPanel.GetDraft().ShaderPath, Option != Options.end() ? Option->Label : "Game/") + GetMaterialSwitchSuffix(MaterialPanel.GetDraft());
		if (const auto* Status = MaterialShaders->GetStatus(Key))
		{
			ShaderStatus = Status->bCompiling ? "Compiling shader..." : !Status->Diagnostics.empty() ? Status->Diagnostics
			                                                        : Status->bReady                 ? "Shader ready"
			                                                                                         : "Waiting for shader";
			GraphParameters = Status->Parameters;
			GraphSwitches = Status->Switches;
		}
	}

	const auto ZoomMaterialPreview = [this](const float Wheel)
	{
		MaterialPreviewZoom = std::clamp(MaterialPreviewZoom * std::pow(1.15f, Wheel), 0.5f, 4.f);
		Assets->SetMaterialPreviewZoom(MaterialPreviewZoom);
	};

	const FMaterialPanelContext Context{
	    .Textures = MaterialTextureOptions,
	    .PreviewTexture = Assets->GetMaterialPreviewAsset() == MaterialPanel.GetAsset() && Assets->GetMaterialPreviewThumbnail() ? *Assets->GetMaterialPreviewThumbnail() : DraftMaterialThumbnail ? *DraftMaterialThumbnail
	                                                                                                                                                                                               : 0,
	    .PreviewStatus = Assets->IsMaterialPreviewLoading() ? "Updating preview..." : Assets->GetMaterialPreviewError(),
	    .ZoomPreview = ZoomMaterialPreview,
	    .ShaderStatus = ShaderStatus,
	    .GraphParameters = GraphParameters,
	    .GraphSwitches = GraphSwitches,
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
	    .CreateGraph = [&](const FAssetId Id) -> std::expected<std::string, FAssetError>
	{
		return CreateMaterialGraph(Id);
	},
	    .OpenGraph = [&](const FAssetId Id, const std::string_view ShaderPath)
	{
		OpenMaterialGraph(Id, ShaderPath);
	},
	};
	MaterialPanel.Draw(*ToolUI, Context);

	std::string GraphStatus;
	if (MaterialShaders && MaterialGraphPanel.IsOpen())
	{
		if (const auto* Status = MaterialShaders->GetStatus(MaterialGraphPanel.GetPath()))
		{
			GraphStatus = Status->bCompiling ? "Compiling shader..." : !Status->Diagnostics.empty() ? Status->Diagnostics
			                                                       : Status->bReady                 ? "Shader ready"
			                                                                                        : "Saved graphs compile in the background";
		}
	}

	ResolveGraphPreview();
	const auto* const GraphShader = MaterialShaders && MaterialGraphPanel.IsOpen() ? MaterialShaders->GetStatus(MaterialGraphPanel.GetPath()) : nullptr;
	const bool bGraphPreviewShown = GraphPreviewMaterial && Assets->GetMaterialPreviewAsset() == GraphPreviewAsset;
	MaterialGraphPanel.Draw(*ToolUI, {
	                                     .Status = GraphStatus,
	                                     .PreviewTexture = bGraphPreviewShown && Assets->GetMaterialPreviewThumbnail() ? *Assets->GetMaterialPreviewThumbnail() : 0,
	                                     .PreviewStatus = bGraphPreviewPending ? "" : !GraphPreviewMaterial                                  ? "No material beside this graph uses it, so there is nothing to preview."
	                                                                              : bGraphPreviewShown && Assets->IsMaterialPreviewLoading() ? "Updating preview..."
	                                                                              : bGraphPreviewShown                                       ? Assets->GetMaterialPreviewError()
	                                                                                                                                         : "",
	                                     .RequestPreview = [&]
	{
		// The preview slot is shared with the Material panel, so take it back when it shows something else, and refresh once a recompiled graph has published.
		const std::uint64_t Generation = GraphShader && GraphShader->bReady && !GraphShader->bCompiling ? GraphShader->Generation : GraphPreviewGeneration;
		if (GraphPreviewMaterial && (Assets->GetMaterialPreviewAsset() != GraphPreviewAsset || Generation != GraphPreviewGeneration))
		{
			Assets->RequestMaterialPreview(GraphPreviewAsset, *GraphPreviewMaterial);
			GraphPreviewGeneration = Generation;
		}
	},
	                                     .ZoomPreview = ZoomMaterialPreview,
	                                 });
}

void FEditorFramework::FImplementation::DrawAnimationGraphPanel()
{
	if (!AnimationGraphPanel.IsOpen())
	{
		return;
	}

	if (AnimationGraphRevision != AnimationGraphPanel.GetDraftRevision())
	{
		AnimationGraphRevision = AnimationGraphPanel.GetDraftRevision();
		const auto Compiled = CompileAnimationGraph(AnimationGraphPanel.GetDraft());
		AnimationStateNodes = Compiled ? Compiled->StateNodes : std::vector<FGraphNodeId>{};
		AnimationTransitionNodes = Compiled ? Compiled->TransitionNodes : std::vector<FGraphNodeId>{};
	}

	// Play runs the machine cooked from the saved graph, so only a saved draft's nodes match the states it reports.
	std::vector<FGraphNodeId> Active;
	std::string Status = AnimationGraphPanel.IsDirty() ? "Saving recooks the character for the next Play" : "Play highlights the character's active state";
	const std::optional<std::size_t> State = PlaySession ? PlaySession->GetCharacterState() : std::nullopt;
	if (State && !AnimationGraphPanel.IsDirty())
	{
		Status = std::format("Playing: {}", PlaySession->GetCharacterStateName());
		if (*State < AnimationStateNodes.size())
		{
			Active.push_back(AnimationStateNodes[*State]);
		}

		if (const auto Transition = PlaySession->GetCharacterTransition(); Transition && *Transition < AnimationTransitionNodes.size())
		{
			Active.push_back(AnimationTransitionNodes[*Transition]);
		}
	}

	AnimationGraphPanel.Draw(*ToolUI, {.Status = Status, .Active = Active});
}

void FEditorFramework::FImplementation::OpenPlayerAnimationGraph()
{
	const FAssetId Character = Level ? Level->GetPlayerCharacter() : FAssetId{};
	const auto Option = std::ranges::find(Assets->GetOptions(), Character, &FPreviewAssetOption::Id);
	if (!Character.IsValid() || Option == Assets->GetOptions().end())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Choose a player character before editing its animation graph");
		return;
	}

	const std::string_view Label = Option->Label;
	const bool bEngine = Label.starts_with("Engine/");
	const std::string_view Mount = Label.substr(0, Label.find('/') + 1);
	const std::string_view Relative = Label.substr(Mount.size());
	const std::filesystem::path ModelFile = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	auto Metadata = LoadAssetMetadata(ModelFile);
	if (!Metadata)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot read {}'s metadata: {}", Label, Metadata.error().Message);
		return;
	}

	const auto Named = Metadata->Settings.find("AnimationGraph");
	const std::string GraphName = Named != Metadata->Settings.end() ? Named->second : ModelFile.stem().generic_string() + std::string(AnimationGraphExtension);
	const std::filesystem::path GraphFile = (ModelFile.parent_path() / std::filesystem::path(GraphName)).lexically_normal();
	if (Named == Metadata->Settings.end())
	{
		if (bEngine)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Engine content is read-only, so {} cannot get an animation graph", Label);
			return;
		}

		// A new graph starts from the built-in locomotion, so the character plays the same until it is edited.
		std::error_code Error;
		if (!std::filesystem::exists(GraphFile, Error))
		{
			const auto Text = SerializeGraph(MakeAnimationGraph(MakeDefaultPlayerStateMachine()), GetAnimationGraphSchema());
			std::ofstream Stream(GraphFile, std::ios::binary | std::ios::trunc);
			Stream.write(Text ? Text->data() : "", Text ? static_cast<std::streamsize>(Text->size()) : 0);
			if (!Text || !Stream)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Cannot create the animation graph {}", GraphName);
				return;
			}
		}

		Metadata->Settings["AnimationGraph"] = GraphName;
		const auto Serialized = SerializeAssetMetadata(*Metadata);
		std::ofstream Stream(GetAssetMetadataPath(ModelFile), std::ios::binary | std::ios::trunc);
		Stream.write(Serialized ? Serialized->data() : "", Serialized ? static_cast<std::streamsize>(Serialized->size()) : 0);
		if (!Serialized || !Stream)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Cannot name the animation graph in {}'s metadata", Label);
			return;
		}

		HERTA_LOG_INFO(*Log, EditorLog, "Created {} for {} from the built-in locomotion", GraphName, Label);
	}

	const std::string Folder = std::string(Relative.substr(0, Relative.rfind('/') + 1));
	const std::u8string Key = (std::filesystem::path(Folder) / std::filesystem::path(GraphName)).lexically_normal().generic_u8string();
	OpenAnimationGraphKey(std::string(Mount) + std::string(Key.begin(), Key.end()));
}

// Key is a mounted path such as Game/Characters/Mannequin/Mannequin.hanimgraph.
void FEditorFramework::FImplementation::OpenAnimationGraphKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetAnimationGraphSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open the animation graph {}: {}", Key, Graph.error().Message);
		return;
	}

	if (AnimationGraphPanel.IsOpen() && AnimationGraphPanel.GetPath() != Key && AnimationGraphPanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another animation graph", AnimationGraphPanel.GetPath());
		return;
	}

	AnimationGraphPanel.Open(Key, File, std::move(*Graph), bEngine);
	AnimationGraphRevision = UINT64_MAX;
}

// A material's graph lives beside it as <material>.hmatgraph; an existing file is reused rather than replaced.
std::expected<std::string, FAssetError> FEditorFramework::FImplementation::CreateMaterialGraph(const FAssetId Id)
{
	const auto MaterialFile = Assets->GetMaterialPath(Id);
	if (!MaterialFile)
	{
		return std::unexpected(MaterialFile.error());
	}

	std::error_code Error;
	const auto Root = std::filesystem::weakly_canonical(AssetPaths.ContentRoot, Error);
	std::filesystem::path GraphFile = *MaterialFile;
	GraphFile.replace_extension(".hmatgraph");
	const auto Relative = std::filesystem::weakly_canonical(GraphFile, Error).lexically_relative(Root);
	const std::u8string RelativeText = Relative.generic_u8string();
	if (Error || Relative.empty() || RelativeText.starts_with(u8".."))
	{
		return std::unexpected(FAssetError{"Material graphs can only be created for Game materials"});
	}

	if (!std::filesystem::exists(GraphFile, Error))
	{
		const auto Text = SerializeGraph(MakeDefaultMaterialGraph(), GetMaterialGraphSchema());
		if (!Text)
		{
			return std::unexpected(FAssetError{Text.error().Message});
		}

		std::ofstream Stream(GraphFile, std::ios::binary | std::ios::trunc);
		Stream.write(Text->data(), static_cast<std::streamsize>(Text->size()));
		Stream.close();
		if (!Stream)
		{
			return std::unexpected(FAssetError{"Cannot write the new Material Graph"});
		}
	}

	return "Game/" + std::string(RelativeText.begin(), RelativeText.end());
}

void FEditorFramework::FImplementation::OpenMaterialGraph(const FAssetId Id, const std::string_view ShaderPath)
{
	const auto Options = Assets->GetOptions();
	const auto Option = std::ranges::find(Options, Id, &FPreviewAssetOption::Id);
	OpenMaterialGraphKey(MaterialShaderKey(ShaderPath, Option != Options.end() ? Option->Label : "Game/"));
}

// Key is a mounted shader path such as Game/Materials/Rock.hmatgraph.
void FEditorFramework::FImplementation::OpenMaterialGraphKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));

	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetMaterialGraphSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open Material Graph {}: {}", Key, Graph.error().Message);
		return;
	}

	if (MaterialGraphPanel.IsOpen() && MaterialGraphPanel.GetPath() != Key && MaterialGraphPanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another Material Graph", MaterialGraphPanel.GetPath());
		return;
	}

	MaterialGraphPanel.Open(Key, File, std::move(*Graph), bEngine);
	GraphPreviewAsset = {};
	GraphPreviewMaterial.reset();
	GraphPreviewGeneration = UINT64_MAX;
	bGraphPreviewPending = true;
}

// A graph is created beside its material as <material>.hmatgraph, so that material is the one to preview, provided it still uses the graph.
// Assets may still be scanning when a graph opens from the command line, so this waits for the scan.
void FEditorFramework::FImplementation::ResolveGraphPreview()
{
	if (!bGraphPreviewPending || !Assets || Assets->IsScanning())
	{
		return;
	}

	bGraphPreviewPending = false;
	const std::string& Key = MaterialGraphPanel.GetPath();
	constexpr std::string_view GraphExtension = ".hmatgraph";
	if (!Key.ends_with(GraphExtension))
	{
		return;
	}

	const std::string MaterialKey = Key.substr(0, Key.size() - GraphExtension.size()) + ".hmat";
	const auto Options = Assets->GetOptions();
	const auto Option = std::ranges::find(Options, MaterialKey, &FPreviewAssetOption::Label);
	const auto MaterialFile = Option != Options.end() ? Assets->GetMaterialPath(Option->Id) : std::unexpected(FAssetError{});
	if (auto Material = MaterialFile ? LoadMaterialAsset(*MaterialFile) : std::unexpected(FAssetError{}); Material && MaterialShaderKey(Material->ShaderPath, MaterialKey) == Key)
	{
		GraphPreviewAsset = Option->Id;
		GraphPreviewMaterial = std::move(*Material);
	}
}
}
