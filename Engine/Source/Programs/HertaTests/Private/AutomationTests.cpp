#include "Herta/EditorCore/Automation.h"
#include "Herta/Platform/Ipc.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <chrono>
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
}
