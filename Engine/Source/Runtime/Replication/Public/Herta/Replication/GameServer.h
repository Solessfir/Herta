#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/Networking/NetworkTransport.h"
#include "Herta/Replication/ReplicationProtocol.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace Herta
{
struct FGameServerSettings
{
	// Zero picks a free port, which GetPort reports.
	std::uint16_t Port = 27015;
	std::size_t MaximumPlayers = 8;
	// Identifies the level and its content; clients with another hash are turned away.
	std::uint64_t ContentHash = 0;
	// Entities farther than this from a client's player are not sent to it.
	float RelevancyRadius = 80.f;
	// Entity state bytes in one snapshot, so a snapshot stays one UDP packet. Entities that do not fit wait for the next one with a higher
	// priority.
	std::size_t SnapshotBudgetBytes = 1100;
	// How long a dropped player's character waits for its client to reconnect.
	float ReconnectSeconds = 30.f;
	// Malformed messages a connection may send before it is dropped.
	std::uint32_t MalformedLimit = 16;
};

struct FGameServerStats
{
	std::uint32_t Tick = 0;
	std::size_t Players = 0;
	std::size_t Connections = 0;
	std::uint64_t SnapshotsSent = 0;
	std::uint64_t SnapshotBytes = 0;
	// Entities that changed but waited for a later snapshot because of the budget.
	std::uint64_t DeferredEntities = 0;
	std::uint64_t MalformedMessages = 0;
	std::uint64_t RejectedClients = 0;
	std::uint64_t Reconnects = 0;
	// Ticks a player's input had not arrived for, which repeated its last held input.
	std::uint64_t MissingInputs = 0;
};

// The authoritative game: it steps the one session every client plays, at a fixed 60 ticks a second, with each player's input for the tick,
// and sends each client snapshots of what is near its player. A dedicated server runs it alone; a listen server runs it beside a client in the
// same process, whose own player connects through a loopback pair like everyone else.
class FGameServer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FGameServer>, FReplicationError> Create(std::unique_ptr<FGameSession> Session, const FGameServerSettings& Settings);

	~FGameServer();

	FGameServer(const FGameServer&) = delete;
	FGameServer& operator=(const FGameServer&) = delete;
	FGameServer(FGameServer&&) = delete;
	FGameServer& operator=(FGameServer&&) = delete;

	// One fixed tick: receives, steps the session, sends outcomes, and on every third tick snapshots.
	[[nodiscard]] std::expected<void, FReplicationError> Tick();
	// Joins Client, a transport in this process, to the server through a loopback pair, and returns Client's end.
	[[nodiscard]] std::expected<FNetworkConnectionId, FReplicationError> ConnectLocal(FNetworkTransport& Client);
	[[nodiscard]] std::uint16_t GetPort() const noexcept;
	[[nodiscard]] FGameSession& GetSession() noexcept;
	// The session player a client's player id plays, while the server holds it.
	[[nodiscard]] std::optional<std::size_t> FindPlayerSlot(std::uint32_t PlayerId) const;
	[[nodiscard]] FGameServerStats GetStats() const noexcept;

private:
	struct FImplementation;
	explicit FGameServer(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
