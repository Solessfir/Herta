#include "Herta/Assets/CookedContent.h"
#include "Herta/Core/Log.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Platform/Process.h"
#include "Herta/Project/Project.h"
#include "Herta/Replication/GameServer.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace Herta
{
namespace
{
inline constexpr FLogCategory ServerLog{.Name = "Server"};

// Set from a signal handler, so it must be lock-free.
std::atomic<bool> StopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void RequestStop(int)
{
	StopRequested.store(true);
}

void ReportFailure(const std::string_view Message) noexcept
{
	try
	{
		std::println(stderr, "HertaServer: {}", Message);
	}
	catch (...)
	{
	}
}

[[nodiscard]] std::string_view FindArgumentValue(const std::span<char* const> Arguments, const std::string_view Prefix) noexcept
{
	for (const char* const Argument : Arguments)
	{
		const std::string_view View(Argument);
		if (View.starts_with(Prefix))
		{
			return View.substr(Prefix.size());
		}
	}

	return {};
}

template <typename T>
[[nodiscard]] bool ParseNumber(const std::string_view Text, T& Value) noexcept
{
	const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
	return Error == std::errc{} && End == Text.data() + Text.size();
}

struct FServerOptions
{
	std::filesystem::path ExecutablePath;
	std::filesystem::path ProjectPath;
	std::uint16_t Port = 27015;
	std::size_t MaximumPlayers = 8;
	// Stops after this many ticks, for unattended checks; zero serves until interrupted.
	std::uint32_t Ticks = 0;
};

int RunServer(const FServerOptions& Options)
{
	const std::filesystem::path ExecutableDirectory = std::filesystem::absolute(Options.ExecutablePath).parent_path();
	const std::filesystem::path ProjectPath = !Options.ProjectPath.empty() ? Options.ProjectPath : FindStagedProject(ExecutableDirectory).value_or(std::filesystem::path{});
	const auto Project = LoadProject(ProjectPath);
	if (!Project)
	{
		ReportFailure(std::format("Cannot load the game project{}: {}. Stage one with HertaEditorCmd game.stage, or pass --project=<path>.", ProjectPath.empty() ? "" : " " + ProjectPath.generic_string(), Project.error().Message));
		return 1;
	}

	FLogOptions LogOptions;
	LogOptions.FilePath = std::filesystem::path("Saved") / "Logs" / "Server.log";
	auto LogResult = FLogService::Create(std::move(LogOptions));
	if (!LogResult)
	{
		ReportFailure(LogResult.error().Message);
		return 1;
	}

	std::unique_ptr<FLogService> Log = std::move(*LogResult);
	auto TaskResult = FTaskSystem::Create({.Log = Log.get()});
	if (!TaskResult)
	{
		HERTA_LOG_ERROR(*Log, ServerLog, "Could not start tasks: {}", TaskResult.error().Message);
		return 1;
	}

	std::unique_ptr<FTaskSystem> Tasks = std::move(*TaskResult);
	const auto Level = LoadLevel(Project->StartingLevel);
	const auto ContentHash = HashLevelFile(Project->StartingLevel);
	if (!Level || !ContentHash)
	{
		HERTA_LOG_ERROR(*Log, ServerLog, "Could not load {}: {}", Project->StartingLevel.generic_string(), !Level ? Level.error().Message : ContentHash.error().Message);
		return 1;
	}

	// The server draws nothing and plays no sound: its session has no device, and its meshes are only bounds for collision.
	const FCookedContent Content({Project->ContentRoot, Project->Root.parent_path() / "Engine" / "Content"});
	auto Staged = CreateStagedSession(Project->StartingLevel, *Level, Content, nullptr, Tasks.get());
	if (!Staged)
	{
		HERTA_LOG_ERROR(*Log, ServerLog, "Could not start the game: {}", Staged.error().Message);
		return 1;
	}

	for (const std::string& Warning : Staged->Warnings)
	{
		HERTA_LOG_WARNING(*Log, ServerLog, "{}", Warning);
	}

	auto ServerResult = FGameServer::Create(std::move(Staged->Session), {.Port = Options.Port, .MaximumPlayers = Options.MaximumPlayers, .ContentHash = *ContentHash});
	if (!ServerResult)
	{
		HERTA_LOG_ERROR(*Log, ServerLog, "Could not start serving: {}", ServerResult.error().Message);
		return 1;
	}

	std::unique_ptr<FGameServer> Server = std::move(*ServerResult);
	HERTA_LOG_INFO(*Log, ServerLog, "Serving {} from {} on port {} for up to {} players", Level->Name, Project->Descriptor.Name, Server->GetPort(), Options.MaximumPlayers);
	std::signal(SIGINT, RequestStop);
	std::signal(SIGTERM, RequestStop);

	using FClock = std::chrono::steady_clock;
	constexpr auto TickDuration = std::chrono::duration_cast<FClock::duration>(std::chrono::duration<double>(1. / ReplicationTicksPerSecond));
	// A server that falls this far behind, such as after a debugger break, starts its clock over instead of running ticks back to back.
	constexpr auto MaximumLag = std::chrono::milliseconds(250);
	auto Next = FClock::now();
	std::uint32_t Tick = 0;
	while (!StopRequested.load() && (Options.Ticks == 0 || Tick < Options.Ticks))
	{
		if (auto Ticked = Server->Tick(); !Ticked)
		{
			HERTA_LOG_ERROR(*Log, ServerLog, "The server stopped: {}", Ticked.error().Message);
			return 1;
		}

		++Tick;
		if (Tick % (ReplicationTicksPerSecond * 30) == 0)
		{
			const FGameServerStats Stats = Server->GetStats();
			HERTA_LOG_INFO(*Log, ServerLog, "{} players, {} snapshots, {} KiB sent, {} malformed messages", Stats.Players, Stats.SnapshotsSent, Stats.SnapshotBytes / 1024, Stats.MalformedMessages);
		}

		Next += TickDuration;
		const auto Now = FClock::now();
		if (Now - Next > MaximumLag)
		{
			Next = Now;
		}

		std::this_thread::sleep_until(Next);
	}

	const FGameServerStats Stats = Server->GetStats();
	HERTA_LOG_INFO(*Log, ServerLog, "Stopped after {} ticks with {} players, {} snapshots, {} reconnects, {} rejected clients", Stats.Tick, Stats.Players, Stats.SnapshotsSent, Stats.Reconnects, Stats.RejectedClients);
	return 0;
}
}
}

int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		const std::span<char* const> All(Arguments, static_cast<std::size_t>(std::max(ArgumentCount, 0)));
		const std::span<char* const> Rest = All.empty() ? All : All.subspan(1);
		Herta::FServerOptions Options;
		Options.ExecutablePath = Herta::GetExecutablePath();
		const std::string_view Project = Herta::FindArgumentValue(Rest, "--project=");
		Options.ProjectPath = std::filesystem::path(std::u8string(Project.begin(), Project.end()));
		const std::string_view Port = Herta::FindArgumentValue(Rest, "--port=");
		const std::string_view Players = Herta::FindArgumentValue(Rest, "--max-players=");
		const std::string_view Ticks = Herta::FindArgumentValue(Rest, "--ticks=");
		if ((!Port.empty() && !Herta::ParseNumber(Port, Options.Port)) || (!Players.empty() && !Herta::ParseNumber(Players, Options.MaximumPlayers)) || (!Ticks.empty() && !Herta::ParseNumber(Ticks, Options.Ticks)))
		{
			Herta::ReportFailure("--port=, --max-players=, and --ticks= take whole numbers");
			return 2;
		}

		return Herta::RunServer(Options);
	}
	catch (const std::exception& Exception)
	{
		Herta::ReportFailure(Exception.what());
		return 1;
	}
	catch (...)
	{
		Herta::ReportFailure("unknown error");
		return 1;
	}
}
