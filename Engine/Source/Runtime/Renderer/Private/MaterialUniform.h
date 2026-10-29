#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Renderer/Visuals.h"

#include <array>

namespace Herta
{
// The shader view of a material's parameters and texture channels, shared by raster draws and the path tracer's material records.
[[nodiscard]] inline FMaterialUniform MakeMaterialUniform(const FMaterialParameters& Material, const std::array<EMaterialChannel, MaterialTextureSlotCount>& Channels)
{
	FMaterialUniform Uniform{.BaseColor = Material.BaseColor, .Surface = {Material.Metallic, Material.Roughness, Material.NormalStrength, Material.OcclusionStrength}, .Emissive = {Material.Emissive[0], Material.Emissive[1], Material.Emissive[2], Material.EmissiveIntensity}, .UV = {Material.UVScale[0], Material.UVScale[1], Material.UVOffset[0], Material.UVOffset[1]}, .Channels1 = {0, 0, static_cast<float>(Material.BlendMode), Material.AlphaCutoff}, .Specular = {Material.SpecularColor[0], Material.SpecularColor[1], Material.SpecularColor[2], Material.DiffuseRoughness}};
	for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
	{
		(Slot < 4 ? Uniform.Channels0[Slot] : Uniform.Channels1[Slot - 4]) = static_cast<float>(Channels[Slot]);
	}

	return Uniform;
}
}
