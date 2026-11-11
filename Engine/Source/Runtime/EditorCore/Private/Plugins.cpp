#include "Herta/EditorCore/Plugins.h"

#include "Herta/Core/Build.h"
#include "Herta/EditorCore/EditorPlugin.h"
#include "Herta/Platform/DynamicLibrary.h"
#include "Herta/Platform/FileSystem.h"

#include <simdjson.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>

namespace Herta
{
namespace
{
constexpr std::string_view DescriptorExtension = ".hplugin";

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

[[nodiscard]] bool IsPluginName(const std::string_view Name)
{
	return !Name.empty() && Name.size() <= 64 && std::ranges::all_of(Name, [](const char Character)
	{
		return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') || Character == '_';
	});
}

[[nodiscard]] std::expected<std::string, std::string> ReadOptionalString(simdjson::dom::object Object, const std::string_view Key)
{
	simdjson::dom::element Element;
	if (Object[Key].get(Element))
	{
		return std::string{};
	}

	std::string_view Value;
	if (Element.get_string().get(Value))
	{
		return std::unexpected(std::format("'{}' must be a string", Key));
	}

	return std::string(Value);
}

// What a loaded plugin keeps open. Libraries are never closed before the process exits: panels, menu callbacks, and commands they created
// are destroyed by their owners at shutdown, and each destructor runs the plugin's code.
std::mutex& GetLoadedMutex()
{
	static std::mutex Mutex;
	return Mutex;
}

std::vector<FDynamicLibrary>& GetLoadedLibraries()
{
	static std::vector<FDynamicLibrary> Libraries;
	return Libraries;
}
}

std::expected<FPluginDescriptor, std::string> ParsePluginDescriptor(const std::string_view Text, const std::filesystem::path& File)
{
	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	simdjson::dom::object Object;
	if (Parser.parse(Text.data(), Text.size()).get(Root) || Root.get_object().get(Object))
	{
		return std::unexpected("The descriptor is not a JSON object");
	}

	std::string_view Format;
	std::uint64_t FormatVersion = 0;
	if (Object["format"].get_string().get(Format) || Format != "HertaPlugin" || Object["formatVersion"].get_uint64().get(FormatVersion) || FormatVersion != 1)
	{
		return std::unexpected("The descriptor must have format HertaPlugin and formatVersion 1");
	}

	FPluginDescriptor Plugin{.Directory = File.parent_path()};
	std::string_view Name;
	if (Object["name"].get_string().get(Name) || !IsPluginName(Name))
	{
		return std::unexpected("'name' must be 1 to 64 letters, digits, or underscores");
	}

	Plugin.Name = std::string(Name);
	if (File.stem().generic_u8string() != std::u8string(Plugin.Name.begin(), Plugin.Name.end()))
	{
		return std::unexpected(std::format("'name' {} must match the descriptor's file name", Plugin.Name));
	}

	for (const auto& [Key, Target] : {std::pair{"friendlyName", &Plugin.FriendlyName}, std::pair{"description", &Plugin.Description}, std::pair{"module", &Plugin.Module}})
	{
		auto Value = ReadOptionalString(Object, Key);
		if (!Value)
		{
			return std::unexpected(Value.error());
		}

		*Target = std::move(*Value);
	}

	if (Plugin.FriendlyName.empty())
	{
		Plugin.FriendlyName = Plugin.Name;
	}

	if (!Plugin.Module.empty() && !IsPluginName(Plugin.Module))
	{
		return std::unexpected("'module' must be 1 to 64 letters, digits, or underscores");
	}

	simdjson::dom::element Dependencies;
	if (!Object["dependencies"].get(Dependencies))
	{
		simdjson::dom::array Array;
		if (Dependencies.get_array().get(Array))
		{
			return std::unexpected("'dependencies' must be an array of plugin names");
		}

		for (const simdjson::dom::element Dependency : Array)
		{
			std::string_view DependencyName;
			if (Dependency.get_string().get(DependencyName) || !IsPluginName(DependencyName))
			{
				return std::unexpected("'dependencies' must be an array of plugin names");
			}

			Plugin.Dependencies.emplace_back(DependencyName);
		}
	}

	simdjson::dom::element Enabled;
	if (!Object["enabledByDefault"].get(Enabled) && Enabled.get_bool().get(Plugin.bEnabledByDefault))
	{
		return std::unexpected("'enabledByDefault' must be true or false");
	}

	return Plugin;
}

FPluginCatalog DiscoverPlugins(const std::span<const FPluginRoot> Roots)
{
	FPluginCatalog Catalog;
	for (const FPluginRoot& Root : Roots)
	{
		std::error_code Error;
		if (Root.Directory.empty() || !std::filesystem::is_directory(Root.Directory, Error))
		{
			continue;
		}

		std::vector<std::filesystem::path> Files;
		for (const auto& Folder : std::filesystem::directory_iterator(Root.Directory, Error))
		{
			const std::filesystem::path File = Folder.path() / (Folder.path().filename().string() + std::string(DescriptorExtension));
			if (Folder.is_directory(Error) && std::filesystem::is_regular_file(File, Error))
			{
				Files.push_back(File);
			}
		}

		std::ranges::sort(Files);
		for (const std::filesystem::path& File : Files)
		{
			std::ifstream Stream(File, std::ios::binary);
			const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
			auto Plugin = ParsePluginDescriptor(Text, File);
			if (!Plugin)
			{
				Catalog.Diagnostics.push_back({.Plugin = File.stem().string(), .Message = std::format("{}: {}", ToUtf8(File), Plugin.error())});
				continue;
			}

			if (std::ranges::contains(Catalog.Plugins, Plugin->Name, &FPluginDescriptor::Name))
			{
				Catalog.Diagnostics.push_back({.Plugin = Plugin->Name, .Message = std::format("{} is ignored: a plugin with this name is already in {}", ToUtf8(File), ToUtf8(std::ranges::find(Catalog.Plugins, Plugin->Name, &FPluginDescriptor::Name)->Directory))});
				continue;
			}

			Plugin->Source = Root.Source;
			Catalog.Plugins.push_back(std::move(*Plugin));
		}
	}

	std::ranges::sort(Catalog.Plugins, {}, &FPluginDescriptor::Name);
	return Catalog;
}

std::expected<FPluginOverrides, std::string> LoadPluginOverrides(const std::filesystem::path& Path)
{
	std::error_code Error;
	if (!std::filesystem::exists(Path, Error))
	{
		return FPluginOverrides{};
	}

	std::ifstream Stream(Path, std::ios::binary);
	if (!Stream)
	{
		return std::unexpected(std::format("Cannot read {}", ToUtf8(Path)));
	}

	FPluginOverrides Overrides;
	std::string Line;
	while (std::getline(Stream, Line))
	{
		if (!Line.empty() && Line.back() == '\r')
		{
			Line.pop_back();
		}

		const std::size_t Equals = Line.find('=');
		const std::string_view Name = std::string_view(Line).substr(0, Equals);
		const std::string_view Value = Equals == std::string::npos ? std::string_view{} : std::string_view(Line).substr(Equals + 1);
		if (Line.empty())
		{
			continue;
		}

		if (!IsPluginName(Name) || (Value != "true" && Value != "false"))
		{
			return std::unexpected(std::format("{}: '{}' is not Name=true or Name=false", ToUtf8(Path), Line));
		}

		Overrides.insert_or_assign(std::string(Name), Value == "true");
	}

	return Overrides;
}

std::expected<void, std::string> SavePluginOverrides(const std::filesystem::path& Path, const FPluginOverrides& Overrides)
{
	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	std::filesystem::path Temporary = Path;
	Temporary += ".tmp";
	{
		std::ofstream Stream(Temporary, std::ios::binary | std::ios::trunc);
		for (const auto& [Name, bEnabled] : Overrides)
		{
			Stream << Name << '=' << (bEnabled ? "true" : "false") << '\n';
		}

		if (!Stream.flush())
		{
			return std::unexpected(std::format("Cannot write {}", ToUtf8(Temporary)));
		}
	}

	if (const auto Moved = MoveAtomically(Temporary, Path, EMoveMode::ReplaceExisting); !Moved)
	{
		return std::unexpected(std::format("Cannot replace {}: {}", ToUtf8(Path), Moved.error().message()));
	}

	return {};
}

bool IsPluginEnabled(const FPluginDescriptor& Plugin, const FPluginOverrides& Overrides)
{
	const auto Override = Overrides.find(Plugin.Name);
	return Override != Overrides.end() ? Override->second : Plugin.bEnabledByDefault;
}

FPluginResolution ResolvePlugins(const FPluginCatalog& Catalog, const FPluginOverrides& Overrides)
{
	FPluginResolution Resolution;
	enum class EState : std::uint8_t
	{
		Unvisited,
		Visiting,
		Loaded,
		Refused
	};

	std::map<std::string_view, EState> States;
	const auto Find = [&Catalog](const std::string_view Name) -> const FPluginDescriptor*
	{
		const auto Plugin = std::ranges::find(Catalog.Plugins, Name, &FPluginDescriptor::Name);
		return Plugin == Catalog.Plugins.end() ? nullptr : &*Plugin;
	};

	// Depth first, so each plugin's dependencies load before it; a plugin seen again while still being visited is part of a cycle.
	std::function<bool(const FPluginDescriptor&)> Visit = [&](const FPluginDescriptor& Plugin) -> bool
	{
		EState& State = States[Plugin.Name];
		if (State == EState::Loaded || State == EState::Refused)
		{
			return State == EState::Loaded;
		}

		if (State == EState::Visiting)
		{
			Resolution.Diagnostics.push_back({.Plugin = Plugin.Name, .Message = std::format("{} is not loaded: its dependencies form a cycle", Plugin.Name)});
			return false;
		}

		State = EState::Visiting;
		for (const std::string& DependencyName : Plugin.Dependencies)
		{
			const FPluginDescriptor* const Dependency = Find(DependencyName);
			std::string Reason;
			if (!Dependency)
			{
				Reason = std::format("its dependency {} is not installed", DependencyName);
			}
			else if (!IsPluginEnabled(*Dependency, Overrides))
			{
				Reason = std::format("its dependency {} is disabled", DependencyName);
			}
			else if (!Visit(*Dependency))
			{
				Reason = std::format("its dependency {} is not loaded", DependencyName);
			}

			if (!Reason.empty())
			{
				States[Plugin.Name] = EState::Refused;
				Resolution.Diagnostics.push_back({.Plugin = Plugin.Name, .Message = std::format("{} is not loaded: {}", Plugin.Name, Reason)});
				return false;
			}
		}

		States[Plugin.Name] = EState::Loaded;
		Resolution.Load.push_back(&Plugin);
		return true;
	};

	for (const FPluginDescriptor& Plugin : Catalog.Plugins)
	{
		if (IsPluginEnabled(Plugin, Overrides))
		{
			Visit(Plugin);
		}
	}

	return Resolution;
}

std::expected<void, std::string> LoadEditorPlugin(const std::filesystem::path& Library, IEditorPluginRegistry& Registry)
{
	std::error_code Error;
	if (!std::filesystem::is_regular_file(Library, Error))
	{
		return std::unexpected(std::format("{} is not built", ToUtf8(Library)));
	}

	auto Opened = FDynamicLibrary::Open(Library);
	if (!Opened)
	{
		return std::unexpected(Opened.error());
	}

	const auto Entry = reinterpret_cast<FEditorPluginEntry>(Opened->FindSymbol(EditorPluginEntryName));
	const FEditorPluginExports* const Exports = Entry ? Entry() : nullptr;
	if (!Exports || !Exports->Register)
	{
		return std::unexpected(std::format("{} has no {} entry; define one with HERTA_EDITOR_PLUGIN", ToUtf8(Library), EditorPluginEntryName));
	}

	// A plugin shares the editor's types and heap, so it must come from the same engine build and configuration.
	const std::string_view Configuration = GetBuildConfigurationName(GetBuildConfiguration());
	if (Exports->InterfaceVersion != EditorPluginInterfaceVersion || std::string_view(Exports->EngineRevision) != GetBuildRevision() || std::string_view(Exports->Configuration) != Configuration)
	{
		return std::unexpected(std::format("{} was built for engine {} {} with interface {}, but this is engine {} {} with interface {}; rebuild it", ToUtf8(Library), Exports->EngineRevision, Exports->Configuration, Exports->InterfaceVersion, GetBuildRevision(), Configuration, EditorPluginInterfaceVersion));
	}

	{
		const std::scoped_lock Lock(GetLoadedMutex());
		GetLoadedLibraries().push_back(std::move(*Opened));
	}

	Exports->Register(Registry);
	return {};
}
}
