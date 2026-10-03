#include "Herta/AssetPipeline/TextureCooker.h"

#include "StbImage.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <vector>

namespace Herta
{
namespace
{
// round(65535 * sRGB-to-linear(i / 255)). A fixed table and integer filtering keep cooked bytes identical across compilers and C runtimes.
inline constexpr std::array<std::uint16_t, 256> SrgbToLinear{
    0,
    20,
    40,
    60,
    80,
    99,
    119,
    139,
    159,
    179,
    199,
    219,
    241,
    264,
    288,
    313,
    340,
    367,
    396,
    427,
    458,
    491,
    526,
    562,
    599,
    637,
    677,
    718,
    761,
    805,
    851,
    898,
    947,
    997,
    1048,
    1101,
    1156,
    1212,
    1270,
    1330,
    1391,
    1453,
    1517,
    1583,
    1651,
    1720,
    1790,
    1863,
    1937,
    2013,
    2090,
    2170,
    2250,
    2333,
    2418,
    2504,
    2592,
    2681,
    2773,
    2866,
    2961,
    3058,
    3157,
    3258,
    3360,
    3464,
    3570,
    3678,
    3788,
    3900,
    4014,
    4129,
    4247,
    4366,
    4488,
    4611,
    4736,
    4864,
    4993,
    5124,
    5257,
    5392,
    5530,
    5669,
    5810,
    5953,
    6099,
    6246,
    6395,
    6547,
    6700,
    6856,
    7014,
    7174,
    7335,
    7500,
    7666,
    7834,
    8004,
    8177,
    8352,
    8528,
    8708,
    8889,
    9072,
    9258,
    9445,
    9635,
    9828,
    10022,
    10219,
    10417,
    10619,
    10822,
    11028,
    11235,
    11446,
    11658,
    11873,
    12090,
    12309,
    12530,
    12754,
    12980,
    13209,
    13440,
    13673,
    13909,
    14146,
    14387,
    14629,
    14874,
    15122,
    15371,
    15623,
    15878,
    16135,
    16394,
    16656,
    16920,
    17187,
    17456,
    17727,
    18001,
    18277,
    18556,
    18837,
    19121,
    19407,
    19696,
    19987,
    20281,
    20577,
    20876,
    21177,
    21481,
    21787,
    22096,
    22407,
    22721,
    23038,
    23357,
    23678,
    24002,
    24329,
    24658,
    24990,
    25325,
    25662,
    26001,
    26344,
    26688,
    27036,
    27386,
    27739,
    28094,
    28452,
    28813,
    29176,
    29542,
    29911,
    30282,
    30656,
    31033,
    31412,
    31794,
    32179,
    32567,
    32957,
    33350,
    33745,
    34143,
    34544,
    34948,
    35355,
    35764,
    36176,
    36591,
    37008,
    37429,
    37852,
    38278,
    38706,
    39138,
    39572,
    40009,
    40449,
    40891,
    41337,
    41785,
    42236,
    42690,
    43147,
    43606,
    44069,
    44534,
    45002,
    45473,
    45947,
    46423,
    46903,
    47385,
    47871,
    48359,
    48850,
    49344,
    49841,
    50341,
    50844,
    51349,
    51858,
    52369,
    52884,
    53401,
    53921,
    54445,
    54971,
    55500,
    56032,
    56567,
    57105,
    57646,
    58190,
    58737,
    59287,
    59840,
    60396,
    60955,
    61517,
    62082,
    62650,
    63221,
    63795,
    64372,
    64952,
    65535,
};

// Linear values at or above Thresholds[i] encode to sRGB i + 1, so encoding picks the nearest table entry in linear space.
inline constexpr auto LinearThresholds = []
{
	std::array<std::uint32_t, 255> Result{};
	for (std::size_t Index = 0; Index < Result.size(); ++Index)
	{
		Result[Index] = (std::uint32_t{SrgbToLinear[Index]} + SrgbToLinear[Index + 1] + 1) / 2;
	}

	return Result;
}();

// Four 16-bit channels per texel: linear light for sRGB color, otherwise the 8-bit value scaled by 257.
struct FWideImage
{
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
	std::vector<std::uint16_t> Texels;
};

[[nodiscard]] bool IsLinearLightChannel(const ETextureColorSpace ColorSpace, const std::size_t Channel) noexcept
{
	return ColorSpace == ETextureColorSpace::Srgb && Channel < 3;
}

[[nodiscard]] FWideImage Widen(const std::uint32_t Width, const std::uint32_t Height, const std::span<const std::byte> Pixels, const ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor)
{
	FWideImage Image{.Width = Width, .Height = Height, .Texels = std::vector<std::uint16_t>(Pixels.size())};
	for (std::size_t Index = 0; Index < Pixels.size(); ++Index)
	{
		const std::size_t Channel = Index % 4;
		const auto Byte = static_cast<std::uint8_t>(Pixels[Index]);
		const std::uint32_t Value = IsLinearLightChannel(ColorSpace, Channel) ? SrgbToLinear[Byte] : std::uint32_t{Byte} * 257;
		Image.Texels[Index] = static_cast<std::uint16_t>(std::clamp(static_cast<float>(Value) * Factor[Channel] + 0.5f, 0.f, 65535.f));
	}

	return Image;
}

[[nodiscard]] FCookedTextureMip Narrow(const FWideImage& Image, const ETextureColorSpace ColorSpace)
{
	FCookedTextureMip Mip{.Width = Image.Width, .Height = Image.Height, .Pixels = std::vector<std::byte>(Image.Texels.size())};
	for (std::size_t Index = 0; Index < Image.Texels.size(); ++Index)
	{
		const std::uint32_t Value = Image.Texels[Index];
		const std::uint32_t Byte = IsLinearLightChannel(ColorSpace, Index % 4) ? static_cast<std::uint32_t>(std::ranges::upper_bound(LinearThresholds, Value) - LinearThresholds.begin()) : (Value * 255 + 32767) / 65535;
		Mip.Pixels[Index] = static_cast<std::byte>(Byte);
	}

	return Mip;
}

// ponytail: 2x2 box filter; odd edges reuse their last row or column. Use a wider kernel if NPOT aliasing becomes visible.
[[nodiscard]] FWideImage Downsample(const FWideImage& Source)
{
	FWideImage Target{.Width = std::max(1u, Source.Width / 2), .Height = std::max(1u, Source.Height / 2), .Texels = {}};
	Target.Texels.resize(std::size_t{Target.Width} * Target.Height * 4);
	for (std::uint32_t Y = 0; Y < Target.Height; ++Y)
	{
		const std::uint32_t Y0 = std::min(Y * 2, Source.Height - 1);
		const std::uint32_t Y1 = std::min(Y * 2 + 1, Source.Height - 1);
		for (std::uint32_t X = 0; X < Target.Width; ++X)
		{
			const std::uint32_t X0 = std::min(X * 2, Source.Width - 1);
			const std::uint32_t X1 = std::min(X * 2 + 1, Source.Width - 1);
			for (std::size_t Channel = 0; Channel < 4; ++Channel)
			{
				const auto Texel = [&](const std::uint32_t SourceX, const std::uint32_t SourceY)
				{
					return std::uint32_t{Source.Texels[(std::size_t{SourceY} * Source.Width + SourceX) * 4 + Channel]};
				};

				const std::uint32_t Sum = Texel(X0, Y0) + Texel(X1, Y0) + Texel(X0, Y1) + Texel(X1, Y1);
				Target.Texels[(std::size_t{Y} * Target.Width + X) * 4 + Channel] = static_cast<std::uint16_t>((Sum + 2) / 4);
			}
		}
	}

	return Target;
}
}

std::expected<FCookedTexture, FAssetError> CookTexture(const std::uint32_t Width, const std::uint32_t Height, const std::span<const std::byte> RgbaPixels, const ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor)
{
	if (Width == 0 || Height == 0 || Width > STBI_MAX_DIMENSIONS || Height > STBI_MAX_DIMENSIONS || RgbaPixels.size() != std::size_t{Width} * Height * 4)
	{
		return std::unexpected(FAssetError{std::format("Texture source must be RGBA8 with dimensions in [1, {}]", STBI_MAX_DIMENSIONS)});
	}

	if (ColorSpace != ETextureColorSpace::Linear && ColorSpace != ETextureColorSpace::Srgb)
	{
		return std::unexpected(FAssetError{"Unknown texture color space"});
	}

	if (!std::ranges::all_of(Factor, [](const float Value)
	{
		return std::isfinite(Value) && Value >= 0.f;
	}))
	{
		return std::unexpected(FAssetError{"Texture color factor must be finite and nonnegative"});
	}

	FCookedTexture Texture{.ColorSpace = ColorSpace, .Mips = {}};
	FWideImage Image = Widen(Width, Height, RgbaPixels, ColorSpace, Factor);
	while (true)
	{
		if (Image.Width <= MaximumCookedTextureDimension && Image.Height <= MaximumCookedTextureDimension)
		{
			Texture.Mips.push_back(Narrow(Image, ColorSpace));
		}

		if (Image.Width == 1 && Image.Height == 1)
		{
			break;
		}

		Image = Downsample(Image);
	}

	return Texture;
}

std::expected<FCookedTexture, FAssetError> CookEncodedTexture(const std::span<const std::byte> EncodedImage, const ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor)
{
	if (EncodedImage.empty() || EncodedImage.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return std::unexpected(FAssetError{"Encoded image is empty or too large"});
	}

	int Width = 0;
	int Height = 0;
	int Channels = 0;
	const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> Pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(EncodedImage.data()), static_cast<int>(EncodedImage.size()), &Width, &Height, &Channels, 4), &stbi_image_free);
	if (!Pixels)
	{
		return std::unexpected(FAssetError{std::format("Cannot decode image: {}", stbi_failure_reason())});
	}

	const auto PixelBytes = std::as_bytes(std::span(Pixels.get(), static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height) * 4));
	return CookTexture(static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height), PixelBytes, ColorSpace, Factor);
}
}
