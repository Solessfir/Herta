#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/Networking/NetworkTransport.h"
#include "Herta/Replication/ReplicationProtocol.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Herta
{
class FGameServer;

enum class EGameClientState : std::uint8_t
{
	Connecting,
	Playing,
	// The server turned the client away, such as for another protocol version or a full game; the reason says why.
	Rejected,
	// The connection dropped; Reconnect tries to take the same player back.
	Disconnected,
};

struct FGameClientSettings
{
	std::string Name = "Player";
	// Identifies the level and its content; the server refuses a client whose hash differs from its own.
	std::uint64_t ContentHash = 0;
	// Remote players and characters show this many ticks behind the newest snapshot, which hides late and lost ones.
	std::uint32_t InterpolationTicks = 6;
	// How far the server may place the local player from where prediction had it before the client corrects itself.
	float CorrectionDistance = 0.01f;
};

struct FGameClientStats
{
	std::uint64_t Snapshots = 0;
	std::uint64_t SnapshotBytes = 0;
	// Snapshots that arrived after a newer one, which only count toward acknowledgements.
	std::uint64_t LateSnapshots = 0;
	std::uint64_t Corrections = 0;
	std::uint64_t Outcomes = 0;
	// Messages from the server this client could not use.
	std::uint64_t Rejected = 0;
	float PingMilliseconds = 0.f;
};

// A player's view of a server's game. Its session simulates the local player ahead of the server from local input, and corrects that
// prediction by replaying unacknowledged input from each authoritative state it receives. Remote players, characters, and moving bodies come
// from snapshots; destruction and cuts arrive as outcomes.
class FGameClient final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FGameClient>, FReplicationError> Connect(std::unique_ptr<FGameSession> Session, std::string_view Address, const FGameClientSettings& Settings);
	// Joins a server in this process, such as a listen server's own player, through the same messages as a remote client.
	[[nodiscard]] static std::expected<std::unique_ptr<FGameClient>, FReplicationError> ConnectLocal(std::unique_ptr<FGameSession> Session, FGameServer& Server, const FGameClientSettings& Settings);

	~FGameClient();

	FGameClient(const FGameClient&) = delete;
	FGameClient& operator=(const FGameClient&) = delete;
	FGameClient(FGameClient&&) = delete;
	FGameClient& operator=(FGameClient&&) = delete;

	// One fixed tick: receives, predicts the local player with Input, and sends Input to the server.
	[[nodiscard]] std::expected<void, FReplicationError> Tick(const FGameInput& Input);
	// After a drop, connects again with the token the server gave, so it hands back the same player while it still holds it.
	[[nodiscard]] std::expected<void, FReplicationError> Reconnect();
	// Drops the connection, as a lost network would.
	void Disconnect();

	[[nodiscard]] EGameClientState GetState() const noexcept;
	[[nodiscard]] std::string_view GetStateReason() const noexcept;
	[[nodiscard]] std::uint32_t GetPlayerId() const noexcept;
	// The local player is always the session's first.
	[[nodiscard]] FGameSession& GetSession() noexcept;
	[[nodiscard]] FGameClientStats GetStats() const noexcept;

private:
	struct FImplementation;
	explicit FGameClient(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
