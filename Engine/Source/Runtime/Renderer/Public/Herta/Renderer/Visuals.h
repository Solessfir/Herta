#pragma once

#include "Herta/Level/World.h"
#include "Herta/RHI/Graphics.h"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>

namespace Herta
{
inline constexpr std::size_t MaximumRenderLights = 256;
// One bit per light in each light cluster. Keep in sync with VisualLights.slangh.
inline constexpr std::size_t LightMaskWords = MaximumRenderLights / 32;
// Light clusters are 64-pixel tiles by exponential depth slices from 0.1 to 500 m. Keep the slice count in sync with VisualShared.slangh.
inline constexpr std::uint32_t LightClusterTileSize = 64;
inline constexpr std::uint32_t LightClusterSlices = 24;
inline constexpr std::size_t MaximumRenderShadows = 16;

// One directional light owns four 4096 cascades in a 16-bit 8192x8192 atlas; local lights share sixteen 512 tiles in a 32-bit 4096x1024 atlas,
// because their perspective depth needs the precision.
inline constexpr std::uint32_t ShadowCascadeResolution = 4096;
inline constexpr std::uint32_t ShadowTileResolution = 512;

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

// Screen-space ambient occlusion slices and steps per side.
enum class EAmbientOcclusion : std::uint8_t
{
	Off,
	Low,
	Medium,
	High,
};

// Presets for the settings below that cost GPU time. Each tier is a starting point; the individual settings stay editable.
enum class ERenderQuality : std::uint8_t
{
	Low,
	Medium,
	High,
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
	EAmbientOcclusion AmbientOcclusion = EAmbientOcclusion::High;
	// Fraction of ShadowCascadeResolution, from 0.25 to 1. Local tiles scale by twice as much, capped at ShadowTileResolution.
	float ShadowResolution = 1.f;
	// Fog is ray marched at this fraction of the view, capped at 2048 times it per dimension.
	float FogResolution = 0.25f;
	// Skips camera instances hidden behind depth drawn this frame. It changes no pixels, only the work. Off by default: the depth-equal shading pass already skips hidden pixels, so it only pays when hidden meshes carry heavy vertex work, which no content does yet.
	bool bOcclusionCulling = false;
	// Writes per-pixel screen motion for temporal techniques; see FMeshRenderer::GetMotionVectors. Off while nothing reads it.
	bool bMotionVectors = false;
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
	// Ray-marches fog at reduced resolution, then denoises it there before the tone mapping composite.
	FShaderAsset FogCompute;
	FShaderAsset FogDenoiseCompute;
	FShaderAsset ToneMapFragment;
	FShaderAsset ToneMapLutFragment;
	FShaderAsset ShadowVertex;
	FShaderAsset ShadowInstancedVertex;
	FShaderAsset ShadowFragment;
	FShaderAsset SmaaEdges;
	FShaderAsset SmaaWeights;
	FShaderAsset SmaaNeighborhood;
	FShaderAsset SelectionOutline;
	FShaderAsset SkyViewFragment;
	// Meters, histograms, and adapts automatic exposure in one dispatch.
	FShaderAsset ExposureCompute;
	FShaderAsset GtaoDepthCompute;
	FShaderAsset GtaoCompute;
	FShaderAsset GtaoDenoiseCompute;
	// Without both, SMAA uses the full-screen edge and weight passes.
	FShaderAsset SmaaEdgeListCompute;
	FShaderAsset SmaaEdgeWeightsCompute;
	FShaderAsset LightClustersCompute;
	// Without both, the camera draws every frustum-visible instance.
	FShaderAsset DepthHierarchyCompute;
	FShaderAsset OcclusionCullCompute;
	// The instanced depth prepass fragment that writes object motion, and the pass that fills camera motion everywhere else.
	FShaderAsset DepthVelocityFragment;
	FShaderAsset MotionVectorsCompute;
};

struct FShadowView
{
	FMatrix4 WorldToClip;
	FRenderViewport Viewport;
	// Local lights fade to nothing at their range, and their infinite projections have no far plane, so the range bounds which casters matter. Zero for cascades.
	FVector3 LightPosition{};
	float LightRange = 0.f;
};

// The planes of a clip volume in world space, pointing inward, for rejecting bounds that cannot reach a view.
struct FViewFrustum
{
	std::array<FVector4, 6> Planes{};
	FVector3 SphereCenter{};
	// Zero leaves the volume bounded by its planes alone.
	float SphereRadius = 0.f;
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
	// The metal edge color for F82-tint Fresnel, and diffuse roughness.
	std::array<float, 4> Specular{1, 1, 1, 0};
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
	// x is 1 when the mesh pass reads screen-space ambient occlusion and bent normals.
	std::array<float, 4> Occlusion{};
	// Tile size in pixels, slice = log2(ViewZ) * y + z, and tiles across. Zero tile size means no clusters, so every pixel sees every light.
	std::array<float, 4> LightGrid{};
	// The first directional light: the direction towards it, with w 1 when there is one, and its color times intensity.
	std::array<float, 4> SunDirection{};
	std::array<float, 4> SunRadiance{};
	// Maps this frame's clip space to the previous frame's, for camera motion.
	std::array<float, 16> ClipToPreviousClip{};
	std::array<FShadowUniform, MaximumRenderShadows> Shadows{};
	FMaterialUniform Material{};
	// Not part of the uniform block: draws read lights from a storage buffer, so only VisualUniformBytes are uploaded as uniforms.
	std::array<FVisualLightUniform, MaximumRenderLights> Lights{};
};

inline constexpr std::size_t VisualUniformBytes = offsetof(FVisualUniforms, Lights);
static_assert(VisualUniformBytes % 16 == 0 && VisualUniformBytes <= MaximumGraphicsUniformBytes);

[[nodiscard]] inline std::span<const std::byte> GetVisualUniformBytes(const FVisualUniforms& Uniforms) noexcept
{
	return std::as_bytes(std::span{&Uniforms, 1}).first(VisualUniformBytes);
}

// Directional albedo of the GGX specular lobe by view cosine (x) and roughness (y), for split-sum lighting, multiple-scattering compensation, and diffuse albedo scaling.
// Each texel holds A and B, where Schlick Fresnel integrates to F0 * A + B, and C, the integral of VoH * (1 - VoH)^6 that F82-tint Fresnel subtracts.
inline constexpr std::uint32_t SpecularAlbedoTableSize = 32;
[[nodiscard]] std::span<const std::array<float, 4>> GetSpecularAlbedoTable();

inline constexpr std::uint32_t GtaoDepthMips = 5;

// Matches FGtaoUniforms in Compute/GtaoShared.slangh.
struct alignas(16) FGtaoUniforms
{
	// View depth is (x * Depth + y) / (z * Depth + w).
	std::array<float, 4> DepthToView{};
	// View xy is ViewZ * (Ndc * xy + zw).
	std::array<float, 4> NdcToView{};
	std::array<float, 4> Extent{};
	// The half-resolution occlusion, which matches view depth mip 1.
	std::array<float, 4> HalfExtent{};
	// Radius and thickness in meters, slices, and steps per side.
	std::array<float, 4> Settings{};
};

void ApplyRenderQuality(FVisualSettings& Settings, ERenderQuality Quality);
// The tier whose preset matches, if any.
std::optional<ERenderQuality> FindRenderQuality(const FVisualSettings& Settings);
[[nodiscard]] FGtaoUniforms BuildGtaoUniforms(const FMatrix4& Projection, const FMatrix4& ClipToView, FExtent2D Extent, EAmbientOcclusion Quality);
[[nodiscard]] std::expected<FVisualUniforms, FPresentationError> BuildVisualUniforms(const FMatrix4& View, const FMatrix4& Projection, FExtent2D Extent, std::span<const FRenderLight> Lights, const FVisualSettings& Settings);
// Resolution scales the cascade and tile sizes; the atlases must be scaled to match.
// Cascades cover the view depth from VisibleNear to VisibleFar within the light's range, so a view that ends close puts all of their texels where it can see.
std::vector<FShadowView> BuildShadowViews(FVisualUniforms& Uniforms, std::span<const FRenderLight> Lights, const FMatrix4& Projection, float Resolution = 1.f, float VisibleNear = 0.f, float VisibleFar = std::numeric_limits<float>::infinity());
FExtent2D GetCascadeAtlasExtent(float Resolution);
FExtent2D GetLocalShadowAtlasExtent(float Resolution);
// Expects Vulkan clip space with reversed depth, so a far plane at infinity never rejects anything.
FViewFrustum MakeViewFrustum(const FMatrix4& WorldToClip, const FVector3& SphereCenter = {}, float SphereRadius = 0.f);
// Conservative: a box straddling two planes outside a frustum corner can still pass.
bool IsBoxInFrustum(const FViewFrustum& Frustum, const FVector3& Center, const FVector3& Extent);
// The pixels a world box covers, clamped to the target. A box crossing the camera plane covers it all; one entirely behind the camera or off screen is empty.
FRenderViewport GetBoxScreenBounds(const FMatrix4& WorldToClip, FExtent2D Target, const FVector3& Center, const FVector3& Extent);
}
