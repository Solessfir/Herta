#include "Herta/Replication/GameServer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr float TickSeconds = 1.f / static_cast<float>(ReplicationTicksPerSecond);
// Inputs further ahead than this are a client running wild, not latency.
constexpr std::uint32_t MaximumInputLead = 120;
// A client that keeps more inputs queued than this skips ahead, so a burst after a stall does not leave its player permanently late.
constexpr std::size_t MaximumQueuedInputs = 6;
// An unowned player waits here, below the level, until someone joins.
const FVector3 ParkedFoot{0.f, -1000.f, 0.f};

// One entity as every client might receive it this snapshot.
struct FCandidate
{
	std::uint32_t Id = 0;
	EReplicatedKind Kind = EReplicatedKind::Player;
	FVector3 Position = FVector3::Zero();
	std::vector<std::byte> State{};
};

[[nodiscard]] float KindWeight(const EReplicatedKind Kind) noexcept
{
	switch (Kind)
	{
		case EReplicatedKind::Player:
			return 4.f;
		case EReplicatedKind::Projectile:
			return 3.f;
		case EReplicatedKind::Npc:
			return 2.f;
		case EReplicatedKind::Body:
			return 1.f;
	}

	return 1.f;
}

// Only movement and held buttons carry over a missing input: a press or a look turn would repeat on every missing tick.
[[nodiscard]] FGameInput HoldInput(const FGameInput& Last) noexcept
{
	return {.Move = Last.Move, .bSprint = Last.bSprint, .bCrouch = Last.bCrouch, .bBlock = Last.bBlock};
}
}

struct FGameServer::FImplementation
{
	struct FKnown
	{
		EReplicatedKind Kind = EReplicatedKind::Player;
		std::uint32_t AckedSequence = 0;
		std::vector<std::byte> AckedState{};
		float Priority = 0.f;
	};

	struct FSent
	{
		std::uint32_t Sequence = 0;
		std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> Entities{};
		bool bAcked = false;
	};

	struct FPlayer
	{
		std::uint32_t Id = 0;
		std::uint64_t Token = 0;
		std::string Name{};
		std::optional<FNetworkConnectionId> Connection{};
		float AwaySeconds = 0.f;
		std::size_t Slot = 0;

		std::map<std::uint32_t, FGameInput> Pending{};
		std::optional<std::uint32_t> NextInputTick{};
		std::uint32_t AppliedInputTick = 0;
		FGameInput LastInput{};

		std::uint32_t NextSequence = 1;
		std::deque<FSent> Sent{};
		std::map<std::uint32_t, FKnown> Known{};
		// Entities the client still has that it must drop, with the first snapshot that told it.
		std::map<std::uint32_t, std::uint32_t> Removing{};
	};

	struct FConnection
	{
		FNetworkConnectionId Id{};
		std::optional<std::uint32_t> Player{};
		std::uint32_t Malformed = 0;
	};

	[[nodiscard]] std::expected<void, FReplicationError> Receive();
	void HandleHello(FConnection& Connection, std::span<const std::byte> Message);
	void HandleInput(FConnection& Connection, FPlayer& Player, std::span<const std::byte> Message);
	void Acknowledge(FPlayer& Player, std::uint32_t Sequence);
	void CountMalformed(FConnection& Connection, std::string_view Why);
	void Refuse(FConnection& Connection, std::string_view Reason);
	[[nodiscard]] std::expected<void, FReplicationError> RemovePlayer(std::uint32_t Id);
	[[nodiscard]] std::expected<void, FReplicationError> Park(std::size_t Slot);
	[[nodiscard]] FPlayer* FindPlayer(std::uint32_t Id);
	[[nodiscard]] FGameInput TakeInput(FPlayer& Player);
	void SendOutcome(const FReplicationOutcome& Outcome);
	void SendHistory(FNetworkConnectionId Connection);
	[[nodiscard]] std::vector<FCandidate> GatherEntities() const;
	void SendSnapshot(FPlayer& Player, std::span<const FCandidate> Entities);

	std::unique_ptr<FGameSession> Session{};
	std::unique_ptr<FNetworkTransport> Transport{};
	FGameServerSettings Settings{};
	std::vector<FConnection> Connections{};
	std::vector<FPlayer> Players{};
	// Which session slots a player owns; an unowned one is parked.
	std::vector<bool> SlotOwned{};
	std::uint32_t NextPlayerId = 1;
	std::uint32_t CurrentTick = 0;
	// Breaks hold each destructible's latest bonds and cuts every one since the start, so a client that joins late catches up.
	std::map<std::uint32_t, FReplicationOutcome> Breaks{};
	std::vector<FReplicationOutcome> Cuts{};
	std::mt19937_64 Random{std::random_device{}()};
	FGameServerStats Stats{};
	std::vector<FNetworkConnectionEvent> Events{};
	std::vector<FNetworkMessage> Messages{};
};

FGameServer::FGameServer(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameServer::~FGameServer() = default;

std::expected<std::unique_ptr<FGameServer>, FReplicationError> FGameServer::Create(std::unique_ptr<FGameSession> Session, const FGameServerSettings& Settings)
{
	if (!Session || Settings.MaximumPlayers == 0 || Settings.MaximumPlayers > FGameSession::MaximumPlayers)
	{
		return std::unexpected(FReplicationError{std::format("A server needs a session and between 1 and {} players", FGameSession::MaximumPlayers)});
	}

	auto Transport = FNetworkTransport::Create();
	if (!Transport)
	{
		return std::unexpected(FReplicationError{Transport.error().Message});
	}

	if (auto Listening = (*Transport)->Listen(Settings.Port); !Listening)
	{
		return std::unexpected(FReplicationError{Listening.error().Message});
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Session = std::move(Session);
	Implementation->Transport = std::move(*Transport);
	Implementation->Settings = Settings;
	Implementation->Session->SetAuthority(true);
	Implementation->Session->SetSlowMotionAllowed(false);
	Implementation->SlotOwned.assign(Implementation->Session->GetPlayerCount(), false);
	for (std::size_t Slot = 0; Slot < Implementation->SlotOwned.size(); ++Slot)
	{
		if (auto Parked = Implementation->Park(Slot); !Parked)
		{
			return std::unexpected(Parked.error());
		}
	}

	return std::unique_ptr<FGameServer>(new FGameServer(std::move(Implementation)));
}

std::expected<void, FReplicationError> FGameServer::FImplementation::Park(const std::size_t Slot)
{
	SlotOwned[Slot] = false;
	if (auto Hidden = Session->SetPlayerSimulated(Slot, false); !Hidden)
	{
		return std::unexpected(FReplicationError{Hidden.error().Message});
	}

	if (auto Placed = Session->PlacePlayer(Slot, ParkedFoot); !Placed)
	{
		return std::unexpected(FReplicationError{Placed.error().Message});
	}

	return {};
}

FGameServer::FImplementation::FPlayer* FGameServer::FImplementation::FindPlayer(const std::uint32_t Id)
{
	const auto Found = std::ranges::find(Players, Id, &FPlayer::Id);
	return Found == Players.end() ? nullptr : &*Found;
}

void FGameServer::FImplementation::Refuse(FConnection& Connection, const std::string_view Reason)
{
	++Stats.RejectedClients;
	(void)Transport->Send(Connection.Id, EncodeReject({.Reason = std::string(Reason)}), ENetworkDelivery::Reliable);
	Transport->Close(Connection.Id, Reason);
	Connection.Player.reset();
	Connection.Id = {};
}

void FGameServer::FImplementation::CountMalformed(FConnection& Connection, const std::string_view Why)
{
	++Stats.MalformedMessages;
	if (++Connection.Malformed > Settings.MalformedLimit)
	{
		Transport->Close(Connection.Id, std::format("Too many malformed messages, the last: {}", Why));
		if (Connection.Player)
		{
			if (FPlayer* const Player = FindPlayer(*Connection.Player))
			{
				Player->Connection.reset();
			}
		}

		Connection.Player.reset();
		Connection.Id = {};
	}
}

void FGameServer::FImplementation::HandleHello(FConnection& Connection, const std::span<const std::byte> Message)
{
	const auto Hello = DecodeHello(Message);
	if (!Hello)
	{
		CountMalformed(Connection, Hello.error().Message);
		return;
	}

	if (Hello->Version != ReplicationProtocolVersion)
	{
		Refuse(Connection, std::format("The server speaks protocol {}, the client {}", ReplicationProtocolVersion, Hello->Version));
		return;
	}

	if (Hello->ContentHash != Settings.ContentHash)
	{
		Refuse(Connection, "The client has another level or other content than the server");
		return;
	}

	// A reconnecting client takes back its player, if it is still waiting and nobody else holds it.
	FPlayer* Player = nullptr;
	if (Hello->ReconnectToken != 0)
	{
		const auto Found = std::ranges::find(Players, Hello->ReconnectToken, &FPlayer::Token);
		if (Found != Players.end() && !Found->Connection)
		{
			Player = &*Found;
			++Stats.Reconnects;
		}
	}

	if (!Player)
	{
		if (Players.size() >= Settings.MaximumPlayers)
		{
			Refuse(Connection, std::format("The server is full with {} players", Players.size()));
			return;
		}

		const auto Free = std::ranges::find(SlotOwned, false);
		std::size_t Slot = static_cast<std::size_t>(Free - SlotOwned.begin());
		if (Free == SlotOwned.end())
		{
			auto Added = Session->AddPlayer();
			if (!Added)
			{
				Refuse(Connection, Added.error().Message);
				return;
			}

			Slot = *Added;
			SlotOwned.push_back(false);
		}

		if (auto Simulated = Session->SetPlayerSimulated(Slot, true); !Simulated || !Session->ResetPlayer(Slot))
		{
			Refuse(Connection, "The server cannot place a new player");
			return;
		}

		SlotOwned[Slot] = true;
		Player = &Players.emplace_back(FPlayer{.Id = NextPlayerId++, .Token = Random() | 1, .Slot = Slot});
	}

	// A new connection starts its input timeline and its snapshots over.
	Player->Name = Hello->Name;
	Player->Connection = Connection.Id;
	Player->AwaySeconds = 0.f;
	Player->Pending.clear();
	Player->NextInputTick.reset();
	Player->AppliedInputTick = 0;
	Player->LastInput = {};
	Player->NextSequence = 1;
	Player->Sent.clear();
	Player->Known.clear();
	Player->Removing.clear();
	Connection.Player = Player->Id;
	(void)Transport->Send(Connection.Id, EncodeWelcome({.PlayerId = Player->Id, .Tick = CurrentTick, .ReconnectToken = Player->Token}), ENetworkDelivery::Reliable);
	SendHistory(Connection.Id);
}

void FGameServer::FImplementation::SendHistory(const FNetworkConnectionId Connection)
{
	for (const auto& [Object, Outcome] : Breaks)
	{
		(void)Transport->Send(Connection, EncodeOutcome(Outcome), ENetworkDelivery::Reliable);
	}

	for (const FReplicationOutcome& Outcome : Cuts)
	{
		(void)Transport->Send(Connection, EncodeOutcome(Outcome), ENetworkDelivery::Reliable);
	}
}

void FGameServer::FImplementation::Acknowledge(FPlayer& Player, const std::uint32_t Sequence)
{
	const auto Sent = std::ranges::find(Player.Sent, Sequence, &FSent::Sequence);
	if (Sent == Player.Sent.end() || Sent->bAcked)
	{
		return;
	}

	Sent->bAcked = true;
	for (auto& [Id, State] : Sent->Entities)
	{
		const auto Known = Player.Known.find(Id);
		if (Known != Player.Known.end() && Sequence > Known->second.AckedSequence)
		{
			Known->second.AckedSequence = Sequence;
			Known->second.AckedState = State;
		}
	}

	for (auto Removal = Player.Removing.begin(); Removal != Player.Removing.end();)
	{
		if (Sequence >= Removal->second)
		{
			Player.Known.erase(Removal->first);
			Removal = Player.Removing.erase(Removal);
		}
		else
		{
			++Removal;
		}
	}
}

void FGameServer::FImplementation::HandleInput(FConnection& Connection, FPlayer& Player, const std::span<const std::byte> Message)
{
	const auto Input = DecodeInput(Message);
	if (!Input)
	{
		CountMalformed(Connection, Input.error().Message);
		return;
	}

	if (Input->AckedSnapshot != 0)
	{
		Acknowledge(Player, Input->AckedSnapshot);
		for (std::uint32_t Bit = 0; Bit < 32 && Bit + 1 < Input->AckedSnapshot; ++Bit)
		{
			if ((Input->AckMask & (1u << Bit)) != 0)
			{
				Acknowledge(Player, Input->AckedSnapshot - Bit - 1);
			}
		}
	}

	for (const FReplicationInputFrame& Frame : Input->Frames)
	{
		const bool bOld = Player.NextInputTick && Frame.Tick < *Player.NextInputTick;
		const bool bWild = Player.NextInputTick && Frame.Tick > *Player.NextInputTick + MaximumInputLead;
		if (!bOld && !bWild && Frame.Tick != 0)
		{
			Player.Pending.try_emplace(Frame.Tick, Frame.Input);
		}
	}
}

std::expected<void, FReplicationError> FGameServer::FImplementation::RemovePlayer(const std::uint32_t Id)
{
	const auto Found = std::ranges::find(Players, Id, &FPlayer::Id);
	if (Found == Players.end())
	{
		return {};
	}

	const std::size_t Slot = Found->Slot;
	Players.erase(Found);
	// The session always keeps one player, so the last one is parked instead of removed.
	if (Session->GetPlayerCount() == 1)
	{
		return Park(Slot);
	}

	if (auto Removed = Session->RemovePlayer(Slot); !Removed)
	{
		return std::unexpected(FReplicationError{Removed.error().Message});
	}

	SlotOwned.erase(SlotOwned.begin() + static_cast<std::ptrdiff_t>(Slot));
	for (FPlayer& Player : Players)
	{
		Player.Slot -= Player.Slot > Slot ? 1 : 0;
	}

	return {};
}

std::expected<void, FReplicationError> FGameServer::FImplementation::Receive()
{
	Events.clear();
	Messages.clear();
	Transport->Poll(Events, Messages);
	for (const FNetworkConnectionEvent& Event : Events)
	{
		if (Event.State == ENetworkConnectionState::Requested)
		{
			if (Transport->Accept(Event.Connection))
			{
				Connections.push_back({.Id = Event.Connection});
			}
		}
		else if (Event.State == ENetworkConnectionState::Connected)
		{
			if (std::ranges::find(Connections, Event.Connection, &FConnection::Id) == Connections.end())
			{
				Connections.push_back({.Id = Event.Connection});
			}
		}
		else
		{
			const auto Found = std::ranges::find(Connections, Event.Connection, &FConnection::Id);
			if (Found != Connections.end())
			{
				const auto& Owner = Found->Player;
				if (FPlayer* const Player = Owner ? FindPlayer(*Owner) : nullptr)
				{
					Player->Connection.reset();
					Player->AwaySeconds = 0.f;
				}

				Connections.erase(Found);
			}
		}
	}

	for (const FNetworkMessage& Message : Messages)
	{
		const auto Found = std::ranges::find(Connections, Message.Connection, &FConnection::Id);
		if (Found == Connections.end())
		{
			continue;
		}

		FConnection& Connection = *Found;
		const auto Type = PeekMessage(Message.Data);
		FPlayer* const Player = Connection.Player ? FindPlayer(*Connection.Player) : nullptr;
		if (Type == EReplicationMessage::Hello && !Player)
		{
			HandleHello(Connection, Message.Data);
		}
		else if (Type == EReplicationMessage::Input && Player)
		{
			HandleInput(Connection, *Player, Message.Data);
		}
		else
		{
			CountMalformed(Connection, "a message the server does not expect from this client now");
		}
	}

	std::erase_if(Connections, [](const FConnection& Connection)
	{
		return Connection.Id.Value == 0;
	});

	// Players whose clients dropped wait a while for them to reconnect, then leave.
	std::vector<std::uint32_t> Expired;
	for (FPlayer& Player : Players)
	{
		if (Player.Connection)
		{
			continue;
		}

		Player.AwaySeconds += TickSeconds;
		if (Player.AwaySeconds > Settings.ReconnectSeconds)
		{
			Expired.push_back(Player.Id);
		}
	}

	for (const std::uint32_t Id : Expired)
	{
		if (auto Removed = RemovePlayer(Id); !Removed)
		{
			return Removed;
		}
	}

	return {};
}

FGameInput FGameServer::FImplementation::TakeInput(FPlayer& Player)
{
	if (!Player.Connection)
	{
		Player.LastInput = {};
		return {};
	}

	if (!Player.NextInputTick && !Player.Pending.empty())
	{
		Player.NextInputTick = Player.Pending.begin()->first;
	}

	if (Player.Pending.size() > MaximumQueuedInputs)
	{
		Player.NextInputTick = std::prev(Player.Pending.end(), static_cast<std::ptrdiff_t>(MaximumQueuedInputs / 2))->first;
	}

	if (Player.NextInputTick)
	{
		std::erase_if(Player.Pending, [Next = *Player.NextInputTick](const auto& Entry)
		{
			return Entry.first < Next;
		});

		const auto Next = Player.Pending.find(*Player.NextInputTick);
		if (Next != Player.Pending.end())
		{
			Player.LastInput = Next->second;
			Player.AppliedInputTick = Next->first;
			Player.Pending.erase(Next);
			++*Player.NextInputTick;
			return Player.LastInput;
		}

		// A newer input arrived but this one never did: its redundant copies were lost too, so the timeline moves past it.
		if (!Player.Pending.empty())
		{
			++*Player.NextInputTick;
		}
	}

	++Stats.MissingInputs;
	return HoldInput(Player.LastInput);
}

std::expected<void, FReplicationError> FGameServer::Tick()
{
	FImplementation& Server = *Implementation;
	if (auto Received = Server.Receive(); !Received)
	{
		return Received;
	}

	std::vector<FGameInput> Inputs(Server.Session->GetPlayerCount());
	for (FImplementation::FPlayer& Player : Server.Players)
	{
		Inputs[Player.Slot] = Server.TakeInput(Player);
	}

	if (auto Updated = Server.Session->UpdatePlayers(TickSeconds, Inputs); !Updated)
	{
		return std::unexpected(FReplicationError{std::format("The game failed: {}", Updated.error().Message)});
	}

	++Server.CurrentTick;
	for (FGameOutcome& Outcome : Server.Session->TakeOutcomes())
	{
		FReplicationOutcome Replicated{.Tick = Server.CurrentTick, .Outcome = std::move(Outcome)};
		Server.SendOutcome(Replicated);
		if (Replicated.Outcome.Kind == EGameOutcome::Break)
		{
			Server.Breaks[Replicated.Outcome.Object] = std::move(Replicated);
		}
		else
		{
			Server.Cuts.push_back(std::move(Replicated));
		}
	}

	if (Server.CurrentTick % SnapshotIntervalTicks == 0)
	{
		const std::vector<FCandidate> Entities = Server.GatherEntities();
		for (FImplementation::FPlayer& Player : Server.Players)
		{
			if (Player.Connection)
			{
				Server.SendSnapshot(Player, Entities);
			}
		}
	}

	Server.Stats.Tick = Server.CurrentTick;
	Server.Stats.Players = Server.Players.size();
	Server.Stats.Connections = Server.Connections.size();
	return {};
}

void FGameServer::FImplementation::SendOutcome(const FReplicationOutcome& Outcome)
{
	const std::vector<std::byte> Message = EncodeOutcome(Outcome);
	for (const FPlayer& Player : Players)
	{
		if (Player.Connection)
		{
			(void)Transport->Send(*Player.Connection, Message, ENetworkDelivery::Reliable);
		}
	}
}

std::vector<FCandidate> FGameServer::FImplementation::GatherEntities() const
{
	std::vector<FCandidate> Entities;
	for (const FPlayer& Player : Players)
	{
		const FGamePlayerReplica Replica = Session->CapturePlayer(Player.Slot);
		Entities.push_back({.Id = Player.Id, .Kind = EReplicatedKind::Player, .Position = Replica.Mover.State.Foot, .State = EncodePlayer(Replica)});
	}

	for (const FLevelBodyReplica& Body : Session->CaptureBodies())
	{
		Entities.push_back({.Id = BodyEntityBase + Body.Body, .Kind = EReplicatedKind::Body, .Position = Body.Transform.Position, .State = EncodeBody(Body)});
	}

	for (const FGameProjectileReplica& Projectile : Session->CaptureProjectiles())
	{
		Entities.push_back({.Id = ProjectileEntityBase + Projectile.Id, .Kind = EReplicatedKind::Projectile, .Position = Projectile.Transform.Position, .State = EncodeProjectile(Projectile)});
	}

	for (const FGameNpcReplica& Npc : Session->CaptureNpcs())
	{
		Entities.push_back({.Id = NpcEntityBase + Npc.Character, .Kind = EReplicatedKind::Npc, .Position = Npc.Foot, .State = EncodeNpc(Npc)});
	}

	return Entities;
}

void FGameServer::FImplementation::SendSnapshot(FPlayer& Player, const std::span<const FCandidate> Entities)
{
	if (!Player.Connection)
	{
		return;
	}

	const FNetworkConnectionId Connection = *Player.Connection;
	const std::uint32_t Sequence = Player.NextSequence++;
	const FVector3 Viewer = Session->CapturePlayer(Player.Slot).Mover.State.Foot;
	const float RadiusSquared = Settings.RelevancyRadius * Settings.RelevancyRadius;

	// What the client has but should no longer: entities gone from the game or out of range.
	for (const auto& [Id, Known] : Player.Known)
	{
		const auto Current = std::ranges::find(Entities, Id, &FCandidate::Id);
		const bool bRelevant = Current != Entities.end() && (Id == Player.Id || (Current->Position - Viewer).LengthSquared() <= RadiusSquared);
		if (!bRelevant)
		{
			Player.Removing.try_emplace(Id, Sequence);
		}
	}

	// Relevant entities that changed since the state the client acknowledged, most important first.
	std::vector<std::pair<float, const FCandidate*>> Changed;
	for (const FCandidate& Entity : Entities)
	{
		const float DistanceSquared = (Entity.Position - Viewer).LengthSquared();
		if (Entity.Id != Player.Id && DistanceSquared > RadiusSquared)
		{
			continue;
		}

		// One that comes back into range may already be gone on the client, so it starts over from its whole state.
		if (Player.Removing.erase(Entity.Id) > 0)
		{
			Player.Known.erase(Entity.Id);
		}

		FKnown& Known = Player.Known.try_emplace(Entity.Id, FKnown{.Kind = Entity.Kind}).first->second;
		if (Known.AckedSequence != 0 && Known.AckedState == Entity.State)
		{
			Known.Priority = 0.f;
			continue;
		}

		Known.Priority += KindWeight(Entity.Kind) * (1.f + 20.f / (std::sqrt(DistanceSquared) + 1.f));
		Changed.emplace_back(Entity.Id == Player.Id ? std::numeric_limits<float>::max() : Known.Priority, &Entity);
	}

	std::ranges::stable_sort(Changed, std::ranges::greater{}, &std::pair<float, const FCandidate*>::first);

	FReplicationSnapshot Snapshot{.Sequence = Sequence, .Tick = CurrentTick, .PlayerId = Player.Id, .AppliedInputTick = Player.AppliedInputTick};
	for (const auto& [Id, First] : Player.Removing)
	{
		Snapshot.Removed.push_back(Id);
	}

	const FBaselineLookup Lookup = [&Player](const std::uint32_t Id, const std::uint32_t BaselineSequence) -> const std::vector<std::byte>*
	{
		const auto Known = Player.Known.find(Id);
		return Known != Player.Known.end() && Known->second.AckedSequence == BaselineSequence ? &Known->second.AckedState : nullptr;
	};

	FSent& Sent = Player.Sent.emplace_back(FSent{.Sequence = Sequence});
	const std::size_t Header = EncodeSnapshot({.Sequence = Sequence}, Lookup).size();
	std::size_t Used = 0;
	for (const auto& [Priority, Entity] : Changed)
	{
		FKnown& Known = Player.Known[Entity->Id];
		const bool bBaseline = Known.AckedSequence != 0 && Sequence - Known.AckedSequence <= SnapshotHistory;
		FReplicatedEntity Replicated{.Id = Entity->Id, .Kind = Entity->Kind, .State = Entity->State, .BaselineSequence = bBaseline ? Known.AckedSequence : 0};
		const std::size_t Cost = EncodeSnapshot({.Sequence = Sequence, .Entities = {Replicated}}, Lookup).size() - Header;
		// The client's own player always goes, whatever the budget, since prediction depends on it.
		if (Entity->Id != Player.Id && Used + Cost > Settings.SnapshotBudgetBytes)
		{
			++Stats.DeferredEntities;
			continue;
		}

		Used += Cost;
		Known.Priority = 0.f;
		Sent.Entities.emplace_back(Entity->Id, Entity->State);
		Snapshot.Entities.push_back(std::move(Replicated));
	}

	while (Player.Sent.size() > SnapshotHistory)
	{
		Player.Sent.pop_front();
	}

	const std::vector<std::byte> Message = EncodeSnapshot(Snapshot, Lookup);
	if (Transport->Send(Connection, Message, ENetworkDelivery::Unreliable))
	{
		++Stats.SnapshotsSent;
		Stats.SnapshotBytes += Message.size();
	}
}

std::expected<FNetworkConnectionId, FReplicationError> FGameServer::ConnectLocal(FNetworkTransport& Client)
{
	auto Pair = Implementation->Transport->ConnectLoopback(Client);
	if (!Pair)
	{
		return std::unexpected(FReplicationError{Pair.error().Message});
	}

	return Pair->second;
}

std::uint16_t FGameServer::GetPort() const noexcept
{
	return Implementation->Transport->GetListenPort();
}

FGameSession& FGameServer::GetSession() noexcept
{
	return *Implementation->Session;
}

std::optional<std::size_t> FGameServer::FindPlayerSlot(const std::uint32_t PlayerId) const
{
	const auto& Players = Implementation->Players;
	const auto Found = std::ranges::find(Players, PlayerId, &FImplementation::FPlayer::Id);
	return Found == Players.end() ? std::nullopt : std::optional(Found->Slot);
}

FGameServerStats FGameServer::GetStats() const noexcept
{
	return Implementation->Stats;
}
}
