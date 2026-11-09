#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
struct FIpcError
{
	std::string Message;
	// The other side closed the connection, as opposed to a failure on this side.
	bool bDisconnected = false;
};

// Messages larger than this are rejected before anything is allocated for them.
inline constexpr std::size_t MaximumIpcMessageBytes = std::size_t{16} * 1024 * 1024;

// A local byte stream between two processes of the same user, carrying whole messages: a little-endian 32-bit length, then that many bytes.
class FIpcConnection
{
public:
	// Connects to a listener's endpoint, waiting up to Timeout for it to accept.
	[[nodiscard]] static std::expected<FIpcConnection, FIpcError> Connect(std::string_view Endpoint, std::chrono::milliseconds Timeout);

	FIpcConnection(const FIpcConnection&) = delete;
	FIpcConnection(FIpcConnection&&) noexcept;
	FIpcConnection& operator=(const FIpcConnection&) = delete;
	FIpcConnection& operator=(FIpcConnection&&) noexcept;
	~FIpcConnection();

	[[nodiscard]] std::expected<void, FIpcError> Send(std::string_view Message, std::chrono::milliseconds Timeout = std::chrono::seconds(10));
	// Empty when no whole message arrives within Timeout; a later call continues where this one stopped.
	[[nodiscard]] std::expected<std::optional<std::string>, FIpcError> Receive(std::chrono::milliseconds Timeout);

	struct FImplementation;

	explicit FIpcConnection(std::unique_ptr<FImplementation> InImplementation) noexcept;

private:
	std::unique_ptr<FImplementation> Implementation;
};

// Accepts connections on a local endpoint that only processes of the same user can reach.
class FIpcListener
{
public:
	[[nodiscard]] static std::expected<FIpcListener, FIpcError> Listen(std::string_view Endpoint);

	FIpcListener(const FIpcListener&) = delete;
	FIpcListener(FIpcListener&&) noexcept;
	FIpcListener& operator=(const FIpcListener&) = delete;
	FIpcListener& operator=(FIpcListener&&) noexcept;
	~FIpcListener();

	// Empty when nobody connects within Timeout.
	[[nodiscard]] std::expected<std::optional<FIpcConnection>, FIpcError> Accept(std::chrono::milliseconds Timeout);

private:
	struct FImplementation;

	explicit FIpcListener(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};

// The platform address for a session name: a named pipe on Windows, a socket in the user's runtime directory on Linux.
[[nodiscard]] std::string MakeIpcEndpoint(std::string_view Name);
}
