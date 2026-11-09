#pragma once

#include "Herta/Renderer/MeshRenderer.h"

namespace Herta::Tests
{
// Every material shader stage from one vertex and one fragment shader, renamed to each stage's entry point, for publishing fake shaders.
inline FMaterialShaderStages MakeMaterialShaderStages(const FShaderAsset& Vertex, const FShaderAsset& Instanced, const FShaderAsset& Fragment)
{
	const auto Renamed = [](FShaderAsset Shader, const std::string_view EntryPoint)
	{
		Shader.EntryPoint = EntryPoint;
		return Shader;
	};

	return {
	    Vertex,
	    Instanced,
	    Fragment,
	    Renamed(Fragment, "depthFragmentMain"),
	    Renamed(Fragment, "velocityFragmentMain"),
	    Renamed(Instanced, "instancedVelocityVertexMain"),
	    Renamed(Fragment, "surfaceFragmentMain"),
	    Renamed(Fragment, "velocitySurfaceFragmentMain"),
	    Renamed(Vertex, "bakeVertexMain"),
	    Renamed(Fragment, "bakeSurfaceFragmentMain"),
	    Renamed(Fragment, "bakeLightFragmentMain"),
	    Renamed(Fragment, "shadowFragmentMain"),
	};
}
}
