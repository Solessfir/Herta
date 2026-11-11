#include "Herta/GameAutomation/GameAutomation.h"

#include "Herta/Core/Json.h"
#include "Herta/Core/Log.h"
#include "Herta/EditorCore/Automation.h"
#include "Herta/Navigation/NavigationWorld.h"
#include "Herta/Replication/GameClient.h"
#include "Herta/Replication/GameServer.h"

#include <simdjson.h>

// Static, so a program that also writes PNGs through stb keeps its own copy without a duplicate symbol.
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <vector>

namespace Herta
{
namespace
{
void AppendVector(std::string& Output, const FVector3& Value)
{
	Output += std::format("[{:.4f}, {:.4f}, {:.4f}]", Value.X, Value.Y, Value.Z);
}

// Reads a tool call's arguments; a malformed object reads as empty, so every argument falls back to its default.
class FArguments
{
public:
	explicit FArguments(const std::string& Text)
	{
		simdjson::dom::element Root;
		bValid = !Parser.parse(Text).get(Root) && !Root.get_object().get(Object);
	}

	[[nodiscard]] double Number(const std::string_view Key, const double Default) const
	{
		double Value = Default;
		if (bValid && Object[Key].get_double().get(Value))
		{
			std::int64_t Integer = 0;
			return Object[Key].get_int64().get(Integer) ? Default : static_cast<double>(Integer);
		}

		return std::isfinite(Value) ? Value : Default;
	}

	[[nodiscard]] bool Flag(const std::string_view Key) const
	{
		bool bValue = false;
		return bValid && !Object[Key].get_bool().get(bValue) && bValue;
	}

	[[nodiscard]] std::optional<std::array<double, 3>> Numbers(const std::string_view Key, const std::size_t Count) const
	{
		simdjson::dom::array Array;
		if (!bValid || Count > 3 || Object[Key].get_array().get(Array) || Array.size() != Count)
		{
			return std::nullopt;
		}

		std::array<double, 3> Values{};
		std::size_t Index = 0;
		for (const auto Element : Array)
		{
			double Value = 0.;
			if (Element.get_double().get(Value) || !std::isfinite(Value))
			{
				return std::nullopt;
			}

			Values[Index++] = Value;
		}

		return Values;
	}

private:
	simdjson::dom::parser Parser;
	simdjson::dom::object Object;
	bool bValid = false;
};

[[nodiscard]] bool WritePng(const std::filesystem::path& Path, const FPresentationCapture& Capture, const std::uint32_t MaximumWidth)
{
	// Nearest-neighbour shrink: captures are for reading UI and scenes, not for fidelity.
	const std::uint32_t Width = std::min(Capture.Extent.Width, MaximumWidth);
	const std::uint32_t Height = std::max(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(Capture.Extent.Height) * Width / std::max(1u, Capture.Extent.Width)));
	std::vector<std::byte> Pixels(static_cast<std::size_t>(Width) * Height * 4);
	for (std::uint32_t Row = 0; Row < Height; ++Row)
	{
		const std::uint32_t SourceRow = Row * Capture.Extent.Height / Height;
		for (std::uint32_t Column = 0; Column < Width; ++Column)
		{
			const std::uint32_t SourceColumn = Column * Capture.Extent.Width / Width;
			std::memcpy(Pixels.data() + (static_cast<std::size_t>(Row) * Width + Column) * 4, Capture.Pixels.data() + (static_cast<std::size_t>(SourceRow) * Capture.Extent.Width + SourceColumn) * 4, 4);
		}
	}

	std::vector<char> Encoded;
	const auto Append = [](void* const Context, void* const Data, const int Size)
	{
		auto& Output = *static_cast<std::vector<char>*>(Context);
		Output.insert(Output.end(), static_cast<const char*>(Data), static_cast<const char*>(Data) + Size);
	};

	if (stbi_write_png_to_func(Append, &Encoded, static_cast<int>(Width), static_cast<int>(Height), 4, Pixels.data(), static_cast<int>(Width * 4)) == 0)
	{
		return false;
	}

	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	Stream.write(Encoded.data(), static_cast<std::streamsize>(Encoded.size()));
	return static_cast<bool>(Stream);
}
}

struct FGameAutomation::FImplementation
{
	std::unique_ptr<FAutomationServer> Server;
	FLogService* Log = nullptr;
	std::filesystem::path Directory;

	// A play_input run: the input to repeat each frame, how many frames remain, and the reply waiting for its end.
	struct FInputRun
	{
		std::uint64_t Id = 0;
		FGameInput Input{};
		int FramesLeft = 0;
		int Frames = 0;
		float YawPerFrame = 0.f;
		float PitchPerFrame = 0.f;
	};

	std::deque<FInputRun> Runs;

	struct FCapture
	{
		std::uint64_t Id = 0;
		std::uint32_t MaximumWidth = 1600;
	};

	std::vector<FCapture> Captures;
};

FGameAutomation::FGameAutomation(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameAutomation::~FGameAutomation() = default;

std::expected<std::unique_ptr<FGameAutomation>, std::string> FGameAutomation::Create(const std::string_view Name, const bool bAllowWrites, FLogService& Log)
{
	const bool bValidName = !Name.empty() && Name.size() <= 64 && std::ranges::all_of(Name, [](const unsigned char Character)
	{
		return std::isalnum(Character) || Character == '-' || Character == '_';
	});

	if (!bValidName)
	{
		return std::unexpected(std::format("'{}' is not an automation name; use letters, digits, '-', and '_'", Name));
	}

	// Lowercase on disk, so the MCP server finds the same folder on case-sensitive file systems whatever case a call uses.
	std::string Folder(Name);
	std::ranges::transform(Folder, Folder.begin(), [](const unsigned char Character)
	{
		return static_cast<char>(std::tolower(Character));
	});

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Directory = GetAutomationSessionsDirectory() / std::filesystem::path(std::u8string(Folder.begin(), Folder.end()));
	auto Server = FAutomationServer::Create({.SessionDirectory = Implementation->Directory, .bAllowWrites = bAllowWrites});
	if (!Server)
	{
		return std::unexpected(Server.error().Message);
	}

	Implementation->Server = std::move(*Server);
	Implementation->Log = &Log;
	return std::unique_ptr<FGameAutomation>(new FGameAutomation(std::move(Implementation)));
}

const std::filesystem::path& FGameAutomation::GetSessionFile() const noexcept
{
	return Implementation->Server->GetSessionFile();
}

std::optional<FGameInput> FGameAutomation::TakeInput()
{
	FImplementation& State = *Implementation;
	if (State.Runs.empty())
	{
		return std::nullopt;
	}

	FImplementation::FInputRun& Run = State.Runs.front();
	FGameInput Input = Run.Input;
	Input.YawDelta = Run.YawPerFrame;
	Input.PitchDelta = Run.PitchPerFrame;
	// Presses happen on the first frame only, as a player's would.
	if (Run.Frames > 0)
	{
		Input.bJump = Input.bThrow = Input.bAttack = Input.bToggleCamera = Input.bToggleRagdoll = false;
	}

	++Run.Frames;
	--Run.FramesLeft;
	return Input;
}

void FGameAutomation::Pump(const FGameAutomationFrame& Frame)
{
	FImplementation& State = *Implementation;
	FAutomationServer& Server = *State.Server;
	const auto Describe = [&Frame]()
	{
		std::string Output = std::format("{{\"playing\": {}, \"paused\": {}", Frame.Session ? "true" : "false", Frame.bPaused ? "true" : "false");
		if (Frame.Session)
		{
			AppendGameSessionState(Output, *Frame.Session, Frame.View);
			// Fields above follow the session's own player; on a server or host every player's feet follow, in player order.
			Output += ", \"players\": [";
			const std::vector<FVector3> Feet = Frame.Session->GetPlayerFootPositions();
			for (std::size_t Index = 0; Index < Feet.size(); ++Index)
			{
				Output += Index == 0 ? "" : ", ";
				AppendVector(Output, Feet[Index]);
			}

			Output += "]";
		}

		if (Frame.Client)
		{
			const FGameClientStats Stats = Frame.Client->GetStats();
			constexpr std::array StateNames{"connecting", "playing", "rejected", "disconnected"};
			Output += std::format(", \"client\": {{\"state\": \"{}\", \"snapshots\": {}, \"corrections\": {}, \"outcomes\": {}, \"rejected\": {}, \"pingMs\": {:.1f}}}", StateNames[static_cast<std::size_t>(Frame.Client->GetState())], Stats.Snapshots, Stats.Corrections, Stats.Outcomes, Stats.Rejected, Stats.PingMilliseconds);
		}

		if (Frame.Server)
		{
			const FGameServerStats Stats = Frame.Server->GetStats();
			Output += std::format(", \"server\": {{\"tick\": {}, \"players\": {}, \"connections\": {}, \"snapshotsSent\": {}, \"deferredEntities\": {}, \"malformed\": {}, \"rejectedClients\": {}, \"missingInputs\": {}}}", Stats.Tick, Stats.Players, Stats.Connections, Stats.SnapshotsSent, Stats.DeferredEntities, Stats.MalformedMessages, Stats.RejectedClients, Stats.MissingInputs);
		}

		if (Frame.Audio)
		{
			Output += ", \"audio\": " + DescribeAudioStats(Frame.Audio->GetStats());
		}

		return Output + "}";
	};

	for (const FAutomationRequest& Request : Server.TakeRequests())
	{
		const FArguments Arguments(Request.Arguments);
		const std::string_view Tool = Request.Tool;
		if (Tool == "play_state" || Tool == "network_state" || Tool == "editor_status")
		{
			Server.Respond(Request.Id, Describe());
		}
		else if (Tool == "play_input")
		{
			if (!Frame.Session)
			{
				Server.Fail(Request.Id, "not_playing", "This process has no game session to drive");
				continue;
			}

			const int Frames = static_cast<int>(std::clamp(Arguments.Number("frames", 1.), 1., 3600.));
			const auto Move = Arguments.Numbers("move", 2).value_or(std::array<double, 3>{});
			const auto Look = Arguments.Numbers("look", 2).value_or(std::array<double, 3>{});
			State.Runs.push_back({
			    .Id = Request.Id,
			    .Input = {.Move = {static_cast<float>(std::clamp(Move[0], -1., 1.)), static_cast<float>(std::clamp(Move[1], -1., 1.))},
			        .bJump = Arguments.Flag("jump"),
			        .bSprint = Arguments.Flag("sprint"),
			        .bCrouch = Arguments.Flag("crouch"),
			        .bThrow = Arguments.Flag("throw"),
			        .bToggleCamera = Arguments.Flag("toggle_camera"),
			        .bToggleRagdoll = Arguments.Flag("ragdoll"),
			        .bAttack = Arguments.Flag("attack"),
			        .bBlock = Arguments.Flag("block")},
			    .FramesLeft = Frames,
			    .YawPerFrame = static_cast<float>(Look[0] / Frames),
			    .PitchPerFrame = static_cast<float>(Look[1] / Frames),
			});
		}
		else if (Tool == "play_teleport")
		{
			const auto Position = Arguments.Numbers("position", 3);
			if (!Frame.Session || Frame.Client || !Position)
			{
				Server.Fail(Request.Id, "invalid", Frame.Client ? "A networked client's player belongs to its server; teleport on the server or a standalone game" : "play_teleport needs a game session and a position [x, y, z]");
				continue;
			}

			const float Yaw = static_cast<float>(Arguments.Number("yaw", Frame.Session->GetPlayerYaw()));
			if (auto Placed = Frame.Session->TeleportPlayer({static_cast<float>((*Position)[0]), static_cast<float>((*Position)[1]), static_cast<float>((*Position)[2])}, Yaw); !Placed)
			{
				Server.Fail(Request.Id, "teleport_failed", Placed.error().Message);
				continue;
			}

			Server.Respond(Request.Id, Describe());
		}
		else if (Tool == "editor_capture")
		{
			if (!Frame.RequestCapture)
			{
				Server.Fail(Request.Id, "no_window", "This process has no window to capture");
				continue;
			}

			State.Captures.push_back({.Id = Request.Id, .MaximumWidth = static_cast<std::uint32_t>(std::clamp(Arguments.Number("max_width", 1600.), 64., 7680.))});
			Frame.RequestCapture();
		}
		else if (Tool == "audio_voices")
		{
			if (!Frame.Audio)
			{
				Server.Fail(Request.Id, "no_audio", "This process plays no audio");
				continue;
			}

			const auto Label = [](const FAssetId& Id)
			{
				return Id.ToString();
			};

			Server.Respond(Request.Id, DescribeAudioPlays(*Frame.Audio, Arguments.Flag("playing"), Label));
		}
		else if (Tool == "editor_log")
		{
			const auto Since = static_cast<std::uint64_t>(std::max(0., Arguments.Number("since", 0.)));
			const auto Limit = static_cast<std::size_t>(std::clamp(Arguments.Number("limit", 200.), 1., 2000.));
			const FLogReadResult Read = State.Log->ReadEditorBuffer({.NextSequence = Since, .Generation = static_cast<std::uint64_t>(std::max(0., Arguments.Number("generation", 0.)))}, Limit);
			std::string Output = "{\"lines\": [";
			for (std::size_t Index = 0; Index < Read.Records.size(); ++Index)
			{
				const FLogRecord& Record = Read.Records[Index];
				Output += Index == 0 ? "" : ", ";
				AppendJsonString(Output, std::format("[{}] [{}] {}", GetLogLevelName(Record.Level), Record.Category, Record.Message));
			}

			Server.Respond(Request.Id, Output + std::format("], \"since\": {}, \"generation\": {}, \"truncated\": {}}}", Read.NextCursor.NextSequence, Read.NextCursor.Generation, Read.bHistoryTruncated ? "true" : "false"));
		}
		else
		{
			Server.Fail(Request.Id, "unknown_tool", std::format("This game process has no tool named '{}'; it offers play_state, network_state, play_input, play_teleport, editor_capture, audio_voices, and editor_log", Tool));
		}
	}

	// A run answers once its last frame has played, so the reply shows where the input led.
	while (!State.Runs.empty() && State.Runs.front().FramesLeft <= 0)
	{
		Server.Respond(State.Runs.front().Id, Describe());
		State.Runs.pop_front();
	}

	if (!State.Captures.empty() && Frame.TakeCapture)
	{
		if (auto Captured = Frame.TakeCapture())
		{
			for (const FImplementation::FCapture& Capture : State.Captures)
			{
				const std::filesystem::path Path = State.Directory / std::format("capture-{}.png", Capture.Id);
				if (!*Captured)
				{
					Server.Fail(Capture.Id, "capture_failed", Captured->error().Message);
				}
				else if (WritePng(Path, **Captured, Capture.MaximumWidth))
				{
					std::string Output = "{\"image\": ";
					AppendJsonString(Output, Path.string());
					Server.Respond(Capture.Id, Output + "}");
				}
				else
				{
					Server.Fail(Capture.Id, "capture_failed", "Could not write the capture");
				}
			}

			State.Captures.clear();
		}
	}
}

void AppendGameSessionState(std::string& Output, const FGameSession& Session, const FMatrix4& View)
{
	Output += std::format(", \"grounded\": {}, \"crouched\": {}, \"sliding\": {}, \"mantling\": {}, \"wallRunning\": {}, \"ragdoll\": {}, \"thirdPerson\": {}", Session.IsPlayerGrounded() ? "true" : "false", Session.IsPlayerCrouched() ? "true" : "false", Session.IsPlayerSliding() ? "true" : "false", Session.IsPlayerMantling() ? "true" : "false",
	    Session.IsPlayerWallRunning() ? "true" : "false", Session.IsPlayerRagdolled() ? "true" : "false", Session.IsThirdPerson() ? "true" : "false");
	Output += ", \"foot\": ";
	AppendVector(Output, Session.GetPlayerFootPosition());
	Output += ", \"velocity\": ";
	AppendVector(Output, Session.GetPlayerVelocity());
	Output += std::format(", \"yaw\": {:.4f}", Session.GetPlayerYaw());
	const FCharacterMover& Mover = Session.GetMover();
	const FMoverState& State = Mover.GetState();
	Output += ", \"mode\": ";
	AppendJsonString(Output, Mover.GetActiveMode());
	Output += std::format(", \"speed\": {:.3f}, \"touchingWall\": {}, \"wallNormal\": ", std::hypot(State.Velocity.X, State.Velocity.Z), State.Ground.bTouchingWall ? "true" : "false");
	AppendVector(Output, State.Ground.WallNormal);
	Output += ", \"groundNormal\": ";
	AppendVector(Output, State.Ground.GroundNormal);
	// The view is a rotation followed by a translation, so the eye is the transposed rotation applied to minus the translation.
	const FVector3 Translation{View(0, 3), View(1, 3), View(2, 3)};
	const FVector3 Eye = -FVector3{View(0, 0) * Translation.X + View(1, 0) * Translation.Y + View(2, 0) * Translation.Z, View(0, 1) * Translation.X + View(1, 1) * Translation.Y + View(2, 1) * Translation.Z, View(0, 2) * Translation.X + View(1, 2) * Translation.Y + View(2, 2) * Translation.Z};
	Output += ", \"camera\": ";
	AppendVector(Output, Eye);
	Output += std::format(", \"cameraDistance\": {:.3f}", (Eye - (State.Foot + FVector3::Up() * 0.95f)).Length());
	Output += ", \"animationState\": ";
	AppendJsonString(Output, Session.GetCharacterStateName());
	const FGameObjective& Objective = Session.GetObjective();
	Output += std::format(", \"objective\": {{\"state\": {}, \"seconds\": {:.3f}}}", static_cast<int>(Objective.State), Objective.ElapsedSeconds);
	Output += std::format(", \"characters\": {}, \"cuts\": {}, \"cutPieces\": {}", Session.GetCharacterCount(), Session.GetCutCount(), Session.GetCutPieceCount());
	const FParticleStats Particles = Session.GetParticleStats();
	Output += std::format(", \"particles\": {{\"effects\": {}, \"live\": {}, \"spawned\": {}, \"dropped\": {}, \"retired\": {}, \"collisions\": {}}}", Particles.Effects, Particles.Particles, Particles.Spawned, Particles.Dropped, Particles.EffectsRetired, Particles.Collisions);
	Output += ", \"agents\": [";
	const std::vector<FGameAgentState> Agents = Session.GetAgentStates();
	for (std::size_t Index = 0; Index < Agents.size(); ++Index)
	{
		const FGameAgentState& Agent = Agents[Index];
		Output += Index == 0 ? "{\"name\": " : ", {\"name\": ";
		AppendJsonString(Output, Agent.Name);
		Output += ", \"state\": ";
		AppendJsonString(Output, Agent.State);
		Output += std::format(", \"foot\": [{:.3f}, {:.3f}, {:.3f}], \"yaw\": {:.3f}, \"health\": {:.2f}, \"sees\": {}, \"remembers\": {}, \"onNavigation\": {}, \"attacking\": {}}}", Agent.Foot.X, Agent.Foot.Y, Agent.Foot.Z, Agent.Yaw, Agent.Health, Agent.bSeesPlayer ? "true" : "false", Agent.bRemembersPlayer ? "true" : "false",
		    Agent.bOnNavigation ? "true" : "false", Agent.bAttacking ? "true" : "false");
	}

	Output += "]";
	if (const FNavigationWorld* const Navigation = Session.GetNavigation())
	{
		const FNavigationStats Loaded = Navigation->GetStats();
		Output += std::format(", \"navigation\": {{\"sources\": {}, \"tiles\": {}, \"polygons\": {}, \"obstacles\": {}, \"pendingObstacles\": {}, \"agents\": {}}}", Loaded.Sources, Loaded.Tiles, Loaded.Polygons, Loaded.Obstacles, Loaded.PendingObstacles, Loaded.Agents);
	}

	const FRegionStats Regions = Session.GetRegionStats();
	Output += std::format(", \"regions\": {{\"loaded\": {}, \"loading\": {}, \"preparing\": {}, \"loads\": {}, \"unloads\": {}, \"cancelled\": {}, \"failed\": {}, \"deferred\": {}, \"lastError\": ", Regions.Loaded, Regions.Loading, Regions.Preparing, Regions.Loads, Regions.Unloads, Regions.Cancelled, Regions.Failed, Regions.Deferred);
	AppendJsonString(Output, Regions.LastError);
	Output += "}";
}

std::string DescribeAudioStats(const FAudioStats& Stats)
{
	std::string Output = std::format("{{\"device\": ");
	AppendJsonString(Output, Stats.Device);
	Output += std::format(", \"null\": {}, \"sounds\": {}, \"voices\": {}, \"audibleVoices\": {}, \"streams\": {}, \"started\": {}, \"stolen\": {}, \"rejected\": {}, \"missing\": {}, \"starvedFrames\": {}, \"commands\": {}, \"dropped\": {}", Stats.bNullDevice ? "true" : "false", Stats.Sounds, Stats.Voices, Stats.AudibleVoices, Stats.Streams,
	    Stats.VoicesStarted, Stats.VoicesStolen, Stats.VoicesRejected, Stats.MissingSounds, Stats.StreamStarvedFrames, Stats.CommandsSubmitted, Stats.CommandsDropped);
	Output += std::format(", \"blockMicroseconds\": {:.1f}, \"blockPeakMicroseconds\": {:.1f}, \"blockBudgetMicroseconds\": {:.1f}, \"buses\": {{", Stats.BlockMicrosecondsAverage, Stats.BlockMicrosecondsPeak, Stats.BlockBudgetMicroseconds);
	for (std::size_t Bus = 0; Bus < Stats.Buses.size(); ++Bus)
	{
		Output += Bus == 0 ? "" : ", ";
		AppendJsonString(Output, Stats.Buses[Bus].Name);
		Output += std::format(": {{\"volume\": {:.3f}, \"peak\": {:.4f}}}", Stats.Buses[Bus].Volume, Stats.Buses[Bus].Peak);
	}

	return Output + "}}";
}

std::string DescribeAudioPlays(const FAudioEngine& Audio, const bool bPlayingOnly, const std::function<std::string(const FAssetId&)>& Label)
{
	std::string Output = "{\"plays\": [";
	bool bFirst = true;
	for (const FAudioPlayRecord& Play : Audio.GetRecentPlays())
	{
		if (bPlayingOnly && !Play.bPlaying)
		{
			continue;
		}

		Output += bFirst ? "{\"sound\": " : ", {\"sound\": ";
		bFirst = false;
		AppendJsonString(Output, Label(Play.Sound));
		Output += ", \"bus\": ";
		AppendJsonString(Output, Play.Bus);
		Output += std::format(", \"voice\": {}, \"volume\": {:.3f}, \"pitch\": {:.3f}, \"secondsAgo\": {:.3f}, \"playing\": {}", Play.Voice, Play.Volume, Play.Pitch, Play.SecondsAgo, Play.bPlaying);
		if (Play.bSpatial)
		{
			Output += std::format(", \"position\": [{:.3f}, {:.3f}, {:.3f}]", Play.Position.X, Play.Position.Y, Play.Position.Z);
		}

		Output += "}";
	}

	return Output + "], \"audio\": " + DescribeAudioStats(Audio.GetStats()) + "}";
}
}
