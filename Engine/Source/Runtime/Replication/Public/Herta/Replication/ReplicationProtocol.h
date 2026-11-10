#pragma once

#include "Herta/GameFramework/GameSession.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
struct FReplicationError
{
	std::string Message;
};

inline constexpr std::uint32_t ReplicationMagic = 0x41545248;
// Peers with another version refuse each other at the handshake, so a wire change only needs this bumped.
inline constexpr std::uint16_t ReplicationProtocolVersion = 2;
inline constexpr std::uint32_t ReplicationTicksPerSecond = 60;
// Snapshots go out every third tick, 20 a second.
inline constexpr std::uint32_t SnapshotIntervalTicks = 3;
inline constexpr std::size_t MaximumMessageBytes = 64 * 1024;
inline constexpr std::size_t MaximumPlayerNameLength = 32;
// Each input message repeats this many of the newest inputs, so one lost packet costs no input.
inline constexpr std::size_t InputRedundancy = 8;
// How many snapshots back a delta may refer to; both ends keep this much history.
inline constexpr std::uint32_t SnapshotHistory = 64;
inline constexpr std::size_t MaximumSnapshotEntities = 1024;
// Entity ids by kind: players count up from 1, and the others add their session index or server id to a base.
inline constexpr std::uint32_t BodyEntityBase = 0x40000000;
inline constexpr std::uint32_t ProjectileEntityBase = 0x50000000;
inline constexpr std::uint32_t NpcEntityBase = 0x60000000;

enum class EReplicationMessage : std::uint8_t
{
	Hello = 1,
	Welcome,
	Reject,
	Input,
	Snapshot,
	Outcome,
};

// The client's first message. A reconnecting client sends the token its Welcome gave it to take its player back.
struct FReplicationHello
{
	std::uint16_t Version = ReplicationProtocolVersion;
	// Identifies the level and its content, which both ends must share.
	std::uint64_t ContentHash = 0;
	std::uint64_t ReconnectToken = 0;
	std::string Name{};
};

struct FReplicationWelcome
{
	std::uint32_t PlayerId = 0;
	std::uint32_t Tick = 0;
	std::uint64_t ReconnectToken = 0;
};

struct FReplicationReject
{
	std::string Reason{};
};

struct FReplicationInputFrame
{
	std::uint32_t Tick = 0;
	FGameInput Input{};
};

struct FReplicationInput
{
	// The newest snapshot received, and which of the 32 before it arrived too, bit 0 being the one just before.
	std::uint32_t AckedSnapshot = 0;
	std::uint32_t AckMask = 0;
	// Consecutive ticks, oldest first.
	std::vector<FReplicationInputFrame> Frames{};
};

enum class EReplicatedKind : std::uint8_t
{
	Player = 1,
	Body,
	Projectile,
	Npc,
};

// One entity's state in a snapshot. Its id stays the same for as long as the entity exists.
struct FReplicatedEntity
{
	std::uint32_t Id = 0;
	EReplicatedKind Kind = EReplicatedKind::Player;
	// The full encoded state; on the wire it travels as the difference from the baseline.
	std::vector<std::byte> State{};
	// Zero sends the state whole; otherwise a snapshot the receiver acknowledged, holding this entity.
	std::uint32_t BaselineSequence = 0;
};

struct FReplicationSnapshot
{
	std::uint32_t Sequence = 0;
	std::uint32_t Tick = 0;
	// The receiver's player, and its newest input the server has applied.
	std::uint32_t PlayerId = 0;
	std::uint32_t AppliedInputTick = 0;
	// Entities left out are unchanged since their baseline or did not fit; Removed ones are gone or out of range.
	std::vector<FReplicatedEntity> Entities{};
	std::vector<std::uint32_t> Removed{};
};

struct FReplicationOutcome
{
	std::uint32_t Tick = 0;
	FGameOutcome Outcome{};
};

// A received entity's state at a snapshot, which a later delta can refer to, or null when the receiver no longer has it.
using FBaselineLookup = std::function<const std::vector<std::byte>*(std::uint32_t Id, std::uint32_t Sequence)>;

[[nodiscard]] std::optional<EReplicationMessage> PeekMessage(std::span<const std::byte> Message) noexcept;
// The content hash both ends compare at the handshake: the starting level file as staged, which changes with any edit to the level.
[[nodiscard]] std::expected<std::uint64_t, FReplicationError> HashLevelFile(const std::filesystem::path& Path);
// The same content hash for a level already read, such as from a package.
[[nodiscard]] std::uint64_t HashLevelBytes(std::span<const std::byte> Bytes) noexcept;

[[nodiscard]] std::vector<std::byte> EncodeHello(const FReplicationHello& Hello);
[[nodiscard]] std::expected<FReplicationHello, FReplicationError> DecodeHello(std::span<const std::byte> Message);
[[nodiscard]] std::vector<std::byte> EncodeWelcome(const FReplicationWelcome& Welcome);
[[nodiscard]] std::expected<FReplicationWelcome, FReplicationError> DecodeWelcome(std::span<const std::byte> Message);
[[nodiscard]] std::vector<std::byte> EncodeReject(const FReplicationReject& Reject);
[[nodiscard]] std::expected<FReplicationReject, FReplicationError> DecodeReject(std::span<const std::byte> Message);
[[nodiscard]] std::vector<std::byte> EncodeInput(const FReplicationInput& Input);
// Rejects non-finite looks, overlong frame lists, and ticks that do not follow each other.
[[nodiscard]] std::expected<FReplicationInput, FReplicationError> DecodeInput(std::span<const std::byte> Message);
// Each entity's baseline must be the state Lookup returns for it; the encoder only writes the difference.
[[nodiscard]] std::vector<std::byte> EncodeSnapshot(const FReplicationSnapshot& Snapshot, const FBaselineLookup& Lookup);
[[nodiscard]] std::expected<FReplicationSnapshot, FReplicationError> DecodeSnapshot(std::span<const std::byte> Message, const FBaselineLookup& Lookup);
[[nodiscard]] std::vector<std::byte> EncodeOutcome(const FReplicationOutcome& Outcome);
[[nodiscard]] std::expected<FReplicationOutcome, FReplicationError> DecodeOutcome(std::span<const std::byte> Message);

// Entity states. Decoding checks lengths, counts, and that every number is finite, so a hostile peer cannot hand the session garbage.
[[nodiscard]] std::vector<std::byte> EncodePlayer(const FGamePlayerReplica& Player);
[[nodiscard]] std::expected<FGamePlayerReplica, FReplicationError> DecodePlayer(std::span<const std::byte> State);
[[nodiscard]] std::vector<std::byte> EncodeBody(const FLevelBodyReplica& Body);
[[nodiscard]] std::expected<FLevelBodyReplica, FReplicationError> DecodeBody(std::span<const std::byte> State);
[[nodiscard]] std::vector<std::byte> EncodeProjectile(const FGameProjectileReplica& Projectile);
[[nodiscard]] std::expected<FGameProjectileReplica, FReplicationError> DecodeProjectile(std::span<const std::byte> State);
[[nodiscard]] std::vector<std::byte> EncodeNpc(const FGameNpcReplica& Npc);
[[nodiscard]] std::expected<FGameNpcReplica, FReplicationError> DecodeNpc(std::span<const std::byte> State);
}
