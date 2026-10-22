#include "Herta/GameUI/GameUI.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
constexpr char FirstGlyph = ' ';
constexpr char LastGlyph = '~';
constexpr std::size_t GlyphCount = LastGlyph - FirstGlyph + 1;
constexpr std::array<float, 2> FontSizes{30.f, 76.f};
constexpr float LayoutHeight = 1440.f;
constexpr std::uint32_t AtlasWidth = 1024;
// An anti-aliased disc in the atlas corner draws dots and rounded corners; its solid middle fills rectangles, so everything batches with text.
constexpr std::uint32_t DiscSize = 32;
// Small dots would shrink the big disc several times over, which bilinear sampling aliases, so each whole-pixel diameter up to this gets its own exact disc.
constexpr std::uint32_t SmallDiscCount = 16;

struct FGlyph
{
	FVector2 UvMinimum{};
	FVector2 UvMaximum{};
	FVector2 Size{};
	// From the pen position on the top of the line to the bitmap's top-left corner.
	FVector2 Offset{};
	float Advance = 0.f;
};

struct FFontFace
{
	std::array<FGlyph, GlyphCount> Glyphs{};
	float LineHeight = 0.f;
};

[[nodiscard]] std::unexpected<FGameUIError> UIError(std::string Message)
{
	return std::unexpected(FGameUIError{std::move(Message)});
}

[[nodiscard]] const FGlyph& FindGlyph(const FFontFace& Face, const char Character)
{
	const char Shown = Character >= FirstGlyph && Character <= LastGlyph ? Character : '?';
	return Face.Glyphs[static_cast<std::size_t>(Shown - FirstGlyph)];
}

[[nodiscard]] bool Contains(const FVector2 Minimum, const FVector2 Maximum, const FVector2 Point)
{
	return Point.X >= Minimum.X && Point.Y >= Minimum.Y && Point.X < Maximum.X && Point.Y < Maximum.Y;
}
}

struct FGameUI::FImplementation
{
	~FImplementation();

	FImplementation() = default;
	FImplementation(const FImplementation&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	[[nodiscard]] std::expected<void, FGameUIError> BuildAtlas(float NewScale);
	void AddQuad(std::uint64_t TextureId, FVector2 Minimum, FVector2 Maximum, FVector2 UvMinimum, FVector2 UvMaximum, std::uint32_t Color);
	// Corners in drawing order. Every primitive is a quad, so screen draw lists convert quad by quad.
	void AddQuad(std::uint64_t TextureId, const std::array<FVector2, 4>& Corners, const std::array<FVector2, 4>& Uvs, std::uint32_t Color);
	[[nodiscard]] FVector2 Place(const FGameUIPlacement& Placement, FVector2 SizePixels) const;

	FGameUIDescriptor Descriptor;
	FT_Library Library = nullptr;
	FT_Face Face = nullptr;
	// FreeType reads the face from this memory for as long as the face lives.
	std::vector<FT_Byte> FontBytes;

	std::array<FFontFace, FontSizes.size()> Faces{};
	FTextureHandle Atlas;
	std::uint64_t AtlasId = 0;
	FVector2 SolidUv{};
	FVector2 DiscUv{};
	// UV minimum of the disc with diameter Index + 1 pixels.
	std::array<FVector2, SmallDiscCount> SmallDiscUvs{};
	FVector2 TexelUv{};
	float AtlasScale = 0.f;

	FExtent2D Extent;
	float Scale = 1.f;
	std::vector<FScreenVertex> Vertices;
	std::vector<std::uint16_t> Indices;
	std::vector<FScreenDrawCommand> Commands;
};

FGameUI::FImplementation::~FImplementation()
{
	if (AtlasId != 0 && Descriptor.UnregisterTexture)
	{
		Descriptor.UnregisterTexture(AtlasId);
	}

	if (Face)
	{
		FT_Done_Face(Face);
	}

	if (Library)
	{
		FT_Done_FreeType(Library);
	}
}

std::expected<void, FGameUIError> FGameUI::FImplementation::BuildAtlas(const float NewScale)
{
	struct FBitmap
	{
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::vector<std::uint8_t> Coverage;
	};

	std::array<std::array<FBitmap, GlyphCount>, FontSizes.size()> Bitmaps;
	std::array<std::array<std::pair<std::uint32_t, std::uint32_t>, GlyphCount>, FontSizes.size()> Positions{};
	// The small discs share the first row with the big one, each with a pixel of padding.
	std::uint32_t X = DiscSize + 1 + SmallDiscCount * (SmallDiscCount + 3) / 2;
	std::uint32_t Y = 0;
	std::uint32_t RowHeight = DiscSize;
	for (std::size_t Size = 0; Size < FontSizes.size(); ++Size)
	{
		const auto Pixels = static_cast<FT_UInt>(std::max(6.f, std::round(FontSizes[Size] * NewScale)));
		if (FT_Set_Pixel_Sizes(Face, 0, Pixels) != 0)
		{
			return UIError("The HUD font cannot be sized");
		}

		// The line box spans ascender to descender without the font's extra line gap, so text sits centered in a box of its measured height.
		const float Ascender = static_cast<float>(Face->size->metrics.ascender) / 64.f;
		Faces[Size].LineHeight = Ascender - static_cast<float>(Face->size->metrics.descender) / 64.f;
		for (std::size_t Index = 0; Index < GlyphCount; ++Index)
		{
			if (FT_Load_Char(Face, static_cast<FT_ULong>(FirstGlyph + static_cast<char>(Index)), FT_LOAD_RENDER) != 0)
			{
				return UIError(std::format("The HUD font has no glyph for '{}'", static_cast<char>(FirstGlyph + static_cast<char>(Index))));
			}

			const FT_GlyphSlot Slot = Face->glyph;
			FBitmap& Bitmap = Bitmaps[Size][Index];
			Bitmap.Width = Slot->bitmap.width;
			Bitmap.Height = Slot->bitmap.rows;
			Bitmap.Coverage.resize(std::size_t{Bitmap.Width} * Bitmap.Height);
			for (std::uint32_t Row = 0; Row < Bitmap.Height; ++Row)
			{
				std::copy_n(Slot->bitmap.buffer + static_cast<std::ptrdiff_t>(Row) * Slot->bitmap.pitch, Bitmap.Width, Bitmap.Coverage.begin() + static_cast<std::ptrdiff_t>(Row * Bitmap.Width));
			}

			FGlyph& Glyph = Faces[Size].Glyphs[Index];
			Glyph.Size = {static_cast<float>(Bitmap.Width), static_cast<float>(Bitmap.Height)};
			Glyph.Offset = {static_cast<float>(Slot->bitmap_left), Ascender - static_cast<float>(Slot->bitmap_top)};
			Glyph.Advance = static_cast<float>(Slot->advance.x) / 64.f;

			// Shelf packing with a pixel of padding keeps neighbors out of bilinear samples.
			if (X + Bitmap.Width + 1 > AtlasWidth)
			{
				X = 0;
				Y += RowHeight + 1;
				RowHeight = 0;
			}

			Positions[Size][Index] = {X, Y};
			X += Bitmap.Width + 1;
			RowHeight = std::max(RowHeight, Bitmap.Height);
		}
	}

	const std::uint32_t AtlasHeight = std::bit_ceil(Y + RowHeight + 1);
	std::vector<std::byte> Pixels(std::size_t{AtlasWidth} * AtlasHeight * 4);
	const auto Write = [&Pixels](const std::uint32_t PixelX, const std::uint32_t PixelY, const std::uint8_t Alpha)
	{
		std::byte* const Texel = Pixels.data() + (std::size_t{PixelY} * AtlasWidth + PixelX) * 4;
		Texel[0] = Texel[1] = Texel[2] = std::byte{255};
		Texel[3] = std::byte{Alpha};
	};

	// Coverage is each texel's area inside the circle, sampled on an 8x8 grid.
	const auto WriteDisc = [&Write](const std::uint32_t Left, const std::uint32_t Diameter)
	{
		constexpr int Samples = 8;
		const float Radius = static_cast<float>(Diameter) * 0.5f;
		for (std::uint32_t Row = 0; Row < Diameter; ++Row)
		{
			for (std::uint32_t Column = 0; Column < Diameter; ++Column)
			{
				int Inside = 0;
				for (int SampleY = 0; SampleY < Samples; ++SampleY)
				{
					for (int SampleX = 0; SampleX < Samples; ++SampleX)
					{
						const float PointX = static_cast<float>(Column) + (static_cast<float>(SampleX) + 0.5f) / Samples - Radius;
						const float PointY = static_cast<float>(Row) + (static_cast<float>(SampleY) + 0.5f) / Samples - Radius;
						Inside += PointX * PointX + PointY * PointY <= Radius * Radius ? 1 : 0;
					}
				}

				Write(Left + Column, Row, static_cast<std::uint8_t>((Inside * 255 + Samples * Samples / 2) / (Samples * Samples)));
			}
		}
	};

	const FVector2 Texel{1.f / static_cast<float>(AtlasWidth), 1.f / static_cast<float>(AtlasHeight)};
	WriteDisc(0, DiscSize);
	TexelUv = Texel;
	SolidUv = {DiscSize * 0.5f * Texel.X, DiscSize * 0.5f * Texel.Y};
	DiscUv = {DiscSize * Texel.X, DiscSize * Texel.Y};
	std::uint32_t DiscLeft = DiscSize + 1;
	for (std::uint32_t Diameter = 1; Diameter <= SmallDiscCount; ++Diameter)
	{
		WriteDisc(DiscLeft, Diameter);
		SmallDiscUvs[Diameter - 1] = {static_cast<float>(DiscLeft) * Texel.X, 0.f};
		DiscLeft += Diameter + 1;
	}

	for (std::size_t Size = 0; Size < FontSizes.size(); ++Size)
	{
		for (std::size_t Index = 0; Index < GlyphCount; ++Index)
		{
			const FBitmap& Bitmap = Bitmaps[Size][Index];
			const auto [Left, Top] = Positions[Size][Index];
			for (std::uint32_t Row = 0; Row < Bitmap.Height; ++Row)
			{
				for (std::uint32_t Column = 0; Column < Bitmap.Width; ++Column)
				{
					Write(Left + Column, Top + Row, Bitmap.Coverage[std::size_t{Row} * Bitmap.Width + Column]);
				}
			}

			FGlyph& Glyph = Faces[Size].Glyphs[Index];
			Glyph.UvMinimum = {static_cast<float>(Left) * Texel.X, static_cast<float>(Top) * Texel.Y};
			Glyph.UvMaximum = {static_cast<float>(Left + Bitmap.Width) * Texel.X, static_cast<float>(Top + Bitmap.Height) * Texel.Y};
		}
	}

	IGraphicsDevice& Device = *Descriptor.Device;
	const auto Texture = Device.CreateTexture({.Name = "Game UI glyphs", .Extent = {.Width = AtlasWidth, .Height = AtlasHeight}, .Format = ETextureFormat::Rgba8});
	if (!Texture)
	{
		return UIError(Texture.error().Message);
	}

	auto Uploaded = Device.BeginCommands();
	if (Uploaded)
	{
		Uploaded = Device.WriteTexture(*Texture, 0, Pixels);
	}

	if (!Uploaded || !Device.SubmitCommands())
	{
		Device.CancelCommands();
		return UIError(Uploaded ? "Cannot submit the HUD glyph upload" : Uploaded.error().Message);
	}

	const auto Registered = Descriptor.RegisterTexture(*Texture);
	if (!Registered)
	{
		return std::unexpected(Registered.error());
	}

	if (AtlasId != 0)
	{
		Descriptor.UnregisterTexture(AtlasId);
	}

	Atlas = *Texture;
	AtlasId = *Registered;
	AtlasScale = NewScale;
	return {};
}

void FGameUI::FImplementation::AddQuad(const std::uint64_t TextureId, const FVector2 Minimum, const FVector2 Maximum, const FVector2 UvMinimum, const FVector2 UvMaximum, const std::uint32_t Color)
{
	AddQuad(TextureId, {Minimum, FVector2{Maximum.X, Minimum.Y}, Maximum, FVector2{Minimum.X, Maximum.Y}}, {UvMinimum, FVector2{UvMaximum.X, UvMinimum.Y}, UvMaximum, FVector2{UvMinimum.X, UvMaximum.Y}}, Color);
}

void FGameUI::FImplementation::AddQuad(const std::uint64_t TextureId, const std::array<FVector2, 4>& Corners, const std::array<FVector2, 4>& Uvs, const std::uint32_t Color)
{
	// Indices are 16 bits; a HUD never comes close, so overflow drops quads instead of failing the frame.
	if (Vertices.size() + 4 > 0xffff)
	{
		return;
	}

	const auto First = static_cast<std::uint16_t>(Vertices.size());
	for (std::size_t Corner = 0; Corner < Corners.size(); ++Corner)
	{
		Vertices.push_back({.X = Corners[Corner].X, .Y = Corners[Corner].Y, .U = Uvs[Corner].X, .V = Uvs[Corner].Y, .Color = Color});
	}

	if (Commands.empty() || Commands.back().TextureId != TextureId)
	{
		Commands.push_back({.TextureId = TextureId, .FirstIndex = static_cast<std::uint32_t>(Indices.size())});
	}

	constexpr std::array<std::uint16_t, 6> Triangles{0, 1, 2, 0, 2, 3};
	for (const std::uint16_t Corner : Triangles)
	{
		Indices.push_back(static_cast<std::uint16_t>(First + Corner));
	}

	Commands.back().IndexCount += 6;
}

FVector2 FGameUI::FImplementation::Place(const FGameUIPlacement& Placement, const FVector2 SizePixels) const
{
	return {
	    Placement.Anchor.X * static_cast<float>(Extent.Width) + Placement.Offset.X * Scale - Placement.Pivot.X * SizePixels.X,
	    Placement.Anchor.Y * static_cast<float>(Extent.Height) + Placement.Offset.Y * Scale - Placement.Pivot.Y * SizePixels.Y,
	};
}

FGameUI::FGameUI(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameUI::~FGameUI() = default;

std::expected<std::unique_ptr<FGameUI>, FGameUIError> FGameUI::Create(FGameUIDescriptor Descriptor)
{
	if (!Descriptor.Device || !Descriptor.RegisterTexture || !Descriptor.UnregisterTexture)
	{
		return UIError("Game UI needs a graphics device and texture registration");
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Descriptor = std::move(Descriptor);
	std::error_code Error;
	const std::uintmax_t Size = std::filesystem::file_size(Implementation->Descriptor.FontPath, Error);
	// Font data is untrusted; anything past a few megabytes is not a HUD font.
	if (Error || Size == 0 || Size > 32 * 1024 * 1024)
	{
		return UIError(std::format("Cannot read the HUD font '{}'", Implementation->Descriptor.FontPath.generic_string()));
	}

	Implementation->FontBytes.resize(static_cast<std::size_t>(Size));
	std::ifstream File(Implementation->Descriptor.FontPath, std::ios::binary);
	if (!File.read(reinterpret_cast<char*>(Implementation->FontBytes.data()), static_cast<std::streamsize>(Size)))
	{
		return UIError("Cannot read the HUD font");
	}

	if (FT_Init_FreeType(&Implementation->Library) != 0 || FT_New_Memory_Face(Implementation->Library, Implementation->FontBytes.data(), static_cast<FT_Long>(Size), 0, &Implementation->Face) != 0)
	{
		return UIError("The HUD font is not a font FreeType can load");
	}

	return std::unique_ptr<FGameUI>(new FGameUI(std::move(Implementation)));
}

std::expected<void, FGameUIError> FGameUI::BeginFrame(const FExtent2D Extent)
{
	FImplementation& UI = *Implementation;
	UI.Extent = Extent;
	UI.Scale = std::max(static_cast<float>(Extent.Height), 1.f) / LayoutHeight;
	UI.Vertices.clear();
	UI.Indices.clear();
	UI.Commands.clear();

	// Glyphs rasterize at their final pixel size; a resize past a few percent rebuilds them so text stays sharp.
	if (UI.AtlasId == 0 || std::abs(UI.Scale - UI.AtlasScale) > UI.AtlasScale * 0.05f)
	{
		return UI.BuildAtlas(UI.Scale);
	}

	return {};
}

void FGameUI::DrawImage(const std::uint64_t TextureId, const FVector2 Minimum, const FVector2 Maximum, const std::uint32_t Color)
{
	Implementation->AddQuad(TextureId, Minimum, Maximum, {0.f, 0.f}, {1.f, 1.f}, Color);
}

void FGameUI::DrawRect(const FGameUIPlacement& Placement, const FVector2 Size, const std::uint32_t Color, const float Radius)
{
	FImplementation& UI = *Implementation;
	const FVector2 Pixels{Size.X * UI.Scale, Size.Y * UI.Scale};
	const FVector2 Minimum = UI.Place(Placement, Pixels);
	const FVector2 Maximum{Minimum.X + Pixels.X, Minimum.Y + Pixels.Y};
	const float Round = std::clamp(Radius * UI.Scale, 0.f, std::min(Pixels.X, Pixels.Y) * 0.5f);
	const FVector2 Uv = UI.SolidUv;
	if (Round < 0.5f)
	{
		UI.AddQuad(UI.AtlasId, Minimum, Maximum, Uv, Uv, Color);
		return;
	}

	// A cross of three solid bands, with each corner a quarter of the anti-aliased disc.
	UI.AddQuad(UI.AtlasId, {Minimum.X, Minimum.Y + Round}, {Maximum.X, Maximum.Y - Round}, Uv, Uv, Color);
	UI.AddQuad(UI.AtlasId, {Minimum.X + Round, Minimum.Y}, {Maximum.X - Round, Minimum.Y + Round}, Uv, Uv, Color);
	UI.AddQuad(UI.AtlasId, {Minimum.X + Round, Maximum.Y - Round}, {Maximum.X - Round, Maximum.Y}, Uv, Uv, Color);
	const FVector2 Disc = UI.DiscUv;
	UI.AddQuad(UI.AtlasId, Minimum, {Minimum.X + Round, Minimum.Y + Round}, {0.f, 0.f}, Uv, Color);
	UI.AddQuad(UI.AtlasId, {Maximum.X - Round, Minimum.Y}, {Maximum.X, Minimum.Y + Round}, {Uv.X, 0.f}, {Disc.X, Uv.Y}, Color);
	UI.AddQuad(UI.AtlasId, {Maximum.X - Round, Maximum.Y - Round}, Maximum, Uv, Disc, Color);
	UI.AddQuad(UI.AtlasId, {Minimum.X, Maximum.Y - Round}, {Minimum.X + Round, Maximum.Y}, {0.f, Uv.Y}, {Uv.X, Disc.Y}, Color);
}

void FGameUI::DrawCircle(const FGameUIPlacement& Placement, const float Diameter, const std::uint32_t Color)
{
	FImplementation& UI = *Implementation;
	const float Pixels = Diameter * UI.Scale;
	if (Pixels > static_cast<float>(SmallDiscCount))
	{
		const FVector2 Minimum = UI.Place(Placement, {Pixels, Pixels});
		UI.AddQuad(UI.AtlasId, Minimum, {Minimum.X + Pixels, Minimum.Y + Pixels}, {0.f, 0.f}, UI.DiscUv, Color);
		return;
	}

	// Snapped to whole pixels, a small disc samples its exact texels one to one.
	const float Snapped = std::max(std::round(Pixels), 1.f);
	const FVector2 Placed = UI.Place(Placement, {Snapped, Snapped});
	const FVector2 Minimum{std::round(Placed.X), std::round(Placed.Y)};
	const FVector2 Uv = UI.SmallDiscUvs[static_cast<std::size_t>(Snapped) - 1];
	UI.AddQuad(UI.AtlasId, Minimum, {Minimum.X + Snapped, Minimum.Y + Snapped}, Uv, {Uv.X + Snapped * UI.TexelUv.X, Snapped * UI.TexelUv.Y}, Color);
}

FVector2 FGameUI::MeasureText(const std::string_view Text, const EGameUIFont Font) const
{
	const FImplementation& UI = *Implementation;
	const FFontFace& Face = UI.Faces[static_cast<std::size_t>(Font)];
	float Width = 0.f;
	for (const char Character : Text)
	{
		Width += FindGlyph(Face, Character).Advance;
	}

	const float AtlasScale = std::max(UI.AtlasScale, 0.001f);
	return {Width / AtlasScale, Face.LineHeight / AtlasScale};
}

void FGameUI::DrawText(const std::string_view Text, const FGameUIPlacement& Placement, const EGameUIFont Font, const std::uint32_t Color)
{
	FImplementation& UI = *Implementation;
	const FFontFace& Face = UI.Faces[static_cast<std::size_t>(Font)];
	const FVector2 Size = MeasureText(Text, Font);
	FVector2 Pen = UI.Place(Placement, {Size.X * UI.AtlasScale, Size.Y * UI.AtlasScale});
	Pen = {std::round(Pen.X), std::round(Pen.Y)};
	for (const char Character : Text)
	{
		const FGlyph& Glyph = FindGlyph(Face, Character);
		if (Glyph.Size.X > 0.f && Glyph.Size.Y > 0.f)
		{
			const FVector2 Minimum{Pen.X + Glyph.Offset.X, Pen.Y + Glyph.Offset.Y};
			UI.AddQuad(UI.AtlasId, Minimum, {Minimum.X + Glyph.Size.X, Minimum.Y + Glyph.Size.Y}, Glyph.UvMinimum, Glyph.UvMaximum, Color);
		}

		Pen.X += Glyph.Advance;
	}
}

std::optional<std::size_t> FGameUI::DrawMenu(const std::span<const std::string_view> Items, std::size_t& Focus, const FGameUIInput& Input)
{
	if (Items.empty())
	{
		return std::nullopt;
	}

	FImplementation& UI = *Implementation;
	constexpr FVector2 ItemSize{520.f, 64.f};
	constexpr float Spacing = 12.f;
	const float Height = static_cast<float>(Items.size()) * (ItemSize.Y + Spacing) - Spacing;
	Focus = std::min(Focus, Items.size() - 1);
	if (Input.bUp)
	{
		Focus = (Focus + Items.size() - 1) % Items.size();
	}

	if (Input.bDown)
	{
		Focus = (Focus + 1) % Items.size();
	}

	std::optional<std::size_t> Chosen = Input.bActivate ? std::optional{Focus} : std::nullopt;
	for (std::size_t Index = 0; Index < Items.size(); ++Index)
	{
		const FGameUIPlacement Placement{.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.f}, .Offset = {0.f, -Height * 0.5f + static_cast<float>(Index) * (ItemSize.Y + Spacing)}};
		const FVector2 Pixels{ItemSize.X * UI.Scale, ItemSize.Y * UI.Scale};
		const FVector2 Minimum = UI.Place(Placement, Pixels);
		if (Input.Pointer && Contains(Minimum, {Minimum.X + Pixels.X, Minimum.Y + Pixels.Y}, *Input.Pointer))
		{
			Focus = Index;
			Chosen = Input.bClicked ? std::optional{Index} : Chosen;
		}
	}

	for (std::size_t Index = 0; Index < Items.size(); ++Index)
	{
		const bool bFocused = Index == Focus;
		const FGameUIPlacement Placement{.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.f}, .Offset = {0.f, -Height * 0.5f + static_cast<float>(Index) * (ItemSize.Y + Spacing)}};
		DrawRect(Placement, ItemSize, bFocused ? MakeGameUIColor(242, 196, 92, 235) : MakeGameUIColor(24, 28, 36, 200), 12.f);
		DrawText(Items[Index], {.Anchor = Placement.Anchor, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, Placement.Offset.Y + ItemSize.Y * 0.5f}}, EGameUIFont::Body, bFocused ? MakeGameUIColor(20, 20, 24) : MakeGameUIColor(235, 238, 245));
	}

	return Chosen;
}

FScreenDrawList FGameUI::GetDrawList() const noexcept
{
	return {.Vertices = Implementation->Vertices, .Indices = Implementation->Indices, .Commands = Implementation->Commands};
}
}
