#include "Herta/Application/Application.h"

#include "Herta/Application/WindowPlacement.h"
#include "Herta/Core/Log.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef HERTA_PLATFORM_WINDOWS
	#define GLFW_EXPOSE_NATIVE_WIN32
	#include <GLFW/glfw3native.h>
	#undef CreateWindow
#endif
#ifdef HERTA_PLATFORM_LINUX
	#define GLFW_EXPOSE_NATIVE_X11
	#include <GLFW/glfw3native.h>
	#include <X11/Xatom.h>
	#undef None
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <thread>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory ApplicationLog{.Name = "Application"};
constinit std::atomic_bool GApplicationExists = false;
thread_local int GWindowCallbackDispatchDepth = 0;

[[nodiscard]] constexpr EWindowSystem ToWindowSystem(const int Platform) noexcept
{
	switch (Platform)
	{
		case GLFW_PLATFORM_WIN32:
			return EWindowSystem::Win32;
		case GLFW_PLATFORM_X11:
			return EWindowSystem::X11;
		case GLFW_PLATFORM_WAYLAND:
			return EWindowSystem::Wayland;
		case GLFW_PLATFORM_NULL:
			return EWindowSystem::Null;
		default:
			return EWindowSystem::Unknown;
	}
}

[[nodiscard]] constexpr int ToGlfwPlatform(const EWindowSystem WindowSystem) noexcept
{
	switch (WindowSystem)
	{
		case EWindowSystem::Win32:
			return GLFW_PLATFORM_WIN32;
		case EWindowSystem::X11:
			return GLFW_PLATFORM_X11;
		case EWindowSystem::Wayland:
			return GLFW_PLATFORM_WAYLAND;
		case EWindowSystem::Null:
			return GLFW_PLATFORM_NULL;
		case EWindowSystem::Unknown:
			return GLFW_ANY_PLATFORM;
	}

	return GLFW_ANY_PLATFORM;
}

[[nodiscard]] constexpr EInputAction ToInputAction(const int Action) noexcept
{
	switch (Action)
	{
		case GLFW_PRESS:
			return EInputAction::Pressed;
		case GLFW_REPEAT:
			return EInputAction::Repeated;
		case GLFW_RELEASE:
		default:
			return EInputAction::Released;
	}
}

[[nodiscard]] constexpr EModifierFlags ToModifierFlags(const int Modifiers) noexcept
{
	EModifierFlags Flags = EModifierFlags::None;
	if ((Modifiers & GLFW_MOD_SHIFT) != 0)
	{
		Flags = Flags | EModifierFlags::Shift;
	}

	if ((Modifiers & GLFW_MOD_CONTROL) != 0)
	{
		Flags = Flags | EModifierFlags::Control;
	}

	if ((Modifiers & GLFW_MOD_ALT) != 0)
	{
		Flags = Flags | EModifierFlags::Alt;
	}

	if ((Modifiers & GLFW_MOD_SUPER) != 0)
	{
		Flags = Flags | EModifierFlags::Super;
	}

	if ((Modifiers & GLFW_MOD_CAPS_LOCK) != 0)
	{
		Flags = Flags | EModifierFlags::CapsLock;
	}

	if ((Modifiers & GLFW_MOD_NUM_LOCK) != 0)
	{
		Flags = Flags | EModifierFlags::NumLock;
	}

	return Flags;
}

[[nodiscard]] constexpr int ToGlfwHitTest(const ETitleBarHitRegion Region) noexcept
{
	switch (Region)
	{
		case ETitleBarHitRegion::Caption:
			return GLFW_HIT_TEST_CAPTION;
		case ETitleBarHitRegion::ResizeLeft:
			return GLFW_HIT_TEST_RESIZE_LEFT;
		case ETitleBarHitRegion::ResizeRight:
			return GLFW_HIT_TEST_RESIZE_RIGHT;
		case ETitleBarHitRegion::ResizeTop:
			return GLFW_HIT_TEST_RESIZE_TOP;
		case ETitleBarHitRegion::ResizeBottom:
			return GLFW_HIT_TEST_RESIZE_BOTTOM;
		case ETitleBarHitRegion::ResizeTopLeft:
			return GLFW_HIT_TEST_RESIZE_TOP_LEFT;
		case ETitleBarHitRegion::ResizeTopRight:
			return GLFW_HIT_TEST_RESIZE_TOP_RIGHT;
		case ETitleBarHitRegion::ResizeBottomLeft:
			return GLFW_HIT_TEST_RESIZE_BOTTOM_LEFT;
		case ETitleBarHitRegion::ResizeBottomRight:
			return GLFW_HIT_TEST_RESIZE_BOTTOM_RIGHT;
		case ETitleBarHitRegion::SystemMenu:
			return GLFW_HIT_TEST_SYSTEM_MENU;
		case ETitleBarHitRegion::MinimizeButton:
			return GLFW_HIT_TEST_MINIMIZE_BUTTON;
		case ETitleBarHitRegion::MaximizeButton:
			return GLFW_HIT_TEST_MAXIMIZE_BUTTON;
		case ETitleBarHitRegion::CloseButton:
			return GLFW_HIT_TEST_CLOSE_BUTTON;
		case ETitleBarHitRegion::Client:
		case ETitleBarHitRegion::ApplicationMenu:
			return GLFW_HIT_TEST_CLIENT;
	}

	return GLFW_HIT_TEST_CLIENT;
}

void GlfwErrorCallback(const int Error, const char* const Description) noexcept
{
	try
	{
		std::println(stderr, "GLFW error {}: {}", Error, Description ? Description : "Unknown error");
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// GLFW callbacks must never unwind into C code.
	}
}

[[nodiscard]] bool ApplyTaskbarVisibility(GLFWwindow* const Handle, const EWindowSystem WindowSystem, const bool bShowInTaskbar) noexcept
{
	if (bShowInTaskbar)
	{
		return true;
	}

#ifdef HERTA_PLATFORM_WINDOWS
	if (WindowSystem == EWindowSystem::Win32)
	{
		HWND const NativeWindow = glfwGetWin32Window(Handle);
		if (NativeWindow == nullptr)
		{
			return false;
		}

		SetLastError(ERROR_SUCCESS);
		const LONG_PTR ExtendedStyle = GetWindowLongPtrW(NativeWindow, GWL_EXSTYLE);
		if (ExtendedStyle == 0 && GetLastError() != ERROR_SUCCESS)
		{
			return false;
		}

		const LONG_PTR ToolWindowStyle = (ExtendedStyle & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) | WS_EX_TOOLWINDOW;
		SetLastError(ERROR_SUCCESS);
		if (SetWindowLongPtrW(NativeWindow, GWL_EXSTYLE, ToolWindowStyle) == 0 && GetLastError() != ERROR_SUCCESS)
		{
			return false;
		}

		return SetWindowPos(NativeWindow, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER) != FALSE;
	}
#endif

#ifdef HERTA_PLATFORM_LINUX
	if (WindowSystem == EWindowSystem::X11)
	{
		Display* const DisplayConnection = glfwGetX11Display();
		const ::Window NativeWindow = glfwGetX11Window(Handle);
		if (DisplayConnection == nullptr || NativeWindow == 0)
		{
			return false;
		}

		const Atom WindowState = XInternAtom(DisplayConnection, "_NET_WM_STATE", False);
		const Atom SkipTaskbar = XInternAtom(DisplayConnection, "_NET_WM_STATE_SKIP_TASKBAR", False);
		if (WindowState == 0 || SkipTaskbar == 0)
		{
			return false;
		}

		XChangeProperty(DisplayConnection, NativeWindow, WindowState, XA_ATOM, 32, PropModeAppend, reinterpret_cast<const unsigned char*>(&SkipTaskbar), 1);
		XFlush(DisplayConnection);
		return true;
	}
#endif

	return false;
}
}

struct FWindow::FImplementation
{
	GLFWwindow* Handle = nullptr;
	FWindow* Owner = nullptr;
	FLogService* Log = nullptr;
	std::thread::id MainThreadId;
	std::string Title;
	int Width = 0;
	int Height = 0;
	int PositionX = 0;
	int PositionY = 0;
	int FramebufferWidth = 0;
	int FramebufferHeight = 0;
	float ContentScale = 1.f;
	bool bVisible = false;
	bool bFocused = false;
	bool bMinimized = false;
	bool bMaximized = false;
	bool bShowInTaskbar = true;
	bool bTopMost = false;
	bool bFocusOnShow = true;
	bool bResizable = true;
	bool bCustomTitleBar = true;
	bool bWayland = false;
	bool bProgrammaticWindowPosition = true;
	bool bPendingDestruction = false;
	std::array<bool, static_cast<std::size_t>(EKey::Last) + 1> KeyStates{};
	std::array<bool, static_cast<std::size_t>(EMouseButton::Last) + 1> MouseButtonStates{};
	FWindowCallbacks Callbacks;
	FWindowActionCapabilities ActionCapabilities;
	FWindowActionPolicy ActionPolicy;
	FTitleBarHitTestState TitleBarHitTestState;

	void VerifyMainThread() const noexcept
	{
		if (std::this_thread::get_id() != MainThreadId)
		{
			std::terminate();
		}
	}

	void RefreshTitleBarLayout() noexcept
	{
		TitleBarHitTestState.Layout = MakeTitleBarLayout(Width, Height, ContentScale, bResizable, bMaximized, ActionCapabilities, ActionPolicy);
	}

	void RefreshActionCapabilities() noexcept
	{
		const FWindowActionCapabilities LatestCapabilities{
		    .bMinimize = glfwGetWindowAttrib(Handle, GLFW_MINIMIZE_SUPPORTED) == GLFW_TRUE,
		    .bMaximize = glfwGetWindowAttrib(Handle, GLFW_MAXIMIZE_SUPPORTED) == GLFW_TRUE,
		    .bWindowMenu = glfwGetWindowAttrib(Handle, GLFW_WINDOW_MENU_SUPPORTED) == GLFW_TRUE,
		};

		if (LatestCapabilities == ActionCapabilities)
		{
			return;
		}

		ActionCapabilities = LatestCapabilities;
		RefreshTitleBarLayout();
	}
};

namespace
{
[[nodiscard]] FWindow::FImplementation* GetWindowImplementation(GLFWwindow* const Handle) noexcept
{
	return static_cast<FWindow::FImplementation*>(glfwGetWindowUserPointer(Handle));
}

class FWindowCallbackScope final
{
public:
	FWindowCallbackScope() noexcept
	{
		++GWindowCallbackDispatchDepth;
	}

	~FWindowCallbackScope()
	{
		--GWindowCallbackDispatchDepth;
	}

	FWindowCallbackScope(const FWindowCallbackScope&) = delete;
	FWindowCallbackScope& operator=(const FWindowCallbackScope&) = delete;
	FWindowCallbackScope(FWindowCallbackScope&&) = delete;
	FWindowCallbackScope& operator=(FWindowCallbackScope&&) = delete;
};

template <typename... CallbackArguments, typename... Arguments> void InvokeWindowCallback(FWindow::FImplementation* const Window, const std::function<void(FWindow&, CallbackArguments...)> FWindowCallbacks::* const Member, Arguments&&... Values) noexcept
{
	try
	{
		const std::function<void(FWindow&, CallbackArguments...)> Callback = Window->Callbacks.*Member;
		if (Callback)
		{
			const FWindowCallbackScope CallbackScope;
			Callback(*Window->Owner, std::forward<Arguments>(Values)...);
		}
	}
	catch (const std::exception& Exception)
	{
		// Platform callbacks cannot unwind through GLFW or the native window procedure.
		if (Window->Log)
		{
			HERTA_LOG_ERROR(*Window->Log, ApplicationLog, "Window callback failed: {}", Exception.what());
		}
	}
	catch (...)
	{
		if (Window->Log)
		{
			HERTA_LOG_ERROR(*Window->Log, ApplicationLog, "Window callback failed due to an unknown error");
		}
	}
}

void WindowPositionCallback(GLFWwindow* const Handle, const int X, const int Y) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->PositionX = X;
	Window->PositionY = Y;
	InvokeWindowCallback(Window, &FWindowCallbacks::Moved, X, Y);
}

void WindowSizeCallback(GLFWwindow* const Handle, const int Width, const int Height) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->Width = Width;
	Window->Height = Height;
	Window->RefreshTitleBarLayout();
	InvokeWindowCallback(Window, &FWindowCallbacks::Resized, Width, Height);
}

void WindowCloseCallback(GLFWwindow* const Handle) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::CloseRequested);
}

void WindowRefreshCallback(GLFWwindow* const Handle) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::RefreshRequested);
}

void WindowFocusCallback(GLFWwindow* const Handle, const int Focused) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->bFocused = Focused == GLFW_TRUE;
	InvokeWindowCallback(Window, &FWindowCallbacks::FocusChanged, Window->bFocused);
}

void WindowIconifyCallback(GLFWwindow* const Handle, const int Minimized) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->bMinimized = Minimized == GLFW_TRUE;
	InvokeWindowCallback(Window, &FWindowCallbacks::MinimizedChanged, Window->bMinimized);
}

void WindowMaximizeCallback(GLFWwindow* const Handle, const int Maximized) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->bMaximized = Maximized == GLFW_TRUE;
	Window->RefreshTitleBarLayout();
	InvokeWindowCallback(Window, &FWindowCallbacks::MaximizedChanged, Window->bMaximized);
}

void FramebufferSizeCallback(GLFWwindow* const Handle, const int Width, const int Height) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->FramebufferWidth = Width;
	Window->FramebufferHeight = Height;
	InvokeWindowCallback(Window, &FWindowCallbacks::FramebufferResized, Width, Height);
}

void ContentScaleCallback(GLFWwindow* const Handle, const float XScale, const float YScale) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	Window->ContentScale = ResolveTitleBarUiScale(Window->bWayland, std::max(XScale, YScale));
	Window->RefreshTitleBarLayout();
	InvokeWindowCallback(Window, &FWindowCallbacks::ContentScaleChanged, XScale, YScale);
}

void KeyCallback(GLFWwindow* const Handle, const int Key, int, const int Action, const int Modifiers) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	if (Key < 0 || Key > static_cast<int>(EKey::Last))
	{
		return;
	}

	Window->KeyStates[static_cast<std::size_t>(Key)] = Action != GLFW_RELEASE;
	InvokeWindowCallback(Window, &FWindowCallbacks::KeyChanged, static_cast<EKey>(Key), ToInputAction(Action), ToModifierFlags(Modifiers));
}

void CharacterCallback(GLFWwindow* const Handle, const unsigned int Codepoint) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::TextInput, static_cast<char32_t>(Codepoint));
}

void MouseButtonCallback(GLFWwindow* const Handle, const int Button, const int Action, const int Modifiers) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	if (Button < 0 || Button > static_cast<int>(EMouseButton::Last))
	{
		return;
	}

	Window->MouseButtonStates[static_cast<std::size_t>(Button)] = Action != GLFW_RELEASE;
	InvokeWindowCallback(Window, &FWindowCallbacks::MouseButtonChanged, static_cast<EMouseButton>(Button), ToInputAction(Action), ToModifierFlags(Modifiers));
}

void CursorPositionCallback(GLFWwindow* const Handle, const double X, const double Y) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::CursorMoved, X, Y);
}

void CursorEnterCallback(GLFWwindow* const Handle, const int Entered) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::CursorEntered, Entered == GLFW_TRUE);
}

void ScrollCallback(GLFWwindow* const Handle, const double X, const double Y) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	InvokeWindowCallback(Window, &FWindowCallbacks::Scrolled, X, Y);
}

void DropCallback(GLFWwindow* const Handle, const int Count, const char** const Paths) noexcept
{
	FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	if (!Window->Callbacks.FilesDropped)
	{
		return;
	}

	try
	{
		std::vector<std::filesystem::path> DroppedPaths;
		DroppedPaths.reserve(static_cast<std::size_t>(Count));
		for (int Index = 0; Index < Count; ++Index)
		{
			DroppedPaths.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(Paths[Index])));
		}

		InvokeWindowCallback(Window, &FWindowCallbacks::FilesDropped, std::span<const std::filesystem::path>(DroppedPaths));
	}
	catch (...)
	{
		if (Window->Log)
		{
			HERTA_LOG_ERROR(*Window->Log, ApplicationLog, "Could not copy dropped file paths");
		}
	}
}

int TitleBarHitTestCallback(GLFWwindow* const Handle, const int X, const int Y) noexcept
{
	const FWindow::FImplementation* const Window = GetWindowImplementation(Handle);
	return ToGlfwHitTest(HitTestTitleBar(Window->TitleBarHitTestState, X, Y));
}

void InstallWindowCallbacks(GLFWwindow* const Handle)
{
	glfwSetWindowPosCallback(Handle, WindowPositionCallback);
	glfwSetWindowSizeCallback(Handle, WindowSizeCallback);
	glfwSetWindowCloseCallback(Handle, WindowCloseCallback);
	glfwSetWindowRefreshCallback(Handle, WindowRefreshCallback);
	glfwSetWindowFocusCallback(Handle, WindowFocusCallback);
	glfwSetWindowIconifyCallback(Handle, WindowIconifyCallback);
	glfwSetWindowMaximizeCallback(Handle, WindowMaximizeCallback);
	glfwSetFramebufferSizeCallback(Handle, FramebufferSizeCallback);
	glfwSetWindowContentScaleCallback(Handle, ContentScaleCallback);
	glfwSetKeyCallback(Handle, KeyCallback);
	glfwSetCharCallback(Handle, CharacterCallback);
	glfwSetMouseButtonCallback(Handle, MouseButtonCallback);
	glfwSetCursorPosCallback(Handle, CursorPositionCallback);
	glfwSetCursorEnterCallback(Handle, CursorEnterCallback);
	glfwSetScrollCallback(Handle, ScrollCallback);
	glfwSetDropCallback(Handle, DropCallback);
	glfwSetWindowHitTestCallback(Handle, TitleBarHitTestCallback);
}
}

struct FApplication::FImplementation
{
	std::thread::id MainThreadId;
	FLogService* Log = nullptr;
	FApplicationCapabilities Capabilities;
	std::vector<std::unique_ptr<FWindow>> Windows;
	bool bHasDeferredWindowDestruction = false;

	void VerifyMainThread() const noexcept
	{
		if (std::this_thread::get_id() != MainThreadId)
		{
			std::terminate();
		}
	}
};

FWindow::FWindow(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
	Implementation->Owner = this;
}

FWindow::~FWindow()
{
	Implementation->VerifyMainThread();
	if (Implementation->Handle)
	{
		glfwDestroyWindow(Implementation->Handle);
	}
}

std::string_view FWindow::GetTitle() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->Title;
}

void FWindow::SetTitle(std::string Title)
{
	Implementation->VerifyMainThread();
	Implementation->Title = std::move(Title);
	glfwSetWindowTitle(Implementation->Handle, Implementation->Title.c_str());
}

int FWindow::GetWidth() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->Width;
}

int FWindow::GetHeight() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->Height;
}

int FWindow::GetFramebufferWidth() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->FramebufferWidth;
}

int FWindow::GetFramebufferHeight() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->FramebufferHeight;
}

FWindowPosition FWindow::GetPosition() const noexcept
{
	Implementation->VerifyMainThread();
	return {.X = Implementation->PositionX, .Y = Implementation->PositionY};
}

float FWindow::GetContentScale() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->ContentScale;
}

bool FWindow::IsVisible() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bVisible;
}

bool FWindow::IsFocused() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bFocused;
}

bool FWindow::IsMinimized() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bMinimized;
}

bool FWindow::IsMaximized() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bMaximized;
}

const FWindowActionCapabilities& FWindow::GetActionCapabilities() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->ActionCapabilities;
}

const FWindowActionPolicy& FWindow::GetActionPolicy() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->ActionPolicy;
}

bool FWindow::IsShownInTaskbar() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bShowInTaskbar;
}

bool FWindow::IsTopMost() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bTopMost;
}

bool FWindow::WillFocusOnShow() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->bFocusOnShow;
}

bool FWindow::IsKeyDown(const EKey Key) const noexcept
{
	Implementation->VerifyMainThread();
	const int KeyIndex = static_cast<int>(Key);
	return KeyIndex >= 0 && KeyIndex <= static_cast<int>(EKey::Last) && Implementation->KeyStates[static_cast<std::size_t>(KeyIndex)];
}

bool FWindow::IsMouseButtonDown(const EMouseButton Button) const noexcept
{
	Implementation->VerifyMainThread();
	const std::size_t ButtonIndex = static_cast<std::size_t>(Button);
	return ButtonIndex < Implementation->MouseButtonStates.size() && Implementation->MouseButtonStates[ButtonIndex];
}

bool FWindow::ShouldClose() const noexcept
{
	Implementation->VerifyMainThread();
	return glfwWindowShouldClose(Implementation->Handle) == GLFW_TRUE;
}

FWindowBackendHandle FWindow::GetBackendHandle() const noexcept
{
	Implementation->VerifyMainThread();
	return {Implementation->Handle};
}

std::expected<std::string, FApplicationError> FWindow::GetClipboardText() const
{
	Implementation->VerifyMainThread();
	const char* const Text = glfwGetClipboardString(Implementation->Handle);
	if (!Text)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::ClipboardUnavailable, .Message = "The platform clipboard does not contain UTF-8 text"});
	}

	return std::string(Text);
}

void FWindow::Show()
{
	Implementation->VerifyMainThread();
	glfwShowWindow(Implementation->Handle);
	Implementation->RefreshActionCapabilities();
	Implementation->bVisible = true;
}

void FWindow::Hide()
{
	Implementation->VerifyMainThread();
	glfwHideWindow(Implementation->Handle);
	Implementation->bVisible = false;
}

void FWindow::Minimize()
{
	Implementation->VerifyMainThread();
	glfwIconifyWindow(Implementation->Handle);
}

void FWindow::Maximize()
{
	Implementation->VerifyMainThread();
	glfwMaximizeWindow(Implementation->Handle);
}

void FWindow::Restore()
{
	Implementation->VerifyMainThread();
	glfwRestoreWindow(Implementation->Handle);
}

void FWindow::Focus()
{
	Implementation->VerifyMainThread();
	glfwFocusWindow(Implementation->Handle);
}

std::expected<void, FApplicationError> FWindow::SetPosition(const int X, const int Y)
{
	Implementation->VerifyMainThread();
	if (!Implementation->bProgrammaticWindowPosition)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::OperationUnsupported, .Message = "The active window system does not support programmatic window positioning"});
	}

	glfwSetWindowPos(Implementation->Handle, X, Y);
	Implementation->PositionX = X;
	Implementation->PositionY = Y;
	return {};
}

std::expected<void, FApplicationError> FWindow::SetSize(const int Width, const int Height)
{
	Implementation->VerifyMainThread();
	if (Width <= 0 || Height <= 0)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::InvalidWindowDescriptor, .Message = "Window dimensions must be greater than zero"});
	}

	glfwSetWindowSize(Implementation->Handle, Width, Height);
	return {};
}

void FWindow::RequestClose() noexcept
{
	SetShouldClose(true);
}

void FWindow::SetShouldClose(const bool bShouldClose) noexcept
{
	Implementation->VerifyMainThread();
	glfwSetWindowShouldClose(Implementation->Handle, bShouldClose ? GLFW_TRUE : GLFW_FALSE);
}

void FWindow::SetClipboardText(const std::string_view Text)
{
	Implementation->VerifyMainThread();
	const std::string NullTerminatedText(Text);
	glfwSetClipboardString(Implementation->Handle, NullTerminatedText.c_str());
}

void FWindow::SetCallbacks(FWindowCallbacks Callbacks)
{
	Implementation->VerifyMainThread();
	Implementation->Callbacks = std::move(Callbacks);
}

void FWindow::SetActionPolicy(const FWindowActionPolicy Policy) noexcept
{
	Implementation->VerifyMainThread();
	if (Implementation->ActionPolicy == Policy)
	{
		return;
	}

	Implementation->ActionPolicy = Policy;
	Implementation->RefreshTitleBarLayout();
}

void FWindow::SetTitleBarHitTestState(FTitleBarHitTestState State) noexcept
{
	Implementation->VerifyMainThread();
	Implementation->TitleBarHitTestState = State;
}

const FTitleBarHitTestState& FWindow::GetTitleBarHitTestState() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->TitleBarHitTestState;
}

std::expected<std::unique_ptr<FApplication>, FApplicationError> FApplication::Create(FLogService* const Log)
{
	return Create({}, Log);
}

std::expected<std::unique_ptr<FApplication>, FApplicationError> FApplication::Create(const FApplicationDescriptor Descriptor, FLogService* const Log)
{
	bool bExpected = false;
	if (!GApplicationExists.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::AlreadyInitialized, .Message = "Only one GLFW application may exist in a process"});
	}

	glfwSetErrorCallback(GlfwErrorCallback);
	glfwInitHint(GLFW_PLATFORM, ToGlfwPlatform(Descriptor.PreferredWindowSystem));
	if (glfwInit() != GLFW_TRUE)
	{
		const char* Description = nullptr;
		const int Error = glfwGetError(&Description);
		GApplicationExists.store(false, std::memory_order_release);
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::GlfwInitializationFailed, .Message = std::format("Could not initialize GLFW ({}): {}", Error, Description ? Description : "Unknown error")});
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->MainThreadId = std::this_thread::get_id();
	Implementation->Log = Log;
	Implementation->Capabilities.WindowSystem = ToWindowSystem(glfwGetPlatform());
	Implementation->Capabilities.bCustomTitleBars = Implementation->Capabilities.WindowSystem == EWindowSystem::Win32 || Implementation->Capabilities.WindowSystem == EWindowSystem::X11 || Implementation->Capabilities.WindowSystem == EWindowSystem::Wayland;
	Implementation->Capabilities.bProgrammaticWindowPosition = Implementation->Capabilities.WindowSystem != EWindowSystem::Wayland && Implementation->Capabilities.WindowSystem != EWindowSystem::Null;
	Implementation->Capabilities.bTaskbarVisibility = Implementation->Capabilities.WindowSystem == EWindowSystem::Win32 || Implementation->Capabilities.WindowSystem == EWindowSystem::X11;
	Implementation->Capabilities.bTopMostWindows = Implementation->Capabilities.WindowSystem == EWindowSystem::Win32 || Implementation->Capabilities.WindowSystem == EWindowSystem::X11;
	Implementation->Capabilities.bVulkanPresentation = glfwVulkanSupported() == GLFW_TRUE;

	return std::unique_ptr<FApplication>(new FApplication(std::move(Implementation)));
}

FApplication::FApplication(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FApplication::~FApplication()
{
	Implementation->VerifyMainThread();
	if (GWindowCallbackDispatchDepth > 0)
	{
		// Window destruction is deferred, but the process-wide GLFW owner cannot be destroyed reentrantly.
		std::terminate();
	}

	Implementation->Windows.clear();
	glfwTerminate();
	GApplicationExists.store(false, std::memory_order_release);
}

std::expected<FWindow*, FApplicationError> FApplication::CreateWindow(FWindowDescriptor Descriptor)
{
	Implementation->VerifyMainThread();
	if (Descriptor.Title.empty() || Descriptor.Width < 0 || Descriptor.Height < 0 || Descriptor.WorkAreaPercent < 1 || Descriptor.WorkAreaPercent > 100)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::InvalidWindowDescriptor, .Message = "Window title, dimensions, or work-area percentage are invalid"});
	}

	int WorkAreaX = 0;
	int WorkAreaY = 0;
	int WorkAreaWidth = 1280;
	int WorkAreaHeight = 720;
	if (GLFWmonitor* const Monitor = glfwGetPrimaryMonitor())
	{
		glfwGetMonitorWorkarea(Monitor, &WorkAreaX, &WorkAreaY, &WorkAreaWidth, &WorkAreaHeight);
	}

	FWindowPlacement Placement = ResolveCenteredWindowPlacement(WorkAreaX, WorkAreaY, WorkAreaWidth, WorkAreaHeight, Descriptor.WorkAreaPercent);
	if (Descriptor.Width > 0)
	{
		Placement.Width = Descriptor.Width;
		Placement.X = WorkAreaX + (WorkAreaWidth - Placement.Width) / 2;
	}

	if (Descriptor.Height > 0)
	{
		Placement.Height = Descriptor.Height;
		Placement.Y = WorkAreaY + (WorkAreaHeight - Placement.Height) / 2;
	}

	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	glfwWindowHint(GLFW_RESIZABLE, Descriptor.bResizable ? GLFW_TRUE : GLFW_FALSE);
	glfwWindowHint(GLFW_MAXIMIZED, Descriptor.bMaximized ? GLFW_TRUE : GLFW_FALSE);
	const FWindowBehaviorPolicy Behavior = ResolveWindowBehaviorPolicy(Implementation->Capabilities.WindowSystem, Descriptor);
	glfwWindowHint(GLFW_FOCUS_ON_SHOW, Behavior.bFocusOnShow ? GLFW_TRUE : GLFW_FALSE);
	if (Behavior.bTopMost)
	{
		glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
	}

	const bool bCustomTitleBar = Descriptor.bCustomTitleBar && Implementation->Capabilities.bCustomTitleBars;
	glfwWindowHint(GLFW_TITLEBAR, bCustomTitleBar ? GLFW_FALSE : GLFW_TRUE);

	GLFWwindow* const CreatedHandle = glfwCreateWindow(Placement.Width, Placement.Height, Descriptor.Title.c_str(), nullptr, nullptr);
	if (!CreatedHandle)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::WindowCreationFailed, .Message = "GLFW could not create the window"});
	}

	if (!ApplyTaskbarVisibility(CreatedHandle, Implementation->Capabilities.WindowSystem, Behavior.bShowInTaskbar))
	{
		glfwDestroyWindow(CreatedHandle);
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::WindowCreationFailed, .Message = "Could not apply the requested taskbar visibility"});
	}

	auto WindowImplementation = std::make_unique<FWindow::FImplementation>();
	WindowImplementation->Handle = CreatedHandle;
	WindowImplementation->Log = Implementation->Log;
	WindowImplementation->MainThreadId = Implementation->MainThreadId;
	WindowImplementation->Title = std::move(Descriptor.Title);
	WindowImplementation->bResizable = Descriptor.bResizable;
	WindowImplementation->bCustomTitleBar = bCustomTitleBar;
	WindowImplementation->bShowInTaskbar = Behavior.bShowInTaskbar;
	WindowImplementation->bTopMost = Behavior.bTopMost;
	WindowImplementation->bFocusOnShow = Behavior.bFocusOnShow;
	WindowImplementation->bWayland = Implementation->Capabilities.WindowSystem == EWindowSystem::Wayland;
	WindowImplementation->bProgrammaticWindowPosition = Implementation->Capabilities.bProgrammaticWindowPosition;
	WindowImplementation->bVisible = Descriptor.bVisible;
	WindowImplementation->bFocused = glfwGetWindowAttrib(CreatedHandle, GLFW_FOCUSED) == GLFW_TRUE;
	WindowImplementation->bMinimized = glfwGetWindowAttrib(CreatedHandle, GLFW_ICONIFIED) == GLFW_TRUE;
	WindowImplementation->bMaximized = glfwGetWindowAttrib(CreatedHandle, GLFW_MAXIMIZED) == GLFW_TRUE;
	glfwGetWindowSize(CreatedHandle, &WindowImplementation->Width, &WindowImplementation->Height);
	if (Implementation->Capabilities.bProgrammaticWindowPosition)
	{
		glfwGetWindowPos(CreatedHandle, &WindowImplementation->PositionX, &WindowImplementation->PositionY);
	}

	glfwGetFramebufferSize(CreatedHandle, &WindowImplementation->FramebufferWidth, &WindowImplementation->FramebufferHeight);
	float XScale = 1.f;
	float YScale = 1.f;
	glfwGetWindowContentScale(CreatedHandle, &XScale, &YScale);
	WindowImplementation->ContentScale = ResolveTitleBarUiScale(Implementation->Capabilities.WindowSystem == EWindowSystem::Wayland, std::max(XScale, YScale));
	WindowImplementation->RefreshActionCapabilities();
	WindowImplementation->RefreshTitleBarLayout();

	auto Window = std::unique_ptr<FWindow>(new FWindow(std::move(WindowImplementation)));
	GLFWwindow* const Handle = Window->Implementation->Handle;
	glfwSetWindowUserPointer(Handle, Window->Implementation.get());
	InstallWindowCallbacks(Handle);
	if (Implementation->Capabilities.bProgrammaticWindowPosition && !Descriptor.bMaximized)
	{
		glfwSetWindowPos(Handle, Placement.X, Placement.Y);
	}

	if (Descriptor.bVisible)
	{
		Window->Show();
	}

	FWindow* const Result = Window.get();
	Implementation->Windows.emplace_back(std::move(Window));
	return Result;
}

void FApplication::DestroyWindow(FWindow& Window) noexcept
{
	Implementation->VerifyMainThread();

	const auto OwnedWindow = std::ranges::find_if(Implementation->Windows, [&Window](const std::unique_ptr<FWindow>& Candidate)
	{
		return Candidate.get() == &Window;
	});

	if (OwnedWindow == Implementation->Windows.end())
	{
		return;
	}

	if (GWindowCallbackDispatchDepth > 0)
	{
		Window.Implementation->bPendingDestruction = true;
		Implementation->bHasDeferredWindowDestruction = true;
		return;
	}

	Implementation->Windows.erase(OwnedWindow);
}

EEventPumpMode FApplication::PumpEvents()
{
	Implementation->VerifyMainThread();
	EEventPumpMode Mode = EEventPumpMode::Polled;
	if (ShouldWaitForEvents())
	{
		glfwWaitEventsTimeout(0.1);
		Mode = EEventPumpMode::Waited;
	}
	else
	{
		glfwPollEvents();
	}

	for (const std::unique_ptr<FWindow>& Window : Implementation->Windows)
	{
		if (!Window->Implementation->bPendingDestruction)
		{
			Window->Implementation->RefreshActionCapabilities();
		}
	}

	if (Implementation->bHasDeferredWindowDestruction)
	{
		std::erase_if(Implementation->Windows, [](const std::unique_ptr<FWindow>& Candidate)
		{
			return Candidate->Implementation->bPendingDestruction;
		});

		Implementation->bHasDeferredWindowDestruction = false;
	}

	return Mode;
}

void FApplication::PostEmptyEvent() noexcept
{
	glfwPostEmptyEvent();
}

bool FApplication::ShouldWaitForEvents() const noexcept
{
	Implementation->VerifyMainThread();
	bool bHasVisibleWindow = false;
	for (const std::unique_ptr<FWindow>& Window : Implementation->Windows)
	{
		if (!Window->IsVisible())
		{
			continue;
		}

		bHasVisibleWindow = true;
		if (!Window->IsMinimized())
		{
			return false;
		}
	}

	return !bHasVisibleWindow || !Implementation->Windows.empty();
}

const FApplicationCapabilities& FApplication::GetCapabilities() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->Capabilities;
}

std::span<const std::unique_ptr<FWindow>> FApplication::GetWindows() const noexcept
{
	Implementation->VerifyMainThread();
	return Implementation->Windows;
}

std::expected<std::vector<std::string>, FApplicationError> FApplication::GetRequiredVulkanInstanceExtensions() const
{
	Implementation->VerifyMainThread();
	if (!Implementation->Capabilities.bVulkanPresentation)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::VulkanUnavailable, .Message = "GLFW reports that Vulkan presentation is unavailable"});
	}

	std::uint32_t ExtensionCount = 0;
	const char** const Extensions = glfwGetRequiredInstanceExtensions(&ExtensionCount);
	if (!Extensions || ExtensionCount == 0)
	{
		return std::unexpected(FApplicationError{.Code = EApplicationErrorCode::VulkanUnavailable, .Message = "GLFW did not provide required Vulkan instance extensions"});
	}

	std::vector<std::string> Result;
	Result.reserve(ExtensionCount);
	for (std::uint32_t Index = 0; Index < ExtensionCount; ++Index)
	{
		Result.emplace_back(Extensions[Index]);
	}

	return Result;
}
}
