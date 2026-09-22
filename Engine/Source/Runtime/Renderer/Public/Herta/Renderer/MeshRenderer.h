#pragma once

#include "Herta/RHI/Graphics.h"

#include <expected>
#include <memory>

namespace Herta
{
class FMeshRenderer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader);
	~FMeshRenderer();
	FMeshRenderer(const FMeshRenderer&) = delete;
	FMeshRenderer& operator=(const FMeshRenderer&) = delete;
	[[nodiscard]] std::expected<void, FPresentationError> Render(FExtent2D Extent, float RotationRadians = 0.4f);
	[[nodiscard]] const FTextureHandle& GetColorTarget() const noexcept;

private:
	struct FImplementation;
	explicit FMeshRenderer(std::unique_ptr<FImplementation> Implementation);
	std::unique_ptr<FImplementation> Implementation;
};
}
