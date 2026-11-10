#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Herta
{
struct FNetworkError
{
	std::string Message;
};

struct FNetworkConnectionId
{
	auto operator<=>(const FNetworkConnectionId&) const = default;

	std::uint32_t Value = 0;
};

enum class ENetworkDelivery : std::uint8_t
{
	// May arrive late, out of order, or not at all; the newest state replaces it anyway.
	Unreliable,
	// Arrives once and in order with other reliable messages on the connection.
	Reliable,
};

enum class ENetworkConnectionState : std::uint8_t
{
	// A peer asked to connect to a listening transport; Accept or Close it.
	Requested,
	Connected,
	// Closed by either side or lost, with the reason in the event.
	Closed,
};

struct FNetworkConnectionEvent
{
	FNetworkConnectionId Connection{};
	ENetworkConnectionState State = ENetworkConnectionState::Connected;
	std::string Reason{};
};

struct FNetworkMessage
{
	FNetworkConnectionId Connection{};
	std::vector<std::byte> Data{};
};

// Simulated network trouble, applied process-wide to every connection, for tests and for trying a game on a bad network.
struct FNetworkConditions
{
	bool operator==(const FNetworkConditions&) const = default;

	float LossPercent = 0.f;
	float LagMilliseconds = 0.f;
	// Extra delay added to some packets, up to this many milliseconds, on top of the lag.
	float JitterMilliseconds = 0.f;
	float ReorderPercent = 0.f;
	float DuplicatePercent = 0.f;
	// Zero leaves the rate unlimited.
	float RateLimitBytesPerSecond = 0.f;
};

struct FNetworkConnectionStats
{
	float PingMilliseconds = 0.f;
	// From 0 to 1, the share of packets that arrived.
	float Quality = 1.f;
	float SentBytesPerSecond = 0.f;
	float ReceivedBytesPerSecond = 0.f;
	std::uint32_t PendingReliableBytes = 0;
};

// Encrypted messages over UDP, through Valve's GameNetworkingSockets. Everything arrives through Poll on the thread that owns the transport.
// One transport can listen and connect at once; a listen server's own player connects through a loopback pair, so it goes through the same
// messages as everyone else.
class FNetworkTransport final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FNetworkTransport>, FNetworkError> Create();

	~FNetworkTransport();

	FNetworkTransport(const FNetworkTransport&) = delete;
	FNetworkTransport& operator=(const FNetworkTransport&) = delete;
	FNetworkTransport(FNetworkTransport&&) = delete;
	FNetworkTransport& operator=(FNetworkTransport&&) = delete;

	// Listens on every local address at Port; zero lets the system pick, which GetListenPort reports.
	[[nodiscard]] std::expected<void, FNetworkError> Listen(std::uint16_t Port);
	[[nodiscard]] std::uint16_t GetListenPort() const;
	// Connects to "host:port", such as "127.0.0.1:27015" or "[::1]:27015".
	[[nodiscard]] std::expected<FNetworkConnectionId, FNetworkError> Connect(std::string_view Address);
	// Two connected ends in this process that pass through the same messages, encryption, and simulated conditions as a real connection.
	[[nodiscard]] std::expected<std::pair<FNetworkConnectionId, FNetworkConnectionId>, FNetworkError> ConnectLoopback();
	[[nodiscard]] std::expected<void, FNetworkError> Accept(FNetworkConnectionId Connection);
	void Close(FNetworkConnectionId Connection, std::string_view Reason);

	[[nodiscard]] std::expected<void, FNetworkError> Send(FNetworkConnectionId Connection, std::span<const std::byte> Data, ENetworkDelivery Delivery);
	// Delivers connection changes and messages that arrived since the last call.
	void Poll(std::vector<FNetworkConnectionEvent>& Events, std::vector<FNetworkMessage>& Messages);

	[[nodiscard]] FNetworkConnectionStats GetStats(FNetworkConnectionId Connection) const;
	static void SetConditions(const FNetworkConditions& Conditions);

private:
	struct FImplementation;
	explicit FNetworkTransport(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
