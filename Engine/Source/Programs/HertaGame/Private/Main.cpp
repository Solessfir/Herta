#include "Herta/Application/Application.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/Audio/AudioEngine.h"
#include "Herta/Audio/StagedSounds.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/GameAutomation/GameAutomation.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameContent.h"
#include "Herta/GameFramework/GameControls.h"
#include "Herta/GameFramework/GameHud.h"
#include "Herta/GameFramework/GameMenu.h"
#include "Herta/GameFramework/GameSave.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/GameText.h"
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
#include "Herta/Replication/GameClient.h"
#include "Herta/Replication/GameServer.h"
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
	std::string Culture{SourceCulture};
};

namespace Game = Text::Game;

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

		// Checked against the loaded catalogs once they are known.
		if (Key == "culture")
		{
			Settings.Culture = Value;
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

		std::println(File, "culture={}", Settings.Culture);
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

// When a file was last written, as local wall-clock time.
[[nodiscard]] std::optional<FCivilTime> GetLocalWriteTime(const std::filesystem::path& Path)
{
	std::error_code Error;
	const auto Written = std::filesystem::last_write_time(Path, Error);
	if (Error)
	{
		return std::nullopt;
	}

	// The time zone database is the standard library's; without one, as on a system with no tzdata, the time is left out.
	try
	{
		const auto System = std::chrono::floor<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(Written));
		return ToCivilTime(std::chrono::current_zone()->to_local(System).time_since_epoch().count());
	}
	catch (const std::exception&)
	{
		return std::nullopt;
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
	// Hosts a game others can join on this port, playing it as its first client; zero plays alone.
	std::uint16_t HostPort = 0;
	// Joins a server at "host:port" instead of playing alone.
	std::string ConnectAddress{};
	std::string PlayerName = "Player";
	// A culture tag that overrides the saved language for this run, such as qps-ploc.
	std::string Culture{};
	// Accepts automation, such as from the MCP server, under this name; empty for none. Shipping builds refuse it.
	std::string AutomationName{};
	bool bAutomationWrites = false;
};

int RunGame(const FGameOptions& Options)
{
	const std::filesystem::path ExecutableDirectory = std::filesystem::absolute(Options.ExecutablePath).parent_path();
	const auto Mounted = MountGameContent({.ExecutableDirectory = ExecutableDirectory, .ProjectPath = Options.ProjectPath});
	if (!Mounted)
	{
		ReportFailure(std::format("Cannot find the game's content: {}. Stage a project with HertaEditorCmd game.stage, or pass --project=<path>.", Mounted.error().Message));
		return 1;
	}

	const FVirtualFileSystem& Files = *Mounted->FileSystem;
	const auto AsText = [](const std::vector<std::byte>& Bytes)
	{
		return std::string_view(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
	};

	const auto ProjectText = Files.Read(Mounted->ProjectPath);
	const auto Project = ProjectText ? LoadStagedProject(Mounted->ProjectPath, AsText(*ProjectText)) : std::unexpected(FProjectError{ProjectText.error().Message});
	if (!Project)
	{
		ReportFailure(std::format("Cannot load the game project {}: {}", Mounted->ProjectPath.generic_string(), Project.error().Message));
		return 1;
	}

	const bool bSmokeTest = Options.SmokeFrames > 0;
	const std::filesystem::path UserDirectory = GetUserSettingsDirectory() / "Herta" / Project->Descriptor.Name;
	FLogOptions LogOptions;
	LogOptions.FilePath = UserDirectory / "Logs" / "Game.log";
	// Automation reads the log through the in-memory buffer the editor uses.
	LogOptions.EditorBufferCapacity = Options.AutomationName.empty() ? 0 : 4096;
	auto LogResult = FLogService::Create(std::move(LogOptions));
	if (!LogResult)
	{
		ReportFailure(LogResult.error().Message);
		return 1;
	}

	std::unique_ptr<FLogService> Log = std::move(*LogResult);
	InstallCrashReporter(std::format("{} {} {}", GetBuildRevision(), GetBuildConfigurationName(GetBuildConfiguration()), GetPlatformName(GetCurrentPlatform())), UserDirectory / "Crashes");
	auto TaskResult = FTaskSystem::Create({.Log = Log.get()});
	if (!TaskResult)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start tasks: {}", TaskResult.error().Message);
		return 1;
	}

	std::unique_ptr<FTaskSystem> Tasks = std::move(*TaskResult);
	for (const std::string& Mount : Files.DescribeMounts())
	{
		HERTA_LOG_INFO(*Log, GameLog, "Content: {}", Mount);
	}

	for (const std::string& Mod : Mounted->Mods)
	{
		HERTA_LOG_INFO(*Log, GameLog, "Mod: {}", Mod);
	}

	for (const std::string& Diagnostic : Mounted->ModDiagnostics)
	{
		HERTA_LOG_WARNING(*Log, GameLog, "{}", Diagnostic);
	}

	const auto LevelBytes = Files.Read(Project->StartingLevel);
	const auto Level = LevelBytes ? ParseLevel(AsText(*LevelBytes)) : std::unexpected(FLevelError{LevelBytes.error().Message});
	if (!Level)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not load {}: {}", Project->StartingLevel.generic_string(), Level.error().Message);
		return 1;
	}

	const std::filesystem::path SettingsPath = UserDirectory / "Settings.ini";
	FGameSettings Settings = bSmokeTest ? FGameSettings{} : LoadSettings(SettingsPath);
	FLocalization Text;
	if (const auto Catalogs = LoadTextCatalogs(Files, CatalogDirectory); !Catalogs)
	{
		HERTA_LOG_WARNING(*Log, GameLog, "Game text shows untranslated: {}", Catalogs.error().Message);
	}
	else if (auto Loaded = Text.SetCatalogs(*Catalogs); !Loaded)
	{
		HERTA_LOG_WARNING(*Log, GameLog, "Game text shows untranslated: {}", Loaded.error().Message);
	}

	const std::string Culture = Options.Culture.empty() ? Settings.Culture : Options.Culture;
	if (!Text.SetCulture(Culture))
	{
		HERTA_LOG_WARNING(*Log, GameLog, "No culture '{}'; the game shows {}", Culture, Text.GetCultureName());
	}

	Settings.Culture = Options.Culture.empty() ? std::string(Text.GetCultureName()) : Settings.Culture;
	HERTA_LOG_INFO(*Log, GameLog, "Text: {} with {}", Text.GetCultureName(), [&Text]
	{
		std::string Names;
		for (const std::string& Name : Text.GetAvailableCultures())
		{
			Names += Names.empty() ? Name : ", " + Name;
		}

		return Names;
	}());
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

	// Engine shaders sit beside the executable, so a package built from that folder carries them too.
	const std::filesystem::path ShaderDirectory = ExecutableDirectory / "Shaders";
	const auto LoadCookedShader = [&Files](const std::filesystem::path& Path) -> std::expected<FShaderAsset, FShaderError>
	{
		const auto Bytes = Files.Read(Path);
		if (!Bytes)
		{
			return std::unexpected(FShaderError{Bytes.error().Message});
		}

		return DeserializeCookedShader(*Bytes);
	};

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
	const FCookedContent Content({Project->ContentRoot, EngineContent}, Mounted->FileSystem);
	auto Staged = CreateStagedSession(Project->StartingLevel, *Level, Content, &Device, Tasks.get());
	if (!Staged)
	{
		HERTA_LOG_ERROR(*Log, GameLog, "Could not start the game: {}", Staged.error().Message);
		return 1;
	}

	for (const std::string& Warning : Staged->Warnings)
	{
		HERTA_LOG_WARNING(*Log, GameLog, "{}", Warning);
	}

	std::unique_ptr<FStagedGameAssets> Assets = std::move(Staged->Assets);
	std::unique_ptr<FGameSession> OwnedSession = std::move(Staged->Session);
	FGameSession* Session = OwnedSession.get();

	// A networked game plays through a client, even when this process hosts: the host's own player joins its server through a loopback
	// connection, so it sees the game exactly as everyone else does.
	std::optional<FStagedSession> HostStaged;
	std::unique_ptr<FGameServer> Host;
	std::unique_ptr<FGameClient> Client;
	const bool bNetwork = Options.HostPort != 0 || !Options.ConnectAddress.empty();
	if (bNetwork)
	{
		const std::uint64_t ContentHash = HashLevelBytes(*LevelBytes);
		const FGameClientSettings ClientSettings{.Name = Options.PlayerName, .ContentHash = ContentHash};
		if (Options.HostPort != 0)
		{
			auto Hosted = CreateStagedSession(Project->StartingLevel, *Level, Content, nullptr, Tasks.get());
			auto Server = Hosted ? FGameServer::Create(std::move(Hosted->Session), {.Port = Options.HostPort, .ContentHash = ContentHash}) : std::unexpected(FReplicationError{Hosted.error().Message});
			if (!Server)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "Could not host: {}", Server.error().Message);
				return 1;
			}

			HostStaged = std::move(*Hosted);
			Host = std::move(*Server);
			HERTA_LOG_INFO(*Log, GameLog, "Hosting on port {}", Host->GetPort());
		}

		auto Joined = Host ? FGameClient::ConnectLocal(std::move(OwnedSession), *Host, ClientSettings) : FGameClient::Connect(std::move(OwnedSession), Options.ConnectAddress, ClientSettings);
		if (!Joined)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not join the game: {}", Joined.error().Message);
			return 1;
		}

		Client = std::move(*Joined);
		Session = &Client->GetSession();
	}

	std::unique_ptr<FGameAutomation> Automation;
	if (!Options.AutomationName.empty())
	{
		auto Created = FGameAutomation::Create(Options.AutomationName, Options.bAutomationWrites, *Log);
		if (!Created)
		{
			HERTA_LOG_ERROR(*Log, GameLog, "Could not start automation: {}", Created.error());
			return 1;
		}

		Automation = std::move(*Created);
		HERTA_LOG_INFO(*Log, GameLog, "Automation is listening as '{}'; clients find it through {}", Options.AutomationName, Automation->GetSessionFile().string());
	}

	FMatrix4 LastView{};
	FTickInput TickInput;
	float NetworkSeconds = 0.f;
	EGameClientState ClientState = EGameClientState::Connecting;
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
		if (const std::filesystem::path TablePath = GetStagedMaterialShaderParametersPath(Mount, Relative); Content.Exists(TablePath))
		{
			const auto Table = Content.ReadFile(TablePath);
			auto Parsed = Table ? DeserializeMaterialShaderParameters(AsText(*Table)) : std::unexpected(FAssetError{Table.error().Message});
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
	    .FontBytes = Files.Read(EngineContent / "Fonts" / "Roboto-Medium.ttf").value_or(std::vector<std::byte>{}),
	    .FallbackFontBytes = {Files.Read(EngineContent / "Fonts" / "NotoEmoji.ttf").value_or(std::vector<std::byte>{})},
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
	FGameMenu Menu(Text);
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
			Menu.ShowNotice(Text.Format(Game::Notice_SaveFailed));
			return;
		}

		HERTA_LOG_INFO(*Log, GameLog, "Saved the game to {}", SavePath.generic_string());
		if (!bQuiet)
		{
			Menu.ShowNotice(Text.Format(Game::Notice_Saved));
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
			// The run also saves and loads through the menu's commands; a networked game belongs to its server and does neither.
			if (Frames == Options.SmokeFrames / 3 && !bNetwork)
			{
				SaveGame(false);
			}

			if (Frames == Options.SmokeFrames / 3 + 10 && !bNetwork)
			{
				if (const auto Loaded = LoadGameSave(SavePath); Loaded)
				{
					(void)Session->ApplySave(Loaded->Save);
				}
			}
		}

		// With regions, the run also crosses to the farthest one and back, and must end holding the assets it held before leaving.
		if (bSmokeTest && !bNetwork && !Level->Regions.empty() && Options.SmokeFrames >= 160)
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

		const bool bRestartPressed = Frames == Options.SmokeFrames * 3 / 4 && bSmokeTest && !bNetwork;

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
		    .Culture = std::string(Text.GetCultureName()),
		};

		// The save's state only shows in the menu, so the files are checked only while it is open or opening.
		// Automated games run several windows at once, most of them unfocused, so losing focus does not pause them, as in a smoke run.
		if (Automation)
		{
			MenuInput.bFocusLost = false;
		}

		if (Menu.IsOpen() || MenuInput.bPause || MenuInput.bFocusLost)
		{
			MenuSettings.bCanLoad = std::filesystem::exists(SavePath) || std::filesystem::exists(GetGameSaveBackupPath(SavePath));
			MenuSettings.SaveTime = GetLocalWriteTime(std::filesystem::exists(SavePath) ? SavePath : GetGameSaveBackupPath(SavePath));
		}

		const bool bMenuOwnedInput = Menu.OwnsInput();
		std::vector<EGameMenuCommand> MenuCommands = Menu.Update(MenuInput, MenuSettings);
		Menu.Tick(DeltaSeconds);
		bPaused = Menu.IsOpen();

		const bool bPlaying = !bPaused && (Window.IsFocused() || bSmokeTest || Automation);
		if (!bSmokeTest)
		{
			// An automated game plays unfocused too, and must not take the pointer from the window the user is in.
			Window.SetCursorCaptured(bPlaying && Window.IsFocused());
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

		// A play_input run drives the player instead of the devices while it lasts.
		if (const std::optional<FGameInput> Scripted = Automation ? Automation->TakeInput() : std::nullopt)
		{
			GameInput = *Scripted;
		}

		if (!bNetwork && (bRestartPressed || (bPlaying && !bMenuOwnedInput && (WasGameActionPressed(Settings.Bindings, EGameAction::Restart, Keyboard) || bPadRestart))))
		{
			if (auto Restarted = Session->Restart(); !Restarted)
			{
				HERTA_LOG_ERROR(*Log, GameLog, "{}", Restarted.error().Message);
				bFailed = true;
				break;
			}

			HERTA_LOG_INFO(*Log, GameLog, "Course restarted");
		}

		if (Client)
		{
			// The network game never pauses: a paused or unfocused player just stands still while the server's ticks go on.
			TickInput.Add(bPlaying ? GameInput : FGameInput{});
			constexpr float TickSeconds = 1.f / static_cast<float>(ReplicationTicksPerSecond);
			for (NetworkSeconds += DeltaSeconds; NetworkSeconds >= TickSeconds && !bFailed; NetworkSeconds -= TickSeconds)
			{
				const auto Hosted = Host ? Host->Tick() : std::expected<void, FReplicationError>{};
				const auto Ticked = Hosted ? Client->Tick(TickInput.Take()) : Hosted;
				if (!Ticked)
				{
					HERTA_LOG_ERROR(*Log, GameLog, "{}", Ticked.error().Message);
					bFailed = true;
				}
			}

			if (bFailed)
			{
				break;
			}

			if (Client->GetState() != ClientState)
			{
				ClientState = Client->GetState();
				constexpr std::array StateNames{"Connecting", "Playing", "Rejected", "Disconnected"};
				HERTA_LOG_INFO(*Log, GameLog, "Network game: {}{}{}", StateNames[static_cast<std::size_t>(ClientState)], Client->GetStateReason().empty() ? "" : ": ", Client->GetStateReason());
				if (ClientState == EGameClientState::Rejected)
				{
					bFailed = bSmokeTest;
					Menu.ShowNotice(Text.Format(Game::Notice_JoinRefused));
				}
				else if (ClientState == EGameClientState::Disconnected)
				{
					Menu.ShowNotice(Text.Format(Game::Notice_ConnectionLost));
				}
			}
		}
		else if (auto Updated = Session->Update(bPlaying ? DeltaSeconds : 0.f, bPlaying ? GameInput : FGameInput{}); !Updated)
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
		LastView = View.View;
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
		if (!bPaused)
		{
			DrawCombatHud(*UI, Session->GetCombatView());
		}

		(void)DrawGameHud(*UI, Text, bPaused, {}, NoMenuFocus, {}, "Esc", GetGameControlHints(Text, Settings.Bindings, "Esc"));
		std::ranges::move(Menu.Draw(*UI, MenuInput, MenuSettings), std::back_inserter(MenuCommands));
		for (const EGameMenuCommand Command : MenuCommands)
		{
			switch (Command)
			{
				case EGameMenuCommand::Resume:
					break;
				case EGameMenuCommand::Save:
					if (bNetwork)
					{
						Menu.ShowNotice(Text.Format(Game::Notice_ServerSaves));
						break;
					}

					SaveGame(false);
					break;
				case EGameMenuCommand::Load:
					if (bNetwork)
					{
						Menu.ShowNotice(Text.Format(Game::Notice_ServerLoads));
					}
					else if (const auto Loaded = LoadGameSave(SavePath); !Loaded)
					{
						HERTA_LOG_WARNING(*Log, GameLog, "Could not load the save: {}", Loaded.error().Message);
						Menu.ShowNotice(Text.Format(Game::Notice_LoadFailed));
					}
					else if (const auto Applied = Session->ApplySave(Loaded->Save); !Applied)
					{
						HERTA_LOG_ERROR(*Log, GameLog, "Could not apply the save: {}", Applied.error().Message);
						Menu.ShowNotice(Text.Format(Game::Notice_LoadFailed));
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

						Menu.ShowNotice(Text.Format(Loaded->bRecovered ? Game::Notice_LoadedBackup : Game::Notice_Loaded));
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
					if (MenuSettings.Culture != Text.GetCultureName() && Text.SetCulture(MenuSettings.Culture))
					{
						Settings.Culture = MenuSettings.Culture;
					}

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
		if (Objective == EGameObjectiveState::Finished && PreviousObjective != EGameObjectiveState::Finished && !bSmokeTest && !bNetwork)
		{
			SaveGame(true);
			Menu.ShowNotice(Text.Format(Game::Notice_CourseSaved));
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

		if (Automation)
		{
			Automation->Pump({
			    .Session = Session,
			    .Audio = Audio.get(),
			    .Client = Client.get(),
			    .Server = Host.get(),
			    .RequestCapture = [&Presentation]()
			{
				Presentation->RequestFrameCapture();
			},
			    .TakeCapture = [&Presentation]()
			{
				return Presentation->TakeFrameCapture();
			},
			    .View = LastView,
			    .bPaused = bPaused,
			    .bPresented = true,
			});
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

			if (Client)
			{
				const FGameClientStats Stats = Client->GetStats();
				HERTA_LOG_INFO(*Log, GameLog, "Smoke run played over the network: {} snapshots, {} KiB, {} corrections, {} outcomes, {} rejected messages", Stats.Snapshots, Stats.SnapshotBytes / 1024, Stats.Corrections, Stats.Outcomes, Stats.Rejected);
				if (Client->GetState() != EGameClientState::Playing || Stats.Snapshots == 0)
				{
					HERTA_LOG_ERROR(*Log, GameLog, "The network game never played");
					bFailed = true;
				}
			}

			// Agents need their StateTree and the cooked navigation to think and walk; in a network game they think on the server.
			const std::vector<FGameAgentState> Agents = Host ? Host->GetSession().GetAgentStates() : Client ? std::vector<FGameAgentState>{} : Session->GetAgentStates();
			if (!Agents.empty())
			{
				const auto Thinking = std::ranges::count_if(Agents, [](const FGameAgentState& Agent)
				{
					return !Agent.State.empty();
				});

				const auto Walking = std::ranges::count_if(Agents, &FGameAgentState::bOnNavigation);
				HERTA_LOG_INFO(*Log, GameLog, "Smoke run had {} agents, {} running their StateTree and {} on navigation", Agents.size(), Thinking, Walking);
				if (Thinking == 0 || Walking == 0)
				{
					HERTA_LOG_ERROR(*Log, GameLog, "No agent ran its StateTree on the navigation");
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
		Options.ConnectAddress = Herta::FindArgumentValue(Rest, "--connect=");
		if (Herta::HasArgument(Rest, "--automation") || !Herta::FindArgumentValue(Rest, "--automation=").empty())
		{
#ifdef HERTA_SHIPPING
			Herta::ReportFailure("Shipping builds do not accept automation");
			return 2;
#else
			const std::string_view Name = Herta::FindArgumentValue(Rest, "--automation-name=");
			// A joining client defaults to "client", a host to "host", and a game played alone to "game".
			Options.AutomationName = !Name.empty() ? std::string(Name) : !Options.ConnectAddress.empty()                                                        ? "client"
			                                                         : Herta::HasArgument(Rest, "--host") || !Herta::FindArgumentValue(Rest, "--host=").empty() ? "host"
			                                                                                                                                                    : "game";
			Options.bAutomationWrites = Herta::FindArgumentValue(Rest, "--automation=") == "write";
#endif
		}

		Options.Culture = Herta::FindArgumentValue(Rest, "--culture=");
		if (const std::string_view Name = Herta::FindArgumentValue(Rest, "--name="); !Name.empty())
		{
			Options.PlayerName = Name;
		}

		if (Herta::HasArgument(Rest, "--host"))
		{
			Options.HostPort = 27015;
		}
		else if (const std::string_view Port = Herta::FindArgumentValue(Rest, "--host="); !Port.empty())
		{
			const auto [End, Error] = std::from_chars(Port.data(), Port.data() + Port.size(), Options.HostPort);
			if (Error != std::errc{} || End != Port.data() + Port.size() || Options.HostPort == 0)
			{
				Herta::ReportFailure("--host= needs a port number");
				return 2;
			}
		}

		if (Options.HostPort != 0 && !Options.ConnectAddress.empty())
		{
			Herta::ReportFailure("--host and --connect= exclude each other");
			return 2;
		}
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
