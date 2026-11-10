#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/TextLayout/TextLayout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>

using namespace Herta;

namespace
{
constexpr std::string_view Family = "\U0001F468‍\U0001F469‍\U0001F467";
constexpr std::string_view ThumbsUpDark = "\U0001F44D\U0001F3FD";
constexpr std::string_view PolishFlag = "\U0001F1F5\U0001F1F1";
constexpr std::string_view CombinedE = "é";

std::unique_ptr<FFontCollection> LoadFonts()
{
	std::vector<std::vector<std::byte>> Fonts;
	Fonts.push_back(ReadDiskFile("Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf").value_or(std::vector<std::byte>{}));
	Fonts.push_back(ReadDiskFile("Engine/Content/Editor/Fonts/NotoEmoji/NotoEmoji.ttf").value_or(std::vector<std::byte>{}));
	auto Collection = FFontCollection::Create(std::move(Fonts));
	REQUIRE(Collection);
	return std::move(*Collection);
}
}

TEST_CASE("UTF-8 decoding rejects malformed input and always advances")
{
	CHECK(DecodeUtf8("\xC5\x82", 0).Value == U'ł');
	CHECK(DecodeUtf8("\xF0\x9F\x91\x8D", 0).Value == U'\U0001F44D');
	CHECK(DecodeUtf8("\xC0\xAF", 0).Value == 0xFFFD);
	CHECK(DecodeUtf8("\xED\xA0\x80", 0).Value == 0xFFFD);
	CHECK(DecodeUtf8("\xE2\x82", 0).Length == 1);

	std::string Text;
	AppendUtf8(Text, U'Ж');
	AppendUtf8(Text, 0xD800);
	CHECK(Text == "Ж�");
}

TEST_CASE("Grapheme clusters keep emoji sequences, flags, and combining marks whole")
{
	CHECK(CountGraphemes(Family) == 1);
	CHECK(CountGraphemes(ThumbsUpDark) == 1);
	CHECK(CountGraphemes(std::string(PolishFlag) + std::string(PolishFlag)) == 2);
	CHECK(CountGraphemes(std::string("\U0001F1F5\U0001F1F1\U0001F1F5")) == 2);
	CHECK(CountGraphemes(CombinedE) == 1);
	CHECK(CountGraphemes("\r\n") == 1);
	CHECK(CountGraphemes("각") == 1);
	CHECK(CountGraphemes("क्ष") == 1);
	CHECK(CountGraphemes("Zażółć") == 6);
	CHECK(CountGraphemes("Привет") == 6);

	const std::string Text = std::string("a") + std::string(Family) + "b";
	CHECK(GetNextGrapheme(Text, 1) == 1 + Family.size());
	CHECK(GetPreviousGrapheme(Text, Text.size() - 1) == 1);
}

TEST_CASE("Text editing moves and deletes whole graphemes")
{
	FTextEdit Edit;
	InsertText(Edit, "Hi ");
	InsertText(Edit, ThumbsUpDark);
	CHECK(Edit.Cursor == Edit.Text.size());

	DeleteBackward(Edit);
	CHECK(Edit.Text == "Hi ");

	InsertText(Edit, "e");
	InsertText(Edit, "́");
	CHECK(Edit.Text == "Hi é");
	MoveCursor(Edit, -1);
	CHECK(Edit.Cursor == 3);
	DeleteForward(Edit);
	CHECK(Edit.Text == "Hi ");

	InsertText(Edit, "a\tb\x7F");
	CHECK(Edit.Text == "Hi ab");

	FTextEdit Limited;
	InsertText(Limited, std::string("ab") + std::string(Family), 6);
	CHECK(Limited.Text == "ab");
}

TEST_CASE("Code point edits that cut a grapheme widen to the whole cluster")
{
	const std::string Before = std::string("a") + std::string(ThumbsUpDark) + "b";
	// Backspace in a code point editor removes only the skin tone.
	const std::string After = std::string("a👍") + "b";
	const auto Repaired = RepairGraphemeEdit(Before, After);
	REQUIRE(Repaired);
	CHECK(Repaired->Text == "ab");
	CHECK(Repaired->Cursor == 1);

	CHECK_FALSE(RepairGraphemeEdit("ab", "a"));
	CHECK_FALSE(RepairGraphemeEdit("e", CombinedE));
	CHECK(SnapToGrapheme(Before, 5, true) == 1 + ThumbsUpDark.size());
	CHECK(SnapToGrapheme(Before, 5, false) == 1);
	CHECK(SnapToGrapheme(Before, 1, false) == 1);
}

TEST_CASE("Shaping falls back to the emoji font for emoji")
{
	const auto Fonts = LoadFonts();
	const FShapedText Latin = Fonts->Shape("Zażółć", 32);
	CHECK(Latin.Width > 0.f);
	CHECK(std::ranges::all_of(Latin.Glyphs, [](const FShapedGlyph& Glyph)
	{
		return Glyph.Font == 0 && Glyph.Glyph != 0;
	}));

	const FShapedText Cyrillic = Fonts->Shape("Привет", 32);
	CHECK(Cyrillic.Glyphs.size() == 6);
	CHECK(Cyrillic.Glyphs.front().Font == 0);

	const std::string Mixed = std::string("ok ") + std::string(ThumbsUpDark);
	const FShapedText Emoji = Fonts->Shape(Mixed, 32);
	REQUIRE(!Emoji.Glyphs.empty());
	CHECK(Emoji.Glyphs.back().Font == 1);
	CHECK(Emoji.Glyphs.back().Cluster == 3);
	CHECK(Emoji.Glyphs.back().Glyph != 0);

	const FGlyphBitmap Bitmap = Fonts->Rasterize(Emoji.Glyphs.back().Font, Emoji.Glyphs.back().Glyph, 32);
	CHECK(Bitmap.Width > 0);
	CHECK(Bitmap.Coverage.size() == std::size_t{Bitmap.Width} * Bitmap.Height);
	CHECK(Fonts->GetMetrics(32).Ascender > 0.f);
	CHECK(Fonts->GetMetrics(32).Descender < 0.f);
}

TEST_CASE("Font collections reject missing and malformed fonts")
{
	CHECK_FALSE(FFontCollection::Create({}));
	std::vector<std::vector<std::byte>> Garbage(1, std::vector<std::byte>(64, std::byte{7}));
	CHECK_FALSE(FFontCollection::Create(std::move(Garbage)));
}
