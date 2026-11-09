#include "EditorFrameworkPrivate.h"

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

	auto Session = FGameSession::Create(VisualEntities, *GameAssets, GraphicsDevice, {}, Tasks, std::move(Character));
	if (!Session)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not start play: {}", Session.error().Message);
		return;
	}

	for (const std::string& Warning : (*Session)->GetWarnings())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "{}", Warning);
	}

	if (!PlayUI && RegisterUITexture && UnregisterUITexture)
	{
		auto UI = FGameUI::Create({
		    .Device = GraphicsDevice,
		    .FontPath = EngineRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf",
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

	PlayAssets = std::move(GameAssets);
	PlaySession = std::move(*Session);
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
		}

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
		Input = AutomationInput.front();
		AutomationInput.pop_front();
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
		Input = {
		    .Move = {Axis(ImGuiKey_D, ImGuiKey_A), Axis(ImGuiKey_W, ImGuiKey_S)},
		    .YawDelta = -IO.MouseDelta.x * Sensitivity,
		    .PitchDelta = -IO.MouseDelta.y * Sensitivity,
		    .bJump = bPlayJumpArmed && ImGui::IsKeyPressed(ImGuiKey_Space, false),
		    .bSprint = IO.KeyShift,
		    .bCrouch = ImGui::IsKeyDown(ImGuiKey_C) || IO.KeyCtrl,
		    .bThrow = ImGui::IsKeyPressed(ImGuiKey_F, false),
		    .bToggleCamera = ImGui::IsKeyPressed(ImGuiKey_V, false),
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

	PlayRenderView = PlaySession->GetRenderView(ViewportExtent);
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

	constexpr std::array<std::string_view, 3> Items{"Resume", "Restart course", "Exit"};
	PlayMenuChoice = DrawGameHud(*PlayUI, PlaySession->GetObjective(), bPlayPaused, Items, PlayMenuFocus, MenuInput, "Shift+Esc");

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

std::string FEditorFramework::FImplementation::DescribePlay() const
{
	std::string Output = std::format("{{\"playing\": {}", PlaySession ? "true" : "false");
	if (PlaySession)
	{
		Output += std::format(", \"paused\": {}, \"grounded\": {}, \"crouched\": {}, \"sliding\": {}, \"mantling\": {}, \"wallRunning\": {}, \"thirdPerson\": {}", bPlayPaused ? "true" : "false", PlaySession->IsPlayerGrounded() ? "true" : "false",
		    PlaySession->IsPlayerCrouched() ? "true" : "false", PlaySession->IsPlayerSliding() ? "true" : "false", PlaySession->IsPlayerMantling() ? "true" : "false", PlaySession->IsPlayerWallRunning() ? "true" : "false", PlaySession->IsThirdPerson() ? "true" : "false");
		Output += ", \"foot\": ";
		AppendVector(Output, PlaySession->GetPlayerFootPosition());
		Output += ", \"velocity\": ";
		AppendVector(Output, PlaySession->GetPlayerVelocity());
		Output += std::format(", \"yaw\": {:.4f}", PlaySession->GetPlayerYaw());
		Output += ", \"animationState\": ";
		AppendJsonString(Output, PlaySession->GetCharacterStateName());
		const FGameObjective& Objective = PlaySession->GetObjective();
		Output += std::format(", \"objective\": {{\"state\": {}, \"seconds\": {:.3f}}}", static_cast<int>(Objective.State), Objective.ElapsedSeconds);
	}

	return Output + "}";
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

	if (Tool == "play_state")
	{
		Server.Respond(Request.Id, Editor.DescribePlay());
		return true;
	}

	if (Tool == "play_start")
	{
		if (Editor.PlaySession)
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
			Editor.AutomationInput.push_back({
			    .Move = {static_cast<float>(std::clamp(Move[0], -1., 1.)), static_cast<float>(std::clamp(Move[1], -1., 1.))},
			    .YawDelta = static_cast<float>(Look[0] / static_cast<double>(Frames)) + (Frame == 0 ? FaceDelta : 0.f),
			    .PitchDelta = static_cast<float>(Look[1] / static_cast<double>(Frames)),
			    .bJump = Frame == 0 && Arguments.Flag("jump"),
			    .bSprint = Arguments.Flag("sprint"),
			    .bCrouch = Arguments.Flag("crouch"),
			    .bThrow = Frame == 0 && Arguments.Flag("throw"),
			    .bToggleCamera = Frame == 0 && Arguments.Flag("toggle_camera"),
			});
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
		Server.Respond(*std::exchange(Editor.AutomationInputRequest, std::nullopt), Editor.DescribePlay());
	}
}
}
