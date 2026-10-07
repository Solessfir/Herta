#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Renderer/Visuals.h"

#include <cstdint>
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

inline constexpr std::uint32_t SkyMultipleScatteringSize = 32;

// One RGBA32F level of Hillaire's multiple-scattering transfer (Psi_ms) for unit sun illuminance, from the atmosphere settings in Snapshot.
// Columns map the cosine between up and the sun from -1 to 1, rows map altitude from the ground to the top of the atmosphere.
// It does not depend on the sun direction, so it only needs rebuilding when the atmosphere changes.
[[nodiscard]] FCookedTextureMip BuildSkyMultipleScattering(const FVisualUniforms& Snapshot);

// Transmittance of the atmosphere in Snapshot from sea level along Direction, and zero below the horizon.
[[nodiscard]] FVector3 GetSkyTransmittance(const FVisualUniforms& Snapshot, const FVector3& Direction);

// Pure CPU work on copied inputs; publication and GPU upload remain on the renderer's owning thread.
[[nodiscard]] std::expected<FEnvironmentLighting, FAssetError> BuildEnvironmentLighting(const FCookedTexture* Environment, const FVisualUniforms& Snapshot, std::stop_token StopToken = {});
}
