#pragma once

#include "Herta/Assets/ContentPackage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FPackageBuildError
{
	std::string Message;
};

// One file for a package: its package path and contents.
struct FPackageInput
{
	std::string Path{};
	std::vector<std::byte> Bytes{};
};

// A publisher's private signing key. It never belongs in source control or a player build; only its public half ships.
struct FPackageSigningKey
{
	std::uint32_t Id = 0;
	// Monocypher's Ed25519 secret key: the 32-byte seed followed by the public key.
	std::array<std::uint8_t, 64> Secret{};
};

struct FPackageBuildOptions
{
	// Zstandard level; chunks that do not shrink are stored whole.
	int CompressionLevel = 19;
	std::optional<FPackageSigningKey> SigningKey{};
};

struct FPackageBuildStats
{
	std::size_t Entries = 0;
	std::uint64_t Bytes = 0;
	std::uint64_t PackageBytes = 0;
};

// Every file under Root, with its path relative to Root, in path order.
[[nodiscard]] std::expected<std::vector<FPackageInput>, FPackageBuildError> CollectPackageInputs(const std::filesystem::path& Root);
// The package bytes for Inputs. The same inputs and options give the same bytes on every run and in any input order.
[[nodiscard]] std::expected<std::vector<std::byte>, FPackageBuildError> BuildPackage(std::vector<FPackageInput> Inputs, const FPackageBuildOptions& Options = {});
// Builds the package and replaces Output with it atomically.
[[nodiscard]] std::expected<FPackageBuildStats, FPackageBuildError> WritePackage(const std::filesystem::path& Output, std::vector<FPackageInput> Inputs, const FPackageBuildOptions& Options = {});

[[nodiscard]] std::expected<FPackageSigningKey, FPackageBuildError> GeneratePackageSigningKey(std::uint32_t Id);
[[nodiscard]] FPackagePublicKey GetPackagePublicKey(const FPackageSigningKey& Key) noexcept;
// A private key file is one line of text: "HertaPackageSigningKey <id> <128 hex digits>". Public keys use FormatPackagePublicKey.
[[nodiscard]] std::string FormatPackageSigningKey(const FPackageSigningKey& Key);
[[nodiscard]] std::expected<FPackageSigningKey, FPackageBuildError> ParsePackageSigningKey(std::string_view Text);
}
