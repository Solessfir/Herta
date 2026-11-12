#pragma once

#include "Herta/EditorCore/EditorPlugin.h"
#include "Herta/EditorCore/Plugins.h"

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FEditorCommandRegistry;
class FLogService;
class FTaskScope;
class FTaskSystem;
class FToolUIContext;

struct FEditorPluginsSettings
{
	std::filesystem::path EngineRoot{};
	// The open project's folder and the folder its plugins build into; empty without a project.
	std::filesystem::path ProjectRoot{};
	std::filesystem::path ProjectBinaries{};
	// Where engine plugins build, beside the editor.
	std::filesystem::path EngineBinaries{};
	std::filesystem::path OverridesPath{};
	std::filesystem::path StartupMarkerPath{};
	bool bSafeMode = false;
};

// The editor's plugins: their discovery and per-user enable choices, the libraries loaded at startup, and what those registered.
class FEditorPlugins final : public IEditorPluginRegistry, public IEditorPluginHost
{
public:
	FEditorPlugins(FTaskSystem* Tasks, FLogService& Log, FEditorCommandRegistry& Commands, FEditorPluginsSettings Settings);
	FEditorPlugins(const FEditorPlugins&) = delete;
	FEditorPlugins& operator=(const FEditorPlugins&) = delete;
	FEditorPlugins(FEditorPlugins&&) = delete;
	FEditorPlugins& operator=(FEditorPlugins&&) = delete;
	~FEditorPlugins() override;

	// Discovers plugins and loads the enabled ones, unless the editor runs in safe mode or the last start with plugins never finished.
	void Load();
	// Called once the editor has drawn frames with its plugins, so the next start knows they did not crash it.
	void MarkStartupFinished();
	// Runs finished processes' callbacks on the main thread.
	void Update();
	void DrawPanels(FToolUIContext& ToolUI);
	void DrawWindowMenuItems();
	void DrawToolsMenuItems();
	[[nodiscard]] bool HasToolsMenuItems() const noexcept;
	// What plugins show in the status bar this frame, in registration order, without the empty ones.
	[[nodiscard]] std::vector<std::string> GetStatusTexts() const;
	// The Plugins tab. TabTitle is its localized name.
	void DrawManager(FToolUIContext& ToolUI, bool& bOpen, bool bFocus, std::string_view TabTitle);

	[[nodiscard]] bool IsSafeMode() const noexcept;
	[[nodiscard]] std::vector<std::string> GetLoadedNames() const;

	IEditorPluginHost& GetHost() override;
	void AddPanel(std::string_view Title, std::unique_ptr<IEditorPanel> Panel) override;
	void AddMenuItem(std::string_view Label, std::function<void()> OnSelected) override;
	void AddStatusText(std::function<std::string()> GetText) override;
	void AddCommand(std::string_view Name, std::string_view Description, bool bMutates, FEditorPluginCommand Run) override;

	void Log(EEditorPluginLog Level, std::string_view Message) override;
	[[nodiscard]] std::string GetProjectDirectory() const override;
	[[nodiscard]] std::string GetEngineDirectory() const override;
	void RunProcess(FEditorPluginProcess Process, std::function<void(const FEditorPluginProcessResult&)> OnFinished) override;

private:
	struct FPanel
	{
		std::string Title;
		std::unique_ptr<IEditorPanel> Panel;
		bool bOpen = false;
	};

	struct FMenuItem
	{
		std::string Label;
		std::function<void()> OnSelected;
	};

	struct FFinished
	{
		std::function<void(const FEditorPluginProcessResult&)> OnFinished;
		FEditorPluginProcessResult Result;
	};

	FTaskSystem* Tasks = nullptr;
	FLogService& LogService;
	FEditorCommandRegistry& Commands;
	FEditorPluginsSettings Settings;

	FPluginCatalog Catalog{};
	FPluginOverrides Overrides{};
	// The Plugins tab's section and search text.
	int ManagerSection = 0;
	std::array<char, 96> ManagerSearch{};
	// The choices the editor started with, so the manager can tell which changes wait for a restart.
	FPluginOverrides StartupOverrides{};
	std::vector<FPluginDiagnostic> Diagnostics{};
	std::vector<std::string> Loaded{};
	// The plugin whose Register is running, which names what it adds.
	std::string Registering{};
	bool bSafeMode = false;
	bool bStartupPending = false;

	std::mutex FinishedMutex{};
	std::vector<FFinished> Finished{};
	// The destructor cancels and waits for these processes before anything their callbacks reach goes away.
	std::unique_ptr<FTaskScope> ProcessScope{};

	std::vector<FMenuItem> MenuItems{};
	std::vector<std::function<std::string()>> StatusTexts{};
	std::vector<FPanel> Panels{};
};
}
