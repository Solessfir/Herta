#include "EditorFrameworkPrivate.h"

namespace Herta
{
void FEditorFramework::FImplementation::DrawPlayerCharacterPicker(const float ValueWidth)
{
	if (!Level || !Assets)
	{
		return;
	}

	const FAssetId Current = Level->GetPlayerCharacter();
	std::string CurrentLabel = Current.IsValid() ? Current.ToString() : "None (capsule)";
	for (const FPreviewAssetOption& Option : Assets->GetOptions())
	{
		if (Option.Id == Current)
		{
			CurrentLabel = Option.Label;
		}
	}

	DrawFieldLabel("Character", ValueWidth);
	ImGui::SetNextItemWidth(-FLT_MIN);
	std::optional<FAssetId> Chosen;
	if (ImGui::BeginCombo("##PlayerCharacter", CurrentLabel.c_str()))
	{
		if (ImGui::Selectable("None (capsule)", !Current.IsValid()) && Current.IsValid())
		{
			Chosen = FAssetId{};
		}

		for (const FPreviewAssetOption& Option : Assets->GetOptions())
		{
			if ((Option.Importer == "Gltf" || Option.Importer == "Blender") && ImGui::Selectable(Option.Label.c_str(), Option.Id == Current) && Option.Id != Current)
			{
				Chosen = Option.Id;
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SetItemTooltip("Saved with the level. Play and the game play as this skinned model; V switches to the third-person camera");
	if (Chosen)
	{
		if (const auto Changed = Level->SetPlayerCharacter(*Chosen); !Changed)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not change the player character: {}", Changed.error().Message);
		}
	}

	if (Current.IsValid())
	{
		const FPreviewMeshSlot* const Cached = Assets->GetCachedAsset(Current);
		const char* const Status = !Cached || Cached->bLoading ? "Loading..." : Assets->GetPlayerCharacter() ? nullptr
		                                                                                                     : "Not a skinned model; Play uses the capsule";
		if (Status)
		{
			DrawFieldLabel("", ValueWidth);
			ImGui::TextDisabled("%s", Status);
		}
		else
		{
			DrawFieldLabel("", ValueWidth);
			if (ToolUIButton("Edit animation graph", EToolUIMenuIcon::Open))
			{
				OpenPlayerAnimationGraph();
			}

			ImGui::SetItemTooltip("Opens the state machine the character plays, cooked with its model; one is created from the built-in locomotion if the model has none");
		}
	}
}

void FEditorFramework::FImplementation::DrawLevelSoundPickers(const float ValueWidth)
{
	if (!Level || !Assets)
	{
		return;
	}

	for (std::size_t Cue = 0; Cue < LevelSoundCues.size(); ++Cue)
	{
		const FAssetId Current = Level->GetSounds()[Cue];
		std::string CurrentLabel = Current.IsValid() ? "Missing sound" : "None";
		for (std::size_t Option = 0; Option < SoundIds.size(); ++Option)
		{
			if (SoundIds[Option] == Current)
			{
				CurrentLabel = SoundLabels[Option];
			}
		}

		ImGui::PushID(static_cast<int>(Cue));
		DrawFieldLabel(LevelSoundCues[Cue].Label.data(), ValueWidth);
		ImGui::SetNextItemWidth(-FLT_MIN);
		std::optional<FAssetId> Chosen;
		if (ImGui::BeginCombo("##Sound", CurrentLabel.c_str()))
		{
			if (ImGui::Selectable("None", !Current.IsValid()) && Current.IsValid())
			{
				Chosen = FAssetId{};
			}

			for (std::size_t Option = 0; Option < SoundIds.size(); ++Option)
			{
				if (ImGui::Selectable(SoundLabels[Option].c_str(), SoundIds[Option] == Current) && SoundIds[Option] != Current)
				{
					Chosen = SoundIds[Option];
				}
			}

			ImGui::EndCombo();
		}

		ImGui::SetItemTooltip("Saved with the level. Play plays this sound or Audio Graph for the event");
		ImGui::PopID();
		if (Chosen)
		{
			if (const auto Changed = Level->SetSound(static_cast<ELevelSound>(Cue), *Chosen); !Changed)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not change the {} sound: {}", LevelSoundCues[Cue].Label, Changed.error().Message);
			}
		}
	}
}

void FEditorFramework::FImplementation::DrawLevelEffectPickers(const float ValueWidth)
{
	if (!Level || !Assets)
	{
		return;
	}

	for (std::size_t Cue = 0; Cue < LevelEffectCues.size(); ++Cue)
	{
		const FAssetId Current = Level->GetEffects()[Cue];
		std::string CurrentLabel = Current.IsValid() ? "Missing particle system" : "None";
		for (std::size_t Option = 0; Option < EffectIds.size(); ++Option)
		{
			if (EffectIds[Option] == Current)
			{
				CurrentLabel = EffectLabels[Option];
			}
		}

		ImGui::PushID(static_cast<int>(Cue));
		DrawFieldLabel(LevelEffectCues[Cue].Label.data(), ValueWidth);
		ImGui::SetNextItemWidth(-FLT_MIN);
		std::optional<FAssetId> Chosen;
		if (ImGui::BeginCombo("##Effect", CurrentLabel.c_str()))
		{
			if (ImGui::Selectable("None", !Current.IsValid()) && Current.IsValid())
			{
				Chosen = FAssetId{};
			}

			for (std::size_t Option = 0; Option < EffectIds.size(); ++Option)
			{
				if (ImGui::Selectable(EffectLabels[Option].c_str(), EffectIds[Option] == Current) && EffectIds[Option] != Current)
				{
					Chosen = EffectIds[Option];
				}
			}

			ImGui::EndCombo();
		}

		ImGui::SetItemTooltip("Saved with the level. Play spawns this VFX Graph for the event");
		ImGui::PopID();
		if (Chosen)
		{
			if (const auto Changed = Level->SetEffect(static_cast<ELevelEffect>(Cue), *Chosen); !Changed)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not change the {} effect: {}", LevelEffectCues[Cue].Label, Changed.error().Message);
			}
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
	FDetailsComponentField Components{.bAllMesh = bHasSelection, .bAllBody = bHasSelection, .bAllLight = bHasSelection, .bAllSkyAtmosphere = bHasSelection, .bAllHeightFog = bHasSelection, .bAllSoftBody = bHasSelection, .bAllMover = bHasSelection, .bAllTrigger = bHasSelection, .bAllConstraint = bHasSelection, .SunIds = SunIds, .SunLabels = SunLabels, .ConstraintTargetIds = ConstraintTargetIds, .ConstraintTargetLabels = ConstraintTargetLabels, .EnvironmentIds = EnvironmentIds, .EnvironmentLabels = EnvironmentLabels, .SoundIds = SoundIds, .SoundLabels = SoundLabels, .EffectIds = EffectIds, .EffectLabels = EffectLabels};
	std::vector<FDetailsMaterialSlot> MaterialSlots;
	std::vector<FAssetId> MaterialIds;
	std::vector<std::string> MaterialLabels;
	std::vector<std::uint64_t> MaterialThumbnails;
	std::vector<FLevelEntity> SelectedMeshes;
	std::array<std::optional<FLevelEntity>, 12> VisualBaselines;
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
