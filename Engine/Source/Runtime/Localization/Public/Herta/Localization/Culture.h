#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
enum class EPluralCategory : std::uint8_t
{
	Zero,
	One,
	Two,
	Few,
	Many,
	Other,
};

// CLDR's plural operands of a number as it is shown: N its absolute value, I its integer digits, V how many fraction digits are shown and W
// without trailing zeros, F the shown fraction digits and T without trailing zeros. "1.50" has V 2, W 1, F 50, T 5.
struct FPluralOperands
{
	double N = 0.;
	std::uint64_t I = 0;
	std::uint64_t V = 0;
	std::uint64_t W = 0;
	std::uint64_t F = 0;
	std::uint64_t T = 0;
};

// The operands of Value shown with exactly FractionDigits digits after the point, as FormatNumber shows it.
[[nodiscard]] FPluralOperands GetPluralOperands(double Value, std::uint32_t FractionDigits);

struct FNumberFormat
{
	std::uint32_t MinimumFractionDigits = 0;
	std::uint32_t MaximumFractionDigits = 3;
	bool bGrouping = true;
	// Shows Value times 100 with the culture's percent pattern.
	bool bPercent = false;
};

enum class EDateTimeStyle : std::uint8_t
{
	Full,
	Long,
	Medium,
	Short,
};

// A calendar date and wall-clock time in whatever zone the caller means; formatting never converts zones.
struct FCivilTime
{
	bool operator==(const FCivilTime&) const = default;

	std::int32_t Year = 1970;
	// 1 to 12.
	std::uint32_t Month = 1;
	std::uint32_t Day = 1;
	std::uint32_t Hour = 0;
	std::uint32_t Minute = 0;
	std::uint32_t Second = 0;
};

// Seconds since 1970-01-01 00:00 in the same zone as the result.
[[nodiscard]] FCivilTime ToCivilTime(std::int64_t Seconds) noexcept;

namespace Cldr
{
struct FLocale;
}

// A culture's plural rules and number and date formats, from the CLDR data generated for the cultures Herta ships. Cultures live for the
// whole program.
class FCulture final
{
public:
	// The culture for a BCP 47 tag such as pl-PL: the exact tag, then its language; null when Herta has neither.
	[[nodiscard]] static const FCulture* Find(std::string_view Tag);
	[[nodiscard]] static std::span<const FCulture> GetAll();

	explicit FCulture(const Cldr::FLocale& InLocale);
	~FCulture();

	FCulture(const FCulture&) = delete;
	FCulture& operator=(const FCulture&) = delete;
	FCulture(FCulture&&) noexcept;
	FCulture& operator=(FCulture&&) = delete;

	[[nodiscard]] std::string_view GetName() const noexcept;
	// The culture to fall back to before the source language, or null.
	[[nodiscard]] const FCulture* GetParent() const;
	[[nodiscard]] EPluralCategory SelectPlural(const FPluralOperands& Operands) const;
	// The category of Value as FormatNumber shows it with Format.
	[[nodiscard]] EPluralCategory SelectPlural(double Value, const FNumberFormat& Format = {}) const;
	[[nodiscard]] std::string FormatNumber(double Value, const FNumberFormat& Format = {}) const;
	[[nodiscard]] std::string FormatDate(const FCivilTime& Time, EDateTimeStyle Style) const;
	// Full and long times name a time zone, which a civil time has none of, so they show as medium.
	[[nodiscard]] std::string FormatTime(const FCivilTime& Time, EDateTimeStyle Style) const;
	[[nodiscard]] std::string FormatDateTime(const FCivilTime& Time, EDateTimeStyle Style) const;

private:
	struct FRules;

	[[nodiscard]] std::string FormatPattern(std::string_view Pattern, const FCivilTime& Time) const;

	const Cldr::FLocale* Locale = nullptr;
	std::unique_ptr<FRules> Rules;
};
}
