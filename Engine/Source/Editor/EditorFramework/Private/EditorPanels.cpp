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
	                                     .ShaderStatus = GraphStatus,
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
