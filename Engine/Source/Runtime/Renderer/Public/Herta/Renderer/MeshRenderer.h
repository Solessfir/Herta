#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Math/Matrix.h"
#include "Herta/RHI/Graphics.h"
#include "Herta/Renderer/Visuals.h"

#include <array>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EDebugPrimitive : std::uint8_t
{
	Points,
	Lines,
	Triangles
};

struct FDebugDrawVertex
{
	FVector3 Position;
	// Point diameter or line width in framebuffer pixels; triangles ignore Size.
	float Size = 1.f;
	std::array<float, 4> Color{1, 1, 1, 1};
};

struct FDebugDrawList
{
	EDebugPrimitive Primitive = EDebugPrimitive::Lines;
	std::span<const FDebugDrawVertex> Vertices;
	bool bDepthTest = true;
};

class FRenderMesh;
class FRenderMaterial;

// A material shader's stages, in MaterialShaderStageNames order: shading, then the depth prepass variants that draw it into the prepass as it shades.
enum class EMaterialShaderStage : std::uint8_t
{
	Vertex,
	InstancedVertex,
	Fragment,
	DepthFragment,
	VelocityFragment,
	InstancedVelocityVertex,
	SurfaceFragment,
	VelocitySurfaceFragment,
	BakeVertex,
	BakeSurfaceFragment,
	BakeLightFragment,
	ShadowFragment,
	Count,
};

using FMaterialShaderStages = std::array<FShaderAsset, static_cast<std::size_t>(EMaterialShaderStage::Count)>;

// The irradiance field's probes a few frames ago, read back without waiting: those that traced, those the camera's pixels read, those of them
// classified inside geometry, those that asked to trace past the field's per-frame capacity, those holding a cell, and the cells that asked for
// a probe and found none free.
struct FGlobalIlluminationStatistics
{
	std::uint32_t Traced = 0;
	std::uint32_t Read = 0;
	std::uint32_t Inside = 0;
	std::uint32_t Overflow = 0;
	std::uint32_t Allocated = 0;
	std::uint32_t Unallocated = 0;
};
struct FEnvironmentLighting;

// Replaces every vertex of a mesh from this frame on, such as a deforming soft body. The upload is recorded with the frame, so earlier frames still draw the old vertices and nothing waits on the GPU.
struct FRenderMeshVertexUpdate
{
	const FRenderMesh* Mesh = nullptr;
	std::span<const FCookedVertex> Vertices{};
};

// Where a model draws. A first-person body draws a headless copy for the camera and the intact one for shadows and ray-traced lighting, so the camera never sees inside the head while the player still casts a whole shadow.
enum class EMeshVisibility : std::uint8_t
{
	Everywhere,
	// The camera's view only: no shadows, reflections, or global illumination.
	CameraOnly,
	// Shadows, reflections, and global illumination, but never the camera's view.
	HiddenFromCamera,
};

// A camera-facing particle. It draws as a soft disc that fades to its rim, lit on the CPU by the view's directional and sky lights, after
// opaque shading and before fog and tone mapping, so it takes the scene's exposure.
struct FRenderParticle
{
	FVector3 Position{};
	// Diameter in meters.
	float Size = 0.1f;
	// Linear albedo and opacity.
	std::array<float, 4> Color{1.f, 1.f, 1.f, 1.f};
	// Luminance in nits added on top of the lit albedo, scaled by the color.
	float Emissive = 0.f;
};

// One point along a ribbon, lit and composited like a particle.
struct FRenderRibbonPoint
{
	FVector3 Position{};
	// Half the strip's span at this point, such as along a sword's blade. Zero turns the strip to face the camera, Size wide.
	FVector3 Edge{};
	// Width in meters when facing the camera; otherwise a multiple of Edge.
	float Size = 0.1f;
	std::array<float, 4> Color{1.f, 1.f, 1.f, 1.f};
	float Emissive = 0.f;
};

// A continuous strip through a run of FMeshRenderView::RibbonPoints, oldest first.
struct FRenderRibbon
{
	std::uint32_t FirstPoint = 0;
	std::uint32_t PointCount = 0;
};

struct FMeshRenderView
{
	FMatrix4 View;
	FMatrix4 Projection;
	std::span<const FMatrix4> Models;
	// Each model's transform in the previous frame, for motion vectors. Leave empty when models never move or their order changed, which treats them as static.
	std::span<const FMatrix4> PreviousModels{};
	bool bDrawGrid = false;
	FVector3 GridCenter{};
	// One entry per model. Null entries are skipped, such as meshes that are still loading.
	std::span<const FRenderMesh* const> Meshes{};
	std::span<const std::span<const FRenderMaterial* const>> Materials{};
	std::span<const FRenderLight> Lights{};
	FVisualSettings Visuals{};
	// Model indices outlined as the editor selection.
	std::span<const std::size_t> Selected{};
	std::span<const FRenderMeshVertexUpdate> VertexUpdates{};
	// One entry per model, or empty to draw every model everywhere.
	std::span<const EMeshVisibility> Visibility{};
	std::span<const FRenderParticle> Particles{};
	std::span<const FRenderRibbon> Ribbons{};
	std::span<const FRenderRibbonPoint> RibbonPoints{};
};

class FRenderMaterial final
{
public:
	[[nodiscard]] static std::expected<std::shared_ptr<const FRenderMaterial>, FPresentationError> Create(IGraphicsDevice& Device, const FMaterialAsset& Material, const std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount>& Textures, std::string_view Name);
	const FMaterialParameters& GetParameters() const noexcept;
	std::string_view GetShaderKey() const noexcept;
	[[nodiscard]] std::expected<std::shared_ptr<const FRenderMaterial>, FPresentationError> WithParameters(const FMaterialParameters& Parameters, std::span<const FMaterialParameterValue> ParameterValues) const;

private:
	FMaterialParameters Parameters;
	std::vector<FMaterialParameterValue> ParameterValues;
	std::array<EMaterialChannel, MaterialTextureSlotCount> Channels{};
	std::array<FTextureHandle, MaterialTextureSlotCount> Textures{};
	std::string ShaderKey;
	// Unique per created material, so caches keyed by address notice a new material at a reused address.
	std::uint64_t Serial = 0;
	friend class FMeshRenderer;
	friend class FRayTracingScene;
};

// GPU copy of a cooked model. Recorded frames share its buffers and textures, so dropping a mesh never waits on the GPU.
class FRenderMesh final
{
public:
	// Uploads every buffer and mip, splitting work across recordings to stay within the per-recording upload budget.
	[[nodiscard]] static std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> Create(IGraphicsDevice& Device, const FCookedModel& Model, std::string_view Name);
	// Bounds and materials without a GPU copy, for headless sessions such as a dedicated server, whose collision fits the bounds and which
	// never draw.
	[[nodiscard]] static std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> CreateBoundsOnly(const FCookedModel& Model);

	[[nodiscard]] const FVector3& GetBoundsMinimum() const noexcept
	{
		return BoundsMinimum;
	}

	[[nodiscard]] const FVector3& GetBoundsMaximum() const noexcept
	{
		return BoundsMaximum;
	}

	std::span<const FCookedMaterial> GetMaterials() const noexcept;

private:
	void UpdateBounds(std::span<const FCookedVertex> NewVertices) const noexcept;

	struct FSection
	{
		std::uint32_t FirstIndex = 0;
		std::uint32_t IndexCount = 0;
		std::size_t Material = 0;
	};

	FBufferHandle Vertices;
	FBufferHandle Indices;
	std::vector<FTextureHandle> Textures;
	std::vector<FCookedMaterial> Materials;
	std::vector<FSection> Sections;
	// Vertex updates move deforming meshes, such as a rolling soft body, so the renderer refits these for culling.
	mutable FVector3 BoundsMinimum{};
	mutable FVector3 BoundsMaximum{};

	friend class FMeshRenderer;
	friend class FRayTracingScene;
};

// A 1 m cube centered on the origin with the whole texture on every face, unmirrored when viewed from outside.
// It matches Engine/Content/Shapes/Cube.gltf and serves texture previews and renderer tests.
[[nodiscard]] FCookedModel CreateTexturedCubeModel(FCookedTexture Texture);

class FMeshRenderer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader = {}, FShaderAsset DebugFragmentShader = {}, FShaderAsset GridVertexShader = {}, FShaderAsset GridFragmentShader = {}, FShaderAsset InstancedVertexShader = {}, FVisualShaderSet VisualShaders = {});
	~FMeshRenderer();
	FMeshRenderer(const FMeshRenderer&) = delete;
	FMeshRenderer& operator=(const FMeshRenderer&) = delete;
	[[nodiscard]] std::expected<void, FPresentationError> Render(FExtent2D Extent, const FMeshRenderView& View, std::span<const FDebugDrawList> DebugDraw = {});
	[[nodiscard]] const FTextureHandle& GetColorTarget() const noexcept;
	// Screen-space motion with FVisualSettings::bMotionVectors: this frame's UV minus the previous frame's, in Rg16Float. Empty otherwise.
	[[nodiscard]] const FTextureHandle& GetMotionVectors() const noexcept;
	std::size_t GetLastDrawCount() const noexcept;
	// Samples per pixel the path tracer has accumulated for the current view, or zero while it is off.
	std::uint32_t GetPathTracingSamples() const noexcept;
	bool SupportsPathTracing() const noexcept;
	FGlobalIlluminationStatistics GetGlobalIlluminationStatistics() const noexcept;
	// Auto exposure meters the next frame directly instead of adapting toward it, as after a camera cut.
	void ResetExposureAdaptation() noexcept;
	std::vector<FGpuPassTiming> GetGpuTimings() const;
	std::size_t GetRenderTargetBytes() const noexcept;
	[[nodiscard]] std::expected<void, FPresentationError> SetEnvironmentLighting(const FEnvironmentLighting& Lighting);
	// An empty key replaces the engine material's shading pipelines and keeps its cooked prepass. Parameters are a Material Graph's, in slot order.
	[[nodiscard]] std::expected<void, FPresentationError> PublishMaterialShader(std::string Key, FMaterialShaderStages Stages, std::vector<FMaterialShaderParameter> Parameters = {});
	[[nodiscard]] std::expected<void, FPresentationError> ShareMaterialShaders(const FMeshRenderer& Source);
	std::uint64_t GetMaterialShaderGeneration() const noexcept;

private:
	struct FImplementation;
	explicit FMeshRenderer(std::unique_ptr<FImplementation> Implementation);
	std::unique_ptr<FImplementation> Implementation;
};
}
