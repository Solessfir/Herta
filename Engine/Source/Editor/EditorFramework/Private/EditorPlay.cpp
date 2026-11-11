#include "EditorFrameworkPrivate.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/GameFramework/GameText.h"
#include "Herta/Navigation/NavigationWorld.h"

#include <simdjson.h>

namespace Herta
{
namespace
{
// Left stick moves and right stick looks; A jumps, clicking the left stick toggles sprinting, and B or clicking the right stick crouches, as in the game.
FGameGamepad GetGameGamepad(const FGamepadState& Gamepad)
{
	return {
	    .Move = {Gamepad.LeftX, -Gamepad.LeftY},
	    .Look = {Gamepad.RightX, -Gamepad.RightY},
	    .bJump = Gamepad.IsDown(EGamepadButton::South),
	    .bSprint = Gamepad.IsDown(EGamepadButton::LeftThumb),
	    .bCrouch = Gamepad.IsDown(EGamepadButton::East) || Gamepad.IsDown(EGamepadButton::RightThumb),
	    .bThrow = Gamepad.IsDown(EGamepadButton::West),
	    .bToggleCamera = Gamepad.IsDown(EGamepadButton::North),
	    .bAttack = Gamepad.IsDown(EGamepadButton::RightShoulder),
	    .bBlock = Gamepad.IsDown(EGamepadButton::LeftShoulder),
	};
}

// Lets play draw with the meshes and materials the editor already loaded, instead of loading the level twice.
class FEditorGameAssets final : public IGameAssets
{
public:
	explicit FEditorGameAssets(const FPreviewAssets& InAssets);

	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId& Asset) const override;
	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId& Asset) const override;
	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId& Asset) const override;
	[[nodiscard]] std::shared_ptr<const FCookedModel> FindSkinnedModel(const FAssetId& Asset) const override;

private:
	const FPreviewAssets& Assets;
};

FEditorGameAssets::FEditorGameAssets(const FPreviewAssets& InAssets)
    : Assets(InAssets)
{
}

const FRenderMesh* FEditorGameAssets::FindMesh(const FAssetId& Asset) const
{
	const FPreviewMeshSlot* const Slot = Assets.GetCachedAsset(Asset);
	return Slot ? Slot->Mesh.get() : nullptr;
}

const FRenderMaterial* FEditorGameAssets::FindMaterial(const FAssetId& Asset) const
{
	return Assets.GetMaterial(Asset).get();
}

std::shared_ptr<const FCookedTexture> FEditorGameAssets::FindEnvironment(const FAssetId& Asset) const
{
	return Assets.GetCookedTexture(Asset);
}

std::shared_ptr<const FCookedModel> FEditorGameAssets::FindSkinnedModel(const FAssetId& Asset) const
{
	const FPreviewMeshSlot* const Slot = Assets.GetCachedAsset(Asset);
	return Slot ? Slot->SkinnedModel : nullptr;
}
}

void FEditorFramework::FImplementation::ToggleSimulation()
{
	if (bProjectBusy)
	{
		return;
	}

	if (!Simulation.IsRunning())
	{
		ToolUI->FocusViewport();
	}

	if (Simulation.IsRunning())
	{
		Simulation.Stop();
		PreviewObjects = std::move(SimulationStart);
		Level->SetSimulationRunning(false);
		bSimulationStoppedThisFrame = true;
	}
	else
	{
		if (Level->HasActiveEdit())
		{
			if (auto Result = Level->EndEdit(); !Result)
			{
				ReportLevelResult(std::move(Result));
				return;
			}
		}

		if (auto Result = Level->CommitEdits(); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not apply level edits: {}", Result.error().Message);
			return;
		}

		const FLevelSimulationSetup Setup = BuildLevelSimulation(VisualEntities, PreviewModels, PreviewMeshes);
		for (const std::string& Warning : Setup.Warnings)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "{}", Warning);
		}

		if (Setup.Bodies.empty() && Setup.SoftBodies.empty())
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Simulation preview requires a Static Mesh with a Rigid Body component or a Soft Body");
			return;
		}

		std::vector<FPreviewObject> OriginalObjects = PreviewObjects;
		FPhysicsWorldSettings PhysicsSettings = LevelPhysicsSettings;
		PhysicsSettings.Tasks = Tasks;
		// Simulate shows the authored objects only, so destructibles stay whole there; Play breaks them.
		std::vector<FLevelSimulationBody> Bodies = Setup.Bodies;
		Bodies.insert(Bodies.end(), Setup.IntactDestructibles.begin(), Setup.IntactDestructibles.end());
		if (const auto Result = Simulation.Start(Bodies, Setup.SoftBodies, Setup.Constraints, PhysicsSettings); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not start simulation: {}", Result.error().Message);
			return;
		}

		SimulationStart = std::move(OriginalObjects);
		Level->SetSimulationRunning(true);
		UpdateSimulation(0.f);
	}

	PreviewDragStart.reset();
	ViewportGizmos.resetId();
}

void FEditorFramework::FImplementation::TogglePlay()
{
	if (PlaySession)
	{
		StopPlayAudio();
		PlaySession.reset();
		PlayAssets.reset();

		PlayRenderView = {};
		bAutomationOwnsPlay = false;
		ToolUI->SetCursorCaptured(false);
		Level->SetSimulationRunning(false);
		bSimulationStoppedThisFrame = true;
		HERTA_LOG_INFO(*Log, EditorLog, "Play stopped");
		return;
	}

	if (bProjectBusy || Simulation.IsRunning() || !Assets || !GraphicsDevice)
	{
		return;
	}

	// Play runs in the level viewport, so starting it from an asset editor brings the level back in front.
	ToolUI->FocusViewport();

	if (Level->HasActiveEdit())
	{
		if (auto Result = Level->EndEdit(); !Result)
		{
			ReportLevelResult(std::move(Result));
			return;
		}
	}

	if (auto Result = Level->CommitEdits(); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not apply level edits: {}", Result.error().Message);
		return;
	}

	auto GameAssets = std::make_unique<FEditorGameAssets>(*Assets);
	const auto Loading = std::ranges::find_if(VisualEntities, [&GameAssets](const FLevelEntity& Entity)
	{
		return Entity.Mesh && !GameAssets->FindMesh(Entity.Mesh->Asset);
	});

	if (Loading != VisualEntities.end())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Play waits for every mesh to load; {} is still loading", Loading->Name);
		return;
	}

	std::shared_ptr<const FCookedModel> Character = Assets->GetPlayerCharacter();
	if (Level->GetPlayerCharacter().IsValid() && !Character)
	{
		const FPreviewMeshSlot* const Cached = Assets->GetCachedAsset(Level->GetPlayerCharacter());
		if (!Cached || Cached->bLoading)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Play waits for the player character to load");
			return;
		}

		HERTA_LOG_WARNING(*Log, EditorLog, "The player character is not a skinned model; playing as the capsule");
	}

	if (AreLevelSoundsLoading())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Play waits for the level's sounds to load");
		return;
	}

	auto Session = FGameSession::Create(VisualEntities, *GameAssets, GraphicsDevice, {}, Tasks, std::move(Character));
	if (!Session)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not start play: {}", Session.error().Message);
		return;
	}

	(*Session)->SetLevelSounds(Level->GetSounds());
	(*Session)->SetLevelEffects(Level->GetEffects());
	for (const FAssetId& System : (*Session)->GetParticleSystems())
	{
		const FPreviewMeshSlot* const Slot = Assets->GetCachedAsset(System);
		if (Slot && Slot->ParticleSystem)
		{
			if (auto Added = (*Session)->AddParticleSystem(System, Slot->ParticleSystem); !Added)
			{
				HERTA_LOG_WARNING(*Log, EditorLog, "{} will not play: {}", Slot->Label, Added.error().Message);
			}
		}
	}

	for (const std::string& Warning : (*Session)->GetWarnings())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "{}", Warning);
	}

	if (!PlayUI && RegisterUITexture && UnregisterUITexture)
	{
		auto UI = FGameUI::Create({
		    .Device = GraphicsDevice,
		    .FontBytes = ReadDiskFile(EngineRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf").value_or(std::vector<std::byte>{}),
		    .FallbackFontBytes = {ReadDiskFile(EngineRoot / "Engine/Content/Editor/Fonts/NotoEmoji/NotoEmoji.ttf").value_or(std::vector<std::byte>{})},
		    .RegisterTexture = [this](const FTextureHandle& Texture) -> std::expected<std::uint64_t, FGameUIError>
		{
			auto Registered = RegisterUITexture(Texture);
			if (!Registered)
			{
				return std::unexpected(FGameUIError{std::move(Registered.error().Message)});
			}

			return *Registered;
		},
		    .UnregisterTexture = UnregisterUITexture,
		});

		if (!UI)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Play runs without its HUD: {}", UI.error().Message);
		}
		else
		{
			PlayUI = std::move(*UI);
		}
	}

	PlaySession = std::move(*Session);
	// A new session gets the latest navigation and its agents' trees again.
	PlayNavigationKey = 0;
	PlayStateTrees.clear();
	PlayGameplayGraphs.clear();
	PlaySession->SetRegions(Level->GetRegions(), MakeRegionLoader());
	PlayAssets = std::move(GameAssets);
	bPlayPaused = false;
	PlayMenuFocus = 0;
	PlayMenuChoice.reset();
	bPlayJumpArmed = false;
	PlayGamepadControls.Reset();
	Level->SetSimulationRunning(true);
	PreviewDragStart.reset();
	ViewportGizmos.resetId();
	// Play keeps the pointer only while the viewport has focus, so take it whichever panel started Play.
	ImGui::SetWindowFocus("Viewport");
	ToolUI->SetCursorCaptured(true);
	HERTA_LOG_INFO(*Log, EditorLog, "Playing; press Esc to stop");
}

// Regions read their level on a worker. The viewport preview already keeps every region's assets loaded, so Play only waits for them.
FRegionLoader FEditorFramework::FImplementation::MakeRegionLoader()
{
	return [this, LevelPath = Level->GetPath()](const FLevelRegion& Region, const std::atomic<bool>&) -> std::expected<FLoadedRegion, FGameError>
	{
		auto Document = LoadLevel(ResolveLevelRegionPath(LevelPath, Region));
		if (!Document)
		{
			return std::unexpected(FGameError{std::format("Cannot load the level of region '{}': {}", Region.Name, Document.error().Message)});
		}

		auto Wanted = std::make_shared<std::vector<FAssetId>>();
		for (const FLevelEntity& Entity : Document->Entities)
		{
			if (Entity.Mesh && IsLevelRegionScenery(Entity))
			{
				Wanted->push_back(Entity.Mesh->Asset);
				std::ranges::copy_if(Entity.Mesh->Materials, std::back_inserter(*Wanted), &FAssetId::IsValid);
			}
		}

		return FLoadedRegion{
		    .Document = std::move(*Document),
		    .Prepare = [this, Wanted]() -> std::expected<bool, FGameError>
		{
			return std::ranges::all_of(*Wanted, [this](const FAssetId& Asset)
			{
				const FPreviewMeshSlot* const Slot = Assets->GetCachedAsset(Asset);
				return Slot && !Slot->bLoading;
			});
		},
		};
	};
}

bool FEditorFramework::FImplementation::IsPlayGamepadPressed(const EGamepadButton Button) const
{
	return PlayGamepad && PlayGamepad->IsDown(Button) && !(PreviousPlayGamepad && PreviousPlayGamepad->IsDown(Button));
}

void FEditorFramework::FImplementation::UpdatePlay(const float DeltaSeconds)
{
	if (!PlaySession)
	{
		return;
	}

	PreviousPlayGamepad = std::exchange(PlayGamepad, Application && !bScriptedPlay ? Application->GetGamepadState() : std::nullopt);

	// The pause menu's choice from the previous frame's HUD.
	if (const std::optional<std::size_t> Choice = std::exchange(PlayMenuChoice, std::nullopt))
	{
		bPlayPaused = false;
		if (*Choice == 2)
		{
			TogglePlay();
			return;
		}

		if (*Choice == 1)
		{
			if (auto Restarted = PlaySession->Restart(); !Restarted)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Play stopped: {}", Restarted.error().Message);
				TogglePlay();
				return;
			}
		}
	}

	// Leaving the editor window pauses play like the game losing focus; leaving only the viewport freezes it until clicked again.
	const ImGuiIO& IO = ImGui::GetIO();
	// Start pauses like Shift+Esc, and B backs out of the menu.
	const bool bGamepadPause = IsPlayGamepadPressed(EGamepadButton::Start) || (bPlayPaused && IsPlayGamepadPressed(EGamepadButton::East));
	const bool bAutomationDriven = !AutomationInput.empty();
	if (bAutomationOwnsPlay && !bAutomationDriven)
	{
		bPlayPaused = false;
		ToolUI->SetCursorCaptured(false);
		PlayGamepadControls.Reset();
		if (auto Updated = PlaySession->Update(0.f, {}); !Updated)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Play stopped: {}", Updated.error().Message);
			TogglePlay();
			return;
		}

		SubmitPlayAudio();
		return;
	}

	if (!bScriptedPlay && !bAutomationDriven && (IO.AppFocusLost || bGamepadPause || (IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Escape, false))))
	{
		bPlayPaused = IO.AppFocusLost || !bPlayPaused;
		PlayMenuFocus = 0;
	}

	if (bAutomationDriven)
	{
		bPlayPaused = false;
	}

	const bool bFocused = !bPlayPaused && (bScriptedPlay || bAutomationDriven || (!IO.AppFocusLost && ToolUI->IsPanelFocused("Viewport")));
	ToolUI->SetCursorCaptured(bFocused && !bScriptedPlay && !bAutomationDriven);
	bPlayJumpArmed |= !ImGui::IsKeyDown(ImGuiKey_Space);
	FGameInput Input;
	if (bAutomationDriven)
	{
		const FAutomationFrame Frame = AutomationInput.front();
		AutomationInput.pop_front();
		Input = Frame.Input;
		if (Frame.SteerTo)
		{
			const FVector3 Foot = PlaySession->GetPlayerFootPosition();
			const float Target = std::atan2(Frame.SteerTo->X - Foot.X, Frame.SteerTo->Y - Foot.Z);
			Input.YawDelta += std::remainder(Target - PlaySession->GetPlayerYaw(), 2.f * std::numbers::pi_v<float>);
		}
	}
	else if (bScriptedPlay)
	{
		const std::uint32_t Frame = ScriptedPlayFrame++;
		Input = {.Move = {0.f, 1.f}, .YawDelta = Frame % 240 < 120 ? 0.004f : -0.004f, .bJump = Frame % 90 == 45};
	}
	else if (bFocused)
	{
		const auto Axis = [](const ImGuiKey Positive, const ImGuiKey Negative)
		{
			return (ImGui::IsKeyDown(Positive) ? 1.f : 0.f) - (ImGui::IsKeyDown(Negative) ? 1.f : 0.f);
		};

		constexpr float Sensitivity = 0.0022f;
		const FToolUIViewportPosition Look = ToolUI->GetCapturedMouseMotion();
		Input = {
		    .Move = {Axis(ImGuiKey_D, ImGuiKey_A), Axis(ImGuiKey_W, ImGuiKey_S)},
		    .YawDelta = -Look.X * Sensitivity,
		    .PitchDelta = -Look.Y * Sensitivity,
		    .bJump = bPlayJumpArmed && ImGui::IsKeyPressed(ImGuiKey_Space, false),
		    .bSprint = IO.KeyShift,
		    .bCrouch = ImGui::IsKeyDown(ImGuiKey_C) || IO.KeyCtrl,
		    .bThrow = ImGui::IsKeyPressed(ImGuiKey_F, false),
		    .bToggleCamera = ImGui::IsKeyPressed(ImGuiKey_V, false),
		    .bToggleRagdoll = ImGui::IsKeyPressed(ImGuiKey_Z, false),
		    .bAttack = ImGui::IsMouseClicked(ImGuiMouseButton_Left),
		    .bBlock = ImGui::IsMouseDown(ImGuiMouseButton_Right),
		};

		if (ImGui::IsKeyPressed(ImGuiKey_R, false) || IsPlayGamepadPressed(EGamepadButton::Back))
		{
			if (auto Restarted = PlaySession->Restart(); !Restarted)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Play stopped: {}", Restarted.error().Message);
				TogglePlay();
				return;
			}
		}
	}

	if (bFocused && PlayGamepad && !bAutomationDriven)
	{
		PlayGamepadControls.Apply(Input, GetGameGamepad(*PlayGamepad), DeltaSeconds);
	}
	else
	{
		PlayGamepadControls.Reset();
	}

	// Automation steps a fixed 1/60 s per frame, so N frames of scripted input play the same however fast the editor is drawing.
	const float StepSeconds = bAutomationDriven ? 1.f / 60.f : bFocused ? DeltaSeconds
	                                                                    : 0.f;
	if (auto Updated = PlaySession->Update(StepSeconds, Input); !Updated)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Play stopped: {}", Updated.error().Message);
		TogglePlay();
		return;
	}

	for (const std::string& Event : PlaySession->TakeEvents())
	{
		HERTA_LOG_INFO(*Log, EditorLog, "{}", Event);
	}

	SubmitPlayAudio();

	if (bAutomationDriven)
	{
		FinishAutomationFrame();
	}

	// The editor renders the whole workspace canvas behind its glass panels and shows only the viewport panel's part of it. The game frames
	// its camera for the panel, so its projection is built for the panel's shape, then widened to the canvas and centered on the panel, as
	// the editor camera's is; otherwise the character would sit off center and too large.
	const FExtent2D PanelExtent{
	    .Width = std::max(static_cast<std::uint32_t>(static_cast<float>(ViewportExtent.Width) * ViewportVisibleSize.X), 1u),
	    .Height = std::max(static_cast<std::uint32_t>(static_cast<float>(ViewportExtent.Height) * ViewportVisibleSize.Y), 1u),
	};

	PlayRenderView = PlaySession->GetRenderView(PanelExtent);
	PlayRenderView.Projection(0, 0) *= ViewportVisibleSize.X;
	PlayRenderView.Projection(1, 1) *= ViewportVisibleSize.Y;
	PlayRenderView.Projection(0, 2) = 2.f * ViewportProjectionCenter.X - 1.f;
	PlayRenderView.Projection(1, 2) = 1.f - 2.f * ViewportProjectionCenter.Y;
	BuildCollisionDebugDraw();

	// Play keeps the viewport's quality settings, so a tier can be judged in motion.
	FVisualSettings& PlayVisuals = PlayRenderView.Visuals;
	PlayVisuals.AntiAliasing = VisualSettings.AntiAliasing;
	PlayVisuals.ShadowQuality = VisualSettings.ShadowQuality;
	PlayVisuals.ShadowResolution = VisualSettings.ShadowResolution;
	PlayVisuals.AmbientOcclusion = VisualSettings.AmbientOcclusion;
	PlayVisuals.FogResolution = VisualSettings.FogResolution;
	PlayVisuals.bOcclusionCulling = VisualSettings.bOcclusionCulling;
	PlayVisuals.bContactShadows = VisualSettings.bContactShadows;
	PlayVisuals.bGlobalIllumination = VisualSettings.bGlobalIllumination;
	PlayVisuals.PenumbraSaturation = VisualSettings.PenumbraSaturation;
	PlayVisuals.ExposureCompensation = VisualSettings.ExposureCompensation;
}

void FEditorFramework::FImplementation::DrawPlayHud(const ImVec2 Minimum, const ImVec2 Size)
{
	if (!PlayUI || Size.x < 1.f || Size.y < 1.f)
	{
		return;
	}

	if (auto Begun = PlayUI->BeginFrame({.Width = static_cast<std::uint32_t>(Size.x), .Height = static_cast<std::uint32_t>(Size.y)}); !Begun)
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Play HUD hidden: {}", Begun.error().Message);
		PlayUI.reset();
		return;
	}

	const ImGuiIO& IO = ImGui::GetIO();
	const auto Pressed = [](const ImGuiKey Key)
	{
		return ImGui::IsKeyPressed(Key, true);
	};

	const bool bHovered = ImGui::IsMouseHoveringRect(Minimum, {Minimum.x + Size.x, Minimum.y + Size.y});
	const FGameUIInput MenuInput{
	    .Pointer = bPlayPaused && bHovered ? std::optional{FVector2{IO.MousePos.x - Minimum.x, IO.MousePos.y - Minimum.y}} : std::nullopt,
	    .bClicked = bPlayPaused && bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
	    .bUp = bPlayPaused && (Pressed(ImGuiKey_UpArrow) || Pressed(ImGuiKey_W) || IsPlayGamepadPressed(EGamepadButton::DpadUp)),
	    .bDown = bPlayPaused && (Pressed(ImGuiKey_DownArrow) || Pressed(ImGuiKey_S) || IsPlayGamepadPressed(EGamepadButton::DpadDown)),
	    .bActivate = bPlayPaused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) || IsPlayGamepadPressed(EGamepadButton::South)),
	};

	const std::array Labels{Localization.Format(Text::Game::Menu_Resume), Localization.Format(Text::Game::Menu_Restart), Localization.Format(Text::Game::Menu_Exit)};
	const std::array<std::string_view, 3> Items{Labels[0], Labels[1], Labels[2]};
	if (!bPlayPaused)
	{
		DrawCombatHud(*PlayUI, PlaySession->GetCombatView());
	}

	PlayMenuChoice = DrawGameHud(*PlayUI, Localization, bPlayPaused, Items, PlayMenuFocus, MenuInput, "Shift+Esc");

	// GameUI emits textured quads in screen pixels; ImGui draws them over the viewport with the same registered textures.
	const FScreenDrawList Draws = PlayUI->GetDrawList();
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const auto Point = [&Draws, Minimum](const std::uint16_t Index)
	{
		return ImVec2{Minimum.x + Draws.Vertices[Index].X, Minimum.y + Draws.Vertices[Index].Y};
	};

	const auto Uv = [&Draws](const std::uint16_t Index)
	{
		return ImVec2{Draws.Vertices[Index].U, Draws.Vertices[Index].V};
	};

	for (const FScreenDrawCommand& Command : Draws.Commands)
	{
		Draw->PushTexture(ImTextureRef(static_cast<ImTextureID>(Command.TextureId)));
		for (std::uint32_t Quad = Command.FirstIndex; Quad + 6 <= Command.FirstIndex + Command.IndexCount; Quad += 6)
		{
			// Each quad is the triangles (0, 1, 2) and (0, 2, 3) over its four corners.
			const std::uint16_t A = Draws.Indices[Quad];
			const std::uint16_t B = Draws.Indices[Quad + 1];
			const std::uint16_t C = Draws.Indices[Quad + 2];
			const std::uint16_t D = Draws.Indices[Quad + 5];
			Draw->PrimReserve(6, 4);
			Draw->PrimQuadUV(Point(A), Point(B), Point(C), Point(D), Uv(A), Uv(B), Uv(C), Uv(D), Draws.Vertices[A].Color);
		}

		Draw->PopTexture();
	}
}

void FEditorFramework::FImplementation::UpdateSimulation(const float DeltaSeconds)
{
	if (!Simulation.IsRunning())
	{
		return;
	}

	if (const auto Result = Simulation.Update(DeltaSeconds); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Simulation stopped: {}", Result.error().Message);
		ToggleSimulation();
		return;
	}

	for (const FLevelTriggerEvent& Event : Simulation.TakeTriggerEvents())
	{
		HERTA_LOG_INFO(*Log, EditorLog, "{} {} {}", PreviewObjects[Event.ObjectIndex].Label, Event.bEntered ? "entered" : "left", PreviewObjects[Event.TriggerObjectIndex].Label);
	}

	std::vector<FObjectId> Overrides;
	Overrides.reserve(Simulation.GetTransforms().size());
	for (const FLevelSimulationTransform& Snapshot : Simulation.GetTransforms())
	{
		const FTransform& Transform = Snapshot.Transform;
		FPreviewObject& Object = PreviewObjects[Snapshot.ObjectIndex];
		Overrides.push_back(Object.Id);
		Object.Translation = ToIm3dVector(Transform.Translation);
		Object.Scale = ToIm3dVector(Transform.Scale3D);
		const FMatrix3 Rotation = FMatrix3::Rotation(Transform.Rotation);

		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			for (std::size_t Row = 0; Row < 3; ++Row)
			{
				Object.Rotation(static_cast<int>(Row), static_cast<int>(Column)) = Rotation(Row, Column);
			}
		}
	}

	if (auto Result = Level->UpdatePreviewHierarchy(Overrides); !Result)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Simulation stopped: {}", Result.error().Message);
		ToggleSimulation();
	}
}

namespace
{
// Reads a tool call's arguments; a malformed object reads as empty, so every argument falls back to its default.
class FAutomationArguments
{
public:
	explicit FAutomationArguments(const std::string& Text)
	{
		simdjson::dom::element Root;
		bValid = !Parser.parse(Text).get(Root) && !Root.get_object().get(Object);
	}

	[[nodiscard]] double Number(const std::string_view Key, const double Default) const
	{
		double Value = Default;
		if (bValid && Object[Key].get_double().get(Value))
		{
			std::int64_t Integer = 0;
			return Object[Key].get_int64().get(Integer) ? Default : static_cast<double>(Integer);
		}

		return std::isfinite(Value) ? Value : Default;
	}

	[[nodiscard]] bool Has(const std::string_view Key) const
	{
		return bValid && !Object[Key].error();
	}

	[[nodiscard]] bool Flag(const std::string_view Key) const
	{
		bool bValue = false;
		return bValid && !Object[Key].get_bool().get(bValue) && bValue;
	}

	[[nodiscard]] std::string_view Text(const std::string_view Key) const
	{
		std::string_view Value;
		return bValid && !Object[Key].get_string().get(Value) ? Value : std::string_view{};
	}

	// A JSON array of Count numbers, or empty when the key is missing, not such an array, or not finite.
	[[nodiscard]] std::optional<std::array<double, 3>> Numbers(const std::string_view Key, const std::size_t Count) const
	{
		simdjson::dom::array Array;
		if (!bValid || Count > 3 || Object[Key].get_array().get(Array) || Array.size() != Count)
		{
			return std::nullopt;
		}

		std::array<double, 3> Values{};
		std::size_t Index = 0;
		for (const auto Element : Array)
		{
			double Value = 0.;
			if (Element.get_double().get(Value) || !std::isfinite(Value))
			{
				return std::nullopt;
			}

			Values[Index++] = Value;
		}

		return Values;
	}

	[[nodiscard]] std::array<double, 2> Pair(const std::string_view Key) const
	{
		std::array<double, 2> Values{};
		simdjson::dom::array Array;
		if (!bValid || Object[Key].get_array().get(Array) || Array.size() != 2)
		{
			return Values;
		}

		std::size_t Index = 0;
		for (const auto Element : Array)
		{
			double Value = 0.;
			Values[Index++] = !Element.get_double().get(Value) && std::isfinite(Value) ? Value : 0.;
		}

		return Values;
	}

private:
	simdjson::dom::parser Parser;
	simdjson::dom::object Object;
	bool bValid = false;
};

void AppendVector(std::string& Output, const FVector3& Value)
{
	Output += std::format("[{:.4f}, {:.4f}, {:.4f}]", Value.X, Value.Y, Value.Z);
}
}

void FEditorFramework::FImplementation::FinishAutomationFrame()
{
	FAutomationRun& Run = AutomationRun;
	const FCharacterMover& Mover = PlaySession->GetMover();
	const FMoverState& State = Mover.GetState();
	const std::string Mode(Mover.GetActiveMode());
	++Run.Frames;
	Run.bWasAirborne |= !State.bGrounded;
	for (const std::string& Event : PlaySession->GetCharacterEvents())
	{
		const auto Found = std::ranges::find(Run.AnimationEvents, Event, &std::pair<std::string, std::size_t>::first);
		if (Found == Run.AnimationEvents.end())
		{
			Run.AnimationEvents.emplace_back(Event, 1);
		}
		else
		{
			++Found->second;
		}
	}

	if (Run.TraceEvery > 0 && (Run.Frames % Run.TraceEvery == 0 || Mode != Run.LastMode))
	{
		Run.Trace += Run.Trace.empty() ? "" : ", ";
		Run.Trace += std::format("{{\"frame\": {}, \"mode\": ", Run.Frames);
		AppendJsonString(Run.Trace, Mode);
		Run.Trace += ", \"foot\": ";
		AppendVector(Run.Trace, State.Foot);
		Run.Trace += ", \"velocity\": ";
		AppendVector(Run.Trace, State.Velocity);
		Run.Trace += std::format(", \"grounded\": {}, \"wall\": {}, \"animationState\": ", State.bGrounded ? "true" : "false", State.Ground.bTouchingWall ? "true" : "false");
		AppendJsonString(Run.Trace, PlaySession->GetCharacterStateName());
		Run.Trace += ", \"animationEvents\": [";
		for (const std::string& Event : PlaySession->GetCharacterEvents())
		{
			Run.Trace += Run.Trace.ends_with('[') ? "" : ", ";
			AppendJsonString(Run.Trace, Event);
		}

		Run.Trace += "]}";
	}

	Run.LastMode = Mode;

	// A steered run ends on arriving, and every run can end on its condition.
	if (!AutomationInput.empty() && AutomationInput.front().SteerTo)
	{
		const FVector2 Target = *AutomationInput.front().SteerTo;
		if (std::hypot(Target.X - State.Foot.X, Target.Y - State.Foot.Z) < 0.3f)
		{
			Run.StoppedBy = "arrived";
		}
	}

	const std::string_view Until = Run.Until;
	const bool bMet = (Until == "grounded" && State.bGrounded) || (Until == "airborne" && !State.bGrounded) || (Until == "landed" && Run.bWasAirborne && State.bGrounded)
	                  || (Until == "mode_changed" && Mode != Run.StartMode) || (Until.starts_with("mode:") && Mode == Until.substr(5));
	if (bMet)
	{
		Run.StoppedBy = "until";
	}

	if (!Run.StoppedBy.empty())
	{
		AutomationInput.clear();
	}
}

std::string FEditorFramework::FImplementation::DescribePlay() const
{
	std::string Output = std::format("{{\"playing\": {}", PlaySession ? "true" : "false");
	if (PlaySession)
	{
		Output += std::format(", \"paused\": {}, \"grounded\": {}, \"crouched\": {}, \"sliding\": {}, \"mantling\": {}, \"wallRunning\": {}, \"ragdoll\": {}, \"thirdPerson\": {}", bPlayPaused ? "true" : "false", PlaySession->IsPlayerGrounded() ? "true" : "false",
		    PlaySession->IsPlayerCrouched() ? "true" : "false", PlaySession->IsPlayerSliding() ? "true" : "false", PlaySession->IsPlayerMantling() ? "true" : "false", PlaySession->IsPlayerWallRunning() ? "true" : "false", PlaySession->IsPlayerRagdolled() ? "true" : "false", PlaySession->IsThirdPerson() ? "true" : "false");
		Output += ", \"foot\": ";
		AppendVector(Output, PlaySession->GetPlayerFootPosition());
		Output += ", \"velocity\": ";
		AppendVector(Output, PlaySession->GetPlayerVelocity());
		Output += std::format(", \"yaw\": {:.4f}", PlaySession->GetPlayerYaw());
		const FCharacterMover& Mover = PlaySession->GetMover();
		const FMoverState& State = Mover.GetState();
		Output += ", \"mode\": ";
		AppendJsonString(Output, Mover.GetActiveMode());
		Output += std::format(", \"speed\": {:.3f}, \"touchingWall\": {}, \"wallNormal\": ", std::hypot(State.Velocity.X, State.Velocity.Z), State.Ground.bTouchingWall ? "true" : "false");
		AppendVector(Output, State.Ground.WallNormal);
		Output += ", \"groundNormal\": ";
		AppendVector(Output, State.Ground.GroundNormal);
		// The view is a rotation followed by a translation, so the eye is the transposed rotation applied to minus the translation.
		const FMatrix4& View = PlayRenderView.View;
		const FVector3 Translation{View(0, 3), View(1, 3), View(2, 3)};
		const FVector3 Eye = -FVector3{View(0, 0) * Translation.X + View(1, 0) * Translation.Y + View(2, 0) * Translation.Z, View(0, 1) * Translation.X + View(1, 1) * Translation.Y + View(2, 1) * Translation.Z, View(0, 2) * Translation.X + View(1, 2) * Translation.Y + View(2, 2) * Translation.Z};
		Output += ", \"camera\": ";
		AppendVector(Output, Eye);
		Output += std::format(", \"cameraDistance\": {:.3f}", (Eye - (State.Foot + FVector3::Up() * 0.95f)).Length());
		Output += ", \"animationState\": ";
		AppendJsonString(Output, PlaySession->GetCharacterStateName());
		const FGameObjective& Objective = PlaySession->GetObjective();
		Output += std::format(", \"objective\": {{\"state\": {}, \"seconds\": {:.3f}}}", static_cast<int>(Objective.State), Objective.ElapsedSeconds);
		Output += std::format(", \"characters\": {}, \"cuts\": {}, \"cutPieces\": {}", PlaySession->GetCharacterCount(), PlaySession->GetCutCount(), PlaySession->GetCutPieceCount());
		const FParticleStats Particles = PlaySession->GetParticleStats();
		Output += std::format(", \"particles\": {{\"effects\": {}, \"live\": {}, \"spawned\": {}, \"dropped\": {}, \"retired\": {}, \"collisions\": {}}}", Particles.Effects, Particles.Particles, Particles.Spawned, Particles.Dropped, Particles.EffectsRetired, Particles.Collisions);
		Output += ", \"agents\": [";
		const std::vector<FGameAgentState> Agents = PlaySession->GetAgentStates();
		for (std::size_t Index = 0; Index < Agents.size(); ++Index)
		{
			const FGameAgentState& Agent = Agents[Index];
			Output += Index == 0 ? "{\"name\": " : ", {\"name\": ";
			AppendJsonString(Output, Agent.Name);
			Output += ", \"state\": ";
			AppendJsonString(Output, Agent.State);
			Output += std::format(", \"foot\": [{:.3f}, {:.3f}, {:.3f}], \"yaw\": {:.3f}, \"health\": {:.2f}, \"sees\": {}, \"remembers\": {}, \"onNavigation\": {}, \"attacking\": {}}}", Agent.Foot.X, Agent.Foot.Y, Agent.Foot.Z, Agent.Yaw, Agent.Health, Agent.bSeesPlayer ? "true" : "false",
			    Agent.bRemembersPlayer ? "true" : "false", Agent.bOnNavigation ? "true" : "false", Agent.bAttacking ? "true" : "false");
		}

		Output += "]";
		if (const FNavigationWorld* const PlayNavigation = PlaySession->GetNavigation())
		{
			const FNavigationStats Loaded = PlayNavigation->GetStats();
			Output += std::format(", \"navigation\": {{\"sources\": {}, \"tiles\": {}, \"polygons\": {}, \"obstacles\": {}, \"pendingObstacles\": {}, \"agents\": {}}}", Loaded.Sources, Loaded.Tiles, Loaded.Polygons, Loaded.Obstacles, Loaded.PendingObstacles, Loaded.Agents);
		}

		const FRegionStats Regions = PlaySession->GetRegionStats();
		Output += std::format(", \"regions\": {{\"loaded\": {}, \"loading\": {}, \"preparing\": {}, \"loads\": {}, \"unloads\": {}, \"cancelled\": {}, \"failed\": {}, \"deferred\": {}, \"lastError\": ", Regions.Loaded, Regions.Loading, Regions.Preparing, Regions.Loads, Regions.Unloads, Regions.Cancelled, Regions.Failed, Regions.Deferred);
		AppendJsonString(Output, Regions.LastError);
		Output += "}";
	}

	if (Audio)
	{
		Output += ", \"audio\": " + DescribeAudio(Audio->GetStats());
	}

	return Output + "}";
}

std::string DescribeAudio(const FAudioStats& Stats)
{
	std::string Output = std::format("{{\"device\": ");
	AppendJsonString(Output, Stats.Device);
	Output += std::format(", \"null\": {}, \"sounds\": {}, \"voices\": {}, \"audibleVoices\": {}, \"streams\": {}, \"started\": {}, \"stolen\": {}, \"rejected\": {}, \"missing\": {}, \"starvedFrames\": {}, \"commands\": {}, \"dropped\": {}",
	    Stats.bNullDevice ? "true" : "false", Stats.Sounds, Stats.Voices, Stats.AudibleVoices, Stats.Streams, Stats.VoicesStarted, Stats.VoicesStolen, Stats.VoicesRejected, Stats.MissingSounds, Stats.StreamStarvedFrames, Stats.CommandsSubmitted, Stats.CommandsDropped);
	Output += std::format(", \"blockMicroseconds\": {:.1f}, \"blockPeakMicroseconds\": {:.1f}, \"blockBudgetMicroseconds\": {:.1f}, \"buses\": {{", Stats.BlockMicrosecondsAverage, Stats.BlockMicrosecondsPeak, Stats.BlockBudgetMicroseconds);
	for (std::size_t Bus = 0; Bus < Stats.Buses.size(); ++Bus)
	{
		Output += Bus == 0 ? "" : ", ";
		AppendJsonString(Output, Stats.Buses[Bus].Name);
		Output += std::format(": {{\"volume\": {:.3f}, \"peak\": {:.4f}}}", Stats.Buses[Bus].Volume, Stats.Buses[Bus].Peak);
	}

	return Output + "}}";
}

bool FEditorFramework::HandleAutomation(FAutomationServer& Server, const FAutomationRequest& Request)
{
	FImplementation& Editor = *Implementation;
	const FAutomationArguments Arguments(Request.Arguments);
	const std::string_view Tool = Request.Tool;
	if (Tool == "editor_status")
	{
		std::string Output = "{\"level\": ";
		const std::u8string LevelPath = Editor.Level ? Editor.Level->GetPath().generic_u8string() : std::u8string();
		AppendJsonString(Output, std::string_view(reinterpret_cast<const char*>(LevelPath.data()), LevelPath.size()));
		const FExtent2D Extent = Editor.ViewportExtent;
		Output += std::format(", \"entities\": {}, \"viewport\": [{}, {}], \"writes\": {}, \"play\": ", Editor.Level ? Editor.Level->GetWorld().SnapshotEntities().size() : 0, Extent.Width, Extent.Height, Server.AllowsWrites() ? "true" : "false");
		Output += Editor.DescribePlay();
		Server.Respond(Request.Id, Output + "}");
		return true;
	}

	if (Tool == "editor_log")
	{
		const auto Since = static_cast<std::uint64_t>(std::max(0., Arguments.Number("since", 0.)));
		const auto Limit = static_cast<std::size_t>(std::clamp(Arguments.Number("limit", 200.), 1., 2000.));
		const FLogReadResult Read = Editor.Log->ReadEditorBuffer({.NextSequence = Since, .Generation = static_cast<std::uint64_t>(std::max(0., Arguments.Number("generation", 0.)))}, Limit);
		std::string Output = "{\"lines\": [";
		for (std::size_t Index = 0; Index < Read.Records.size(); ++Index)
		{
			const FLogRecord& Record = Read.Records[Index];
			Output += Index == 0 ? "" : ", ";
			AppendJsonString(Output, std::format("[{}] [{}] {}", GetLogLevelName(Record.Level), Record.Category, Record.Message));
		}

		Output += std::format("], \"since\": {}, \"generation\": {}, \"truncated\": {}}}", Read.NextCursor.NextSequence, Read.NextCursor.Generation, Read.bHistoryTruncated ? "true" : "false");
		Server.Respond(Request.Id, Output);
		return true;
	}

	if (Tool == "editor_commands")
	{
		std::string Output = "{\"commands\": [";
		const std::vector<FEditorCommandInfo> Listed = Editor.EditorCommands->List();
		for (std::size_t Index = 0; Index < Listed.size(); ++Index)
		{
			Output += Index == 0 ? "{\"name\": " : ", {\"name\": ";
			AppendJsonString(Output, Listed[Index].Name);
			Output += ", \"description\": ";
			AppendJsonString(Output, Listed[Index].Description);
			Output += std::format(", \"mutates\": {}}}", Listed[Index].bMutates ? "true" : "false");
		}

		Server.Respond(Request.Id, Output + std::format("], \"writes\": {}}}", Server.AllowsWrites() ? "true" : "false"));
		return true;
	}

	// Runs as the Output Log would; a read-only session gets read_only for anything that changes state, before it runs.
	if (Tool == "editor_command")
	{
		const std::string_view Line = Arguments.Text("command");
		auto Result = Editor.EditorCommands->Execute(Line, Server.AllowsWrites());
		if (!Result)
		{
			Server.Fail(Request.Id, GetEditorCommandErrorCodeName(Result.error().Code), Result.error().Message);
			return true;
		}

		HERTA_LOG_INFO(*Editor.Log, EditorLog, "Automation ran: {}", Line);
		std::string Output = std::format("{{\"exitCode\": {}, \"message\": ", Result->ExitCode);
		AppendJsonString(Output, Result->Message);
		Server.Respond(Request.Id, Output + "}");
		return true;
	}

	if (Tool == "level_entities")
	{
		if (!Editor.Level)
		{
			Server.Fail(Request.Id, "no_level", "No level is open");
			return true;
		}

		const std::string_view Filter = Arguments.Text("name_contains");
		const auto Limit = static_cast<std::size_t>(std::clamp(Arguments.Number("limit", 100.), 1., 5000.));
		std::string Output = "{\"entities\": [";
		std::size_t Count = 0;
		std::size_t Matched = 0;
		for (const FLevelEntity& Entity : Editor.Level->GetWorld().SnapshotEntities())
		{
			if (!Filter.empty() && Entity.Name.find(Filter) == std::string::npos)
			{
				continue;
			}

			++Matched;
			if (Count == Limit)
			{
				continue;
			}

			Output += Count++ == 0 ? "{\"id\": " : ", {\"id\": ";
			AppendJsonString(Output, Entity.Id.ToString());
			Output += ", \"name\": ";
			AppendJsonString(Output, Entity.Name);
			const FLevelTransform& Transform = Entity.Transform;
			Output += std::format(", \"parent\": {}, \"translation\": [{:.3f}, {:.3f}, {:.3f}], \"rotation\": [{:.4f}, {:.4f}, {:.4f}, {:.4f}], \"scale\": [{:.3f}, {:.3f}, {:.3f}], \"components\": [", Entity.Parent.IsValid() ? "true" : "false",
			    Transform.Translation.Meters.X, Transform.Translation.Meters.Y, Transform.Translation.Meters.Z, Transform.Rotation.X, Transform.Rotation.Y, Transform.Rotation.Z, Transform.Rotation.W, Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z);
			const std::array<std::pair<std::string_view, bool>, 10> Present{{
			    {"staticMesh", Entity.Mesh.has_value()},
			    {"rigidBody", Entity.BodyType != ELevelBodyType::None},
			    {"light", Entity.Light.has_value()},
			    {"skyAtmosphere", Entity.SkyAtmosphere.has_value()},
			    {"heightFog", Entity.HeightFog.has_value()},
			    {"softBody", Entity.SoftBody.has_value()},
			    {"mover", Entity.Mover.has_value()},
			    {"trigger", Entity.Trigger.has_value()},
			    {"constraint", Entity.Constraint.has_value()},
			    {"destructible", Entity.Destructible.has_value()},
			}};
			std::vector<std::string_view> Components;
			for (const auto& [Key, bPresent] : Present)
			{
				if (bPresent)
				{
					Components.push_back(Key);
				}
			}

			for (std::size_t Index = 0; Index < Components.size(); ++Index)
			{
				Output += Index == 0 ? "" : ", ";
				AppendJsonString(Output, Components[Index]);
			}

			Output += "]}";
		}

		Server.Respond(Request.Id, Output + std::format("], \"matched\": {}}}", Matched));
		return true;
	}

	if (Tool == "viewport_view")
	{
		if (const std::string_view Focus = Arguments.Text("focus"); !Focus.empty())
		{
			Editor.PendingFocusPanel = std::string(Focus);
		}

		if (Arguments.Has("navigation"))
		{
			Editor.bNavigationVisible = Arguments.Flag("navigation");
		}

		if (Arguments.Has("collision"))
		{
			Editor.bCollisionVisible = Arguments.Flag("collision");
		}

		const auto Position = Arguments.Numbers("position", 3);
		const auto Target = Arguments.Numbers("target", 3);
		if (Position && Target)
		{
			const FVector3 From{static_cast<float>((*Position)[0]), static_cast<float>((*Position)[1]), static_cast<float>((*Position)[2])};
			const FVector3 Direction = FVector3{static_cast<float>((*Target)[0]), static_cast<float>((*Target)[1]), static_cast<float>((*Target)[2])} - From;
			Editor.ViewportCamera.SetView(From, std::atan2(Direction.X, Direction.Z), std::atan2(Direction.Y, std::hypot(Direction.X, Direction.Z)));
		}

		const FVector3& Camera = Editor.ViewportCamera.GetPosition();
		std::string Output = std::format(R"({{"navigation": {}, "collision": {}, "camera": {{"position": [{:.3f}, {:.3f}, {:.3f}], "yaw": {:.4f}, "pitch": {:.4f}}}, "navigationBuild": {{"building": {}, "built": {}, "triangles": {}, "milliseconds": {:.1f}, "error": )",
		    Editor.bNavigationVisible ? "true" : "false", Editor.bCollisionVisible ? "true" : "false", Camera.X, Camera.Y, Camera.Z, Editor.ViewportCamera.GetYaw(), Editor.ViewportCamera.GetPitch(), Editor.Navigation && Editor.Navigation->IsBuilding() ? "true" : "false",
		    Editor.Navigation && Editor.Navigation->GetFile() ? "true" : "false", Editor.Navigation ? Editor.Navigation->GetDebugTriangles().size() / 3 : 0, Editor.Navigation ? Editor.Navigation->GetBuildMilliseconds() : 0.);
		AppendJsonString(Output, Editor.Navigation ? Editor.Navigation->GetError() : std::string{});
		Server.Respond(Request.Id, Output + "}}");
		return true;
	}

	if (Tool == "play_state")
	{
		Server.Respond(Request.Id, Editor.DescribePlay());
		return true;
	}

	if (Tool == "play_teleport")
	{
		const auto Position = Arguments.Numbers("position", 3);
		if (!Editor.PlaySession || !Position)
		{
			Server.Fail(Request.Id, "invalid", Editor.PlaySession ? "position must be [x, y, z] in meters" : "Start Play with play_start first");
			return true;
		}

		const float Yaw = Arguments.Has("yaw") ? static_cast<float>(Arguments.Number("yaw", 0.)) : Editor.PlaySession->GetPlayerYaw();
		if (auto Placed = Editor.PlaySession->TeleportPlayer({static_cast<float>((*Position)[0]), static_cast<float>((*Position)[1]), static_cast<float>((*Position)[2])}, Yaw); !Placed)
		{
			Server.Fail(Request.Id, "teleport_failed", Placed.error().Message);
			return true;
		}

		Server.Respond(Request.Id, Editor.DescribePlay());
		return true;
	}

	if (Tool == "play_start")
	{
		// Starting elsewhere places the player once Play is up; a running Play only moves when asked to.
		const auto At = Arguments.Numbers("at", 3);
		Editor.AutomationPlayAt = At ? std::optional(FVector3{static_cast<float>((*At)[0]), static_cast<float>((*At)[1]), static_cast<float>((*At)[2])}) : std::nullopt;
		Editor.AutomationPlayYaw = static_cast<float>(Arguments.Number("yaw", 0.));
		if (Editor.PlaySession && !At)
		{
			Server.Respond(Request.Id, Editor.DescribePlay());
			return true;
		}

		// Play may wait for meshes or the character to load, so keep trying for a while before giving up.
		Editor.AutomationPlayRequest = Request.Id;
		Editor.AutomationPlayDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		Editor.AutomationPlayRetry = {};
		return true;
	}

	if (Tool == "play_stop")
	{
		Editor.AutomationInput.clear();
		if (Editor.PlaySession)
		{
			Editor.TogglePlay();
		}

		Server.Respond(Request.Id, Editor.DescribePlay());
		return true;
	}

	if (Tool == "play_input")
	{
		if (!Editor.PlaySession)
		{
			Server.Fail(Request.Id, "not_playing", "Start Play with play_start first");
			return true;
		}

		if (Editor.AutomationInputRequest)
		{
			Server.Fail(Request.Id, "busy", "Earlier play_input is still running");
			return true;
		}

		const auto Frames = static_cast<std::size_t>(std::clamp(Arguments.Number("frames", 60.), 1., 3600.));
		const auto Steer = Arguments.Numbers("steer_to", 2);
		const std::optional<FVector2> SteerTo = Steer ? std::optional(FVector2{static_cast<float>((*Steer)[0]), static_cast<float>((*Steer)[1])}) : std::nullopt;
		const std::string Until(Arguments.Text("until"));
		if (!Until.empty() && Until != "grounded" && Until != "airborne" && Until != "landed" && Until != "mode_changed" && !Until.starts_with("mode:"))
		{
			Server.Fail(Request.Id, "invalid", "until must be grounded, airborne, landed, mode_changed, or mode:<Name>");
			return true;
		}

		Editor.AutomationRun = {.Until = Until, .TraceEvery = static_cast<std::size_t>(std::clamp(Arguments.Number("trace", 0.), 0., 3600.)), .StartMode = std::string(Editor.PlaySession->GetMover().GetActiveMode())};
		Editor.AutomationRun.LastMode = Editor.AutomationRun.StartMode;
		const auto Move = Arguments.Pair("move");
		const auto Look = Arguments.Pair("look");
		// Facing a point turns all the way on the first frame, measured from where the player stands now, before any look spread.
		float FaceDelta = 0.f;
		if (Arguments.Has("face"))
		{
			const auto Face = Arguments.Pair("face");
			const FVector3 Foot = Editor.PlaySession->GetPlayerFootPosition();
			const float Target = std::atan2(static_cast<float>(Face[0]) - Foot.X, static_cast<float>(Face[1]) - Foot.Z);
			FaceDelta = std::remainder(Target - Editor.PlaySession->GetPlayerYaw(), 2.f * std::numbers::pi_v<float>);
		}

		for (std::size_t Frame = 0; Frame < Frames; ++Frame)
		{
			const FGameInput Input{
			    .Move = {static_cast<float>(std::clamp(Move[0], -1., 1.)), static_cast<float>(std::clamp(Move[1], -1., 1.))},
			    .YawDelta = static_cast<float>(Look[0] / static_cast<double>(Frames)) + (Frame == 0 ? FaceDelta : 0.f),
			    .PitchDelta = static_cast<float>(Look[1] / static_cast<double>(Frames)),
			    .bJump = Frame == 0 && Arguments.Flag("jump"),
			    .bSprint = Arguments.Flag("sprint"),
			    .bCrouch = Arguments.Flag("crouch"),
			    .bThrow = Frame == 0 && Arguments.Flag("throw"),
			    .bToggleCamera = Frame == 0 && Arguments.Flag("toggle_camera"),
			    .bToggleRagdoll = Frame == 0 && Arguments.Flag("ragdoll"),
			    .bAttack = Frame == 0 && Arguments.Flag("attack"),
			    .bBlock = Arguments.Flag("block"),
			};

			Editor.AutomationInput.push_back({.Input = Input, .SteerTo = SteerTo});
		}

		Editor.AutomationInputRequest = Request.Id;
		return true;
	}

	return false;
}

void FEditorFramework::UpdateAutomation(FAutomationServer& Server)
{
	FImplementation& Editor = *Implementation;
	if (Editor.AutomationPlayRequest)
	{
		const auto Now = std::chrono::steady_clock::now();
		if (Editor.PlaySession)
		{
			Editor.bAutomationOwnsPlay = true;
			if (const auto At = std::exchange(Editor.AutomationPlayAt, std::nullopt))
			{
				if (auto Placed = Editor.PlaySession->TeleportPlayer(*At, Editor.AutomationPlayYaw); !Placed)
				{
					Server.Fail(*std::exchange(Editor.AutomationPlayRequest, std::nullopt), "teleport_failed", Placed.error().Message);
					return;
				}
			}

			Server.Respond(*std::exchange(Editor.AutomationPlayRequest, std::nullopt), Editor.DescribePlay());
		}
		else if (Now >= Editor.AutomationPlayDeadline)
		{
			Server.Fail(*std::exchange(Editor.AutomationPlayRequest, std::nullopt), "play_not_started", "Play did not start within 20 seconds; read editor_log for the reason");
		}
		else if (Now >= Editor.AutomationPlayRetry)
		{
			Editor.AutomationPlayRetry = Now + std::chrono::milliseconds(250);
			Editor.TogglePlay();
		}
	}

	if (Editor.AutomationInputRequest && (Editor.AutomationInput.empty() || !Editor.PlaySession))
	{
		Editor.AutomationInput.clear();
		std::string Reply = Editor.DescribePlay();
		const FImplementation::FAutomationRun& Run = Editor.AutomationRun;
		Reply.pop_back();
		Reply += std::format(", \"framesPlayed\": {}, \"stoppedBy\": ", Run.Frames);
		AppendJsonString(Reply, Run.StoppedBy.empty() ? std::string_view("frames") : std::string_view(Run.StoppedBy));
		Reply += ", \"animationEvents\": {";
		for (const auto& [Name, Count] : Run.AnimationEvents)
		{
			Reply += Name == Run.AnimationEvents.front().first ? "" : ", ";
			AppendJsonString(Reply, Name);
			Reply += std::format(": {}", Count);
		}

		Reply += "}";
		Reply += Run.TraceEvery > 0 ? ", \"trace\": [" + Run.Trace + "]}" : "}";
		Server.Respond(*std::exchange(Editor.AutomationInputRequest, std::nullopt), Reply);
	}
}
}
