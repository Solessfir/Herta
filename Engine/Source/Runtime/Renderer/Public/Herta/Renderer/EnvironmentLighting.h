#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Renderer/Visuals.h"

#include <expected>
#include <stop_token>

namespace Herta
{
struct FEnvironmentLighting
{
	// Irradiance includes the cosine integral, so Lambert shading divides it by pi.
	FCookedTexture Diffuse;
	// Lat-long mip zero retains the source; successive mips contain roughness-dependent GGX filtering.
	FCookedTexture Specular;
	bool bFromHdr = false;
};

// Pure CPU work on copied inputs; publication and GPU upload remain on the renderer's owning thread.
[[nodiscard]] std::expected<FEnvironmentLighting, FAssetError> BuildEnvironmentLighting(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, std::stop_token StopToken = {});
}
