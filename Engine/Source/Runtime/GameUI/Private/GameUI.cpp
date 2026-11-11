#include "Herta/GameUI/GameUI.h"

#include "Herta/TextLayout/TextLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::array<float, 3> FontSizes{30.f, 76.f, 22.f};
constexpr float LayoutHeight = 1440.f;
constexpr std::uint32_t PageSize = 1024;
// Glyphs past this many pages are not drawn; that is thousands of glyphs at title size.
constexpr std::size_t MaximumPages = 8;
// An anti-aliased disc in the first page's corner draws dots and rounded corners; its solid middle fills rectangles, so everything batches with text.
constexpr std::uint32_t DiscSize = 32;
// Small dots would shrink the big disc several times over, which bilinear sampling aliases, so each whole-pixel diameter up to this gets its own exact disc.
constexpr std::uint32_t SmallDiscCount = 16;
// Rasterized with the atlas, so menus and the HUD show from the first frame instead of one frame later.
constexpr std::string_view WarmText = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

struct FGlyph
{
	std::uint32_t Page = 0;
	FVector2 UvMinimum{};
	FVector2 UvMaximum{};
	FVector2 Size{};
	// From the pen position on the top of the line to the bitmap's top-left corner.
	FVector2 Offset{};
	// Rasterized into a page that has not been uploaded yet.
	bool bPending = true;
};

struct FFontFace
{
	std::uint32_t PixelSize = 0;
	float Ascender = 0.f;
	float LineHeight = 0.f;
};

// A fixed-size atlas texture with shelf packing. Glyphs keep their page and UVs for the atlas's life, and a page with new glyphs uploads
// as a fresh texture at the next frame boundary, so nothing rewrites a texture a frame in flight still samples.
struct FPage
{
	std::vector<std::byte> Pixels = std::vector<std::byte>(std::size_t{PageSize} * PageSize * 4);
	FTextureHandle Texture;
	std::uint64_t TextureId = 0;
	std::uint32_t X = 0;
	std::uint32_t Y = 0;
	std::uint32_t RowHeight = 0;
	bool bDirty = true;
};

[[nodiscard]] std::unexpected<FGameUIError> UIError(std::string Message)
{
	return std::unexpected(FGameUIError{std::move(Message)});
}

[[nodiscard]] constexpr std::uint64_t GlyphKey(const std::size_t Size, const FShapedGlyph& Glyph) noexcept
{
	return static_cast<std::uint64_t>(Glyph.Font) << 40 | static_cast<std::uint64_t>(Size) << 32 | Glyph.Glyph;
}

void WriteTexel(FPage& Page, const std::uint32_t PixelX, const std::uint32_t PixelY, const std::uint8_t Alpha)
{
	std::byte* const Texel = Page.Pixels.data() + (std::size_t{PixelY} * PageSize + PixelX) * 4;
	Texel[0] = Texel[1] = Texel[2] = std::byte{255};
	Texel[3] = std::byte{Alpha};
}

// Coverage is each texel's area inside the circle, sampled on an 8x8 grid.
void WriteDisc(FPage& Page, const std::uint32_t Left, const std::uint32_t Diameter)
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

			WriteTexel(Page, Left + Column, Row, static_cast<std::uint8_t>((Inside * 255 + Samples * Samples / 2) / (Samples * Samples)));
		}
	}
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
	[[nodiscard]] std::expected<void, FGameUIError> PublishPages();
	// The glyph's atlas entry, rasterizing it into a page on first use; null when the atlas is full.
	const FGlyph* FindGlyph(std::size_t Size, const FShapedGlyph& Shaped);
	void AddQuad(std::uint64_t TextureId, FVector2 Minimum, FVector2 Maximum, FVector2 UvMinimum, FVector2 UvMaximum, std::uint32_t Color);
	// Corners in drawing order. Every primitive is a quad, so screen draw lists convert quad by quad.
	void AddQuad(std::uint64_t TextureId, const std::array<FVector2, 4>& Corners, const std::array<FVector2, 4>& Uvs, std::uint32_t Color);
	[[nodiscard]] FVector2 Place(const FGameUIPlacement& Placement, FVector2 SizePixels) const;
	[[nodiscard]] std::uint64_t GetShapesTextureId() const noexcept;
	// Shaped text by font size and string. A HUD measures and draws the same strings every frame, so shaping repeats only when one changes.
	[[nodiscard]] const FShapedText& Shape(std::string_view Text, std::size_t Size) const;

	FGameUIDescriptor Descriptor;
	std::unique_ptr<FFontCollection> Fonts;

	std::array<FFontFace, FontSizes.size()> Faces{};
	std::vector<FPage> Pages;
	std::unordered_map<std::uint64_t, FGlyph> Glyphs;
	// Rasterized since the last publication.
	std::vector<std::uint64_t> PendingGlyphs;
	struct FShapeEntry
	{
		FShapedText Shaped;
		bool bUsed = true;
	};

	// Entries unused for a whole frame are dropped at the next frame's start.
	mutable std::unordered_map<std::string, FShapeEntry> ShapedTexts;
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
	for (const FPage& Page : Pages)
	{
		if (Page.TextureId != 0 && Descriptor.UnregisterTexture)
		{
			Descriptor.UnregisterTexture(Page.TextureId);
		}
	}
}

const FShapedText& FGameUI::FImplementation::Shape(const std::string_view Text, const std::size_t Size) const
{
	std::string Key(1, static_cast<char>('0' + Size));
	Key += Text;
	auto [Found, bInserted] = ShapedTexts.try_emplace(std::move(Key));
	if (bInserted)
	{
		Found->second.Shaped = Fonts->Shape(Text, Faces[Size].PixelSize);
	}

	Found->second.bUsed = true;
	return Found->second.Shaped;
}

std::uint64_t FGameUI::FImplementation::GetShapesTextureId() const noexcept
{
	return Pages.empty() ? 0 : Pages.front().TextureId;
}

std::expected<void, FGameUIError> FGameUI::FImplementation::BuildAtlas(const float NewScale)
{
	for (const FPage& Page : Pages)
	{
		if (Page.TextureId != 0)
		{
			Descriptor.UnregisterTexture(Page.TextureId);
		}
	}

	Pages.clear();
	Glyphs.clear();
	PendingGlyphs.clear();
	ShapedTexts.clear();
	FPage& Shapes = Pages.emplace_back();
	TexelUv = {1.f / static_cast<float>(PageSize), 1.f / static_cast<float>(PageSize)};
	WriteDisc(Shapes, 0, DiscSize);
	SolidUv = {DiscSize * 0.5f * TexelUv.X, DiscSize * 0.5f * TexelUv.Y};
	DiscUv = {DiscSize * TexelUv.X, DiscSize * TexelUv.Y};
	std::uint32_t DiscLeft = DiscSize + 1;
	for (std::uint32_t Diameter = 1; Diameter <= SmallDiscCount; ++Diameter)
	{
		WriteDisc(Shapes, DiscLeft, Diameter);
		SmallDiscUvs[Diameter - 1] = {static_cast<float>(DiscLeft) * TexelUv.X, 0.f};
		DiscLeft += Diameter + 1;
	}

	// The small discs share the first row with the big one, each with a pixel of padding.
	Shapes.X = DiscLeft;
	Shapes.RowHeight = DiscSize;
	for (std::size_t Size = 0; Size < FontSizes.size(); ++Size)
	{
		FFontFace& Face = Faces[Size];
		Face.PixelSize = static_cast<std::uint32_t>(std::max(6.f, std::round(FontSizes[Size] * NewScale)));
		// The line box spans ascender to descender without the font's extra line gap, so text sits centered in a box of its measured height.
		const FFontMetrics Metrics = Fonts->GetMetrics(Face.PixelSize);
		Face.Ascender = Metrics.Ascender;
		Face.LineHeight = Metrics.Ascender - Metrics.Descender;
		for (const FShapedGlyph& Glyph : Fonts->Shape(WarmText, Face.PixelSize).Glyphs)
		{
			FindGlyph(Size, Glyph);
		}
	}

	AtlasScale = NewScale;
	return PublishPages();
}

std::expected<void, FGameUIError> FGameUI::FImplementation::PublishPages()
{
	IGraphicsDevice& Device = *Descriptor.Device;
	for (FPage& Page : Pages)
	{
		if (!Page.bDirty)
		{
			continue;
		}

		const auto Texture = Device.CreateTexture({.Name = "Game UI glyphs", .Extent = {.Width = PageSize, .Height = PageSize}, .Format = ETextureFormat::Rgba8});
		if (!Texture)
		{
			return UIError(Texture.error().Message);
		}

		auto Uploaded = Device.BeginCommands();
		if (Uploaded)
		{
			Uploaded = Device.WriteTexture(*Texture, 0, Page.Pixels);
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

		if (Page.TextureId != 0)
		{
			Descriptor.UnregisterTexture(Page.TextureId);
		}

		Page.Texture = *Texture;
		Page.TextureId = *Registered;
		Page.bDirty = false;
	}

	for (const std::uint64_t Key : PendingGlyphs)
	{
		Glyphs.at(Key).bPending = false;
	}

	PendingGlyphs.clear();
	return {};
}

const FGlyph* FGameUI::FImplementation::FindGlyph(const std::size_t Size, const FShapedGlyph& Shaped)
{
	const std::uint64_t Key = GlyphKey(Size, Shaped);
	if (const auto Found = Glyphs.find(Key); Found != Glyphs.end())
	{
		return &Found->second;
	}

	const FFontFace& Face = Faces[Size];
	const FGlyphBitmap Bitmap = Fonts->Rasterize(Shaped.Font, Shaped.Glyph, Face.PixelSize);
	FGlyph Glyph{
	    .Size = {static_cast<float>(Bitmap.Width), static_cast<float>(Bitmap.Height)},
	    .Offset = {Bitmap.Left, Face.Ascender + Bitmap.Top},
	    .bPending = false,
	};

	if (Bitmap.Width == 0 || Bitmap.Height == 0)
	{
		return &Glyphs.emplace(Key, Glyph).first->second;
	}

	if (Bitmap.Width + 1 > PageSize || Bitmap.Height + 1 > PageSize)
	{
		return nullptr;
	}

	// Shelf packing with a pixel of padding keeps neighbors out of bilinear samples.
	FPage* Page = &Pages.back();
	if (Page->X + Bitmap.Width + 1 > PageSize)
	{
		Page->X = 0;
		Page->Y += Page->RowHeight + 1;
		Page->RowHeight = 0;
	}

	if (Page->Y + Bitmap.Height + 1 > PageSize)
	{
		if (Pages.size() == MaximumPages)
		{
			return nullptr;
		}

		Page = &Pages.emplace_back();
	}

	for (std::uint32_t Row = 0; Row < Bitmap.Height; ++Row)
	{
		for (std::uint32_t Column = 0; Column < Bitmap.Width; ++Column)
		{
			WriteTexel(*Page, Page->X + Column, Page->Y + Row, Bitmap.Coverage[std::size_t{Row} * Bitmap.Width + Column]);
		}
	}

	Glyph.Page = static_cast<std::uint32_t>(Pages.size() - 1);
	Glyph.UvMinimum = {static_cast<float>(Page->X) * TexelUv.X, static_cast<float>(Page->Y) * TexelUv.Y};
	Glyph.UvMaximum = {static_cast<float>(Page->X + Bitmap.Width) * TexelUv.X, static_cast<float>(Page->Y + Bitmap.Height) * TexelUv.Y};
	Glyph.bPending = true;
	Page->X += Bitmap.Width + 1;
	Page->RowHeight = std::max(Page->RowHeight, Bitmap.Height);
	Page->bDirty = true;
	PendingGlyphs.push_back(Key);
	return &Glyphs.emplace(Key, Glyph).first->second;
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

	std::vector<std::vector<std::byte>> FontFiles;
	FontFiles.push_back(std::move(Descriptor.FontBytes));
	std::ranges::move(Descriptor.FallbackFontBytes, std::back_inserter(FontFiles));
	Descriptor.FallbackFontBytes.clear();
	auto Fonts = FFontCollection::Create(std::move(FontFiles));
	if (!Fonts)
	{
		return UIError(std::format("The HUD fonts cannot load: {}", Fonts.error().Message));
	}

	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Descriptor = std::move(Descriptor);
	Implementation->Fonts = std::move(*Fonts);
	return std::unique_ptr<FGameUI>(new FGameUI(std::move(Implementation)));
}

std::expected<void, FGameUIError> FGameUI::BeginFrame(const FExtent2D Extent)
{
	FImplementation& UI = *Implementation;
	UI.Extent = Extent;
	UI.Scale = std::max(static_cast<float>(Extent.Height), 1.f) / LayoutHeight;
	UI.Vertices.clear();
	std::erase_if(UI.ShapedTexts, [](auto& Entry)
	{
		return !std::exchange(Entry.second.bUsed, false);
	});

	UI.Indices.clear();
	UI.Commands.clear();

	// Glyphs rasterize at their final pixel size; a resize past a few percent rebuilds them so text stays sharp.
	if (UI.GetShapesTextureId() == 0 || std::abs(UI.Scale - UI.AtlasScale) > UI.AtlasScale * 0.05f)
	{
		return UI.BuildAtlas(UI.Scale);
	}

	return UI.PublishPages();
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
		UI.AddQuad(UI.GetShapesTextureId(), Minimum, Maximum, Uv, Uv, Color);
		return;
	}

	// A cross of three solid bands, with each corner a quarter of the anti-aliased disc.
	UI.AddQuad(UI.GetShapesTextureId(), {Minimum.X, Minimum.Y + Round}, {Maximum.X, Maximum.Y - Round}, Uv, Uv, Color);
	UI.AddQuad(UI.GetShapesTextureId(), {Minimum.X + Round, Minimum.Y}, {Maximum.X - Round, Minimum.Y + Round}, Uv, Uv, Color);
	UI.AddQuad(UI.GetShapesTextureId(), {Minimum.X + Round, Maximum.Y - Round}, {Maximum.X - Round, Maximum.Y}, Uv, Uv, Color);
	const FVector2 Disc = UI.DiscUv;
	UI.AddQuad(UI.GetShapesTextureId(), Minimum, {Minimum.X + Round, Minimum.Y + Round}, {0.f, 0.f}, Uv, Color);
	UI.AddQuad(UI.GetShapesTextureId(), {Maximum.X - Round, Minimum.Y}, {Maximum.X, Minimum.Y + Round}, {Uv.X, 0.f}, {Disc.X, Uv.Y}, Color);
	UI.AddQuad(UI.GetShapesTextureId(), {Maximum.X - Round, Maximum.Y - Round}, Maximum, Uv, Disc, Color);
	UI.AddQuad(UI.GetShapesTextureId(), {Minimum.X, Maximum.Y - Round}, {Minimum.X + Round, Maximum.Y}, {0.f, Uv.Y}, {Uv.X, Disc.Y}, Color);
}

void FGameUI::DrawCircle(const FGameUIPlacement& Placement, const float Diameter, const std::uint32_t Color)
{
	FImplementation& UI = *Implementation;
	const float Pixels = Diameter * UI.Scale;
	if (Pixels > static_cast<float>(SmallDiscCount))
	{
		const FVector2 Minimum = UI.Place(Placement, {Pixels, Pixels});
		UI.AddQuad(UI.GetShapesTextureId(), Minimum, {Minimum.X + Pixels, Minimum.Y + Pixels}, {0.f, 0.f}, UI.DiscUv, Color);
		return;
	}

	// Snapped to whole pixels, a small disc samples its exact texels one to one.
	const float Snapped = std::max(std::round(Pixels), 1.f);
	const FVector2 Placed = UI.Place(Placement, {Snapped, Snapped});
	const FVector2 Minimum{std::round(Placed.X), std::round(Placed.Y)};
	const FVector2 Uv = UI.SmallDiscUvs[static_cast<std::size_t>(Snapped) - 1];
	UI.AddQuad(UI.GetShapesTextureId(), Minimum, {Minimum.X + Snapped, Minimum.Y + Snapped}, Uv, {Uv.X + Snapped * UI.TexelUv.X, Snapped * UI.TexelUv.Y}, Color);
}

FVector2 FGameUI::MeasureText(const std::string_view Text, const EGameUIFont Font) const
{
	const FImplementation& UI = *Implementation;
	const FFontFace& Face = UI.Faces[static_cast<std::size_t>(Font)];
	const float AtlasScale = std::max(UI.AtlasScale, 0.001f);
	return {UI.Shape(Text, static_cast<std::size_t>(Font)).Width / AtlasScale, Face.LineHeight / AtlasScale};
}

void FGameUI::DrawText(const std::string_view Text, const FGameUIPlacement& Placement, const EGameUIFont Font, const std::uint32_t Color)
{
	FImplementation& UI = *Implementation;
	const auto Size = static_cast<std::size_t>(Font);
	const FFontFace& Face = UI.Faces[Size];
	const FShapedText& Shaped = UI.Shape(Text, Size);
	FVector2 Pen = UI.Place(Placement, {Shaped.Width, Face.LineHeight});
	Pen = {std::round(Pen.X), std::round(Pen.Y)};
	for (const FShapedGlyph& Shape : Shaped.Glyphs)
	{
		// A glyph first drawn this frame publishes at the next frame boundary and shows from then on.
		const FGlyph* const Glyph = UI.FindGlyph(Size, Shape);
		if (Glyph && !Glyph->bPending && Glyph->Size.X > 0.f && Glyph->Size.Y > 0.f)
		{
			const FVector2 Minimum{std::round(Pen.X + Shape.X + Glyph->Offset.X), std::round(Pen.Y + Shape.Y + Glyph->Offset.Y)};
			UI.AddQuad(UI.Pages[Glyph->Page].TextureId, Minimum, {Minimum.X + Glyph->Size.X, Minimum.Y + Glyph->Size.Y}, Glyph->UvMinimum, Glyph->UvMaximum, Color);
		}
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
