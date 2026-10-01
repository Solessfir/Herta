#pragma once

#include "Herta/Math/Matrix.h"
#include "Herta/RHI/Graphics.h"

#include <expected>
#include <memory>

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
	float Size = 1.0f;
	std::array<float, 4> Color{1, 1, 1, 1};
};

struct FDebugDrawList
{
	EDebugPrimitive Primitive = EDebugPrimitive::Lines;
	std::span<const FDebugDrawVertex> Vertices;
	bool bDepthTest = true;
};

struct FMeshRenderView
{
	FMatrix4 View;
	FMatrix4 Projection;
	std::span<const FMatrix4> Models;
	bool bDrawGrid = false;
	FVector3 GridCenter{};
};

class FMeshRenderer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader = {}, FShaderAsset DebugFragmentShader = {}, FShaderAsset GridVertexShader = {}, FShaderAsset GridFragmentShader = {});
	~FMeshRenderer();
	FMeshRenderer(const FMeshRenderer&) = delete;
	FMeshRenderer& operator=(const FMeshRenderer&) = delete;
	[[nodiscard]] std::expected<void, FPresentationError> Render(FExtent2D Extent, float RotationRadians = 0.4f);
	[[nodiscard]] std::expected<void, FPresentationError> Render(FExtent2D Extent, const FMeshRenderView& View, std::span<const FDebugDrawList> DebugDraw = {});
	[[nodiscard]] const FTextureHandle& GetColorTarget() const noexcept;

private:
	struct FImplementation;
	explicit FMeshRenderer(std::unique_ptr<FImplementation> Implementation);
	std::unique_ptr<FImplementation> Implementation;
};
}
