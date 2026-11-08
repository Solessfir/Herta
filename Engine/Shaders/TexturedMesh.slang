// Selects MeshShading's texture-only prepass code for the engine material.
#define HERTA_ENGINE_MATERIAL
#include "MeshShading.slangh"

FMaterialSample EvaluateMaterial(FVertexOutput Input)
{
    return DefaultMaterial(Input);
}
