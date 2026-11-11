#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The interface between the editor and a plugin, a shared library built from a plugin's source with only this header. Like a game module, a
// plugin links no engine library and no UI library: it draws through IEditorPluginUI, whose virtual calls run the editor's own widgets, so
// it never needs the editor's private headers or a second copy of its UI state.
namespace Herta
{
// Bump when anything in this header changes layout or meaning; the editor refuses plugins built against another version.
inline constexpr std::uint32_t EditorPluginInterfaceVersion = 1;

// Widgets a plugin panel draws with, laid out like the editor's own panels.
class IEditorPluginUI
{
public:
	IEditorPluginUI() = default;
	IEditorPluginUI(const IEditorPluginUI&) = delete;
	IEditorPluginUI& operator=(const IEditorPluginUI&) = delete;
	IEditorPluginUI(IEditorPluginUI&&) = delete;
	IEditorPluginUI& operator=(IEditorPluginUI&&) = delete;
	virtual ~IEditorPluginUI() = default;

	virtual void Text(std::string_view Text) = 0;
	virtual void TextDisabled(std::string_view Text) = 0;
	virtual void TextWrapped(std::string_view Text) = 0;
	// Lines of monospaced text, such as terminal output, without wrapping. Only visible lines are asked for, so long histories stay cheap.
	virtual void MonospaceLines(std::size_t Count, const std::function<std::string_view(std::size_t Line)>& GetLine) = 0;
	virtual bool Button(std::string_view Label) = 0;
	virtual bool Checkbox(std::string_view Label, bool& bValue) = 0;
	virtual bool Selectable(std::string_view Label, bool bSelected) = 0;
	// A single-line field across the remaining width; true when Enter is pressed in it.
	virtual bool InputText(std::string_view Id, std::string& Value, std::string_view Hint) = 0;
	virtual void InputTextMultiline(std::string_view Id, std::string& Value, float Lines) = 0;
	// Gives the next widget keyboard focus, such as a terminal's input after Enter.
	virtual void FocusNext() = 0;
	virtual void SameLine() = 0;
	virtual void Separator() = 0;
	// A scrolling region across the panel. Positive Lines sets its height in text lines; zero or negative fills the panel except that many
	// lines. With bFollowEnd it stays scrolled to the end while the user has not scrolled up.
	virtual void BeginScrollRegion(std::string_view Id, float Lines, bool bFollowEnd) = 0;
	virtual void EndScrollRegion() = 0;
	virtual void BeginDisabled(bool bDisabled) = 0;
	virtual void EndDisabled() = 0;
	virtual void PushId(std::string_view Id) = 0;
	virtual void PopId() = 0;
};

enum class EEditorPluginLog : std::uint8_t
{
	Info,
	Warning,
	Error
};

struct FEditorPluginProcess
{
	// A path, or a program name the editor finds on PATH.
	std::string Executable{};
	std::vector<std::string> Arguments{};
	// Set on top of the editor's environment.
	std::vector<std::pair<std::string, std::string>> Environment{};
	// Zero waits indefinitely; the editor still kills the process when it closes.
	std::uint32_t TimeoutMilliseconds = 0;
};

struct FEditorPluginProcessResult
{
	// False when the process could not start, timed out, or was cancelled; Error says why.
	bool bRan = false;
	int ExitCode = 0;
	std::string Output{};
	std::string Error{};
};

// Editor services a plugin may use, implemented by the editor.
class IEditorPluginHost
{
public:
	IEditorPluginHost() = default;
	IEditorPluginHost(const IEditorPluginHost&) = delete;
	IEditorPluginHost& operator=(const IEditorPluginHost&) = delete;
	IEditorPluginHost(IEditorPluginHost&&) = delete;
	IEditorPluginHost& operator=(IEditorPluginHost&&) = delete;
	virtual ~IEditorPluginHost() = default;

	virtual void Log(EEditorPluginLog Level, std::string_view Message) = 0;
	// UTF-8 paths of the open project's folder, empty without one, and of the engine checkout.
	[[nodiscard]] virtual std::string GetProjectDirectory() const = 0;
	[[nodiscard]] virtual std::string GetEngineDirectory() const = 0;
	// Runs the process on a background task, so the editor never waits for it. OnFinished runs on the main thread between frames, and never
	// after the editor starts closing.
	virtual void RunProcess(FEditorPluginProcess Process, std::function<void(const FEditorPluginProcessResult&)> OnFinished) = 0;
};

// A dockable panel, listed under Window and drawn each frame while open.
class IEditorPanel
{
public:
	IEditorPanel() = default;
	IEditorPanel(const IEditorPanel&) = delete;
	IEditorPanel& operator=(const IEditorPanel&) = delete;
	IEditorPanel(IEditorPanel&&) = delete;
	IEditorPanel& operator=(IEditorPanel&&) = delete;
	virtual ~IEditorPanel() = default;

	virtual void Draw(IEditorPluginUI& UI) = 0;
};

struct FEditorPluginCommandResult
{
	bool bSucceeded = true;
	std::string Message{};
};

using FEditorPluginCommand = std::function<FEditorPluginCommandResult(std::span<const std::string_view> Arguments)>;

// What a plugin registers when it loads. Everything it adds lives until the editor closes, since enabling or disabling a plugin takes a restart.
class IEditorPluginRegistry
{
public:
	IEditorPluginRegistry() = default;
	IEditorPluginRegistry(const IEditorPluginRegistry&) = delete;
	IEditorPluginRegistry& operator=(const IEditorPluginRegistry&) = delete;
	IEditorPluginRegistry(IEditorPluginRegistry&&) = delete;
	IEditorPluginRegistry& operator=(IEditorPluginRegistry&&) = delete;
	virtual ~IEditorPluginRegistry() = default;

	// Outlives everything the plugin registers.
	[[nodiscard]] virtual IEditorPluginHost& GetHost() = 0;
	virtual void AddPanel(std::string_view Title, std::unique_ptr<IEditorPanel> Panel) = 0;
	// An item in the Tools menu.
	virtual void AddMenuItem(std::string_view Label, std::function<void()> OnSelected) = 0;
	// A console command, which automation and the console run like the editor's own. Mutating commands need a write session.
	virtual void AddCommand(std::string_view Name, std::string_view Description, bool bMutates, FEditorPluginCommand Run) = 0;
};

// What a plugin exports, checked before its Register runs: the interface version, and the engine revision and configuration it was built
// against, since a plugin shares the editor's types and heap.
struct FEditorPluginExports
{
	std::uint32_t InterfaceVersion = 0;
	const char* EngineRevision = "";
	const char* Configuration = "";
	void (*Register)(IEditorPluginRegistry& Registry) = nullptr;
};

inline constexpr const char* EditorPluginEntryName = "HertaEditorPlugin";
using FEditorPluginEntry = const FEditorPluginExports* (*)();
}

#ifdef HERTA_PLATFORM_WINDOWS
	#define HERTA_EDITOR_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
	#define HERTA_EDITOR_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(HERTA_DEBUG)
	#define HERTA_EDITOR_PLUGIN_CONFIGURATION "Debug"
#elif defined(HERTA_DEVELOPMENT)
	#define HERTA_EDITOR_PLUGIN_CONFIGURATION "Development"
#else
	#define HERTA_EDITOR_PLUGIN_CONFIGURATION "Shipping"
#endif

#ifndef HERTA_ENGINE_REVISION
	#define HERTA_ENGINE_REVISION "unknown"
#endif

// Defines a plugin's entry point, which the editor finds by name and calls before anything else in the plugin.
#define HERTA_EDITOR_PLUGIN(RegisterFunction)                                                                                                                                  \
	HERTA_EDITOR_PLUGIN_EXPORT const ::Herta::FEditorPluginExports* HertaEditorPlugin()                                                                                        \
	{                                                                                                                                                                          \
		static const ::Herta::FEditorPluginExports Exports{::Herta::EditorPluginInterfaceVersion, HERTA_ENGINE_REVISION, HERTA_EDITOR_PLUGIN_CONFIGURATION, RegisterFunction}; \
		return &Exports;                                                                                                                                                       \
	}
