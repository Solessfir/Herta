#include "Herta/EditorCore/Automation.h"
#include "Herta/EditorCore/Mcp.h"
#include "Herta/Platform/Ipc.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

namespace Herta
{
namespace
{
using namespace std::chrono_literals;

std::optional<std::string> Exchange(FIpcConnection& Connection, const std::string_view Request, FAutomationServer* const Server = nullptr)
{
	REQUIRE(Connection.Send(Request));
	for (int Attempt = 0; Attempt < 300; ++Attempt)
	{
		if (Server)
		{
			for (const FAutomationRequest& Pending : Server->TakeRequests())
			{
				if (Pending.Tool == "echo")
				{
					Server->Respond(Pending.Id, Pending.Arguments);
				}
				else if (Pending.Tool == "broken")
				{
					Server->Fail(Pending.Id, "broken", "This tool always fails");
				}
			}
		}

		const auto Reply = Connection.Receive(10ms);
		REQUIRE(Reply);
		if (*Reply)
		{
			return **Reply;
		}
	}

	return std::nullopt;
}
}

TEST_CASE("The automation server checks its token, queues tool calls for the editor, and times out unanswered ones")
{
	Tests::FScratchDirectory Scratch("HertaAutomation");
	auto Server = FAutomationServer::Create({.SessionDirectory = Scratch.GetPath(), .bAllowWrites = false, .RequestTimeout = 300ms});
	REQUIRE_MESSAGE(Server, (Server ? std::string() : Server.error().Message));
	const auto Session = ReadAutomationSession((*Server)->GetSessionFile());
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	CHECK_FALSE(Session->bAllowWrites);
	CHECK(Session->Token.size() == 64);

	// A wrong token is refused before any tool runs.
	{
		auto Intruder = FIpcConnection::Connect(Session->Endpoint, 2s);
		REQUIRE(Intruder);
		const auto Refused = Exchange(*Intruder, R"({"token": "nope"})");
		REQUIRE(Refused);
		CHECK(Refused->find("unauthorized") != std::string::npos);
	}

	auto Client = FIpcConnection::Connect(Session->Endpoint, 2s);
	REQUIRE(Client);
	const auto Hello = Exchange(*Client, std::string(R"({"token": ")") + Session->Token + "\"}");
	REQUIRE(Hello);
	CHECK(*Hello == R"({"ok": true, "write": false})");

	const auto Echoed = Exchange(*Client, R"({"id": 7, "tool": "echo", "arguments": {"frames": 3, "label": "a\"b"}})", Server->get());
	REQUIRE(Echoed);
	CHECK(*Echoed == R"({"id": 7, "result": {"frames":3,"label":"a\"b"}})");

	const auto Broken = Exchange(*Client, R"({"id": 8, "tool": "broken"})", Server->get());
	REQUIRE(Broken);
	CHECK(*Broken == R"({"id": 8, "error": {"code": "broken", "message": "This tool always fails"}})");

	const auto Malformed = Exchange(*Client, R"({"tool": "echo"})", Server->get());
	REQUIRE(Malformed);
	CHECK(Malformed->find("invalid_request") != std::string::npos);

	// Nothing answers "slow", so the server gives up on the editor's behalf.
	const auto Slow = Exchange(*Client, R"({"id": 9, "tool": "slow", "arguments": {}})", Server->get());
	REQUIRE(Slow);
	CHECK(Slow->find("\"timeout\"") != std::string::npos);

	const std::filesystem::path SessionFile = (*Server)->GetSessionFile();
	Server->reset();
	CHECK_FALSE(std::filesystem::exists(SessionFile));
	CHECK_FALSE(ReadAutomationSession(SessionFile));
	CHECK(std::filesystem::exists(Scratch.GetPath() / "Audit.log"));
}

TEST_CASE("MCP messages get one-line replies that forward tool calls and return captures as images")
{
	Tests::FScratchDirectory Scratch("HertaMcp");
	const std::filesystem::path Png = Scratch.GetPath() / "capture.png";
	{
		std::ofstream File(Png, std::ios::binary);
		File << "PNGBYTES";
	}

	std::vector<std::pair<std::string, std::string>> Calls;
	const FMcpToolCall Call = [&](const std::string_view Tool, const std::string_view Arguments) -> std::expected<std::string, std::string>
	{
		Calls.emplace_back(Tool, Arguments);
		if (Tool == "viewport_capture")
		{
			std::string Path = Png.generic_string();
			return std::string(R"({"id": 1, "result": {"image": ")") + Path + R"(", "width": 2, "height": 2}})";
		}

		if (Tool == "broken")
		{
			return std::string(R"({"id": 1, "error": {"code": "no_level", "message": "No level is open"}})");
		}

		if (Tool == "offline")
		{
			return std::unexpected(std::string("No editor is running"));
		}

		return std::string(R"({"id": 1, "result": {"ok": true}})");
	};

	const auto Single = [](const std::optional<std::string>& Reply)
	{
		REQUIRE(Reply);
		CHECK(Reply->find('\n') == std::string::npos);
		return *Reply;
	};

	const std::string Initialized = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}})", Call));
	CHECK(Initialized.find(R"("protocolVersion": "2025-06-18")") != std::string::npos);
	CHECK(Initialized.find("herta-editor") != std::string::npos);
	CHECK_FALSE(HandleMcpMessage(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})", Call));
	CHECK_FALSE(HandleMcpMessage("not json", Call));

	const std::string Listed = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": "list", "method": "tools/list"})", Call));
	CHECK(Listed.find(R"("id": "list")") != std::string::npos);
	for (const char* const Name : {"editor_status", "editor_log", "level_entities", "viewport_capture", "gpu_timings", "play_start", "play_stop", "play_state", "play_input", "play_teleport"})
	{
		CHECK(Listed.find(std::string("\"") + Name + "\"") != std::string::npos);
	}

	const std::string Status = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "play_input", "arguments": {"frames": 30, "move": [0, 1]}}})", Call));
	REQUIRE(Calls.size() == 1);
	CHECK(Calls[0].first == "play_input");
	CHECK(Calls[0].second == R"({"frames":30,"move":[0,1]})");
	CHECK(Status.find(R"("isError": false)") != std::string::npos);

	const std::string Captured = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "viewport_capture"}})", Call));
	CHECK(Calls.back().second == "{}");
	CHECK(Captured.find(R"("type": "image", "mimeType": "image/png", "data": "UE5HQllURVM=")") != std::string::npos);

	const std::string Failed = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "broken"}})", Call));
	CHECK(Failed.find(R"("isError": true)") != std::string::npos);
	CHECK(Failed.find("No level is open") != std::string::npos);

	const std::string Offline = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "offline"}})", Call));
	CHECK(Offline.find("No editor is running") != std::string::npos);

	const std::string Unknown = Single(HandleMcpMessage(R"({"jsonrpc": "2.0", "id": 6, "method": "resources/list"})", Call));
	CHECK(Unknown.find("-32601") != std::string::npos);
}
}
