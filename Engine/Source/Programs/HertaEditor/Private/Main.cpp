#include "Herta/Application/Application.h"
#include "Herta/Core/Build.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/NvrhiVulkan/NvrhiVulkan.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/ToolUI.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory EditorLog{"Editor"};

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
	    static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferWidth())),
	    static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferHeight()))};
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

int RunEditor(const std::filesystem::path& ExecutablePath, const bool bSmokeTest, const bool bPlatformSmokeTest, const std::string_view ExpectedWindowSystem)
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
		(void)Application->PumpEvents();
	}

	if (bPlatformSmokeTest)
	{
		for (std::uint32_t Iteration = 0; Iteration < 3; ++Iteration)
		{
			(void)Application->PumpEvents();
			(void)TaskSystem->RunMainThreadTasks();
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
	PresentationDescriptor.bEnableValidation = GetBuildConfiguration() != EBuildConfiguration::Shipping;
	PresentationDescriptor.Log = Log.get();
	std::expected<std::unique_ptr<INvrhiVulkanPresentation>, FPresentationError> PresentationResult = CreateNvrhiVulkanPresentation(std::move(PresentationDescriptor));
	if (!PresentationResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize Vulkan presentation: {}", PresentationResult.error().Message);
		return 1;
	}
	std::unique_ptr<INvrhiVulkanPresentation> Presentation = std::move(*PresentationResult);

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
	RendererBridge.Initialize = [&Presentation](const std::span<const std::byte> FontRgba8, const std::uint32_t Width, const std::uint32_t Height) -> std::expected<std::uint64_t, FToolUIError>
	{
		std::expected<std::uint64_t, FPresentationError> Result = Presentation->InitializeToolUIRenderer(FontRgba8, Width, Height);
		if (!Result)
		{
			return std::unexpected(FToolUIError{std::move(Result.error().Message)});
		}
		return *Result;
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
		std::expected<FPresentationViewportHandle, FPresentationError> Result = Presentation->CreateViewport(WindowBackendHandle, {Width, Height});
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
		std::expected<void, FPresentationError> Result = Presentation->ResizeViewport({Handle}, {Width, Height});
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
	FToolUIDescriptor ToolUIDescriptor;
	ToolUIDescriptor.Application = Application.get();
	ToolUIDescriptor.Window = &Window;
	ToolUIDescriptor.RegularFontPath = RepositoryRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf";
	ToolUIDescriptor.MediumFontPath = RepositoryRoot / "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf";
	ToolUIDescriptor.LayoutPath = RepositoryRoot / "Saved/Editor/ImGui.ini";
	ToolUIDescriptor.AppearancePath = RepositoryRoot / "Saved/Editor/Appearance.ini";
	ToolUIDescriptor.Renderer = std::move(RendererBridge);
	ToolUIDescriptor.RefreshRequested = [&RenderFrame]
	{
		if (RenderFrame)
		{
			(void)RenderFrame();
		}
	};
	std::expected<std::unique_ptr<FToolUIContext>, FToolUIError> ToolUIResult = FToolUIContext::Create(std::move(ToolUIDescriptor));
	if (!ToolUIResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize ToolUI: {}", ToolUIResult.error().Message);
		return 1;
	}
	std::unique_ptr<FToolUIContext> ToolUI = std::move(*ToolUIResult);

	std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> EditorFrameworkResult = FEditorFramework::Create({.Log = Log.get(), .Commands = &Commands, .ToolUI = ToolUI.get()});
	if (!EditorFrameworkResult)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Could not initialize EditorFramework: {}", EditorFrameworkResult.error().Message);
		return 1;
	}
	std::unique_ptr<FEditorFramework> EditorFramework = std::move(*EditorFrameworkResult);
	const FToolUIColor CanvasColor = ToolUITheme::Canvas;
	const FLinearColor EditorClearColor = ConvertSrgb8ToLinearColor(CanvasColor.Red, CanvasColor.Green, CanvasColor.Blue, CanvasColor.Alpha);

	bool bRenderFailed = false;
	RenderFrame = [&]
	{
		if (bRendering || bRenderFailed)
		{
			return false;
		}
		bRendering = true;
		struct FRenderGuard
		{
			bool& bRendering;
			~FRenderGuard()
			{
				bRendering = false;
			}
		} RenderGuard{bRendering};

		const FExtent2D FramebufferExtent = GetFramebufferExtent(Window);
		if (!FramebufferExtent.IsEmpty() && FramebufferExtent != Presentation->GetExtent())
		{
			std::expected<void, FPresentationError> ResizeResult = Presentation->Resize(FramebufferExtent);
			if (!ResizeResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not resize Vulkan presentation: {}", ResizeResult.error().Message);
				bRenderFailed = true;
				return false;
			}
		}

		std::expected<EPresentationStatus, FPresentationError> BeginResult = Presentation->BeginFrame();
		if (!BeginResult)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not begin the editor frame: {}", BeginResult.error().Message);
			bRenderFailed = true;
			return false;
		}
		bool bMainFrameReady = *BeginResult == EPresentationStatus::Ready || *BeginResult == EPresentationStatus::Suboptimal;
		if (*BeginResult == EPresentationStatus::SurfaceOutOfDate)
		{
			if (!FramebufferExtent.IsEmpty())
			{
				std::expected<void, FPresentationError> RecreateResult = Presentation->Resize(FramebufferExtent);
				if (!RecreateResult)
				{
					HERTA_LOG_ERROR(*Log, EditorLog, "Could not recreate Vulkan presentation: {}", RecreateResult.error().Message);
					bRenderFailed = true;
					return false;
				}
			}
			bMainFrameReady = false;
		}

		bool bFrameFailed = false;
		bool bPresentedMainFrame = false;
		if (bMainFrameReady)
		{
			std::expected<void, FPresentationError> ClearResult = Presentation->Clear(EditorClearColor);
			if (!ClearResult)
			{
				HERTA_LOG_ERROR(*Log, EditorLog, "Could not clear the editor frame: {}", ClearResult.error().Message);
				bFrameFailed = true;
			}
		}

		ToolUI->BeginFrame();
		std::expected<void, FEditorFrameworkError> DrawResult = EditorFramework->Draw();
		if (!DrawResult)
		{
			HERTA_LOG_ERROR(*Log, EditorLog, "Could not draw the editor: {}", DrawResult.error().Message);
			bFrameFailed = true;
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
	while (!Window.ShouldClose() && !bRenderFailed)
	{
		(void)Application->PumpEvents();
		(void)TaskSystem->RunMainThreadTasks();
		if (RenderFrame())
		{
			++SmokeFrameCount;
		}

		if (bSmokeTest && SmokeFrameCount >= 3)
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
		const std::string_view ExpectedWindowSystem = Herta::FindArgumentValue(ArgumentCount, Arguments, "--expect-window-system=");
		return Herta::RunEditor(ExecutablePath, bSmokeTest, bPlatformSmokeTest, ExpectedWindowSystem);
	}
	catch (const std::exception& Exception)
	{
		std::println(stderr, "Herta Editor failed: {}", Exception.what());
		return 1;
	}
	catch (...)
	{
		std::println(stderr, "Herta Editor failed due to an unknown error");
		return 1;
	}
}
