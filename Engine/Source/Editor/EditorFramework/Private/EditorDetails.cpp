#include "EditorFrameworkPrivate.h"

namespace Herta
{
std::vector<FLevelRegion> FEditorFramework::FImplementation::GetShownRegions() const
{
	std::vector<FLevelRegion> Regions(Level->GetRegions().begin(), Level->GetRegions().end());
	if (RegionDraft)
	{
		const auto Found = std::ranges::find(Regions, RegionDraft->Id, &FLevelRegion::Id);
		if (Found != Regions.end())
		{
			*Found = *RegionDraft;
		}
	}

	return Regions;
}

void FEditorFramework::FImplementation::DrawLevelRegions(const float ValueWidth, const float Scale)
{
	const std::span<const FLevelRegion> Regions = Level->GetRegions();
	std::optional<std::vector<FLevelRegion>> Changed;
	std::string Label;
	std::optional<FObjectId> Frame;
	for (std::size_t Index = 0; Index < Regions.size(); ++Index)
	{
		const FLevelRegion& Region = Regions[Index];
		const bool bSelected = SelectedRegion == Region.Id;
		ImGui::PushID(static_cast<int>(Index));
		if (ImGui::Selectable(Region.Name.c_str(), bSelected))
		{
			SelectedRegion = bSelected ? std::nullopt : std::optional{Region.Id};
			RegionDraft.reset();
		}

		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			Frame = Region.Id;
		}

		ImGui::SetItemTooltip("%s\nDouble-click to frame. Right-click to rename or delete.", Region.Level.c_str());
		const std::string Radii = std::format("{:.0f} / {:.0f} m", Region.LoadRadius, Region.UnloadRadius);
		const ImVec2 RowMaximum = ImGui::GetItemRectMax();
		const ImVec2 RadiiSize = ImGui::CalcTextSize(Radii.c_str());
		ImGui::GetWindowDrawList()->AddText({RowMaximum.x - RadiiSize.x - 4.f * Scale, RowMaximum.y - RadiiSize.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Radii.c_str());
		if (ImGui::BeginPopupContextItem("RegionActions"))
		{
			if (ImGui::IsWindowAppearing())
			{
				RegionName.fill('\0');
				Region.Name.copy(RegionName.data(), std::min(Region.Name.size(), RegionName.size() - 1));
				ImGui::SetKeyboardFocusHere();
			}

			ImGui::SetNextItemWidth(180.f * Scale);
			if (ImGui::InputText("##Name", RegionName.data(), RegionName.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) && RegionName[0] != '\0')
			{
				Changed.emplace(Regions.begin(), Regions.end());
				(*Changed)[Index].Name = RegionName.data();
				Label = "Rename region";
				ImGui::CloseCurrentPopup();
			}

			if (ImGui::MenuItem("Frame"))
			{
				Frame = Region.Id;
			}

			if (ImGui::MenuItem("Delete"))
			{
				Changed.emplace(Regions.begin(), Regions.end());
				Changed->erase(Changed->begin() + static_cast<std::ptrdiff_t>(Index));
				Label = "Delete region";
			}

			ImGui::EndPopup();
		}

		if (bSelected)
		{
			FLevelRegion Draft = RegionDraft && RegionDraft->Id == Region.Id ? *RegionDraft : Region;
			std::array Offset{static_cast<float>(Draft.Offset.Meters.X), static_cast<float>(Draft.Offset.Meters.Y), static_cast<float>(Draft.Offset.Meters.Z)};
			bool bEdited = false;
			bool bReleased = false;
			ImGui::Indent(8.f * Scale);
			DrawFieldLabel("Offset", ValueWidth);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat3("##Offset", Offset.data(), 0.1f, -100000.f, 100000.f, "%.1f m"))
			{
				Draft.Offset = FWorldPosition{Offset[0], Offset[1], Offset[2]};
				bEdited = true;
			}

			bReleased |= ImGui::IsItemDeactivatedAfterEdit();
			DrawFieldLabel("Load radius", ValueWidth);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat("##Load", &Draft.LoadRadius, 0.5f, 1.f, 10000.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp))
			{
				Draft.UnloadRadius = std::max(Draft.UnloadRadius, Draft.LoadRadius + 1.f);
				bEdited = true;
			}

			bReleased |= ImGui::IsItemDeactivatedAfterEdit();
			DrawFieldLabel("Unload radius", ValueWidth);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat("##Unload", &Draft.UnloadRadius, 0.5f, Draft.LoadRadius + 1.f, 10001.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp))
			{
				bEdited = true;
			}

			bReleased |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::Unindent(8.f * Scale);
			if (bEdited)
			{
				RegionDraft = Draft;
			}

			if (bReleased && RegionDraft)
			{
				Changed.emplace(Regions.begin(), Regions.end());
				(*Changed)[Index] = *RegionDraft;
				Label = "Edit region";
				RegionDraft.reset();
			}
		}

		ImGui::PopID();
	}

	if (Regions.empty())
	{
		ImGui::TextDisabled("Place other levels to stream in around the player");
	}

	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##AddRegion", "Add region...", ImGuiComboFlags_HeightLarge))
	{
		if (ImGui::IsWindowAppearing())
		{
			RegionLevelChoices.clear();
			const std::filesystem::path Folder = Level->GetPath().parent_path();
			std::error_code Error;
			for (auto Entry = std::filesystem::recursive_directory_iterator(Folder, Error); !Error && Entry != std::filesystem::recursive_directory_iterator(); Entry.increment(Error))
			{
				if (Entry->is_regular_file(Error) && Entry->path().extension() == ".hlevel" && Entry->path() != Level->GetPath())
				{
					const std::u8string Relative = Entry->path().lexically_relative(Folder).generic_u8string();
					RegionLevelChoices.emplace_back(Relative.begin(), Relative.end());
				}
			}

			std::ranges::sort(RegionLevelChoices);
		}

		if (RegionLevelChoices.empty())
		{
			ImGui::TextDisabled("No other levels beside this one");
		}

		for (const std::string& Choice : RegionLevelChoices)
		{
			if (ImGui::Selectable(Choice.c_str()))
			{
				// New regions are placed in front of the camera on the ground, where the viewer is looking.
				const FVector3& Camera = ViewportCamera.GetPosition();
				const float Yaw = ViewportCamera.GetYaw();
				const FVector3 Ahead{Camera.X + std::sin(Yaw) * 30.f, 0.f, Camera.Z + std::cos(Yaw) * 30.f};
				const std::string Stem = std::filesystem::path(Choice).stem().string();
				const auto Uses = std::ranges::count_if(Regions, [&Choice](const FLevelRegion& Existing)
				{
					return Existing.Level == Choice;
				});

				Changed.emplace(Regions.begin(), Regions.end());
				Changed->push_back({.Id = FObjectId::Generate(), .Name = std::format("{} {}", Stem, Uses + 1), .Level = Choice, .Offset = FWorldPosition{Ahead.X, Ahead.Y, Ahead.Z}});
				SelectedRegion = Changed->back().Id;
				Label = "Add region";
			}
		}

		ImGui::EndCombo();
	}

	// Applied after the loop, which reads from the level's own storage.
	if (Changed)
	{
		ReportLevelResult(Level->SetRegions(Label, std::move(*Changed)));
	}

	if (Frame)
	{
		const auto Found = std::ranges::find(Level->GetRegions(), *Frame, &FLevelRegion::Id);
		if (Found != Level->GetRegions().end())
		{
			const FVector3d& Center = Found->Offset.Meters;
			const float Distance = std::max(Found->LoadRadius * 0.6f, 10.f);
			ViewportCamera.SetView({static_cast<float>(Center.X), static_cast<float>(Center.Y) + Distance, static_cast<float>(Center.Z) - Distance}, 0.f, -std::numbers::pi_v<float> * 0.25f);
		}
	}
}

void FEditorFramework::FImplementation::DrawDetailsPanel()
{
	FPreviewObject PreviousObject = GetActivePreviewObject();
	[[maybe_unused]] auto& [Label, Translation, Rotation, Scale, Mesh, ObjectId, Parent, Kind] = GetActivePreviewObject();
	FDetailsMeshField MeshField;
	std::string MeshStatus;
	const bool bHasSelection = !PreviewSelection.Indices.empty();
	FDetailsComponentField Components{.bAllMesh = bHasSelection, .bAllBody = bHasSelection, .bAllLight = bHasSelection, .bAllSkyAtmosphere = bHasSelection, .bAllHeightFog = bHasSelection, .bAllSoftBody = bHasSelection, .bAllMover = bHasSelection, .bAllTrigger = bHasSelection, .bAllConstraint = bHasSelection, .SunIds = SunIds, .SunLabels = SunLabels, .ConstraintTargetIds = ConstraintTargetIds, .ConstraintTargetLabels = ConstraintTargetLabels, .EnvironmentIds = EnvironmentIds, .EnvironmentLabels = EnvironmentLabels, .SoundIds = SoundIds, .SoundLabels = SoundLabels, .EffectIds = EffectIds, .EffectLabels = EffectLabels, .StateTreeIds = StateTreeIds, .StateTreeLabels = StateTreeLabels, .GameplayGraphIds = GameplayGraphIds, .GameplayGraphLabels = GameplayGraphLabels, .ModelIds = ModelIds, .ModelLabels = ModelLabels};
	// Each starts true and turns false at the first selected entity without the component.
	for (bool* const bAll : {&Components.bAllDestructible, &Components.bAllCharacter, &Components.bAllPlayer, &Components.bAllAudioEmitter, &Components.bAllAudioListener, &Components.bAllParticleEmitter, &Components.bAllNavigationBounds, &Components.bAllNavigationObstacle, &Components.bAllAiAgent, &Components.bAllGameplayGraph})
	{
		*bAll = bHasSelection;
	}

	std::vector<FDetailsMaterialSlot> MaterialSlots;
	std::vector<FAssetId> MaterialIds;
	std::vector<std::string> MaterialLabels;
	std::vector<std::uint64_t> MaterialThumbnails;
	std::vector<FLevelEntity> SelectedMeshes;
	std::array<std::optional<FLevelEntity>, 18> VisualBaselines;
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
			Aggregate(6, ELevelComponentType::Constraint, Entity->Constraint.has_value(), Components.bAnyConstraint, Components.bAllConstraint, Components.MixedConstraint);
			Aggregate(7, ELevelComponentType::Destructible, Entity->Destructible.has_value(), Components.bAnyDestructible, Components.bAllDestructible, Components.MixedDestructible);
			Aggregate(8, ELevelComponentType::Character, Entity->Character.has_value(), Components.bAnyCharacter, Components.bAllCharacter, Components.MixedCharacter);
			Aggregate(9, ELevelComponentType::AudioEmitter, Entity->AudioEmitter.has_value(), Components.bAnyAudioEmitter, Components.bAllAudioEmitter, Components.MixedAudioEmitter);
			Aggregate(10, ELevelComponentType::AudioListener, Entity->AudioListener.has_value(), Components.bAnyAudioListener, Components.bAllAudioListener, Components.MixedAudioListener);
			Aggregate(11, ELevelComponentType::ParticleEmitter, Entity->ParticleEmitter.has_value(), Components.bAnyParticleEmitter, Components.bAllParticleEmitter, Components.MixedParticleEmitter);
			Aggregate(12, ELevelComponentType::NavigationBounds, Entity->NavigationBounds.has_value(), Components.bAnyNavigationBounds, Components.bAllNavigationBounds, Components.MixedNavigationBounds);
			Aggregate(13, ELevelComponentType::NavigationObstacle, Entity->NavigationObstacle.has_value(), Components.bAnyNavigationObstacle, Components.bAllNavigationObstacle, Components.MixedNavigationObstacle);
			Aggregate(14, ELevelComponentType::AiAgent, Entity->AiAgent.has_value(), Components.bAnyAiAgent, Components.bAllAiAgent, Components.MixedAiAgent);
			Aggregate(15, ELevelComponentType::GameplayGraph, Entity->GameplayGraph.has_value(), Components.bAnyGameplayGraph, Components.bAllGameplayGraph, Components.MixedGameplayGraph);
			Aggregate(16, ELevelComponentType::Player, Entity->Player.has_value(), Components.bAnyPlayer, Components.bAllPlayer, Components.MixedPlayer);
			bool bUnusedAny = false;
			bool bUnusedAll = true;
			Aggregate(17, ELevelComponentType::RigidBody, Type != ELevelBodyType::None, bUnusedAny, bUnusedAll, Components.MixedBodyAssets);
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

	if (VisualBaselines[7])
	{
		Components.Destructible = *VisualBaselines[7]->Destructible;
	}

	if (VisualBaselines[8])
	{
		Components.Character = *VisualBaselines[8]->Character;
	}

	if (VisualBaselines[9])
	{
		Components.AudioEmitter = *VisualBaselines[9]->AudioEmitter;
	}

	if (VisualBaselines[10])
	{
		Components.AudioListener = *VisualBaselines[10]->AudioListener;
	}

	if (VisualBaselines[11])
	{
		Components.ParticleEmitter = *VisualBaselines[11]->ParticleEmitter;
	}

	if (VisualBaselines[12])
	{
		Components.NavigationBounds = *VisualBaselines[12]->NavigationBounds;
	}

	if (VisualBaselines[13])
	{
		Components.NavigationObstacle = *VisualBaselines[13]->NavigationObstacle;
	}

	if (VisualBaselines[14])
	{
		Components.AiAgent = *VisualBaselines[14]->AiAgent;
	}

	if (VisualBaselines[15])
	{
		Components.GameplayGraph = *VisualBaselines[15]->GameplayGraph;
	}

	if (VisualBaselines[16])
	{
		Components.Player = *VisualBaselines[16]->Player;
	}

	if (VisualBaselines[5])
	{
		Components.Trigger = *VisualBaselines[5]->Trigger;
	}

	if (VisualBaselines[6])
	{
		Components.Constraint = *VisualBaselines[6]->Constraint;
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
	const FDetailsMeshResult MeshResult = DrawPreviewDetailsPanel(*ToolUI, bDetailsOpen, PreviewSelection.Active >= 0, bProjectBusy || IsSimulating() || ViewportInteraction.DragButton >= 0, Translation, Rotation, Scale, DetailsPanelState, Label, PreviewSelection.Indices.size(), Assets && Mesh.IsValid() ? &MeshField : nullptr, &Edits, &Components);
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
			case EDetailsComponentAction::AddConstraint:
				ReportLevelResult(Level->AddConstraintToSelected());
				break;
			case EDetailsComponentAction::RemoveConstraint:
				ReportLevelResult(Level->SetSelectedConstraint(std::nullopt));
				break;
			case EDetailsComponentAction::AddDestructible:
				ReportLevelResult(Level->AddDestructibleToSelected());
				break;
			case EDetailsComponentAction::RemoveDestructible:
				ReportLevelResult(Level->SetSelectedDestructible(std::nullopt));
				break;
			case EDetailsComponentAction::AddCharacter:
				ReportLevelResult(Level->AddCharacterToSelected());
				break;
			case EDetailsComponentAction::RemoveCharacter:
				ReportLevelResult(Level->SetSelectedCharacter(std::nullopt));
				break;
			case EDetailsComponentAction::AddPlayer:
				ReportLevelResult(Level->AddPlayerToSelected());
				break;
			case EDetailsComponentAction::RemovePlayer:
				ReportLevelResult(Level->SetSelectedPlayer(std::nullopt));
				break;
			case EDetailsComponentAction::AddAudioEmitter:
				ReportLevelResult(Level->AddAudioEmitterToSelected());
				break;
			case EDetailsComponentAction::RemoveAudioEmitter:
				ReportLevelResult(Level->SetSelectedAudioEmitter(std::nullopt));
				break;
			case EDetailsComponentAction::AddAudioListener:
				ReportLevelResult(Level->AddAudioListenerToSelected());
				break;
			case EDetailsComponentAction::RemoveAudioListener:
				ReportLevelResult(Level->SetSelectedAudioListener(std::nullopt));
				break;
			case EDetailsComponentAction::AddParticleEmitter:
				ReportLevelResult(Level->AddParticleEmitterToSelected());
				break;
			case EDetailsComponentAction::RemoveParticleEmitter:
				ReportLevelResult(Level->SetSelectedParticleEmitter(std::nullopt));
				break;
			case EDetailsComponentAction::AddNavigationBounds:
				ReportLevelResult(Level->AddNavigationBoundsToSelected());
				break;
			case EDetailsComponentAction::RemoveNavigationBounds:
				ReportLevelResult(Level->SetSelectedNavigationBounds(std::nullopt));
				break;
			case EDetailsComponentAction::AddNavigationObstacle:
				ReportLevelResult(Level->AddNavigationObstacleToSelected());
				break;
			case EDetailsComponentAction::RemoveNavigationObstacle:
				ReportLevelResult(Level->SetSelectedNavigationObstacle(std::nullopt));
				break;
			case EDetailsComponentAction::AddAiAgent:
				ReportLevelResult(Level->AddAiAgentToSelected());
				break;
			case EDetailsComponentAction::RemoveAiAgent:
				ReportLevelResult(Level->SetSelectedAiAgent(std::nullopt));
				break;
			case EDetailsComponentAction::AddGameplayGraph:
				ReportLevelResult(Level->AddGameplayGraphToSelected());
				break;
			case EDetailsComponentAction::RemoveGameplayGraph:
				ReportLevelResult(Level->SetSelectedGameplayGraph(std::nullopt));
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
	const bool bFocusRequested = DrawPreviewOutlinerPanel(*ToolUI, bOutlinerOpen, NewSelection, PreviewObjects, bProjectBusy || IsSimulating() || ViewportInteraction.DragButton >= 0, OutlinerPanelState, Level->GetFolders());
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
}
