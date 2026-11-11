#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FAutomationError
{
	std::string Message;
};

// One tool call from an automation client. Arguments is the call's JSON object, as text.
struct FAutomationRequest
{
	std::uint64_t Id = 0;
	std::string Tool{};
	std::string Arguments{};
};

struct FAutomationSettings
{
	// Holds Session.json, which tells clients where to connect, and Audit.log.
	std::filesystem::path SessionDirectory{};
	// Read-only sessions refuse tools that change the level, content, or files.
	bool bAllowWrites = false;
	// A request the editor has not answered by then fails with a timeout, so a stalled frame never hangs the client.
	std::chrono::milliseconds RequestTimeout{30'000};
};

// Where a running editor accepts automation connections.
struct FAutomationSession
{
	std::string Endpoint{};
	std::string Token{};
	bool bAllowWrites = false;
	std::uint32_t ProcessId = 0;
};

// The editor's automation endpoint. A background thread accepts one local client at a time, checks its session token, and queues its tool calls;
// the editor takes them on its main thread at a safe point and answers when ready, which may be a later frame.
//
// Protocol, one JSON object per IPC message:
//   client: {"token": "..."}                                   server: {"ok": true, "write": false}
//   client: {"id": 1, "tool": "name", "arguments": {...}}      server: {"id": 1, "result": <JSON>} or {"id": 1, "error": {"code": "...", "message": "..."}}
class FAutomationServer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FAutomationServer>, FAutomationError> Create(const FAutomationSettings& Settings);

	FAutomationServer(const FAutomationServer&) = delete;
	FAutomationServer(FAutomationServer&&) = delete;
	FAutomationServer& operator=(const FAutomationServer&) = delete;
	FAutomationServer& operator=(FAutomationServer&&) = delete;
	~FAutomationServer();

	// Requests received since the previous call, in arrival order.
	[[nodiscard]] std::vector<FAutomationRequest> TakeRequests();
	// ResultJson must be one JSON value.
	void Respond(std::uint64_t Id, std::string ResultJson);
	void Fail(std::uint64_t Id, std::string_view Code, std::string_view Message);

	[[nodiscard]] bool AllowsWrites() const noexcept;
	[[nodiscard]] const std::filesystem::path& GetSessionFile() const noexcept;

private:
	struct FImplementation;

	explicit FAutomationServer(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};

[[nodiscard]] std::expected<FAutomationSession, FAutomationError> ReadAutomationSession(const std::filesystem::path& SessionFile);
// Where game, server, and client automation sessions register, one folder per process name: the user's settings directory, so the MCP
// server finds a packaged game wherever it runs. The editor keeps its session in the repository's Saved/Automation.
[[nodiscard]] std::filesystem::path GetAutomationSessionsDirectory();
}
