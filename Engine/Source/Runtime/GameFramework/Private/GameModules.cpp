#include "Herta/GameFramework/GameModules.h"

#include "Herta/Core/Build.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Platform/DynamicLibrary.h"

#include <algorithm>
#include <format>
#include <map>

namespace Herta
{
namespace
{
using FSystemFactory = std::unique_ptr<IGameSystem> (*)();

struct FLoadedModule
{
	std::string Name{};
	std::vector<std::span<const FGeneratedGameplayGraph>> Graphs{};
	std::vector<std::pair<std::string, FSystemFactory>> Systems{};
	// Declared last, so it closes after the tables and factories that point into it are gone.
	FDynamicLibrary Library{};
};

class FRegistry final : public IGameModuleRegistry
{
public:
	explicit FRegistry(FLoadedModule& InModule)
	    : Module(InModule)
	{
	}

	void AddGameplayGraphs(const std::span<const FGeneratedGameplayGraph> Graphs) override
	{
		Module.Graphs.push_back(Graphs);
	}

	void AddSystem(const std::string_view Name, const FSystemFactory Create) override
	{
		if (Create && !std::ranges::contains(Module.Systems, Name, &std::pair<std::string, FSystemFactory>::first))
		{
			Module.Systems.emplace_back(std::string(Name), Create);
		}
	}

private:
	FLoadedModule& Module;
};

void ReleaseGraphs(const FLoadedModule& Module)
{
	for (const std::span<const FGeneratedGameplayGraph> Graphs : Module.Graphs)
	{
		UnregisterGeneratedGameplayGraphs(Graphs);
	}
}

[[nodiscard]] std::expected<std::unique_ptr<FLoadedModule>, std::string> LoadModule(const std::string& Name, const FGameModuleSettings& Settings, const std::uint32_t Generation)
{
	const std::filesystem::path Built = Settings.Directory / GetDynamicLibraryFileName(Name);
	std::error_code Error;
	if (!std::filesystem::is_regular_file(Built, Error))
	{
		return std::unexpected(std::format("Game module {} is not built: {} is missing", Name, Built.string()));
	}

	std::filesystem::path Path = Built;
	if (!Settings.ShadowDirectory.empty())
	{
		// Each load gets its own copy, since the previous one stays loaded until the swap and the build overwrites the original.
		std::filesystem::create_directories(Settings.ShadowDirectory, Error);
		Path = Settings.ShadowDirectory / std::format("{}-{}{}", Built.stem().string(), Generation, Built.extension().string());
		std::filesystem::copy_file(Built, Path, std::filesystem::copy_options::overwrite_existing, Error);
		if (Error)
		{
			return std::unexpected(std::format("Cannot copy game module {} to {}: {}", Name, Path.string(), Error.message()));
		}
	}

	auto Module = std::make_unique<FLoadedModule>(FLoadedModule{.Name = Name});
	auto Library = FDynamicLibrary::Open(Path);
	if (!Library)
	{
		return std::unexpected(Library.error());
	}

	Module->Library = std::move(*Library);
	const auto Entry = reinterpret_cast<FGameModuleEntry>(Module->Library.FindSymbol(GameModuleEntryName));
	const FGameModuleExports* const Exports = Entry ? Entry() : nullptr;
	if (!Exports || !Exports->Register)
	{
		return std::unexpected(std::format("Game module {} has no {} entry; define one with HERTA_GAME_MODULE", Name, GameModuleEntryName));
	}

	// A module shares the host's types, heap, and inline engine code, so it must come from the same engine build and configuration.
	const std::string_view Configuration = GetBuildConfigurationName(GetBuildConfiguration());
	if (Exports->InterfaceVersion != GameModuleInterfaceVersion || std::string_view(Exports->EngineRevision) != GetBuildRevision() || std::string_view(Exports->Configuration) != Configuration)
	{
		return std::unexpected(std::format("Game module {} was built for engine {} {} with interface {}, but this is engine {} {} with interface {}; rebuild it", Name, Exports->EngineRevision, Exports->Configuration, Exports->InterfaceVersion, GetBuildRevision(), Configuration, GameModuleInterfaceVersion));
	}

	FRegistry Registry(*Module);
	Exports->Register(Registry);
	for (const std::span<const FGeneratedGameplayGraph> Graphs : Module->Graphs)
	{
		RegisterGeneratedGameplayGraphs(Graphs);
	}

	return Module;
}
}

struct FGameModules::FImplementation
{
	FGameModuleSettings Settings{};
	std::vector<std::unique_ptr<FLoadedModule>> Modules{};
	std::uint32_t Generation = 0;
};

FGameModules::FGameModules(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameModules::~FGameModules()
{
	for (const std::unique_ptr<FLoadedModule>& Module : Implementation->Modules)
	{
		ReleaseGraphs(*Module);
	}
}

std::expected<std::unique_ptr<FGameModules>, std::string> FGameModules::Load(const std::span<const std::string> Names, const FGameModuleSettings& Settings)
{
	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Settings = Settings;
	for (const std::string& Name : Names)
	{
		auto Module = LoadModule(Name, Settings, ++Implementation->Generation);
		if (!Module)
		{
			for (const std::unique_ptr<FLoadedModule>& Loaded : Implementation->Modules)
			{
				ReleaseGraphs(*Loaded);
			}

			return std::unexpected(Module.error());
		}

		Implementation->Modules.push_back(std::move(*Module));
	}

	return std::unique_ptr<FGameModules>(new FGameModules(std::move(Implementation)));
}

void FGameModules::Start(FGameSession& Session) const
{
	for (const std::unique_ptr<FLoadedModule>& Module : Implementation->Modules)
	{
		for (const auto& [Name, Create] : Module->Systems)
		{
			Session.AddGameSystem(Name, Create());
		}
	}
}

FGameModuleReloadResult FGameModules::Reload(const std::string_view Name, FGameSession* const Session)
{
	FImplementation& State = *Implementation;
	const auto Old = std::ranges::find(State.Modules, Name, [](const std::unique_ptr<FLoadedModule>& Module)
	{
		return std::string_view(Module->Name);
	});

	if (Old == State.Modules.end())
	{
		return {.Result = EGameModuleReload::Failed, .Message = std::format("No game module named {} is loaded", Name)};
	}

	auto New = LoadModule(std::string(Name), State.Settings, ++State.Generation);
	if (!New)
	{
		return {.Result = EGameModuleReload::Failed, .Message = New.error() + "; the previous build keeps running"};
	}

	std::size_t Kept = 0;
	if (Session)
	{
		// Every system leaves the session; this module's move to the new build through their state, and the rest go back as they were.
		std::vector<std::pair<std::string, std::unique_ptr<IGameSystem>>> Running = Session->TakeGameSystems();
		std::vector<std::pair<std::string, std::unique_ptr<IGameSystem>>> Replacements;
		std::string Refused;
		for (auto& [SystemName, System] : Running)
		{
			const bool bOurs = std::ranges::contains((*Old)->Systems, SystemName, &std::pair<std::string, FSystemFactory>::first);
			const auto Factory = std::ranges::find((*New)->Systems, SystemName, &std::pair<std::string, FSystemFactory>::first);
			if (!bOurs || Factory == (*New)->Systems.end())
			{
				continue;
			}

			std::unique_ptr<IGameSystem> Replacement = Factory->second();
			if (!Replacement->LoadState(System->GetStateVersion(), System->SaveState()))
			{
				Refused = SystemName;
				break;
			}

			Replacements.emplace_back(SystemName, std::move(Replacement));
		}

		if (!Refused.empty())
		{
			// Back as it was: the new build's objects go before its code does.
			Replacements.clear();
			for (auto& [SystemName, System] : Running)
			{
				Session->AddGameSystem(SystemName, std::move(System));
			}

			ReleaseGraphs(**New);
			return {.Result = EGameModuleReload::RestartRequired, .Message = std::format("System {} of {} cannot take the running state of its previous build; the previous build keeps running until a restart", Refused, Name)};
		}

		for (auto& [SystemName, System] : Running)
		{
			const auto Replacement = std::ranges::find(Replacements, SystemName, &std::pair<std::string, std::unique_ptr<IGameSystem>>::first);
			if (Replacement != Replacements.end())
			{
				System = std::move(Replacement->second);
				++Kept;
			}
			else if (std::ranges::contains((*Old)->Systems, SystemName, &std::pair<std::string, FSystemFactory>::first))
			{
				// A system the new build no longer has ends with the old build.
				System.reset();
			}

			if (System)
			{
				Session->AddGameSystem(SystemName, std::move(System));
			}
		}

		// Systems the new build adds start fresh.
		for (const auto& [SystemName, Create] : (*New)->Systems)
		{
			if (!std::ranges::contains(Running, SystemName, &std::pair<std::string, std::unique_ptr<IGameSystem>>::first))
			{
				Session->AddGameSystem(SystemName, Create());
			}
		}
	}

	ReleaseGraphs(**Old);
	*Old = std::move(*New);
	return {.Result = EGameModuleReload::Reloaded, .Message = std::format("Reloaded {}; {} systems kept their state", Name, Kept)};
}

std::vector<std::string> FGameModules::GetModuleNames() const
{
	std::vector<std::string> Names;
	for (const std::unique_ptr<FLoadedModule>& Module : Implementation->Modules)
	{
		Names.push_back(Module->Name);
	}

	return Names;
}

std::vector<std::string> FGameModules::GetSystemNames() const
{
	std::vector<std::string> Names;
	for (const std::unique_ptr<FLoadedModule>& Module : Implementation->Modules)
	{
		for (const auto& [Name, Create] : Module->Systems)
		{
			Names.push_back(Name);
		}
	}

	return Names;
}

std::filesystem::path FGameModules::GetBuiltPath(const std::string_view Name) const
{
	return Implementation->Settings.Directory / GetDynamicLibraryFileName(Name);
}
}
