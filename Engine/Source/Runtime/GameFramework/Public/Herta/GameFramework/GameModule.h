#pragma once

#include "Herta/GameFramework/GameplayProgram.h"
#include "Herta/Math/Vector.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The interface between the engine and a project's game module, a shared library the editor, the game, and the server load. A module links
// no engine library: its own copies of engine globals, such as the physics factory, would never be initialized. It reaches the session only
// through these interfaces, whose virtual calls run the host's code, and uses engine headers only for value types and inline functions.
namespace Herta
{
// Bump when anything in this header changes layout or meaning; the host refuses modules built against another version.
inline constexpr std::uint32_t GameModuleInterfaceVersion = 1;

// What a game system may read and do in the session it runs in, implemented by the host.
class IGameSystemContext
{
public:
	IGameSystemContext() = default;
	IGameSystemContext(const IGameSystemContext&) = delete;
	IGameSystemContext& operator=(const IGameSystemContext&) = delete;
	IGameSystemContext(IGameSystemContext&&) = delete;
	IGameSystemContext& operator=(IGameSystemContext&&) = delete;
	virtual ~IGameSystemContext() = default;

	[[nodiscard]] virtual std::size_t GetPlayerCount() const = 0;
	[[nodiscard]] virtual FVector3 GetPlayerFoot(std::size_t Player) const = 0;
	[[nodiscard]] virtual FVector3 GetPlayerVelocity(std::size_t Player) const = 0;
	[[nodiscard]] virtual bool IsPlayerGrounded(std::size_t Player) const = 0;
	// A line in the session's event log, which the editor and the game write to their logs.
	virtual void Log(std::string_view Message) = 0;
	// A level sound or effect cue, such as objective or sparks, as gameplay graphs name them.
	virtual void PlaySound(std::string_view Cue, const FVector3& Position) = 0;
	virtual void SpawnEffect(std::string_view Cue, const FVector3& Position, float Scale) = 0;
};

// Game logic a module adds to every session, run each update after the players and gameplay graphs. Systems drive presentation and their own
// state, never the simulation, so they stay out of rollback snapshots.
class IGameSystem
{
public:
	IGameSystem() = default;
	IGameSystem(const IGameSystem&) = delete;
	IGameSystem& operator=(const IGameSystem&) = delete;
	IGameSystem(IGameSystem&&) = delete;
	IGameSystem& operator=(IGameSystem&&) = delete;
	virtual ~IGameSystem() = default;

	virtual void Update(IGameSystemContext& Context, float DeltaSeconds) = 0;
	// State a hot reload carries to the system's next build, in the system's own format and version. Stateless systems keep the defaults.
	[[nodiscard]] virtual std::uint32_t GetStateVersion() const;
	[[nodiscard]] virtual std::vector<std::byte> SaveState() const;
	// False for state this build cannot take, such as an older version, which makes the reload ask for a restart instead.
	[[nodiscard]] virtual bool LoadState(std::uint32_t Version, std::span<const std::byte> State);
};

inline std::uint32_t IGameSystem::GetStateVersion() const
{
	return 0;
}

inline std::vector<std::byte> IGameSystem::SaveState() const
{
	return {};
}

inline bool IGameSystem::LoadState(const std::uint32_t Version, const std::span<const std::byte> State)
{
	return Version == GetStateVersion() && State.empty();
}

// What a module registers when it loads, through the host.
class IGameModuleRegistry
{
public:
	IGameModuleRegistry() = default;
	IGameModuleRegistry(const IGameModuleRegistry&) = delete;
	IGameModuleRegistry& operator=(const IGameModuleRegistry&) = delete;
	IGameModuleRegistry(IGameModuleRegistry&&) = delete;
	IGameModuleRegistry& operator=(IGameModuleRegistry&&) = delete;
	virtual ~IGameModuleRegistry() = default;

	// C++ generated from the project's gameplay graphs, which Shipping runs instead of the call list.
	virtual void AddGameplayGraphs(std::span<const FGeneratedGameplayGraph> Graphs) = 0;
	// Each session creates one system of this name from Create; names stay stable across builds so a reload can match old and new.
	virtual void AddSystem(std::string_view Name, std::unique_ptr<IGameSystem> (*Create)()) = 0;
};

// What a module exports, checked before its Register runs: the interface version, and the engine revision and configuration it was built
// against, since a module shares the host's types and heap.
struct FGameModuleExports
{
	std::uint32_t InterfaceVersion = 0;
	const char* EngineRevision = "";
	const char* Configuration = "";
	void (*Register)(IGameModuleRegistry& Registry) = nullptr;
};

inline constexpr const char* GameModuleEntryName = "HertaGameModule";
using FGameModuleEntry = const FGameModuleExports* (*)();
}

#ifdef HERTA_PLATFORM_WINDOWS
	#define HERTA_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
	#define HERTA_GAME_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(HERTA_DEBUG)
	#define HERTA_GAME_MODULE_CONFIGURATION "Debug"
#elif defined(HERTA_DEVELOPMENT)
	#define HERTA_GAME_MODULE_CONFIGURATION "Development"
#else
	#define HERTA_GAME_MODULE_CONFIGURATION "Shipping"
#endif

#ifndef HERTA_ENGINE_REVISION
	#define HERTA_ENGINE_REVISION "unknown"
#endif

// Defines a module's entry point, which the host finds by name and calls before anything else in the module.
#define HERTA_GAME_MODULE(RegisterFunction)                                                                                                                              \
	HERTA_GAME_MODULE_EXPORT const ::Herta::FGameModuleExports* HertaGameModule()                                                                                        \
	{                                                                                                                                                                    \
		static const ::Herta::FGameModuleExports Exports{::Herta::GameModuleInterfaceVersion, HERTA_ENGINE_REVISION, HERTA_GAME_MODULE_CONFIGURATION, RegisterFunction}; \
		return &Exports;                                                                                                                                                 \
	}
