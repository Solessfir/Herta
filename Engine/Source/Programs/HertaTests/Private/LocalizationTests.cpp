#include "Herta/Localization/Localization.h"

#include <doctest/doctest.h>

#include <array>
#include <format>
#include <ostream>

using namespace Herta;

namespace
{
const FCulture& Culture(const std::string_view Tag)
{
	const FCulture* const Found = FCulture::Find(Tag);
	REQUIRE(Found);
	return *Found;
}

std::string Formatted(const std::string_view Pattern, const std::string_view Tag, const std::span<const FMessageArgument> Arguments)
{
	const auto Message = FMessage::Compile(Pattern);
	REQUIRE(Message);
	return Message->Format(Culture(Tag), Arguments);
}

constexpr std::string_view Nbsp = " ";
}

TEST_CASE("Cultures resolve tags by exact name, then language")
{
	CHECK(Culture("pl-PL").GetName() == "pl");
	CHECK(Culture("ru").GetName() == "ru");
	CHECK(Culture("en-US").GetName() == "en");
	CHECK(FCulture::Find("xx") == nullptr);
	CHECK(FCulture::GetAll().size() == 3);
}

TEST_CASE("Plural categories follow CLDR for English, Polish, and Russian")
{
	CHECK(Culture("en").SelectPlural(1.) == EPluralCategory::One);
	CHECK(Culture("en").SelectPlural(0.) == EPluralCategory::Other);
	CHECK(Culture("en").SelectPlural(1., {.MinimumFractionDigits = 1}) == EPluralCategory::Other);

	constexpr std::array<std::pair<int, EPluralCategory>, 9> Polish{{
	    {1, EPluralCategory::One},
	    {2, EPluralCategory::Few},
	    {4, EPluralCategory::Few},
	    {5, EPluralCategory::Many},
	    {12, EPluralCategory::Many},
	    {22, EPluralCategory::Few},
	    {21, EPluralCategory::Many},
	    {112, EPluralCategory::Many},
	    {0, EPluralCategory::Many},
	}};

	for (const auto& [Value, Category] : Polish)
	{
		CAPTURE(Value);
		CHECK(Culture("pl").SelectPlural(Value) == Category);
	}

	constexpr std::array<std::pair<int, EPluralCategory>, 8> Russian{{
	    {1, EPluralCategory::One},
	    {21, EPluralCategory::One},
	    {11, EPluralCategory::Many},
	    {3, EPluralCategory::Few},
	    {23, EPluralCategory::Few},
	    {13, EPluralCategory::Many},
	    {5, EPluralCategory::Many},
	    {100, EPluralCategory::Many},
	}};

	for (const auto& [Value, Category] : Russian)
	{
		CAPTURE(Value);
		CHECK(Culture("ru").SelectPlural(Value) == Category);
	}

	CHECK(Culture("pl").SelectPlural(1.5) == EPluralCategory::Other);
	CHECK(Culture("ru").SelectPlural(2.5) == EPluralCategory::Other);
}

TEST_CASE("Plural operands follow the shown digits")
{
	const FPluralOperands Operands = GetPluralOperands(1.5, 2);
	CHECK(Operands.I == 1);
	CHECK(Operands.V == 2);
	CHECK(Operands.W == 1);
	CHECK(Operands.F == 50);
	CHECK(Operands.T == 5);
}

TEST_CASE("Numbers use each culture's separators and minimum grouping")
{
	CHECK(Culture("en").FormatNumber(1234.5) == "1,234.5");
	CHECK(Culture("en").FormatNumber(-1234567) == "-1,234,567");
	CHECK(Culture("pl").FormatNumber(1234) == "1234");
	CHECK(Culture("pl").FormatNumber(12345.25) == std::format("12{}345,25", Nbsp));
	CHECK(Culture("ru").FormatNumber(1234) == std::format("1{}234", Nbsp));
	CHECK(Culture("en").FormatNumber(0.256, {.MaximumFractionDigits = 0, .bPercent = true}) == "26%");
	CHECK(Culture("ru").FormatNumber(0.5, {.MaximumFractionDigits = 0, .bPercent = true}) == std::format("50{}%", Nbsp));
	CHECK(Culture("en").FormatNumber(2, {.MinimumFractionDigits = 2}) == "2.00");
}

TEST_CASE("Dates use format month names, standalone forms, and quoted literals")
{
	const FCivilTime Time{.Year = 2026, .Month = 3, .Day = 7, .Hour = 14, .Minute = 5, .Second = 9};
	CHECK(Culture("en").FormatDate(Time, EDateTimeStyle::Long) == "March 7, 2026");
	CHECK(Culture("en").FormatDate(Time, EDateTimeStyle::Full) == "Saturday, March 7, 2026");
	CHECK(Culture("en").FormatTime(Time, EDateTimeStyle::Short) == "2:05 PM");
	CHECK(Culture("pl").FormatDate(Time, EDateTimeStyle::Long) == "7 marca 2026");
	CHECK(Culture("pl").FormatDate(Time, EDateTimeStyle::Short) == "7.03.2026");
	CHECK(Culture("ru").FormatDate(Time, EDateTimeStyle::Long) == "7 марта 2026 г.");
	CHECK(Culture("ru").FormatDateTime(Time, EDateTimeStyle::Short) == "07.03.2026, 14:05");
	CHECK(Culture("en").FormatTime(Time, EDateTimeStyle::Full) == "2:05:09 PM");
	CHECK(ToCivilTime(0) == FCivilTime{});
	CHECK(ToCivilTime(1'772'892'309) == Time);
}

TEST_CASE("Messages format arguments, plurals with offsets, and selects")
{
	constexpr std::string_view Pattern = "{count, plural, =0 {Brak wrogów} one {# wróg} few {# wrogowie} many {# wrogów} other {# wroga}}";
	CHECK(Formatted(Pattern, "pl", std::array{FMessageArgument{"count", std::int64_t{0}}}) == "Brak wrogów");
	CHECK(Formatted(Pattern, "pl", std::array{FMessageArgument{"count", std::int64_t{1}}}) == "1 wróg");
	CHECK(Formatted(Pattern, "pl", std::array{FMessageArgument{"count", std::int64_t{3}}}) == "3 wrogowie");
	CHECK(Formatted(Pattern, "pl", std::array{FMessageArgument{"count", std::int64_t{25}}}) == "25 wrogów");
	CHECK(Formatted(Pattern, "pl", std::array{FMessageArgument{"count", 1.5}}) == "1,5 wroga");

	constexpr std::string_view Offset = "{guests, plural, offset:1 =0 {nobody} =1 {{host}} one {{host} and # other} other {{host} and # others}}";
	CHECK(Formatted(Offset, "en", std::array{FMessageArgument{"guests", std::int64_t{2}}, FMessageArgument{"host", std::string("Ada")}}) == "Ada and 1 other");
	CHECK(Formatted(Offset, "en", std::array{FMessageArgument{"guests", std::int64_t{5}}, FMessageArgument{"host", std::string("Ada")}}) == "Ada and 4 others");

	constexpr std::string_view Select = "{side, select, ally {Friend} other {Foe}}";
	CHECK(Formatted(Select, "en", std::array{FMessageArgument{"side", std::string("ally")}}) == "Friend");
	CHECK(Formatted(Select, "en", std::array{FMessageArgument{"side", std::string("x")}}) == "Foe");

	CHECK(Formatted("Score {score, number} at {when, date, short}", "en", std::array{FMessageArgument{"score", std::int64_t{12345}}, FMessageArgument{"when", FCivilTime{.Year = 2026, .Month = 11, .Day = 10}}}) == "Score 12,345 at 11/10/26");
	CHECK(Formatted("Missing {name}", "en", {}) == "Missing {name}");
	CHECK(Formatted("It''s '{literal}' # here", "en", {}) == "It's {literal} # here");
}

TEST_CASE("Invalid messages report why")
{
	CHECK_FALSE(FMessage::Compile("{count, plural, one {x}}"));
	CHECK_FALSE(FMessage::Compile("{count, plural, lots {x} other {y}}"));
	CHECK_FALSE(FMessage::Compile("{open"));
	CHECK_FALSE(FMessage::Compile("close}"));
	CHECK_FALSE(FMessage::Compile("{x, number, currency}"));
	CHECK_FALSE(FMessage::Compile("{x, bogus}"));
	CHECK_FALSE(FMessage::Compile("{a, select, other {{b, select, other {{c, select, other {{d, select, other {{e, select, other {{f, select, other {{g, select, other {{h, select, other {{i, select, other {x}}}}}}}}}}}}}}}}}}"));

	const auto Names = FMessage::Compile("{b} {a, plural, other {{c}}} {b}");
	REQUIRE(Names);
	CHECK(Names->GetArgumentNames() == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("Text catalogs round-trip canonically")
{
	const FTextCatalog Catalog{
	    .Culture = "pl",
	    .Namespace = "Game",
	    .Messages = {{.Key = "Menu.Resume", .Text = "Wznów"}, {.Key = "Hud.Enemies", .Text = "\"{count}\"", .Comment = "Shown at the top"}},
	};

	const std::string Json = SerializeTextCatalog(Catalog);
	const auto Parsed = ParseTextCatalog(Json);
	REQUIRE(Parsed);
	CHECK(Parsed->Messages.front().Key == "Hud.Enemies");
	CHECK(Parsed->Messages.front().Comment == "Shown at the top");
	CHECK(SerializeTextCatalog(*Parsed) == Json);
	CHECK_FALSE(ParseTextCatalog(R"({"format": "HertaTextCatalog", "version": 2, "culture": "pl", "namespace": "Game", "messages": []})"));
	CHECK_FALSE(ParseTextCatalog(R"({"format": "HertaTextCatalog", "version": 1, "culture": "pl", "namespace": "Game", "messages": [{"key": "a b", "text": "x"}]})"));
}

TEST_CASE("Localization falls back, pseudolocalizes, and keeps the last valid generation")
{
	static constexpr FTextId Resume("Game.Menu.Resume");
	static constexpr FTextId Enemies("Game.Hud.Enemies");
	static constexpr FTextId Quit("Game.Menu.Quit");
	static_assert(Resume.Hash == HashTextKey("Game.Menu.Resume"));

	const std::array Catalogs{
	    FTextCatalog{
	        .Culture = "en",
	        .Namespace = "Game",
	        .Messages = {{.Key = "Menu.Resume", .Text = "Resume"}, {.Key = "Menu.Quit", .Text = "Quit"}, {.Key = "Hud.Enemies", .Text = "{count, plural, one {# enemy} other {# enemies}}"}},
	    },
	    FTextCatalog{.Culture = "pl", .Namespace = "Game", .Messages = {{.Key = "Menu.Resume", .Text = "Wznów"}, {.Key = "Hud.Enemies", .Text = "{count, plural, one {# wróg} few {# wrogów} many {# wrogów} other {# wroga}}"}}},
	    FTextCatalog{.Culture = "ru", .Namespace = "Game", .Messages = {{.Key = "Hud.Enemies", .Text = "{count, plural, one {# враг} few {# врага} many {# врагов} other {# врага}}"}}},
	};

	FLocalization Localization;
	REQUIRE(Localization.SetCatalogs(Catalogs));
	CHECK(Localization.GetAvailableCultures() == std::vector<std::string>{"en", "pl", "ru"});
	CHECK(Localization.Format(Resume) == "Resume");

	REQUIRE(Localization.SetCulture("pl-PL"));
	CHECK(Localization.Format(Resume) == "Wznów");
	CHECK(Localization.Format(Quit) == "Quit");

	REQUIRE(Localization.SetCulture("ru"));
	CHECK(Localization.Format(Enemies, std::array{FMessageArgument{"count", std::int64_t{22}}}) == "22 врага");
	CHECK(Localization.Format(Enemies, std::array{FMessageArgument{"count", std::int64_t{1234}}}) == std::format("1{}234 врага", Nbsp));
	CHECK(Localization.Format(FTextId::FromKey("Game.Unknown")) == "Game.Unknown");
	CHECK_FALSE(Localization.SetCulture("xx"));
	CHECK(Localization.GetCultureName() == "ru");

	REQUIRE(Localization.SetCulture(PseudoCulture));
	CHECK(Localization.Format(Quit) == "[Qúúíít]");
	CHECK(Localization.Format(Enemies, std::array{FMessageArgument{"count", std::int64_t{2}}}).starts_with("[2 "));

	const std::uint64_t Generation = Localization.GetGeneration();
	auto Broken = Catalogs;
	Broken[1].Messages[0].Text = "{oops";
	CHECK_FALSE(Localization.SetCatalogs(Broken));
	Broken[1].Messages[0].Text = "{name}";
	CHECK_FALSE(Localization.SetCatalogs(Broken));
	CHECK(Localization.GetGeneration() == Generation);
	REQUIRE(Localization.SetCulture("pl"));
	CHECK(Localization.Format(Resume) == "Wznów");
}
