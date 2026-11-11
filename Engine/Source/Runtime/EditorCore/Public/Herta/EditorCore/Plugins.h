#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class IEditorPluginRegistry;

enum class EPluginSource : std::uint8_t
{
	Engine,
	Project
};

// A plugin's <Name>.hplugin descriptor, found one folder deep under Engine/Plugins or a project's Plugins.
struct FPluginDescriptor
{
	std::string Name{};
	std::string FriendlyName{};
	std::string Description{};
	// The shared library the plugin builds from its Source folder, without platform prefix or extension; empty for a plugin without code.
	std::string Module{};
	std::vector<std::string> Dependencies{};
	bool bEnabledByDefault = false;
	EPluginSource Source = EPluginSource::Engine;
	std::filesystem::path Directory{};
};

struct FPluginDiagnostic
{
	std::string Plugin{};
	std::string Message{};
};

struct FPluginRoot
{
	std::filesystem::path Directory{};
	EPluginSource Source = EPluginSource::Engine;
};

struct FPluginCatalog
{
	// Sorted by name. A project plugin with an engine plugin's name is refused rather than replacing it.
	std::vector<FPluginDescriptor> Plugins{};
	std::vector<FPluginDiagnostic> Diagnostics{};
};

[[nodiscard]] FPluginCatalog DiscoverPlugins(std::span<const FPluginRoot> Roots);
[[nodiscard]] std::expected<FPluginDescriptor, std::string> ParsePluginDescriptor(std::string_view Text, const std::filesystem::path& File);

// Per-user choices that differ from each plugin's default, which take effect when the editor next starts.
using FPluginOverrides = std::map<std::string, bool, std::less<>>;

// A missing file holds no overrides.
[[nodiscard]] std::expected<FPluginOverrides, std::string> LoadPluginOverrides(const std::filesystem::path& Path);
[[nodiscard]] std::expected<void, std::string> SavePluginOverrides(const std::filesystem::path& Path, const FPluginOverrides& Overrides);
[[nodiscard]] bool IsPluginEnabled(const FPluginDescriptor& Plugin, const FPluginOverrides& Overrides);

struct FPluginResolution
{
	// The enabled plugins whose dependencies are all enabled too, dependencies first.
	std::vector<const FPluginDescriptor*> Load{};
	std::vector<FPluginDiagnostic> Diagnostics{};
};

// Plugins with a missing, disabled, or cyclic dependency stay unloaded, with a diagnostic naming why.
[[nodiscard]] FPluginResolution ResolvePlugins(const FPluginCatalog& Catalog, const FPluginOverrides& Overrides);

// Loads a plugin library and runs its Register after checking its interface version, engine revision, and configuration. Loaded plugins
// stay loaded until the process exits, because what they registered may be destroyed as late as the editor's last shutdown step.
[[nodiscard]] std::expected<void, std::string> LoadEditorPlugin(const std::filesystem::path& Library, IEditorPluginRegistry& Registry);
}
