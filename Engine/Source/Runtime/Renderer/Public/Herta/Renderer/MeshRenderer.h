#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Math/Matrix.h"
#include "Herta/RHI/Graphics.h"

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

struct FMeshRenderView
{
	FMatrix4 View;
	FMatrix4 Projection;
	std::span<const FMatrix4> Models;
	bool bDrawGrid = false;
	FVector3 GridCenter{};
	// One entry per model. Null entries are skipped, such as meshes that are still loading.
	std::span<const FRenderMesh* const> Meshes{};
};

// GPU copy of a cooked model. Recorded frames share its buffers and textures, so dropping a mesh never waits on the GPU.
class FRenderMesh final
{
public:
	// Uploads every buffer and mip, splitting work across recordings to stay within the per-recording upload budget.
	[[nodiscard]] static std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> Create(IGraphicsDevice& Device, const FCookedModel& Model, std::string_view Name);

	[[nodiscard]] const FVector3& GetBoundsMinimum() const noexcept
	{
		return BoundsMinimum;
	}

	[[nodiscard]] const FVector3& GetBoundsMaximum() const noexcept
	{
		return BoundsMaximum;
	}

private:
	struct FSection
	{
		std::uint32_t FirstIndex = 0;
		std::uint32_t IndexCount = 0;
		std::size_t Texture = 0;
	};

	FBufferHandle Vertices;
	FBufferHandle Indices;
	std::vector<FTextureHandle> Textures;
	std::vector<FSection> Sections;
	FVector3 BoundsMinimum{};
	FVector3 BoundsMaximum{};

	friend class FMeshRenderer;
};

// A 1 m cube centered on the origin with the whole texture on every face, unmirrored when viewed from outside.
// It matches Engine/Content/Shapes/Cube.gltf and serves texture previews and renderer tests.
[[nodiscard]] FCookedModel CreateTexturedCubeModel(FCookedTexture Texture);

class FMeshRenderer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader = {}, FShaderAsset DebugFragmentShader = {}, FShaderAsset GridVertexShader = {}, FShaderAsset GridFragmentShader = {});
	~FMeshRenderer();
	FMeshRenderer(const FMeshRenderer&) = delete;
	FMeshRenderer& operator=(const FMeshRenderer&) = delete;
	[[nodiscard]] std::expected<void, FPresentationError> Render(FExtent2D Extent, const FMeshRenderView& View, std::span<const FDebugDrawList> DebugDraw = {});
	[[nodiscard]] const FTextureHandle& GetColorTarget() const noexcept;

private:
	struct FImplementation;
	explicit FMeshRenderer(std::unique_ptr<FImplementation> Implementation);
	std::unique_ptr<FImplementation> Implementation;
};
}
