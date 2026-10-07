#include "Herta/Application/Application.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/ViewportCamera.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/EditorFramework/ScalingStatistics.h"
#include "Herta/NvrhiVulkan/NvrhiVulkan.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"
#include "Herta/Project/Project.h"
#include "Herta/Renderer/EnvironmentLighting.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/ToolUI.h"
#include "RendererSmoke.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
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
inline constexpr FLogCategory EditorLog{.Name = "Editor"};

void ReportFailure(const std::string_view Message) noexcept
{
	try
	{
		std::println(stderr, "Herta Editor failed: {}", Message);
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// Reporting must not let a formatting exception escape main.
	}
}

[[nodiscard]] std::string MakeVisualCaptureName(const std::uint32_t Slot, const std::string_view Label)
{
	std::string Name = std::format("{:02}-", Slot);
	for (const char Character : Label)
	{
		Name += std::isalnum(static_cast<unsigned char>(Character)) ? Character : '-';
	}

	return Name + ".png";
}

// Captures keep the viewport's overlays, as the user sees them, but drop alpha so PNG viewers show the image opaque.
[[nodiscard]] bool WriteVisualCapture(const std::filesystem::path& Path, std::vector<std::byte> Pixels, const FExtent2D Extent)
{
	const auto Width = static_cast<int>(Extent.Width);
	const auto Height = static_cast<int>(Extent.Height);
	if (Pixels.size() != std::size_t{Extent.Width} * Extent.Height * 4)
	{
		return false;
	}

	for (std::size_t Alpha = 3; Alpha < Pixels.size(); Alpha += 4)
	{
		Pixels[Alpha] = std::byte{0xff};
	}

	// Encoding to memory keeps non-ASCII paths on the standard library instead of stb's narrow fopen.
	std::vector<char> Encoded;
	const auto Append = [](void* const Context, void* const Data, const int Size)
	{
		auto& Output = *static_cast<std::vector<char>*>(Context);
		const auto* const Bytes = static_cast<const char*>(Data);
		Output.insert(Output.end(), Bytes, Bytes + Size);
	};

	if (stbi_write_png_to_func(Append, &Encoded, Width, Height, 4, Pixels.data(), Width * 4) == 0)
	{
		return false;
	}

	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	File.write(Encoded.data(), static_cast<std::streamsize>(Encoded.size()));
	return !Error && static_cast<bool>(File);
}

[[nodiscard]] std::filesystem::path FindRepositoryRoot(const std::filesystem::path& ExecutablePath)
{
	std::error_code PathError;
	std::filesystem::path Candidate = std::filesystem::current_path(PathError);
	if (!PathError && std::filesystem::exists(Candidate / "Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf"))
	{
		return Candidate;
	}

	Candidate = std::filesystem::absolute(ExecutablePath, PathError).parent_path();
	for (int Parent = 0; Parent < 8 && !Candidate.empty(); ++Parent)
	{
		if (std::filesystem::exists(Candidate / "Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf"))
		{
			return Candidate;
		}

		Candidate = Candidate.parent_path();
	}

	return {};
}

[[nodiscard]] FExtent2D GetFramebufferExtent(const FWindow& Window) noexcept
{
	return {
	    .Width = static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferWidth())),
	    .Height = static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferHeight())),
	};
}

[[nodiscard]] constexpr EToolUIViewportFrameStatus ToToolUIViewportFrameStatus(const EPresentationStatus Status) noexcept
{
	switch (Status)
	{
		case EPresentationStatus::Ready:
			return EToolUIViewportFrameStatus::Ready;
		case EPresentationStatus::Minimized:
			return EToolUIViewportFrameStatus::Skipped;
		case EPresentationStatus::SurfaceOutOfDate:
		case EPresentationStatus::Suboptimal:
			return EToolUIViewportFrameStatus::NeedsResize;
	}

	return EToolUIViewportFrameStatus::Skipped;
}

[[nodiscard]] bool HasArgument(const int ArgumentCount, char** const Arguments, const std::string_view Argument) noexcept
{
	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		if (Arguments[Index] != nullptr && Argument == Arguments[Index])
		{
			return true;
		}
	}

	return false;
}

[[nodiscard]] std::string_view FindArgumentValue(const int ArgumentCount, char** const Arguments, const std::string_view Prefix) noexcept
{
	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		if (Arguments[Index] == nullptr)
		{
			continue;
		}

		const std::string_view Argument = Arguments[Index];
		if (Argument.starts_with(Prefix))
		{
			return Argument.substr(Prefix.size());
		}
	}

	return {};
}

[[nodiscard]] constexpr EWindowSystem ParseWindowSystem(const std::string_view Name) noexcept
{
	if (Name == "win32")
	{
		return EWindowSystem::Win32;
	}

	if (Name == "x11")
	{
		return EWindowSystem::X11;
	}

	if (Name == "wayland")
	{
		return EWindowSystem::Wayland;
	}

	if (Name == "null")
	{
		return EWindowSystem::Null;
	}

	return EWindowSystem::Unknown;
}

[[nodiscard]] constexpr bool ShouldEnableValidation(const EBuildConfiguration Configuration, const bool bRequested) noexcept
{
	return Configuration == EBuildConfiguration::Debug || (Configuration == EBuildConfiguration::Development && bRequested);
}

static_assert(ShouldEnableValidation(EBuildConfiguration::Debug, false));
static_assert(!ShouldEnableValidation(EBuildConfiguration::Development, false));
static_assert(ShouldEnableValidation(EBuildConfiguration::Development, true));
static_assert(!ShouldEnableValidation(EBuildConfiguration::Shipping, true));

int RunEditor(const std::filesystem::path& ExecutablePath, const bool bSmokeTest, const bool bPlatformSmokeTest, const bool bRendererTest, const bool bValidationRequested, const std::string_view ExpectedWindowSystem, const std::string_view ScalingLevelPath, const bool bScalingSimulate, const std::string_view ProjectArgument, const bool bVisualTest, const std::filesystem::path& OpenPath)
{
	const bool bScalingTest = !ScalingLevelPath.empty();
	FLogOptions LogOptions;
	LogOptions.EditorBufferCapacity = 20'000;
	std::expected<std::unique_ptr<FLogService>, FLogError> LogResult = FLogService::Create(std::move(LogOptions));
	if (!LogResult)
	{
		std::println(stderr, "Could not initialize logging: {}", LogResult.error().Message);
		return 1;
	}

	std::unique_ptr<FLogService> Log = std::move(*LogResult);

	FTaskSystemOptions TaskOptions;
	TaskOptions.Log = Log.get();
	std::expected<std::unique_ptr<FTaskSystem>, FTaskError> TaskSystemResult = FTaskSystem::Create(TaskOptions);
	if (!TaskSystemResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize task system: {}", TaskSystemResult.error().Message);
		return 1;
	}

	std::unique_ptr<FTaskSystem> TaskSystem = std::move(*TaskSystemResult);

	const EWindowSystem RequestedWindowSystem = ParseWindowSystem(ExpectedWindowSystem);
	if (!ExpectedWindowSystem.empty() && RequestedWindowSystem == EWindowSystem::Unknown)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Unknown expected window system '{}'", ExpectedWindowSystem);
		return 2;
	}

	FApplicationDescriptor ApplicationDescriptor;
	ApplicationDescriptor.PreferredWindowSystem = RequestedWindowSystem;
	std::expected<std::unique_ptr<FApplication>, FApplicationError> ApplicationResult = FApplication::Create(ApplicationDescriptor, Log.get());
	if (!ApplicationResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize Application: {}", ApplicationResult.error().Message);
		return 1;
	}

	std::unique_ptr<FApplication> Application = std::move(*ApplicationResult);
	if (!ExpectedWindowSystem.empty())
	{
		if (Application->GetCapabilities().WindowSystem != RequestedWindowSystem)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Expected window system '{}' but GLFW selected backend {}", ExpectedWindowSystem, static_cast<int>(Application->GetCapabilities().WindowSystem));
			return 1;
		}
	}

	FWindowDescriptor WindowDescriptor;
	WindowDescriptor.Title = "Herta Editor";
	WindowDescriptor.bVisible = false;
	WindowDescriptor.bCustomTitleBar = true;
	// Automated runs keep the work-area-sized window so captures and measurements stay comparable.
	WindowDescriptor.bMaximized = !bSmokeTest && !bPlatformSmokeTest && !bVisualTest && !bScalingTest;
	std::expected<FWindow*, FApplicationError> WindowResult = Application->CreateWindow(std::move(WindowDescriptor));
	if (!WindowResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not create the editor window: {}", WindowResult.error().Message);
		return 1;
	}

	FWindow& Window = **WindowResult;
	const bool bShowBeforePresentation = Application->GetCapabilities().WindowSystem == EWindowSystem::Wayland;
	if (bShowBeforePresentation)
	{
		// Wayland must configure the XDG surface before Vulkan attaches a swapchain image.
		Window.Show();
		Application->PumpEvents();
	}

	if (bPlatformSmokeTest)
	{
		for (std::uint32_t Iteration = 0; Iteration < 3; ++Iteration)
		{
			Application->PumpEvents();
			TaskSystem->RunMainThreadTasks();
		}

		return 0;
	}

	std::expected<std::vector<std::string>, FApplicationError> InstanceExtensions = Application->GetRequiredVulkanInstanceExtensions();
	if (!InstanceExtensions)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not query Vulkan presentation extensions: {}", InstanceExtensions.error().Message);
		return 1;
	}

	FNvrhiVulkanPresentationDescriptor PresentationDescriptor;
	PresentationDescriptor.ApplicationName = "Herta Editor";
	PresentationDescriptor.WindowBackendHandle = Window.GetBackendHandle().Value;
	PresentationDescriptor.RequiredInstanceExtensions = std::move(*InstanceExtensions);
	PresentationDescriptor.InitialExtent = GetFramebufferExtent(Window);
	PresentationDescriptor.bEnableValidation = ShouldEnableValidation(GetBuildConfiguration(), bValidationRequested || bRendererTest);
	PresentationDescriptor.bRequireValidation = bRendererTest && GetBuildConfiguration() != EBuildConfiguration::Shipping;
	PresentationDescriptor.Log = Log.get();
	std::expected<std::unique_ptr<INvrhiVulkanPresentation>, FPresentationError> PresentationResult = CreateNvrhiVulkanPresentation(std::move(PresentationDescriptor));
	if (!PresentationResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize Vulkan presentation: {}", PresentationResult.error().Message);
		return 1;
	}

	std::unique_ptr<INvrhiVulkanPresentation> Presentation = std::move(*PresentationResult);
	const std::filesystem::path ShaderDirectory = std::filesystem::absolute(ExecutablePath).parent_path() / "Shaders";
	auto VertexShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.vert.hshader");
	auto InstancedVertexShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.instanced.vert.hshader");
	auto FragmentShader = LoadCookedShader(ShaderDirectory / "TexturedMesh.frag.hshader");
	auto DebugVertexShader = LoadCookedShader(ShaderDirectory / "DebugDraw.vert.hshader");
	auto DebugFragmentShader = LoadCookedShader(ShaderDirectory / "DebugDraw.frag.hshader");
	auto GridVertexShader = LoadCookedShader(ShaderDirectory / "WorldGrid.vert.hshader");
	auto GridFragmentShader = LoadCookedShader(ShaderDirectory / "WorldGrid.frag.hshader");
	if (!VertexShader || !FragmentShader)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not load cooked shaders: {}. Build HertaShaders before launching the editor.", !VertexShader ? VertexShader.error().Message : FragmentShader.error().Message);
		return 1;
	}

	if (!InstancedVertexShader)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not load instanced mesh shader: {}. Build HertaShaders before launching the editor.", InstancedVertexShader.error().Message);
		return 1;
	}

	if (!DebugVertexShader || !DebugFragmentShader)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not load debug shaders: {}. Build HertaShaders before launching the editor.", !DebugVertexShader ? DebugVertexShader.error().Message : DebugFragmentShader.error().Message);
		return 1;
	}

	if (!GridVertexShader || !GridFragmentShader)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not load grid shaders: {}. Build HertaShaders before launching the editor.", !GridVertexShader ? GridVertexShader.error().Message : GridFragmentShader.error().Message);
		return 1;
	}

	const FShaderAsset ThumbnailVertexShader = *VertexShader;
	const FShaderAsset ThumbnailFragmentShader = *FragmentShader;
	FVisualShaderSet VisualShaders;
	const std::array VisualShaderFiles{
	    // Slang reflects module-global bindings into the vertex stage, so the shared fullscreen vertex comes from a module whose only texture is at binding 0.
	    std::pair{"SkyView.vert.hshader", &VisualShaders.FullscreenVertex},
	    std::pair{"Sky.frag.hshader", &VisualShaders.SkyFragment},
	    std::pair{"VolumetricFog.frag.hshader", &VisualShaders.FogFragment},
	    std::pair{"FogComposite.frag.hshader", &VisualShaders.CompositeFragment},
	    std::pair{"ToneMap.frag.hshader", &VisualShaders.ToneMapFragment},
	    std::pair{"Shadow.vert.hshader", &VisualShaders.ShadowVertex},
	    std::pair{"Shadow.instanced.vert.hshader", &VisualShaders.ShadowInstancedVertex},
	    std::pair{"Shadow.frag.hshader", &VisualShaders.ShadowFragment},
	    std::pair{"SmaaEdges.frag.hshader", &VisualShaders.SmaaEdges},
	    std::pair{"SmaaWeights.frag.hshader", &VisualShaders.SmaaWeights},
	    std::pair{"SmaaNeighborhood.frag.hshader", &VisualShaders.SmaaNeighborhood},
	    std::pair{"SelectionOutline.frag.hshader", &VisualShaders.SelectionOutline},
	    std::pair{"SkyView.frag.hshader", &VisualShaders.SkyViewFragment},
	    std::pair{"ExposureMeter.frag.hshader", &VisualShaders.ExposureMeterFragment},
	    std::pair{"ExposureAdapt.frag.hshader", &VisualShaders.ExposureAdaptFragment},
	};

	for (const auto& [File, Destination] : VisualShaderFiles)
	{
		auto Shader = LoadCookedShader(ShaderDirectory / File);
		if (!Shader)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not load {}: {}", File, Shader.error().Message);
			return 1;
		}

		*Destination = std::move(*Shader);
	}

	if (bRendererTest)
	{
		auto VSyncResult = Presentation->SetVSyncEnabled(false);
		if (VSyncResult)
		{
			VSyncResult = Presentation->SetVSyncEnabled(true);
		}

		if (!VSyncResult || !Presentation->IsVSyncEnabled())
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Renderer VSync toggle regression failed: {}", VSyncResult ? "VSync state was not restored" : VSyncResult.error().Message);
			return 1;
		}

		auto Test = RunRendererSmoke(Presentation->GetGraphicsDevice(), *VertexShader, *FragmentShader, *DebugVertexShader, *DebugFragmentShader, *GridVertexShader, *GridFragmentShader, *InstancedVertexShader, VisualShaders);
		if (!Test || Presentation->HasValidationErrors())
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Renderer regression failed: {}", Test ? "Validation reported an error" : Test.error().Message);
			return 1;
		}

		HERTA_LOG_INFO(*Log, EditorLog, "Renderer readback, visual authoring, resize, and frame retirement checks passed");
	}

	const FVisualShaderSet ThumbnailVisualShaders = VisualShaders;
	auto MeshResult = FMeshRenderer::Create(Presentation->GetGraphicsDevice(), std::move(*VertexShader), std::move(*FragmentShader), std::move(*DebugVertexShader), std::move(*DebugFragmentShader), std::move(*GridVertexShader), std::move(*GridFragmentShader), std::move(*InstancedVertexShader), std::move(VisualShaders));
	if (!MeshResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize the mesh renderer: {}", MeshResult.error().Message);
		return 1;
	}

	std::unique_ptr<FMeshRenderer> MeshRenderer = std::move(*MeshResult);

	FEditorCommandRegistry Commands;
	std::expected<void, FEditorCommandError> CommandResult = RegisterCoreEditorCommands(Commands);
	if (!CommandResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not register editor commands: {}", CommandResult.error().Message);
		return 1;
	}

	const std::filesystem::path RepositoryRoot = FindRepositoryRoot(ExecutablePath);
	if (RepositoryRoot.empty())
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not locate Engine/Content from the editor executable or working directory");
		return 1;
	}

	FToolUIRendererBridge RendererBridge;
	RendererBridge.Initialize = [&Presentation]() -> std::expected<void, FToolUIError>
	{
		std::expected<void, FPresentationError> Result = Presentation->InitializeToolUIRenderer();
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return {};
	};

	RendererBridge.Render = [&Presentation](const void* const DrawData) -> std::expected<void, FToolUIError>
	{
		std::expected<void, FPresentationError> Result = Presentation->RenderToolUIDrawData(DrawData);
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return {};
	};

	RendererBridge.CreateViewport = [&Presentation](void* const WindowBackendHandle, const std::uint32_t Width, const std::uint32_t Height) -> std::expected<std::uint64_t, FToolUIError>
	{
		std::expected<FPresentationViewportHandle, FPresentationError> Result = Presentation->CreateViewport(WindowBackendHandle, {.Width = Width, .Height = Height});
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return Result->Value;
	};

	RendererBridge.DestroyViewport = [&Presentation](const std::uint64_t Handle) -> std::expected<void, FToolUIError>
	{
		std::expected<void, FPresentationError> Result = Presentation->DestroyViewport({Handle});
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return {};
	};

	RendererBridge.ResizeViewport = [&Presentation](const std::uint64_t Handle, const std::uint32_t Width, const std::uint32_t Height) -> std::expected<void, FToolUIError>
	{
		std::expected<void, FPresentationError> Result = Presentation->ResizeViewport({Handle}, {.Width = Width, .Height = Height});
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return {};
	};

	RendererBridge.BeginViewportFrame = [&Presentation](const std::uint64_t Handle) -> std::expected<EToolUIViewportFrameStatus, FToolUIError>
	{
		std::expected<EPresentationStatus, FPresentationError> Result = Presentation->BeginViewportFrame({Handle});
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return ToToolUIViewportFrameStatus(*Result);
	};

	RendererBridge.RenderViewport = [&Presentation](const std::uint64_t Handle, const void* const DrawData) -> std::expected<void, FToolUIError>
	{
		std::expected<void, FPresentationError> Result = Presentation->RenderViewportToolUIDrawData({Handle}, DrawData);
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return {};
	};

	RendererBridge.PresentViewport = [&Presentation](const std::uint64_t Handle) -> std::expected<EToolUIViewportFrameStatus, FToolUIError>
	{
		std::expected<EPresentationStatus, FPresentationError> Result = Presentation->PresentViewport({Handle});
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}

		return ToToolUIViewportFrameStatus(*Result);
	};

	RendererBridge.Shutdown = [&Presentation]
	{
		Presentation->ShutdownToolUIRenderer();
	};

	bool bRendering = false;
	std::function<bool()> RenderFrame;
	std::optional<bool> PendingVSync;
	FToolUIDescriptor ToolUIDescriptor;
	ToolUIDescriptor.Application = Application.get();
	ToolUIDescriptor.Window = &Window;
	ToolUIDescriptor.RegularFontPath = RepositoryRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf";
	ToolUIDescriptor.MediumFontPath = RepositoryRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf";
	ToolUIDescriptor.LogFontPath = RepositoryRoot / "Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf";
	ToolUIDescriptor.LayoutPath = RepositoryRoot / "Saved/Editor/ImGui.ini";
	ToolUIDescriptor.AppearancePath = RepositoryRoot / "Saved/Editor/Appearance.ini";
	if (bSmokeTest)
	{
		ToolUIDescriptor.LayoutPath = RepositoryRoot / "TestResults/Smoke/ImGui.ini";
		ToolUIDescriptor.AppearancePath = RepositoryRoot / "TestResults/Smoke/Appearance.ini";
	}

	if (bScalingTest || bVisualTest)
	{
		ToolUIDescriptor.LayoutPath.clear();
		ToolUIDescriptor.AppearancePath.clear();
	}

	if (bScalingTest)
	{
		if (auto Result = Presentation->SetVSyncEnabled(false); !Result)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not disable VSync for scaling capture: {}", Result.error().Message);
			return 1;
		}
	}

	ToolUIDescriptor.Renderer = std::move(RendererBridge);
	ToolUIDescriptor.bVSync = Presentation->IsVSyncEnabled();

	ToolUIDescriptor.VSyncChanged = [&PendingVSync](const bool bEnabled)
	{
		PendingVSync = bEnabled;
	};

	ToolUIDescriptor.RefreshRequested = [&RenderFrame]
	{
		if (RenderFrame)
		{
			RenderFrame();
		}
	};

	std::expected<std::unique_ptr<FToolUIContext>, FToolUIError> ToolUIResult = FToolUIContext::Create(std::move(ToolUIDescriptor));
	if (!ToolUIResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize ToolUI: {}", ToolUIResult.error().Message);
		return 1;
	}

	std::unique_ptr<FToolUIContext> ToolUI = std::move(*ToolUIResult);

	const std::filesystem::path EditorExecutable = GetExecutablePath();
	std::filesystem::path AssetWorker = EditorExecutable.parent_path() / "HertaAssetWorker";
	AssetWorker += EditorExecutable.extension();
	const std::string_view Platform = GetPlatformName(GetCurrentPlatform());
	const FEditorAssetPaths AssetPaths{.EngineContentRoot = RepositoryRoot / "Engine/Content", .ContentRoot = RepositoryRoot / "Games/Sandbox/Content", .DerivedDataRoot = RepositoryRoot / "DerivedDataCache" / Platform, .WorkerPath = AssetWorker, .TargetPlatform = std::string(Platform)};
	// A level opens inside the nearest project that owns it, falling back to Sandbox for loose files.
	std::filesystem::path OpenedLevel;
	std::filesystem::path OpenedProject;
	if (!OpenPath.empty() && !bScalingTest)
	{
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(OpenPath, Error);
		std::string Extension = OpenPath.extension().string();
		std::ranges::transform(Extension, Extension.begin(), [](const unsigned char Character)
		{
			return static_cast<char>(std::tolower(Character));
		});

		if (Extension == ".hlevel")
		{
			OpenedLevel = Absolute;
			OpenedProject = FindOwningProject(Absolute).value_or(std::filesystem::path{});
		}
		else if (Extension == ".hertaproject")
		{
			OpenedProject = Absolute;
		}
		else
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Ignoring {}: the editor opens .hlevel and .hertaproject files", OpenPath.string());
		}
	}

	const std::filesystem::path DefaultProject = RepositoryRoot / "Games/Sandbox/Sandbox.hertaproject";
	const std::filesystem::path ProjectPath = bScalingTest ? std::filesystem::path{} : !ProjectArgument.empty() ? std::filesystem::path(std::u8string(ProjectArgument.begin(), ProjectArgument.end()))
	                                                                                 : !OpenedProject.empty()   ? OpenedProject
	                                                                                                            : DefaultProject;
	const std::filesystem::path LevelPath = bScalingTest ? std::filesystem::path(std::u8string(ScalingLevelPath.begin(), ScalingLevelPath.end())) : OpenedLevel;
	if (bScalingTest)
	{
		std::error_code Error;
		if (!std::filesystem::is_regular_file(LevelPath, Error))
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Scaling capture requires an existing level: {}", LevelPath.string());
			return 1;
		}
	}

	const std::array MaterialStudioLights{
	    FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 45000.f, .bCastShadows = false}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle(FVector3::Up(), -0.6f) * FQuaternion::FromAxisAngle(FVector3::Left(), 0.7f))},
	    FRenderLight{.Settings = {.Type = ELightType::Sky, .Intensity = 1.f, .bCastShadows = false, .bEnvironmentVisible = false}},
	};

	const FVisualSettings MaterialStudioSettings{.Atmosphere = FSkyAtmosphereComponent{}, .ShadowQuality = EShadowQuality::Off, .bStudioPreview = false};
	const auto StudioUniforms = BuildVisualUniforms(FMatrix4{}, FMatrix4{}, {512, 512}, MaterialStudioLights, MaterialStudioSettings);
	if (!StudioUniforms)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize material studio lighting: {}", StudioUniforms.error().Message);
		return 1;
	}

	// Filter the reusable studio before interactive frames, not during property gestures.
	const auto StudioEnvironment = BuildEnvironmentLighting(nullptr, *StudioUniforms);
	if (!StudioEnvironment)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not filter material studio environment: {}", StudioEnvironment.error().Message);
		return 1;
	}

	const auto RenderThumbnail = [&](const FRenderMesh& Mesh, const FRenderMaterial* Material) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		const FVector3 Center = Mesh.GetBoundsMinimum() * 0.5f + Mesh.GetBoundsMaximum() * 0.5f;
		const FVector3 HalfExtent = Mesh.GetBoundsMaximum() * 0.5f - Mesh.GetBoundsMinimum() * 0.5f;
		const float Radius = std::hypot(HalfExtent.X, HalfExtent.Y, HalfExtent.Z);
		if (!std::isfinite(Radius) || Radius <= 0.f || !std::isfinite(1.f / Radius))
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Asset thumbnail requires finite, nonzero mesh bounds"});
		}

		// Normalize before camera fitting so unusually large or small authoring units do not hit camera distance limits.
		const float Scale = 1.f / Radius;
		const std::array Models{FMatrix4::Scale({Scale, Scale, Scale}) * FMatrix4::Translation(-Center)};
		const std::array<const FRenderMesh*, 1> Meshes{&Mesh};
		const std::array<const FRenderMaterial*, 1> MaterialSlots{Material};
		const std::array<std::span<const FRenderMaterial* const>, 1> Materials{MaterialSlots};
		FViewportCameraController Camera;
		const float Sensitivity = Camera.GetMouseSensitivity();
		Camera.Update({.Mode = EViewportCameraMode::Orbit, .MouseDeltaPixels = {-std::numbers::pi_v<float> / (4.f * Sensitivity), (Camera.GetPitch() + std::numbers::pi_v<float> / 6.f) / Sensitivity}, .Movement = {}}, 0.f, {192.f, 192.f});
		Camera.Focus({}, HalfExtent * (Scale * (Material ? 0.65f : 1.f)), 1.f, {0.9f, 0.9f});
		const FViewportCameraSnapshot View = Camera.GetSnapshot(1.f);

		// ponytail: one pipeline per cached thumbnail; reuse pipelines when RHI supports copying render targets.
		auto Renderer = FMeshRenderer::Create(Presentation->GetGraphicsDevice(), ThumbnailVertexShader, ThumbnailFragmentShader, {}, {}, {}, {}, {}, Material ? ThumbnailVisualShaders : FVisualShaderSet{});
		if (!Renderer)
		{
			return std::unexpected(Renderer.error());
		}

		const std::array StudioLights{
		    FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 3.f, .bCastShadows = false}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle(FVector3::Up(), -0.6f) * FQuaternion::FromAxisAngle(FVector3::Left(), 0.7f))},
		    FRenderLight{.Settings = {.Type = ELightType::Sky, .Intensity = 0.4f, .bCastShadows = false}},
		};

		if (Material)
		{
			if (const auto SharedShaders = (*Renderer)->ShareMaterialShaders(*MeshRenderer); !SharedShaders)
			{
				return std::unexpected(SharedShaders.error());
			}

			if (const auto Environment = (*Renderer)->SetEnvironmentLighting(*StudioEnvironment); !Environment)
			{
				return std::unexpected(Environment.error());
			}
		}

		const std::span<const FRenderLight> Lights = Material ? std::span<const FRenderLight>(MaterialStudioLights) : std::span<const FRenderLight>(StudioLights);
		const FExtent2D Extent = Material ? FExtent2D{512, 512} : FExtent2D{192, 192};
		if (auto Result = (*Renderer)->Render(Extent, {.View = View.View, .Projection = View.Projection, .Models = Models, .Meshes = Meshes, .Materials = Materials, .Lights = Lights, .Visuals = Material ? MaterialStudioSettings : FVisualSettings{}}); !Result)
		{
			return std::unexpected(Result.error());
		}

		auto TextureId = Presentation->RegisterToolUITexture((*Renderer)->GetColorTarget());
		if (!TextureId)
		{
			return std::unexpected(TextureId.error());
		}

		return FEditorAssetThumbnail(new std::uint64_t(*TextureId), [Device = Presentation.get()](const std::uint64_t* const Id)
		{
			Device->UnregisterToolUITexture(*Id);
			delete Id;
		});
	};
	const auto RenderAssetThumbnail = [&](const FRenderMesh& Mesh)
	{
		return RenderThumbnail(Mesh, nullptr);
	};

	const auto RenderMaterialThumbnail = [&](const FRenderMesh& Mesh, const FRenderMaterial& Material) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return RenderThumbnail(Mesh, &Material);
	};

	std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> EditorFrameworkResult = FEditorFramework::Create({.Log = Log.get(), .Commands = &Commands, .ToolUI = ToolUI.get(), .Tasks = TaskSystem.get(), .GraphicsDevice = &Presentation->GetGraphicsDevice(), .Assets = AssetPaths, .LevelPath = LevelPath, .EngineRoot = RepositoryRoot, .ProjectPath = ProjectPath, .RenderAssetThumbnail = RenderAssetThumbnail, .RenderMaterialThumbnail = RenderMaterialThumbnail, .MeshRenderer = MeshRenderer.get()});
	if (!EditorFrameworkResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize EditorFramework: {}", EditorFrameworkResult.error().Message);
		return 1;
	}

	std::unique_ptr<FEditorFramework> EditorFramework = std::move(*EditorFrameworkResult);
	if (bSmokeTest)
	{
		for (const std::string_view Command : {"stat unit", "stat fps"})
		{
			const auto Result = Commands.Execute(Command);
			if (!Result || Result->ExitCode != 0)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not enable {} for smoke testing", Command);
				return 1;
			}
		}
	}

	const FToolUIColor CanvasColor = ToolUITheme::Canvas;
	const FSrgbColor EditorClearColor = ConvertSrgb8ToSrgbColor(CanvasColor.Red, CanvasColor.Green, CanvasColor.Blue, CanvasColor.Alpha);

	bool bRenderFailed = false;
	FTextureHandle RegisteredLevelTexture;
	std::uint64_t LevelTextureId = 0;
	double RenderMilliseconds = 0.;
	double LastCpuMilliseconds = 0.;
	RenderFrame = [&]
	{
		if (bRendering || bRenderFailed)
		{
			return false;
		}

		bRendering = true;
		const auto CpuFrameStart = std::chrono::steady_clock::now();
		bool bViewportRendered = false;
		Presentation->SetToolUIGpuTimingEnabled(bScalingTest || EditorFramework->IsUnitStatsVisible());
		struct FRenderGuard
		{
			explicit FRenderGuard(bool& bInRendering)
			    : bRendering(bInRendering)
			{
			}

			~FRenderGuard()
			{
				bRendering = false;
			}

			FRenderGuard(const FRenderGuard&) = delete;
			FRenderGuard& operator=(const FRenderGuard&) = delete;
			FRenderGuard(FRenderGuard&&) = delete;
			FRenderGuard& operator=(FRenderGuard&&) = delete;

			bool& bRendering;
		} RenderGuard{bRendering};

		if (PendingVSync)
		{
			const bool bEnabled = *PendingVSync;
			PendingVSync.reset();
			if (auto Result = Presentation->SetVSyncEnabled(bEnabled); !Result)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not {} VSync: {}", bEnabled ? "enable" : "disable", Result.error().Message);
				bRenderFailed = true;
				return false;
			}
		}

		const FExtent2D FramebufferExtent = Window.IsMinimized() ? FExtent2D{} : GetFramebufferExtent(Window);
		if (FramebufferExtent != Presentation->GetExtent())
		{
			std::expected<void, FPresentationError> ResizeResult = Presentation->Resize(FramebufferExtent);
			if (!ResizeResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not resize Vulkan presentation: {}", ResizeResult.error().Message);
				bRenderFailed = true;
				return false;
			}
		}

		bool bFrameFailed = false;
		bool bPresentedMainFrame = false;

		ToolUI->BeginFrame();

		std::expected<void, FEditorFrameworkError> DrawResult = EditorFramework->Draw([&]
		{
			const FExtent2D ViewExtent = EditorFramework->GetViewportExtent();
			if (ViewExtent.IsEmpty() || Window.IsMinimized())
			{
				return;
			}

			const auto RenderStart = std::chrono::steady_clock::now();
			auto MeshFrame = MeshRenderer->Render(ViewExtent, EditorFramework->GetViewportRenderView(), EditorFramework->GetViewportDebugDrawLists());
			RenderMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - RenderStart).count();
			bViewportRendered = MeshFrame.has_value();
			if (!MeshFrame)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not render level: {}", MeshFrame.error().Message);
				bFrameFailed = true;
				return;
			}

			if (RegisteredLevelTexture != MeshRenderer->GetColorTarget())
			{
				auto TextureId = Presentation->RegisterToolUITexture(MeshRenderer->GetColorTarget(), true);
				if (!TextureId)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Could not display level: {}", TextureId.error().Message);
					bFrameFailed = true;
					return;
				}

				Presentation->UnregisterToolUITexture(LevelTextureId);
				LevelTextureId = *TextureId;
				RegisteredLevelTexture = MeshRenderer->GetColorTarget();
				EditorFramework->SetViewportImage(LevelTextureId);
			}
		});

		if (!DrawResult)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not draw the editor: {}", DrawResult.error().Message);
			bFrameFailed = true;
		}

		std::expected<EPresentationStatus, FPresentationError> BeginResult = Presentation->BeginFrame();
		const bool bMainFrameReady = BeginResult && (*BeginResult == EPresentationStatus::Ready || *BeginResult == EPresentationStatus::Suboptimal);
		if (!BeginResult)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not begin the editor frame: {}", BeginResult.error().Message);
			bFrameFailed = true;
		}
		else if (*BeginResult == EPresentationStatus::SurfaceOutOfDate && !FramebufferExtent.IsEmpty())
		{
			if (auto RecreateResult = Presentation->Resize(FramebufferExtent); !RecreateResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not recreate Vulkan presentation: {}", RecreateResult.error().Message);
				bFrameFailed = true;
			}
		}

		if (bMainFrameReady)
		{
			if (auto ClearResult = Presentation->Clear(EditorClearColor); !ClearResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not clear the editor frame: {}", ClearResult.error().Message);
				bFrameFailed = true;
			}
		}

		std::expected<void, FToolUIError> ToolUIRenderResult = ToolUI->EndFrame(bMainFrameReady);
		if (!ToolUIRenderResult)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not render ToolUI: {}", ToolUIRenderResult.error().Message);
			bFrameFailed = true;
		}

		if (bMainFrameReady)
		{
			std::expected<EPresentationStatus, FPresentationError> PresentResult = Presentation->Present();
			if (!PresentResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not present the editor frame: {}", PresentResult.error().Message);
				bFrameFailed = true;
			}
			else if ((*PresentResult == EPresentationStatus::SurfaceOutOfDate || *PresentResult == EPresentationStatus::Suboptimal) && !FramebufferExtent.IsEmpty())
			{
				std::expected<void, FPresentationError> RecreateResult = Presentation->Resize(FramebufferExtent);
				if (!RecreateResult)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Could not recreate Vulkan presentation: {}", RecreateResult.error().Message);
					bFrameFailed = true;
				}
			}

			if (PresentResult && (*PresentResult == EPresentationStatus::Ready || *PresentResult == EPresentationStatus::Suboptimal))
			{
				bPresentedMainFrame = true;
			}
		}

		if (!bFrameFailed)
		{
			std::expected<void, FToolUIError> PlatformRenderResult = ToolUI->RenderPlatformWindows();
			if (!PlatformRenderResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not render ToolUI platform windows: {}", PlatformRenderResult.error().Message);
				bFrameFailed = true;
			}
		}

		bRenderFailed = bFrameFailed;
		if (Presentation->HasValidationErrors())
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Renderer validation reported an error");
			bRenderFailed = true;
			return false;
		}

		if (bPresentedMainFrame && !bFrameFailed)
		{
			const double CpuMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - CpuFrameStart).count();
			EditorFramework->SetFrameTimings(CpuMilliseconds, Presentation->GetToolUIGpuMilliseconds());
			LastCpuMilliseconds = CpuMilliseconds;
		}

		return bPresentedMainFrame && !bFrameFailed && (!bScalingTest || bViewportRendered);
	};

	HERTA_LOG_INFO(*Log, EditorLog, "Herta Editor started in {} configuration", GetBuildConfigurationName(GetBuildConfiguration()));
	const bool bInitialFramePresented = RenderFrame();
	if (!bRenderFailed && !bShowBeforePresentation)
	{
		Window.Show();
	}

	std::uint32_t SmokeFrameCount = bInitialFramePresented ? 1 : 0;
	std::uint32_t SmokeAttemptCount = 1;
	std::uint32_t StressStep = 0;
	const FEditorAppearance SmokeAppearance = ToolUI->GetAppearance();
	std::array<std::vector<double>, 5> ScalingSamples;
	std::vector<FMatrix4> AuthoredModels;
	std::size_t ScalingPhase = 0;
	std::size_t ScalingFrame = 0;
	std::size_t VisualFrames = 0;
	std::uint32_t VisualSlot = 0;
	std::string VisualSlotName;
	std::size_t VisualSettleFrames = 0;
	std::size_t VisualCaptures = 0;
	bool bVisualCompleted = false;
	const std::filesystem::path VisualCaptureDirectory = RepositoryRoot / "Saved/VisualTest";
	constexpr std::size_t VisualBookmarkSettleFrames = 60;
	const auto CaptureVisualView = [&](const std::string& FileName)
	{
		const auto Pixels = Presentation->GetGraphicsDevice().ReadbackTexture(MeshRenderer->GetColorTarget());
		if (!Pixels || !WriteVisualCapture(VisualCaptureDirectory / FileName, *Pixels, EditorFramework->GetViewportExtent()))
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not save visual capture {}{}", FileName, Pixels ? "" : std::format(": {}", Pixels.error().Message));
			bRenderFailed = true;
			return;
		}

		++VisualCaptures;
	};

	const auto ShowNextVisualBookmark = [&]
	{
		while (++VisualSlot <= 9)
		{
			if (auto Name = EditorFramework->ShowCameraBookmark(VisualSlot))
			{
				VisualSlotName = std::move(*Name);
				VisualSettleFrames = 0;
				return true;
			}
		}

		return false;
	};

	const auto CompleteVisualCapture = [&]
	{
		HERTA_LOG_INFO(*Log, EditorLog, "Visual capture saved {} views to {}", VisualCaptures, VisualCaptureDirectory.generic_string());
		bVisualCompleted = true;
		Window.RequestClose();
	};

	bool bScalingStarted = false;
	bool bScalingCompleted = false;
	const auto ScalingDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
	constexpr std::size_t ScalingWarmupFrames = 60;
	constexpr std::size_t ScalingSampleFrames = 240;
	constexpr std::array<std::string_view, 4> ScalingPhaseNames{"rendering", "selected", "simulation", "restored"};
	constexpr std::array<std::string_view, 5> ScalingMetricNames{"cpu_frame", "inspectors", "extraction", "simulation_and_sync", "render_submission"};
	while (!bRenderFailed)
	{
		if (Window.ShouldClose())
		{
			if (EditorFramework->RequestClose())
			{
				break;
			}

			Window.SetShouldClose(false);
		}

		if (bRendererTest && StressStep < 12)
		{
			FEditorAppearance Appearance = SmokeAppearance;
			Appearance.PanelOpacity = StressStep % 3 == 0 ? 0.f : 0.35f;
			Appearance.BlurRadius = StressStep % 3 == 0 ? 0.f : StressStep % 3 == 1 ? 10.f
			                                                                        : 40.f;
			ToolUI->SetAppearance(Appearance);
			if (StressStep == 4)
			{
				Window.Minimize();
			}
			else if (StressStep == 5)
			{
				Window.Restore();
			}
			else
			{
				const auto Resize = Window.SetSize(StressStep % 2 == 0 ? 800 : 1200, StressStep % 2 == 0 ? 600 : 720);
				if (!Resize)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Native resize stress failed: {}", Resize.error().Message);
					bRenderFailed = true;
					break;
				}
			}

			++StressStep;
			if (StressStep == 12)
			{
				ToolUI->SetAppearance(SmokeAppearance);
			}
		}

		Application->PumpEvents();
		TaskSystem->RunMainThreadTasks();
		const bool bPresented = RenderFrame();
		if (EditorFramework->HasConfirmedClose())
		{
			break;
		}

		if (bPresented)
		{
			++SmokeFrameCount;
		}

		if (bVisualTest)
		{
			if (std::chrono::steady_clock::now() >= ScalingDeadline)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Visual capture exceeded its five minute asset loading and presentation budget");
				bRenderFailed = true;
				break;
			}

			if (bPresented && EditorFramework->GetFrameMetrics().bAssetsReady && VisualFrames < 600 && ++VisualFrames == 600)
			{
				const FExtent2D Extent = EditorFramework->GetViewportExtent();
				HERTA_LOG_INFO(*Log, EditorLog, "Visual capture completed: frames={} viewport={}x{} target_bytes={}", VisualFrames, Extent.Width, Extent.Height, MeshRenderer->GetRenderTargetBytes());
				for (const auto& Timing : MeshRenderer->GetGpuTimings())
				{
					HERTA_LOG_INFO(*Log, EditorLog, "Visual pass={} gpu_ms={:.3f}", Timing.Name, Timing.Milliseconds);
				}

				CaptureVisualView("00-Start.png");
				if (!ShowNextVisualBookmark())
				{
					CompleteVisualCapture();
				}
			}
			else if (bPresented && VisualFrames == 600 && !bVisualCompleted && ++VisualSettleFrames == VisualBookmarkSettleFrames)
			{
				// Each bookmark settles for a second of frames so shadows and GPU timings reflect its view.
				double GpuMilliseconds = 0.;
				for (const auto& Timing : MeshRenderer->GetGpuTimings())
				{
					GpuMilliseconds += Timing.Milliseconds;
				}

				HERTA_LOG_INFO(*Log, EditorLog, "Visual bookmark slot={} name=\"{}\" gpu_ms={:.3f} draws={}", VisualSlot, VisualSlotName, GpuMilliseconds, MeshRenderer->GetLastDrawCount());
				CaptureVisualView(MakeVisualCaptureName(VisualSlot, VisualSlotName));
				if (!ShowNextVisualBookmark())
				{
					CompleteVisualCapture();
				}
			}
		}

		if (bScalingTest)
		{
			if (std::chrono::steady_clock::now() >= ScalingDeadline)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Scaling capture exceeded its five minute budget, including asset loading");
				bRenderFailed = true;
				break;
			}

			const FEditorFrameMetrics Metrics = EditorFramework->GetFrameMetrics();
			if (ScalingPhase == 2 && bScalingStarted && !Metrics.bSimulationRunning)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Scaling simulation stopped before capture completed");
				bRenderFailed = true;
				break;
			}

			if (!bPresented || !Metrics.bAssetsReady)
			{
				continue;
			}

			if (!bScalingStarted)
			{
				const auto Prepared = EditorFramework->SetScalingTestPhase(false, false);
				if (!Prepared)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Scaling phase failed: {}", Prepared.error().Message);
					bRenderFailed = true;
					break;
				}

				bScalingStarted = true;
				continue;
			}

			if (ScalingFrame == 0 && ScalingPhase == 0)
			{
				const auto Models = EditorFramework->GetViewportRenderView().Models;
				AuthoredModels.assign(Models.begin(), Models.end());
			}

			if (ScalingPhase == 3)
			{
				const auto Models = EditorFramework->GetViewportRenderView().Models;
				if (Models.size() != AuthoredModels.size() || !std::ranges::equal(Models, AuthoredModels, [](const FMatrix4& Left, const FMatrix4& Right)
				{
					return Left.Data() == Right.Data();
				}))
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Scaling Stop did not restore every authored model");
					bRenderFailed = true;
					break;
				}
			}

			if (++ScalingFrame > ScalingWarmupFrames)
			{
				const std::array Values{LastCpuMilliseconds, Metrics.InspectorMilliseconds, Metrics.ExtractionMilliseconds, Metrics.SimulationMilliseconds, RenderMilliseconds};
				for (std::size_t Index = 0; Index < Values.size(); ++Index)
				{
					ScalingSamples[Index].push_back(Values[Index]);
				}
			}

			if (ScalingFrame < ScalingWarmupFrames + ScalingSampleFrames)
			{
				continue;
			}

			const FExtent2D Extent = EditorFramework->GetViewportExtent();
			HERTA_LOG_INFO(*Log, EditorLog, "Scaling phase={} objects={} selected={} frames={} draws={} viewport={}x{} configuration={}", ScalingPhaseNames[ScalingPhase], Metrics.ObjectCount, Metrics.SelectedCount, ScalingSampleFrames, MeshRenderer->GetLastDrawCount(), Extent.Width, Extent.Height, GetBuildConfigurationName(GetBuildConfiguration()));
			for (std::size_t Index = 0; Index < ScalingSamples.size(); ++Index)
			{
				const FScalingStatistics Summary = SummarizeScalingSamples(ScalingSamples[Index]);
				HERTA_LOG_INFO(*Log, EditorLog, "Scaling metric={} median_ms={:.4f} p95_ms={:.4f} p99_ms={:.4f}", ScalingMetricNames[Index], Summary.Median, Summary.P95, Summary.P99);
				ScalingSamples[Index].clear();
			}

			if (const auto Gpu = Presentation->GetToolUIGpuMilliseconds())
			{
				HERTA_LOG_INFO(*Log, EditorLog, "Scaling GPU UI last_ms={:.4f} (not level GPU timing)", *Gpu);
			}

			++ScalingPhase;
			if (ScalingPhase == 2 && !bScalingSimulate)
			{
				bScalingCompleted = true;
				Window.RequestClose();
				continue;
			}

			if (ScalingPhase == ScalingPhaseNames.size())
			{
				bScalingCompleted = true;
				Window.RequestClose();
				continue;
			}

			ScalingFrame = 0;
			if (auto Prepared = EditorFramework->SetScalingTestPhase(ScalingPhase != 0, ScalingPhase == 2); !Prepared)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Scaling phase failed: {}", Prepared.error().Message);
				bRenderFailed = true;
				break;
			}
		}

		if (bSmokeTest && SmokeFrameCount >= (bRendererTest ? 24u : 3u))
		{
			Window.RequestClose();
		}
		else if (bSmokeTest && ++SmokeAttemptCount >= 60)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Smoke test did not present three frames within the bounded attempt budget");
			bRenderFailed = true;
		}
	}

	if (bScalingTest && !bScalingCompleted)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Scaling capture ended before every required phase completed");
		bRenderFailed = true;
	}

	if (bVisualTest && !bVisualCompleted)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Visual capture ended before its ready-frame budget and bookmark views completed");
		bRenderFailed = true;
	}

	std::expected<void, FPresentationError> IdleResult = Presentation->WaitIdle();
	if (!IdleResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not wait for the editor renderer: {}", IdleResult.error().Message);
		return 1;
	}

	if (bSmokeTest)
	{
		if (const auto GpuMilliseconds = Presentation->GetToolUIGpuMilliseconds())
		{
			HERTA_LOG_INFO(*Log, EditorLog, "Viewport stats received GPU UI timing: {:.3f} ms", *GpuMilliseconds);
		}
	}

	return bRenderFailed ? 1 : 0;
}
}
}

int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		const std::filesystem::path ExecutablePath = ArgumentCount > 0 && Arguments[0] != nullptr ? Arguments[0] : "HertaEditor";
		const bool bSmokeTest = Herta::HasArgument(ArgumentCount, Arguments, "--smoke-test");
		const bool bPlatformSmokeTest = Herta::HasArgument(ArgumentCount, Arguments, "--platform-smoke-test");
		const bool bRendererTest = Herta::HasArgument(ArgumentCount, Arguments, "--renderer-test");
		const bool bVisualTest = Herta::HasArgument(ArgumentCount, Arguments, "--visual-test");
		const bool bValidationRequested = Herta::HasArgument(ArgumentCount, Arguments, "--validation");
		const std::string_view ExpectedWindowSystem = Herta::FindArgumentValue(ArgumentCount, Arguments, "--expect-window-system=");
		const std::string_view ScalingLevelPath = Herta::FindArgumentValue(ArgumentCount, Arguments, "--scaling-test=");
		const bool bScalingSimulate = Herta::HasArgument(ArgumentCount, Arguments, "--scaling-simulate");
		const std::string_view ProjectPath = Herta::FindArgumentValue(ArgumentCount, Arguments, "--project=");
		// Explorer passes a file dropped on the executable as a plain argument.
		std::filesystem::path OpenPath;
		for (const std::filesystem::path& Argument : Herta::GetProcessArgumentPaths(ArgumentCount, Arguments))
		{
			if (!Argument.u8string().starts_with(u8"-"))
			{
				OpenPath = Argument;
				break;
			}
		}

		return Herta::RunEditor(ExecutablePath, bSmokeTest || bRendererTest, bPlatformSmokeTest, bRendererTest, bValidationRequested, ExpectedWindowSystem, ScalingLevelPath, bScalingSimulate, ProjectPath, bVisualTest, OpenPath);
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
