#include "Herta/Renderer/Visuals.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
using namespace Herta;

FMatrix4 TestProjection()
{
	return FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 16.f / 9.f, 0.1f);
}

void CheckFinite(const FMatrix4& Matrix)
{
	CHECK(std::ranges::all_of(Matrix.Data(), [](const float Value)
	{
		return std::isfinite(Value);
	}));
}
}

TEST_CASE("Visual uniforms preserve camera inverses and native pixel dimensions")
{
	const FMatrix4 View = FMatrix4::Scale({2, 3, 4}) * FMatrix4::Translation({1, -2, 3});
	const FMatrix4 Projection = TestProjection();
	const auto Result = BuildVisualUniforms(View, Projection, {1280, 720}, {}, {});
	REQUIRE(Result);
	const FMatrix4 ViewIdentity = View * FMatrix4(Result->ViewToWorld);
	const FMatrix4 ProjectionIdentity = Projection * FMatrix4(Result->ClipToView);
	for (std::size_t Row = 0; Row < 4; ++Row)
	{
		for (std::size_t Column = 0; Column < 4; ++Column)
		{
			CHECK(ViewIdentity(Row, Column) == doctest::Approx(Row == Column ? 1.f : 0.f));
			CHECK(ProjectionIdentity(Row, Column) == doctest::Approx(Row == Column ? 1.f : 0.f));
		}
	}

	CHECK(Result->Viewport[0] == doctest::Approx(1.f / 1280.f));
	CHECK(Result->Viewport[1] == doctest::Approx(1.f / 720.f));
	CHECK(Result->Viewport[2] == 1280.f);
	CHECK(Result->Viewport[3] == 720.f);
	CHECK(Result->Controls[2] == doctest::Approx(1.f / (1.2f * std::exp2(FVisualSettings{}.ExposureEV100))));
	CHECK(Result->Controls[0] == 0.f);
	CHECK(sizeof(FVisualUniforms) <= MaximumGraphicsUniformBytes);
	CHECK(sizeof(FVisualLightUniform) == 96);
	CHECK(sizeof(FShadowUniform) == 96);
}

TEST_CASE("Visual uniforms reject invalid cameras and bounded settings")
{
	const FMatrix4 Identity;
	FVisualSettings Settings;
	std::vector<FRenderLight> Lights(MaximumRenderLights);
	CHECK(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, Lights, Settings));
	const std::vector<FRenderLight> OverflowLights(MaximumRenderLights + 1);
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, OverflowLights, Settings));
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {0, 64}, {}, Settings));
	CHECK_FALSE(BuildVisualUniforms(FMatrix4::Zero(), TestProjection(), {64, 64}, {}, Settings));
	CHECK_FALSE(BuildVisualUniforms(Identity, FMatrix4::Zero(), {64, 64}, {}, Settings));
	FMatrix4 Nonfinite = Identity;
	Nonfinite(0, 0) = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(BuildVisualUniforms(Nonfinite, TestProjection(), {64, 64}, {}, Settings));
	Settings.ExposureEV100 = 24.1f;
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, {}, Settings));
	Settings.ExposureEV100 = -24.f;
	CHECK(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, {}, Settings));
	Settings.ExposureEV100 = std::numeric_limits<float>::infinity();
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, {}, Settings));
	Settings.ExposureEV100 = 0.f;
	Settings.AntiAliasing = static_cast<EAntiAliasing>(255);
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, {}, Settings));
	Settings.AntiAliasing = EAntiAliasing::Off;
	Settings.FogHeight = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, {}, Settings));
	Settings.FogHeight = 0.f;
	std::array<FRenderLight, 1> InvalidLights{};
	InvalidLights[0].Transform = FMatrix4::Zero();
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, InvalidLights, Settings));
	InvalidLights[0].Transform = Nonfinite;
	CHECK_FALSE(BuildVisualUniforms(Identity, TestProjection(), {64, 64}, InvalidLights, Settings));
}

TEST_CASE("Visual lights convert authored lumens and preserve Left Up Forward axes")
{
	std::array<FRenderLight, 4> Lights{};
	Lights[0].Settings.bEnabled = false;
	Lights[1].Settings.Type = ELightType::Point;
	Lights[1].Settings.Intensity = 4.f * std::numbers::pi_v<float>;
	Lights[1].Transform = FMatrix4::Translation({2, 3, 4}) * FMatrix4::Scale({2, 3, 4});
	Lights[2].Settings.Type = ELightType::Spot;
	Lights[2].Settings.Intensity = 2.f * std::numbers::pi_v<float> * (1.f - std::cos(Lights[2].Settings.OuterConeAngle));
	Lights[3].Settings.Type = ELightType::Directional;
	Lights[3].Settings.Intensity = 12000.f;
	const auto Result = BuildVisualUniforms({}, TestProjection(), {64, 64}, Lights, {});
	REQUIRE(Result);
	CHECK(Result->Controls[0] == 3.f);
	CHECK(Result->Lights[0].PositionType == std::array<float, 4>{2, 3, 4, static_cast<float>(ELightType::Point)});
	CHECK(Result->Lights[0].DirectionRange[0] == 0.f);
	CHECK(Result->Lights[0].DirectionRange[1] == 0.f);
	CHECK(Result->Lights[0].DirectionRange[2] == 1.f);
	CHECK(Result->Lights[0].RightWidth[0] == -1.f);
	CHECK(Result->Lights[0].UpHeight[1] == 1.f);
	CHECK(Result->Lights[0].ColorIntensity[3] == doctest::Approx(1.f));
	CHECK(Result->Lights[1].ColorIntensity[3] == doctest::Approx(1.f));
	CHECK(Result->Lights[2].ColorIntensity[3] == 12000.f);
	CHECK(Result->Lights[1].ConeShadow[0] == doctest::Approx(std::cos(Lights[2].Settings.InnerConeAngle)));
	CHECK(Result->Lights[1].ConeShadow[1] == doctest::Approx(std::cos(Lights[2].Settings.OuterConeAngle)));
}

TEST_CASE("Visual temperature sky atmosphere and fog controls use authored settings")
{
	std::array<FRenderLight, 3> Lights{};
	Lights[0].Settings.bUseTemperature = true;
	Lights[0].Settings.TemperatureKelvin = 2000.f;
	Lights[1].Settings.bUseTemperature = true;
	Lights[1].Settings.TemperatureKelvin = 12000.f;
	Lights[2].Settings.Type = ELightType::Sky;
	Lights[2].Settings.Intensity = 2.f;
	Lights[2].Settings.AmbientStrength = 0.25f;
	Lights[2].Settings.bEnvironmentVisible = false;
	FVisualSettings Settings{.ExposureEV100 = 2.f, .Atmosphere = FSkyAtmosphereComponent{}, .Fog = FHeightFogComponent{}, .FogHeight = 3.f};
	const auto Result = BuildVisualUniforms({}, TestProjection(), {64, 64}, Lights, Settings);
	REQUIRE(Result);
	CHECK(Result->Lights[0].ColorIntensity[0] > Result->Lights[0].ColorIntensity[2]);
	CHECK(Result->Lights[1].ColorIntensity[2] > Result->Lights[1].ColorIntensity[0]);
	CHECK(Result->Sky[0] == 0.5f);
	CHECK(Result->Sky[1] == 0.f);
	CHECK(Result->Atmosphere[3] == 1.f);
	CHECK(Result->AtmosphereGeometry[0] == Settings.Atmosphere->PlanetRadius);
	CHECK(Result->Controls[2] == doctest::Approx(1.f / (1.2f * 4.f)));
	CHECK(Result->Controls[3] == 32.f);
	CHECK(Result->Fog[0] == Settings.Fog->Density);
	CHECK(Result->FogColor[3] == 3.f);
	Settings.Fog->bVolumetric = false;
	const auto HeightOnly = BuildVisualUniforms({}, TestProjection(), {64, 64}, {}, Settings);
	REQUIRE(HeightOnly);
	CHECK(HeightOnly->Controls[3] == -1.f);
	CHECK(HeightOnly->Sky[1] == 1.f);
	Settings.Atmosphere->bEnabled = false;
	Settings.Fog->bEnabled = false;
	const auto Disabled = BuildVisualUniforms({}, TestProjection(), {64, 64}, {}, Settings);
	REQUIRE(Disabled);
	CHECK(Disabled->Atmosphere[3] == 0.f);
	CHECK(Disabled->Controls[3] == 0.f);
}

TEST_CASE("Directional shadows allocate stable cascade tiles with increasing splits")
{
	const std::array Lights{FRenderLight{.Settings = {.Type = ELightType::Directional, .Range = 100.f}, .Transform = FMatrix4{}}};
	const auto Built = BuildVisualUniforms({}, TestProjection(), {1280, 720}, Lights, {});
	REQUIRE(Built);
	FVisualUniforms Uniforms = *Built;
	const auto Shadows = BuildShadowViews(Uniforms, Lights, TestProjection());
	REQUIRE(Shadows.size() == 4);
	CHECK(Uniforms.Controls[1] == 4.f);
	CHECK(Uniforms.Lights[0].ConeShadow[2] == 0.f);
	CHECK(Uniforms.Lights[0].ConeShadow[3] == 4.f);
	float Previous = 0.f;
	for (std::size_t Index = 0; Index < Shadows.size(); ++Index)
	{
		const auto& Shadow = Shadows[Index];
		CheckFinite(Shadow.WorldToClip);
		CHECK(Shadow.Viewport.X == Index % 2 * ShadowCascadeResolution);
		CHECK(Shadow.Viewport.Y == Index / 2 * ShadowCascadeResolution);
		CHECK(Shadow.Viewport.Width == ShadowCascadeResolution);
		CHECK(Shadow.Viewport.Height == ShadowCascadeResolution);
		CHECK(Uniforms.Shadows[Index].WorldToClip == Shadow.WorldToClip.Data());
		CHECK(Uniforms.Shadows[Index].Atlas[0] == 0.5f);
		CHECK(Uniforms.Shadows[Index].Atlas[1] == 0.4f);
		CHECK(Uniforms.Shadows[Index].Atlas[2] == static_cast<float>(Index % 2) * 0.5f);
		CHECK(Uniforms.Shadows[Index].Atlas[3] == static_cast<float>(Index / 2) * 0.4f);
		CHECK(Uniforms.Shadows[Index].Parameters[3] > 0.f);
		CHECK(Uniforms.Shadows[Index].Parameters[1] > Previous);
		CHECK(Shadow.WorldToClip(2, 2) < 0.f);
		Previous = Uniforms.Shadows[Index].Parameters[1];
	}

	CHECK(Previous == 100.f);
	FVisualUniforms Repeated = *Built;
	const auto Same = BuildShadowViews(Repeated, Lights, TestProjection());
	REQUIRE(Same.size() == Shadows.size());
	for (std::size_t Index = 0; Index < Shadows.size(); ++Index)
	{
		CHECK(Same[Index].WorldToClip.Data() == Shadows[Index].WorldToClip.Data());
	}
}

TEST_CASE("Local shadows use six point faces and single spot and rect projections")
{
	std::array<FRenderLight, 4> Lights{};
	Lights[0].Settings.Type = ELightType::Point;
	Lights[1].Settings.Type = ELightType::Spot;
	Lights[2].Settings.Type = ELightType::Rect;
	Lights[3].Settings.Type = ELightType::Sky;
	const auto Built = BuildVisualUniforms({}, TestProjection(), {64, 64}, Lights, {});
	REQUIRE(Built);
	FVisualUniforms Uniforms = *Built;
	const auto Shadows = BuildShadowViews(Uniforms, Lights, TestProjection());
	REQUIRE(Shadows.size() == 8);
	CHECK(Uniforms.Lights[0].ConeShadow[3] == 6.f);
	CHECK(Uniforms.Lights[1].ConeShadow[2] == 6.f);
	CHECK(Uniforms.Lights[1].ConeShadow[3] == 1.f);
	CHECK(Uniforms.Lights[2].ConeShadow[2] == 7.f);
	CHECK(Uniforms.Lights[2].ConeShadow[3] == 1.f);
	CHECK(Uniforms.Lights[3].ConeShadow[2] == -1.f);
	constexpr std::array<FVector3, 6> Directions{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
	for (std::size_t Face = 0; Face < Directions.size(); ++Face)
	{
		CheckFinite(Shadows[Face].WorldToClip);
		const FVector3 Direction = Directions[Face];
		const FVector4 Near = Shadows[Face].WorldToClip * FVector4{Direction.X, Direction.Y, Direction.Z, 1};
		const FVector4 Far = Shadows[Face].WorldToClip * FVector4{Direction.X * 2.f, Direction.Y * 2.f, Direction.Z * 2.f, 1};
		CHECK(Near.X == doctest::Approx(0.f));
		CHECK(Near.Y == doctest::Approx(0.f));
		CHECK(Near.W > 0.f);
		CHECK(Near.Z / Near.W > Far.Z / Far.W);
	}
}

TEST_CASE("Directional cascades contain camera frustum corners for off-center projections")
{
	const FMatrix4 View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.4f) * FQuaternion::FromAxisAngle({1, 0, 0}, -0.2f)) * FMatrix4::Translation({-8, -3, 12});
	const std::array Lights{FRenderLight{.Settings = {.Type = ELightType::Directional, .Range = 100.f}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.5f) * FQuaternion::FromAxisAngle({1, 0, 0}, 0.6f))}};
	constexpr std::array<std::array<float, 2>, 3> Offsets{{{0, 0}, {0.75f, -0.65f}, {-1.2f, 0.85f}}};
	for (const auto& Offset : Offsets)
	{
		FMatrix4 Projection = TestProjection();
		Projection(0, 2) = Offset[0];
		Projection(1, 2) = Offset[1];
		const auto Built = BuildVisualUniforms(View, Projection, {1280, 720}, Lights, {});
		REQUIRE(Built);
		FVisualUniforms Uniforms = *Built;
		const FMatrix4 CameraToWorld(Uniforms.ViewToWorld);
		const auto Shadows = BuildShadowViews(Uniforms, Lights, Projection);
		REQUIRE(Shadows.size() == 4);
		float PreviousSplit = Projection(2, 3);
		for (std::size_t Cascade = 0; Cascade < Shadows.size(); ++Cascade)
		{
			const float Split = Uniforms.Shadows[Cascade].Parameters[1];
			for (const float Z : {PreviousSplit, Split})
			{
				for (const float X : {-1.f, 1.f})
				{
					for (const float Y : {-1.f, 1.f})
					{
						const FVector4 World = CameraToWorld * FVector4{(X - Projection(0, 2)) * Z / Projection(0, 0), (Y - Projection(1, 2)) * Z / Projection(1, 1), Z, 1};
						const FVector4 Clip = Shadows[Cascade].WorldToClip * World;
						REQUIRE(Clip.W > 0.f);
						// Snapping the light view can move a boundary by at most one atlas texel.
						CHECK(std::abs(Clip.X / Clip.W) <= 1.f + 2.f / ShadowCascadeResolution);
						CHECK(std::abs(Clip.Y / Clip.W) <= 1.f + 2.f / ShadowCascadeResolution);
						CHECK(Clip.Z / Clip.W >= 0.f);
						CHECK(Clip.Z / Clip.W <= 1.f);
					}
				}
			}

			PreviousSplit = Split;
		}
	}
}

TEST_CASE("Shadow atlas budget skips whole lights instead of allocating partial cube maps")
{
	std::array<FRenderLight, 5> Lights{};
	Lights[0].Settings.bEnabled = false;
	Lights[1].Settings.Type = ELightType::Directional;
	Lights[2].Settings.Type = ELightType::Point;
	Lights[3].Settings.Type = ELightType::Point;
	Lights[4].Settings.Type = ELightType::Spot;
	const auto Built = BuildVisualUniforms({}, TestProjection(), {64, 64}, Lights, {});
	REQUIRE(Built);
	FVisualUniforms Uniforms = *Built;
	const auto Shadows = BuildShadowViews(Uniforms, Lights, TestProjection());
	REQUIRE(Shadows.size() == MaximumRenderShadows);
	CHECK(Uniforms.Controls[1] == static_cast<float>(MaximumRenderShadows));
	CHECK(Uniforms.Lights[3].ConeShadow[2] == -1.f);
	CHECK(Uniforms.Lights[3].ConeShadow[3] == 0.f);
	CHECK(Shadows.back().Viewport.X == 3 * ShadowTileResolution);
	CHECK(Shadows.back().Viewport.Y == 2 * ShadowCascadeResolution + ShadowTileResolution);
	CHECK(Shadows.back().Viewport.Width == ShadowTileResolution);
	Lights[1].Settings.bCastShadows = false;
	Uniforms = *Built;
	const auto Reduced = BuildShadowViews(Uniforms, Lights, TestProjection());
	CHECK(Reduced.size() == 13);
	CHECK(Uniforms.Lights[0].ConeShadow[2] == -1.f);
}

TEST_CASE("Only the first shadowed directional light receives cascades")
{
	std::array<FRenderLight, 3> Lights{};
	Lights[0].Settings.Type = ELightType::Directional;
	Lights[1].Settings.Type = ELightType::Directional;
	Lights[2].Settings.Type = ELightType::Spot;
	const auto Built = BuildVisualUniforms({}, TestProjection(), {64, 64}, Lights, {});
	REQUIRE(Built);
	FVisualUniforms Uniforms = *Built;
	const auto Shadows = BuildShadowViews(Uniforms, Lights, TestProjection());
	REQUIRE(Shadows.size() == 5);
	CHECK(Uniforms.Lights[1].ConeShadow[2] == -1.f);
	CHECK(Uniforms.Lights[2].ConeShadow[2] == 4.f);
	CHECK(Shadows[4].Viewport.X == 0);
	CHECK(Shadows[4].Viewport.Y == 2 * ShadowCascadeResolution);
}

TEST_CASE("Atmosphere dims the sun at noon, reddens it near the horizon, and removes it below")
{
	const auto Uniforms = [](const float ElevationDegrees, const bool bAtmosphere)
	{
		// A light's +Z axis is its travel direction; tilting it down by the elevation raises the sun.
		const std::array Lights{FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 128'000.f, .Range = 100.f}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, ElevationDegrees * std::numbers::pi_v<float> / 180.f))}};
		FVisualSettings Settings;
		if (bAtmosphere)
		{
			Settings.Atmosphere = FSkyAtmosphereComponent{};
		}

		const auto Result = BuildVisualUniforms(FMatrix4{}, TestProjection(), {64, 64}, Lights, Settings);
		REQUIRE(Result);
		return *Result;
	};

	const FVisualUniforms Space = Uniforms(90.f, false);
	CHECK(Space.SunIlluminance[3] == 0.f);
	const auto Ratio = [&Space](const FVisualUniforms& Lit, const std::size_t Channel)
	{
		return Lit.Lights[0].ColorIntensity[Channel] / Space.Lights[0].ColorIntensity[Channel];
	};

	const FVisualUniforms Noon = Uniforms(90.f, true);
	CHECK(Noon.SunIlluminance[3] == 1.f);
	CHECK(Noon.SunIlluminance[1] == doctest::Approx(Space.Lights[0].ColorIntensity[1] * Space.Lights[0].ColorIntensity[3]));
	CHECK(Ratio(Noon, 1) > 0.8f);
	CHECK(Ratio(Noon, 1) < 0.95f);

	const FVisualUniforms Sunset = Uniforms(2.f, true);
	CHECK(Ratio(Sunset, 0) > Ratio(Sunset, 2) * 2.f);
	CHECK(Ratio(Sunset, 1) < Ratio(Noon, 1));

	const FVisualUniforms Night = Uniforms(-10.f, true);
	CHECK(Night.Lights[0].ColorIntensity[0] == 0.f);
	CHECK(Night.Lights[0].ColorIntensity[2] == 0.f);
}
