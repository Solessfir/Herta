#pragma once

#include "Herta/Level/World.h"
#include "Herta/RHI/Graphics.h"

#include <array>
#include <optional>

namespace Herta
{
inline constexpr std::size_t MaximumRenderLights = 32;
inline constexpr std::size_t MaximumRenderShadows = 16;

// One directional light owns four 2048 cascades in the top 4096x4096 of the atlas; local lights share sixteen 512 tiles in the strip below.
inline constexpr std::uint32_t ShadowCascadeResolution = 2048;
inline constexpr std::uint32_t ShadowTileResolution = 512;
inline constexpr std::uint32_t ShadowAtlasWidth = 2 * ShadowCascadeResolution;
inline constexpr std::uint32_t ShadowAtlasHeight = 2 * ShadowCascadeResolution + 2 * ShadowTileResolution;

enum class EAntiAliasing : std::uint8_t
{
	Off,
	SmaaLow,
	SmaaMedium,
	SmaaHigh,
	SmaaUltra,
};

enum class EShadowQuality : std::uint8_t
{
	Off,
	Hard,
	Soft,
};

struct FRenderLight
{
	FLightComponent Settings{};
	FMatrix4 Transform{};
	FObjectId Id{};
};

// HDR10 output. The display target then holds linear Rec.709 relative to paper white for the presenter to encode, instead of sRGB.
struct FHdrDisplaySettings
{
	// Luminance of SDR white and the scene's diffuse white, in cd/m^2.
	float PaperWhite = 200.f;
	// Brightest luminance the tone mapper targets, in cd/m^2. GT7 is tuned for 250 and up.
	float PeakLuminance = 1000.f;
	// Replaces the scene with a clipping pattern for matching PeakLuminance to the display.
	bool bCalibrationPattern = false;
};

struct FVisualSettings
{
	// Photographic EV at ISO 100. The saturating luminance is 1.2 * 2^EV100 cd/m^2 (Lagarde and de Rousiers).
	float ExposureEV100 = 15.f;
	EAntiAliasing AntiAliasing = EAntiAliasing::SmaaHigh;
	std::optional<FSkyAtmosphereComponent> Atmosphere{};
	std::optional<FHeightFogComponent> Fog{};
	float FogHeight = 0.f;
	EShadowQuality ShadowQuality = EShadowQuality::Soft;
	bool bStudioPreview = true;
	// Meters the HDR scene on the GPU and eases towards it instead of using ExposureEV100.
	bool bAutoExposure = false;
	// Positive values brighten the automatic exposure, in stops.
	float ExposureCompensation = 0.f;
	// Frame time that paces automatic adaptation.
	float DeltaSeconds = 0.f;
	// Unset renders for an sRGB display.
	std::optional<FHdrDisplaySettings> HdrDisplay{};
};

struct FVisualShaderSet
{
	FShaderAsset FullscreenVertex;
	FShaderAsset SkyFragment;
	FShaderAsset FogFragment;
	FShaderAsset CompositeFragment;
	FShaderAsset ToneMapFragment;
	FShaderAsset ShadowVertex;
	FShaderAsset ShadowInstancedVertex;
	FShaderAsset ShadowFragment;
	FShaderAsset SmaaEdges;
	FShaderAsset SmaaWeights;
	FShaderAsset SmaaNeighborhood;
	FShaderAsset SelectionOutline;
	FShaderAsset SkyViewFragment;
	FShaderAsset ExposureMeterFragment;
	FShaderAsset ExposureAdaptFragment;
};

struct FShadowView
{
	FMatrix4 WorldToClip;
	FRenderViewport Viewport;
};

// Mirrors VisualShared.slang. Float4 fields keep the uniform layout identical across compilers.
struct alignas(16) FVisualLightUniform
{
	std::array<float, 4> PositionType{};
	std::array<float, 4> DirectionRange{};
	std::array<float, 4> ColorIntensity{};
	std::array<float, 4> RightWidth{};
	std::array<float, 4> UpHeight{};
	std::array<float, 4> ConeShadow{};
};

struct alignas(16) FShadowUniform
{
	std::array<float, 16> WorldToClip{};
	std::array<float, 4> Atlas{};
	std::array<float, 4> Parameters{};
};

struct alignas(16) FMaterialUniform
{
	std::array<float, 4> BaseColor{1, 1, 1, 1};
	std::array<float, 4> Surface{0, 0.5f, 1, 1};
	std::array<float, 4> Emissive{};
	std::array<float, 4> UV{1, 1, 0, 0};
	std::array<float, 4> Channels0{};
	std::array<float, 4> Channels1{};
	std::array<float, 4> TextureFlags{};
};

struct alignas(16) FVisualUniforms
{
	std::array<float, 16> ViewToWorld{};
	std::array<float, 16> ClipToView{};
	std::array<float, 4> Viewport{};
	std::array<float, 4> Sky{};
	std::array<float, 4> SkyColor{1, 1, 1, 1};
	std::array<float, 4> Atmosphere{};
	std::array<float, 4> AtmosphereGeometry{6360000.f, 80000.f, 0, 0};
	std::array<float, 4> Fog{};
	std::array<float, 4> FogColor{};
	std::array<float, 4> Controls{};
	// The atmosphere's sun above the air, in lux; w is 1 when present. Its light entry carries what reaches the ground.
	std::array<float, 4> SunIlluminance{};
	// x enables automatic exposure, y is its compensation in stops, z the frame time, and w restarts adaptation.
	std::array<float, 4> Exposure{};
	// x is 1 for HDR output, y the paper white and z the peak in cd/m^2, and w shows the calibration pattern.
	std::array<float, 4> Display{};
	std::array<FVisualLightUniform, MaximumRenderLights> Lights{};
	std::array<FShadowUniform, MaximumRenderShadows> Shadows{};
	FMaterialUniform Material{};
};

[[nodiscard]] std::expected<FVisualUniforms, FPresentationError> BuildVisualUniforms(const FMatrix4& View, const FMatrix4& Projection, FExtent2D Extent, std::span<const FRenderLight> Lights, const FVisualSettings& Settings);
std::vector<FShadowView> BuildShadowViews(FVisualUniforms& Uniforms, std::span<const FRenderLight> Lights, const FMatrix4& Projection);
}
