#include "SandboxGame.h"

#include <cmath>
#include <cstring>
#include <format>

namespace Herta
{
// From GeneratedSandboxGameplayGraphs.cpp, which HertaEditorCmd gameplay.generate writes from the Sandbox's content.
std::span<const FGeneratedGameplayGraph> GetSandboxGameplayGraphs();

namespace
{
// The first player's run: meters covered on foot and jumps taken, called out every 50 m, with the objective chime at each 200 m.
class FRunStatsSystem final : public IGameSystem
{
public:
	void Update(IGameSystemContext& Context, float DeltaSeconds) override;
	[[nodiscard]] std::uint32_t GetStateVersion() const override;
	[[nodiscard]] std::vector<std::byte> SaveState() const override;
	[[nodiscard]] bool LoadState(std::uint32_t Version, std::span<const std::byte> State) override;

private:
	struct FState
	{
		float Meters = 0.f;
		float NextCallout = 50.f;
		std::uint32_t Jumps = 0;
		bool bWasGrounded = true;
	};

	FState State{};
	bool bHasLast = false;
	FVector3 Last{};
};

void FRunStatsSystem::Update(IGameSystemContext& Context, float)
{
	if (Context.GetPlayerCount() == 0)
	{
		return;
	}

	const FVector3 Foot = Context.GetPlayerFoot(0);
	const bool bGrounded = Context.IsPlayerGrounded(0);
	// Only ground covered on foot counts; a teleport or a respawn moves more than a sprint can in one update and is skipped.
	const float Step = bHasLast ? std::hypot(Foot.X - Last.X, Foot.Z - Last.Z) : 0.f;
	if (bGrounded && Step < 1.f)
	{
		State.Meters += Step;
	}

	if (State.bWasGrounded && !bGrounded)
	{
		++State.Jumps;
	}

	State.bWasGrounded = bGrounded;
	Last = Foot;
	bHasLast = true;
	if (State.Meters >= State.NextCallout)
	{
		Context.Log(std::format("Ran {:.0f} m with {} jumps", State.NextCallout, State.Jumps));
		if (std::fmod(State.NextCallout, 200.f) == 0.f)
		{
			Context.PlaySound("objective", Foot);
		}

		State.NextCallout += 50.f;
	}
}

std::uint32_t FRunStatsSystem::GetStateVersion() const
{
	return 1;
}

std::vector<std::byte> FRunStatsSystem::SaveState() const
{
	std::vector<std::byte> Bytes(sizeof(FState));
	std::memcpy(Bytes.data(), &State, sizeof(FState));
	return Bytes;
}

bool FRunStatsSystem::LoadState(const std::uint32_t Version, const std::span<const std::byte> Bytes)
{
	if (Version != GetStateVersion() || Bytes.size() != sizeof(FState))
	{
		return false;
	}

	std::memcpy(&State, Bytes.data(), sizeof(FState));
	return true;
}

std::unique_ptr<IGameSystem> CreateRunStats()
{
	return std::make_unique<FRunStatsSystem>();
}
}

void RegisterSandboxGame(IGameModuleRegistry& Registry)
{
	Registry.AddGameplayGraphs(GetSandboxGameplayGraphs());
	Registry.AddSystem("RunStats", CreateRunStats);
}
}

HERTA_GAME_MODULE(Herta::RegisterSandboxGame)
