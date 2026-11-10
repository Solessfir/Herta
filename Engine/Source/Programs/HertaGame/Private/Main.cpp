#include "Herta/Application/Application.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Audio/AudioEngine.h"
#include "Herta/Audio/StagedSounds.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameControls.h"
#include "Herta/GameFramework/GameHud.h"
#include "Herta/GameFramework/GameMenu.h"
#include "Herta/GameFramework/GameSave.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/VisualEnvironment.h"
#include "Herta/GameUI/GameUI.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Navigation/NavigationWorld.h"
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
#include <iterator>
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
	// A multiple of the default sensitivity, which is 0.0022 radians per pixel.
	float Sensitivity = 1.f;
	ERenderQuality Quality = ERenderQuality::High;
	// Linear gains for the Master, Effects, and Ambient buses.
	std::array<float, 3> Volumes{1.f, 1.f, 1.f};
	FGameBindings Bindings = GetDefaultGameBindings();
};

constexpr float BaseMouseSensitivity = 0.0022f;
constexpr std::array<std::string_view, 3> VolumeKeys{"volumeMaster", "volumeEffects", "volumeAmbient"};
constexpr std::array<std::string_view, 3> VolumeBuses{"Master", "Effects", "Ambient"};

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
		if (Key.starts_with("bind."))
		{
			(void)ApplyGameBindingSetting(Settings.Bindings, Key, Value);
			continue;
		}

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
		else if (Key == "sensitivity" && Number >= 0.2f && Number <= 5.f)
		{
			Settings.Sensitivity = Number;
		}
		else if (const auto Volume = std::ranges::find(VolumeKeys, Key); Volume != VolumeKeys.end() && Number >= 0.f && Number <= 1.f)
		{
			Settings.Volumes[static_cast<std::size_t>(Volume - VolumeKeys.begin())] = Number;
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
		std::println(File, "fullscreen={}\nhdr={}\npaperWhite={}\npeakLuminance={}\nsensitivity={}\nquality={}", Settings.bFullscreen ? 1 : 0, Settings.bHdr ? 1 : 0, Settings.PaperWhite, Settings.PeakLuminance, Settings.Sensitivity, static_cast<int>(Settings.Quality));
		for (std::size_t Bus = 0; Bus < VolumeKeys.size(); ++Bus)
		{
			std::println(File, "{}={}", VolumeKeys[Bus], Settings.Volumes[Bus]);
		}

		File << SerializeGameBindings(Settings.Bindings);
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
	// Pressed since the last frame, in order, for gameplay through the bindings and for the menu to bind.
	std::vector<EKey> PressedKeys{};
	std::vector<EMouseButton> PressedMouse{};
	bool bClicked = false;
	bool bEscape = false;
	bool bToggleFullscreen = false;
	bool bMenuUp = false;
	bool bMenuDown = false;
	bool bMenuLeft = false;
	bool bMenuRight = false;
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
	    std::pair{"Bloom.comp.hshader", &VisualShaders.BloomCompute},
	    std::pair{"GtaoDepth.comp.hshader", &VisualShaders.GtaoDepthCompute},
	    std::pair{"Gtao.comp.hshader", &VisualShaders.GtaoCompute},
	    std::pair{"GtaoDenoise.comp.hshader", &VisualShaders.GtaoDenoiseCompute},
	    std::pair{"SmaaEdgeList.comp.hshader", &VisualShaders.SmaaEdgeListCompute},
	    std::pair{"SmaaEdgeWeights.comp.hshader", &VisualShaders.SmaaEdgeWeightsCompute},
	    std::pair{"LightClusters.comp.hshader", &VisualShaders.LightClustersCompute},
	    std::pair{"DepthHierarchy.comp.hshader", &VisualShaders.DepthHierarchyCompute},
	    std::pair{"OcclusionCull.comp.hshader", &VisualShaders.OcclusionCullCompute},
	    std::pair{"TexturedMesh.velocity.frag.hshader", &VisualShaders.DepthVelocityFragment},
	    std::pair{"TexturedMesh.instanced.velocity.vert.hshader", &VisualShaders.DepthVelocityVertex},
	    std::pair{"TexturedMesh.surface.frag.hshader", &VisualShaders.DepthSurfaceFragment},
	    std::pair{"TexturedMesh.velocity.surface.frag.hshader", &VisualShaders.DepthVelocitySurfaceFragment},
	    std::pair{"MotionVectors.comp.hshader", &VisualShaders.MotionVectorsCompute},
	    std::pair{"DepthBounds.comp.hshader", &VisualShaders.DepthBoundsCompute},
	    std::pair{"ContactShadows.comp.hshader", &VisualShaders.ContactShadowsCompute},
	    std::pair{"SunShadows.comp.hshader", &VisualShaders.SunShadowsCompute},
	    std::pair{"IrradianceFieldUpdate.comp.hshader", &VisualShaders.IrradianceFieldUpdateCompute},
	    std::pair{"IrradianceFieldTrace.comp.hshader", &VisualShaders.IrradianceFieldTraceCompute},
	    std::pair{"IrradianceFieldBlend.comp.hshader", &VisualShaders.IrradianceFieldBlendCompute},
	    std::pair{"IrradianceFieldGather.comp.hshader", &VisualShaders.IrradianceFieldGatherCompute},
	    std::pair{"IrradianceFieldReflect.comp.hshader", &VisualShaders.IrradianceFieldReflectCompute},
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

	// Particles draw with the debug primitives' pass-through shaders.
	auto ParticleVertexShader = LoadCookedShader(ShaderDirectory / "DebugDraw.vert.hshader");
	auto ParticleFragmentShader = LoadCookedShader(ShaderDirectory / "DebugDraw.frag.hshader");
	if (!ParticleVertexShader || !ParticleFragmentShader)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not load the particle shaders: {}", !ParticleVertexShader ? ParticleVertexShader.error().Message : ParticleFragmentShader.error().Message);
		return 1;
	}

	auto RendererResult = FMeshRenderer::Create(Device, std::move(*VertexShader), std::move(*FragmentShader), std::move(*ParticleVertexShader), std::move(*ParticleFragmentShader), {}, {}, std::move(*InstancedVertexShader), std::move(VisualShaders));
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
	for (const std::string& Shader : Assets->GetMaterialShaders())
	{
		const std::filesystem::path& Mount = Shader.starts_with("Engine/") ? EngineContent : Project->ContentRoot;
		const std::string_view Relative = std::string_view(Shader).substr(Shader.find('/') + 1);
		FMaterialShaderStages Stages;
		for (std::size_t Stage = 0; Stage < Stages.size(); ++Stage)
		{
			auto Loaded = LoadCookedShader(GetStagedMaterialShaderPath(Mount, Relative, MaterialShaderStageNames[Stage]));
			if (!Loaded)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not load the material shader {}: {}", Shader, Loaded.error().Message);
				return 1;
			}

			Stages[Stage] = std::move(*Loaded);
		}

		// Hand-written shaders stage an empty table.
		std::vector<FMaterialShaderParameter> Parameters;
		if (std::ifstream Table(GetStagedMaterialShaderParametersPath(Mount, Relative), std::ios::binary); Table)
		{
			const std::string Text{std::istreambuf_iterator<char>(Table), std::istreambuf_iterator<char>()};
			auto Parsed = DeserializeMaterialShaderParameters(Text);
			if (!Parsed)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not load the parameters of {}: {}", Shader, Parsed.error().Message);
				return 1;
			}

			Parameters = std::move(*Parsed);
		}

		if (auto Published = Renderer->PublishMaterialShader(Shader, std::move(Stages), std::move(Parameters)); !Published)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not create the material shader {}: {}", Shader, Published.error().Message);
			return 1;
		}
	}

	// The level's character is optional: without one, or if it fails to load, the player is the bare capsule.
	std::shared_ptr<const FCookedModel> Character;
	if (Level->PlayerCharacter.IsValid())
	{
		auto Cooked = Content.Load(Level->PlayerCharacter);
		if (Cooked && std::holds_alternative<FCookedModel>(*Cooked))
		{
			Character = std::make_shared<const FCookedModel>(std::move(std::get<FCookedModel>(*Cooked)));
		}
		else
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Could not load the player character: {}", Cooked ? "the asset is not a model" : Cooked.error().Message);
		}
	}

	auto SessionResult = FGameSession::Create(Level->Entities, *Assets, &Device, {}, Tasks.get(), std::move(Character));
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

	Session->SetLevelEffects(Level->Effects);
	for (const FAssetId& System : Session->GetParticleSystems())
	{
		auto Cooked = Content.Load(System);
		auto* const Particles = Cooked ? std::get_if<FCookedParticleSystem>(&*Cooked) : nullptr;
		if (!Particles)
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Particle system {} will not play: {}", System.ToString(), Cooked ? "the asset is not a particle system" : Cooked.error().Message);
			continue;
		}

		if (auto Added = Session->AddParticleSystem(System, std::make_shared<const FCookedParticleSystem>(std::move(*Particles))); !Added)
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Particle system {} will not play: {}", System.ToString(), Added.error().Message);
		}
	}

	// Without a device the game still plays, with the same commands going to the null path.
	Session->SetLevelSounds(Level->Sounds);
	Session->SetRegions(Level->Regions, MakeStagedRegionLoader(Project->StartingLevel, Content, *Assets, Device));
	// Staging cooks the level's navigation beside it; a level without Navigation Bounds has none.
	std::filesystem::path NavigationPath = Project->StartingLevel;
	NavigationPath.replace_extension(NavigationDataExtension);
	if (std::ifstream NavigationFile(NavigationPath, std::ios::binary); NavigationFile)
	{
		const std::vector<char> Bytes{std::istreambuf_iterator<char>(NavigationFile), std::istreambuf_iterator<char>()};
		auto Navigation = ParseNavigationFile(std::as_bytes(std::span(Bytes)));
		const auto Loaded = Navigation ? Session->SetNavigation(std::make_shared<const FNavigationFile>(std::move(*Navigation))) : std::unexpected(FGameError{Navigation.error().Message});
		if (Loaded)
		{
			HERTA_LOG_INFO(*Log, GameLog, "Navigation loaded with {} polygons", Session->GetNavigation() ? Session->GetNavigation()->GetStats().Polygons : 0);
		}
		else
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Agents have no navigation: {}", Loaded.error().Message);
		}
	}
	std::unique_ptr<FAudioEngine> Audio;
	if (auto AudioResult = FAudioEngine::Create({.Device = bSmokeTest ? EAudioDevice::Null : EAudioDevice::Default}); AudioResult)
	{
		Audio = std::move(*AudioResult);
		const FAudioStats AudioDevice = Audio->GetStats();
		if (AudioDevice.bNullDevice && !AudioDevice.DeviceError.empty())
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Audio plays through the null path: {}", AudioDevice.DeviceError);
		}
		else
		{
			HERTA_LOG_INFO(*Log, GameLog, "Audio plays on {} at {} Hz", AudioDevice.Device, AudioDevice.SampleRate);
		}

		if (auto Added = AddStagedSounds(*Audio, Content, Session->GetSounds()); !Added)
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Some sounds will not play: {}", Added.error().Message);
		}
	}
	else
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Audio is unavailable: {}", AudioResult.error().Message);
	}

	bool bAudioMuted = false;

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
		if (bPressed && Key != EKey::Unknown)
		{
			Pending.PressedKeys.push_back(Key);
		}

		Pending.bEscape |= bPressed && Key == EKey::Escape;
		Pending.bToggleFullscreen |= bPressed && (Key == EKey::F11 || (Key == EKey::Enter && HasModifier(Modifiers, EModifierFlags::Alt)));
		Pending.bMenuUp |= Key == EKey::Up || Key == EKey::W;
		Pending.bMenuDown |= Key == EKey::Down || Key == EKey::S;
		Pending.bMenuLeft |= Key == EKey::Left || Key == EKey::A;
		Pending.bMenuRight |= Key == EKey::Right || Key == EKey::D;
		Pending.bMenuActivate |= bPressed && (Key == EKey::Enter || Key == EKey::KeypadEnter || Key == EKey::Space) && !HasModifier(Modifiers, EModifierFlags::Alt);
	};

	Callbacks.MouseButtonChanged = [&Pending](FWindow&, const EMouseButton Button, const EInputAction Action, EModifierFlags)
	{
		Pending.bClicked |= Button == EMouseButton::Left && Action == EInputAction::Pressed;
		if (Action == EInputAction::Pressed)
		{
			Pending.PressedMouse.push_back(Button);
		}
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
	FGameMenu Menu;
	// Unattended runs save beside other temporary files, never over a player's save.
	std::optional<std::pair<FVector3, std::size_t>> StreamBaseline;
	const std::filesystem::path SaveDirectory = bSmokeTest ? std::filesystem::temp_directory_path() / "HertaSmokeSaves" : UserDirectory / "Saves";
	const std::filesystem::path SavePath = SaveDirectory / (Level->Id.ToString() + std::string(GameSaveExtension));
	EGameObjectiveState PreviousObjective = Session->GetObjective().State;
	const auto ApplyVolumes = [&Audio, &Settings](const bool bMuted)
	{
		for (std::size_t Bus = 0; Bus < VolumeBuses.size() && Audio; ++Bus)
		{
			Audio->Submit(FAudioBusVolumeCommand{.Bus = std::string(VolumeBuses[Bus]), .Volume = Bus == 0 && bMuted ? 0.f : Settings.Volumes[Bus]});
		}
	};

	ApplyVolumes(false);
	const auto SaveGame = [&](const bool bQuiet)
	{
		if (auto Written = WriteGameSave(SavePath, Session->CaptureSave(Level->Id)); !Written)
		{
			HERTA_LOG_WARNING(*Log, GameLog, "Could not save the game: {}", Written.error().Message);
			Menu.ShowNotice("The game could not be saved");
			return;
		}

		HERTA_LOG_INFO(*Log, GameLog, "Saved the game to {}", SavePath.generic_string());
		if (!bQuiet)
		{
			Menu.ShowNotice("Game saved");
		}
	};
	std::optional<FGamepadState> PreviousGamepad;
	FGameGamepadControls GamepadControls;
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
			if (!Menu.IsOpen())
			{
				Menu.Open();
			}
		}

		Application->PumpEvents();
		Tasks->RunMainThreadTasks();
		const auto Now = std::chrono::steady_clock::now();
		const float DeltaSeconds = std::clamp(std::chrono::duration<float>(Now - LastFrame).count(), 0.f, 0.1f);
		FrameMillisecondsSum += std::chrono::duration<double, std::milli>(Now - LastFrame).count();
		LastFrame = Now;
		FPendingInput Input = std::exchange(Pending, FPendingInput{.LastCursor = Pending.LastCursor, .Pointer = Pending.Pointer});
		const std::optional<FGamepadState> Gamepad = bSmokeTest ? std::nullopt : Application->GetGamepadState();
		const auto GamepadPressed = [&Gamepad, &PreviousGamepad](const EGamepadButton Button)
		{
			return Gamepad && Gamepad->IsDown(Button) && !(PreviousGamepad && PreviousGamepad->IsDown(Button));
		};

		// Start pauses like Escape and B backs out of the menu; Back restarts the course like R.
		std::optional<EGamepadButton> PadPressed;
		for (std::size_t Button = 0; Button <= static_cast<std::size_t>(EGamepadButton::Last) && !PadPressed; ++Button)
		{
			PadPressed = GamepadPressed(static_cast<EGamepadButton>(Button)) ? std::optional(static_cast<EGamepadButton>(Button)) : std::nullopt;
		}

		const bool bPadRestart = Settings.Bindings.Get(EGameAction::Restart).Pad && GamepadPressed(*Settings.Bindings.Get(EGameAction::Restart).Pad);
		FGameMenuInput MenuInput{
		    .Pointer = Input.Pointer,
		    .bClicked = Input.bClicked,
		    .bUp = Input.bMenuUp || GamepadPressed(EGamepadButton::DpadUp),
		    .bDown = Input.bMenuDown || GamepadPressed(EGamepadButton::DpadDown),
		    .bLeft = Input.bMenuLeft || GamepadPressed(EGamepadButton::DpadLeft),
		    .bRight = Input.bMenuRight || GamepadPressed(EGamepadButton::DpadRight),
		    .bActivate = Input.bMenuActivate || GamepadPressed(EGamepadButton::South),
		    .bBack = GamepadPressed(EGamepadButton::East),
		    .bPause = Input.bEscape || GamepadPressed(EGamepadButton::Start),
		    .bFocusLost = Input.bFocusLost,
		    .Key = Input.PressedKeys.empty() ? std::nullopt : std::optional(Input.PressedKeys.back()),
		    .Mouse = Input.PressedMouse.empty() ? std::nullopt : std::optional(Input.PressedMouse.back()),
		    .Pad = PadPressed,
		};

		// The gamepad's B also crouches, so it backs out of the menu only while the menu is open.
		MenuInput.bBack = MenuInput.bBack && Menu.IsOpen();
		PreviousGamepad = Gamepad;

		// A scripted run walks the course, jumps, pauses, and restarts to exercise the same paths a player does.
		FGameInput GameInput;
		if (bSmokeTest)
		{
			GameInput = {.Move = {0.f, 1.f}, .YawDelta = Frames % 240 < 120 ? 0.004f : -0.004f, .PitchDelta = Frames >= 100 && Frames < 160 ? -0.015f : Frames >= 280 && Frames < 340 ? 0.015f
			                                                                                                                                                                          : 0.f,
			    .bJump = Frames % 90 == 45,
			    .bSprint = Frames % 300 > 150,
			    .bCrouch = Frames % 300 > 240,
			    .bThrow = Frames % 40 == 20,
			    .bToggleCamera = Frames == Options.SmokeFrames / 2 + 40};
			MenuInput = {.bPause = Frames == Options.SmokeFrames / 4 + 30 || Frames == Options.SmokeFrames / 2 || Frames == Options.SmokeFrames / 2 + 20, .bFocusLost = Frames == Options.SmokeFrames / 4};
			// The run also saves and loads through the menu's commands.
			if (Frames == Options.SmokeFrames / 3)
			{
				SaveGame(false);
			}

			if (Frames == Options.SmokeFrames / 3 + 10)
			{
				if (const auto Loaded = LoadGameSave(SavePath); Loaded)
				{
					(void)Session->ApplySave(Loaded->Save);
				}
			}
		}

		// With regions, the run also crosses to the farthest one and back, and must end holding the assets it held before leaving.
		if (bSmokeTest && !Level->Regions.empty() && Options.SmokeFrames >= 160)
		{
			if (Frames == Options.SmokeFrames * 13 / 16)
			{
				StreamBaseline = {Session->GetPlayerFootPosition(), Assets->GetLoadedAssetCount()};
				const auto Farthest = std::ranges::max_element(Level->Regions, {}, [](const FLevelRegion& Region)
				{
					return Region.Offset.Meters.X * Region.Offset.Meters.X + Region.Offset.Meters.Z * Region.Offset.Meters.Z;
				});

				const FVector3d& Far = Farthest->Offset.Meters;
				(void)Session->TeleportPlayer({static_cast<float>(Far.X), static_cast<float>(Far.Y) + 0.05f, static_cast<float>(Far.Z)}, 0.f);
			}

			if (Frames == Options.SmokeFrames * 14 / 16 && StreamBaseline)
			{
				(void)Session->TeleportPlayer(StreamBaseline->first, 0.f);
			}
		}

		const bool bRestartPressed = Frames == Options.SmokeFrames * 3 / 4 && bSmokeTest;

		if (Input.bToggleFullscreen)
		{
			Settings.bFullscreen = !Window.IsFullscreen();
			Window.SetFullscreen(Settings.bFullscreen);
			SaveSettings(SettingsPath, Settings, *Log);
		}

		// Losing focus opens the menu, so alt-tabbing never leaves the player running or the pointer trapped. While the menu is open it owns
		// every key, so a key pressed to rebind never also reaches gameplay.
		FGameMenuSettings MenuSettings{
		    .bFullscreen = Window.IsFullscreen(),
		    .bHdrAvailable = Presentation->IsHdrOutputSupported(),
		    .bHdr = Settings.bHdr,
		    .Quality = static_cast<std::uint32_t>(Settings.Quality),
		    .Volumes = Settings.Volumes,
		    .Sensitivity = Settings.Sensitivity,
		    .Bindings = Settings.Bindings,
		    .bCanLoad = std::filesystem::exists(SavePath) || std::filesystem::exists(GetGameSaveBackupPath(SavePath)),
		};

		const bool bMenuOwnedInput = Menu.OwnsInput();
		std::vector<EGameMenuCommand> MenuCommands = Menu.Update(MenuInput, MenuSettings);
		Menu.Tick(DeltaSeconds);
		bPaused = Menu.IsOpen();

		const bool bPlaying = !bPaused && (Window.IsFocused() || bSmokeTest);
		if (!bSmokeTest)
		{
			Window.SetCursorCaptured(bPlaying);
		}

		// The frame that closes the menu still belongs to it, so the Escape or click that resumed does not also act in the game.
		FGameKeyboardState Keyboard;
		std::vector<EKey> HeldKeys;
		std::vector<EMouseButton> HeldMouse;
		if (bPlaying && !bSmokeTest && !bMenuOwnedInput)
		{
			for (const FGameBinding& Binding : Settings.Bindings.Actions)
			{
				if (Binding.Key && Window.IsKeyDown(*Binding.Key))
				{
					HeldKeys.push_back(*Binding.Key);
				}

				if (Binding.Mouse && Window.IsMouseButtonDown(*Binding.Mouse))
				{
					HeldMouse.push_back(*Binding.Mouse);
				}
			}

			Keyboard = {.HeldKeys = HeldKeys, .PressedKeys = Input.PressedKeys, .HeldMouse = HeldMouse, .PressedMouse = Input.PressedMouse, .MouseDelta = Input.MouseDelta, .Sensitivity = BaseMouseSensitivity * Settings.Sensitivity};
			GameInput = BuildKeyboardGameInput(Settings.Bindings, Keyboard);
			if (Gamepad)
			{
				GamepadControls.Apply(GameInput, GetBoundGamepad(Settings.Bindings, *Gamepad), DeltaSeconds);
			}
		}
		else
		{
			GamepadControls.Reset();
		}

		if (bRestartPressed || (bPlaying && !bMenuOwnedInput && (WasGameActionPressed(Settings.Bindings, EGameAction::Restart, Keyboard) || bPadRestart)))
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

		const std::vector<FAudioCommand> AudioCommands = Session->TakeAudioCommands();
		if (Audio)
		{
			// The pause menu mutes rather than stops, so loops resume where they were.
			if (bPaused != bAudioMuted)
			{
				bAudioMuted = bPaused;
				ApplyVolumes(bPaused);
			}

			Audio->Submit(AudioCommands);
			Audio->Update(DeltaSeconds);
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
		View.Visuals.ExposureCompensation = Level->ExposureCompensation;
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
		std::size_t NoMenuFocus = 0;
		(void)DrawGameHud(*UI, Session->GetObjective(), bPaused, {}, NoMenuFocus, {}, "Esc", FormatGameControlsHint(Settings.Bindings, "Esc"));
		std::ranges::move(Menu.Draw(*UI, MenuInput, MenuSettings), std::back_inserter(MenuCommands));
		for (const EGameMenuCommand Command : MenuCommands)
		{
			switch (Command)
			{
				case EGameMenuCommand::Resume:
					break;
				case EGameMenuCommand::Save:
					SaveGame(false);
					break;
				case EGameMenuCommand::Load:
					if (const auto Loaded = LoadGameSave(SavePath); !Loaded)
					{
						HERTA_LOG_WARNING(*Log, GameLog, "Could not load the save: {}", Loaded.error().Message);
						Menu.ShowNotice("The save could not be loaded");
					}
					else if (const auto Applied = Session->ApplySave(Loaded->Save); !Applied)
					{
						HERTA_LOG_ERROR(*Log, GameLog, "Could not apply the save: {}", Applied.error().Message);
						Menu.ShowNotice("The save could not be loaded");
					}
					else
					{
						for (const std::string& Skipped : *Applied)
						{
							HERTA_LOG_WARNING(*Log, GameLog, "{}", Skipped);
						}

						if (Loaded->bRecovered)
						{
							HERTA_LOG_WARNING(*Log, GameLog, "The save was damaged ({}); loaded the one before it", Loaded->Problem);
						}

						Menu.ShowNotice(Loaded->bRecovered ? "The save was damaged; loaded the one before it" : "Game loaded");
						PreviousObjective = Session->GetObjective().State;
					}

					break;
				case EGameMenuCommand::Restart:
					if (auto Restarted = Session->Restart(); !Restarted)
					{
						HERTA_LOG_ERROR(*Log, GameLog, "{}", Restarted.error().Message);
						bFailed = true;
					}

					break;
				case EGameMenuCommand::Quit:
					Window.RequestClose();
					break;
				case EGameMenuCommand::SettingsChanged:
					if (MenuSettings.bFullscreen != Window.IsFullscreen())
					{
						Window.SetFullscreen(MenuSettings.bFullscreen);
					}

					Settings.bFullscreen = MenuSettings.bFullscreen;
					Settings.bHdr = MenuSettings.bHdr;
					Settings.Quality = static_cast<ERenderQuality>(std::min<std::uint32_t>(MenuSettings.Quality, 2));
					Settings.Volumes = MenuSettings.Volumes;
					Settings.Sensitivity = MenuSettings.Sensitivity;
					Settings.Bindings = MenuSettings.Bindings;
					ApplyVolumes(bPaused);
					if (!bSmokeTest)
					{
						SaveSettings(SettingsPath, Settings, *Log);
					}

					break;
			}
		}

		// Finishing the course saves, so a best time is never lost.
		const EGameObjectiveState Objective = Session->GetObjective().State;
		if (Objective == EGameObjectiveState::Finished && PreviousObjective != EGameObjectiveState::Finished && !bSmokeTest)
		{
			SaveGame(true);
			Menu.ShowNotice("Course finished; game saved");
		}

		PreviousObjective = Objective;

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
			if (StreamBaseline)
			{
				const FRegionStats Regions = Session->GetRegionStats();
				HERTA_LOG_INFO(*Log, GameLog, "Smoke run streamed {} region loads and {} unloads, {} failed; {} assets loaded, {} before leaving", Regions.Loads, Regions.Unloads, Regions.Failed, Assets->GetLoadedAssetCount(), StreamBaseline->second);
				if (Regions.Failed > 0 || Regions.Unloads == 0 || Assets->GetLoadedAssetCount() != StreamBaseline->second)
				{
					HERTA_LOG_ERROR(*Log, GameLog, "Streaming left the run with failed regions, no unloads, or a different set of loaded assets");
					bFailed = true;
				}
			}

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
