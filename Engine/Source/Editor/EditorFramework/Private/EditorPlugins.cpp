#include "EditorPlugins.h"

#include "Herta/Core/Log.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/Platform/DynamicLibrary.h"
#include "Herta/Platform/Process.h"
#include "Herta/Tasks/TaskSystem.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>

namespace Herta
{
namespace
{
inline constexpr FLogCategory PluginLog{.Name = "Plugins"};

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

// The editor's widgets behind the plugin interface. Labels pass through as given, so plugins pick their own IDs with ## as ImGui does.
class FPluginUI final : public IEditorPluginUI
{
public:
	explicit FPluginUI(FToolUIContext& InToolUI)
	    : ToolUI(InToolUI)
	{
	}

	void Text(const std::string_view Text) override
	{
		ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
	}

	void TextDisabled(const std::string_view Text) override
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
		ImGui::PopStyleColor();
	}

	void TextWrapped(const std::string_view Text) override
	{
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
		ImGui::PopTextWrapPos();
	}

	void MonospaceLines(const std::size_t Count, const std::function<std::string_view(std::size_t Line)>& GetLine) override
	{
		ToolUI.PushLogFont();
		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int>(std::min<std::size_t>(Count, std::numeric_limits<int>::max())));
		while (Clipper.Step())
		{
			for (int Line = Clipper.DisplayStart; Line < Clipper.DisplayEnd; ++Line)
			{
				const std::string_view Text = GetLine(static_cast<std::size_t>(Line));
				ImGui::TextUnformatted(Text.data(), Text.data() + Text.size());
			}
		}

		ToolUI.PopLogFont();
	}

	bool Button(const std::string_view Label) override
	{
		return ImGui::Button(std::string(Label).c_str());
	}

	bool Checkbox(const std::string_view Label, bool& bValue) override
	{
		return ImGui::Checkbox(std::string(Label).c_str(), &bValue);
	}

	bool Selectable(const std::string_view Label, const bool bSelected) override
	{
		return ImGui::Selectable(std::string(Label).c_str(), bSelected);
	}

	bool InputText(const std::string_view Id, std::string& Value, const std::string_view Hint) override
	{
		const std::string HintText(Hint);
		ToolUIInputText(std::string(Id).c_str(), Value, HintText.empty() ? nullptr : HintText.c_str());
		// Enter may also end the edit, so the field counts while active or in the frame it lets go.
		return (ImGui::IsItemActive() || ImGui::IsItemDeactivated()) && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
	}

	void InputTextMultiline(const std::string_view Id, std::string& Value, const float Lines) override
	{
		// ImGui edits a fixed buffer, so the text gets room to grow each frame and is trimmed back after.
		std::vector<char> Buffer(Value.size() + 4096, '\0');
		std::ranges::copy(Value, Buffer.begin());
		if (ImGui::InputTextMultiline(std::string(Id).c_str(), Buffer.data(), Buffer.size(), {-1.f, ImGui::GetTextLineHeightWithSpacing() * std::max(Lines, 1.f)}))
		{
			Value.assign(Buffer.data());
		}
	}

	void FocusNext() override
	{
		ImGui::SetKeyboardFocusHere();
	}

	void SameLine() override
	{
		ImGui::SameLine();
	}

	void Separator() override
	{
		ImGui::Separator();
	}

	void BeginScrollRegion(const std::string_view Id, const float Lines, const bool bFollowEnd) override
	{
		const float LineHeight = ImGui::GetTextLineHeightWithSpacing();
		const float Height = Lines > 0.f ? Lines * LineHeight : std::min(0.f, Lines * LineHeight - ImGui::GetStyle().ItemSpacing.y);
		ImGui::BeginChild(std::string(Id).c_str(), {0.f, Height}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		FollowEnd.push_back(bFollowEnd && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f);
	}

	void EndScrollRegion() override
	{
		if (!FollowEnd.empty())
		{
			if (FollowEnd.back())
			{
				ImGui::SetScrollHereY(1.f);
			}

			FollowEnd.pop_back();
		}

		ImGui::EndChild();
	}

	void BeginDisabled(const bool bDisabled) override
	{
		ImGui::BeginDisabled(bDisabled);
	}

	void EndDisabled() override
	{
		ImGui::EndDisabled();
	}

	void PushId(const std::string_view Id) override
	{
		ImGui::PushID(Id.data(), Id.data() + Id.size());
	}

	void PopId() override
	{
		ImGui::PopID();
	}

private:
	FToolUIContext& ToolUI;
	std::vector<bool> FollowEnd;
};
}

FEditorPlugins::FEditorPlugins(FTaskSystem* const InTasks, FLogService& InLog, FEditorCommandRegistry& InCommands, FEditorPluginsSettings InSettings)
    : Tasks(InTasks)
    , LogService(InLog)
    , Commands(InCommands)
    , Settings(std::move(InSettings))
{
	if (Tasks)
	{
		if (auto Scope = Tasks->CreateScope("Plugin processes"))
		{
			ProcessScope = std::move(*Scope);
		}
	}
}

FEditorPlugins::~FEditorPlugins()
{
	// Every process stops before anything its callback could reach goes away; finished callbacks never run.
	if (ProcessScope)
	{
		ProcessScope->RequestCancellation();
		ProcessScope->Wait();
	}

	Finished.clear();
	// Closing normally proves the plugins did not crash the editor, however short the run.
	MarkStartupFinished();
}

void FEditorPlugins::Load()
{
	const std::array Roots = {
	    FPluginRoot{.Directory = Settings.EngineRoot.empty() ? std::filesystem::path{} : Settings.EngineRoot / "Engine" / "Plugins", .Source = EPluginSource::Engine},
	    FPluginRoot{.Directory = Settings.ProjectRoot.empty() ? std::filesystem::path{} : Settings.ProjectRoot / "Plugins", .Source = EPluginSource::Project},
	};

	Catalog = DiscoverPlugins(Roots);
	Diagnostics = Catalog.Diagnostics;
	if (auto Read = LoadPluginOverrides(Settings.OverridesPath))
	{
		Overrides = std::move(*Read);
	}
	else
	{
		Diagnostics.push_back({.Plugin = {}, .Message = Read.error()});
	}

	StartupOverrides = Overrides;
	std::error_code Error;
	bSafeMode = Settings.bSafeMode;
	if (!bSafeMode && std::filesystem::exists(Settings.StartupMarkerPath, Error))
	{
		bSafeMode = true;
		LogService.LogText(PluginLog, ELogLevel::Warning, "The editor did not finish starting with its plugins last time, so this start skips them. Disable the plugin that failed in Window > Plugins, then restart");
	}

	for (const FPluginDiagnostic& Diagnostic : Diagnostics)
	{
		LogService.LogText(PluginLog, ELogLevel::Warning, Diagnostic.Message);
	}

	if (bSafeMode)
	{
		std::filesystem::remove(Settings.StartupMarkerPath, Error);
		LogService.LogText(PluginLog, ELogLevel::Info, "Safe mode: no plugins are loaded");
		return;
	}

	FPluginResolution Resolution = ResolvePlugins(Catalog, Overrides);
	for (FPluginDiagnostic& Diagnostic : Resolution.Diagnostics)
	{
		LogService.LogText(PluginLog, ELogLevel::Warning, Diagnostic.Message);
		Diagnostics.push_back(std::move(Diagnostic));
	}

	if (std::ranges::none_of(Resolution.Load, [](const FPluginDescriptor* const Plugin)
	{
		return !Plugin->Module.empty();
	}))
	{
		for (const FPluginDescriptor* const Plugin : Resolution.Load)
		{
			Loaded.push_back(Plugin->Name);
		}

		return;
	}

	// The marker stays until the editor has drawn with its plugins, so a crash while loading or in a first frame starts the next run in safe mode.
	std::filesystem::create_directories(Settings.StartupMarkerPath.parent_path(), Error);
	std::ofstream(Settings.StartupMarkerPath, std::ios::binary | std::ios::trunc) << "loading plugins\n";
	bStartupPending = true;
	for (const FPluginDescriptor* const Plugin : Resolution.Load)
	{
		if (!Plugin->Module.empty())
		{
			const std::filesystem::path& Directory = Plugin->Source == EPluginSource::Engine ? Settings.EngineBinaries : Settings.ProjectBinaries;
			Registering = Plugin->Name;
			if (auto Result = LoadEditorPlugin(Directory / GetDynamicLibraryFileName(Plugin->Module), *this); !Result)
			{
				Registering.clear();
				const std::string Message = std::format("{} is not loaded: {}", Plugin->Name, Result.error());
				LogService.LogText(PluginLog, ELogLevel::Warning, Message);
				Diagnostics.push_back({.Plugin = Plugin->Name, .Message = Message});
				continue;
			}

			Registering.clear();
		}

		Loaded.push_back(Plugin->Name);
	}

	if (!Loaded.empty())
	{
		std::string Names;
		for (const std::string& Name : Loaded)
		{
			Names += (Names.empty() ? "" : ", ") + Name;
		}

		LogService.LogText(PluginLog, ELogLevel::Info, std::format("Loaded plugins {}", Names));
	}
}

void FEditorPlugins::MarkStartupFinished()
{
	if (std::exchange(bStartupPending, false))
	{
		std::error_code Error;
		std::filesystem::remove(Settings.StartupMarkerPath, Error);
	}
}

void FEditorPlugins::Update()
{
	std::vector<FFinished> Ready;
	{
		const std::scoped_lock Lock(FinishedMutex);
		Ready.swap(Finished);
	}

	for (FFinished& Process : Ready)
	{
		Process.OnFinished(Process.Result);
	}
}

void FEditorPlugins::DrawPanels(FToolUIContext& ToolUI)
{
	FPluginUI UI(ToolUI);
	for (FPanel& Panel : Panels)
	{
		if (!Panel.bOpen)
		{
			continue;
		}

		// Plugin panels start floating, so they need a size their contents fit until the user docks or resizes them.
		const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
		ImGui::SetNextWindowSize({560.f * Scale, 480.f * Scale}, ImGuiCond_FirstUseEver);
		if (ToolUI.BeginPanel(Panel.Title, &Panel.bOpen))
		{
			ImGui::PushID(Panel.Title.c_str());
			Panel.Panel->Draw(UI);
			ImGui::PopID();
		}

		ToolUI.EndPanel();
	}
}

void FEditorPlugins::DrawWindowMenuItems()
{
	for (FPanel& Panel : Panels)
	{
		ToolUIMenuItem(Panel.Title, EToolUIMenuIcon::Panel, &Panel.bOpen);
	}
}

void FEditorPlugins::DrawToolsMenuItems()
{
	for (FMenuItem& Item : MenuItems)
	{
		if (ToolUIMenuItem(Item.Label, EToolUIMenuIcon::Entity))
		{
			Item.OnSelected();
		}
	}
}

bool FEditorPlugins::HasToolsMenuItems() const noexcept
{
	return !MenuItems.empty();
}

void FEditorPlugins::DrawManager(FToolUIContext& ToolUI, bool& bOpen)
{
	if (!bOpen)
	{
		return;
	}

	ImGui::SetNextWindowSize({560.f * ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize, 420.f * ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize}, ImGuiCond_FirstUseEver);
	if (!ToolUI.BeginPanel("Plugins", &bOpen))
	{
		ToolUI.EndPanel();
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	if (bSafeMode)
	{
		ImGui::TextColored({0.95f, 0.75f, 0.35f, 1.f}, "Safe mode: plugins are not loaded this run.");
	}

	const bool bRestart = std::ranges::any_of(Catalog.Plugins, [this](const FPluginDescriptor& Plugin)
	{
		return IsPluginEnabled(Plugin, Overrides) != IsPluginEnabled(Plugin, StartupOverrides);
	});

	if (bRestart)
	{
		ImGui::TextColored({0.95f, 0.75f, 0.35f, 1.f}, "Restart the editor to apply plugin changes.");
	}

	if (Catalog.Plugins.empty())
	{
		ImGui::TextDisabled("No plugins in Engine/Plugins or the project's Plugins folder.");
	}

	for (const FPluginDescriptor& Plugin : Catalog.Plugins)
	{
		ImGui::PushID(Plugin.Name.c_str());
		bool bEnabled = IsPluginEnabled(Plugin, Overrides);
		// The toggle right-aligns within the item width, so a width of just its track keeps it at the left of the row.
		ImGui::SetNextItemWidth(std::round(ImGui::GetFontSize() * 0.9f * 1.8f));
		if (ToolUIToggle("##Enabled", &bEnabled))
		{
			// Choosing the default again drops the override, so a later default change reaches this user too.
			if (bEnabled == Plugin.bEnabledByDefault)
			{
				Overrides.erase(Plugin.Name);
			}
			else
			{
				Overrides.insert_or_assign(Plugin.Name, bEnabled);
			}

			if (auto Saved = SavePluginOverrides(Settings.OverridesPath, Overrides); !Saved)
			{
				LogService.LogText(PluginLog, ELogLevel::Warning, Saved.error());
			}
		}

		ImGui::SameLine(0.f, 10.f * Scale);
		ImGui::BeginGroup();
		ImGui::TextUnformatted(Plugin.FriendlyName.c_str());
		ImGui::SameLine();
		const bool bLoaded = std::ranges::contains(Loaded, Plugin.Name);
		ImGui::TextDisabled("%s%s", Plugin.Source == EPluginSource::Engine ? "Engine" : "Project", bLoaded ? ", loaded" : "");
		if (!Plugin.Description.empty())
		{
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextDisabled("%s", Plugin.Description.c_str());
			ImGui::PopTextWrapPos();
		}

		if (!Plugin.Dependencies.empty())
		{
			std::string Names;
			for (const std::string& Dependency : Plugin.Dependencies)
			{
				Names += (Names.empty() ? "" : ", ") + Dependency;
			}

			ImGui::TextDisabled("Needs %s", Names.c_str());
		}

		for (const FPluginDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Plugin == Plugin.Name)
			{
				ImGui::PushTextWrapPos(0.f);
				ImGui::TextColored({0.95f, 0.55f, 0.45f, 1.f}, "%s", Diagnostic.Message.c_str());
				ImGui::PopTextWrapPos();
			}
		}

		ImGui::EndGroup();
		ImGui::Spacing();
		ImGui::PopID();
	}

	for (const FPluginDiagnostic& Diagnostic : Diagnostics)
	{
		if (!std::ranges::contains(Catalog.Plugins, Diagnostic.Plugin, &FPluginDescriptor::Name))
		{
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextColored({0.95f, 0.55f, 0.45f, 1.f}, "%s", Diagnostic.Message.c_str());
			ImGui::PopTextWrapPos();
		}
	}

	ToolUI.EndPanel();
}

bool FEditorPlugins::IsSafeMode() const noexcept
{
	return bSafeMode;
}

std::vector<std::string> FEditorPlugins::GetLoadedNames() const
{
	return Loaded;
}

IEditorPluginHost& FEditorPlugins::GetHost()
{
	return *this;
}

void FEditorPlugins::AddPanel(const std::string_view Title, std::unique_ptr<IEditorPanel> Panel)
{
	if (Panel && !Title.empty() && !std::ranges::contains(Panels, Title, &FPanel::Title))
	{
		Panels.push_back({.Title = std::string(Title), .Panel = std::move(Panel), .bOpen = false});
	}
}

void FEditorPlugins::AddMenuItem(const std::string_view Label, std::function<void()> OnSelected)
{
	if (OnSelected && !Label.empty())
	{
		MenuItems.push_back({.Label = std::string(Label), .OnSelected = std::move(OnSelected)});
	}
}

void FEditorPlugins::AddCommand(const std::string_view Name, const std::string_view Description, const bool bMutates, FEditorPluginCommand Run)
{
	auto Registered = Commands.Register({.Name = std::string(Name),
	    .Description = std::string(Description),
	    .bMutates = bMutates,
	    .Handler = [Run = std::move(Run)](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		FEditorPluginCommandResult Result = Run(Arguments);
		if (!Result.bSucceeded)
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = std::move(Result.Message)});
		}

		return FEditorCommandResult{.ExitCode = 0, .Message = std::move(Result.Message)};
	}});

	if (!Registered)
	{
		LogService.LogText(PluginLog, ELogLevel::Warning, std::format("{} cannot add command {}: {}", Registering, Name, Registered.error().Message));
	}
}

void FEditorPlugins::Log(const EEditorPluginLog Level, const std::string_view Message)
{
	LogService.LogText(PluginLog, Level == EEditorPluginLog::Error ? ELogLevel::Error : Level == EEditorPluginLog::Warning ? ELogLevel::Warning
	                                                                                                                       : ELogLevel::Info,
	    Message);
}

std::string FEditorPlugins::GetProjectDirectory() const
{
	return ToUtf8(Settings.ProjectRoot);
}

std::string FEditorPlugins::GetEngineDirectory() const
{
	return ToUtf8(Settings.EngineRoot);
}

void FEditorPlugins::RunProcess(FEditorPluginProcess Process, std::function<void(const FEditorPluginProcessResult&)> OnFinished)
{
	const auto Finish = [this, &OnFinished](FEditorPluginProcessResult Result)
	{
		const std::scoped_lock Lock(FinishedMutex);
		Finished.push_back({.OnFinished = std::move(OnFinished), .Result = std::move(Result)});
	};

	if (!ProcessScope)
	{
		Finish({.bRan = false, .ExitCode = 0, .Output = {}, .Error = "The editor runs without background tasks"});
		return;
	}

	auto Task = Tasks->Submit(*ProcessScope, {.Name = "Plugin process", .Lane = ETaskLane::BlockingIo}, [this, Process = std::move(Process), OnFinished](FTaskContext& Context) mutable
	{
		FEditorPluginProcessResult Result;
		std::filesystem::path Executable = std::filesystem::path(std::u8string(Process.Executable.begin(), Process.Executable.end()));
		if (!Executable.has_parent_path())
		{
			if (auto Found = FindExecutableOnPath(Process.Executable))
			{
				Executable = std::move(*Found);
			}
		}

		const auto Ran = Herta::RunProcess({.Executable = Executable,
		    .Arguments = std::move(Process.Arguments),
		    .Timeout = std::chrono::milliseconds(Process.TimeoutMilliseconds),
		    .ShouldCancel = [&Context]
		{
			return Context.IsCancellationRequested();
		},
		    .Environment = std::move(Process.Environment)});

		if (Ran)
		{
			Result = {.bRan = true, .ExitCode = Ran->ExitCode, .Output = std::move(Ran->StandardOutput), .Error = std::move(Ran->StandardError)};
		}
		else
		{
			Result.Error = Ran.error().Message;
		}

		if (!Context.IsCancellationRequested())
		{
			const std::scoped_lock Lock(FinishedMutex);
			Finished.push_back({.OnFinished = std::move(OnFinished), .Result = std::move(Result)});
		}
	});

	if (!Task)
	{
		Finish({.bRan = false, .ExitCode = 0, .Output = {}, .Error = Task.error().Message});
	}
}
}
