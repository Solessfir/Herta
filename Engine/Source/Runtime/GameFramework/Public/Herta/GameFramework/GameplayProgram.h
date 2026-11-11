#pragma once

#include "Herta/Assets/GameplayGraph.h"
#include "Herta/Math/Vector.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// What a gameplay graph can do to the world. The session implements it; tests record the calls.
class IGameplayHost
{
public:
	IGameplayHost() = default;
	IGameplayHost(const IGameplayHost&) = delete;
	IGameplayHost& operator=(const IGameplayHost&) = delete;
	IGameplayHost(IGameplayHost&&) = delete;
	IGameplayHost& operator=(IGameplayHost&&) = delete;
	virtual ~IGameplayHost() = default;

	virtual void PlaySound(std::string_view Cue, const FVector3& Position) = 0;
	virtual void SpawnEffect(std::string_view Cue, const FVector3& Position, float Scale) = 0;
	virtual void Log(std::string_view Message, float Value) = 0;
};

// One run of an event on one instance: its frame, the world it acts on, and what it reads about its entity.
struct FGameplayContext
{
	std::span<float> Slots{};
	IGameplayHost* Host = nullptr;
	std::span<const std::string> Strings{};
	FVector3 SelfPosition{};
	float DeltaSeconds = 0.f;
	float PlayerDistance = 0.f;
	// Graph nodes the call list ran, in order, when a debugger asks for them; generated code never traces.
	std::vector<std::uint32_t>* Trace = nullptr;
};

// Code generated from one cooked program: a function per event, or null where the graph has none.
struct FGeneratedGameplayGraph
{
	std::uint64_t Hash = 0;
	std::array<void (*)(FGameplayContext&), GameplayEventCount> Events{};
};

// Every program compiled into this executable as C++, from GeneratedGameplayGraphs.cpp.
[[nodiscard]] std::span<const FGeneratedGameplayGraph> GetGeneratedGameplayGraphs();

enum class EGameplayBackend : std::uint8_t
{
	// Each instruction is a direct call through a function pointer: no build step, live swaps, and traces for debuggers.
	CallList,
	// Generated C++ compiled into the executable, used when it was generated from exactly this program.
	Native,
};

// A cooked gameplay graph ready to run on any number of instances, each holding only its own frame.
class FGameplayProgram
{
public:
	// Native falls back to the call list when no generated code matches the program's hash.
	[[nodiscard]] static FGameplayProgram Create(std::shared_ptr<const FCookedGameplayGraph> Graph, EGameplayBackend Preferred);

	[[nodiscard]] std::vector<float> MakeFrame() const;
	[[nodiscard]] bool Handles(EGameplayEvent Event) const;
	void Run(EGameplayEvent Event, FGameplayContext& Context) const;
	[[nodiscard]] EGameplayBackend GetBackend() const;
	[[nodiscard]] const FCookedGameplayGraph& GetGraph() const;

private:
	using FStep = std::uint32_t (*)(FGameplayContext&, const FGameplayInstruction&, std::uint32_t);

	std::shared_ptr<const FCookedGameplayGraph> Graph{};
	std::vector<FStep> Steps{};
	const FGeneratedGameplayGraph* Native = nullptr;
};
}
