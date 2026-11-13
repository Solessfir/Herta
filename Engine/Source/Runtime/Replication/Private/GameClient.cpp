#include "Herta/Replication/GameClient.h"

#include "Herta/Replication/GameServer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <map>
#include <numbers>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr float TickSeconds = 1.f / static_cast<float>(ReplicationTicksPerSecond);
// Unacknowledged input kept for replay; a server that stops acknowledging for longer than this has lost the client anyway.
constexpr std::size_t MaximumUnacknowledgedInputs = 256;

[[nodiscard]] float LerpAngle(const float From, const float To, const float Alpha) noexcept
{
	return From + std::remainder(To - From, 2.f * std::numbers::pi_v<float>) * Alpha;
}

// A remote state between the two received around Tick, holding the newest when Tick is past it.
template <typename FReplica, typename FBlend>
[[nodiscard]] std::optional<FReplica> Sample(std::deque<std::pair<std::uint32_t, FReplica>>& States, const float Tick, FBlend&& Blend)
{
	if (States.empty())
	{
		return std::nullopt;
	}

	while (States.size() > 2 && static_cast<float>(States[1].first) <= Tick)
	{
		States.pop_front();
	}

	if (States.size() == 1 || static_cast<float>(States.front().first) >= Tick)
	{
		return States.front().second;
	}

	const auto& [FromTick, From] = States[0];
	const auto& [ToTick, To] = States[1];
	if (Tick >= static_cast<float>(ToTick))
	{
		return To;
	}

	const float Alpha = (Tick - static_cast<float>(FromTick)) / static_cast<float>(ToTick - FromTick);
	return Blend(From, To, Alpha);
}
}

void FTickInput::Add(const FGameInput& Frame) noexcept
{
	Pending.Move = Frame.Move;
	Pending.YawDelta += Frame.YawDelta;
	Pending.PitchDelta += Frame.PitchDelta;
	Pending.bSprint = Frame.bSprint;
	Pending.bCrouch = Frame.bCrouch;
	Pending.bBlock = Frame.bBlock;
	Pending.bJump |= Frame.bJump;
	Pending.bThrow |= Frame.bThrow;
	Pending.bToggleCamera |= Frame.bToggleCamera;
	Pending.bToggleRagdoll |= Frame.bToggleRagdoll;
	Pending.bAttack |= Frame.bAttack;
}

FGameInput FTickInput::Take() noexcept
{
	const FGameInput Tick = Pending;
	Pending = {.Move = Tick.Move, .bSprint = Tick.bSprint, .bCrouch = Tick.bCrouch, .bBlock = Tick.bBlock};
	return Tick;
}

struct FGameClient::FImplementation
{
	struct FRemote
	{
		std::size_t Slot = 0;
		std::deque<std::pair<std::uint32_t, FGamePlayerReplica>> States{};
		std::optional<FVector3> LastFoot{};
	};

	void Receive();
	void HandleWelcome(std::span<const std::byte> Message);
	void HandleSnapshot(std::span<const std::byte> Message);
	void Reconcile(const FGamePlayerReplica& Replica, std::uint32_t AppliedTick);
	void RemoveRemote(std::uint32_t Id);
	void ResetReplication();
	void Interpolate();
	void SendInput();
	[[nodiscard]] std::expected<void, FReplicationError> Open();

	std::unique_ptr<FGameSession> Session{};
	std::unique_ptr<FNetworkTransport> Transport{};
	FGameClientSettings Settings{};
	std::string Address{};
	FGameServer* LocalServer = nullptr;
	FNetworkConnectionId Connection{};
	EGameClientState State = EGameClientState::Connecting;
	std::string Reason{};
	std::uint32_t PlayerId = 0;
	std::uint64_t Token = 0;

	// Snapshot sequences received, for acknowledgements, and each entity's states by sequence for later deltas.
	std::uint32_t LatestSequence = 0;
	std::uint32_t AckMask = 0;
	std::map<std::uint32_t, std::deque<std::pair<std::uint32_t, std::vector<std::byte>>>> History{};
	// The server's tick as this client estimates it.
	std::uint32_t ServerTick = 0;

	std::uint32_t InputTick = 0;
	std::deque<FReplicationInputFrame> Inputs{};
	// Where prediction had the local player after each unacknowledged input.
	std::map<std::uint32_t, FVector3> Predicted{};

	std::map<std::uint32_t, FRemote> Remotes{};
	std::map<std::uint32_t, std::deque<std::pair<std::uint32_t, FGameNpcReplica>>> Npcs{};
	std::map<std::uint32_t, FGameProjectileReplica> Projectiles{};

	FGameClientStats Stats{};
	std::vector<FNetworkConnectionEvent> Events{};
	std::vector<FNetworkMessage> Messages{};
};

FGameClient::FGameClient(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameClient::~FGameClient()
{
	if (Implementation->Connection.Value != 0)
	{
		Implementation->Transport->Close(Implementation->Connection, "The client left");
	}
}

std::expected<void, FReplicationError> FGameClient::FImplementation::Open()
{
	auto Opened = LocalServer ? LocalServer->ConnectLocal(*Transport) : Transport->Connect(Address).transform_error([](const FNetworkError& Error)
	{
		return FReplicationError{Error.Message};
	});

	if (!Opened)
	{
		return std::unexpected(Opened.error());
	}

	Connection = *Opened;
	State = EGameClientState::Connecting;
	Reason.clear();
	return {};
}

std::expected<std::unique_ptr<FGameClient>, FReplicationError> FGameClient::Connect(std::unique_ptr<FGameSession> Session, const std::string_view Address, const FGameClientSettings& Settings)
{
	if (!Session)
	{
		return std::unexpected(FReplicationError{"A client needs a session"});
	}

	auto Transport = FNetworkTransport::Create();
	if (!Transport)
	{
		return std::unexpected(FReplicationError{Transport.error().Message});
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Session = std::move(Session);
	Implementation->Transport = std::move(*Transport);
	Implementation->Settings = Settings;
	Implementation->Address = Address;
	Implementation->Session->SetAuthority(false);
	if (auto Opened = Implementation->Open(); !Opened)
	{
		return std::unexpected(Opened.error());
	}

	return std::unique_ptr<FGameClient>(new FGameClient(std::move(Implementation)));
}

std::expected<std::unique_ptr<FGameClient>, FReplicationError> FGameClient::ConnectLocal(std::unique_ptr<FGameSession> Session, FGameServer& Server, const FGameClientSettings& Settings)
{
	if (!Session)
	{
		return std::unexpected(FReplicationError{"A client needs a session"});
	}

	auto Transport = FNetworkTransport::Create();
	if (!Transport)
	{
		return std::unexpected(FReplicationError{Transport.error().Message});
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Session = std::move(Session);
	Implementation->Transport = std::move(*Transport);
	Implementation->Settings = Settings;
	Implementation->LocalServer = &Server;
	Implementation->Session->SetAuthority(false);
	if (auto Opened = Implementation->Open(); !Opened)
	{
		return std::unexpected(Opened.error());
	}

	return std::unique_ptr<FGameClient>(new FGameClient(std::move(Implementation)));
}

void FGameClient::FImplementation::ResetReplication()
{
	LatestSequence = 0;
	AckMask = 0;
	History.clear();
	InputTick = 0;
	Inputs.clear();
	Predicted.clear();
	while (!Remotes.empty())
	{
		RemoveRemote(Remotes.begin()->first);
	}

	Npcs.clear();
}

void FGameClient::FImplementation::RemoveRemote(const std::uint32_t Id)
{
	const auto Found = Remotes.find(Id);
	if (Found == Remotes.end())
	{
		return;
	}

	const std::size_t Slot = Found->second.Slot;
	Remotes.erase(Found);
	if (Session->RemovePlayer(Slot))
	{
		for (auto& [Other, Remote] : Remotes)
		{
			Remote.Slot -= Remote.Slot > Slot ? 1 : 0;
		}
	}
}

void FGameClient::FImplementation::HandleWelcome(const std::span<const std::byte> Message)
{
	const auto Welcome = DecodeWelcome(Message);
	if (!Welcome)
	{
		++Stats.Rejected;
		return;
	}

	ResetReplication();
	PlayerId = Welcome->PlayerId;
	Token = Welcome->ReconnectToken;
	ServerTick = Welcome->Tick;
	State = EGameClientState::Playing;
}

void FGameClient::FImplementation::HandleSnapshot(const std::span<const std::byte> Message)
{
	const FBaselineLookup Lookup = [this](const std::uint32_t Id, const std::uint32_t Sequence) -> const std::vector<std::byte>*
	{
		const auto Entity = History.find(Id);
		if (Entity == History.end())
		{
			return nullptr;
		}

		const auto Found = std::ranges::find(Entity->second, Sequence, &std::pair<std::uint32_t, std::vector<std::byte>>::first);
		return Found == Entity->second.end() ? nullptr : &Found->second;
	};

	auto Snapshot = DecodeSnapshot(Message, Lookup);
	if (!Snapshot || Snapshot->PlayerId != PlayerId)
	{
		++Stats.Rejected;
		return;
	}

	const std::uint32_t Sequence = Snapshot->Sequence;
	if (Sequence == LatestSequence || (LatestSequence > Sequence && LatestSequence - Sequence > 32))
	{
		return;
	}

	const bool bLate = Sequence < LatestSequence;
	if (bLate)
	{
		AckMask |= 1u << (LatestSequence - Sequence - 1);
		++Stats.LateSnapshots;
	}
	else
	{
		const std::uint32_t Shift = Sequence - LatestSequence;
		AckMask = Shift >= 32 ? 0 : (AckMask << Shift) | (LatestSequence != 0 ? 1u << (Shift - 1) : 0u);
		LatestSequence = Sequence;
	}

	++Stats.Snapshots;
	Stats.SnapshotBytes += Message.size();
	for (const FReplicatedEntity& Entity : Snapshot->Entities)
	{
		auto& States = History[Entity.Id];
		const auto Position = std::ranges::upper_bound(States, Sequence, {}, &std::pair<std::uint32_t, std::vector<std::byte>>::first);
		States.insert(Position, {Sequence, Entity.State});
		while (!States.empty() && LatestSequence - States.front().first > SnapshotHistory)
		{
			States.pop_front();
		}
	}

	// An older snapshot still serves as a baseline, but the newer one already placed everything.
	if (bLate)
	{
		return;
	}

	if (Snapshot->Tick > ServerTick || ServerTick - Snapshot->Tick > 2 * SnapshotIntervalTicks)
	{
		ServerTick = Snapshot->Tick;
	}

	for (const std::uint32_t Id : Snapshot->Removed)
	{
		History.erase(Id);
		RemoveRemote(Id);
		Npcs.erase(Id);
		Projectiles.erase(Id);
	}

	std::vector<FLevelBodyReplica> Bodies;
	for (const FReplicatedEntity& Entity : Snapshot->Entities)
	{
		switch (Entity.Kind)
		{
			case EReplicatedKind::Player:
			{
				auto Player = DecodePlayer(Entity.State);
				if (!Player)
				{
					++Stats.Rejected;
					break;
				}

				if (Entity.Id == PlayerId)
				{
					Reconcile(*Player, Snapshot->AppliedInputTick);
					break;
				}

				auto Remote = Remotes.find(Entity.Id);
				if (Remote == Remotes.end())
				{
					auto Added = Session->AddPlayer();
					if (!Added || !Session->SetPlayerSimulated(*Added, false))
					{
						++Stats.Rejected;
						break;
					}

					Remote = Remotes.emplace(Entity.Id, FRemote{.Slot = *Added}).first;
				}

				Remote->second.States.emplace_back(Snapshot->Tick, std::move(*Player));
				break;
			}
			case EReplicatedKind::Body:
			{
				if (auto Body = DecodeBody(Entity.State))
				{
					Bodies.push_back(*Body);
				}
				else
				{
					++Stats.Rejected;
				}

				break;
			}
			case EReplicatedKind::Projectile:
			{
				if (auto Projectile = DecodeProjectile(Entity.State))
				{
					Projectiles[Entity.Id] = *Projectile;
				}
				else
				{
					++Stats.Rejected;
				}

				break;
			}
			case EReplicatedKind::Npc:
			{
				if (auto Npc = DecodeNpc(Entity.State))
				{
					Npcs[Entity.Id].emplace_back(Snapshot->Tick, *Npc);
				}
				else
				{
					++Stats.Rejected;
				}

				break;
			}
		}
	}

	if (!Bodies.empty() && !Session->ApplyBodies(Bodies))
	{
		++Stats.Rejected;
	}

	std::vector<FGameProjectileReplica> Balls;
	Balls.reserve(Projectiles.size());
	for (const auto& [Id, Ball] : Projectiles)
	{
		Balls.push_back(Ball);
	}

	if (!Session->ApplyProjectiles(Balls))
	{
		++Stats.Rejected;
	}
}

void FGameClient::FImplementation::Reconcile(const FGamePlayerReplica& Replica, const std::uint32_t AppliedTick)
{
	while (!Inputs.empty() && Inputs.front().Tick <= AppliedTick)
	{
		Inputs.pop_front();
	}

	const auto Prediction = Predicted.find(AppliedTick);
	const bool bPredicted = Prediction != Predicted.end();
	const bool bClose = bPredicted && (Prediction->second - Replica.Mover.State.Foot).Length() <= Settings.CorrectionDistance;
	const FGamePlayerReplica Local = Session->CapturePlayer(0);
	const bool bSameRun = Local.Objective.State == Replica.Objective.State && Local.Objective.Respawns == Replica.Objective.Respawns && Local.bRagdolled == Replica.bRagdolled;
	std::erase_if(Predicted, [AppliedTick](const auto& Entry)
	{
		return Entry.first <= AppliedTick;
	});

	if (bClose && bSameRun)
	{
		return;
	}

	// The server disagrees: start from its state at the applied input and replay every input it has not applied yet.
	++Stats.Corrections;
	if (!Session->ApplyPlayer(0, Replica))
	{
		++Stats.Rejected;
		return;
	}

	for (const FReplicationInputFrame& Frame : Inputs)
	{
		if (!Session->ReplayPlayer(0, std::span(&Frame.Input, 1)))
		{
			++Stats.Rejected;
			return;
		}

		Predicted[Frame.Tick] = Session->GetPlayerFootPosition();
	}
}

void FGameClient::FImplementation::Receive()
{
	Events.clear();
	Messages.clear();
	Transport->Poll(Events, Messages);
	for (const FNetworkConnectionEvent& Event : Events)
	{
		if (Event.Connection != Connection)
		{
			continue;
		}

		if (Event.State == ENetworkConnectionState::Connected)
		{
			(void)Transport->Send(Connection, EncodeHello({.ContentHash = Settings.ContentHash, .ReconnectToken = Token, .Name = Settings.Name}), ENetworkDelivery::Reliable);
		}
		else if (Event.State == ENetworkConnectionState::Closed)
		{
			if (State != EGameClientState::Rejected)
			{
				State = EGameClientState::Disconnected;
				Reason = Event.Reason;
			}

			Connection = {};
		}
	}

	for (const FNetworkMessage& Message : Messages)
	{
		if (Message.Connection != Connection)
		{
			continue;
		}

		switch (PeekMessage(Message.Data).value_or(EReplicationMessage::Hello))
		{
			case EReplicationMessage::Welcome:
				HandleWelcome(Message.Data);
				break;
			case EReplicationMessage::Reject:
				State = EGameClientState::Rejected;
				Reason = DecodeReject(Message.Data).transform([](const FReplicationReject& Reject)
				{
					return Reject.Reason;
				}).value_or("The server refused the client");
				break;
			case EReplicationMessage::Snapshot:
				if (State == EGameClientState::Playing)
				{
					HandleSnapshot(Message.Data);
				}

				break;
			case EReplicationMessage::Outcome:
			{
				const auto Outcome = DecodeOutcome(Message.Data);
				if (State == EGameClientState::Playing && Outcome && Session->ApplyOutcome(Outcome->Outcome))
				{
					++Stats.Outcomes;
				}
				else
				{
					++Stats.Rejected;
				}

				break;
			}
			default:
				++Stats.Rejected;
				break;
		}
	}
}

void FGameClient::FImplementation::Interpolate()
{
	const float RenderTick = static_cast<float>(ServerTick) - static_cast<float>(Settings.InterpolationTicks);
	for (auto& [Id, Remote] : Remotes)
	{
		auto Replica = Sample(Remote.States, RenderTick, [](const FGamePlayerReplica& From, const FGamePlayerReplica& To, const float Alpha)
		{
			FGamePlayerReplica Blended = From;
			FMoverState& Mover = Blended.Mover.State;
			Mover.Foot = From.Mover.State.Foot * (1.f - Alpha) + To.Mover.State.Foot * Alpha;
			Mover.Velocity = From.Mover.State.Velocity * (1.f - Alpha) + To.Mover.State.Velocity * Alpha;
			Blended.Yaw = LerpAngle(From.Yaw, To.Yaw, Alpha);
			Blended.Pitch = From.Pitch + (To.Pitch - From.Pitch) * Alpha;
			Blended.Attacks = To.Attacks;
			return Blended;
		});

		if (!Replica)
		{
			continue;
		}

		Replica->Mover.State.PreviousFoot = Remote.LastFoot.value_or(Replica->Mover.State.Foot);
		Remote.LastFoot = Replica->Mover.State.Foot;
		if (!Session->ApplyPlayer(Remote.Slot, *Replica))
		{
			++Stats.Rejected;
		}
	}

	std::vector<FGameNpcReplica> Characters;
	for (auto& [Id, States] : Npcs)
	{
		auto Replica = Sample(States, RenderTick, [](const FGameNpcReplica& From, const FGameNpcReplica& To, const float Alpha)
		{
			FGameNpcReplica Blended = To;
			Blended.Foot = From.Foot * (1.f - Alpha) + To.Foot * Alpha;
			Blended.Yaw = LerpAngle(From.Yaw, To.Yaw, Alpha);
			Blended.GroundSpeed = From.GroundSpeed + (To.GroundSpeed - From.GroundSpeed) * Alpha;
			return Blended;
		});

		if (Replica)
		{
			Characters.push_back(*Replica);
		}
	}

	Session->ApplyNpcs(Characters);
}

void FGameClient::FImplementation::SendInput()
{
	FReplicationInput Message{.AckedSnapshot = LatestSequence, .AckMask = AckMask};
	const std::size_t First = Inputs.size() - std::min(Inputs.size(), InputRedundancy);
	Message.Frames.assign(Inputs.begin() + static_cast<std::ptrdiff_t>(First), Inputs.end());
	(void)Transport->Send(Connection, EncodeInput(Message), ENetworkDelivery::Unreliable);
}

std::expected<void, FReplicationError> FGameClient::Tick(const FGameInput& Input)
{
	FImplementation& Client = *Implementation;
	Client.Receive();
	if (Client.State != EGameClientState::Playing)
	{
		return {};
	}

	Client.Inputs.push_back({.Tick = ++Client.InputTick, .Input = Input});
	while (Client.Inputs.size() > MaximumUnacknowledgedInputs)
	{
		Client.Inputs.pop_front();
	}

	Client.SendInput();
	Client.Interpolate();
	std::vector<FGameInput> Inputs(Client.Session->GetPlayerCount());
	Inputs[0] = Input;
	if (auto Updated = Client.Session->UpdatePlayers(TickSeconds, Inputs); !Updated)
	{
		return std::unexpected(FReplicationError{std::format("The game failed: {}", Updated.error().Message)});
	}

	Client.Predicted[Client.InputTick] = Client.Session->GetPlayerFootPosition();
	++Client.ServerTick;
	Client.Stats.PingMilliseconds = Client.Transport->GetStats(Client.Connection).PingMilliseconds;
	return {};
}

std::expected<void, FReplicationError> FGameClient::Reconnect()
{
	if (Implementation->State != EGameClientState::Disconnected)
	{
		return std::unexpected(FReplicationError{"Only a disconnected client reconnects"});
	}

	return Implementation->Open();
}

void FGameClient::Disconnect()
{
	FImplementation& Client = *Implementation;
	if (Client.Connection.Value != 0)
	{
		Client.Transport->Close(Client.Connection, "The client disconnected");
		Client.Connection = {};
	}

	Client.State = EGameClientState::Disconnected;
	Client.Reason = "The client disconnected";
}

EGameClientState FGameClient::GetState() const noexcept
{
	return Implementation->State;
}

std::string_view FGameClient::GetStateReason() const noexcept
{
	return Implementation->Reason;
}

std::uint32_t FGameClient::GetPlayerId() const noexcept
{
	return Implementation->PlayerId;
}

FGameSession& FGameClient::GetSession() noexcept
{
	return *Implementation->Session;
}

FGameClientStats FGameClient::GetStats() const noexcept
{
	return Implementation->Stats;
}
}
