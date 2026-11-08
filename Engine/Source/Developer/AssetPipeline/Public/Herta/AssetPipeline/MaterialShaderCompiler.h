#pragma once

#include "Herta/AssetPipeline/ShaderSources.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Core/Hash.h"

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FMaterialShaderSources
{
	// Index into the roots: engine shaders, Engine content, then Game content.
	std::size_t Root = 0;
	// What the worker compiles: the shader itself, or the Slang generated from a Material Graph.
	std::string Source;
	std::vector<FShaderSourceFile> Files;
	// A Material Graph's parameters in slot order; hand-written shaders have none.
	std::vector<FMaterialShaderParameter> Parameters;
};

// Reads a mounted material shader ("Engine/..." or "Game/...", empty for the engine default) with its includes. A Material Graph compiles to Slang here,
// beside the graph, so the worker builds it exactly like a hand-written shader. Roots are the engine shaders, Engine content, and Game content.
// GraphText replaces a Material Graph's file, such as an unsaved draft in the editor.
[[nodiscard]] std::expected<FMaterialShaderSources, FAssetError> CollectMaterialShaderSources(std::span<const std::filesystem::path> Roots, std::string_view MountedPath, std::optional<std::string_view> GraphText = std::nullopt);

// Identifies the sources by every file's path and bytes.
[[nodiscard]] FHash128 HashMaterialShaderSources(const FMaterialShaderSources& Sources);

// One cooked shader file per stage, in MaterialShaderStageNames order.
using FMaterialShaderBytes = std::array<std::vector<std::byte>, MaterialShaderStageCount>;

// Compiles every stage, shading and depth prepass, in HertaShaderWorker from a private snapshot of the sources. Key names the snapshot so debug
// info, and with it the driver's pipeline cache, stays stable across runs.
[[nodiscard]] std::expected<FMaterialShaderBytes, FAssetError> CompileMaterialShader(const std::filesystem::path& Worker, const FMaterialShaderSources& Sources, const FHash128& Key, bool bDebug, const std::function<bool()>& ShouldCancel);
}
