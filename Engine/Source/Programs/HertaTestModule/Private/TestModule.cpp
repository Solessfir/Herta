// A game module for HertaTests, built twice: as HertaTestModule with state version 1 and as HertaTestModuleV2 with version 2, so a test can
// reload a build that keeps the running state and one that cannot take it.
#include "Herta/GameFramework/GameModule.h"

#include <cstring>
#include <format>

#ifndef HERTA_TEST_MODULE_STATE_VERSION
	#define HERTA_TEST_MODULE_STATE_VERSION 1
#endif

namespace Herta
{
namespace
{
// Counts its updates and logs the count, so a test sees whether a reload carried the count over.
class FCounterSystem final : public IGameSystem
{
public:
	void Update(IGameSystemContext& Context, float DeltaSeconds) override;
	[[nodiscard]] std::uint32_t GetStateVersion() const override;
	[[nodiscard]] std::vector<std::byte> SaveState() const override;
	[[nodiscard]] bool LoadState(std::uint32_t Version, std::span<const std::byte> State) override;

private:
	std::uint64_t Count = 0;
};

void FCounterSystem::Update(IGameSystemContext& Context, float)
{
	++Count;
	Context.Log(std::format("Counter {} at version {}", Count, HERTA_TEST_MODULE_STATE_VERSION));
}

std::uint32_t FCounterSystem::GetStateVersion() const
{
	return HERTA_TEST_MODULE_STATE_VERSION;
}

std::vector<std::byte> FCounterSystem::SaveState() const
{
	std::vector<std::byte> State(sizeof(Count));
	std::memcpy(State.data(), &Count, sizeof(Count));
	return State;
}

bool FCounterSystem::LoadState(const std::uint32_t Version, const std::span<const std::byte> State)
{
	if (Version != HERTA_TEST_MODULE_STATE_VERSION || State.size() != sizeof(Count))
	{
		return false;
	}

	std::memcpy(&Count, State.data(), sizeof(Count));
	return true;
}

std::unique_ptr<IGameSystem> CreateCounter()
{
	return std::make_unique<FCounterSystem>();
}

void Register(IGameModuleRegistry& Registry)
{
	Registry.AddSystem("Counter", CreateCounter);
}
}
}

HERTA_GAME_MODULE(Herta::Register)
