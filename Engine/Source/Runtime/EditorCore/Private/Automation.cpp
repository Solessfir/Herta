#include "Herta/EditorCore/Automation.h"

#include "Herta/Core/Json.h"
#include "Herta/Platform/Ipc.h"
#include "Herta/Platform/Platform.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <thread>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>
#else
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace Herta
{
namespace
{
using namespace std::chrono_literals;

[[nodiscard]] std::uint32_t GetProcessId() noexcept
{
#ifdef HERTA_PLATFORM_WINDOWS
	return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
	return static_cast<std::uint32_t>(getpid());
#endif
}

[[nodiscard]] std::string MakeToken()
{
	std::random_device Device;
	std::string Token;

	for (int Word = 0; Word < 8; ++Word)
	{
		Token += std::format("{:08x}", Device());
	}

	return Token;
}

// Compares every byte so the time taken says nothing about how much of a guessed token was right.
[[nodiscard]] bool TokensMatch(const std::string_view Left, const std::string_view Right) noexcept
{
	if (Left.size() != Right.size())
	{
		return false;
	}

	unsigned char Difference = 0;

	for (std::size_t Index = 0; Index < Left.size(); ++Index)
	{
		Difference |= static_cast<unsigned char>(Left[Index] ^ Right[Index]);
	}

	return Difference == 0;
}

[[nodiscard]] std::string MakeError(const std::optional<std::uint64_t> Id, const std::string_view Code, const std::string_view Message)
{
	std::string Output = "{";
	if (Id)
	{
		Output += std::format("\"id\": {}, ", *Id);
	}

	Output += "\"error\": {\"code\": ";
	AppendJsonString(Output, Code);
	Output += ", \"message\": ";
	AppendJsonString(Output, Message);
	Output += "}}";
	return Output;
}
}

struct FAutomationServer::FImplementation
{
	FAutomationSettings Settings;
	std::filesystem::path SessionFile;
	std::string Token;
	std::optional<FIpcListener> Listener;

	struct FPending
	{
		std::string Tool;
		std::chrono::steady_clock::time_point Deadline;
	};

	std::mutex Mutex;
	std::vector<FAutomationRequest> Incoming;

	struct FResponse
	{
		std::uint64_t Id = 0;
		std::string Text{};
		bool bFailed = false;
	};

	std::vector<FResponse> Outgoing;
	std::jthread Thread;

	void Audit(const std::string_view Tool, const std::string_view Outcome) const
	{
		std::ofstream Log(Settings.SessionDirectory / "Audit.log", std::ios::app);
		const auto Now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		Log << std::format("{:%Y-%m-%d %H:%M:%S} {} {}\n", Now, Tool, Outcome);
	}

	// Runs on the server thread for one connected client until it leaves or the editor shuts down.
	void Serve(FIpcConnection& Connection, const std::stop_token& Stop)
	{
		simdjson::dom::parser Parser;
		const std::optional<std::string> Hello = Connection.Receive(5s).value_or(std::nullopt);
		simdjson::dom::element Element;
		std::string_view Offered;
		if (!Hello || Parser.parse(*Hello).get(Element) || Element["token"].get_string().get(Offered) || !TokensMatch(Offered, Token))
		{
			(void)Connection.Send(MakeError(std::nullopt, "unauthorized", "The session token does not match"));
			Audit("connect", "rejected");
			return;
		}

		if (!Connection.Send(std::format("{{\"ok\": true, \"write\": {}}}", Settings.bAllowWrites ? "true" : "false")))
		{
			return;
		}

		Audit("connect", "accepted");
		std::map<std::uint64_t, FPending> Pending;
		while (!Stop.stop_requested())
		{
			const auto Received = Connection.Receive(15ms);
			if (!Received)
			{
				break;
			}

			if (const std::optional<std::string>& Message = *Received)
			{
				std::uint64_t Id = 0;
				std::string_view Tool;
				simdjson::dom::element Arguments;
				const bool bValid = !Parser.parse(*Message).get(Element) && !Element["id"].get_uint64().get(Id) && !Element["tool"].get_string().get(Tool) && !Tool.empty();
				if (!bValid)
				{
					(void)Connection.Send(MakeError(std::nullopt, "invalid_request", "A request needs an unsigned id, a tool name, and an arguments object"));
				}
				else if (Pending.contains(Id))
				{
					(void)Connection.Send(MakeError(Id, "duplicate_id", "That request id is still being answered"));
				}
				else
				{
					const bool bHasArguments = !Element["arguments"].get(Arguments) && Arguments.is_object();
					Pending.emplace(Id, FPending{.Tool = std::string(Tool), .Deadline = std::chrono::steady_clock::now() + Settings.RequestTimeout});
					std::scoped_lock Lock(Mutex);
					Incoming.push_back({.Id = Id, .Tool = std::string(Tool), .Arguments = bHasArguments ? simdjson::minify(Arguments) : std::string("{}")});
				}
			}

			std::vector<FResponse> Ready;

			{
				std::scoped_lock Lock(Mutex);
				Ready.swap(Outgoing);
			}

			for (const FResponse& Response : Ready)
			{
				const auto Found = Pending.find(Response.Id);
				if (Found == Pending.end())
				{
					continue;
				}

				Audit(Found->second.Tool, Response.bFailed ? "failed" : "ok");
				Pending.erase(Found);
				if (!Connection.Send(Response.Text))
				{
					return;
				}
			}

			const auto Now = std::chrono::steady_clock::now();
			for (auto Entry = Pending.begin(); Entry != Pending.end();)
			{
				if (Now < Entry->second.Deadline)
				{
					++Entry;
					continue;
				}

				Audit(Entry->second.Tool, "timed out");
				if (!Connection.Send(MakeError(Entry->first, "timeout", "The editor did not answer in time; it may be loading or paused in a debugger")))
				{
					return;
				}

				Entry = Pending.erase(Entry);
			}
		}

		Audit("disconnect", "closed");
	}

	void Run(const std::stop_token& Stop)
	{
		if (!Listener)
		{
			return;
		}

		FIpcListener& Clients = *Listener;
		while (!Stop.stop_requested())
		{
			auto Accepted = Clients.Accept(100ms);
			if (!Accepted)
			{
				Audit("listen", Accepted.error().Message);
				std::this_thread::sleep_for(500ms);
				continue;
			}

			if (std::optional<FIpcConnection>& Connection = *Accepted)
			{
				Serve(*Connection, Stop);
				std::scoped_lock Lock(Mutex);
				Outgoing.clear();
			}
		}
	}
};

FAutomationServer::FAutomationServer(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FAutomationServer::~FAutomationServer()
{
	Implementation->Thread.request_stop();
	if (Implementation->Thread.joinable())
	{
		Implementation->Thread.join();
	}

	std::error_code Error;
	std::filesystem::remove(Implementation->SessionFile, Error);
}

std::expected<std::unique_ptr<FAutomationServer>, FAutomationError> FAutomationServer::Create(const FAutomationSettings& Settings)
{
	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Settings = Settings;
	Implementation->Token = MakeToken();
	const std::string Endpoint = MakeIpcEndpoint(std::format("Editor-{}", GetProcessId()));
	auto Listener = FIpcListener::Listen(Endpoint);
	if (!Listener)
	{
		return std::unexpected(FAutomationError{std::format("Cannot listen for automation clients: {}", Listener.error().Message)});
	}

	Implementation->Listener = std::move(*Listener);

	std::error_code Error;
	std::filesystem::create_directories(Settings.SessionDirectory, Error);
	Implementation->SessionFile = Settings.SessionDirectory / "Session.json";
	std::string Session = "{\"endpoint\": ";
	AppendJsonString(Session, Endpoint);
	Session += ", \"token\": ";
	AppendJsonString(Session, Implementation->Token);
	Session += std::format(", \"write\": {}, \"processId\": {}}}\n", Settings.bAllowWrites ? "true" : "false", GetProcessId());

	{
		std::ofstream File(Implementation->SessionFile, std::ios::binary | std::ios::trunc);
		File << Session;
		if (!File)
		{
			return std::unexpected(FAutomationError{"Cannot write the automation session file"});
		}
	}

#ifndef HERTA_PLATFORM_WINDOWS
	// The token is the only secret; other users on the machine must not read it.
	chmod(Implementation->SessionFile.c_str(), 0600);
#endif

	FImplementation* const Raw = Implementation.get();
	Implementation->Thread = std::jthread([Raw](const std::stop_token& Stop)
	{
		Raw->Run(Stop);
	});

	return std::unique_ptr<FAutomationServer>(new FAutomationServer(std::move(Implementation)));
}

std::vector<FAutomationRequest> FAutomationServer::TakeRequests()
{
	std::scoped_lock Lock(Implementation->Mutex);
	return std::exchange(Implementation->Incoming, {});
}

void FAutomationServer::Respond(const std::uint64_t Id, std::string ResultJson)
{
	ResultJson.insert(0, std::format("{{\"id\": {}, \"result\": ", Id));
	ResultJson += "}";
	std::scoped_lock Lock(Implementation->Mutex);
	Implementation->Outgoing.push_back({.Id = Id, .Text = std::move(ResultJson), .bFailed = false});
}

void FAutomationServer::Fail(const std::uint64_t Id, const std::string_view Code, const std::string_view Message)
{
	std::scoped_lock Lock(Implementation->Mutex);
	Implementation->Outgoing.push_back({.Id = Id, .Text = MakeError(Id, Code, Message), .bFailed = true});
}

bool FAutomationServer::AllowsWrites() const noexcept
{
	return Implementation->Settings.bAllowWrites;
}

const std::filesystem::path& FAutomationServer::GetSessionFile() const noexcept
{
	return Implementation->SessionFile;
}

std::expected<FAutomationSession, FAutomationError> ReadAutomationSession(const std::filesystem::path& SessionFile)
{
	std::ifstream File(SessionFile, std::ios::binary);
	if (!File)
	{
		return std::unexpected(FAutomationError{"No editor is running with automation; start Herta with --automation"});
	}

	const std::string Text{std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
	simdjson::dom::parser Parser;
	simdjson::dom::element Element;
	std::string_view Endpoint;
	std::string_view Token;
	bool bWrite = false;
	std::uint64_t ProcessId = 0;
	if (Parser.parse(Text).get(Element) || Element["endpoint"].get_string().get(Endpoint) || Element["token"].get_string().get(Token) || Element["write"].get_bool().get(bWrite) || Element["processId"].get_uint64().get(ProcessId))
	{
		return std::unexpected(FAutomationError{"The automation session file is damaged"});
	}

	return FAutomationSession{.Endpoint = std::string(Endpoint), .Token = std::string(Token), .bAllowWrites = bWrite, .ProcessId = static_cast<std::uint32_t>(ProcessId)};
}

std::filesystem::path GetAutomationSessionsDirectory()
{
	return GetUserSettingsDirectory() / "Herta" / "Automation";
}
}
