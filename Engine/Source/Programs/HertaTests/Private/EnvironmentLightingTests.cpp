#include "Herta/AssetPipeline/TextureCooker.h"
#include "Herta/Renderer/EnvironmentLighting.h"
#include "VisualEnvironment.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <numbers>
#include <stop_token>

namespace Herta
{
namespace
{
std::vector<std::byte> CookedBytes(const FCookedAsset& Asset)
{
	const auto Bytes = SerializeCookedAsset(Asset);
	REQUIRE(Bytes);
	return *Bytes;
}

std::array<float, 4> EnvironmentPixel(const FCookedTextureMip& Mip, const std::size_t Index)
{
	std::array<float, 4> Pixel{};
	std::memcpy(Pixel.data(), Mip.Pixels.data() + Index * sizeof(Pixel), sizeof(Pixel));
	return Pixel;
}

float MaximumRadiance(const FCookedTextureMip& Mip)
{
	float Result = 0.f;

	for (std::size_t Index = 0; Index < std::size_t{Mip.Width} * Mip.Height; ++Index)
	{
		Result = std::max(Result, EnvironmentPixel(Mip, Index)[0]);
	}

	return Result;
}
}

TEST_CASE("Environment convolution preserves constant HDR radiance and diffuse energy")
{
	const auto Source = CookHdrTexture(1, 1, std::array<float, 4>{4.f, 2.f, 0.5f, 1.f});
	REQUIRE(Source);
	const auto Original = CookedBytes(*Source);
	const auto Result = BuildEnvironmentLighting(&*Source, FVisualUniforms{});
	REQUIRE(Result);
	CHECK(CookedBytes(*Source) == Original);
	REQUIRE(ValidateCookedTexture(Result->Diffuse));
	REQUIRE(ValidateCookedTexture(Result->Specular));
	CHECK(Result->Specular.Mips.front().Width == 128);
	CHECK(Result->Diffuse.Mips.front().Width == 32);
	CHECK(Result->Specular.Mips.size() == 8);

	for (const auto& Mip : Result->Specular.Mips)
	{
		const auto Pixel = EnvironmentPixel(Mip, 0);
		CHECK(Pixel[0] == doctest::Approx(4.f));
		CHECK(Pixel[1] == doctest::Approx(2.f));
		CHECK(Pixel[2] == doctest::Approx(0.5f));
		CHECK(Pixel[3] == 1.f);
	}

	const auto Diffuse = EnvironmentPixel(Result->Diffuse.Mips.front(), 0);
	CHECK(Diffuse[0] == doctest::Approx(4.f * std::numbers::pi_v<float>));
	CHECK(Diffuse[1] == doctest::Approx(2.f * std::numbers::pi_v<float>));
	CHECK(Diffuse[2] == doctest::Approx(0.5f * std::numbers::pi_v<float>));
}

TEST_CASE("Environment filtering decodes sRGB before radiance integration")
{
	constexpr std::array Pixels{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
	const auto Source = CookTexture(1, 1, Pixels, ETextureColorSpace::Srgb);
	REQUIRE(Source);
	const auto Result = BuildEnvironmentLighting(&*Source, FVisualUniforms{});
	REQUIRE(Result);
	CHECK(EnvironmentPixel(Result->Specular.Mips.front(), 0)[0] == doctest::Approx(0.2158605f));
	CHECK(EnvironmentPixel(Result->Diffuse.Mips.front(), 0)[0] == doctest::Approx(0.2158605f * std::numbers::pi_v<float>));
}

TEST_CASE("GGX environment roughness broadens bright highlights deterministically")
{
	constexpr std::uint32_t Width = 128;
	constexpr std::uint32_t Height = 64;
	std::vector<float> Pixels(std::size_t{Width} * Height * 4, 0.f);

	for (std::uint32_t Y = 0; Y < Height; ++Y)
	{
		for (std::uint32_t X = 0; X < Width; ++X)
		{
			const auto Offset = (std::size_t{Y} * Width + X) * 4;
			const float Value = X >= 56 && X < 72 && Y >= 24 && Y < 40 ? 100.f : 0.f;
			Pixels[Offset] = Value;
			Pixels[Offset + 1] = Value;
			Pixels[Offset + 2] = Value;
			Pixels[Offset + 3] = 1.f;
		}
	}

	const auto Source = CookHdrTexture(Width, Height, Pixels);
	REQUIRE(Source);
	const auto First = BuildEnvironmentLighting(&*Source, FVisualUniforms{});
	const auto Second = BuildEnvironmentLighting(&*Source, FVisualUniforms{});
	REQUIRE(First);
	REQUIRE(Second);
	CHECK(CookedBytes(First->Specular) == CookedBytes(Second->Specular));
	CHECK(CookedBytes(First->Diffuse) == CookedBytes(Second->Diffuse));
	CHECK(MaximumRadiance(First->Specular.Mips[0]) == doctest::Approx(100.f));
	CHECK(MaximumRadiance(First->Specular.Mips[5]) < MaximumRadiance(First->Specular.Mips[0]));
	CHECK(MaximumRadiance(First->Specular.Mips[5]) > 0.f);
}

TEST_CASE("Procedural environment follows sun intensity independently of sky visibility")
{
	FVisualUniforms Snapshot;
	Snapshot.Atmosphere = {1.f, 1.f, 0.8f, 1.f};
	Snapshot.Controls[0] = 1.f;
	Snapshot.Lights[0].PositionType[3] = static_cast<float>(ELightType::Directional);
	Snapshot.Lights[0].DirectionRange = {0.f, -1.f, 0.f, 0.f};
	Snapshot.Lights[0].ColorIntensity = {1.f, 1.f, 1.f, 50000.f};
	const auto Day = BuildEnvironmentLighting(nullptr, Snapshot);
	REQUIRE(Day);
	Snapshot.Sky[1] = 0.f;
	const auto Hidden = BuildEnvironmentLighting(nullptr, Snapshot);
	REQUIRE(Hidden);
	CHECK(CookedBytes(Hidden->Diffuse) == CookedBytes(Day->Diffuse));
	Snapshot.Lights[0].ColorIntensity[3] = 10000.f;
	const auto Dim = BuildEnvironmentLighting(nullptr, Snapshot);
	REQUIRE(Dim);
	CHECK(MaximumRadiance(Dim->Specular.Mips.front()) < MaximumRadiance(Day->Specular.Mips.front()));
}

TEST_CASE("Environment filtering validates inputs and supports cancellation")
{
	std::stop_source Cancellation;
	Cancellation.request_stop();
	CHECK_FALSE(BuildEnvironmentLighting(nullptr, FVisualUniforms{}, Cancellation.get_token()));
	FCookedTexture Empty;
	CHECK_FALSE(BuildEnvironmentLighting(&Empty, FVisualUniforms{}));
	FVisualUniforms Invalid;
	Invalid.Controls[0] = static_cast<float>(MaximumRenderLights + 1);
	CHECK_FALSE(BuildEnvironmentLighting(nullptr, Invalid));
	Invalid.Controls[0] = 0.f;
	Invalid.Atmosphere[2] = 1.f;
	CHECK_FALSE(BuildEnvironmentLighting(nullptr, Invalid));
	Invalid.Atmosphere[2] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(BuildEnvironmentLighting(nullptr, Invalid));
}

TEST_CASE("Environment generation keys track radiance without camera or visibility churn")
{
	FVisualUniforms Snapshot;
	Snapshot.Atmosphere = {1.f, 1.f, 0.8f, 1.f};
	Snapshot.Controls[0] = 2.f;
	Snapshot.Lights[0].PositionType[3] = static_cast<float>(ELightType::Point);
	Snapshot.Lights[1].PositionType[3] = static_cast<float>(ELightType::Directional);
	Snapshot.Lights[1].DirectionRange = {0.f, -1.f, 0.f, 0.f};
	Snapshot.Lights[1].ColorIntensity = {1.f, 1.f, 1.f, 50000.f};
	const auto Key = GetVisualEnvironmentKey(nullptr, Snapshot);
	Snapshot.ViewToWorld[12] = 500.f;
	Snapshot.ClipToView[0] = 20.f;
	Snapshot.Fog[0] = 0.5f;
	Snapshot.Material.BaseColor[0] = 0.1f;
	Snapshot.Sky = {42.f, 0.f, 7.f, 0.f};
	Snapshot.Lights[0].ColorIntensity[3] = 2000.f;
	CHECK(GetVisualEnvironmentKey(nullptr, Snapshot) == Key);
	Snapshot.Lights[1].ColorIntensity[3] = 10000.f;
	CHECK(GetVisualEnvironmentKey(nullptr, Snapshot) != Key);
	const auto Source = CookHdrTexture(1, 1, std::array<float, 4>{4.f, 2.f, 1.f, 1.f});
	REQUIRE(Source);
	const auto HdrKey = GetVisualEnvironmentKey(&*Source, Snapshot);
	Snapshot.Atmosphere[0] = 20.f;
	Snapshot.Lights[1].ColorIntensity[3] = 1.f;
	CHECK(GetVisualEnvironmentKey(&*Source, Snapshot) == HdrKey);
	const auto Changed = CookHdrTexture(1, 1, std::array<float, 4>{5.f, 2.f, 1.f, 1.f});
	REQUIRE(Changed);
	CHECK(GetVisualEnvironmentKey(&*Changed, Snapshot) != HdrKey);
}
}
