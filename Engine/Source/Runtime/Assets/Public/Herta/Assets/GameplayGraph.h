#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Herta
{
inline constexpr std::size_t MaximumGameplaySlots = 4096;
inline constexpr std::size_t MaximumGameplayInstructions = 16384;
inline constexpr std::size_t MaximumGameplayStrings = 256;
inline constexpr std::uint32_t GameplayNoEntry = 0xffffffffu;

// What starts a gameplay graph running on its entity.
enum class EGameplayEvent : std::uint8_t
{
	// Once, when the session starts.
	Start,
	// The player stepped into the entity's trigger.
	PlayerEnter,
	// The player left the entity's trigger.
	PlayerExit,
	// Every fixed step.
	Tick,
};

inline constexpr std::size_t GameplayEventCount = 4;

// One step of a gameplay program. Slots are floats in the instance's frame; a vector takes three in a row and a bool is 0 or 1.
enum class EGameplayOp : std::uint8_t
{
	// Out = A + B, A - B, A * B.
	Add,
	Subtract,
	Multiply,
	// Out = 1 when A > B, else 0.
	Greater,
	// Out = 1 when A is 0, else 0.
	Not,
	// Out = A.
	Copy,
	// Out = the entity's position, a vector.
	SelfPosition,
	// Out = the fixed step in seconds.
	DeltaSeconds,
	// Out = meters from the entity to the player.
	PlayerDistance,
	// Continues when A is not 0, else jumps.
	Branch,
	Jump,
	End,
	// Plays the level sound named by String at vector A.
	PlaySound,
	// Spawns the level effect named by String at vector A, scaled by B.
	SpawnEffect,
	// Logs String with the value of A.
	Log,
};

struct FGameplayInstruction
{
	bool operator==(const FGameplayInstruction&) const = default;

	EGameplayOp Op = EGameplayOp::End;
	std::uint16_t A = 0;
	std::uint16_t B = 0;
	std::uint16_t Out = 0;
	std::uint16_t String = 0;
	// Branch and Jump only; always forward, so a program cannot loop.
	std::uint32_t Jump = 0;
	// The graph node it came from, for debuggers to light.
	std::uint32_t Node = 0;
};

// A gameplay graph compiled to one flat program: each event's entry runs straight through to an End, jumping only forward.
struct FCookedGameplayGraph
{
	bool operator==(const FCookedGameplayGraph&) const = default;

	// Every slot's value when an instance starts, constants included.
	std::vector<float> Slots{};
	std::array<std::uint32_t, GameplayEventCount> Entries{GameplayNoEntry, GameplayNoEntry, GameplayNoEntry, GameplayNoEntry};
	std::vector<FGameplayInstruction> Code{};
	// Sound and effect cues and log messages.
	std::vector<std::string> Strings{};
};

[[nodiscard]] std::expected<void, FAssetError> ValidateGameplayGraph(const FCookedGameplayGraph& Graph);
// Identifies a program for the native code generated from it; changing any instruction, slot, or string changes it.
[[nodiscard]] std::uint64_t HashGameplayGraph(const FCookedGameplayGraph& Graph);
}
