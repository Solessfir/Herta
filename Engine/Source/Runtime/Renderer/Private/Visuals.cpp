#include "Herta/Renderer/Visuals.h"

#include <algorithm>
#include <cmath>
#include <numbers>

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

std::expected<FVisualUniforms, FPresentationError> BuildVisualUniforms(const FMatrix4& View, const FMatrix4& Projection, const FExtent2D Extent, const std::span<const FRenderLight> Lights, const FVisualSettings& Settings)
{
	const auto ViewToWorld = Inverse(View);
	const auto ClipToView = Inverse(Projection);
	if (!ViewToWorld || !ClipToView)
	{
		return std::unexpected(!ViewToWorld ? ViewToWorld.error() : ClipToView.error());
	}

	if (Lights.size() > MaximumRenderLights || Extent.IsEmpty() || !std::isfinite(Settings.FogHeight) || !std::isfinite(Settings.ExposureEV) || std::abs(Settings.ExposureEV) > 24.f || static_cast<unsigned>(Settings.AntiAliasing) > static_cast<unsigned>(EAntiAliasing::SmaaUltra) || static_cast<unsigned>(Settings.ShadowQuality) > static_cast<unsigned>(EShadowQuality::Soft))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Visual settings exceed the light, exposure, extent, or antialiasing budget"});
	}

	FVisualUniforms Result;
	Result.ViewToWorld = ViewToWorld->Data();
	Result.ClipToView = ClipToView->Data();
	Result.Viewport = {1.f / static_cast<float>(Extent.Width), 1.f / static_cast<float>(Extent.Height), static_cast<float>(Extent.Width), static_cast<float>(Extent.Height)};
	Result.Controls[2] = std::exp2(Settings.ExposureEV);
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

	return Result;
}

std::vector<FShadowView> BuildShadowViews(FVisualUniforms& Uniforms, const std::span<const FRenderLight> Lights, const FMatrix4& Projection)
{
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
		float PreviousSplit = std::max(0.05f, Projection(2, 3));
		for (std::size_t Face = 0; Face < Count; ++Face)
		{
			FMatrix4 Matrix;
			float Split = Light.Range;
			float Texel = 0.f;
			if (Light.Type == ELightType::Directional)
			{
				constexpr std::array<float, 4> Splits{0.04f, 0.14f, 0.4f, 1.f};
				Split = std::max(PreviousSplit + 0.1f, Light.Range * Splits[Face]);
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
				Texel = 2.f * Radius / static_cast<float>(ShadowCascadeResolution);
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
			const FRenderViewport Viewport = bCascaded ? FRenderViewport{.X = Cascade % 2 * ShadowCascadeResolution, .Y = Cascade / 2 * ShadowCascadeResolution, .Width = ShadowCascadeResolution, .Height = ShadowCascadeResolution}
			                                          : FRenderViewport{.X = LocalTiles % 8 * ShadowTileResolution, .Y = 2 * ShadowCascadeResolution + LocalTiles / 8 * ShadowTileResolution, .Width = ShadowTileResolution, .Height = ShadowTileResolution};
			LocalTiles += bCascaded ? 0 : 1;
			constexpr float Width = ShadowAtlasWidth;
			constexpr float Height = ShadowAtlasHeight;
			Uniforms.Shadows[Result.size()] = {.WorldToClip = Matrix.Data(), .Atlas = {static_cast<float>(Viewport.Width) / Width, static_cast<float>(Viewport.Height) / Height, static_cast<float>(Viewport.X) / Width, static_cast<float>(Viewport.Y) / Height}, .Parameters = {Light.ShadowBias, Split, Light.ShadowNormalBias, Texel}};
			Result.push_back({.WorldToClip = Matrix, .Viewport = Viewport});
		}
	}

	Uniforms.Controls[1] = static_cast<float>(Result.size());
	return Result;
}
}
