#pragma once

#include "Herta/Renderer/MeshRenderer.h"

#include <filesystem>

namespace Herta
{
// Draws a cube with the engine material and with a Material Graph that rebuilds it from nodes, compiled headless through the shader worker, and requires the same pixels.
[[nodiscard]] std::expected<void, FPresentationError> CheckMaterialGraphParity(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& VisualShaders, const std::filesystem::path& ShaderWorker, const std::filesystem::path& EngineShaderRoot, const std::filesystem::path& ScratchRoot);
[[nodiscard]] std::expected<void, FPresentationError> RunRendererSmoke(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& VisualShaders = {});
}
