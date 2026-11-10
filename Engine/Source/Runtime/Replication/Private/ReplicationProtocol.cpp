#include "Herta/Replication/ReplicationProtocol.h"

#include "Herta/Core/BinaryStream.h"
#include "Herta/Core/Hash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumEntityStateBytes = 4096;
constexpr std::size_t MaximumModeCount = 32;
constexpr std::size_t MaximumModeValues = 32;
constexpr std::size_t MaximumBonds = 65536;
constexpr std::size_t MaximumJointNameLength = 64;
constexpr std::size_t MaximumRejectLength = 256;

[[nodiscard]] std::unexpected<FReplicationError> Malformed(const std::string_view What)
{
	return std::unexpected(FReplicationError{std::format("Malformed {}", What)});
}

void WriteVarint(FBinaryWriter& Writer, std::uint32_t Value)
{
	while (Value >= 0x80)
	{
		Writer.Write(static_cast<std::uint8_t>(Value | 0x80));
		Value >>= 7;
	}

	Writer.Write(static_cast<std::uint8_t>(Value));
}

[[nodiscard]] std::uint32_t ReadVarint(FBinaryReader& Reader)
{
	std::uint32_t Value = 0;
	for (int Shift = 0; Shift < 35; Shift += 7)
	{
		const auto Byte = Reader.Read<std::uint8_t>();
		Value |= static_cast<std::uint32_t>(Byte & 0x7f) << Shift;
		if ((Byte & 0x80) == 0)
		{
			return Value;
		}
	}

	Reader.Invalidate();
	return 0;
}

void WriteDouble(FBinaryWriter& Writer, const double Value)
{
	Writer.Write(std::bit_cast<std::uint64_t>(Value));
}

[[nodiscard]] double ReadDouble(FBinaryReader& Reader)
{
	return std::bit_cast<double>(Reader.Read<std::uint64_t>());
}

void WriteVector(FBinaryWriter& Writer, const FVector3& Value)
{
	Writer.WriteFloat(Value.X);
	Writer.WriteFloat(Value.Y);
	Writer.WriteFloat(Value.Z);
}

[[nodiscard]] FVector3 ReadVector(FBinaryReader& Reader)
{
	const float X = Reader.ReadFloat();
	const float Y = Reader.ReadFloat();
	const float Z = Reader.ReadFloat();
	return {X, Y, Z};
}

void WriteQuaternion(FBinaryWriter& Writer, const FQuaternion& Value)
{
	Writer.WriteFloat(Value.X);
	Writer.WriteFloat(Value.Y);
	Writer.WriteFloat(Value.Z);
	Writer.WriteFloat(Value.W);
}

[[nodiscard]] FQuaternion ReadQuaternion(FBinaryReader& Reader)
{
	FQuaternion Value;
	Value.X = Reader.ReadFloat();
	Value.Y = Reader.ReadFloat();
	Value.Z = Reader.ReadFloat();
	Value.W = Reader.ReadFloat();
	return Value;
}

[[nodiscard]] bool IsFinite(const FVector3& Value) noexcept
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
}

[[nodiscard]] bool IsFinite(const FQuaternion& Value) noexcept
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z) && std::isfinite(Value.W);
}

void WriteOptionalVector(FBinaryWriter& Writer, const std::optional<FVector3>& Value)
{
	Writer.Write(static_cast<std::uint8_t>(Value ? 1 : 0));
	if (Value)
	{
		WriteVector(Writer, *Value);
	}
}

[[nodiscard]] std::optional<FVector3> ReadOptionalVector(FBinaryReader& Reader)
{
	const auto Present = Reader.Read<std::uint8_t>();
	if (Present > 1)
	{
		Reader.Invalidate();
	}

	return Present == 1 ? std::optional(ReadVector(Reader)) : std::nullopt;
}

[[nodiscard]] std::expected<FBinaryReader, FReplicationError> BeginMessage(const std::span<const std::byte> Message, const EReplicationMessage Type, const std::string_view What)
{
	if (Message.empty() || Message.size() > MaximumMessageBytes || static_cast<EReplicationMessage>(Message.front()) != Type)
	{
		return Malformed(What);
	}

	return FBinaryReader(Message.subspan(1));
}

[[nodiscard]] FBinaryWriter StartMessage(const EReplicationMessage Type)
{
	FBinaryWriter Writer;
	Writer.Write(static_cast<std::uint8_t>(Type));
	return Writer;
}

[[nodiscard]] bool Finished(const FBinaryReader& Reader) noexcept
{
	return Reader.IsValid() && Reader.IsAtEnd();
}

// Button presses and holds as bits, in FGameInput's order.
[[nodiscard]] std::uint16_t PackButtons(const FGameInput& Input) noexcept
{
	const std::array Buttons{Input.bJump, Input.bSprint, Input.bCrouch, Input.bThrow, Input.bToggleCamera, Input.bToggleRagdoll, Input.bAttack, Input.bBlock};
	std::uint16_t Bits = 0;
	for (std::size_t Index = 0; Index < Buttons.size(); ++Index)
	{
		Bits |= static_cast<std::uint16_t>(Buttons[Index] ? 1u << Index : 0u);
	}

	return Bits;
}

void UnpackButtons(const std::uint16_t Bits, FGameInput& Input) noexcept
{
	const auto Bit = [Bits](const int Index)
	{
		return (Bits & (1u << Index)) != 0;
	};

	Input.bJump = Bit(0);
	Input.bSprint = Bit(1);
	Input.bCrouch = Bit(2);
	Input.bThrow = Bit(3);
	Input.bToggleCamera = Bit(4);
	Input.bToggleRagdoll = Bit(5);
	Input.bAttack = Bit(6);
	Input.bBlock = Bit(7);
}

void WriteMover(FBinaryWriter& Writer, const FMoverSnapshot& Mover)
{
	const FMoverState& State = Mover.State;
	WriteVector(Writer, State.Foot);
	WriteVector(Writer, State.PreviousFoot);
	WriteVector(Writer, State.Velocity);
	WriteVector(Writer, State.Ground.FootPosition);
	WriteVector(Writer, State.Ground.Velocity);
	WriteVector(Writer, State.Ground.GroundVelocity);
	WriteVector(Writer, State.Ground.GroundNormal);
	WriteVector(Writer, State.Ground.WallNormal);
	const std::array Flags{State.Ground.bGrounded, State.Ground.bTouchingWall, State.bGrounded, State.bCrouched, State.bCrouchPressed, State.bJumpedSinceGround, Mover.bCrouchWasHeld};
	std::uint8_t Bits = 0;
	for (std::size_t Index = 0; Index < Flags.size(); ++Index)
	{
		Bits |= static_cast<std::uint8_t>(Flags[Index] ? 1u << Index : 0u);
	}

	Writer.Write(Bits);
	Writer.WriteFloat(State.AirborneSeconds);
	Writer.WriteFloat(State.JumpBufferSeconds);
	WriteVector(Writer, State.LastJumpFoot);
	Writer.Write(static_cast<std::uint8_t>(State.Takeoff));
	Writer.WriteFloat(State.TakeoffSpeed);
	WriteOptionalVector(Writer, State.LastWallNormal);
	WriteOptionalVector(Writer, State.WallRunNormal);
	Writer.Write(static_cast<std::uint32_t>(State.ChainedWallRuns));
	Writer.WriteFloat(State.AccelerationSeconds);
	Writer.WriteFloat(State.StoppingSeconds);
	Writer.Write(static_cast<std::uint8_t>(Mover.Active));
	Writer.Write(static_cast<std::uint8_t>(Mover.Modes.size()));
	for (const std::vector<float>& Mode : Mover.Modes)
	{
		Writer.Write(static_cast<std::uint8_t>(Mode.size()));
		for (const float Value : Mode)
		{
			Writer.WriteFloat(Value);
		}
	}
}

[[nodiscard]] FMoverSnapshot ReadMover(FBinaryReader& Reader)
{
	FMoverSnapshot Mover;
	FMoverState& State = Mover.State;
	State.Foot = ReadVector(Reader);
	State.PreviousFoot = ReadVector(Reader);
	State.Velocity = ReadVector(Reader);
	State.Ground.FootPosition = ReadVector(Reader);
	State.Ground.Velocity = ReadVector(Reader);
	State.Ground.GroundVelocity = ReadVector(Reader);
	State.Ground.GroundNormal = ReadVector(Reader);
	State.Ground.WallNormal = ReadVector(Reader);
	const auto Bits = Reader.Read<std::uint8_t>();
	State.Ground.bGrounded = (Bits & 1u) != 0;
	State.Ground.bTouchingWall = (Bits & 2u) != 0;
	State.bGrounded = (Bits & 4u) != 0;
	State.bCrouched = (Bits & 8u) != 0;
	State.bCrouchPressed = (Bits & 16u) != 0;
	State.bJumpedSinceGround = (Bits & 32u) != 0;
	Mover.bCrouchWasHeld = (Bits & 64u) != 0;
	State.AirborneSeconds = Reader.ReadFloat();
	State.JumpBufferSeconds = Reader.ReadFloat();
	State.LastJumpFoot = ReadVector(Reader);
	const auto Takeoff = Reader.Read<std::uint8_t>();
	if (Takeoff > static_cast<std::uint8_t>(EMoverTakeoff::WallJump))
	{
		Reader.Invalidate();
	}

	State.Takeoff = static_cast<EMoverTakeoff>(Takeoff);
	State.TakeoffSpeed = Reader.ReadFloat();
	State.LastWallNormal = ReadOptionalVector(Reader);
	State.WallRunNormal = ReadOptionalVector(Reader);
	State.ChainedWallRuns = static_cast<std::int32_t>(Reader.Read<std::uint32_t>());
	State.AccelerationSeconds = Reader.ReadFloat();
	State.StoppingSeconds = Reader.ReadFloat();
	Mover.Active = Reader.Read<std::uint8_t>();
	const auto ModeCount = Reader.Read<std::uint8_t>();
	if (ModeCount > MaximumModeCount)
	{
		Reader.Invalidate();
		return Mover;
	}

	for (std::size_t Mode = 0; Mode < ModeCount && Reader.IsValid(); ++Mode)
	{
		const auto ValueCount = Reader.Read<std::uint8_t>();
		if (ValueCount > MaximumModeValues || !Reader.CanRead(ValueCount, sizeof(float)))
		{
			Reader.Invalidate();
			return Mover;
		}

		std::vector<float>& Values = Mover.Modes.emplace_back();
		for (std::size_t Value = 0; Value < ValueCount; ++Value)
		{
			Values.push_back(Reader.ReadFloat());
		}
	}

	return Mover;
}

[[nodiscard]] bool IsFinite(const FMoverSnapshot& Mover) noexcept
{
	const FMoverState& State = Mover.State;
	const bool bVectors = IsFinite(State.Foot) && IsFinite(State.PreviousFoot) && IsFinite(State.Velocity) && IsFinite(State.Ground.FootPosition) && IsFinite(State.Ground.Velocity) && IsFinite(State.Ground.GroundVelocity)
	    && IsFinite(State.Ground.GroundNormal) && IsFinite(State.Ground.WallNormal) && IsFinite(State.LastJumpFoot) && (!State.LastWallNormal || IsFinite(*State.LastWallNormal)) && (!State.WallRunNormal || IsFinite(*State.WallRunNormal));
	const bool bScalars = std::isfinite(State.AirborneSeconds) && std::isfinite(State.JumpBufferSeconds) && std::isfinite(State.TakeoffSpeed) && std::isfinite(State.AccelerationSeconds) && std::isfinite(State.StoppingSeconds);
	return bVectors && bScalars && std::ranges::all_of(Mover.Modes, [](const std::vector<float>& Mode)
	{
		return std::ranges::all_of(Mode, [](const float Value)
		{
			return std::isfinite(Value);
		});
	});
}

// The XOR of a state with its baseline, as alternating runs of unchanged and changed bytes: a moving player changes a few dozen of its bytes
// between snapshots, and a resting body none.
void WriteDelta(FBinaryWriter& Writer, const std::span<const std::byte> State, const std::span<const std::byte> Baseline)
{
	const auto At = [&](const std::size_t Index)
	{
		return Index < Baseline.size() ? State[Index] ^ Baseline[Index] : State[Index];
	};

	WriteVarint(Writer, static_cast<std::uint32_t>(State.size()));
	std::size_t Index = 0;
	while (Index < State.size())
	{
		const std::size_t ZeroStart = Index;
		while (Index < State.size() && At(Index) == std::byte{0})
		{
			++Index;
		}

		const std::size_t LiteralStart = Index;
		while (Index < State.size() && At(Index) != std::byte{0})
		{
			++Index;
		}

		WriteVarint(Writer, static_cast<std::uint32_t>(LiteralStart - ZeroStart));
		WriteVarint(Writer, static_cast<std::uint32_t>(Index - LiteralStart));
		for (std::size_t Literal = LiteralStart; Literal < Index; ++Literal)
		{
			Writer.Write(static_cast<std::uint8_t>(At(Literal)));
		}
	}
}

[[nodiscard]] std::vector<std::byte> ReadDelta(FBinaryReader& Reader, const std::span<const std::byte> Baseline)
{
	const std::uint32_t Size = ReadVarint(Reader);
	if (Size > MaximumEntityStateBytes || (!Baseline.empty() && Baseline.size() != Size))
	{
		Reader.Invalidate();
		return {};
	}

	std::vector<std::byte> State(Baseline.begin(), Baseline.end());
	State.resize(Size);
	std::size_t Index = 0;
	while (Index < Size && Reader.IsValid())
	{
		const std::uint32_t Zeros = ReadVarint(Reader);
		const std::uint32_t Literals = ReadVarint(Reader);
		if (Zeros > Size - Index || Literals > Size - Index - Zeros || Zeros + Literals == 0)
		{
			Reader.Invalidate();
			return {};
		}

		Index += Zeros;
		for (std::uint32_t Literal = 0; Literal < Literals; ++Literal, ++Index)
		{
			State[Index] ^= static_cast<std::byte>(Reader.Read<std::uint8_t>());
		}
	}

	return State;
}
}

std::expected<std::uint64_t, FReplicationError> HashLevelFile(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		return std::unexpected(FReplicationError{std::format("Cannot read {}", Path.generic_string())});
	}

	const std::vector<char> Bytes{std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
	const FHash128 Hash = HashBytes(std::as_bytes(std::span(Bytes)));
	return Hash.Low ^ Hash.High;
}

std::optional<EReplicationMessage> PeekMessage(const std::span<const std::byte> Message) noexcept
{
	if (Message.empty() || Message.size() > MaximumMessageBytes)
	{
		return std::nullopt;
	}

	const auto Type = static_cast<std::uint8_t>(Message.front());
	if (Type < static_cast<std::uint8_t>(EReplicationMessage::Hello) || Type > static_cast<std::uint8_t>(EReplicationMessage::Outcome))
	{
		return std::nullopt;
	}

	return static_cast<EReplicationMessage>(Type);
}

std::vector<std::byte> EncodeHello(const FReplicationHello& Hello)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Hello);
	Writer.Write(ReplicationMagic);
	Writer.Write(Hello.Version);
	Writer.Write(Hello.ContentHash);
	Writer.Write(Hello.ReconnectToken);
	Writer.WriteString(Hello.Name.substr(0, MaximumPlayerNameLength));
	return Writer.TakeBytes();
}

std::expected<FReplicationHello, FReplicationError> DecodeHello(const std::span<const std::byte> Message)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Hello, "hello");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	if (Reader->Read<std::uint32_t>() != ReplicationMagic)
	{
		return Malformed("hello: not a Herta client");
	}

	FReplicationHello Hello;
	Hello.Version = Reader->Read<std::uint16_t>();
	Hello.ContentHash = Reader->Read<std::uint64_t>();
	Hello.ReconnectToken = Reader->Read<std::uint64_t>();
	Hello.Name = Reader->ReadString(MaximumPlayerNameLength);
	if (!Finished(*Reader))
	{
		return Malformed("hello");
	}

	return Hello;
}

std::vector<std::byte> EncodeWelcome(const FReplicationWelcome& Welcome)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Welcome);
	Writer.Write(Welcome.PlayerId);
	Writer.Write(Welcome.Tick);
	Writer.Write(Welcome.ReconnectToken);
	return Writer.TakeBytes();
}

std::expected<FReplicationWelcome, FReplicationError> DecodeWelcome(const std::span<const std::byte> Message)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Welcome, "welcome");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	FReplicationWelcome Welcome;
	Welcome.PlayerId = Reader->Read<std::uint32_t>();
	Welcome.Tick = Reader->Read<std::uint32_t>();
	Welcome.ReconnectToken = Reader->Read<std::uint64_t>();
	if (!Finished(*Reader))
	{
		return Malformed("welcome");
	}

	return Welcome;
}

std::vector<std::byte> EncodeReject(const FReplicationReject& Reject)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Reject);
	Writer.WriteString(Reject.Reason.substr(0, MaximumRejectLength));
	return Writer.TakeBytes();
}

std::expected<FReplicationReject, FReplicationError> DecodeReject(const std::span<const std::byte> Message)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Reject, "reject");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	FReplicationReject Reject{.Reason = Reader->ReadString(MaximumRejectLength)};
	if (!Finished(*Reader))
	{
		return Malformed("reject");
	}

	return Reject;
}

std::vector<std::byte> EncodeInput(const FReplicationInput& Input)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Input);
	Writer.Write(Input.AckedSnapshot);
	Writer.Write(Input.AckMask);
	const std::size_t Count = std::min(Input.Frames.size(), InputRedundancy);
	Writer.Write(static_cast<std::uint8_t>(Count));
	Writer.Write(Count > 0 ? Input.Frames[Input.Frames.size() - Count].Tick : 0u);
	for (std::size_t Index = Input.Frames.size() - Count; Index < Input.Frames.size(); ++Index)
	{
		const FGameInput& Frame = Input.Frames[Index].Input;
		Writer.Write(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(std::clamp(Frame.Move.X, -1.f, 1.f) * 127.f))));
		Writer.Write(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(std::clamp(Frame.Move.Y, -1.f, 1.f) * 127.f))));
		Writer.WriteFloat(Frame.YawDelta);
		Writer.WriteFloat(Frame.PitchDelta);
		Writer.Write(PackButtons(Frame));
	}

	return Writer.TakeBytes();
}

std::expected<FReplicationInput, FReplicationError> DecodeInput(const std::span<const std::byte> Message)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Input, "input");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	FReplicationInput Input;
	Input.AckedSnapshot = Reader->Read<std::uint32_t>();
	Input.AckMask = Reader->Read<std::uint32_t>();
	const auto Count = Reader->Read<std::uint8_t>();
	const auto FirstTick = Reader->Read<std::uint32_t>();
	if (Count > InputRedundancy)
	{
		return Malformed("input: too many frames");
	}

	for (std::uint32_t Index = 0; Index < Count; ++Index)
	{
		FReplicationInputFrame& Frame = Input.Frames.emplace_back();
		Frame.Tick = FirstTick + Index;
		Frame.Input.Move = {static_cast<float>(static_cast<std::int8_t>(Reader->Read<std::uint8_t>())) / 127.f, static_cast<float>(static_cast<std::int8_t>(Reader->Read<std::uint8_t>())) / 127.f};
		Frame.Input.YawDelta = Reader->ReadFloat();
		Frame.Input.PitchDelta = Reader->ReadFloat();
		UnpackButtons(Reader->Read<std::uint16_t>(), Frame.Input);
		// A look turn beyond a half circle in one tick is no mouse, and a non-finite one would poison the simulation.
		if (!std::isfinite(Frame.Input.YawDelta) || !std::isfinite(Frame.Input.PitchDelta) || std::abs(Frame.Input.YawDelta) > 3.2f || std::abs(Frame.Input.PitchDelta) > 3.2f)
		{
			return Malformed("input: impossible look");
		}
	}

	if (!Finished(*Reader) || (Count > 0 && FirstTick > std::numeric_limits<std::uint32_t>::max() - Count))
	{
		return Malformed("input");
	}

	return Input;
}

std::vector<std::byte> EncodeSnapshot(const FReplicationSnapshot& Snapshot, const FBaselineLookup& Lookup)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Snapshot);
	Writer.Write(Snapshot.Sequence);
	Writer.Write(Snapshot.Tick);
	Writer.Write(Snapshot.PlayerId);
	Writer.Write(Snapshot.AppliedInputTick);
	WriteVarint(Writer, static_cast<std::uint32_t>(Snapshot.Entities.size()));
	for (const FReplicatedEntity& Entity : Snapshot.Entities)
	{
		const std::vector<std::byte>* const Baseline = Entity.BaselineSequence != 0 && Lookup ? Lookup(Entity.Id, Entity.BaselineSequence) : nullptr;
		const bool bDelta = Baseline && Baseline->size() == Entity.State.size() && Snapshot.Sequence > Entity.BaselineSequence && Snapshot.Sequence - Entity.BaselineSequence <= SnapshotHistory;
		WriteVarint(Writer, Entity.Id);
		Writer.Write(static_cast<std::uint8_t>(Entity.Kind));
		Writer.Write(static_cast<std::uint8_t>(bDelta ? Snapshot.Sequence - Entity.BaselineSequence : 0));
		WriteDelta(Writer, Entity.State, bDelta ? std::span<const std::byte>(*Baseline) : std::span<const std::byte>{});
	}

	WriteVarint(Writer, static_cast<std::uint32_t>(Snapshot.Removed.size()));
	for (const std::uint32_t Id : Snapshot.Removed)
	{
		WriteVarint(Writer, Id);
	}

	return Writer.TakeBytes();
}

std::expected<FReplicationSnapshot, FReplicationError> DecodeSnapshot(const std::span<const std::byte> Message, const FBaselineLookup& Lookup)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Snapshot, "snapshot");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	FReplicationSnapshot Snapshot;
	Snapshot.Sequence = Reader->Read<std::uint32_t>();
	Snapshot.Tick = Reader->Read<std::uint32_t>();
	Snapshot.PlayerId = Reader->Read<std::uint32_t>();
	Snapshot.AppliedInputTick = Reader->Read<std::uint32_t>();
	const std::uint32_t Count = ReadVarint(*Reader);
	if (Count > MaximumSnapshotEntities || Snapshot.Sequence == 0)
	{
		return Malformed("snapshot");
	}

	for (std::uint32_t Index = 0; Index < Count && Reader->IsValid(); ++Index)
	{
		FReplicatedEntity& Entity = Snapshot.Entities.emplace_back();
		Entity.Id = ReadVarint(*Reader);
		const auto Kind = Reader->Read<std::uint8_t>();
		const auto Back = Reader->Read<std::uint8_t>();
		if (Kind < static_cast<std::uint8_t>(EReplicatedKind::Player) || Kind > static_cast<std::uint8_t>(EReplicatedKind::Npc) || Back > SnapshotHistory || Back >= Snapshot.Sequence)
		{
			return Malformed("snapshot entity");
		}

		Entity.Kind = static_cast<EReplicatedKind>(Kind);
		Entity.BaselineSequence = Back == 0 ? 0 : Snapshot.Sequence - Back;
		const std::vector<std::byte>* const Baseline = Back != 0 && Lookup ? Lookup(Entity.Id, Entity.BaselineSequence) : nullptr;
		if (Back != 0 && !Baseline)
		{
			return std::unexpected(FReplicationError{std::format("The snapshot refers to a baseline {} this peer no longer has", Entity.BaselineSequence)});
		}

		Entity.State = ReadDelta(*Reader, Baseline ? std::span<const std::byte>(*Baseline) : std::span<const std::byte>{});
	}

	const std::uint32_t Removed = ReadVarint(*Reader);
	if (Removed > MaximumSnapshotEntities)
	{
		return Malformed("snapshot removals");
	}

	for (std::uint32_t Index = 0; Index < Removed && Reader->IsValid(); ++Index)
	{
		Snapshot.Removed.push_back(ReadVarint(*Reader));
	}

	if (!Finished(*Reader))
	{
		return Malformed("snapshot");
	}

	return Snapshot;
}

std::vector<std::byte> EncodeOutcome(const FReplicationOutcome& Outcome)
{
	FBinaryWriter Writer = StartMessage(EReplicationMessage::Outcome);
	Writer.Write(Outcome.Tick);
	Writer.Write(static_cast<std::uint8_t>(Outcome.Outcome.Kind));
	Writer.Write(Outcome.Outcome.Object);
	WriteVarint(Writer, static_cast<std::uint32_t>(Outcome.Outcome.IntactBonds.size()));
	for (std::size_t Index = 0; Index < Outcome.Outcome.IntactBonds.size(); Index += 8)
	{
		std::uint8_t Bits = 0;
		for (std::size_t Bit = 0; Bit < 8 && Index + Bit < Outcome.Outcome.IntactBonds.size(); ++Bit)
		{
			Bits |= static_cast<std::uint8_t>(Outcome.Outcome.IntactBonds[Index + Bit] ? 1u << Bit : 0u);
		}

		Writer.Write(Bits);
	}

	WriteVector(Writer, Outcome.Outcome.Point);
	WriteVector(Writer, Outcome.Outcome.Normal);
	Writer.WriteString(Outcome.Outcome.Joint.substr(0, MaximumJointNameLength));
	return Writer.TakeBytes();
}

std::expected<FReplicationOutcome, FReplicationError> DecodeOutcome(const std::span<const std::byte> Message)
{
	auto Reader = BeginMessage(Message, EReplicationMessage::Outcome, "outcome");
	if (!Reader)
	{
		return std::unexpected(Reader.error());
	}

	FReplicationOutcome Outcome;
	Outcome.Tick = Reader->Read<std::uint32_t>();
	const auto Kind = Reader->Read<std::uint8_t>();
	Outcome.Outcome.Object = Reader->Read<std::uint32_t>();
	const std::uint32_t Bonds = ReadVarint(*Reader);
	if (Kind > static_cast<std::uint8_t>(EGameOutcome::Kill) || Bonds > MaximumBonds || !Reader->CanRead((Bonds + 7) / 8, 1))
	{
		return Malformed("outcome");
	}

	Outcome.Outcome.Kind = static_cast<EGameOutcome>(Kind);
	Outcome.Outcome.IntactBonds.resize(Bonds);
	for (std::uint32_t Index = 0; Index < Bonds; Index += 8)
	{
		const auto Bits = Reader->Read<std::uint8_t>();
		for (std::uint32_t Bit = 0; Bit < 8 && Index + Bit < Bonds; ++Bit)
		{
			Outcome.Outcome.IntactBonds[Index + Bit] = (Bits & (1u << Bit)) != 0;
		}
	}

	Outcome.Outcome.Point = ReadVector(*Reader);
	Outcome.Outcome.Normal = ReadVector(*Reader);
	Outcome.Outcome.Joint = Reader->ReadString(MaximumJointNameLength);
	if (!Finished(*Reader) || !IsFinite(Outcome.Outcome.Point) || !IsFinite(Outcome.Outcome.Normal))
	{
		return Malformed("outcome");
	}

	return Outcome;
}

std::vector<std::byte> EncodePlayer(const FGamePlayerReplica& Player)
{
	FBinaryWriter Writer;
	WriteMover(Writer, Player.Mover);
	Writer.WriteFloat(Player.Yaw);
	Writer.WriteFloat(Player.Pitch);
	Writer.Write(static_cast<std::uint8_t>(Player.Objective.State));
	WriteDouble(Writer, Player.Objective.ElapsedSeconds);
	Writer.Write(static_cast<std::uint8_t>(Player.Objective.BestSeconds ? 1 : 0));
	WriteDouble(Writer, Player.Objective.BestSeconds.value_or(0.));
	Writer.Write(Player.Objective.Respawns);
	Writer.Write(static_cast<std::uint8_t>((Player.bRagdolled ? 1 : 0) | (Player.bBlocking ? 2 : 0)));
	Writer.Write(Player.Attacks);
	return Writer.TakeBytes();
}

std::expected<FGamePlayerReplica, FReplicationError> DecodePlayer(const std::span<const std::byte> State)
{
	FBinaryReader Reader(State);
	FGamePlayerReplica Player;
	Player.Mover = ReadMover(Reader);
	Player.Yaw = Reader.ReadFloat();
	Player.Pitch = Reader.ReadFloat();
	const auto Objective = Reader.Read<std::uint8_t>();
	Player.Objective.State = static_cast<EGameObjectiveState>(Objective);
	Player.Objective.ElapsedSeconds = ReadDouble(Reader);
	const auto bBest = Reader.Read<std::uint8_t>();
	const double Best = ReadDouble(Reader);
	Player.Objective.BestSeconds = bBest == 1 ? std::optional(Best) : std::nullopt;
	Player.Objective.Respawns = Reader.Read<std::uint32_t>();
	const auto Flags = Reader.Read<std::uint8_t>();
	Player.bRagdolled = (Flags & 1u) != 0;
	Player.bBlocking = (Flags & 2u) != 0;
	Player.Attacks = Reader.Read<std::uint32_t>();
	if (!Finished(Reader) || Objective > static_cast<std::uint8_t>(EGameObjectiveState::Finished) || bBest > 1 || Flags > 3 || !IsFinite(Player.Mover) || !std::isfinite(Player.Yaw) || !std::isfinite(Player.Pitch)
	    || !std::isfinite(Player.Objective.ElapsedSeconds) || !std::isfinite(Best))
	{
		return Malformed("player");
	}

	return Player;
}

std::vector<std::byte> EncodeBody(const FLevelBodyReplica& Body)
{
	FBinaryWriter Writer;
	Writer.Write(Body.Body);
	WriteVector(Writer, Body.Transform.Position);
	WriteQuaternion(Writer, Body.Transform.Rotation);
	WriteVector(Writer, Body.LinearVelocity);
	WriteVector(Writer, Body.AngularVelocity);
	return Writer.TakeBytes();
}

std::expected<FLevelBodyReplica, FReplicationError> DecodeBody(const std::span<const std::byte> State)
{
	FBinaryReader Reader(State);
	FLevelBodyReplica Body;
	Body.Body = Reader.Read<std::uint32_t>();
	Body.Transform.Position = ReadVector(Reader);
	Body.Transform.Rotation = ReadQuaternion(Reader);
	Body.LinearVelocity = ReadVector(Reader);
	Body.AngularVelocity = ReadVector(Reader);
	if (!Finished(Reader) || !IsFinite(Body.Transform.Position) || !IsFinite(Body.Transform.Rotation) || !IsFinite(Body.LinearVelocity) || !IsFinite(Body.AngularVelocity))
	{
		return Malformed("body");
	}

	return Body;
}

std::vector<std::byte> EncodeProjectile(const FGameProjectileReplica& Projectile)
{
	FBinaryWriter Writer;
	Writer.Write(Projectile.Id);
	WriteVector(Writer, Projectile.Transform.Position);
	WriteQuaternion(Writer, Projectile.Transform.Rotation);
	WriteVector(Writer, Projectile.LinearVelocity);
	return Writer.TakeBytes();
}

std::expected<FGameProjectileReplica, FReplicationError> DecodeProjectile(const std::span<const std::byte> State)
{
	FBinaryReader Reader(State);
	FGameProjectileReplica Projectile;
	Projectile.Id = Reader.Read<std::uint32_t>();
	Projectile.Transform.Position = ReadVector(Reader);
	Projectile.Transform.Rotation = ReadQuaternion(Reader);
	Projectile.LinearVelocity = ReadVector(Reader);
	if (!Finished(Reader) || !IsFinite(Projectile.Transform.Position) || !IsFinite(Projectile.Transform.Rotation) || !IsFinite(Projectile.LinearVelocity))
	{
		return Malformed("projectile");
	}

	return Projectile;
}

std::vector<std::byte> EncodeNpc(const FGameNpcReplica& Npc)
{
	FBinaryWriter Writer;
	Writer.Write(Npc.Character);
	WriteVector(Writer, Npc.Foot);
	Writer.WriteFloat(Npc.Yaw);
	Writer.WriteFloat(Npc.GroundSpeed);
	Writer.WriteFloat(Npc.Health);
	Writer.Write(Npc.Attacks);
	return Writer.TakeBytes();
}

std::expected<FGameNpcReplica, FReplicationError> DecodeNpc(const std::span<const std::byte> State)
{
	FBinaryReader Reader(State);
	FGameNpcReplica Npc;
	Npc.Character = Reader.Read<std::uint32_t>();
	Npc.Foot = ReadVector(Reader);
	Npc.Yaw = Reader.ReadFloat();
	Npc.GroundSpeed = Reader.ReadFloat();
	Npc.Health = Reader.ReadFloat();
	Npc.Attacks = Reader.Read<std::uint32_t>();
	if (!Finished(Reader) || !IsFinite(Npc.Foot) || !std::isfinite(Npc.Yaw) || !std::isfinite(Npc.GroundSpeed) || !std::isfinite(Npc.Health))
	{
		return Malformed("character");
	}

	return Npc;
}
}
