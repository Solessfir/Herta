#include "EditorFrameworkPrivate.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"

namespace Herta
{
void FEditorFramework::FImplementation::LoadGameModules(const FLoadedProject& Project)
{
	// The old modules' systems must go first, so a running Play stops before its code unloads.
	if (PlaySession)
	{
		TogglePlay();
	}

	GameModules.reset();
	GameModuleBuilds.clear();
	std::vector<std::string> Names;
	Names.reserve(Project.Descriptor.Modules.size());
	for (const FProjectModule& Module : Project.Descriptor.Modules)
	{
		Names.push_back(Module.Name);
	}

	if (Names.empty())
	{
		return;
	}

	// The project builds its modules beside the engine's configuration, such as Binaries/windows/x86_64/Development-ClangCL.
	const std::filesystem::path Configuration = GetExecutablePath().parent_path().filename();
	const FGameModuleSettings Settings{.Directory = Project.Root / "Binaries" / GetPlatformName(GetCurrentPlatform()) / "x86_64" / Configuration, .ShadowDirectory = Project.Root / "Intermediate" / "HotReload"};
	auto Loaded = FGameModules::Load(Names, Settings);
	if (!Loaded)
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Play runs without the project's game modules: {}", Loaded.error());
		return;
	}

	GameModules = std::move(*Loaded);
	std::error_code Error;
	for (const std::string& Name : Names)
	{
		GameModuleBuilds[Name] = std::filesystem::last_write_time(GameModules->GetBuiltPath(Name), Error);
	}

	const auto Join = [](const std::vector<std::string>& Items)
	{
		std::string Joined;
		for (const std::string& Item : Items)
		{
			Joined += (Joined.empty() ? "" : ", ") + Item;
		}

		return Joined.empty() ? std::string("none") : Joined;
	};

	HERTA_LOG_INFO(*Log, EditorLog, "Loaded game modules {} with systems {}", Join(Names), Join(GameModules->GetSystemNames()));
}

FGameModuleReloadResult FEditorFramework::FImplementation::ReloadGameModule(const std::string_view Name)
{
	if (!GameModules)
	{
		return {.Result = EGameModuleReload::Failed, .Message = "The project has no loaded game modules"};
	}

	const FGameModuleReloadResult Result = GameModules->Reload(Name, PlaySession.get());
	std::error_code Error;
	GameModuleBuilds[std::string(Name)] = std::filesystem::last_write_time(GameModules->GetBuiltPath(Name), Error);
	if (Result.Result == EGameModuleReload::Reloaded)
	{
		HERTA_LOG_INFO(*Log, EditorLog, "{}", Result.Message);
	}
	else
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "{}", Result.Message);
	}

	return Result;
}

void FEditorFramework::FImplementation::LoadPlugins(const FLoadedProject* const Project, const bool bSafeMode)
{
	const std::filesystem::path Configuration = GetExecutablePath().parent_path().filename();
	const std::filesystem::path Settings = GetUserSettingsDirectory() / "Herta" / "Editor";
	PluginProject = Project ? Project->DescriptorPath : std::filesystem::path{};
	Plugins = std::make_unique<FEditorPlugins>(Tasks, *Log, *EditorCommands, FEditorPluginsSettings{
	                                                                             .EngineRoot = EngineRoot,
	                                                                             .ProjectRoot = Project ? Project->Root : std::filesystem::path{},
	                                                                             .ProjectBinaries = Project ? Project->Root / "Binaries" / GetPlatformName(GetCurrentPlatform()) / "x86_64" / Configuration : std::filesystem::path{},
	                                                                             .EngineBinaries = GetExecutablePath().parent_path(),
	                                                                             .OverridesPath = Settings / "Plugins.txt",
	                                                                             .StartupMarkerPath = Settings / "PluginStartup.marker",
	                                                                             .bSafeMode = bSafeMode,
	                                                                         });

	Plugins->Load();
	PluginsLoaded = std::chrono::steady_clock::now();
}

void FEditorFramework::FImplementation::PollGameModules()
{
	const auto Now = std::chrono::steady_clock::now();
	if (!GameModules || Now < NextModulePoll)
	{
		return;
	}

	NextModulePoll = Now + std::chrono::seconds(1);
	for (const std::string& Name : GameModules->GetModuleNames())
	{
		std::error_code Error;
		const auto Built = std::filesystem::last_write_time(GameModules->GetBuiltPath(Name), Error);
		// A linker writes the module over a moment, so a reload waits for a build that has stood still for a whole poll.
		if (Error || Built == GameModuleBuilds[Name] || std::filesystem::file_time_type::clock::now() - Built < std::chrono::seconds(1))
		{
			continue;
		}

		(void)ReloadGameModule(Name);
	}
}
}
