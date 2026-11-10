#include "Herta/TextLayout/TextLayout.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumFontBytes = std::size_t{32} * 1024 * 1024;
constexpr std::uint32_t MaximumPixelSize = 512;

// Characters that only modify the cluster they sit in; a fallback font need not have them for the cluster to use it.
[[nodiscard]] bool IsIgnorableForCoverage(const char32_t CodePoint) noexcept
{
	return CodePoint == 0x200D || (CodePoint >= 0xFE00 && CodePoint <= 0xFE0F) || (CodePoint >= 0xE0020 && CodePoint <= 0xE007F);
}

struct FFont
{
	// FreeType and HarfBuzz read the face from this memory for as long as they live.
	std::vector<std::byte> Bytes;
	FT_Face Face = nullptr;
	hb_blob_t* Blob = nullptr;
	hb_face_t* ShapingFace = nullptr;
	// HarfBuzz fonts by pixel size; each is cheap and keeps the scale shaping uses.
	std::unordered_map<std::uint32_t, hb_font_t*> Sizes;
	std::uint32_t FreeTypeSize = 0;
};
}

struct FFontCollection::FImplementation
{
	~FImplementation();

	FImplementation() = default;
	FImplementation(const FImplementation&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	[[nodiscard]] hb_font_t* GetShapingFont(std::size_t Font, std::uint32_t PixelSize);
	[[nodiscard]] FT_Face SizeFace(std::size_t Font, std::uint32_t PixelSize);
	[[nodiscard]] std::uint32_t ChooseFont(std::string_view Text, std::size_t Start, std::size_t End) const;

	FT_Library Library = nullptr;
	std::vector<FFont> Fonts;
	hb_buffer_t* Buffer = nullptr;
};

FFontCollection::FImplementation::~FImplementation()
{
	for (FFont& Font : Fonts)
	{
		for (const auto& [Size, Shaping] : Font.Sizes)
		{
			hb_font_destroy(Shaping);
		}

		hb_face_destroy(Font.ShapingFace);
		hb_blob_destroy(Font.Blob);
		if (Font.Face)
		{
			FT_Done_Face(Font.Face);
		}
	}

	hb_buffer_destroy(Buffer);
	if (Library)
	{
		FT_Done_FreeType(Library);
	}
}

hb_font_t* FFontCollection::FImplementation::GetShapingFont(const std::size_t Font, const std::uint32_t PixelSize)
{
	auto [Found, bInserted] = Fonts[Font].Sizes.try_emplace(PixelSize, nullptr);
	if (bInserted)
	{
		Found->second = hb_font_create(Fonts[Font].ShapingFace);
		// 26.6 fixed point, matching FreeType's pixel sizes.
		const int Scale = static_cast<int>(PixelSize) * 64;
		hb_font_set_scale(Found->second, Scale, Scale);
	}

	return Found->second;
}

FT_Face FFontCollection::FImplementation::SizeFace(const std::size_t Font, const std::uint32_t PixelSize)
{
	FFont& Entry = Fonts[Font];
	if (Entry.FreeTypeSize != PixelSize)
	{
		// Fixed-size bitmap fonts reject sizes they lack; they then keep their nearest strike.
		if (FT_Set_Pixel_Sizes(Entry.Face, 0, PixelSize) != 0 && Entry.Face->num_fixed_sizes > 0)
		{
			FT_Select_Size(Entry.Face, 0);
		}

		Entry.FreeTypeSize = PixelSize;
	}

	return Entry.Face;
}

std::uint32_t FFontCollection::FImplementation::ChooseFont(const std::string_view Text, const std::size_t Start, const std::size_t End) const
{
	for (std::size_t Font = 0; Font < Fonts.size(); ++Font)
	{
		bool bCovered = true;
		for (std::size_t Offset = Start; Offset < End && bCovered;)
		{
			const FUtf8CodePoint CodePoint = DecodeUtf8(Text, Offset);
			Offset += CodePoint.Length;
			bCovered = IsIgnorableForCoverage(CodePoint.Value) || FT_Get_Char_Index(Fonts[Font].Face, CodePoint.Value) != 0;
		}

		if (bCovered)
		{
			return static_cast<std::uint32_t>(Font);
		}
	}

	// No font has it: the primary font's missing-glyph box shows that something is there.
	return 0;
}

FFontCollection::FFontCollection(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FFontCollection::~FFontCollection() = default;

std::expected<std::unique_ptr<FFontCollection>, FTextError> FFontCollection::Create(std::vector<std::vector<std::byte>> Fonts)
{
	if (Fonts.empty())
	{
		return std::unexpected(FTextError{"A font collection needs at least one font"});
	}

	auto Implementation = std::make_unique<FImplementation>();
	if (FT_Init_FreeType(&Implementation->Library) != 0)
	{
		return std::unexpected(FTextError{"FreeType cannot start"});
	}

	Implementation->Buffer = hb_buffer_create();
	Implementation->Fonts.resize(Fonts.size());
	for (std::size_t Index = 0; Index < Fonts.size(); ++Index)
	{
		FFont& Font = Implementation->Fonts[Index];
		Font.Bytes = std::move(Fonts[Index]);
		if (Font.Bytes.empty() || Font.Bytes.size() > MaximumFontBytes)
		{
			return std::unexpected(FTextError{std::format("Font {} is missing or larger than 32 MiB", Index)});
		}

		const auto* const Data = reinterpret_cast<const FT_Byte*>(Font.Bytes.data());
		if (FT_New_Memory_Face(Implementation->Library, Data, static_cast<FT_Long>(Font.Bytes.size()), 0, &Font.Face) != 0)
		{
			Font.Face = nullptr;
			return std::unexpected(FTextError{std::format("Font {} is not a font FreeType can load", Index)});
		}

		Font.Blob = hb_blob_create(reinterpret_cast<const char*>(Font.Bytes.data()), static_cast<unsigned>(Font.Bytes.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
		Font.ShapingFace = hb_face_create(Font.Blob, 0);
	}

	return std::unique_ptr<FFontCollection>(new FFontCollection(std::move(Implementation)));
}

FShapedText FFontCollection::Shape(const std::string_view Text, std::uint32_t PixelSize)
{
	FImplementation& Collection = *Implementation;
	PixelSize = std::clamp<std::uint32_t>(PixelSize, 1, MaximumPixelSize);
	FShapedText Shaped;
	float Pen = 0.f;
	for (std::size_t RunStart = 0; RunStart < Text.size();)
	{
		// A run is the longest sequence of clusters that choose the same font.
		const std::uint32_t Font = Collection.ChooseFont(Text, RunStart, GetNextGrapheme(Text, RunStart));
		std::size_t RunEnd = GetNextGrapheme(Text, RunStart);
		while (RunEnd < Text.size())
		{
			const std::size_t Next = GetNextGrapheme(Text, RunEnd);
			if (Collection.ChooseFont(Text, RunEnd, Next) != Font)
			{
				break;
			}

			RunEnd = Next;
		}

		hb_buffer_t* const Buffer = Collection.Buffer;
		hb_buffer_clear_contents(Buffer);
		// The whole text is context, so shaping at a run's edges sees its neighbors.
		hb_buffer_add_utf8(Buffer, Text.data(), static_cast<int>(Text.size()), static_cast<unsigned>(RunStart), static_cast<int>(RunEnd - RunStart));
		hb_buffer_guess_segment_properties(Buffer);
		// Herta lays out left to right until BiDi lands; right-to-left scripts keep their logical order.
		hb_buffer_set_direction(Buffer, HB_DIRECTION_LTR);
		hb_shape(Collection.GetShapingFont(Font, PixelSize), Buffer, nullptr, 0);

		unsigned Count = 0;
		const hb_glyph_info_t* const Infos = hb_buffer_get_glyph_infos(Buffer, &Count);
		const hb_glyph_position_t* const Positions = hb_buffer_get_glyph_positions(Buffer, &Count);
		for (unsigned Index = 0; Index < Count; ++Index)
		{
			Shaped.Glyphs.push_back({
			    .Font = Font,
			    .Glyph = Infos[Index].codepoint,
			    .X = Pen + static_cast<float>(Positions[Index].x_offset) / 64.f,
			    .Y = -static_cast<float>(Positions[Index].y_offset) / 64.f,
			    .Cluster = Infos[Index].cluster,
			});

			Pen += static_cast<float>(Positions[Index].x_advance) / 64.f;
		}

		RunStart = RunEnd;
	}

	Shaped.Width = Pen;
	return Shaped;
}

FFontMetrics FFontCollection::GetMetrics(const std::uint32_t PixelSize)
{
	const FT_Face Face = Implementation->SizeFace(0, std::clamp<std::uint32_t>(PixelSize, 1, MaximumPixelSize));
	return {.Ascender = static_cast<float>(Face->size->metrics.ascender) / 64.f, .Descender = static_cast<float>(Face->size->metrics.descender) / 64.f};
}

FGlyphBitmap FFontCollection::Rasterize(const std::uint32_t Font, const std::uint32_t Glyph, const std::uint32_t PixelSize)
{
	FGlyphBitmap Bitmap;
	if (Font >= Implementation->Fonts.size())
	{
		return Bitmap;
	}

	// Light hinting snaps only vertically, so shaped advances, which are unhinted, still line up with the bitmaps.
	const FT_Face Face = Implementation->SizeFace(Font, std::clamp<std::uint32_t>(PixelSize, 1, MaximumPixelSize));
	if (FT_Load_Glyph(Face, Glyph, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0 || Face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
	{
		return Bitmap;
	}

	const FT_GlyphSlot Slot = Face->glyph;
	Bitmap.Width = Slot->bitmap.width;
	Bitmap.Height = Slot->bitmap.rows;
	Bitmap.Left = static_cast<float>(Slot->bitmap_left);
	Bitmap.Top = -static_cast<float>(Slot->bitmap_top);
	Bitmap.Coverage.resize(std::size_t{Bitmap.Width} * Bitmap.Height);
	for (std::uint32_t Row = 0; Row < Bitmap.Height; ++Row)
	{
		std::copy_n(Slot->bitmap.buffer + static_cast<std::ptrdiff_t>(Row) * Slot->bitmap.pitch, Bitmap.Width, Bitmap.Coverage.begin() + static_cast<std::ptrdiff_t>(Row) * Bitmap.Width);
	}

	return Bitmap;
}
}
