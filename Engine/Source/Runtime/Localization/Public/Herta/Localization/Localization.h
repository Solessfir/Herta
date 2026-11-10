#pragma once

#include "Herta/Localization/MessageFormat.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// The source culture every catalog falls back to.
inline constexpr std::string_view SourceCulture = "en";
// Shows source text with accents and longer words, to find unlocalized and clipped text without a translation.
inline constexpr std::string_view PseudoCulture = "qps-ploc";

// A message's identity: the 64-bit FNV-1a hash of its full key, Namespace.Key, computed at compile time for generated IDs.
struct FTextId
{
	consteval explicit FTextId(const char* InKey)
	    : FTextId(std::string_view(InKey), 0)
	{
	}

	[[nodiscard]] static constexpr FTextId FromKey(std::string_view InKey) noexcept;

	std::uint64_t Hash = 0;
	std::string_view Key{};

private:
	constexpr FTextId(std::string_view InKey, int) noexcept;
};

[[nodiscard]] constexpr std::uint64_t HashTextKey(const std::string_view Key) noexcept
{
	std::uint64_t Hash = 0xcbf29ce484222325ull;
	for (const char Character : Key)
	{
		Hash = (Hash ^ static_cast<std::uint8_t>(Character)) * 0x100000001b3ull;
	}

	return Hash;
}

constexpr FTextId::FTextId(const std::string_view InKey, int) noexcept
    : Hash(HashTextKey(InKey))
    , Key(InKey)
{
}

constexpr FTextId FTextId::FromKey(const std::string_view InKey) noexcept
{
	return FTextId(InKey, 0);
}

struct FCatalogMessage
{
	std::string Key;
	std::string Text;
	// Context for translators; only source catalogs carry it.
	std::string Comment;
};

// One culture's messages for one namespace, stored as canonical UTF-8 JSON (.hloc).
struct FTextCatalog
{
	std::string Culture;
	std::string Namespace;
	std::vector<FCatalogMessage> Messages;
};

[[nodiscard]] std::expected<FTextCatalog, FLocalizationError> ParseTextCatalog(std::string_view Json);
// Messages sorted by key, so the same catalog always writes the same bytes.
[[nodiscard]] std::string SerializeTextCatalog(const FTextCatalog& Catalog);
// Problems that make Translation unusable against Source: messages that do not compile, keys the source lacks, or arguments that differ.
// Keys the translation lacks are not problems; they fall back to the source.
[[nodiscard]] std::vector<std::string> ValidateTranslation(const FTextCatalog& Source, const FTextCatalog& Translation);
// Literal text with letters accented and vowels doubled; the pseudo culture applies it to each literal run of a message.
[[nodiscard]] std::string Pseudolocalize(std::string_view Text);

// The current culture and every loaded catalog compiled into messages. Lookups fall back from the culture to its parent and then to the
// source; a message no catalog has shows its key. Used from one thread.
class FLocalization final
{
public:
	FLocalization();
	~FLocalization();

	FLocalization(const FLocalization&) = delete;
	FLocalization& operator=(const FLocalization&) = delete;
	FLocalization(FLocalization&&) noexcept;
	FLocalization& operator=(FLocalization&&) noexcept;

	// Replaces every catalog at once. Invalid input keeps the previous generation and reports why.
	[[nodiscard]] std::expected<void, FLocalizationError> SetCatalogs(std::span<const FTextCatalog> Catalogs);
	// False keeps the current culture when Herta has no culture for the tag.
	bool SetCulture(std::string_view Tag);
	[[nodiscard]] std::string_view GetCultureName() const noexcept;
	[[nodiscard]] const FCulture& GetCulture() const noexcept;
	// Cultures with at least one catalog, the source first.
	[[nodiscard]] std::vector<std::string> GetAvailableCultures() const;
	// Increases whenever the catalogs or the culture change, so cached text knows to refresh.
	[[nodiscard]] std::uint64_t GetGeneration() const noexcept;
	[[nodiscard]] std::string Format(FTextId Id, std::span<const FMessageArgument> Arguments = {}) const;

private:
	struct FState;

	std::unique_ptr<FState> State;
};
}
