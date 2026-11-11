#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
// Forwards one tool call to the editor and returns its raw automation reply, or a message saying why the editor could not be reached.
using FMcpToolCall = std::function<std::expected<std::string, std::string>(std::string_view Tool, std::string_view Arguments)>;

// Answers one line of MCP JSON-RPC from a client: initialize, ping, tools/list, and tools/call. Returns the one-line reply to write, or
// nothing for notifications and lines that are not requests.
[[nodiscard]] std::optional<std::string> HandleMcpMessage(std::string_view Line, const FMcpToolCall& Call);
// The automation process a tool call is for: its arguments' target, such as game, server, or client-2, or editor when it names none.
[[nodiscard]] std::string ReadMcpTarget(std::string_view Arguments);
}
