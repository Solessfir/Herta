#pragma once

#include "Herta/Renderer/MeshRenderer.h"

namespace Herta
{
[[nodiscard]] std::expected<void, FPresentationError> RunRendererSmoke(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader, const FShaderAsset& InstancedVertexShader);
}
