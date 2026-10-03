#include "Herta/Application/Application.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/NvrhiVulkan/NvrhiVulkan.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/ToolUI.h"
#include "RendererSmoke.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

int RunEditor(const std::filesystem::path& ExecutablePath, const bool bSmokeTest, const bool bPlatformSmokeTest, const bool bRendererTest, const bool bValidationRequested, const std::string_view ExpectedWindowSystem)
{
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

		auto Test = RunRendererSmoke(Presentation->GetGraphicsDevice(), *VertexShader, *FragmentShader, *DebugVertexShader, *DebugFragmentShader, *GridVertexShader, *GridFragmentShader);
		if (!Test || Presentation->HasValidationErrors())
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Renderer regression failed: {}", Test ? "Validation reported an error" : Test.error().Message);
			return 1;
		}

		HERTA_LOG_INFO(*Log, EditorLog, "Renderer readback, reversed-Z, resize, and frame retirement checks passed");
	}

	auto MeshResult = FMeshRenderer::Create(Presentation->GetGraphicsDevice(), std::move(*VertexShader), std::move(*FragmentShader), std::move(*DebugVertexShader), std::move(*DebugFragmentShader), std::move(*GridVertexShader), std::move(*GridFragmentShader));
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
	std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> EditorFrameworkResult = FEditorFramework::Create({.Log = Log.get(), .Commands = &Commands, .ToolUI = ToolUI.get(), .Tasks = TaskSystem.get(), .GraphicsDevice = &Presentation->GetGraphicsDevice(), .Assets = AssetPaths, .ScenePath = RepositoryRoot / "Games/Sandbox/Scenes/Sandbox.hscene"});
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
	FTextureHandle RegisteredSceneTexture;
	std::uint64_t SceneTextureId = 0;
	RenderFrame = [&]
	{
		if (bRendering || bRenderFailed)
		{
			return false;
		}

		bRendering = true;
		const auto CpuFrameStart = std::chrono::steady_clock::now();
		Presentation->SetToolUIGpuTimingEnabled(EditorFramework->IsUnitStatsVisible());
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

			auto MeshFrame = MeshRenderer->Render(ViewExtent, EditorFramework->GetViewportRenderView(), EditorFramework->GetViewportDebugDrawLists());
			if (!MeshFrame)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not render scene: {}", MeshFrame.error().Message);
				bFrameFailed = true;
				return;
			}

			if (RegisteredSceneTexture != MeshRenderer->GetColorTarget())
			{
				auto TextureId = Presentation->RegisterToolUITexture(MeshRenderer->GetColorTarget(), true);
				if (!TextureId)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Could not display scene: {}", TextureId.error().Message);
					bFrameFailed = true;
					return;
				}

				Presentation->UnregisterToolUITexture(SceneTextureId);
				SceneTextureId = *TextureId;
				RegisteredSceneTexture = MeshRenderer->GetColorTarget();
				EditorFramework->SetViewportImage(SceneTextureId);
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
		}

		return bPresentedMainFrame && !bFrameFailed;
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
	while (!Window.ShouldClose() && !bRenderFailed)
	{
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
		if (RenderFrame())
		{
			++SmokeFrameCount;
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
		const bool bValidationRequested = Herta::HasArgument(ArgumentCount, Arguments, "--validation");
		const std::string_view ExpectedWindowSystem = Herta::FindArgumentValue(ArgumentCount, Arguments, "--expect-window-system=");
		return Herta::RunEditor(ExecutablePath, bSmokeTest || bRendererTest, bPlatformSmokeTest, bRendererTest, bValidationRequested, ExpectedWindowSystem);
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
