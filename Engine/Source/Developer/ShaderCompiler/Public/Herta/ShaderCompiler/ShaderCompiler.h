#pragma once

#include "Herta/RHI/CookedShader.h"

namespace Herta
{
struct FShaderCompileRequest
{
	std::filesystem::path Source;
	std::string EntryPoint;
	EShaderStage Stage = EShaderStage::Vertex;
	bool bDebugInformation = false;
	std::vector<std::filesystem::path> IncludeRoots{};
};

[[nodiscard]] std::expected<FShaderAsset, FShaderError> CompileShader(const FShaderCompileRequest& Request);
[[nodiscard]] std::expected<void, FShaderError> SaveCookedShader(const std::filesystem::path& Path, const FShaderAsset& Shader);
}
