#include "Herta/GameFramework/GameAssets.h"
#include "Herta/Networking/NetworkTransport.h"
#include "Herta/Replication/GameClient.h"
#include "Herta/Replication/GameServer.h"
#include "Herta/Replication/ReplicationProtocol.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <random>
#include <string>
#include <thread>

namespace Herta
{
namespace
{
class FNoReplicationAssets final : public IGameAssets
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

constexpr FAssetId CourseCube{0x59f13694df4844e5, 0x863555194493f242};
constexpr std::uint64_t CourseHash = 0x5eed;

// The session tests' course, with a crate the players can push. Ids are fixed so server and clients build the same level.
[[nodiscard]] std::vector<FLevelEntity> MakeNetworkCourse()
{
	return {
	    FLevelEntity{.Id = FObjectId{1, 1}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = CourseCube}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{1, 2}, .Name = "Start", .Transform = {.Translation = {0., 1., 0.}}, .Trigger = FTriggerComponent{.Size = {3.f, 2.f, 3.f}, .Role = ETriggerRole::Start}},
	    FLevelEntity{.Id = FObjectId{1, 3}, .Name = "Finish", .Transform = {.Translation = {0., 1., 10.}}, .Trigger = FTriggerComponent{.Size = {6.f, 2.f, 2.f}, .Role = ETriggerRole::Finish}},
	    FLevelEntity{.Id = FObjectId{1, 4}, .Name = "Crate", .Transform = {.Translation = {2.5, 0.5, 5.}}, .Mesh = FStaticMeshComponent{.Asset = CourseCube}, .BodyType = ELevelBodyType::Dynamic},
	};
}

[[nodiscard]] std::unique_ptr<FGameSession> MakeSession(const IGameAssets& Assets)
{
	auto Session = FGameSession::Create(MakeNetworkCourse(), Assets, nullptr);
	REQUIRE_MESSAGE(Session, (Session ? std::string{} : Session.error().Message));
	return std::move(*Session);
}

// Ticks the server and every client until Done holds or Ticks run out. Paced runs sleep a tick between steps, so simulated latency, which is
// measured in wall time, costs the same ticks it would in a game.
bool Run(FGameServer& Server, std::span<FGameClient* const> Clients, const std::function<FGameInput(std::size_t Client, int Tick)>& Input, const int Ticks, const std::function<bool()>& Done, const bool bPaced = false)
{
	for (int Tick = 0; Tick < Ticks; ++Tick)
	{
		REQUIRE(Server.Tick());
		for (std::size_t Index = 0; Index < Clients.size(); ++Index)
		{
			REQUIRE(Clients[Index]->Tick(Input(Index, Tick)));
		}

		if (Done && Done())
		{
			return true;
		}

		std::this_thread::sleep_for(bPaced ? std::chrono::microseconds(16667) : std::chrono::microseconds(200));
	}

	return !Done;
}

[[nodiscard]] std::vector<std::byte> Text(const std::string_view Value)
{
	const auto View = std::as_bytes(std::span(Value.data(), Value.size()));
	return {View.begin(), View.end()};
}
}

TEST_CASE("Replication messages round trip and refuse truncated, oversized, and non-finite data")
{
	const auto Hello = DecodeHello(EncodeHello({.ContentHash = 7, .ReconnectToken = 9, .Name = std::string(80, 'x')}));
	REQUIRE(Hello);
	CHECK(Hello->Version == ReplicationProtocolVersion);
	CHECK(Hello->ContentHash == 7);
	CHECK(Hello->ReconnectToken == 9);
	CHECK(Hello->Name.size() == MaximumPlayerNameLength);

	FReplicationInput Input{.AckedSnapshot = 40, .AckMask = 5};
	for (std::uint32_t Tick = 10; Tick < 20; ++Tick)
	{
		Input.Frames.push_back({.Tick = Tick, .Input = {.Move = {0.5f, -1.f}, .YawDelta = 0.01f * static_cast<float>(Tick), .bJump = Tick == 15, .bBlock = true}});
	}

	const auto Decoded = DecodeInput(EncodeInput(Input));
	REQUIRE(Decoded);
	// Only the newest frames travel; the rest were sent before.
	REQUIRE(Decoded->Frames.size() == InputRedundancy);
	CHECK(Decoded->Frames.front().Tick == 12);
	CHECK(Decoded->Frames[3].Input.bJump);
	CHECK(Decoded->Frames.back().Input.Move.Y == doctest::Approx(-1.f));
	CHECK(Decoded->Frames.back().Input.YawDelta == doctest::Approx(0.19f));

	Input.Frames.back().Input.YawDelta = std::numeric_limits<float>::infinity();
	CHECK(!DecodeInput(EncodeInput(Input)));
	std::vector<std::byte> Truncated = EncodeInput({.Frames = {{.Tick = 1}}});
	Truncated.pop_back();
	CHECK(!DecodeInput(Truncated));
	CHECK(!DecodeHello(Text("not a message")));
	CHECK(!PeekMessage(std::vector<std::byte>(MaximumMessageBytes + 1)));

	FGamePlayerReplica Player{.Yaw = 1.f, .Objective = {.State = EGameObjectiveState::Running, .ElapsedSeconds = 3.5, .BestSeconds = 2.}, .bRagdolled = true, .Attacks = 4};
	Player.Mover.State.Foot = {1.f, 2.f, 3.f};
	Player.Mover.State.WallRunNormal = FVector3{1.f, 0.f, 0.f};
	Player.Mover.Modes = {{}, {1.f, 2.f}, {}};
	const auto Replica = DecodePlayer(EncodePlayer(Player));
	REQUIRE(Replica);
	CHECK(Replica->Mover.State.Foot.Z == 3.f);
	CHECK(Replica->Mover.State.WallRunNormal);
	CHECK(Replica->Mover.Modes[1].size() == 2);
	CHECK(Replica->Objective.BestSeconds == 2.);
	CHECK(Replica->bRagdolled);
	CHECK(Replica->Attacks == 4);
	Player.Mover.State.Velocity.X = std::numeric_limits<float>::quiet_NaN();
	CHECK(!DecodePlayer(EncodePlayer(Player)));

	const auto Outcome = DecodeOutcome(EncodeOutcome({.Tick = 3, .Outcome = {.Kind = EGameOutcome::Break, .Object = 5, .IntactBonds = {true, false, true, true, false, false, true, true, false}}}));
	REQUIRE(Outcome);
	CHECK(Outcome->Outcome.IntactBonds == std::vector<bool>{true, false, true, true, false, false, true, true, false});
}

TEST_CASE("Replication snapshots send only what changed since an acknowledged baseline")
{
	FLevelBodyReplica Body{.Body = 3, .Transform = {.Position = {1.f, 2.f, 3.f}, .Rotation = FQuaternion::Identity()}};
	const std::vector<std::byte> Baseline = EncodeBody(Body);
	Body.Transform.Position.Y = 2.5f;
	const std::vector<std::byte> Moved = EncodeBody(Body);
	const FBaselineLookup Lookup = [&Baseline](const std::uint32_t Id, const std::uint32_t Sequence) -> const std::vector<std::byte>*
	{
		return Id == BodyEntityBase + 3 && Sequence == 4 ? &Baseline : nullptr;
	};

	const FReplicationSnapshot Snapshot{.Sequence = 6, .Tick = 18, .PlayerId = 1, .Entities = {{.Id = BodyEntityBase + 3, .Kind = EReplicatedKind::Body, .State = Moved, .BaselineSequence = 4}}, .Removed = {7}};
	const std::vector<std::byte> Delta = EncodeSnapshot(Snapshot, Lookup);
	const std::vector<std::byte> Whole = EncodeSnapshot(Snapshot, {});
	// One changed float costs a few bytes against the baseline instead of the whole state.
	CHECK(Delta.size() + 8 < Whole.size());

	const auto Received = DecodeSnapshot(Delta, Lookup);
	REQUIRE(Received);
	REQUIRE(Received->Entities.size() == 1);
	CHECK(Received->Entities.front().State == Moved);
	CHECK(Received->Removed == std::vector<std::uint32_t>{7});
	const auto Decoded = DecodeBody(Received->Entities.front().State);
	REQUIRE(Decoded);
	CHECK(Decoded->Transform.Position.Y == 2.5f);

	// A receiver that lost the baseline cannot rebuild the state and says so instead of guessing.
	CHECK(!DecodeSnapshot(Delta, {}));
	CHECK(DecodeSnapshot(Whole, {}));
}

TEST_CASE("Replication decoders survive random and corrupted messages")
{
	std::mt19937 Random(1234);
	const std::vector<std::vector<std::byte>> Valid{
	    EncodeHello({.Name = "Fuzz"}),
	    EncodeInput({.AckedSnapshot = 3, .Frames = {{.Tick = 5}, {.Tick = 6}}}),
	    EncodeSnapshot({.Sequence = 2, .Entities = {{.Id = 1, .State = EncodePlayer({})}}}, {}),
	    EncodeOutcome({.Outcome = {.Kind = EGameOutcome::Sever, .Joint = "hand_l"}}),
	    EncodeWelcome({.PlayerId = 1}),
	};

	std::size_t Accepted = 0;
	for (int Round = 0; Round < 20000; ++Round)
	{
		std::vector<std::byte> Message = Valid[static_cast<std::size_t>(Round) % Valid.size()];
		const int Edits = 1 + static_cast<int>(Random() % 4);
		for (int Edit = 0; Edit < Edits && !Message.empty(); ++Edit)
		{
			const std::size_t At = Random() % Message.size();
			switch (Random() % 3)
			{
				case 0:
					Message[At] = static_cast<std::byte>(Random());
					break;
				case 1:
					Message.resize(At);
					break;
				default:
					Message.insert(Message.begin() + static_cast<std::ptrdiff_t>(At), static_cast<std::byte>(Random()));
					break;
			}
		}

		Accepted += DecodeHello(Message).has_value() + DecodeInput(Message).has_value() + DecodeSnapshot(Message, {}).has_value() + DecodeOutcome(Message).has_value() + DecodeWelcome(Message).has_value();
		if (const auto Snapshot = DecodeSnapshot(Message, {}))
		{
			for (const FReplicatedEntity& Entity : Snapshot->Entities)
			{
				(void)DecodePlayer(Entity.State);
				(void)DecodeBody(Entity.State);
			}
		}
	}

	// Some corruptions still decode, such as a changed name byte; the point is that none of them crash or hand back non-finite values.
	CHECK(Accepted > 0);
}

TEST_CASE("Replicated sessions play the course through a listen server's loopback client and a UDP client")
{
	const FNoReplicationAssets Assets;
	auto Server = FGameServer::Create(MakeSession(Assets), {.Port = 0, .ContentHash = CourseHash});
	REQUIRE_MESSAGE(Server, (Server ? std::string{} : Server.error().Message));
	auto Local = FGameClient::ConnectLocal(MakeSession(Assets), **Server, {.Name = "Host", .ContentHash = CourseHash});
	REQUIRE(Local);
	auto Remote = FGameClient::Connect(MakeSession(Assets), std::format("127.0.0.1:{}", (*Server)->GetPort()), {.Name = "Guest", .ContentHash = CourseHash});
	REQUIRE(Remote);
	const std::array<FGameClient*, 2> Clients{Local->get(), Remote->get()};

	REQUIRE(Run(**Server, Clients, [](std::size_t, int)
	{
		return FGameInput{};
	}, 600, [&]
	{
		return std::ranges::all_of(Clients, [](FGameClient* const Client)
		{
			return Client->GetState() == EGameClientState::Playing && Client->GetSession().GetPlayerCount() == 2;
		});
	}));

	CHECK((*Local)->GetPlayerId() != (*Remote)->GetPlayerId());
	CHECK((*Server)->GetStats().Players == 2);

	// The host runs to the finish; the guest, who spawned on the host's right, angles left into the crate and pushes it.
	const auto Input = [](const std::size_t Client, int)
	{
		return Client == 0 ? FGameInput{.Move = {0.f, 1.f}} : FGameInput{.Move = {-0.55f, 0.83f}};
	};

	REQUIRE(Run(**Server, Clients, Input, 900, [&]
	{
		return (*Local)->GetSession().GetObjective().State == EGameObjectiveState::Finished;
	}));

	// Prediction finished the run locally, a little ahead of the server; once the server catches up it agrees, and the guest sees the host
	// where the server has it.
	FGameSession& Authoritative = (*Server)->GetSession();
	REQUIRE(Run(**Server, Clients, Input, 120, {}));
	// Whichever client's hello arrived first took the server's first player.
	const auto HostSlot = (*Server)->FindPlayerSlot((*Local)->GetPlayerId());
	REQUIRE(HostSlot);
	const auto HostOnServer = Authoritative.CapturePlayer(*HostSlot);
	CHECK(HostOnServer.Objective.State == EGameObjectiveState::Finished);
	CHECK(((*Local)->GetSession().GetPlayerFootPosition() - HostOnServer.Mover.State.Foot).Length() < 0.5f);
	const FGamePlayerReplica HostOnGuest = (*Remote)->GetSession().CapturePlayer(1);
	CHECK((HostOnGuest.Mover.State.Foot - Authoritative.CapturePlayer(*HostSlot).Mover.State.Foot).Length() < 1.f);
	CHECK(HostOnGuest.Objective.State == EGameObjectiveState::Finished);

	// The crate moved on the server, and the guest's copy followed it.
	const auto ServerCrate = Authoritative.CaptureBodies();
	const auto GuestCrate = (*Remote)->GetSession().CaptureBodies();
	REQUIRE(ServerCrate.size() == 1);
	REQUIRE(GuestCrate.size() == 1);
	CHECK((ServerCrate.front().Transform.Position - FVector3{2.5f, 0.5f, 5.f}).Length() > 0.2f);
	CHECK((GuestCrate.front().Transform.Position - ServerCrate.front().Transform.Position).Length() < 0.5f);
	CHECK((*Server)->GetStats().SnapshotsSent > 0);
	CHECK((*Remote)->GetStats().Snapshots > 0);
}

TEST_CASE("Replicated clients reconnect to their player, and servers refuse other content, protocols, full games, and garbage")
{
	const FNoReplicationAssets Assets;
	auto Server = FGameServer::Create(MakeSession(Assets), {.Port = 0, .MaximumPlayers = 1, .ContentHash = CourseHash, .MalformedLimit = 4});
	REQUIRE(Server);
	const std::string Address = std::format("127.0.0.1:{}", (*Server)->GetPort());
	auto Client = FGameClient::Connect(MakeSession(Assets), Address, {.ContentHash = CourseHash});
	REQUIRE(Client);
	const std::array<FGameClient*, 1> Clients{Client->get()};
	const auto Idle = [](std::size_t, int)
	{
		return FGameInput{};
	};

	REQUIRE(Run(**Server, Clients, Idle, 600, [&]
	{
		return (*Client)->GetState() == EGameClientState::Playing;
	}));

	const std::uint32_t Id = (*Client)->GetPlayerId();
	REQUIRE(Run(**Server, Clients, [](std::size_t, int)
	{
		return FGameInput{.Move = {0.f, 1.f}};
	}, 60, {}));

	// A full server turns a second client away and says why.
	auto Extra = FGameClient::Connect(MakeSession(Assets), Address, {.ContentHash = CourseHash});
	REQUIRE(Extra);
	const std::array<FGameClient*, 2> Both{Client->get(), Extra->get()};
	REQUIRE(Run(**Server, Both, Idle, 600, [&]
	{
		return (*Extra)->GetState() == EGameClientState::Rejected;
	}));

	CHECK((*Extra)->GetStateReason().find("full") != std::string_view::npos);

	// Other content is refused at the handshake.
	auto Stranger = FGameClient::Connect(MakeSession(Assets), Address, {.ContentHash = CourseHash + 1});
	REQUIRE(Stranger);
	const std::array<FGameClient*, 2> WithStranger{Client->get(), Stranger->get()};
	REQUIRE(Run(**Server, WithStranger, Idle, 600, [&]
	{
		return (*Stranger)->GetState() == EGameClientState::Rejected;
	}));

	// A dropped client gets its own player back, where it left it.
	const FVector3 Before = (*Server)->GetSession().CapturePlayer(0).Mover.State.Foot;
	(*Client)->Disconnect();
	REQUIRE(Run(**Server, Clients, Idle, 120, {}));
	CHECK((*Server)->GetStats().Players == 1);
	REQUIRE((*Client)->Reconnect());
	REQUIRE(Run(**Server, Clients, Idle, 600, [&]
	{
		return (*Client)->GetState() == EGameClientState::Playing;
	}));

	CHECK((*Client)->GetPlayerId() == Id);
	CHECK((*Server)->GetStats().Reconnects == 1);
	CHECK(((*Server)->GetSession().CapturePlayer(0).Mover.State.Foot - Before).Length() < 0.5f);

	// A peer speaking another protocol, then one sending garbage, is refused or dropped, and the game goes on.
	auto Raw = FNetworkTransport::Create();
	REQUIRE(Raw);
	const auto Connection = (*Raw)->Connect(Address);
	REQUIRE(Connection);
	std::vector<FNetworkConnectionEvent> Events;
	std::vector<FNetworkMessage> Messages;
	bool bClosed = false;
	bool bSentGarbage = false;
	for (int Tick = 0; Tick < 1200 && !bClosed; ++Tick)
	{
		REQUIRE((*Server)->Tick());
		REQUIRE((*Client)->Tick({}));
		Events.clear();
		Messages.clear();
		(*Raw)->Poll(Events, Messages);
		for (const FNetworkConnectionEvent& Event : Events)
		{
			if (Event.State == ENetworkConnectionState::Connected && !bSentGarbage)
			{
				bSentGarbage = true;
				for (int Junk = 0; Junk < 8; ++Junk)
				{
					REQUIRE((*Raw)->Send(*Connection, Text(std::format("junk {}", Junk)), ENetworkDelivery::Reliable));
				}
			}

			bClosed = bClosed || Event.State == ENetworkConnectionState::Closed;
		}

		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}

	CHECK(bClosed);
	CHECK((*Server)->GetStats().MalformedMessages > 4);
	CHECK((*Client)->GetState() == EGameClientState::Playing);

	FReplicationHello Future{.Version = ReplicationProtocolVersion + 1, .ContentHash = CourseHash};
	auto Newer = FNetworkTransport::Create();
	REQUIRE(Newer);
	const auto NewerConnection = (*Newer)->Connect(Address);
	REQUIRE(NewerConnection);
	std::optional<std::string> Refusal;
	for (int Tick = 0; Tick < 1200 && !Refusal; ++Tick)
	{
		REQUIRE((*Server)->Tick());
		Events.clear();
		Messages.clear();
		(*Newer)->Poll(Events, Messages);
		for (const FNetworkConnectionEvent& Event : Events)
		{
			if (Event.State == ENetworkConnectionState::Connected)
			{
				REQUIRE((*Newer)->Send(*NewerConnection, EncodeHello(Future), ENetworkDelivery::Reliable));
			}
		}

		for (const FNetworkMessage& Message : Messages)
		{
			if (const auto Reject = DecodeReject(Message.Data))
			{
				Refusal = Reject->Reason;
			}
		}

		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}

	REQUIRE(Refusal);
	CHECK(Refusal->find("protocol") != std::string::npos);
}

TEST_CASE("Replicated prediction stays with the server through loss, latency, jitter, reordering, duplication, and a bandwidth limit" * doctest::test_suite("Slow"))
{
	const FNoReplicationAssets Assets;
	auto Server = FGameServer::Create(MakeSession(Assets), {.Port = 0, .ContentHash = CourseHash, .SnapshotBudgetBytes = 600});
	REQUIRE(Server);
	auto Client = FGameClient::Connect(MakeSession(Assets), std::format("127.0.0.1:{}", (*Server)->GetPort()), {.ContentHash = CourseHash});
	REQUIRE(Client);
	auto Watcher = FGameClient::Connect(MakeSession(Assets), std::format("127.0.0.1:{}", (*Server)->GetPort()), {.ContentHash = CourseHash});
	REQUIRE(Watcher);
	const std::array<FGameClient*, 2> Clients{Client->get(), Watcher->get()};
	const auto Idle = [](std::size_t, int)
	{
		return FGameInput{};
	};

	REQUIRE(Run(**Server, Clients, Idle, 600, [&]
	{
		return (*Client)->GetState() == EGameClientState::Playing && (*Watcher)->GetState() == EGameClientState::Playing;
	}));

	FNetworkTransport::SetConditions({.LossPercent = 10.f, .LagMilliseconds = 40.f, .JitterMilliseconds = 15.f, .ReorderPercent = 5.f, .DuplicatePercent = 5.f, .RateLimitBytesPerSecond = 64000.f});
	// Weaving, jumping, and turning, so prediction covers acceleration, air control, and look changes.
	const auto Input = [](const std::size_t Index, const int Tick)
	{
		if (Index == 1)
		{
			return FGameInput{};
		}

		return FGameInput{.Move = {std::sin(static_cast<float>(Tick) * 0.05f) * 0.5f, 1.f}, .YawDelta = Tick % 40 < 20 ? 0.004f : -0.004f, .bJump = Tick % 50 == 0};
	};

	const auto Runner = (*Server)->FindPlayerSlot((*Client)->GetPlayerId());
	REQUIRE(Runner);
	const bool bFinished = Run(**Server, Clients, Input, 600, [&]
	{
		return (*Server)->GetSession().CapturePlayer(*Runner).Objective.State == EGameObjectiveState::Finished;
	}, true);
	FNetworkTransport::SetConditions({});
	REQUIRE(bFinished);

	// Once input stops, every correction has arrived, and the prediction settles on the server's state.
	REQUIRE(Run(**Server, Clients, Idle, 90, {}, true));
	const FVector3 Authoritative = (*Server)->GetSession().CapturePlayer(*Runner).Mover.State.Foot;
	CHECK(((*Client)->GetSession().GetPlayerFootPosition() - Authoritative).Length() < 0.05f);
	CHECK((*Client)->GetSession().GetObjective().State == EGameObjectiveState::Finished);
	// The watcher sees the runner from interpolated snapshots, a little behind.
	CHECK(((*Watcher)->GetSession().CapturePlayer(1).Mover.State.Foot - Authoritative).Length() < 0.5f);
	MESSAGE(std::format("{} corrections, {} snapshots, {} late, {} deferred entities, {} missing inputs", (*Client)->GetStats().Corrections, (*Client)->GetStats().Snapshots, (*Client)->GetStats().LateSnapshots, (*Server)->GetStats().DeferredEntities, (*Server)->GetStats().MissingInputs));
}
}
