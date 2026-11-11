#pragma once

#include "Herta/GameFramework/GameModule.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FGameSession;

enum class EGameModuleReload : std::uint8_t
{
	// The new build runs, and every system that existed before kept its state.
	Reloaded,
	// The new build cannot take the running state, so the old build keeps running; restart Play, the game, or the editor to use it.
	RestartRequired,
	// The new build did not load or register, and the old build keeps running.
	Failed,
};

struct FGameModuleReloadResult
{
	EGameModuleReload Result = EGameModuleReload::Failed;
	std::string Message{};
};

struct FGameModuleSettings
{
	// Where the built modules are, such as the project's Binaries/<platform>/<architecture>/<configuration>, or beside a packaged game.
	std::filesystem::path Directory{};
	// Empty loads each module in place. Otherwise each load copies the module here first, so the build can replace the original while it runs,
	// which hot reload needs.
	std::filesystem::path ShadowDirectory{};
};

// A project's game modules, loaded in the editor, the game, and the server. Sessions that ran their systems must be destroyed, or have
// their systems taken, before this is, since the systems' code lives in the modules.
class FGameModules final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FGameModules>, std::string> Load(std::span<const std::string> Names, const FGameModuleSettings& Settings);

	FGameModules(const FGameModules&) = delete;
	FGameModules& operator=(const FGameModules&) = delete;
	FGameModules(FGameModules&&) = delete;
	FGameModules& operator=(FGameModules&&) = delete;
	~FGameModules();

	// Adds every module's systems to Session.
	void Start(FGameSession& Session) const;
	// Loads Name's current build and swaps it in. Call it between updates, when no system runs. Session, when there is one, keeps its other
	// modules' systems as they are and moves this module's to the new build through their saved state.
	[[nodiscard]] FGameModuleReloadResult Reload(std::string_view Name, FGameSession* Session);
	[[nodiscard]] std::vector<std::string> GetModuleNames() const;
	[[nodiscard]] std::vector<std::string> GetSystemNames() const;
	// Where Name's current build is, for watching it for a rebuild.
	[[nodiscard]] std::filesystem::path GetBuiltPath(std::string_view Name) const;

	struct FImplementation;

private:
	explicit FGameModules(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
