#include "Herta/Renderer/Visuals.h"

#include "Herta/Renderer/EnvironmentLighting.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace Herta
{
namespace
{
[[nodiscard]] std::expected<FMatrix4, FPresentationError> Inverse(const FMatrix4& Matrix)
{
	std::array<std::array<double, 8>, 4> Rows{};
	for (std::size_t R = 0; R < 4; ++R)
	{
		for (std::size_t C = 0; C < 4; ++C)
		{
			Rows[R][C] = Matrix(R, C);
			Rows[R][C + 4] = R == C ? 1. : 0.;
		}
	}

	for (std::size_t C = 0; C < 4; ++C)
	{
		std::size_t Pivot = C;
		for (std::size_t R = C + 1; R < 4; ++R)
		{
			if (std::abs(Rows[R][C]) > std::abs(Rows[Pivot][C]))
			{
				Pivot = R;
			}
		}

		if (!std::isfinite(Rows[Pivot][C]) || std::abs(Rows[Pivot][C]) < 1e-12)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Visual rendering requires invertible finite camera matrices"});
		}

		std::swap(Rows[C], Rows[Pivot]);
		const double Scale = Rows[C][C];
		for (double& Value : Rows[C])
		{
			Value /= Scale;
		}

		for (std::size_t R = 0; R < 4; ++R)
		{
			if (R == C)
			{
				continue;
			}

			const double Factor = Rows[R][C];
			for (std::size_t K = 0; K < 8; ++K)
			{
				Rows[R][K] -= Factor * Rows[C][K];
			}
		}
	}

	FMatrix4 Result = FMatrix4::Zero();
	for (std::size_t R = 0; R < 4; ++R)
	{
		for (std::size_t C = 0; C < 4; ++C)
		{
			Result(R, C) = static_cast<float>(Rows[R][C + 4]);
		}
	}

	return Result;
}

[[nodiscard]] FVector3 TemperatureColor(const float Kelvin)
{
	const float T = std::clamp(Kelvin, 1000.f, 40000.f) / 100.f;
	const float Red = T <= 66.f ? 255.f : 329.6987f * std::pow(T - 60.f, -0.1332f);
	const float Green = T <= 66.f ? 99.4708f * std::log(T) - 161.1196f : 288.1222f * std::pow(T - 60.f, -0.07551f);
	const float Blue = T >= 66.f ? 255.f : T <= 19.f ? 0.f
	                                                 : 138.5177f * std::log(T - 10.f) - 305.0448f;
	const auto Linear = [](const float Value)
	{
		const float S = std::clamp(Value / 255.f, 0.f, 1.f);
		return S <= 0.04045f ? S / 12.92f : std::pow((S + 0.055f) / 1.055f, 2.4f);
	};

	return {Linear(Red), Linear(Green), Linear(Blue)};
}
}

namespace
{
struct FShadowSizes
{
	std::uint32_t Cascade = ShadowCascadeResolution;
	std::uint32_t Tile = ShadowTileResolution;
};

FShadowSizes GetShadowSizes(const float Resolution)
{
	const float Scale = std::isfinite(Resolution) ? std::clamp(Resolution, 0.25f, 1.f) : 1.f;
	return {.Cascade = static_cast<std::uint32_t>(std::lround(ShadowCascadeResolution * Scale)), .Tile = static_cast<std::uint32_t>(std::lround(ShadowTileResolution * std::min(1.f, 2.f * Scale)))};
}

FVisualSettings MakeQualityPreset(const ERenderQuality Quality)
{
	FVisualSettings Preset;
	ApplyRenderQuality(Preset, Quality);
	return Preset;
}
}

void ApplyRenderQuality(FVisualSettings& Settings, const ERenderQuality Quality)
{
	switch (Quality)
	{
		case ERenderQuality::Low:
			Settings.AntiAliasing = EAntiAliasing::SmaaLow;
			Settings.ShadowQuality = EShadowQuality::Hard;
			Settings.ShadowResolution = 0.5f;
			Settings.AmbientOcclusion = EAmbientOcclusion::Off;
			Settings.FogResolution = 0.125f;
			break;
		case ERenderQuality::Medium:
			Settings.AntiAliasing = EAntiAliasing::SmaaMedium;
			Settings.ShadowQuality = EShadowQuality::Soft;
			Settings.ShadowResolution = 0.5f;
			Settings.AmbientOcclusion = EAmbientOcclusion::Low;
			Settings.FogResolution = 0.25f;
			break;
		case ERenderQuality::High:
			Settings.AntiAliasing = EAntiAliasing::SmaaHigh;
			Settings.ShadowQuality = EShadowQuality::Soft;
			Settings.ShadowResolution = 1.f;
			Settings.AmbientOcclusion = EAmbientOcclusion::High;
			Settings.FogResolution = 0.25f;
			break;
	}
}

std::optional<ERenderQuality> FindRenderQuality(const FVisualSettings& Settings)
{
	for (const ERenderQuality Quality : {ERenderQuality::Low, ERenderQuality::Medium, ERenderQuality::High})
	{
		const FVisualSettings Preset = MakeQualityPreset(Quality);
		if (Settings.AntiAliasing == Preset.AntiAliasing && Settings.ShadowQuality == Preset.ShadowQuality && Settings.ShadowResolution == Preset.ShadowResolution && Settings.AmbientOcclusion == Preset.AmbientOcclusion && Settings.FogResolution == Preset.FogResolution)
		{
			return Quality;
		}
	}

	return std::nullopt;
}

std::span<const std::array<float, 4>> GetSpecularAlbedoTable()
{
	// Integrated once per process with the shader's GGX and height-correlated Smith terms, so the table matches the lobe it compensates.
	static const std::vector<std::array<float, 4>> Table = []
	{
		constexpr std::uint32_t Size = SpecularAlbedoTableSize;
		constexpr std::uint32_t Samples = 1024;
		std::vector<std::array<float, 4>> Texels(std::size_t{Size} * Size);
		for (std::uint32_t Row = 0; Row < Size; ++Row)
		{
			const double Roughness = static_cast<double>(Row) / (Size - 1);
			const double Alpha = std::max(0.0025, Roughness * Roughness);
			const double A2 = Alpha * Alpha;
			for (std::uint32_t Column = 0; Column < Size; ++Column)
			{
				const double NoV = std::max(1e-4, static_cast<double>(Column) / (Size - 1));
				const double ViewX = std::sqrt(1.0 - NoV * NoV);
				std::array<double, 3> Sum{};
				for (std::uint32_t Index = 0; Index < Samples; ++Index)
				{
					// Hammersley points importance-sample GGX half vectors.
					std::uint32_t Bits = Index;
					Bits = (Bits << 16u) | (Bits >> 16u);
					Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xAAAAAAAAu) >> 1u);
					Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xCCCCCCCCu) >> 2u);
					Bits = ((Bits & 0x0F0F0F0Fu) << 4u) | ((Bits & 0xF0F0F0F0u) >> 4u);
					Bits = ((Bits & 0x00FF00FFu) << 8u) | ((Bits & 0xFF00FF00u) >> 8u);
					const double U = (Index + 0.5) / Samples;
					const double W = static_cast<double>(Bits) / 4294967296.0;
					const double Phi = 2.0 * std::numbers::pi * U;
					const double CosTheta = std::sqrt((1.0 - W) / (1.0 + (A2 - 1.0) * W));
					const double SinTheta = std::sqrt(1.0 - CosTheta * CosTheta);
					const double HalfX = SinTheta * std::cos(Phi);
					const double VoH = ViewX * HalfX + NoV * CosTheta;
					const double NoL = 2.0 * VoH * CosTheta - NoV;
					if (NoL <= 0.0 || VoH <= 0.0)
					{
						continue;
					}

					const double Visibility = 0.5 / (NoL * std::sqrt(NoV * NoV * (1.0 - A2) + A2) + NoV * std::sqrt(NoL * NoL * (1.0 - A2) + A2));
					const double Weight = Visibility * 4.0 * VoH * NoL / CosTheta;
					const double Complement = 1.0 - VoH;
					const double Fresnel = std::pow(Complement, 5.0);
					Sum[0] += (1.0 - Fresnel) * Weight;
					Sum[1] += Fresnel * Weight;
					Sum[2] += VoH * Fresnel * Complement * Weight;
				}

				Texels[std::size_t{Row} * Size + Column] = {static_cast<float>(Sum[0] / Samples), static_cast<float>(Sum[1] / Samples), static_cast<float>(Sum[2] / Samples), 1.f};
			}
		}

		return Texels;
	}();

	return Table;
}

FExtent2D GetCascadeAtlasExtent(const float Resolution)
{
	const std::uint32_t Cascade = GetShadowSizes(Resolution).Cascade;
	return {2 * Cascade, 2 * Cascade};
}

FExtent2D GetLocalShadowAtlasExtent(const float Resolution)
{
	const std::uint32_t Tile = GetShadowSizes(Resolution).Tile;
	return {8 * Tile, 2 * Tile};
}

FGtaoUniforms BuildGtaoUniforms(const FMatrix4& Projection, const FMatrix4& ClipToView, const FExtent2D Extent, const EAmbientOcclusion Quality)
{
	// Thin rails and posts stay open behind their assumed thickness, while 75 cm of radius still darkens room corners and the ground under props.
	constexpr float Radius = 0.75f;
	constexpr float Thickness = 0.3f;
	constexpr std::array<std::array<float, 2>, 4> SlicesAndSteps{{{0.f, 0.f}, {1.f, 3.f}, {2.f, 3.f}, {3.f, 4.f}}};
	const auto& Sampling = SlicesAndSteps[static_cast<std::size_t>(Quality)];
	const float Width = static_cast<float>(Extent.Width);
	const float Height = static_cast<float>(Extent.Height);
	const FExtent2D Half = GetMipExtent(Extent, 1);
	const float HalfWidth = static_cast<float>(Half.Width);
	const float HalfHeight = static_cast<float>(Half.Height);
	return {
	    .DepthToView = {ClipToView(2, 2), ClipToView(2, 3), ClipToView(3, 2), ClipToView(3, 3)},
	    .NdcToView = {1.f / Projection(0, 0), 1.f / Projection(1, 1), -Projection(0, 2) / Projection(0, 0), -Projection(1, 2) / Projection(1, 1)},
	    .Extent = {Width, Height, 1.f / Width, 1.f / Height},
	    .HalfExtent = {HalfWidth, HalfHeight, 1.f / HalfWidth, 1.f / HalfHeight},
	    .Settings = {Radius, Thickness, Sampling[0], Sampling[1]},
	};
}

std::expected<FVisualUniforms, FPresentationError> BuildVisualUniforms(const FMatrix4& View, const FMatrix4& Projection, const FExtent2D Extent, const std::span<const FRenderLight> Lights, const FVisualSettings& Settings)
{
	const auto ViewToWorld = Inverse(View);
	const auto ClipToView = Inverse(Projection);
	if (!ViewToWorld || !ClipToView)
	{
		return std::unexpected(!ViewToWorld ? ViewToWorld.error() : ClipToView.error());
	}

	if (Lights.size() > MaximumRenderLights || Extent.IsEmpty() || !std::isfinite(Settings.FogHeight) || !std::isfinite(Settings.ExposureEV100) || std::abs(Settings.ExposureEV100) > 24.f || static_cast<unsigned>(Settings.AntiAliasing) > static_cast<unsigned>(EAntiAliasing::SmaaUltra) || static_cast<unsigned>(Settings.ShadowQuality) > static_cast<unsigned>(EShadowQuality::Soft))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Visual settings exceed the light, exposure, extent, or antialiasing budget"});
	}

	FVisualUniforms Result;
	Result.ViewToWorld = ViewToWorld->Data();
	Result.ClipToView = ClipToView->Data();
	Result.Viewport = {1.f / static_cast<float>(Extent.Width), 1.f / static_cast<float>(Extent.Height), static_cast<float>(Extent.Width), static_cast<float>(Extent.Height)};
	Result.Controls[2] = 1.f / (1.2f * std::exp2(Settings.ExposureEV100));
	if (!std::isfinite(Settings.ExposureCompensation) || std::abs(Settings.ExposureCompensation) > 10.f || !std::isfinite(Settings.DeltaSeconds) || Settings.DeltaSeconds < 0.f)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Exposure compensation must be within 10 stops and the frame time finite and nonnegative"});
	}

	// A stalled frame should not jump the adaptation, so the step is capped like the physics catch-up.
	Result.Exposure = {Settings.bAutoExposure ? 1.f : 0.f, Settings.ExposureCompensation, std::min(Settings.DeltaSeconds, 0.25f), 0.f};
	if (const std::optional<FHdrDisplaySettings>& Hdr = Settings.HdrDisplay)
	{
		if (!std::isfinite(Hdr->PaperWhite) || Hdr->PaperWhite < 80.f || Hdr->PaperWhite > 500.f || !std::isfinite(Hdr->PeakLuminance) || Hdr->PeakLuminance < 250.f || Hdr->PeakLuminance > 10000.f || Hdr->PeakLuminance < Hdr->PaperWhite)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "HDR paper white must be 80 to 500 cd/m2 and the peak 250 to 10000 cd/m2, at least the paper white"});
		}

		Result.Display = {1.f, Hdr->PaperWhite, Hdr->PeakLuminance, Hdr->bCalibrationPattern ? 1.f : 0.f};
	}
	bool bHasSky = false;
	FVector3 SkyColor{};
	for (const FRenderLight& Source : Lights)
	{
		if (const auto Valid = ValidateLightComponent(Source.Settings); !Valid)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
		}

		if (!Source.Settings.bEnabled)
		{
			continue;
		}

		const FLightComponent& Light = Source.Settings;
		const auto Finite = [](const float Value)
		{
			return std::isfinite(Value);
		};

		const FVector3 X{Source.Transform(0, 0), Source.Transform(1, 0), Source.Transform(2, 0)};
		const FVector3 Y{Source.Transform(0, 1), Source.Transform(1, 1), Source.Transform(2, 1)};
		const FVector3 Z{Source.Transform(0, 2), Source.Transform(1, 2), Source.Transform(2, 2)};
		if (!std::ranges::all_of(Source.Transform.Data(), Finite) || X.Length() < 1e-6f || Y.Length() < 1e-6f || Z.Length() < 1e-6f || std::abs(X.Normalized().Dot(Y.Normalized().Cross(Z.Normalized()))) < 1e-5f)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Lights require a finite nondegenerate transform"});
		}

		const FVector3 Direction = FVector3{Source.Transform(0, 2), Source.Transform(1, 2), Source.Transform(2, 2)}.Normalized();
		const FVector3 Right = FVector3{-Source.Transform(0, 0), -Source.Transform(1, 0), -Source.Transform(2, 0)}.Normalized();
		const FVector3 Up = FVector3{Source.Transform(0, 1), Source.Transform(1, 1), Source.Transform(2, 1)}.Normalized();
		FVector3 Color = Light.Color;
		if (Light.bUseTemperature)
		{
			const FVector3 Temperature = TemperatureColor(Light.TemperatureKelvin);
			Color = {Color.X * Temperature.X, Color.Y * Temperature.Y, Color.Z * Temperature.Z};
		}

		const float Intensity = Light.Type == ELightType::Point ? Light.Intensity / (4.f * std::numbers::pi_v<float>) : Light.Type == ELightType::Spot ? Light.Intensity / (2.f * std::numbers::pi_v<float> * std::max(0.001f, 1.f - std::cos(Light.OuterConeAngle)))
		                                                                                                                                               : Light.Intensity;
		FVisualLightUniform& Uniform = Result.Lights[static_cast<std::size_t>(Result.Controls[0]++)];
		Uniform.PositionType = {Source.Transform(0, 3), Source.Transform(1, 3), Source.Transform(2, 3), static_cast<float>(Light.Type)};
		Uniform.DirectionRange = {Direction.X, Direction.Y, Direction.Z, Light.Range};
		Uniform.ColorIntensity = {Color.X, Color.Y, Color.Z, Intensity};
		Uniform.RightWidth = {Right.X, Right.Y, Right.Z, Light.Width};
		Uniform.UpHeight = {Up.X, Up.Y, Up.Z, Light.Height};
		Uniform.ConeShadow = {std::cos(Light.InnerConeAngle), std::cos(Light.OuterConeAngle), -1, 0};
		if (Light.Type == ELightType::Sky)
		{
			bHasSky = true;
			Result.Sky[0] += Light.Intensity * Light.AmbientStrength;
			SkyColor = SkyColor + Color * (Light.Intensity * Light.AmbientStrength);
			Result.Sky[1] = Light.bEnvironmentVisible ? 1.f : Result.Sky[1];
		}
	}

	if (Result.Sky[0] > 0.f)
	{
		SkyColor = SkyColor / Result.Sky[0];
		Result.SkyColor = {SkyColor.X, SkyColor.Y, SkyColor.Z, 1};
	}

	if (Settings.Atmosphere && Settings.Atmosphere->bEnabled)
	{
		const FSkyAtmosphereComponent& Sky = *Settings.Atmosphere;
		if (const auto Valid = ValidateSkyAtmosphereComponent(Sky); !Valid)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
		}

		Result.Atmosphere = {Sky.RayleighScattering, Sky.MieScattering, Sky.MieAnisotropy, 1};
		Result.AtmosphereGeometry = {Sky.PlanetRadius, Sky.AtmosphereHeight, 0, 0};

		// With an atmosphere, the sun is authored above the air. Surfaces receive what it transmits, so sunsets redden and night falls when the sun sets.
		for (std::size_t Index = 0; Index < static_cast<std::size_t>(Result.Controls[0]); ++Index)
		{
			auto& Light = Result.Lights[Index];
			if (Light.PositionType[3] != static_cast<float>(ELightType::Directional))
			{
				continue;
			}

			Result.SunIlluminance = {Light.ColorIntensity[0] * Light.ColorIntensity[3], Light.ColorIntensity[1] * Light.ColorIntensity[3], Light.ColorIntensity[2] * Light.ColorIntensity[3], 1};
			const FVector3 Transmittance = GetSkyTransmittance(Result, {-Light.DirectionRange[0], -Light.DirectionRange[1], -Light.DirectionRange[2]});
			Light.ColorIntensity[0] *= Transmittance.X;
			Light.ColorIntensity[1] *= Transmittance.Y;
			Light.ColorIntensity[2] *= Transmittance.Z;
			break;
		}
		if (!bHasSky)
		{
			Result.Sky[1] = 1;
		}
	}

	if (Settings.Fog && Settings.Fog->bEnabled)
	{
		const FHeightFogComponent& Fog = *Settings.Fog;
		if (const auto Valid = ValidateHeightFogComponent(Fog); !Valid)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
		}

		Result.Fog = {Fog.Density, Fog.HeightFalloff, Fog.Anisotropy, Fog.MaxDistance};
		Result.FogColor = {Fog.Albedo.X, Fog.Albedo.Y, Fog.Albedo.Z, Settings.FogHeight};
		Result.Controls[3] = Fog.bVolumetric ? static_cast<float>(16u << static_cast<unsigned>(Fog.Quality)) : -1.f;
	}

	Result.AtmosphereGeometry[3] = static_cast<float>(Settings.ShadowQuality);
	const auto ActiveLights = std::span(Result.Lights).first(static_cast<std::size_t>(Result.Controls[0]));
	const auto Sun = std::ranges::find(ActiveLights, static_cast<float>(ELightType::Directional), [](const FVisualLightUniform& Light)
	{
		return Light.PositionType[3];
	});

	if (Sun != ActiveLights.end())
	{
		Result.SunDirection = {-Sun->DirectionRange[0], -Sun->DirectionRange[1], -Sun->DirectionRange[2], 1};
		Result.SunRadiance = {Sun->ColorIntensity[0] * Sun->ColorIntensity[3], Sun->ColorIntensity[1] * Sun->ColorIntensity[3], Sun->ColorIntensity[2] * Sun->ColorIntensity[3], 0};
	}

	return Result;
}

std::vector<FShadowView> BuildShadowViews(FVisualUniforms& Uniforms, const std::span<const FRenderLight> Lights, const FMatrix4& Projection, const float Resolution, const float VisibleNear, const float VisibleFar)
{
	const FShadowSizes Sizes = GetShadowSizes(Resolution);
	std::vector<FShadowView> Result;
	if (Lights.size() > MaximumRenderLights)
	{
		Uniforms.Controls[1] = 0.f;
		return Result;
	}

	const FMatrix4 CameraToWorld(Uniforms.ViewToWorld);
	const auto Look = [](const FVector3& Position, const FVector3& Forward, const FVector3& Up)
	{
		const FVector3 Z = Forward.Normalized();
		const FVector3 X = Up.Cross(Z).Normalized();
		const FVector3 Y = Z.Cross(X);
		FMatrix4 Matrix;
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			Matrix(0, Axis) = X[Axis];
			Matrix(1, Axis) = Y[Axis];
			Matrix(2, Axis) = Z[Axis];
		}

		Matrix(0, 3) = -X.Dot(Position);
		Matrix(1, 3) = -Y.Dot(Position);
		Matrix(2, 3) = -Z.Dot(Position);
		return Matrix;
	};

	std::size_t UniformIndex = 0;
	bool bCascadesAssigned = false;
	std::uint32_t LocalTiles = 0;
	for (const FRenderLight& Source : Lights)
	{
		const FLightComponent& Light = Source.Settings;
		if (!Light.bEnabled)
		{
			continue;
		}

		FVisualLightUniform& Target = Uniforms.Lights[UniformIndex++];
		if (!Light.bCastShadows || Light.Type == ELightType::Sky)
		{
			continue;
		}

		const std::size_t Count = Light.Type == ELightType::Directional ? 4 : Light.Type == ELightType::Point ? 6
		                                                                                                      : 1;
		const bool bCascaded = Light.Type == ELightType::Directional;
		if (Count > MaximumRenderShadows - Result.size() || (bCascaded && bCascadesAssigned))
		{
			continue;
		}

		bCascadesAssigned = bCascadesAssigned || bCascaded;

		Target.ConeShadow[2] = static_cast<float>(Result.size());
		Target.ConeShadow[3] = static_cast<float>(Count);
		const FVector3 Position{Source.Transform(0, 3), Source.Transform(1, 3), Source.Transform(2, 3)};
		const FVector3 Direction{Target.DirectionRange[0], Target.DirectionRange[1], Target.DirectionRange[2]};
		const FVector3 Up = std::abs(Direction.Y) > 0.95f ? FVector3{0, 0, 1} : FVector3{0, 1, 0};
		// Without a visible range yet, the cascades cover the whole shadow distance.
		const float Far = std::isfinite(VisibleFar) ? std::clamp(VisibleFar, 1.f, Light.Range) : Light.Range;
		const float Near = std::clamp(VisibleNear, std::max(0.05f, Projection(2, 3)), Far * 0.5f);
		float PreviousSplit = Near;
		for (std::size_t Face = 0; Face < Count; ++Face)
		{
			FMatrix4 Matrix;
			float Split = Light.Range;
			float Texel = 0.f;
			if (Light.Type == ELightType::Directional)
			{
				constexpr std::array<float, 4> Splits{0.04f, 0.14f, 0.4f, 1.f};
				Split = std::max(PreviousSplit + 0.1f, Near + (Far - Near) * Splits[Face]);
				const float CenterDepth = (PreviousSplit + Split) * 0.5f;
				const FVector4 Center4 = CameraToWorld * FVector4{-Projection(0, 2) * CenterDepth / Projection(0, 0), -Projection(1, 2) * CenterDepth / Projection(1, 1), CenterDepth, 1};
				const FVector3 Center{Center4.X, Center4.Y, Center4.Z};
				float Radius = 0.f;
				for (const float Z : {PreviousSplit, Split})
				{
					for (const float X : {-1.f, 1.f})
					{
						for (const float Y : {-1.f, 1.f})
						{
							const FVector4 Corner = CameraToWorld * FVector4{(X - Projection(0, 2)) * Z / Projection(0, 0), (Y - Projection(1, 2)) * Z / Projection(1, 1), Z, 1};
							Radius = std::max(Radius, (FVector3{Corner.X, Corner.Y, Corner.Z} - Center).Length());
						}
					}
				}

				Radius = std::ceil(Radius * 16.f) / 16.f;
				FMatrix4 LightView = Look(Center - Direction * (Radius + 64.f), Direction, Up);
				Texel = 2.f * Radius / static_cast<float>(Sizes.Cascade);
				LightView(0, 3) = std::round(LightView(0, 3) / Texel) * Texel;
				LightView(1, 3) = std::round(LightView(1, 3) / Texel) * Texel;
				FMatrix4 Ortho = FMatrix4::Zero();
				Ortho(0, 0) = -1.f / Radius;
				Ortho(1, 1) = 1.f / Radius;
				Ortho(2, 2) = -1.f / (2.f * Radius + 128.f);
				Ortho(2, 3) = 1.f;
				Ortho(3, 3) = 1.f;
				Matrix = Ortho * LightView;
				PreviousSplit = Split;
			}
			else if (Light.Type == ELightType::Point)
			{
				constexpr std::array<FVector3, 6> Directions{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
				const FVector3 FaceUp = Face == 2 || Face == 3 ? FVector3{0, 0, 1} : FVector3{0, 1, 0};
				Matrix = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.05f) * Look(Position, Directions[Face], FaceUp);
			}
			else
			{
				// Rect shadows initially use a single center projection; surface lighting still integrates the full emitter.
				const float FOV = Light.Type == ELightType::Rect ? std::numbers::pi_v<float> * 0.85f : Light.OuterConeAngle * 2.f;
				Matrix = FMatrix4::PerspectiveReversedInfinite(FOV, 1.f, 0.05f) * Look(Position, Direction, Up);
			}

			const auto Cascade = static_cast<std::uint32_t>(Face);
			const FRenderViewport Viewport = bCascaded ? FRenderViewport{.X = Cascade % 2 * Sizes.Cascade, .Y = Cascade / 2 * Sizes.Cascade, .Width = Sizes.Cascade, .Height = Sizes.Cascade}
			                                           : FRenderViewport{.X = LocalTiles % 8 * Sizes.Tile, .Y = LocalTiles / 8 * Sizes.Tile, .Width = Sizes.Tile, .Height = Sizes.Tile};
			LocalTiles += bCascaded ? 0 : 1;
			const auto Width = static_cast<float>(bCascaded ? 2 * Sizes.Cascade : 8 * Sizes.Tile);
			const auto Height = static_cast<float>(bCascaded ? 2 * Sizes.Cascade : 2 * Sizes.Tile);
			Uniforms.Shadows[Result.size()] = {.WorldToClip = Matrix.Data(), .Atlas = {static_cast<float>(Viewport.Width) / Width, static_cast<float>(Viewport.Height) / Height, static_cast<float>(Viewport.X) / Width, static_cast<float>(Viewport.Y) / Height}, .Parameters = {Light.ShadowBias, Split, Light.ShadowNormalBias, Texel}};
			Result.push_back({.WorldToClip = Matrix, .Viewport = Viewport, .LightPosition = Position, .LightRange = bCascaded ? 0.f : Light.Range});
		}
	}

	Uniforms.Controls[1] = static_cast<float>(Result.size());
	return Result;
}

FViewFrustum MakeViewFrustum(const FMatrix4& WorldToClip, const FVector3& SphereCenter, const float SphereRadius)
{
	const auto Row = [&](const std::size_t Index)
	{
		return FVector4{WorldToClip(Index, 0), WorldToClip(Index, 1), WorldToClip(Index, 2), WorldToClip(Index, 3)};
	};

	const FVector4 X = Row(0);
	const FVector4 Y = Row(1);
	const FVector4 Z = Row(2);
	const FVector4 W = Row(3);
	return {.Planes = {W + X, W - X, W + Y, W - Y, Z, W - Z}, .SphereCenter = SphereCenter, .SphereRadius = SphereRadius};
}

bool IsBoxInFrustum(const FViewFrustum& Frustum, const FVector3& Center, const FVector3& Extent)
{
	for (const FVector4& Plane : Frustum.Planes)
	{
		const float Distance = Plane.X * Center.X + Plane.Y * Center.Y + Plane.Z * Center.Z + Plane.W;
		const float Radius = std::abs(Plane.X) * Extent.X + std::abs(Plane.Y) * Extent.Y + std::abs(Plane.Z) * Extent.Z;
		if (Distance + Radius < 0.f)
		{
			return false;
		}
	}

	if (Frustum.SphereRadius <= 0.f)
	{
		return true;
	}

	const FVector3 Offset = Center - Frustum.SphereCenter;
	const FVector3 Outside{std::max(std::abs(Offset.X) - Extent.X, 0.f), std::max(std::abs(Offset.Y) - Extent.Y, 0.f), std::max(std::abs(Offset.Z) - Extent.Z, 0.f)};
	return Outside.Dot(Outside) <= Frustum.SphereRadius * Frustum.SphereRadius;
}

FRenderViewport GetBoxScreenBounds(const FMatrix4& WorldToClip, const FExtent2D Target, const FVector3& Center, const FVector3& Extent)
{
	float MinimumX = std::numeric_limits<float>::infinity();
	float MinimumY = MinimumX;
	float MaximumX = -MinimumX;
	float MaximumY = -MinimumX;
	std::uint32_t BehindCorners = 0;
	for (std::uint32_t Corner = 0; Corner < 8; ++Corner)
	{
		const FVector3 Point{Center.X + ((Corner & 1) != 0 ? Extent.X : -Extent.X), Center.Y + ((Corner & 2) != 0 ? Extent.Y : -Extent.Y), Center.Z + ((Corner & 4) != 0 ? Extent.Z : -Extent.Z)};
		const auto Clip = [&](const std::size_t Row)
		{
			return WorldToClip(Row, 0) * Point.X + WorldToClip(Row, 1) * Point.Y + WorldToClip(Row, 2) * Point.Z + WorldToClip(Row, 3);
		};

		const float W = Clip(3);
		if (W <= 0.0001f)
		{
			++BehindCorners;
			continue;
		}

		MinimumX = std::min(MinimumX, Clip(0) / W);
		MaximumX = std::max(MaximumX, Clip(0) / W);
		MinimumY = std::min(MinimumY, Clip(1) / W);
		MaximumY = std::max(MaximumY, Clip(1) / W);
	}

	// Projecting corners behind the camera inverts them; a box crossing the camera plane can cover any pixel.
	if (BehindCorners == 8)
	{
		return {};
	}

	if (BehindCorners > 0)
	{
		return {.X = 0, .Y = 0, .Width = Target.Width, .Height = Target.Height};
	}

	// NVRHI flips Vulkan viewports to D3D semantics, so clip-space +Y is the top row.
	const auto Pixel = [](const float Ndc, const std::uint32_t Size)
	{
		return std::clamp((Ndc * 0.5f + 0.5f) * static_cast<float>(Size), 0.f, static_cast<float>(Size));
	};

	const float Left = std::floor(Pixel(MinimumX, Target.Width));
	const float Right = std::ceil(Pixel(MaximumX, Target.Width));
	const float Top = std::floor(Pixel(-MaximumY, Target.Height));
	const float Bottom = std::ceil(Pixel(-MinimumY, Target.Height));
	if (Right <= Left || Bottom <= Top)
	{
		return {};
	}

	return {.X = static_cast<std::uint32_t>(Left), .Y = static_cast<std::uint32_t>(Top), .Width = static_cast<std::uint32_t>(Right - Left), .Height = static_cast<std::uint32_t>(Bottom - Top)};
}
}
