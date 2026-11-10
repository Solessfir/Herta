#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FTextError
{
	std::string Message{};
};

struct FUtf8CodePoint
{
	char32_t Value = 0;
	// Bytes it takes; malformed input decodes as one byte of U+FFFD, so decoding always advances.
	std::uint32_t Length = 1;
};

[[nodiscard]] FUtf8CodePoint DecodeUtf8(std::string_view Text, std::size_t Offset) noexcept;
// Surrogates and values past U+10FFFF append U+FFFD.
void AppendUtf8(std::string& Text, char32_t CodePoint);

// Byte offsets of the extended grapheme clusters' starts after Offset and before it, by the Unicode rules in UAX #29. Editing moves and
// deletes whole clusters, so a flag, an accented letter written with a combining mark, or an emoji with a skin tone stays one character.
[[nodiscard]] std::size_t GetNextGrapheme(std::string_view Text, std::size_t Offset) noexcept;
[[nodiscard]] std::size_t GetPreviousGrapheme(std::string_view Text, std::size_t Offset) noexcept;
[[nodiscard]] std::size_t CountGraphemes(std::string_view Text) noexcept;

// A single-line text field's text and its cursor, a byte offset that always sits on a grapheme boundary.
struct FTextEdit
{
	std::string Text{};
	std::size_t Cursor = 0;
};

// Inserts at the cursor, dropping control characters, then stops at MaximumBytes without cutting a grapheme.
void InsertText(FTextEdit& Edit, std::string_view Inserted, std::size_t MaximumBytes = 256);
void DeleteBackward(FTextEdit& Edit);
void DeleteForward(FTextEdit& Edit);
void MoveCursor(FTextEdit& Edit, int Graphemes);

// Offset moved onto a grapheme boundary, forward or back, for editors that move by code point.
[[nodiscard]] std::size_t SnapToGrapheme(std::string_view Text, std::size_t Offset, bool bForward) noexcept;
// When an editor that works in code points changed Before into After by cutting into a grapheme cluster, such as deleting only an emoji's
// skin tone, the edit widened to whole clusters with the cursor after it. Nothing when the edit kept every cluster whole.
[[nodiscard]] std::optional<FTextEdit> RepairGraphemeEdit(std::string_view Before, std::string_view After);

struct FShapedGlyph
{
	// Index of the font in the collection that drew it.
	std::uint32_t Font = 0;
	std::uint32_t Glyph = 0;
	// Pen position of the glyph from the line's start, in pixels with Y down.
	float X = 0.f;
	float Y = 0.f;
	// Byte offset of the first character of the cluster it belongs to.
	std::uint32_t Cluster = 0;
};

struct FShapedText
{
	std::vector<FShapedGlyph> Glyphs{};
	float Width = 0.f;
};

struct FFontMetrics
{
	float Ascender = 0.f;
	// Negative below the baseline.
	float Descender = 0.f;
};

struct FGlyphBitmap
{
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
	// From the pen position on the baseline to the bitmap's top-left corner, Y down.
	float Left = 0.f;
	float Top = 0.f;
	std::vector<std::uint8_t> Coverage{};
};

// A primary font and its fallbacks: each grapheme cluster shapes with the first font that has all of its characters, so Latin text uses
// the primary font and emoji the emoji font. Shaping and rasterizing cache per-size state, so a collection is used from one thread.
class FFontCollection final
{
public:
	// Font files' bytes, primary first. Font data is untrusted; FreeType and HarfBuzz parse it.
	[[nodiscard]] static std::expected<std::unique_ptr<FFontCollection>, FTextError> Create(std::vector<std::vector<std::byte>> Fonts);

	~FFontCollection();

	FFontCollection(const FFontCollection&) = delete;
	FFontCollection& operator=(const FFontCollection&) = delete;
	FFontCollection(FFontCollection&&) = delete;
	FFontCollection& operator=(FFontCollection&&) = delete;

	[[nodiscard]] FShapedText Shape(std::string_view Text, std::uint32_t PixelSize);
	// The primary font's line metrics.
	[[nodiscard]] FFontMetrics GetMetrics(std::uint32_t PixelSize);
	// Coverage of one shaped glyph; empty for glyphs such as spaces.
	[[nodiscard]] FGlyphBitmap Rasterize(std::uint32_t Font, std::uint32_t Glyph, std::uint32_t PixelSize);

private:
	struct FImplementation;

	explicit FFontCollection(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation{};
};
}
