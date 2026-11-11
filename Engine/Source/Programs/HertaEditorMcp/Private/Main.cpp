// An MCP server over stdio for coding agents. It forwards tool calls to a running Herta started with --automation, which it finds through
// Saved/Automation/Session.json, or to the game, server, or client a call's target names, each registered under the user's automation
// sessions directory. It never edits files or runs commands itself.

#include "Herta/Core/Json.h"
#include "Herta/EditorCore/Automation.h"
#include "Herta/EditorCore/Mcp.h"
#include "Herta/Platform/Ipc.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <fcntl.h>
	#include <io.h>
#endif

namespace Herta
{
namespace
{
using namespace std::chrono_literals;

// The repository holds Saved/Automation; HERTA_ROOT overrides the search from the executable's folder.
[[nodiscard]] std::filesystem::path FindSessionFile()
{
	if (const std::optional<std::string> Root = ReadEnvironment("HERTA_ROOT"))
	{
		return std::filesystem::path(std::u8string(Root->begin(), Root->end())) / "Saved/Automation/Session.json";
	}

	std::error_code Error;
	for (std::filesystem::path Folder = GetExecutablePath().parent_path(); !Folder.empty(); Folder = Folder.parent_path())
	{
		if (std::filesystem::exists(Folder / "Engine/Content", Error))
		{
			return Folder / "Saved/Automation/Session.json";
		}

		if (Folder == Folder.parent_path())
		{
			break;
		}
	}

	return std::filesystem::current_path(Error) / "Saved/Automation/Session.json";
}

class FEditorLink
{
public:
	explicit FEditorLink(std::filesystem::path InSessionFile)
	    : SessionFile(std::move(InSessionFile))
	{
	}

	// Sends one tool call and waits for its answer, connecting first and reconnecting once if an earlier editor went away.
	[[nodiscard]] std::expected<std::string, std::string> Call(const std::string_view Tool, const std::string_view Arguments)
	{
		for (int Attempt = 0; Attempt < 2; ++Attempt)
		{
			if (!Connection)
			{
				if (auto Opened = Open(); !Opened)
				{
					return std::unexpected(Opened.error());
				}
			}

			const std::uint64_t Id = ++NextId;
			std::string Request = std::format("{{\"id\": {}, \"tool\": ", Id);
			AppendJsonString(Request, Tool);
			Request += ", \"arguments\": ";
			Request += Arguments.empty() ? std::string_view("{}") : Arguments;
			Request += "}";
			if (!Connection->Send(Request))
			{
				Connection.reset();
				continue;
			}

			// Long scripted Play input runs for up to a minute.
			const auto Deadline = std::chrono::steady_clock::now() + 150s;
			while (std::chrono::steady_clock::now() < Deadline)
			{
				const auto Reply = Connection->Receive(500ms);
				if (!Reply)
				{
					Connection.reset();
					return std::unexpected(std::format("The editor closed the connection: {}", Reply.error().Message));
				}

				if (*Reply)
				{
					return std::move(**Reply);
				}
			}

			return std::unexpected("The editor did not answer within 150 seconds");
		}

		return std::unexpected("Could not reach the editor");
	}

private:
	[[nodiscard]] std::expected<void, std::string> Open()
	{
		// An editor that crashed or was killed leaves its session file behind, and a starting editor replaces it a moment later, so the file
		// is read again until an editor answers.
		std::expected<FAutomationSession, FAutomationError> Session = std::unexpected(FAutomationError{});
		std::optional<FIpcConnection> Opened;
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (!Opened && std::chrono::steady_clock::now() < Deadline)
		{
			Session = ReadAutomationSession(SessionFile);
			if (Session)
			{
				if (auto Connected = FIpcConnection::Connect(Session->Endpoint, 1s))
				{
					Opened = std::move(*Connected);
					break;
				}
			}

			std::this_thread::sleep_for(250ms);
		}

		if (!Session)
		{
			return std::unexpected(Session.error().Message);
		}

		if (!Opened)
		{
			return std::unexpected(std::format("The process named in {} is not running; restart it with --automation", SessionFile.string()));
		}

		std::string Hello = "{\"token\": ";
		AppendJsonString(Hello, Session->Token);
		Hello += "}";
		if (!Opened->Send(Hello))
		{
			return std::unexpected("Could not send the session token to the editor");
		}

		const auto Reply = Opened->Receive(5s);
		if (!Reply || !*Reply || !(*Reply)->contains("\"ok\": true"))
		{
			return std::unexpected("The editor refused the session token");
		}

		Connection = std::move(*Opened);
		return {};
	}

	std::filesystem::path SessionFile;
	std::optional<FIpcConnection> Connection;
	std::uint64_t NextId = 0;
};

[[nodiscard]] std::filesystem::path FindTargetSessionFile(const std::string& Target)
{
	return Target == "editor" ? FindSessionFile() : GetAutomationSessionsDirectory() / std::filesystem::path(std::u8string(Target.begin(), Target.end())) / "Session.json";
}

// Every process with a session file, and whether it answers a connection now.
[[nodiscard]] std::string ListTargets()
{
	std::vector<std::pair<std::string, std::filesystem::path>> Targets{{"editor", FindSessionFile()}};
	std::error_code Error;
	for (const auto& Entry : std::filesystem::directory_iterator(GetAutomationSessionsDirectory(), Error))
	{
		if (Entry.is_directory(Error) && std::filesystem::exists(Entry.path() / "Session.json", Error))
		{
			const std::u8string Name = Entry.path().filename().u8string();
			Targets.emplace_back(std::string(Name.begin(), Name.end()), Entry.path() / "Session.json");
		}
	}

	std::string Output = R"({"id": 0, "result": {"targets": [)";
	for (std::size_t Index = 0; Index < Targets.size(); ++Index)
	{
		const auto Session = ReadAutomationSession(Targets[Index].second);
		const bool bAlive = Session && FIpcConnection::Connect(Session->Endpoint, 200ms).has_value();
		Output += Index == 0 ? "{\"name\": " : ", {\"name\": ";
		AppendJsonString(Output, Targets[Index].first);
		Output += std::format(", \"answers\": {}, \"writes\": {}}}", bAlive ? "true" : "false", Session && Session->bAllowWrites ? "true" : "false");
	}

	return Output + "]}}";
}

int Run()
{
#ifdef HERTA_PLATFORM_WINDOWS
	// MCP frames are UTF-8 lines; text mode would turn them into CRLF and mangle non-ASCII.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	std::map<std::string, FEditorLink> Links;
	std::string Line;
	while (std::getline(std::cin, Line))
	{
		if (!Line.empty() && Line.back() == '\r')
		{
			Line.pop_back();
		}

		if (Line.empty())
		{
			continue;
		}

		if (const std::optional<std::string> Reply = HandleMcpMessage(Line, [&Links](const std::string_view Tool, const std::string_view Arguments) -> std::expected<std::string, std::string>
		{
			if (Tool == "automation_targets")
			{
				return ListTargets();
			}

			// Names are lowercase on disk, so Client-2 and client-2 reach the same process on every platform.
			std::string Target = ReadMcpTarget(Arguments);
			std::ranges::transform(Target, Target.begin(), [](const unsigned char Character)
			{
				return static_cast<char>(std::tolower(Character));
			});

			auto Link = Links.find(Target);
			if (Link == Links.end())
			{
				Link = Links.emplace(Target, FEditorLink(FindTargetSessionFile(Target))).first;
			}

			return Link->second.Call(Tool, Arguments);
		}))
		{
			std::cout << *Reply << '\n';
			std::cout.flush();
		}
	}

	return 0;
}
}
}

int main()
{
	try
	{
		return Herta::Run();
	}
	catch (const std::exception& Error)
	{
		std::fprintf(stderr, "HertaEditorMcp failed: %s\n", Error.what());
		return 1;
	}
	catch (...)
	{
		std::fputs("HertaEditorMcp failed with an unknown error\n", stderr);
		return 1;
	}
}
