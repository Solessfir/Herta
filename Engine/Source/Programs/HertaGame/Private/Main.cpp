#include "Herta/Application/Application.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameHud.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/VisualEnvironment.h"
#include "Herta/GameUI/GameUI.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/NvrhiVulkan/NvrhiVulkan.h"
#include "Herta/Platform/FileSystem.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"
#include "Herta/Project/Project.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
inline constexpr FLogCategory GameLog{.Name = "Game"};

void ReportFailure(const std::string_view Message) noexcept
{
	try
	{
		std::println(stderr, "Herta Game failed: {}", Message);
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// Reporting must not let a formatting exception escape main.
	}
}

[[nodiscard]] std::string_view FindArgumentValue(const std::span<char* const> Arguments, const std::string_view Prefix) noexcept
{
	for (const char* const Argument : Arguments)
	{
		if (Argument != nullptr && std::string_view(Argument).starts_with(Prefix))
		{
			return std::string_view(Argument).substr(Prefix.size());
		}
	}

	return {};
}

[[nodiscard]] bool HasArgument(const std::span<char* const> Arguments, const std::string_view Name) noexcept
{
	return std::ranges::any_of(Arguments, [Name](const char* const Argument)
	{
		return Argument != nullptr && Name == Argument;
	});
}

// A staged build keeps one project folder next to the executable.
[[nodiscard]] std::optional<std::filesystem::path> FindStagedProject(const std::filesystem::path& Directory)
{
	std::error_code Error;
	for (const auto& Entry : std::filesystem::directory_iterator(Directory, Error))
	{
		if (!Entry.is_directory(Error))
		{
			continue;
		}

		for (const auto& File : std::filesystem::directory_iterator(Entry.path(), Error))
		{
			if (File.path().extension() == ".hertaproject")
			{
				return File.path();
			}
		}
	}

	return std::nullopt;
}

// Display preferences are per user, not per install, so a shared or read-only install still remembers them.
struct FGameSettings
{
	bool bFullscreen = false;
	bool bHdr = false;
	// Zero follows what the operating system reports for the display.
	float PaperWhite = 0.f;
	float PeakLuminance = 0.f;
	float MouseSensitivity = 0.0022f;
	ERenderQuality Quality = ERenderQuality::High;
};

[[nodiscard]] FGameSettings LoadSettings(const std::filesystem::path& Path)
{
	FGameSettings Settings;
	std::ifstream File(Path);
	for (std::string Line; std::getline(File, Line);)
	{
		const std::size_t Separator = Line.find('=');
		if (Separator == std::string::npos)
		{
			continue;
		}

		const std::string_view Key = std::string_view(Line).substr(0, Separator);
		const std::string_view Value = std::string_view(Line).substr(Separator + 1);
		float Number = 0.f;
		const auto [End, Error] = std::from_chars(Value.data(), Value.data() + Value.size(), Number);
		if (Error != std::errc{} || End != Value.data() + Value.size() || !std::isfinite(Number))
		{
			continue;
		}

		if (Key == "fullscreen")
		{
			Settings.bFullscreen = Number != 0.f;
		}
		else if (Key == "hdr")
		{
			Settings.bHdr = Number != 0.f;
		}
		else if (Key == "paperWhite" && (Number == 0.f || (Number >= 80.f && Number <= 500.f)))
		{
			Settings.PaperWhite = Number;
		}
		else if (Key == "peakLuminance" && (Number == 0.f || (Number >= 250.f && Number <= 10000.f)))
		{
			Settings.PeakLuminance = Number;
		}
		else if (Key == "mouseSensitivity" && Number >= 0.0001f && Number <= 0.02f)
		{
			Settings.MouseSensitivity = Number;
		}
		else if (Key == "quality" && (Number == 0.f || Number == 1.f || Number == 2.f))
		{
			Settings.Quality = static_cast<ERenderQuality>(Number);
		}
	}

	return Settings;
}

void SaveSettings(const std::filesystem::path& Path, const FGameSettings& Settings, FLogService& Log)
{
	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	std::filesystem::path Temporary = Path;
	Temporary += ".tmp";
	{
		std::ofstream File(Temporary, std::ios::trunc);
		std::println(File, "fullscreen={}\nhdr={}\npaperWhite={}\npeakLuminance={}\nmouseSensitivity={}\nquality={}", Settings.bFullscreen ? 1 : 0, Settings.bHdr ? 1 : 0, Settings.PaperWhite, Settings.PeakLuminance, Settings.MouseSensitivity, static_cast<int>(Settings.Quality));
		if (!File)
		{
			HERTA_LOG_WARNING(Log, GameLog, "Could not save settings to {}", Path.generic_string());
			return;
		}
	}

	if (!MoveAtomically(Temporary, Path, EMoveMode::ReplaceExisting))
	{
		HERTA_LOG_WARNING(Log, GameLog, "Could not save settings to {}", Path.generic_string());
	}
}

[[nodiscard]] FExtent2D GetFramebufferExtent(const FWindow& Window) noexcept
{
	return {
	    .Width = static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferWidth())),
	    .Height = static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferHeight())),
	};
}

// Input gathered from window callbacks between frames.
struct FPendingInput
{
	FVector2 MouseDelta{};
	std::optional<FVector2> LastCursor{};
	std::optional<FVector2> Pointer{};
	bool bJump = false;
	bool bClicked = false;
	bool bEscape = false;
	bool bRestart = false;
	bool bToggleFullscreen = false;
	bool bMenuUp = false;
	bool bMenuDown = false;
	bool bMenuActivate = false;
	bool bFocusLost = false;
};

struct FGameOptions
{
	std::filesystem::path ExecutablePath;
	std::filesystem::path ProjectPath;
	// Plays a scripted run for this many frames and exits, for unattended checks.
	std::uint32_t SmokeFrames = 0;
	bool bValidation = false;
};

int RunGame(const FGameOptions& Options)
{
	const std::filesystem::path ExecutableDirectory = std::filesystem::absolute(Options.ExecutablePath).parent_path();
	const std::filesystem::path ProjectPath = !Options.ProjectPath.empty() ? Options.ProjectPath : FindStagedProject(ExecutableDirectory).value_or(std::filesystem::path{});
	const auto Project = LoadProject(ProjectPath);
	if (!Project)
	{
		ReportFailure(std::format("Cannot load the game project{}: {}. Stage one with HertaEditorCmd game.stage, or pass --project=<path>.", ProjectPath.empty() ? "" : " " + ProjectPath.generic_string(), Project.error().Message));
		return 1;
	}

	const bool bSmokeTest = Options.SmokeFrames > 0;
	const std::filesystem::path UserDirectory = GetUserSettingsDirectory() / "Herta" / Project->Descriptor.Name;
	FLogOptions LogOptions;
	LogOptions.FilePath = UserDirectory / "Logs" / "Game.log";
	auto LogResult = FLogService::Create(std::move(LogOptions));
	if (!LogResult)
	{
		ReportFailure(LogResult.error().Message);
		return 1;
	}

	std::unique_ptr<FLogService> Log = std::move(*LogResult);
	auto TaskResult = FTaskSystem::Create({.Log = Log.get()});
	if (!TaskResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start tasks: {}", TaskResult.error().Message);
		return 1;
	}

	std::unique_ptr<FTaskSystem> Tasks = std::move(*TaskResult);
	const auto Level = LoadLevel(Project->StartingLevel);
	if (!Level)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not load {}: {}", Project->StartingLevel.generic_string(), Level.error().Message);
		return 1;
	}

	const std::filesystem::path SettingsPath = UserDirectory / "Settings.ini";
	FGameSettings Settings = bSmokeTest ? FGameSettings{} : LoadSettings(SettingsPath);
	auto ApplicationResult = FApplication::Create(Log.get());
	if (!ApplicationResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start the application: {}", ApplicationResult.error().Message);
		return 1;
	}

	std::unique_ptr<FApplication> Application = std::move(*ApplicationResult);
	// Unattended runs must not take focus from whatever the user is doing.
	auto WindowResult = Application->CreateWindow({.Title = Project->Descriptor.Name, .bVisible = false, .bCustomTitleBar = false, .bFocusOnShow = !bSmokeTest});
	if (!WindowResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not create the window: {}", WindowResult.error().Message);
		return 1;
	}

	FWindow& Window = **WindowResult;
	const bool bShowBeforePresentation = Application->GetCapabilities().WindowSystem == EWindowSystem::Wayland;
	if (bShowBeforePresentation)
	{
		// Wayland must configure the surface before Vulkan attaches a swapchain image.
		Window.Show();
		Application->PumpEvents();
	}

	auto Extensions = Application->GetRequiredVulkanInstanceExtensions();
	if (!Extensions)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not query Vulkan presentation extensions: {}", Extensions.error().Message);
		return 1;
	}

	FNvrhiVulkanPresentationDescriptor PresentationDescriptor;
	PresentationDescriptor.ApplicationName = Project->Descriptor.Name;
	PresentationDescriptor.WindowBackendHandle = Window.GetBackendHandle().Value;
	PresentationDescriptor.RequiredInstanceExtensions = std::move(*Extensions);
	PresentationDescriptor.InitialExtent = GetFramebufferExtent(Window);
	PresentationDescriptor.bEnableValidation = Options.bValidation && GetBuildConfiguration() != EBuildConfiguration::Shipping;
	PresentationDescriptor.bRequireValidation = PresentationDescriptor.bEnableValidation;
	PresentationDescriptor.Log = Log.get();
	auto PresentationResult = CreateNvrhiVulkanPresentation(std::move(PresentationDescriptor));
	if (!PresentationResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start Vulkan presentation: {}", PresentationResult.error().Message);
		return 1;
	}

	std::unique_ptr<INvrhiVulkanPresentation> Presentation = std::move(*PresentationResult);
	IGraphicsDevice& Device = Presentation->GetGraphicsDevice();
	if (auto Initialized = Presentation->InitializeToolUIRenderer(); !Initialized)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start screen drawing: {}", Initialized.error().Message);
		return 1;
	}

	const std::filesystem::path ShaderDirectory = ExecutableDirectory / "Shaders";
	auto VertexShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.vert.hshader");
	auto InstancedVertexShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.instanced.vert.hshader");
	auto FragmentShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.frag.hshader");
	if (!VertexShader || !InstancedVertexShader || !FragmentShader)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not load the mesh shaders from {}", ShaderDirectory.generic_string());
		return 1;
	}

	FVisualShaderSet VisualShaders;
	const std::array VisualShaderFiles{
	    std::pair{"SkyView.vert.hshader", &VisualShaders.FullscreenVertex},
	    std::pair{"Sky.frag.hshader", &VisualShaders.SkyFragment},
	    std::pair{"VolumetricFog.comp.hshader", &VisualShaders.FogCompute},
	    std::pair{"FogDenoise.comp.hshader", &VisualShaders.FogDenoiseCompute},
	    std::pair{"ToneMap.frag.hshader", &VisualShaders.ToneMapFragment},
	    std::pair{"ToneMapLut.frag.hshader", &VisualShaders.ToneMapLutFragment},
	    std::pair{"Shadow.vert.hshader", &VisualShaders.ShadowVertex},
	    std::pair{"Shadow.instanced.vert.hshader", &VisualShaders.ShadowInstancedVertex},
	    std::pair{"Shadow.frag.hshader", &VisualShaders.ShadowFragment},
	    std::pair{"SmaaEdges.frag.hshader", &VisualShaders.SmaaEdges},
	    std::pair{"SmaaWeights.frag.hshader", &VisualShaders.SmaaWeights},
	    std::pair{"SmaaNeighborhood.frag.hshader", &VisualShaders.SmaaNeighborhood},
	    std::pair{"SkyView.frag.hshader", &VisualShaders.SkyViewFragment},
	    std::pair{"Exposure.comp.hshader", &VisualShaders.ExposureCompute},
	    std::pair{"GtaoDepth.comp.hshader", &VisualShaders.GtaoDepthCompute},
	    std::pair{"Gtao.comp.hshader", &VisualShaders.GtaoCompute},
	    std::pair{"GtaoDenoise.comp.hshader", &VisualShaders.GtaoDenoiseCompute},
	    std::pair{"SmaaEdgeList.comp.hshader", &VisualShaders.SmaaEdgeListCompute},
	    std::pair{"SmaaEdgeWeights.comp.hshader", &VisualShaders.SmaaEdgeWeightsCompute},
	    std::pair{"LightClusters.comp.hshader", &VisualShaders.LightClustersCompute},
	    std::pair{"DepthHierarchy.comp.hshader", &VisualShaders.DepthHierarchyCompute},
	    std::pair{"OcclusionCull.comp.hshader", &VisualShaders.OcclusionCullCompute},
	    std::pair{"TexturedMesh.velocity.frag.hshader", &VisualShaders.DepthVelocityFragment},
	    std::pair{"MotionVectors.comp.hshader", &VisualShaders.MotionVectorsCompute},
	    std::pair{"DepthBounds.comp.hshader", &VisualShaders.DepthBoundsCompute},
	};

	for (const auto& [File, Destination] : VisualShaderFiles)
	{
		auto Shader = LoadCookedShader(ShaderDirectory / File);
		if (!Shader)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not load {}: {}", File, Shader.error().Message);
			return 1;
		}

		*Destination = std::move(*Shader);
	}

	auto RendererResult = FMeshRenderer::Create(Device, std::move(*VertexShader), std::move(*FragmentShader), {}, {}, {}, {}, std::move(*InstancedVertexShader), std::move(VisualShaders));
	if (!RendererResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start the renderer: {}", RendererResult.error().Message);
		return 1;
	}

	std::unique_ptr<FMeshRenderer> Renderer = std::move(*RendererResult);
	const std::filesystem::path EngineContent = Project->Root.parent_path() / "Engine" / "Content";
	const FCookedContent Content({Project->ContentRoot, EngineContent});
	auto AssetsResult = FStagedGameAssets::Create(Device, Content, Level->Entities);
	if (!AssetsResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not load game content: {}", AssetsResult.error().Message);
		return 1;
	}

	std::unique_ptr<FStagedGameAssets> Assets = std::move(*AssetsResult);
	auto SessionResult = FGameSession::Create(Level->Entities, *Assets, &Device);
	if (!SessionResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start the game: {}", SessionResult.error().Message);
		return 1;
	}

	std::unique_ptr<FGameSession> Session = std::move(*SessionResult);
	for (const std::string& Warning : Session->GetWarnings())
	{
		HERTA_LOG_WARNING(*Log, GameLog, "{}", Warning);
	}

	std::unique_ptr<FVisualEnvironment> Environment = FVisualEnvironment::Create(*Tasks, *Renderer, *Log);
	auto UIResult = FGameUI::Create({
	    .Device = &Device,
	    .FontPath = EngineContent / "Fonts" / "Roboto-Medium.ttf",
	    .RegisterTexture = [&Presentation](const FTextureHandle& Texture) -> std::expected<std::uint64_t, FGameUIError>
	{
		auto Registered = Presentation->RegisterToolUITexture(Texture);
		if (!Registered)
		{
			return std::unexpected(FGameUIError{std::move(Registered.error().Message)});
		}

		return *Registered;
	},
	    .UnregisterTexture = [&Presentation](const std::uint64_t TextureId)
	{
		Presentation->UnregisterToolUITexture(TextureId);
	},
	});

	if (!UIResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start the HUD: {}", UIResult.error().Message);
		return 1;
	}

	std::unique_ptr<FGameUI> UI = std::move(*UIResult);
	FPendingInput Pending;
	FWindowCallbacks Callbacks;
	Callbacks.FocusChanged = [&Pending](FWindow&, const bool bFocused)
	{
		Pending.bFocusLost |= !bFocused;
	};

	Callbacks.KeyChanged = [&Pending](FWindow&, const EKey Key, const EInputAction Action, const EModifierFlags Modifiers)
	{
		if (Action == EInputAction::Released)
		{
			return;
		}

		const bool bPressed = Action == EInputAction::Pressed;
		Pending.bJump |= bPressed && Key == EKey::Space;
		Pending.bEscape |= bPressed && Key == EKey::Escape;
		Pending.bRestart |= bPressed && Key == EKey::R;
		Pending.bToggleFullscreen |= bPressed && (Key == EKey::F11 || (Key == EKey::Enter && HasModifier(Modifiers, EModifierFlags::Alt)));
		Pending.bMenuUp |= Key == EKey::Up || Key == EKey::W;
		Pending.bMenuDown |= Key == EKey::Down || Key == EKey::S;
		Pending.bMenuActivate |= bPressed && (Key == EKey::Enter || Key == EKey::KeypadEnter || Key == EKey::Space) && !HasModifier(Modifiers, EModifierFlags::Alt);
	};

	Callbacks.MouseButtonChanged = [&Pending](FWindow&, const EMouseButton Button, const EInputAction Action, EModifierFlags)
	{
		Pending.bClicked |= Button == EMouseButton::Left && Action == EInputAction::Pressed;
	};

	Callbacks.CursorMoved = [&Pending](FWindow& Source, const double X, const double Y)
	{
		const FVector2 Position{static_cast<float>(X), static_cast<float>(Y)};
		if (Source.IsCursorCaptured() && Pending.LastCursor)
		{
			Pending.MouseDelta += Position - *Pending.LastCursor;
		}

		Pending.LastCursor = Position;
		Pending.Pointer = Source.IsCursorCaptured() ? std::nullopt : std::optional{Position};
	};

	Window.SetCallbacks(std::move(Callbacks));

	if (Settings.bFullscreen)
	{
		Window.SetFullscreen(true);
	}

	FTextureHandle RegisteredScene;
	std::uint64_t SceneTextureId = 0;
	bool bPaused = false;
	std::size_t MenuFocus = 0;
	std::optional<FDisplayLuminance> SystemLuminance;
	auto SystemLuminanceQuery = std::chrono::steady_clock::time_point{};
	auto LastFrame = std::chrono::steady_clock::now();
	std::uint32_t Frames = 0;
	double FrameMillisecondsSum = 0.;
	bool bFailed = false;
	HERTA_LOG_INFO(*Log, GameLog, "{} started in {} configuration", Project->Descriptor.Name, GetBuildConfigurationName(GetBuildConfiguration()));

	while (!bFailed && !Window.ShouldClose())
	{
		if (Window.IsMinimized())
		{
			// A minimized game draws nothing, so it waits instead of spinning.
			Window.SetCursorCaptured(false);
			bPaused = true;
		}

		Application->PumpEvents();
		Tasks->RunMainThreadTasks();
		const auto Now = std::chrono::steady_clock::now();
		const float DeltaSeconds = std::clamp(std::chrono::duration<float>(Now - LastFrame).count(), 0.f, 0.1f);
		FrameMillisecondsSum += std::chrono::duration<double, std::milli>(Now - LastFrame).count();
		LastFrame = Now;
		FPendingInput Input = std::exchange(Pending, FPendingInput{.LastCursor = Pending.LastCursor, .Pointer = Pending.Pointer});

		// A scripted run walks the course, jumps, pauses, and restarts to exercise the same paths a player does.
		FGameInput GameInput;
		if (bSmokeTest)
		{
			GameInput = {.Move = {0.f, 1.f}, .YawDelta = Frames % 240 < 120 ? 0.004f : -0.004f, .bJump = Frames % 90 == 45, .bSprint = Frames % 300 > 150, .bCrouch = Frames % 300 > 240};
			Input.bEscape = Frames == Options.SmokeFrames / 4 + 30 || Frames == Options.SmokeFrames / 2 || Frames == Options.SmokeFrames / 2 + 20;
			Input.bRestart = Frames == Options.SmokeFrames * 3 / 4;
			Input.bFocusLost = Frames == Options.SmokeFrames / 4;
		}

		if (Input.bToggleFullscreen)
		{
			Settings.bFullscreen = !Window.IsFullscreen();
			Window.SetFullscreen(Settings.bFullscreen);
			SaveSettings(SettingsPath, Settings, *Log);
		}

		// Losing focus pauses, so alt-tabbing never leaves the player running or the pointer trapped.
		if (Input.bFocusLost && !bPaused)
		{
			bPaused = true;
		}
		else if (Input.bEscape)
		{
			bPaused = !bPaused;
			MenuFocus = 0;
		}

		const bool bPlaying = !bPaused && (Window.IsFocused() || bSmokeTest);
		if (!bSmokeTest)
		{
			Window.SetCursorCaptured(bPlaying);
		}

		if (bPlaying && !bSmokeTest)
		{
			const auto Axis = [&Window](const EKey Positive, const EKey Negative)
			{
				return (Window.IsKeyDown(Positive) ? 1.f : 0.f) - (Window.IsKeyDown(Negative) ? 1.f : 0.f);
			};

			GameInput = {
			    .Move = {Axis(EKey::D, EKey::A), Axis(EKey::W, EKey::S)},
			    .YawDelta = -Input.MouseDelta.X * Settings.MouseSensitivity,
			    .PitchDelta = -Input.MouseDelta.Y * Settings.MouseSensitivity,
			    .bJump = Input.bJump,
			    .bSprint = Window.IsKeyDown(EKey::LeftShift) || Window.IsKeyDown(EKey::RightShift),
			    .bCrouch = Window.IsKeyDown(EKey::C) || Window.IsKeyDown(EKey::LeftControl),
			};
		}

		if (bPlaying && Input.bRestart)
		{
			if (auto Restarted = Session->Restart(); !Restarted)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "{}", Restarted.error().Message);
				bFailed = true;
				break;
			}

			HERTA_LOG_INFO(*Log, GameLog, "Course restarted");
		}

		if (auto Updated = Session->Update(bPlaying ? DeltaSeconds : 0.f, bPlaying ? GameInput : FGameInput{}); !Updated)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "{}", Updated.error().Message);
			bFailed = true;
			break;
		}

		for (const std::string& Event : Session->TakeEvents())
		{
			HERTA_LOG_INFO(*Log, GameLog, "{}", Event);
		}

		const FExtent2D Extent = Window.IsMinimized() ? FExtent2D{} : GetFramebufferExtent(Window);
		if (Extent.IsEmpty())
		{
			Application->PumpEvents();
			continue;
		}

		if (Extent != Presentation->GetExtent())
		{
			if (auto Resized = Presentation->Resize(Extent); !Resized)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not resize presentation: {}", Resized.error().Message);
				bFailed = true;
				break;
			}
		}

		// The display under the window can change as it moves; querying walks every display output, so it refreshes once per second.
		if (Settings.bHdr && Now - SystemLuminanceQuery > std::chrono::seconds(1))
		{
			SystemLuminanceQuery = Now;
			SystemLuminance = Window.GetDisplayLuminance();
		}

		const float SystemPaperWhite = SystemLuminance ? SystemLuminance->SdrWhite : 0.f;
		const float SystemPeak = SystemLuminance ? SystemLuminance->PeakLuminance : 0.f;
		const float PaperWhite = std::clamp(Settings.PaperWhite > 0.f ? Settings.PaperWhite : SystemPaperWhite > 0.f ? SystemPaperWhite
		                                                                                                             : 200.f,
		    80.f, 500.f);
		const float PeakLuminance = std::clamp(Settings.PeakLuminance > 0.f ? Settings.PeakLuminance : SystemPeak > 0.f ? SystemPeak
		                                                                                                                : 1000.f,
		    PaperWhite, 10000.f);
		if (auto Hdr = Presentation->SetHdrOutput({.bEnabled = Settings.bHdr && !bSmokeTest, .PaperWhite = PaperWhite, .PeakLuminance = PeakLuminance}); !Hdr)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not change HDR output: {}", Hdr.error().Message);
			bFailed = true;
			break;
		}

		FMeshRenderView View = Session->GetRenderView(Extent);
		ApplyRenderQuality(View.Visuals, Settings.Quality);
		if (Presentation->IsHdrOutputActive())
		{
			View.Visuals.HdrDisplay = FHdrDisplaySettings{.PaperWhite = PaperWhite, .PeakLuminance = PeakLuminance};
		}

		if (const auto Uniforms = BuildVisualUniforms(View.View, View.Projection, Extent, View.Lights, View.Visuals); Uniforms && Environment)
		{
			Environment->Tick(Session->GetEnvironment(), *Uniforms);
		}

		if (auto Rendered = Renderer->Render(Extent, View); !Rendered)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not render: {}", Rendered.error().Message);
			bFailed = true;
			break;
		}

		if (RegisteredScene != Renderer->GetColorTarget())
		{
			auto Registered = Presentation->RegisterToolUITexture(Renderer->GetColorTarget());
			if (!Registered)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not present the scene: {}", Registered.error().Message);
				bFailed = true;
				break;
			}

			Presentation->UnregisterToolUITexture(SceneTextureId);
			SceneTextureId = *Registered;
			RegisteredScene = Renderer->GetColorTarget();
		}

		if (auto Begun = UI->BeginFrame(Extent); !Begun)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not lay out the HUD: {}", Begun.error().Message);
			bFailed = true;
			break;
		}

		const auto Width = static_cast<float>(Extent.Width);
		const auto Height = static_cast<float>(Extent.Height);
		UI->DrawImage(SceneTextureId, {0.f, 0.f}, {Width, Height});
		const bool bHdrSupported = Presentation->IsHdrOutputSupported();
		const std::string FullscreenLabel = std::format("Fullscreen: {}", Window.IsFullscreen() ? "On" : "Off");
		const std::string HdrLabel = bHdrSupported ? std::format("HDR: {}", Settings.bHdr ? "On" : "Off") : std::string("HDR: not available on this display");
		constexpr std::array QualityNames{"Low", "Medium", "High"};
		const std::string QualityLabel = std::format("Quality: {}", QualityNames[static_cast<std::size_t>(Settings.Quality)]);
		const std::array<std::string_view, 6> Items{"Resume", "Restart course", FullscreenLabel, HdrLabel, QualityLabel, "Quit"};
		const FGameUIInput MenuInput{.Pointer = Input.Pointer, .bClicked = Input.bClicked, .bUp = Input.bMenuUp, .bDown = Input.bMenuDown, .bActivate = Input.bMenuActivate};
		if (const auto Chosen = DrawGameHud(*UI, Session->GetObjective(), bPaused, Items, MenuFocus, MenuInput))
		{
			if (*Chosen == 0)
			{
				bPaused = false;
			}
			else if (*Chosen == 1)
			{
				bPaused = false;
				if (auto Restarted = Session->Restart(); !Restarted)
				{
					HERTA_LOG_ERROR(*Log, GameLog, "{}", Restarted.error().Message);
					bFailed = true;
				}
			}
			else if (*Chosen == 2)
			{
				Settings.bFullscreen = !Window.IsFullscreen();
				Window.SetFullscreen(Settings.bFullscreen);
				SaveSettings(SettingsPath, Settings, *Log);
			}
			else if (*Chosen == 3 && bHdrSupported)
			{
				Settings.bHdr = !Settings.bHdr;
				SaveSettings(SettingsPath, Settings, *Log);
			}
			else if (*Chosen == 4)
			{
				Settings.Quality = static_cast<ERenderQuality>((static_cast<int>(Settings.Quality) + 1) % 3);
				SaveSettings(SettingsPath, Settings, *Log);
			}
			else if (*Chosen == 5)
			{
				Window.RequestClose();
			}
		}

		auto Begin = Presentation->BeginFrame();
		if (!Begin)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not begin the frame: {}", Begin.error().Message);
			bFailed = true;
			break;
		}

		if (*Begin == EPresentationStatus::SurfaceOutOfDate)
		{
			if (auto Recreated = Presentation->Resize(Extent); !Recreated)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not recreate presentation: {}", Recreated.error().Message);
				bFailed = true;
			}

			continue;
		}

		if (*Begin != EPresentationStatus::Ready && *Begin != EPresentationStatus::Suboptimal)
		{
			continue;
		}

		auto Drawn = Presentation->Clear({});
		if (Drawn)
		{
			Drawn = Presentation->RenderScreenDraws(UI->GetDrawList());
		}

		if (!Drawn)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not draw the frame: {}", Drawn.error().Message);
			bFailed = true;
		}

		auto Presented = Presentation->Present();
		if (!Presented)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not present: {}", Presented.error().Message);
			bFailed = true;
			break;
		}

		if (*Presented == EPresentationStatus::SurfaceOutOfDate || *Presented == EPresentationStatus::Suboptimal)
		{
			if (auto Recreated = Presentation->Resize(Extent); !Recreated)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not recreate presentation: {}", Recreated.error().Message);
				bFailed = true;
			}
		}

		if (Presentation->HasValidationErrors())
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Vulkan validation reported an error");
			bFailed = true;
		}

		if (Frames == 0 && !bShowBeforePresentation)
		{
			Window.Show();
		}

		++Frames;
		if (bSmokeTest && Frames >= Options.SmokeFrames)
		{
			HERTA_LOG_INFO(*Log, GameLog, "Smoke run presented {} frames, average {:.3f} ms, objective state {}, player at ({:.2f}, {:.2f}, {:.2f})", Frames, FrameMillisecondsSum / Frames, static_cast<int>(Session->GetObjective().State), Session->GetPlayerFootPosition().X, Session->GetPlayerFootPosition().Y, Session->GetPlayerFootPosition().Z);
			Window.RequestClose();
		}
	}

	Window.SetCursorCaptured(false);
	if (auto Idle = Presentation->WaitIdle(); !Idle)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not wait for the GPU: {}", Idle.error().Message);
		return 1;
	}

	if (bSmokeTest && Frames < Options.SmokeFrames)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Smoke run ended after {} of {} frames", Frames, Options.SmokeFrames);
		return 1;
	}

	return bFailed ? 1 : 0;
}
}
}

int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		const std::span<char* const> All(Arguments, static_cast<std::size_t>(std::max(ArgumentCount, 0)));
		const std::span<char* const> Rest = All.empty() ? All : All.subspan(1);
		Herta::FGameOptions Options;
		Options.ExecutablePath = Herta::GetExecutablePath();
		const std::string_view Project = Herta::FindArgumentValue(Rest, "--project=");
		Options.ProjectPath = std::filesystem::path(std::u8string(Project.begin(), Project.end()));
		const std::string_view SmokeFrames = Herta::FindArgumentValue(Rest, "--smoke-test=");
		if (!SmokeFrames.empty())
		{
			const auto [End, Error] = std::from_chars(SmokeFrames.data(), SmokeFrames.data() + SmokeFrames.size(), Options.SmokeFrames);
			if (Error != std::errc{} || End != SmokeFrames.data() + SmokeFrames.size() || Options.SmokeFrames == 0)
			{
				Herta::ReportFailure("--smoke-test= needs a positive frame count");
				return 2;
			}
		}

		Options.bValidation = Herta::HasArgument(Rest, "--validation");
		return Herta::RunGame(Options);
	}
	catch (const std::exception& Exception)
	{
		Herta::ReportFailure(Exception.what());
		return 1;
	}
	catch (...)
	{
		Herta::ReportFailure("unknown error");
		return 1;
	}
}
