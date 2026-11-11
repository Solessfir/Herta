#include "Herta/Audio/AudioEngine.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
class FNoAudioGameAssets final : public IGameAssets
{
public:
	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId&) const override
	{
		return nullptr;
	}
};

constexpr FAssetId Cube{0x59f13694df4844e5, 0x863555194493f242};
constexpr FAssetId Hum{7, 1};
constexpr FAssetId Step{7, 2};
constexpr FAssetId Jump{7, 3};
constexpr FAssetId Impact{7, 4};
constexpr FAssetId Objective{7, 5};

[[nodiscard]] std::vector<FLevelEntity> MakeAudioCourse()
{
	return {
	    FLevelEntity{.Id = FObjectId{7, 10}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = Cube}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{7, 11}, .Name = "Start", .Transform = {.Translation = {0., 1., 0.}}, .Trigger = FTriggerComponent{.Size = {2.f, 2.f, 2.f}, .Role = ETriggerRole::Start, .Sound = Objective}},
	    FLevelEntity{.Id = FObjectId{7, 12}, .Name = "Finish", .Transform = {.Translation = {0., 1., 10.}}, .Trigger = FTriggerComponent{.Size = {4.f, 2.f, 2.f}, .Role = ETriggerRole::Finish, .Sound = Objective}},
	    // Without a skinned model the player is the capsule, which still plays its Character's sounds.
	    FLevelEntity{.Id = FObjectId{7, 16}, .Name = "Player", .Transform = {.Translation = {0., 0.05, 0.}}, .Character = FCharacterComponent{.FootstepSound = Step, .JumpSound = Jump}, .Player = FPlayerComponent{}},
	    FLevelEntity{.Id = FObjectId{7, 13}, .Name = "Speaker", .Transform = {.Translation = {3., 1., 4.}}, .AudioEmitter = FAudioEmitterComponent{.Sound = Hum, .Volume = 0.5f}},
	    FLevelEntity{.Id = FObjectId{7, 14}, .Name = "Silent", .Transform = {.Translation = {-3., 1., 4.}}, .AudioEmitter = FAudioEmitterComponent{.Sound = Hum, .bAutoPlay = false}},
	    FLevelEntity{.Id = FObjectId{7, 15}, .Name = "Crate", .Transform = {.Translation = {2., 3., 2.}, .Scale = {0.5f, 0.5f, 0.5f}}, .Mesh = FStaticMeshComponent{.Asset = Cube}, .BodyType = ELevelBodyType::Dynamic, .BodySettings = {.MassKg = 20.f, .ImpactSound = Impact}},
	};
}

[[nodiscard]] const FAudioPlayCommand* FindPlay(const std::span<const FAudioCommand> Commands, const FAssetId& Sound)
{
	for (const FAudioCommand& Command : Commands)
	{
		if (const auto* Play = std::get_if<FAudioPlayCommand>(&Command); Play && Play->Sound == Sound)
		{
			return Play;
		}
	}

	return nullptr;
}

// Walks out of the Start zone, jumps, and throws a ball, recording every audio command.
[[nodiscard]] std::vector<FAudioCommand> PlayScript()
{
	const FNoAudioGameAssets Assets;
	auto Session = FGameSession::Create(MakeAudioCourse(), Assets, nullptr);
	REQUIRE(Session);
	REQUIRE((*Session)->Restart());
	std::vector<FAudioCommand> Commands;
	const auto Run = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Input));
			std::ranges::move((*Session)->TakeAudioCommands(), std::back_inserter(Commands));
		}
	};

	Run({}, 30);
	Run({.Move = {0.f, 1.f}}, 90);
	Run({.Move = {0.f, 1.f}, .bJump = true}, 1);
	Run({.Move = {0.f, 1.f}}, 40);
	Run({.bThrow = true}, 1);
	Run({}, 60);
	return Commands;
}
}

TEST_CASE("Game sessions issue the same audio commands for the same play, headless")
{
	const std::vector<FAudioCommand> Commands = PlayScript();
	REQUIRE_FALSE(Commands.empty());
	// Every start stops what played before and starts the auto-play emitters, which skip manual ones; the script starts twice, on creation and
	// on the restart.
	CHECK(std::holds_alternative<FAudioStopAllCommand>(Commands.front()));
	const FAudioPlayCommand* const Speaker = FindPlay(Commands, Hum);
	REQUIRE(Speaker);
	CHECK(Speaker->Voice.Value == 1);
	CHECK(Speaker->bSpatial);
	CHECK(Speaker->Volume == 0.5f);
	CHECK(Speaker->Position == FVector3{3.f, 1.f, 4.f});
	CHECK(std::ranges::count_if(Commands, [](const FAudioCommand& Command)
	{
		const auto* Play = std::get_if<FAudioPlayCommand>(&Command);
		return Play && Play->Sound == Hum;
	}) == 2);

	// Walking off the start begins the run, the capsule ticks off footsteps, and the jump and the falling crate sound.
	const FAudioPlayCommand* const Started = FindPlay(Commands, Objective);
	REQUIRE(Started);
	CHECK_FALSE(Started->bSpatial);
	CHECK(FindPlay(Commands, Step));
	CHECK(FindPlay(Commands, Jump));
	const FAudioPlayCommand* const Crash = FindPlay(Commands, Impact);
	REQUIRE(Crash);
	REQUIRE(Crash->Parameters.size() == 1);
	CHECK(Crash->Parameters[0].Name == "Impulse");
	CHECK(Crash->Parameters[0].Value >= 1.5f);
	CHECK(std::ranges::any_of(Commands, [](const FAudioCommand& Command)
	{
		return std::holds_alternative<FAudioListenerCommand>(Command);
	}));

	const std::vector<FAudioCommand> Again = PlayScript();
	REQUIRE(Again.size() == Commands.size());
	for (std::size_t Index = 0; Index < Commands.size(); ++Index)
	{
		CHECK_MESSAGE(Again[Index] == Commands[Index], FormatAudioCommand(Commands[Index]));
	}

	// The null path plays them all without a device: every sound resolves and voices start.
	auto Engine = FAudioEngine::Create({.Device = EAudioDevice::Null});
	REQUIRE(Engine);
	for (const FAssetId& Sound : {Hum, Step, Jump, Impact, Objective})
	{
		auto Tone = std::make_shared<FCookedSound>();
		Tone->Samples.assign(4800, 1000);
		Tone->bLoop = Sound == Hum;
		REQUIRE((*Engine)->AddSound(Sound, Tone));
	}

	(*Engine)->Submit(Commands);
	(*Engine)->Update(0.5f);
	const FAudioStats Stats = (*Engine)->GetStats();
	CHECK(Stats.MissingSounds == 0);
	CHECK(Stats.VoicesStarted >= 5);
	CHECK((*Engine)->IsVoicePlaying({1}));
}

TEST_CASE("An active listener component hears the level instead of the player")
{
	const FNoAudioGameAssets Assets;
	std::vector<FLevelEntity> Entities = MakeAudioCourse();
	Entities.push_back(FLevelEntity{.Id = FObjectId{7, 20}, .Name = "Inactive ears", .Transform = {.Translation = {9., 9., 9.}}, .AudioListener = FAudioListenerComponent{.bActive = false}});
	Entities.push_back(FLevelEntity{.Id = FObjectId{7, 21}, .Name = "Camera ears", .Transform = {.Translation = {-5., 4., 6.}}, .AudioListener = FAudioListenerComponent{}});
	auto Session = FGameSession::Create(Entities, Assets, nullptr);
	REQUIRE(Session);
	REQUIRE((*Session)->Update(1.f / 60.f, {}));
	const std::vector<FAudioCommand> Commands = (*Session)->TakeAudioCommands();
	const auto Listener = std::ranges::find_if(Commands, [](const FAudioCommand& Command)
	{
		return std::holds_alternative<FAudioListenerCommand>(Command);
	});

	REQUIRE(Listener != Commands.end());
	const FAudioListener& Heard = std::get<FAudioListenerCommand>(*Listener).Listener;
	CHECK(Heard.Position == FVector3{-5.f, 4.f, 6.f});
	CHECK(Heard.Forward.Z == doctest::Approx(1.f));
}
}
