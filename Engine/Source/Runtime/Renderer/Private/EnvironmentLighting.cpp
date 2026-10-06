#include "Herta/Renderer/EnvironmentLighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float Pi = std::numbers::pi_v<float>;
constexpr std::uint32_t SampleCount = 64;
constexpr std::uint32_t SpecularWidth = 128;
constexpr std::uint32_t SpecularHeight = 64;
constexpr std::uint32_t DiffuseWidth = 32;
constexpr std::uint32_t DiffuseHeight = 16;

float Saturate(const float Value)
{
	return std::clamp(Value, 0.f, 1.f);
}

float SmoothStep(const float Low, const float High, const float Value)
{
	const float T = Saturate((Value - Low) / (High - Low));
	return T * T * (3.f - 2.f * T);
}

FVector3 Multiply(const FVector3& A, const FVector3& B)
{
	return {A.X * B.X, A.Y * B.Y, A.Z * B.Z};
}

FVector3 Lerp(const FVector3& A, const FVector3& B, const float Weight)
{
	return A * (1.f - Weight) + B * Weight;
}

FVector3 ProceduralSky(const FVector3& Direction, const FVisualUniforms& Snapshot)
{
	if (Snapshot.Atmosphere[3] < 0.5f)
	{
		return {0.035f, 0.035f, 0.035f};
	}

	FVector3 SunDirection{0.f, 1.f, 0.f};
	FVector3 SunRadiance{1.f, 1.f, 1.f};

	for (std::size_t Index = 0; Index < static_cast<std::size_t>(Snapshot.Controls[0]); ++Index)
	{
		const auto& Light = Snapshot.Lights[Index];
		if (Light.PositionType[3] == static_cast<float>(ELightType::Directional))
		{
			SunDirection = FVector3{-Light.DirectionRange[0], -Light.DirectionRange[1], -Light.DirectionRange[2]}.Normalized();
			SunRadiance = FVector3{Light.ColorIntensity[0], Light.ColorIntensity[1], Light.ColorIntensity[2]} * (Light.ColorIntensity[3] / 50000.f);
			break;
		}
	}

	const float Mu = std::clamp(Direction.Dot(SunDirection), -1.f, 1.f);
	const float G = Snapshot.Atmosphere[2];
	const float RayleighPhase = 3.f / (16.f * Pi) * (1.f + Mu * Mu);
	const float MiePhase = (1.f - G * G) / (4.f * Pi * std::pow(std::max(0.002f, 1.f + G * G - 2.f * G * Mu), 1.5f));
	const float Curvature = std::sqrt(Snapshot.AtmosphereGeometry[1] / std::max(1.f, Snapshot.AtmosphereGeometry[0]));
	const float Height = std::max(0.025f, Direction.Y + Curvature);
	const FVector3 OpticalDepth = FVector3{0.055f, 0.13f, 0.3f} * (Snapshot.Atmosphere[0] * Snapshot.AtmosphereGeometry[1] / 80000.f / Height);
	const float SunHeight = std::max(0.025f, SunDirection.Y + 0.08f);
	SunRadiance = Multiply(SunRadiance, FVector3{std::exp(-0.02f / SunHeight), std::exp(-0.05f / SunHeight), std::exp(-0.12f / SunHeight)}) * SmoothStep(-0.12f, 0.03f, SunDirection.Y);
	const FVector3 Transmittance{std::exp(-OpticalDepth.X), std::exp(-OpticalDepth.Y), std::exp(-OpticalDepth.Z)};
	const FVector3 Scattering = (FVector3::One() - Transmittance) * (RayleighPhase * 12.f + MiePhase * Snapshot.Atmosphere[1] * 0.4f);
	const FVector3 Ground = FVector3{0.025f, 0.03f, 0.04f} * Saturate(SunDirection.Y + 0.2f);
	const float SunDisc = SmoothStep(std::cos(0.005f), std::cos(0.0044f), Mu);
	const FVector3 Result = Lerp(Ground, Multiply(Scattering, SunRadiance), SmoothStep(-0.03f, 0.03f, Direction.Y)) + Multiply(Transmittance, SunRadiance) * (SunDisc * 20.f);
	return FVector3{std::max(0.f, Result.X), std::max(0.f, Result.Y), std::max(0.f, Result.Z)} * 15000.f;
}

FVector3 ReadPixel(const FCookedTexture& Texture, const std::uint32_t X, const std::uint32_t Y)
{
	const auto& Mip = Texture.Mips.front();
	const std::size_t Pixel = std::size_t{Y} * Mip.Width + X;
	if (Texture.PixelFormat == ETexturePixelFormat::Rgba32Float)
	{
		std::array<float, 4> Values{};
		std::memcpy(Values.data(), Mip.Pixels.data() + Pixel * sizeof(Values), sizeof(Values));
		return {Values[0], Values[1], Values[2]};
	}

	const auto Channel = [&](const std::size_t Index)
	{
		const float Value = static_cast<float>(Mip.Pixels[Pixel * 4 + Index]) / 255.f;
		return Texture.ColorSpace == ETextureColorSpace::Linear ? Value : Value <= 0.04045f ? Value / 12.92f
		                                                                                    : std::pow((Value + 0.055f) / 1.055f, 2.4f);
	};

	return {Channel(0), Channel(1), Channel(2)};
}

FVector3 SampleEnvironment(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, const FVector3& Direction)
{
	if (!Environment)
	{
		return ProceduralSky(Direction, Snapshot);
	}

	const auto& Mip = Environment->Mips.front();
	const float U = std::atan2(Direction.Z, Direction.X) / (2.f * Pi) + 0.5f;
	const float V = std::acos(std::clamp(Direction.Y, -1.f, 1.f)) / Pi;
	const float X = U * static_cast<float>(Mip.Width) - 0.5f;
	const float Y = V * static_cast<float>(Mip.Height) - 0.5f;
	const auto X0 = static_cast<std::int32_t>(std::floor(X));
	const auto Y0 = static_cast<std::int32_t>(std::floor(Y));
	const auto WrapX = [&](const std::int32_t Value)
	{
		const auto Width = static_cast<std::int32_t>(Mip.Width);
		return static_cast<std::uint32_t>((Value % Width + Width) % Width);
	};

	const auto ClampY = [&](const std::int32_t Value)
	{
		return static_cast<std::uint32_t>(std::clamp(Value, 0, static_cast<std::int32_t>(Mip.Height) - 1));
	};

	const FVector3 Top = Lerp(ReadPixel(*Environment, WrapX(X0), ClampY(Y0)), ReadPixel(*Environment, WrapX(X0 + 1), ClampY(Y0)), X - std::floor(X));
	const FVector3 Bottom = Lerp(ReadPixel(*Environment, WrapX(X0), ClampY(Y0 + 1)), ReadPixel(*Environment, WrapX(X0 + 1), ClampY(Y0 + 1)), X - std::floor(X));
	return Lerp(Top, Bottom, Y - std::floor(Y));
}

FVector3 TexelDirection(const std::uint32_t X, const std::uint32_t Y, const std::uint32_t Width, const std::uint32_t Height)
{
	const float Longitude = ((static_cast<float>(X) + 0.5f) / static_cast<float>(Width) - 0.5f) * (2.f * Pi);
	const float Latitude = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Height) * Pi;
	const float SinLatitude = std::sin(Latitude);
	return {SinLatitude * std::cos(Longitude), std::cos(Latitude), SinLatitude * std::sin(Longitude)};
}

float RadicalInverse(std::uint32_t Bits)
{
	Bits = (Bits << 16) | (Bits >> 16);
	Bits = ((Bits & 0x55555555u) << 1) | ((Bits & 0xaaaaaaaau) >> 1);
	Bits = ((Bits & 0x33333333u) << 2) | ((Bits & 0xccccccccu) >> 2);
	Bits = ((Bits & 0x0f0f0f0fu) << 4) | ((Bits & 0xf0f0f0f0u) >> 4);
	Bits = ((Bits & 0x00ff00ffu) << 8) | ((Bits & 0xff00ff00u) >> 8);
	return static_cast<float>(Bits) * 2.3283064365386963e-10f;
}

FVector3 LocalToWorld(const FVector3& Local, const FVector3& Normal)
{
	const FVector3 Up = std::abs(Normal.Y) < 0.999f ? FVector3{0.f, 1.f, 0.f} : FVector3{1.f, 0.f, 0.f};
	const FVector3 Tangent = Up.Cross(Normal).Normalized();
	return Tangent * Local.X + Normal.Cross(Tangent) * Local.Y + Normal * Local.Z;
}

FVector3 IntegrateDiffuse(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, const FVector3& Normal)
{
	FVector3 Sum;

	for (std::uint32_t Index = 0; Index < SampleCount; ++Index)
	{
		const float U = (static_cast<float>(Index) + 0.5f) / static_cast<float>(SampleCount);
		const float Phi = 2.f * Pi * RadicalInverse(Index);
		const float Radius = std::sqrt(U);
		const FVector3 Local{Radius * std::cos(Phi), Radius * std::sin(Phi), std::sqrt(1.f - U)};
		Sum += SampleEnvironment(Environment, Snapshot, LocalToWorld(Local, Normal));
	}

	return Sum * (Pi / static_cast<float>(SampleCount));
}

FVector3 IntegrateSpecular(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, const FVector3& Normal, const float Roughness)
{
	if (Roughness == 0.f)
	{
		return SampleEnvironment(Environment, Snapshot, Normal);
	}

	const float Alpha = Roughness * Roughness;
	const float AlphaSquared = Alpha * Alpha;
	FVector3 Sum;
	float Weight = 0.f;

	for (std::uint32_t Index = 0; Index < SampleCount; ++Index)
	{
		const float U = (static_cast<float>(Index) + 0.5f) / static_cast<float>(SampleCount);
		const float Phi = 2.f * Pi * RadicalInverse(Index);
		const float CosTheta = std::sqrt((1.f - U) / (1.f + (AlphaSquared - 1.f) * U));
		const float SinTheta = std::sqrt(std::max(0.f, 1.f - CosTheta * CosTheta));
		const FVector3 Half = LocalToWorld({SinTheta * std::cos(Phi), SinTheta * std::sin(Phi), CosTheta}, Normal);
		const FVector3 Light = Half * (2.f * Normal.Dot(Half)) - Normal;
		const float NDotL = std::max(0.f, Normal.Dot(Light));
		if (NDotL > 0.f)
		{
			Sum += SampleEnvironment(Environment, Snapshot, Light) * NDotL;
			Weight += NDotL;
		}
	}

	return Weight > 0.f ? Sum / Weight : SampleEnvironment(Environment, Snapshot, Normal);
}

void WritePixel(FCookedTextureMip& Mip, const std::uint32_t X, const std::uint32_t Y, const FVector3& Color)
{
	const std::array<float, 4> Pixel{Color.X, Color.Y, Color.Z, 1.f};
	std::memcpy(Mip.Pixels.data() + (std::size_t{Y} * Mip.Width + X) * sizeof(Pixel), Pixel.data(), sizeof(Pixel));
}

FCookedTextureMip MakeMip(const std::uint32_t Width, const std::uint32_t Height)
{
	return {.Width = Width, .Height = Height, .Pixels = std::vector<std::byte>(std::size_t{Width} * Height * sizeof(float) * 4)};
}

void AppendDiffuseMips(FCookedTexture& Texture)
{
	while (Texture.Mips.back().Width > 1 || Texture.Mips.back().Height > 1)
	{
		const auto& Previous = Texture.Mips.back();
		auto Mip = MakeMip(std::max(1u, Previous.Width / 2), std::max(1u, Previous.Height / 2));

		for (std::uint32_t Y = 0; Y < Mip.Height; ++Y)
		{
			for (std::uint32_t X = 0; X < Mip.Width; ++X)
			{
				FVector3 Sum;

				for (std::uint32_t DY = 0; DY < 2; ++DY)
				{
					for (std::uint32_t DX = 0; DX < 2; ++DX)
					{
						std::array<float, 4> Pixel{};
						const auto Offset = (std::size_t{std::min(Previous.Height - 1, Y * 2 + DY)} * Previous.Width + std::min(Previous.Width - 1, X * 2 + DX)) * sizeof(Pixel);
						std::memcpy(Pixel.data(), Previous.Pixels.data() + Offset, sizeof(Pixel));
						Sum += FVector3{Pixel[0], Pixel[1], Pixel[2]};
					}
				}

				WritePixel(Mip, X, Y, Sum * 0.25f);
			}
		}

		Texture.Mips.push_back(std::move(Mip));
	}
}
}

std::expected<FEnvironmentLighting, FAssetError> BuildEnvironmentLighting(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, const std::stop_token StopToken)
{
	if (Environment)
	{
		if (auto Valid = ValidateCookedTexture(*Environment); !Valid)
		{
			return std::unexpected(Valid.error());
		}
	}
	else
	{
		if (!std::isfinite(Snapshot.Controls[0]) || Snapshot.Controls[0] < 0.f || Snapshot.Controls[0] > static_cast<float>(MaximumRenderLights)
		    || Snapshot.Controls[0] != std::floor(Snapshot.Controls[0])
		    || !std::isfinite(Snapshot.AtmosphereGeometry[0]) || Snapshot.AtmosphereGeometry[0] <= 0.f
		    || !std::isfinite(Snapshot.AtmosphereGeometry[1]) || Snapshot.AtmosphereGeometry[1] <= 0.f
		    || !std::ranges::all_of(Snapshot.Atmosphere, [](const float Value)
		{
			return std::isfinite(Value);
		}) || Snapshot.Atmosphere[0] < 0.f
		    || Snapshot.Atmosphere[0] > 100.f || Snapshot.Atmosphere[1] < 0.f || Snapshot.Atmosphere[1] > 100.f || std::abs(Snapshot.Atmosphere[2]) >= 1.f)
		{
			return std::unexpected(FAssetError{"Invalid procedural environment settings"});
		}

		for (std::size_t Index = 0; Index < static_cast<std::size_t>(Snapshot.Controls[0]); ++Index)
		{
			const auto& Light = Snapshot.Lights[Index];
			if (!std::ranges::all_of(Light.DirectionRange, [](const float Value)
			{
				return std::isfinite(Value);
			}) || !std::ranges::all_of(Light.ColorIntensity, [](const float Value)
			{
				return std::isfinite(Value) && Value >= 0.f;
			}))
			{
				return std::unexpected(FAssetError{"Invalid environment light settings"});
			}
		}
	}

	FEnvironmentLighting Result{
	    .Diffuse = {.ColorSpace = ETextureColorSpace::Linear, .PixelFormat = ETexturePixelFormat::Rgba32Float, .Mips = {}},
	    .Specular = {.ColorSpace = ETextureColorSpace::Linear, .PixelFormat = ETexturePixelFormat::Rgba32Float, .Mips = {}},
	    .bFromHdr = Environment != nullptr,
	};

	auto Diffuse = MakeMip(DiffuseWidth, DiffuseHeight);

	for (std::uint32_t Y = 0; Y < DiffuseHeight; ++Y)
	{
		if (StopToken.stop_requested())
		{
			return std::unexpected(FAssetError{"Environment filtering cancelled"});
		}

		for (std::uint32_t X = 0; X < DiffuseWidth; ++X)
		{
			WritePixel(Diffuse, X, Y, IntegrateDiffuse(Environment, Snapshot, TexelDirection(X, Y, DiffuseWidth, DiffuseHeight)));
		}
	}

	Result.Diffuse.Mips.push_back(std::move(Diffuse));
	AppendDiffuseMips(Result.Diffuse);
	constexpr std::uint32_t MipCount = 8;
	std::uint32_t Width = SpecularWidth;
	std::uint32_t Height = SpecularHeight;

	for (std::uint32_t Level = 0; Level < MipCount; ++Level)
	{
		auto Mip = MakeMip(Width, Height);
		const float Roughness = static_cast<float>(Level) / static_cast<float>(MipCount - 1);

		for (std::uint32_t Y = 0; Y < Height; ++Y)
		{
			if (StopToken.stop_requested())
			{
				return std::unexpected(FAssetError{"Environment filtering cancelled"});
			}

			for (std::uint32_t X = 0; X < Width; ++X)
			{
				WritePixel(Mip, X, Y, IntegrateSpecular(Environment, Snapshot, TexelDirection(X, Y, Width, Height), Roughness));
			}
		}

		Result.Specular.Mips.push_back(std::move(Mip));
		Width = std::max(1u, Width / 2);
		Height = std::max(1u, Height / 2);
	}

	if (auto Valid = ValidateCookedTexture(Result.Diffuse); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	if (auto Valid = ValidateCookedTexture(Result.Specular); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	return Result;
}
}
