#pragma once

#include "Herta/Application/TitleBar.h"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FLogService;
class FWindow;

enum class EWindowSystem : std::uint8_t
{
	Unknown,
	Win32,
	X11,
	Wayland,
	Null
};

enum class EEventPumpMode : std::uint8_t
{
	Polled,
	Waited
};

enum class EInputAction : std::uint8_t
{
	Released,
	Pressed,
	Repeated
};

enum class EModifierFlags : std::uint8_t
{
	None = 0,
	Shift = 1 << 0,
	Control = 1 << 1,
	Alt = 1 << 2,
	Super = 1 << 3,
	CapsLock = 1 << 4,
	NumLock = 1 << 5
};

[[nodiscard]] constexpr EModifierFlags operator|(const EModifierFlags Left, const EModifierFlags Right) noexcept
{
	return static_cast<EModifierFlags>(static_cast<std::uint8_t>(Left) | static_cast<std::uint8_t>(Right));
}

[[nodiscard]] constexpr bool HasModifier(const EModifierFlags Flags, const EModifierFlags Modifier) noexcept
{
	return (static_cast<std::uint8_t>(Flags) & static_cast<std::uint8_t>(Modifier)) != 0;
}

enum class EKey : int
{
	Unknown = -1,
	Space = 32,
	Apostrophe = 39,
	Comma = 44,
	Minus = 45,
	Period = 46,
	Slash = 47,
	Zero = 48,
	One = 49,
	Two = 50,
	Three = 51,
	Four = 52,
	Five = 53,
	Six = 54,
	Seven = 55,
	Eight = 56,
	Nine = 57,
	Semicolon = 59,
	Equal = 61,
	A = 65,
	B = 66,
	C = 67,
	D = 68,
	E = 69,
	F = 70,
	G = 71,
	H = 72,
	I = 73,
	J = 74,
	K = 75,
	L = 76,
	M = 77,
	N = 78,
	O = 79,
	P = 80,
	Q = 81,
	R = 82,
	S = 83,
	T = 84,
	U = 85,
	V = 86,
	W = 87,
	X = 88,
	Y = 89,
	Z = 90,
	LeftBracket = 91,
	Backslash = 92,
	RightBracket = 93,
	GraveAccent = 96,
	World1 = 161,
	World2 = 162,
	Escape = 256,
	Enter = 257,
	Tab = 258,
	Backspace = 259,
	Insert = 260,
	Delete = 261,
	Right = 262,
	Left = 263,
	Down = 264,
	Up = 265,
	PageUp = 266,
	PageDown = 267,
	Home = 268,
	End = 269,
	CapsLock = 280,
	ScrollLock = 281,
	NumLock = 282,
	PrintScreen = 283,
	Pause = 284,
	F1 = 290,
	F2 = 291,
	F3 = 292,
	F4 = 293,
	F5 = 294,
	F6 = 295,
	F7 = 296,
	F8 = 297,
	F9 = 298,
	F10 = 299,
	F11 = 300,
	F12 = 301,
	F13 = 302,
	F14 = 303,
	F15 = 304,
	F16 = 305,
	F17 = 306,
	F18 = 307,
	F19 = 308,
	F20 = 309,
	F21 = 310,
	F22 = 311,
	F23 = 312,
	F24 = 313,
	F25 = 314,
	KeypadZero = 320,
	KeypadOne = 321,
	KeypadTwo = 322,
	KeypadThree = 323,
	KeypadFour = 324,
	KeypadFive = 325,
	KeypadSix = 326,
	KeypadSeven = 327,
	KeypadEight = 328,
	KeypadNine = 329,
	KeypadDecimal = 330,
	KeypadDivide = 331,
	KeypadMultiply = 332,
	KeypadSubtract = 333,
	KeypadAdd = 334,
	KeypadEnter = 335,
	KeypadEqual = 336,
	LeftShift = 340,
	LeftControl = 341,
	LeftAlt = 342,
	LeftSuper = 343,
	RightShift = 344,
	RightControl = 345,
	RightAlt = 346,
	RightSuper = 347,
	Menu = 348,
	Last = Menu
};

enum class EMouseButton : std::uint8_t
{
	Left,
	Right,
	Middle,
	Four,
	Five,
	Six,
	Seven,
	Eight,
	Last = Eight
};

enum class EApplicationErrorCode : std::uint8_t
{
	AlreadyInitialized,
	GlfwInitializationFailed,
	InvalidWindowDescriptor,
	WindowCreationFailed,
	VulkanUnavailable,
	ClipboardUnavailable,
	OperationUnsupported
};

struct FApplicationError
{
	EApplicationErrorCode Code = EApplicationErrorCode::GlfwInitializationFailed;
	std::string Message;
};

struct FApplicationCapabilities
{
	EWindowSystem WindowSystem = EWindowSystem::Unknown;
	bool bCustomTitleBars = false;
	bool bProgrammaticWindowPosition = false;
	bool bTaskbarVisibility = false;
	bool bTopMostWindows = false;
	bool bVulkanPresentation = false;
};

struct FApplicationDescriptor
{
	EWindowSystem PreferredWindowSystem = EWindowSystem::Unknown;
};

struct FWindowDescriptor
{
	std::string Title = "Herta";
	int Width = 0;
	int Height = 0;
	int WorkAreaPercent = 80;
	bool bVisible = false;
	bool bResizable = true;
	bool bCustomTitleBar = true;
	bool bMaximized = false;
	bool bShowInTaskbar = true;
	bool bTopMost = false;
	bool bFocusOnShow = true;
};

struct FWindowBehaviorPolicy
{
	bool bShowInTaskbar = true;
	bool bTopMost = false;
	bool bFocusOnShow = true;

	[[nodiscard]] constexpr bool operator==(const FWindowBehaviorPolicy&) const noexcept = default;
};

[[nodiscard]] constexpr FWindowBehaviorPolicy ResolveWindowBehaviorPolicy(const EWindowSystem WindowSystem, const FWindowDescriptor& Descriptor) noexcept
{
	const bool bTaskbarVisibilitySupported = WindowSystem == EWindowSystem::Win32 || WindowSystem == EWindowSystem::X11;
	const bool bTopMostSupported = WindowSystem == EWindowSystem::Win32 || WindowSystem == EWindowSystem::X11;
	return {
	    .bShowInTaskbar = bTaskbarVisibilitySupported ? Descriptor.bShowInTaskbar : true,
	    .bTopMost = bTopMostSupported && Descriptor.bTopMost,
	    .bFocusOnShow = Descriptor.bFocusOnShow};
}

struct FWindowBackendHandle
{
	void* Value = nullptr;
};

struct FWindowPosition
{
	int X = 0;
	int Y = 0;
};

struct FWindowCallbacks
{
	std::function<void(FWindow&)> CloseRequested;
	std::function<void(FWindow&, int, int)> Moved;
	std::function<void(FWindow&, int, int)> Resized;
	std::function<void(FWindow&, int, int)> FramebufferResized;
	std::function<void(FWindow&, float, float)> ContentScaleChanged;
	std::function<void(FWindow&, bool)> FocusChanged;
	std::function<void(FWindow&, bool)> MinimizedChanged;
	std::function<void(FWindow&, bool)> MaximizedChanged;
	std::function<void(FWindow&)> RefreshRequested;
	std::function<void(FWindow&, EKey, EInputAction, EModifierFlags)> KeyChanged;
	std::function<void(FWindow&, char32_t)> TextInput;
	std::function<void(FWindow&, EMouseButton, EInputAction, EModifierFlags)> MouseButtonChanged;
	std::function<void(FWindow&, double, double)> CursorMoved;
	std::function<void(FWindow&, bool)> CursorEntered;
	std::function<void(FWindow&, double, double)> Scrolled;
	std::function<void(FWindow&, std::span<const std::filesystem::path>)> FilesDropped;
};

class FWindow final
{
public:
	struct FImplementation;

	~FWindow();

	FWindow(const FWindow&) = delete;
	FWindow& operator=(const FWindow&) = delete;
	FWindow(FWindow&&) = delete;
	FWindow& operator=(FWindow&&) = delete;

	[[nodiscard]] std::string_view GetTitle() const noexcept;
	void SetTitle(std::string Title);
	[[nodiscard]] int GetWidth() const noexcept;
	[[nodiscard]] int GetHeight() const noexcept;
	[[nodiscard]] int GetFramebufferWidth() const noexcept;
	[[nodiscard]] int GetFramebufferHeight() const noexcept;
	[[nodiscard]] FWindowPosition GetPosition() const noexcept;
	[[nodiscard]] float GetContentScale() const noexcept;
	[[nodiscard]] bool IsVisible() const noexcept;
	[[nodiscard]] bool IsFocused() const noexcept;
	[[nodiscard]] bool IsMinimized() const noexcept;
	[[nodiscard]] bool IsMaximized() const noexcept;
	[[nodiscard]] bool IsShownInTaskbar() const noexcept;
	[[nodiscard]] bool IsTopMost() const noexcept;
	[[nodiscard]] bool WillFocusOnShow() const noexcept;
	[[nodiscard]] bool IsKeyDown(EKey Key) const noexcept;
	[[nodiscard]] bool IsMouseButtonDown(EMouseButton Button) const noexcept;
	[[nodiscard]] bool ShouldClose() const noexcept;
	[[nodiscard]] FWindowBackendHandle GetBackendHandle() const noexcept;
	[[nodiscard]] std::expected<std::string, FApplicationError> GetClipboardText() const;

	void Show();
	void Hide();
	void Minimize();
	void Maximize();
	void Restore();
	void Focus();
	[[nodiscard]] std::expected<void, FApplicationError> SetPosition(int X, int Y);
	[[nodiscard]] std::expected<void, FApplicationError> SetSize(int Width, int Height);
	void RequestClose() noexcept;
	void SetShouldClose(bool bShouldClose) noexcept;
	[[nodiscard]] std::expected<void, FApplicationError> SetClipboardText(std::string_view Text);
	void SetCallbacks(FWindowCallbacks Callbacks);
	void SetTitleBarHitTestState(FTitleBarHitTestState State) noexcept;
	[[nodiscard]] const FTitleBarHitTestState& GetTitleBarHitTestState() const noexcept;

private:
	explicit FWindow(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;

	friend class FApplication;
};

class FApplication final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FApplication>, FApplicationError> Create(FLogService* Log = nullptr);
	[[nodiscard]] static std::expected<std::unique_ptr<FApplication>, FApplicationError> Create(FApplicationDescriptor Descriptor, FLogService* Log = nullptr);

	~FApplication();

	FApplication(const FApplication&) = delete;
	FApplication& operator=(const FApplication&) = delete;
	FApplication(FApplication&&) = delete;
	FApplication& operator=(FApplication&&) = delete;

	[[nodiscard]] std::expected<FWindow*, FApplicationError> CreateWindow(FWindowDescriptor Descriptor);
	void DestroyWindow(FWindow& Window) noexcept;
	[[nodiscard]] EEventPumpMode PumpEvents();
	void PostEmptyEvent() noexcept;
	[[nodiscard]] bool ShouldWaitForEvents() const noexcept;
	[[nodiscard]] const FApplicationCapabilities& GetCapabilities() const noexcept;
	[[nodiscard]] std::span<const std::unique_ptr<FWindow>> GetWindows() const noexcept;
	[[nodiscard]] std::expected<std::vector<std::string>, FApplicationError> GetRequiredVulkanInstanceExtensions() const;

private:
	explicit FApplication(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
