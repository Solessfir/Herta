#include "Herta/Localization/Culture.h"

#include <CldrData.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
// A parsed CLDR plural condition: "or" of "and" of relations such as "i % 10 = 2..4".
struct FRelation
{
	char Operand = 'n';
	std::uint64_t Modulus = 0;
	bool bNegated = false;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> Ranges{};
};

using FAndCondition = std::vector<FRelation>;
using FCondition = std::vector<FAndCondition>;

// Rules are fixed data generated from CLDR and parsed once, so a malformed one is a generation bug; it then never matches.
[[nodiscard]] std::optional<FCondition> ParseCondition(const std::string_view Text)
{
	std::vector<std::string_view> Tokens;
	for (std::size_t Start = 0; Start < Text.size();)
	{
		const std::size_t End = std::min(Text.find(' ', Start), Text.size());
		if (End > Start)
		{
			Tokens.push_back(Text.substr(Start, End - Start));
		}

		Start = End + 1;
	}

	FCondition Condition;
	if (Tokens.empty())
	{
		return Condition;
	}

	FAndCondition And;
	const auto Number = [](const std::string_view Token, std::uint64_t& Value)
	{
		Value = 0;
		if (Token.empty())
		{
			return false;
		}

		for (const char Character : Token)
		{
			if (Character < '0' || Character > '9')
			{
				return false;
			}

			Value = Value * 10 + static_cast<std::uint64_t>(Character - '0');
		}

		return true;
	};

	for (std::size_t Index = 0; Index < Tokens.size();)
	{
		FRelation Relation;
		if (Tokens[Index].size() != 1 || std::string_view("niwvftce").find(Tokens[Index][0]) == std::string_view::npos)
		{
			return std::nullopt;
		}

		Relation.Operand = Tokens[Index++][0];
		if (Index + 1 < Tokens.size() && Tokens[Index] == "%")
		{
			if (!Number(Tokens[Index + 1], Relation.Modulus) || Relation.Modulus == 0)
			{
				return std::nullopt;
			}

			Index += 2;
		}

		if (Index + 1 >= Tokens.size() || (Tokens[Index] != "=" && Tokens[Index] != "!="))
		{
			return std::nullopt;
		}

		Relation.bNegated = Tokens[Index++] == "!=";
		const std::string_view List = Tokens[Index];
		for (std::size_t Start = 0; Start <= List.size();)
		{
			const std::size_t End = std::min(List.find(',', Start), List.size());
			const std::string_view Range = List.substr(Start, End - Start);
			const std::size_t Dots = Range.find("..");
			std::uint64_t Low = 0;
			std::uint64_t High = 0;
			if (!Number(Dots == std::string_view::npos ? Range : Range.substr(0, Dots), Low) || (Dots != std::string_view::npos && (!Number(Range.substr(Dots + 2), High) || High < Low)))
			{
				return std::nullopt;
			}

			Relation.Ranges.emplace_back(Low, Dots == std::string_view::npos ? Low : High);
			Start = End + 1;
		}

		++Index;
		And.push_back(std::move(Relation));
		if (Index == Tokens.size() || Tokens[Index] == "or")
		{
			Condition.push_back(std::move(And));
			And.clear();
		}
		else if (Tokens[Index] != "and")
		{
			return std::nullopt;
		}

		Index += Index < Tokens.size() ? 1 : 0;
	}

	return Condition;
}

[[nodiscard]] bool Holds(const FRelation& Relation, const FPluralOperands& Operands)
{
	// N may have a fraction; a range only ever holds its whole numbers.
	double Value = 0.;
	switch (Relation.Operand)
	{
		case 'n':
			Value = Operands.N;
			break;
		case 'i':
			Value = static_cast<double>(Operands.I);
			break;
		case 'v':
			Value = static_cast<double>(Operands.V);
			break;
		case 'w':
			Value = static_cast<double>(Operands.W);
			break;
		case 'f':
			Value = static_cast<double>(Operands.F);
			break;
		case 't':
			Value = static_cast<double>(Operands.T);
			break;
		default:
			Value = 0.;
			break;
	}

	if (Relation.Modulus != 0)
	{
		Value = std::fmod(Value, static_cast<double>(Relation.Modulus));
	}

	const bool bWhole = Value == std::floor(Value);
	const bool bIn = bWhole && std::ranges::any_of(Relation.Ranges, [Value](const std::pair<std::uint64_t, std::uint64_t>& Range)
	{
		return Value >= static_cast<double>(Range.first) && Value <= static_cast<double>(Range.second);
	});

	return Relation.bNegated ? !bIn : bIn;
}

// The days since 1970-01-01 of a proleptic Gregorian date, after Howard Hinnant's days_from_civil.
[[nodiscard]] std::int64_t DaysFromCivil(std::int64_t Year, const std::uint32_t Month, const std::uint32_t Day) noexcept
{
	Year -= Month <= 2 ? 1 : 0;
	const std::int64_t Era = (Year >= 0 ? Year : Year - 399) / 400;
	const auto YearOfEra = static_cast<std::uint64_t>(Year - Era * 400);
	const std::uint64_t DayOfYear = (153 * (Month > 2 ? Month - 3 : Month + 9) + 2) / 5 + Day - 1;
	const std::uint64_t DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
	return Era * 146097 + static_cast<std::int64_t>(DayOfEra) - 719468;
}

// A number pattern such as "#,##0.###" or "#,##0 %": the text around the number and its grouping sizes.
struct FNumberPattern
{
	std::string Prefix;
	std::string Suffix;
	std::size_t PrimaryGroup = 0;
	std::size_t SecondaryGroup = 0;
	std::size_t MinimumIntegerDigits = 1;
};

[[nodiscard]] FNumberPattern ParseNumberPattern(const std::string_view Pattern)
{
	FNumberPattern Result;
	const std::size_t First = Pattern.find_first_of("#0,.");
	const std::size_t Last = Pattern.find_last_of("#0,.");
	if (First == std::string_view::npos)
	{
		return Result;
	}

	Result.Prefix = Pattern.substr(0, First);
	Result.Suffix = Pattern.substr(Last + 1);
	const std::string_view Number = Pattern.substr(First, Last - First + 1);
	const std::string_view Integer = Number.substr(0, std::min(Number.find('.'), Number.size()));
	Result.MinimumIntegerDigits = std::max<std::size_t>(static_cast<std::size_t>(std::ranges::count(Integer, '0')), 1);
	if (const std::size_t LastComma = Integer.rfind(','); LastComma != std::string_view::npos)
	{
		Result.PrimaryGroup = Integer.size() - LastComma - 1;
		const std::size_t PreviousComma = Integer.rfind(',', LastComma - 1);
		Result.SecondaryGroup = PreviousComma != std::string_view::npos && LastComma > 0 ? LastComma - PreviousComma - 1 : Result.PrimaryGroup;
	}

	return Result;
}

[[nodiscard]] std::size_t CountRepeat(const std::string_view Pattern, const std::size_t Start)
{
	std::size_t Count = 1;
	while (Start + Count < Pattern.size() && Pattern[Start + Count] == Pattern[Start])
	{
		++Count;
	}

	return Count;
}
}

struct FCulture::FRules
{
	std::array<std::optional<FCondition>, 5> Plural;
	FNumberPattern Decimal;
	FNumberPattern Percent;
};

FPluralOperands GetPluralOperands(const double Value, const std::uint32_t FractionDigits)
{
	const std::uint32_t Digits = std::min<std::uint32_t>(FractionDigits, 15);
	const std::string Text = std::format("{:.{}f}", std::abs(Value), Digits);
	FPluralOperands Operands{.V = Digits};
	std::from_chars(Text.data(), Text.data() + Text.size(), Operands.N);
	const std::size_t Point = Text.find('.');
	for (const char Character : std::string_view(Text).substr(0, Point))
	{
		Operands.I = Operands.I * 10 + static_cast<std::uint64_t>(Character - '0');
	}

	if (Point != std::string::npos)
	{
		std::string_view Fraction = std::string_view(Text).substr(Point + 1);
		for (const char Character : Fraction)
		{
			Operands.F = Operands.F * 10 + static_cast<std::uint64_t>(Character - '0');
		}

		while (!Fraction.empty() && Fraction.back() == '0')
		{
			Fraction.remove_suffix(1);
		}

		Operands.W = Fraction.size();
		for (const char Character : Fraction)
		{
			Operands.T = Operands.T * 10 + static_cast<std::uint64_t>(Character - '0');
		}
	}

	return Operands;
}

FCivilTime ToCivilTime(const std::int64_t Seconds) noexcept
{
	// Howard Hinnant's civil_from_days.
	const std::int64_t Days = (Seconds >= 0 ? Seconds : Seconds - 86399) / 86400;
	const std::int64_t SecondOfDay = Seconds - Days * 86400;
	const std::int64_t Shifted = Days + 719468;
	const std::int64_t Era = (Shifted >= 0 ? Shifted : Shifted - 146096) / 146097;
	const auto DayOfEra = static_cast<std::uint64_t>(Shifted - Era * 146097);
	const std::uint64_t YearOfEra = (DayOfEra - DayOfEra / 1460 + DayOfEra / 36524 - DayOfEra / 146096) / 365;
	const std::uint64_t DayOfYear = DayOfEra - (365 * YearOfEra + YearOfEra / 4 - YearOfEra / 100);
	const std::uint64_t MonthIndex = (5 * DayOfYear + 2) / 153;
	const auto Month = static_cast<std::uint32_t>(MonthIndex < 10 ? MonthIndex + 3 : MonthIndex - 9);
	return FCivilTime{
	    .Year = static_cast<std::int32_t>(static_cast<std::int64_t>(YearOfEra) + Era * 400 + (Month <= 2 ? 1 : 0)),
	    .Month = Month,
	    .Day = static_cast<std::uint32_t>(DayOfYear - (153 * MonthIndex + 2) / 5 + 1),
	    .Hour = static_cast<std::uint32_t>(SecondOfDay / 3600),
	    .Minute = static_cast<std::uint32_t>(SecondOfDay / 60 % 60),
	    .Second = static_cast<std::uint32_t>(SecondOfDay % 60),
	};
}

FCulture::FCulture(const Cldr::FLocale& InLocale)
    : Locale(&InLocale)
    , Rules(std::make_unique<FRules>())
{
	for (std::size_t Category = 0; Category < Rules->Plural.size(); ++Category)
	{
		if (!InLocale.PluralRules[Category].empty())
		{
			Rules->Plural[Category] = ParseCondition(InLocale.PluralRules[Category]);
		}
	}

	Rules->Decimal = ParseNumberPattern(InLocale.DecimalPattern);
	Rules->Percent = ParseNumberPattern(InLocale.PercentPattern);
}

FCulture::~FCulture() = default;

FCulture::FCulture(FCulture&&) noexcept = default;

std::span<const FCulture> FCulture::GetAll()
{
	static const std::vector<FCulture> Cultures = []
	{
		std::vector<FCulture> All;
		All.reserve(Cldr::Locales.size());
		for (const Cldr::FLocale& Locale : Cldr::Locales)
		{
			All.emplace_back(Locale);
		}

		return All;
	}();

	return Cultures;
}

const FCulture* FCulture::Find(const std::string_view Tag)
{
	const auto Lower = [](std::string_view Text)
	{
		std::string Result;
		for (const char Character : Text)
		{
			Result.push_back(Character == '_' ? '-' : static_cast<char>(Character >= 'A' && Character <= 'Z' ? Character - 'A' + 'a' : Character));
		}

		return Result;
	};

	const std::string Wanted = Lower(Tag);
	const std::span<const FCulture> All = GetAll();
	for (std::string_view Candidate = Wanted; !Candidate.empty();)
	{
		const auto Found = std::ranges::find_if(All, [&Lower, Candidate](const FCulture& Culture)
		{
			return Lower(Culture.GetName()) == Candidate;
		});

		if (Found != All.end())
		{
			return &*Found;
		}

		const std::size_t Dash = Candidate.rfind('-');
		Candidate = Dash == std::string_view::npos ? std::string_view{} : Candidate.substr(0, Dash);
	}

	return nullptr;
}

std::string_view FCulture::GetName() const noexcept
{
	return Locale->Name;
}

const FCulture* FCulture::GetParent() const
{
	return Locale->Parent.empty() ? nullptr : Find(Locale->Parent);
}

EPluralCategory FCulture::SelectPlural(const FPluralOperands& Operands) const
{
	for (std::size_t Category = 0; Category < Rules->Plural.size(); ++Category)
	{
		const std::optional<FCondition>& Condition = Rules->Plural[Category];
		if (Condition && std::ranges::any_of(*Condition, [&Operands](const FAndCondition& And)
		{
			return std::ranges::all_of(And, [&Operands](const FRelation& Relation)
			{
				return Holds(Relation, Operands);
			});
		}))
		{
			return static_cast<EPluralCategory>(Category);
		}
	}

	return EPluralCategory::Other;
}

EPluralCategory FCulture::SelectPlural(const double Value, const FNumberFormat& Format) const
{
	// The digits shown decide the category: 1 is "one" in English, but 1.0 is not.
	const double Scaled = Format.bPercent ? Value * 100. : Value;
	std::string Shown = std::format("{:.{}f}", std::abs(Scaled), std::min<std::uint32_t>(Format.MaximumFractionDigits, 15));
	const std::size_t Point = Shown.find('.');
	std::uint32_t Digits = Point == std::string::npos ? 0 : static_cast<std::uint32_t>(Shown.size() - Point - 1);
	while (Digits > Format.MinimumFractionDigits && Shown.back() == '0')
	{
		Shown.pop_back();
		--Digits;
	}

	return SelectPlural(GetPluralOperands(Scaled, Digits));
}

std::string FCulture::FormatNumber(const double Value, const FNumberFormat& Format) const
{
	if (!std::isfinite(Value))
	{
		return std::isnan(Value) ? "NaN" : std::string(Value < 0. ? Locale->Minus : "") + "∞";
	}

	const FNumberPattern& Pattern = Format.bPercent ? Rules->Percent : Rules->Decimal;
	const double Scaled = Format.bPercent ? Value * 100. : Value;
	const std::uint32_t Maximum = std::min<std::uint32_t>(std::max(Format.MaximumFractionDigits, Format.MinimumFractionDigits), 15);
	std::string Digits = std::format("{:.{}f}", std::abs(Scaled), Maximum);
	const bool bNegative = Scaled < 0. && Digits.find_first_not_of("0.") != std::string::npos;
	std::string Integer = Digits.substr(0, std::min(Digits.find('.'), Digits.size()));
	std::string Fraction = Digits.find('.') == std::string::npos ? std::string{} : Digits.substr(Digits.find('.') + 1);
	while (Fraction.size() > Format.MinimumFractionDigits && Fraction.back() == '0')
	{
		Fraction.pop_back();
	}

	while (Integer.size() < Pattern.MinimumIntegerDigits)
	{
		Integer.insert(Integer.begin(), '0');
	}

	// Grouping starts only once the integer part reaches a group plus the culture's minimum grouping digits: Polish writes 1234 but 12 345.
	std::string Grouped;
	const std::size_t Primary = Pattern.PrimaryGroup;
	const bool bGroup = Format.bGrouping && Primary > 0 && Integer.size() >= Primary + static_cast<std::size_t>(Locale->MinimumGroupingDigits);
	for (std::size_t Index = 0; Index < Integer.size(); ++Index)
	{
		const std::size_t Remaining = Integer.size() - Index;
		const bool bBoundary = bGroup && Index > 0 && Remaining >= Primary && (Remaining == Primary || (Remaining - Primary) % std::max<std::size_t>(Pattern.SecondaryGroup, 1) == 0);
		if (bBoundary)
		{
			Grouped += Locale->Group;
		}

		Grouped.push_back(Integer[Index]);
	}

	if (!Fraction.empty())
	{
		Grouped += Locale->Decimal;
		Grouped += Fraction;
	}

	const auto Substitute = [this](std::string Text)
	{
		for (std::size_t Found = Text.find('%'); Found != std::string::npos; Found = Text.find('%', Found + Locale->Percent.size()))
		{
			Text.replace(Found, 1, Locale->Percent);
		}

		return Text;
	};

	return std::string(bNegative ? Locale->Minus : "") + Substitute(Pattern.Prefix) + Grouped + Substitute(Pattern.Suffix);
}

std::string FCulture::FormatPattern(const std::string_view Pattern, const FCivilTime& Time) const
{
	const auto Padded = [](const std::uint32_t Value, const std::size_t Width)
	{
		return std::format("{:0{}}", Value, Width);
	};

	const auto Weekday = static_cast<std::size_t>(((DaysFromCivil(Time.Year, Time.Month, Time.Day) % 7) + 11) % 7);
	const std::size_t Month = std::clamp<std::uint32_t>(Time.Month, 1, 12) - 1;
	std::string Result;
	for (std::size_t Index = 0; Index < Pattern.size();)
	{
		const char Letter = Pattern[Index];
		if (Letter == '\'')
		{
			// Quoted literal text; two quotes are one.
			const std::size_t Close = Pattern.find('\'', Index + 1);
			if (Close == Index + 1)
			{
				Result.push_back('\'');
				Index += 2;
				continue;
			}

			Result += Pattern.substr(Index + 1, (Close == std::string_view::npos ? Pattern.size() : Close) - Index - 1);
			Index = Close == std::string_view::npos ? Pattern.size() : Close + 1;
			continue;
		}

		if (!((Letter >= 'a' && Letter <= 'z') || (Letter >= 'A' && Letter <= 'Z')))
		{
			Result.push_back(Letter);
			++Index;
			continue;
		}

		const std::size_t Count = CountRepeat(Pattern, Index);
		Index += Count;
		switch (Letter)
		{
			case 'y':
				Result += Count == 2 ? Padded(static_cast<std::uint32_t>(std::abs(Time.Year) % 100), 2) : std::format("{}", Time.Year);
				break;
			case 'M':
			case 'L':
				Result += Count >= 4 ? (Letter == 'L' ? Locale->MonthsStandalone[Month] : Locale->MonthsWide[Month]) : Count == 3 ? std::string(Locale->MonthsAbbreviated[Month])
				                                                                                                                  : Padded(Time.Month, Count);
				break;
			case 'd':
				Result += Padded(Time.Day, Count);
				break;
			case 'E':
			case 'c':
				Result += Count >= 4 ? Locale->DaysWide[Weekday] : Locale->DaysAbbreviated[Weekday];
				break;
			case 'H':
				Result += Padded(Time.Hour, Count);
				break;
			case 'h':
				Result += Padded(Time.Hour % 12 == 0 ? 12 : Time.Hour % 12, Count);
				break;
			case 'm':
				Result += Padded(Time.Minute, Count);
				break;
			case 's':
				Result += Padded(Time.Second, Count);
				break;
			case 'a':
			case 'b':
			case 'B':
				Result += Locale->DayPeriods[Time.Hour < 12 ? 0 : 1];
				break;
			default:
				// Zones and fields Herta does not show leave nothing behind.
				break;
		}
	}

	// A dropped field can leave a separator space at the end.
	while (!Result.empty() && Result.back() == ' ')
	{
		Result.pop_back();
	}

	return Result;
}

std::string FCulture::FormatDate(const FCivilTime& Time, const EDateTimeStyle Style) const
{
	return FormatPattern(Locale->DateFormats[static_cast<std::size_t>(Style)], Time);
}

std::string FCulture::FormatTime(const FCivilTime& Time, const EDateTimeStyle Style) const
{
	const EDateTimeStyle Shown = Style == EDateTimeStyle::Full || Style == EDateTimeStyle::Long ? EDateTimeStyle::Medium : Style;
	return FormatPattern(Locale->TimeFormats[static_cast<std::size_t>(Shown)], Time);
}

std::string FCulture::FormatDateTime(const FCivilTime& Time, const EDateTimeStyle Style) const
{
	// Glue such as "{1}, {0}" or "{1} 'at' {0}": the date, then the time, between quoted or plain literal text.
	const std::string_view Pattern = Locale->DateTimeFormats[static_cast<std::size_t>(Style)];
	std::string Result;
	for (std::size_t Index = 0; Index < Pattern.size();)
	{
		if (Pattern.substr(Index, 3) == "{1}" || Pattern.substr(Index, 3) == "{0}")
		{
			Result += Pattern[Index + 1] == '1' ? FormatDate(Time, Style) : FormatTime(Time, Style);
			Index += 3;
		}
		else if (Pattern[Index] == '\'')
		{
			const std::size_t Close = Pattern.find('\'', Index + 1);
			Result += Close == Index + 1 ? std::string_view("'") : Pattern.substr(Index + 1, (Close == std::string_view::npos ? Pattern.size() : Close) - Index - 1);
			Index = Close == std::string_view::npos ? Pattern.size() : Close + 1;
		}
		else
		{
			Result.push_back(Pattern[Index++]);
		}
	}

	return Result;
}
}
